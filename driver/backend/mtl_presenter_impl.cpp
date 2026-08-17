#include "mtl_presenter_impl.h"

#include "mtl_device_impl.h"

#include "../mtl/mtl_context.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

#include <chrono>

namespace dxvk::backend {

  namespace {
    WMTPixelFormat wmtFormat(MtlFormat f) { return WMTPixelFormat(uint32_t(f)); }

    constexpr int64_t kAcquireStallMs   = 100;
    constexpr int64_t kAcquireBackoffMs = 250;
  }

  class MtlPresenterImpl : public MtlPresenter {

  public:

    MtlPresenterImpl(const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc)
    : m_device(device), m_desc(desc) {
      m_backend   = static_cast<MtlDeviceImpl*>(device.ptr());
      m_wmtDevice = m_backend ? m_backend->wmtDevice() : 0u;

      obj_handle_t layer = 0u;
      m_view  = CreateMetalViewFromHWND(hwnd, m_wmtDevice, &layer);
      m_layer = layer;
      if (!m_layer)
        Logger::err("MtlPresenterImpl: CreateMetalViewFromHWND returned no layer");
      applyProps();

      attachLayerResidency();
    }

    ~MtlPresenterImpl() {
      if (m_backend && m_layer)
        m_backend->residency().detachLayer(m_layer);
      if (m_view)
        ReleaseMetalView(m_view);
    }

    void recreate(const MtlPresenterDesc& desc) override {
      m_desc = desc;
      applyProps();
    }

    void setSyncInterval(uint32_t syncInterval) override {
      bool vsync = syncInterval != 0u;

      if (m_desc.vsync != vsync) {
        m_desc.vsync = vsync;
        applyProps();
      }
    }

    void syncLayerGeometry(uint32_t clientWidth, uint32_t clientHeight) override {
      if (!m_layer || !clientWidth || !clientHeight)
        return;

      /* Wine's client rect is already in backing pixels, so it IS the
       * drawable size — scaling it again would double the drawable. */
      WMTLayerProps cur = { };
      MetalLayer_getProps(m_layer, &cur);

      double w = double(clientWidth);
      double h = double(clientHeight);

      if (cur.drawable_width == w && cur.drawable_height == h)
        return;

      WMTLayerProps props = cur;
      props.drawable_width  = w;
      props.drawable_height = h;
      MetalLayer_setProps(m_layer, &props);
    }

    MtlAcquiredDrawable acquireNextImage(uint64_t frameId) override {
      MtlAcquiredDrawable image = {};
      image.sync.frameId = frameId;
      image.imageIndex   = uint32_t(frameId % (m_desc.imageCount ? m_desc.imageCount : 1u));

      if (!m_layer)
        return image;

      attachLayerResidency();

      if (std::chrono::steady_clock::now() < m_acquireBackoffUntil)
        return image;

      auto t0 = std::chrono::steady_clock::now();
      obj_handle_t drawable = MetalLayer_nextDrawable(m_layer);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
      if (unlikely(ms > kAcquireStallMs)) {
        m_acquireBackoffUntil = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(kAcquireBackoffMs);
      }

      if (unlikely(ms > 500))
        Logger::warn(str::format("MtlPresenter: nextDrawable blocked ", ms,
          "ms (frame ", frameId, ", got=", drawable != 0u, ")"));
      if (!drawable)
        return image;

      image.drawable = drawable;
      image.sync.drawable = drawable;

      Rc<MtlTextureImpl> tex = new MtlTextureImpl();
      obj_handle_t drawableTex = MetalDrawable_texture(drawable);
      tex->texture.tex = drawableTex;
      tex->format      = m_desc.imageFormat;
      uint32_t dw = uint32_t(MTLTexture_width(drawableTex));
      uint32_t dh = uint32_t(MTLTexture_height(drawableTex));
      tex->extent      = { dw ? dw : m_desc.imageExtent.width,
                           dh ? dh : m_desc.imageExtent.height, 1u };
      tex->m_info.type        = MtlImageType::e2D;
      tex->m_info.format      = m_desc.imageFormat;
      tex->m_info.extent      = { tex->extent.width, tex->extent.height, 1u };
      tex->m_info.mipLevels   = 1u;
      tex->m_info.numLayers   = 1u;
      tex->m_info.sampleCount = 1u;
      tex->m_info.usage       = MtlTextureUsageRenderTarget;

      Rc<MtlTextureViewImpl> view = new MtlTextureViewImpl();
      view->owned      = WmtTexture::retain(drawableTex);
      view->view.tex   = drawableTex;
      view->image      = tex.ptr();
      view->key.format = m_desc.imageFormat;

      image.texture    = tex;
      image.view       = view;
      image.valid      = true;
      return image;
    }

    void present(const MtlAcquiredDrawable& image) override {
      m_lastPresented = image.sync.frameId;
    }

    void signalFrameLatency(uint64_t frameId) override {
      /* Must advance every frame — the swapchain's frame limiter waits on
       * this monotonically-increasing counter and deadlocks otherwise. */
      if (frameId > m_completedFrame)
        m_completedFrame = frameId;
    }

    MtlExtent2D getImageExtent() const override { return m_desc.imageExtent; }

    bool isVisible() override {
      if (!m_view)
        return true;

      uint64_t vis = MetalView_visibility(m_view);

      return (vis & WMT_VISIBILITY_VISIBLE)
          && (vis & WMT_VISIBILITY_ON_ACTIVE_SPACE)
          && !(vis & WMT_VISIBILITY_MINIATURIZED);
    }

  private:

    Rc<MtlDevice>    m_device;
    MtlPresenterDesc m_desc;
    MtlDeviceImpl*   m_backend   = nullptr;
    obj_handle_t     m_wmtDevice = 0u;
    obj_handle_t     m_view      = 0u;
    obj_handle_t     m_layer     = 0u;
    std::chrono::steady_clock::time_point m_acquireBackoffUntil = { };
    bool             m_layerResidencyAttached = false;
    uint64_t         m_lastPresented = 0u;
    uint64_t         m_completedFrame = 0u;

    void attachLayerResidency() {
      if (m_layerResidencyAttached || !m_backend || !m_layer)
        return;
      m_layerResidencyAttached = m_backend->residency().attachLayer(m_layer);
    }

    void applyProps() {
      if (!m_layer)
        return;
      WMTLayerProps props = {};
      props.device               = m_wmtDevice;
      props.contents_scale       = 0.0;
      props.drawable_width       = double(m_desc.imageExtent.width);
      props.drawable_height      = double(m_desc.imageExtent.height);
      props.opaque               = true;
      props.display_sync_enabled = m_desc.vsync;
      /* The present path copies into the drawable via a compute encoder
       * (MTL4 has no blit encoder); a framebufferOnly drawable rejects that. */
      props.framebuffer_only     = false;
      props.pixel_format         = wmtFormat(m_desc.imageFormat);
      MetalLayer_setProps(m_layer, &props);
    }
  };

  Rc<MtlPresenter> createMtlPresenterImpl(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc) {
    return new MtlPresenterImpl(device, hwnd, desc);
  }


}

namespace dxvk {
  Rc<MtlPresenter> backendCreatePresenter(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc) {
    return backend::createMtlPresenterImpl(device, hwnd, desc);
  }
}
