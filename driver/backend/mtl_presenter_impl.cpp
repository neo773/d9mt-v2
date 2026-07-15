#include "mtl_presenter_impl.h"

#include "mtl_device_impl.h"

#include "../mtl/mtl_context.h"   // MtlDevice
#include "../util/log/log.h"

namespace dxvk::backend {

  namespace {
    WMTPixelFormat wmtFormat(MtlFormat f) { return WMTPixelFormat(uint32_t(f)); }
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

      /* Drawable residency: attach the layer's OWN residency set at queue
       * level (mtl_residency.h invariant 4) — drawable textures never enter
       * the driver set. The layer's set only exists once its device is
       * applied (async on the main thread), so retry from acquireNextImage
       * until it sticks. */
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

    MtlAcquiredDrawable acquireNextImage(uint64_t frameId) override {
      MtlAcquiredDrawable image = {};
      image.sync.frameId = frameId;
      image.imageIndex   = uint32_t(frameId % (m_desc.imageCount ? m_desc.imageCount : 1u));

      if (!m_layer)
        return image;

      /* The GPU writes the drawable this frame — its residency set must be
       * queue-attached before then (retries the async-init race, no-ops
       * once attached). */
      attachLayerResidency();

      obj_handle_t drawable = MetalLayer_nextDrawable(m_layer);
      if (!drawable)
        return image;   // occluded / no drawable this frame

      image.drawable = drawable;
      image.sync.drawable = drawable;

      /* wrap the drawable's texture so the swapchain can render into it. The
       * drawable owns the texture; we hold a borrowed handle only. */
      Rc<MtlTextureImpl> tex = new MtlTextureImpl();
      obj_handle_t drawableTex = MetalDrawable_texture(drawable);
      tex->texture.tex = drawableTex;
      tex->format      = m_desc.imageFormat;
      /* The CAMetalLayer sizes its drawables to the window, which under
       * CrossOver can differ from the requested imageExtent. Record the ACTUAL
       * drawable dimensions so the present copy clamps to them — copying the
       * full backbuffer into a smaller drawable is an out-of-bounds copy that
       * faults the GPU. */
      uint32_t dw = uint32_t(MTLTexture_width(drawableTex));
      uint32_t dh = uint32_t(MTLTexture_height(drawableTex));
      tex->extent      = { dw ? dw : m_desc.imageExtent.width,
                           dh ? dh : m_desc.imageExtent.height, 1u };

      /* Re-pin drawableSize when the layer drifted: applyProps sets it on the
       * main thread asynchronously, and any later view layout resets it to
       * bounds*contentsScale (window-sized). The present copy cannot scale, so
       * a window-sized drawable shows the backbuffer in the top-left corner.
       * Pinning back to the backbuffer size makes CA scale it to the window.
       * Takes effect on a subsequent drawable — self-heals within a frame. */
      if (dw && dh &&
          (dw != m_desc.imageExtent.width || dh != m_desc.imageExtent.height))
        applyProps();
      image.texture    = tex;
      image.valid      = true;
      return image;
    }

    void present(const MtlAcquiredDrawable& image) override {
      /* presentDrawable is recorded on the context's command buffer in
       * synchronizeWsi (must precede commit); here we only track the frame. */
      m_lastPresented = image.sync.frameId;
    }

    void signalFrameLatency(uint64_t frameId) override {
      /* Contract 3e: advance the frame-latency signal every frame or the
       * frame limiter deadlocks. The swapchain's limiter waits on this
       * monotonically-increasing counter. */
      if (frameId > m_completedFrame)
        m_completedFrame = frameId;
    }

    MtlExtent2D getImageExtent() const override { return m_desc.imageExtent; }

  private:

    Rc<MtlDevice>    m_device;
    MtlPresenterDesc m_desc;
    MtlDeviceImpl*   m_backend   = nullptr;   // borrowed; m_device keeps it alive
    obj_handle_t     m_wmtDevice = 0u;
    obj_handle_t     m_view      = 0u;
    obj_handle_t     m_layer     = 0u;
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
      props.contents_scale       = 1.0;
      props.drawable_width       = double(m_desc.imageExtent.width);
      props.drawable_height      = double(m_desc.imageExtent.height);
      props.opaque               = true;
      props.display_sync_enabled = m_desc.vsync;
      /* The present path copies the app backbuffer INTO the drawable via a
       * compute encoder (MTL4 has no blit encoder). A framebufferOnly drawable
       * cannot be a copy destination — Metal rejects the copy. Must be NO so
       * the drawable is copyable. */
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
