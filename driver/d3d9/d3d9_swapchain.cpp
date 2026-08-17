#include "d3d9_swapchain.h"
#include "d3d9_surface.h"
#include "d3d9_monitor.h"

#include "d3d9_window.h"

#include "../util/util_time.h"

#include <cstdio>
#include <cstdint>
#include <thread>
#include <chrono>

namespace dxvk {

  static void DumpBackbufferIfRequested(
          MtlContext*          ctx,
    const Rc<MtlDevice>&       device,
    const Rc<MtlTexture>&      image,
          uint32_t             paramsW,
          uint32_t             paramsH) {
    if (ctx == nullptr || device == nullptr || image == nullptr)
      return;

    FILE* trig = std::fopen("C:/d9mt-test/DUMP", "rb");
    if (trig == nullptr)
      return;
    std::fclose(trig);
    std::remove("C:/d9mt-test/DUMP");

    uint32_t w = uint32_t(image->extent.width);
    uint32_t h = uint32_t(image->extent.height);
    if (!w || !h) { w = image->info().extent.width;  h = image->info().extent.height; }
    if (!w || !h) { w = paramsW;                      h = paramsH; }
    if (!w || !h) {
      Logger::warn("D3D9 dump: zero extent, aborting");
      return;
    }

    const uint64_t rowBytes   = uint64_t(w) * 4u;
    const uint64_t imageBytes = rowBytes * h;

    MtlBufferCreateInfo bi = { };
    bi.size      = imageBytes;
    bi.usage     = MtlUsage::Staging;
    bi.cpuAccess = MtlCpuAccess::Read;
    Rc<MtlBuffer> readback = device->createBuffer(bi);
    if (readback == nullptr) {
      Logger::warn("D3D9 dump: createBuffer failed");
      return;
    }

    ctx->copyTextureToBuffer(
      readback, 0u, rowBytes, imageBytes, image->format,
      image, MtlSubresourceLayers{ }, MtlOrigin{ },
      MtlSize{ w, h, 1u });

    ctx->flushCommandList("backbuffer-dump", nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const void* src = readback->mapPtr(0u);
    if (src == nullptr) {
      Logger::warn("D3D9 dump: readback buffer not host-mapped");
      return;
    }

    FILE* out = std::fopen("C:/d9mt-test/rt_dump.bin", "wb");
    if (out == nullptr) {
      Logger::warn("D3D9 dump: could not open rt_dump.bin for write");
      return;
    }
    std::fwrite(&w, sizeof(uint32_t), 1u, out);
    std::fwrite(&h, sizeof(uint32_t), 1u, out);
    std::fwrite(src, 1u, size_t(imageBytes), out);
    std::fclose(out);
    Logger::info(str::format("D3D9 dump: wrote rt_dump.bin ", w, "x", h,
      " (", imageBytes, " bytes) fmt=", uint32_t(image->format)));
  }

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }

  D3D9SwapChainEx::D3D9SwapChainEx(
          D3D9DeviceEx*          pDevice,
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          bool                   EnableLatencyTracking)
    : D3D9SwapChainExBase(pDevice)
    , m_device           (pDevice->GetDXVKDevice())
    , m_frameLatencyCap  (pDevice->GetOptions()->maxFrameLatency)
    , m_latencyTracking  (EnableLatencyTracking) {
    this->NormalizePresentParameters(pPresentParams);
    m_presentParams = *pPresentParams;
    m_window = m_presentParams.hDeviceWindow;

    UpdateWindowCtx();

    UpdatePresentRegion(nullptr, nullptr);

    if (FAILED(CreateBackBuffers(m_presentParams.BackBufferCount, m_presentParams.Flags)))
      throw DxvkError("D3D9: Failed to create swapchain backbuffers");

    InitRamp();

    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {
    if (this_thread::isInModuleDetachment())
      return;

    {
      D3D9DeviceLock lock = m_parent->LockDevice();

      if (m_parent->GetMostRecentlyUsedSwapchain() == this) {
        m_parent->ResetMostRecentlyUsedSwapchain();
      }
    }

    DestroyBackBuffers();

    RestoreDisplayMode(m_monitor);

    for (auto& p : m_presenters) {
      p.second.presenter = nullptr;
    }

    m_parent->DecrementLosableCounter();
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DSwapChain9)
     || (GetParent()->IsExtended() && riid == __uuidof(IDirect3DSwapChain9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDirect3DSwapChain9), riid)) {
      Logger::warn("D3D9SwapChainEx::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD    dwFlags) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    m_parent->SetMostRecentlyUsedSwapchain(this);

    if (unlikely(m_parent->IsDeviceLost()))
      return D3DERR_DEVICELOST;

    if (m_backBuffers.empty())
      return D3D_OK;

    uint32_t presentInterval = m_presentParams.PresentationInterval;

    presentInterval = std::max(presentInterval, 1u);

    if (presentInterval == D3DPRESENT_INTERVAL_IMMEDIATE || (dwFlags & D3DPRESENT_FORCEIMMEDIATE))
      presentInterval = 0;

    auto options = m_parent->GetOptions();

    if (options->presentInterval >= 0)
      presentInterval = options->presentInterval;

    HWND window = m_presentParams.hDeviceWindow;

    if (hDestWindowOverride != nullptr)
      window = hDestWindowOverride;

    if (m_window != window) {
      m_window = window;
      m_displayRefreshRateDirty = true;
    }

    if (!UpdateWindowCtx())
      return D3D_OK;

    UpdatePresentRegion(pSourceRect, pDestRect);
    UpdatePresentParameters();

    if (m_wctx->presenter != nullptr) {
      m_wctx->presenter->setSyncInterval(presentInterval);

      RECT client = { };
      if (::GetClientRect(m_window, &client)) {
        m_wctx->presenter->syncLayerGeometry(
          uint32_t(client.right - client.left),
          uint32_t(client.bottom - client.top));
      }
    }

    if (wsi::isMinimized(m_window))
      return D3D_OK;

    if (!SwapWithFrontBuffer() && m_parent->GetOptions()->extraFrontbuffer) {
      const auto& backbuffer = m_backBuffers[0];
      const auto& frontbuffer = GetFrontBuffer();
      if (FAILED(m_parent->StretchRect(backbuffer.ptr(), nullptr, frontbuffer.ptr(), nullptr, D3DTEXF_NONE))) {
        Logger::err("Failed to blit to front buffer");
      }
    }

#ifdef _WIN32
    const bool useGDIFallback = m_partialCopy && !SwapWithFrontBuffer();
    if (useGDIFallback)
      return PresentImageGDI(m_window);
#endif

    try {
      UpdateWindowedRefreshRate();
      UpdateTargetFrameRate(presentInterval);
      PresentImage(presentInterval);
      m_fpsLimiter.delay();
      return D3D_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
#ifdef _WIN32
      return PresentImageGDI(m_window);
#else
      return D3DERR_DEVICEREMOVED;
#endif
    }
  }

#ifdef _WIN32
  #define DCX_USESTYLE 0x00010000

  HRESULT D3D9SwapChainEx::PresentImageGDI(HWND Window) {
    m_parent->EndFrame(nullptr);
    m_parent->Flush();

    if (!std::exchange(m_warnedAboutGDIFallback, true))
      Logger::warn("Using GDI for swapchain presentation. This will impact performance.");

    HDC hDC;
    HRESULT result = m_backBuffers[0]->GetDC(&hDC);
    if (result) {
      Logger::err("D3D9SwapChainEx::BlitGDI Surface GetDC failed");
      return D3DERR_DEVICEREMOVED;
    }

    HDC dstDC = GetDCEx(Window, 0, DCX_CACHE | DCX_USESTYLE);
    if (!dstDC) {
      Logger::err("D3D9SwapChainEx::BlitGDI: GetDCEx failed");
      m_backBuffers[0]->ReleaseDC(hDC);
      return D3DERR_DEVICEREMOVED;
    }

    bool success = StretchBlt(dstDC, m_dstRect.left, m_dstRect.top, m_dstRect.right - m_dstRect.left,
            m_dstRect.bottom - m_dstRect.top, hDC, m_srcRect.left, m_srcRect.top,
            m_srcRect.right - m_srcRect.left, m_srcRect.bottom - m_srcRect.top, SRCCOPY);

    m_backBuffers[0]->ReleaseDC(hDC);
    ReleaseDC(Window, dstDC);

    if (!success) {
      Logger::err("D3D9SwapChainEx::BlitGDI: StretchBlt failed");
      return D3DERR_DEVICEREMOVED;
    }

    return S_OK;
  }
#endif

  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetFrontBufferData(IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);

    if (unlikely(dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTexInfo = dst->GetCommonTexture();

    if (unlikely(dstTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM && dstTexInfo->Desc()->Pool != D3DPOOL_SCRATCH))
      return D3DERR_INVALIDCALL;

    if (unlikely(m_parent->IsDeviceLost()))
      return D3DERR_DEVICELOST;

    static bool s_errorShown = false;
    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::GetFrontBufferData: PHASE-E stub (no readback)");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetBackBuffer(
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(ppBackBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(iBackBuffer >= m_presentParams.BackBufferCount)) {
      Logger::err(str::format("D3D9: GetBackBuffer: Invalid back buffer index: ", iBackBuffer));
      return D3DERR_INVALIDCALL;
    }

    if (m_backBuffers.empty()) {
      *ppBackBuffer = nullptr;
      return D3D_OK;
    }

    *ppBackBuffer = ref(m_backBuffers[iBackBuffer].ptr());
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
    constexpr uint32_t vBlankLineCount = 20;

    if (unlikely(pRasterStatus == nullptr))
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    if (FAILED(this->GetDisplayModeEx(&mode, nullptr)))
      return D3DERR_INVALIDCALL;

    uint32_t scanLineCount = mode.Height + vBlankLineCount;

    auto nowUs = std::chrono::time_point_cast<std::chrono::microseconds>(
      dxvk::high_resolution_clock::now())
      .time_since_epoch();

    auto frametimeUs = std::chrono::microseconds(1000000u / mode.RefreshRate);
    auto scanLineUs  = frametimeUs / scanLineCount;

    pRasterStatus->ScanLine = (nowUs % frametimeUs) / scanLineUs;
    pRasterStatus->InVBlank = pRasterStatus->ScanLine >= mode.Height;

    if (pRasterStatus->InVBlank)
      pRasterStatus->ScanLine = 0;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayMode(D3DDISPLAYMODE* pMode) {
    if (unlikely(pMode == nullptr))
      return D3DERR_INVALIDCALL;

    *pMode = D3DDISPLAYMODE();

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    HRESULT hr = this->GetDisplayModeEx(&mode, nullptr);

    if (FAILED(hr))
      return hr;

    pMode->Width       = mode.Width;
    pMode->Height      = mode.Height;
    pMode->Format      = mode.Format;
    pMode->RefreshRate = mode.RefreshRate;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    *pPresentationParameters = m_presentParams;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetLastPresentCount(UINT* pLastPresentCount) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::GetLastPresentCount: Stub");

    if (likely(pLastPresentCount != nullptr))
      *pLastPresentCount = 0;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::GetPresentStats: Stub");

    if (likely(pPresentationStatistics != nullptr)) {
      D3DPRESENTSTATS presentationStatistics = { };
      *pPresentationStatistics = presentationStatistics;
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    if (unlikely(pMode == nullptr && pRotation == nullptr))
      return D3DERR_INVALIDCALL;

    if (pRotation != nullptr)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;

    if (pMode != nullptr) {
      if (unlikely(pMode->Size != sizeof(D3DDISPLAYMODEEX)))
        return D3DERR_INVALIDCALL;

      wsi::WsiMode devMode = { };

      if (!wsi::getCurrentDisplayMode(wsi::getDefaultMonitor(), &devMode)) {
        Logger::err("D3D9SwapChainEx::GetDisplayModeEx: Failed to enum display settings");
        return D3DERR_INVALIDCALL;
      }

      *pMode = ConvertDisplayMode(devMode);
    }

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::Reset(
          D3DPRESENT_PARAMETERS* pPresentParams,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    HRESULT hr = D3D_OK;

    this->NormalizePresentParameters(pPresentParams);

    bool changeFullscreen = m_presentParams.Windowed != pPresentParams->Windowed;

    if (pPresentParams->Windowed) {
      if (changeFullscreen)
        this->LeaveFullscreenMode();
    }
    else {
      m_parent->NotifyFullscreen(m_window, true);

      if (changeFullscreen) {
        hr = this->EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode);
        if (FAILED(hr))
          return hr;
      }

      if (!changeFullscreen) {
        RECT rect = { };
        wsi::getDesktopCoordinates(m_monitor, &rect);

        ::SetWindowPos(m_window, HWND_TOP,
          rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
          SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
    }

    m_presentParams = *pPresentParams;

    if (changeFullscreen)
      SetGammaRamp(0, &m_ramp);

    Logger::info("D9MT-RESET: UpdatePresentParameters...");
    UpdatePresentParameters();

    Logger::info("D9MT-RESET: CreateBackBuffers...");
    hr = CreateBackBuffers(m_presentParams.BackBufferCount, m_presentParams.Flags);
    if (FAILED(hr))
      return hr;

    Logger::info("D9MT-RESET: done");
    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::WaitForVBlank() {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::WaitForVBlank: Stub");

    return D3D_OK;
  }

  static bool validateGammaRamp(const WORD (&ramp)[256]) {
    if (ramp[0] >= ramp[std::size(ramp) - 1]) {
      Logger::warn("validateGammaRamp: ramp inverted or flat");
      return false;
    }

    for (size_t i = 1; i < std::size(ramp); i++) {
      if (ramp[i] < ramp[i - 1]) {
        Logger::warn("validateGammaRamp: ramp not monotonically increasing");
        return false;
      }
      if (ramp[i] - ramp[i - 1] >= UINT16_MAX / 2) {
        Logger::warn("validateGammaRamp: huuuge jump");
        return false;
      }
    }

    return true;
  }


  void    D3D9SwapChainEx::SetGammaRamp(
            DWORD         Flags,
      const D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(pRamp == nullptr))
      return;

    if (unlikely(!validateGammaRamp(pRamp->red)
              && !validateGammaRamp(pRamp->blue)
              && !validateGammaRamp(pRamp->green)))
      return;

    m_ramp = *pRamp;
  }


  void    D3D9SwapChainEx::GetGammaRamp(D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (likely(pRamp != nullptr))
      *pRamp = m_ramp;
  }


  void    D3D9SwapChainEx::Invalidate(HWND hWindow) {
    if (!hWindow)
      hWindow = m_parent->GetWindow();

    auto entry = m_presenters.find(hWindow);

    if (entry != m_presenters.end()) {
      if (entry->second.presenter) {
        entry->second.presenter = nullptr;

        if (m_presentParams.hDeviceWindow == hWindow)
          DestroyLatencyTracker();
      }

      if (m_wctx == &entry->second)
        m_wctx = nullptr;

      m_presenters.erase(entry);
    }
  }


  void D3D9SwapChainEx::SetCursorTexture(UINT Width, UINT Height, uint8_t* pCursorBitmap) {
    (void)Width; (void)Height; (void)pCursorBitmap;
  }


  void D3D9SwapChainEx::SetCursorPosition(int32_t X, int32_t Y, UINT Width, UINT Height) {
    (void)X; (void)Y; (void)Width; (void)Height;
  }


  HRESULT D3D9SwapChainEx::SetDialogBoxMode(bool bEnableDialogs) {
    return D3D_OK;
  }


  D3D9Surface* D3D9SwapChainEx::GetBackBuffer(UINT iBackBuffer) {
    if (iBackBuffer >= m_presentParams.BackBufferCount)
      return nullptr;

    return m_backBuffers[iBackBuffer].ptr();
  }


  void D3D9SwapChainEx::NormalizePresentParameters(D3DPRESENT_PARAMETERS* pPresentParams) {
    if (pPresentParams->hDeviceWindow == nullptr)
      pPresentParams->hDeviceWindow    = m_parent->GetWindow();

    pPresentParams->BackBufferCount    = std::max(pPresentParams->BackBufferCount, 1u);

    if (pPresentParams->Windowed) {
      wsi::getWindowSize(pPresentParams->hDeviceWindow,
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }
    else {
      wsi::getMonitorClientSize(wsi::getDefaultMonitor(),
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }

    if (pPresentParams->BackBufferFormat == D3DFMT_UNKNOWN)
      pPresentParams->BackBufferFormat = D3DFMT_X8R8G8B8;

    if (env::getEnvVar("DXVK_FORCE_WINDOWED") == "1")
      pPresentParams->Windowed         = TRUE;
  }


  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
    m_parent->EndFrame(m_latencyTracker);
    m_parent->Flush();

    Rc<MtlTexture>     swapImage     = m_backBuffers[0]->GetCommonTexture()->GetImage();
    Rc<MtlTextureView> swapImageView = m_backBuffers[0]->GetCommonTexture()->GetSampleView(false);

    Rc<MtlTexture>     resolveImage;
    Rc<MtlTextureView> resolveView;
    if (m_resolveBuffer != nullptr) {
      resolveImage = m_resolveBuffer->GetCommonTexture()->GetImage();
      resolveView  = m_resolveBuffer->GetCommonTexture()->GetSampleView(false);
    }

    m_wctx->frameId += 1;

    // The entire present (acquire/blit/signal/present/release) runs on the CS
    // thread: acquiring on the producer thread instead let drawables exceed
    // maximumDrawableCount(3), deadlocking both threads.
    m_parent->EmitCs([
      cPresenter    = m_wctx->presenter,
      cSrcView      = swapImageView,
      cSrcImage     = swapImage,
      cResolveView  = resolveView,
      cResolveImage = resolveImage,
      cDevice       = m_device,
      cFrameId      = m_wctx->frameId,
      cParamsW      = uint32_t(m_presentParams.BackBufferWidth),
      cParamsH      = uint32_t(m_presentParams.BackBufferHeight),
      cSignal       = m_wctx->frameLatencySignal
    ] (MtlContext* ctx) {
      if (cResolveImage != nullptr && cSrcImage != nullptr) {
        MtlResolveRegion region = { };
        region.dstSubresource = { MtlAspectColor, 0u, 0u, 1u };
        region.srcSubresource = { MtlAspectColor, 0u, 0u, 1u };
        region.extent         = cSrcImage->info().extent;
        ctx->resolveTexture(cResolveImage, cSrcImage, region,
          cSrcImage->info().format, MtlResolveMode::Average, MtlResolveMode::SampleZero);
      }
      const Rc<MtlTexture>&     cPresentImage = cResolveImage != nullptr ? cResolveImage : cSrcImage;
      const Rc<MtlTextureView>& cPresentView  = cResolveView  != nullptr ? cResolveView  : cSrcView;

      DumpBackbufferIfRequested(ctx, cDevice, cPresentImage, cParamsW, cParamsH);

      if (!cPresenter->isVisible()) {
        ctx->flushCommandList("present-skipped-not-visible", nullptr);
        cPresenter->signalFrameLatency(cFrameId);
        if (cSignal != nullptr)
          cSignal->signal(cFrameId);
        return;
      }

      MtlAcquiredDrawable cDrawable = cPresenter->acquireNextImage(cFrameId);
      if (!cDrawable.valid) {
        ctx->flushCommandList("present-skipped-no-drawable", nullptr);
        cPresenter->signalFrameLatency(cFrameId);
        if (cSignal != nullptr)
          cSignal->signal(cFrameId);
        return;
      }

      if (cPresentView != nullptr && cPresentView->image != nullptr && cDrawable.texture != nullptr) {
        const MtlSize src = cPresentView->image->extent;
        const MtlSize dst = cDrawable.texture->extent;
        if (src.width == dst.width && src.height == dst.height) {
          MtlSize extent = { src.width, src.height, 1u };
          ctx->copyTexture(
            cDrawable.texture,     MtlSubresourceLayers{ }, MtlOrigin{ },
            cPresentView->image,   MtlSubresourceLayers{ }, MtlOrigin{ }, extent);
        } else if (cDrawable.view != nullptr) {
          if (!ctx->presentUpscale(cDrawable.texture, cPresentImage)) {
            MtlOffset3D dstBox[2] = { { 0, 0, 0 },
              { int32_t(dst.width), int32_t(dst.height), 1 } };
            MtlOffset3D srcBox[2] = { { 0, 0, 0 },
              { int32_t(src.width), int32_t(src.height), 1 } };
            ctx->blitTextureView(cDrawable.view, dstBox, cPresentView, srcBox,
              MtlSamplerFilter::Linear);
          }
        } else {
          MtlSize extent = {
            std::min(src.width,  dst.width  ? dst.width  : src.width),
            std::min(src.height, dst.height ? dst.height : src.height),
            1u };
          if (extent.width && extent.height) {
            ctx->copyTexture(
              cDrawable.texture,     MtlSubresourceLayers{ }, MtlOrigin{ },
              cPresentView->image,   MtlSubresourceLayers{ }, MtlOrigin{ }, extent);
          }
        }
      }
      // No pre-commit waitForDrawable: waiting on a drawable the display
      // never releases would block the queue timeline forever.
      ctx->synchronizeWsi(cDrawable.sync);
      cPresenter->present(cDrawable);
      Logger::debug("D3D9: frame presented");

      cPresenter->signalFrameLatency(cFrameId);
      if (cSignal != nullptr)
        cSignal->signal(cFrameId);
    });

    m_parent->FlushCsChunk();

    SyncFrameLatency();

    (void)0;

    m_parent->m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  }


  Rc<MtlPresenter> D3D9SwapChainEx::CreatePresenter(HWND Window, Rc<sync::Signal> Signal) {
    (void)Signal;

    MtlPresenterDesc desc = { };
    desc.imageExtent = m_swapchainExtent;
    desc.imageFormat = GetSurfaceFormat();
    desc.imageCount  = m_presentParams.BackBufferCount + 1u;
    desc.vsync       = true;
    desc.hwnd        = reinterpret_cast<intptr_t>(Window);

    return createMtlPresenter(m_device, desc);
  }


  void D3D9SwapChainEx::DestroyBackBuffers() {
    for (auto& backBuffer : m_backBuffers)
      backBuffer->ClearContainer();

    m_backBuffers.clear();

    if (m_resolveBuffer != nullptr) {
      m_resolveBuffer->ClearContainer();
      m_resolveBuffer = nullptr;
    }
  }


  bool D3D9SwapChainEx::UpdateWindowCtx() {
    if (!m_window)
      return false;

    auto entry = m_presenters.find(m_window);

    if (entry == m_presenters.end()) {
      entry = m_presenters.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(m_window),
        std::forward_as_tuple()).first;

      entry->second.frameLatencySignal = new sync::Fence(entry->second.frameId);
      entry->second.presenter = CreatePresenter(m_window, entry->second.frameLatencySignal);
    }

    m_wctx = &entry->second;
    return true;
  }


  HRESULT D3D9SwapChainEx::CreateBackBuffers(uint32_t NumBackBuffers, DWORD Flags) {
    DestroyBackBuffers();

    const uint32_t frontBufferCount = (SwapWithFrontBuffer() || m_parent->GetOptions()->extraFrontbuffer) ? 1 : 0;
    const uint32_t bufferCount = NumBackBuffers + frontBufferCount;

    m_backBuffers.reserve(bufferCount);

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_presentParams.BackBufferWidth,  1u);
    desc.Height             = std::max(m_presentParams.BackBufferHeight, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = EnumerateFormat(m_presentParams.BackBufferFormat);
    desc.MultiSample        = m_presentParams.MultiSampleType;
    desc.MultisampleQuality = m_presentParams.MultiSampleQuality;
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Usage              = D3DUSAGE_RENDERTARGET;
    desc.Discard            = FALSE;
    desc.IsBackBuffer       = TRUE;
    desc.IsAttachmentOnly   = FALSE;
    desc.IsLockable         = true;

    for (uint32_t i = 0; i < bufferCount; i++) {
      D3D9Surface* surface;
      try {
        surface = new D3D9Surface(m_parent, &desc, m_parent->IsExtended(), this, nullptr);
        m_parent->IncrementLosableCounter();
      } catch (const DxvkError& e) {
        DestroyBackBuffers();
        Logger::err(e.message());
        return D3DERR_OUTOFVIDEOMEMORY;
      }

      m_backBuffers.emplace_back(surface);
    }

    if (m_presentParams.MultiSampleType != D3DMULTISAMPLE_NONE) {
      D3D9_COMMON_TEXTURE_DESC resolveDesc = desc;
      resolveDesc.MultiSample        = D3DMULTISAMPLE_NONE;
      resolveDesc.MultisampleQuality = 0;
      try {
        m_resolveBuffer = new D3D9Surface(m_parent, &resolveDesc, m_parent->IsExtended(), this, nullptr);
        m_parent->IncrementLosableCounter();
      } catch (const DxvkError& e) {
        DestroyBackBuffers();
        Logger::err(e.message());
        return D3DERR_OUTOFVIDEOMEMORY;
      }
    }

    small_vector<Rc<MtlTexture>, 4> images;

    for (size_t i = 0; i < m_backBuffers.size(); i++)
      images.push_back(m_backBuffers[i]->GetCommonTexture()->GetImage());

    if (m_resolveBuffer != nullptr)
      images.push_back(m_resolveBuffer->GetCommonTexture()->GetImage());

    m_parent->InjectCs([
      cImages = std::move(images)
    ] (MtlContext* ctx) {
      for (size_t i = 0; i < cImages.size(); i++) {
        ctx->initImage(cImages[i]);
      }
    });

    return D3D_OK;
  }


  void D3D9SwapChainEx::InitRamp() {
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      DWORD identity = DWORD(MapGammaControlPoint(float(i) / float(NumControlPoints - 1)));

      m_ramp.red[i]   = identity;
      m_ramp.green[i] = identity;
      m_ramp.blue[i]  = identity;
    }
  }


  void D3D9SwapChainEx::DestroyLatencyTracker() {
    if (!m_latencyTracker)
      return;

    m_parent->InjectCs([
      cTracker = std::move(m_latencyTracker)
    ] (MtlContext* ctx) {
      ctx->endLatencyTracking(cTracker);
    });
  }


  void D3D9SwapChainEx::UpdateTargetFrameRate(uint32_t SyncInterval) {
    double frameRate = double(m_parent->GetOptions()->maxFrameRate);

    if (frameRate != -1.0) {
      if (frameRate == 0.0 && SyncInterval) {
        bool engageLimiter = SyncInterval > 1u || m_monitor;

        if (engageLimiter)
          frameRate = -m_displayRefreshRate / double(SyncInterval);
      }

      m_targetFrameRate = frameRate;
    }

    if (!m_presentParams.Windowed && m_presentParams.FullScreen_RefreshRateInHz)
      m_targetFrameRate = double(m_presentParams.FullScreen_RefreshRateInHz);

    if (m_parent->GetOptions()->maxFrameRate > 0)
      m_targetFrameRate = double(m_parent->GetOptions()->maxFrameRate);

    m_fpsLimiter.setTargetFrameRate(m_targetFrameRate, GetActualFrameLatency());
  }


  double D3D9SwapChainEx::GetPresentMinimumDuration(uint32_t SyncInterval) const {
    double refresh = m_targetFrameRate > 0.0
      ? m_targetFrameRate
      : m_displayRefreshRate;

    if (!SyncInterval || refresh < 20.0 || refresh > 1000.0)
      return 0.0;

    double duration = double(SyncInterval) / refresh;

    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      Logger::info(str::format("D3D9: present pacing ", duration * 1000.0,
        "ms (target ", refresh, "Hz, interval ", SyncInterval, ")"));
    }

    return duration;
  }


  void D3D9SwapChainEx::SyncFrameLatency() {
    m_wctx->frameLatencySignal->wait(m_wctx->frameId - GetActualFrameLatency());
  }

  uint32_t D3D9SwapChainEx::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_parent->GetFrameLatency();

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_presentParams.BackBufferCount + 1);
    return maxFrameLatency;
  }


  MtlFormat D3D9SwapChainEx::GetSurfaceFormat() {
    D3D9Format format = EnumerateFormat(m_presentParams.BackBufferFormat);

    switch (format) {
      default:
        Logger::warn(str::format("D3D9SwapChainEx: Unexpected format: ", format));
        [[fallthrough]];

      case D3D9Format::A8R8G8B8:
      case D3D9Format::X8R8G8B8:
        return MtlFormat::BGRA8Unorm;

      case D3D9Format::A8B8G8R8:
      case D3D9Format::X8B8G8R8:
        return MtlFormat::RGBA8Unorm;

      case D3D9Format::A2R10G10B10:
        return MtlFormat::BGR10A2Unorm;

      case D3D9Format::A2B10G10R10:
        return MtlFormat::RGB10A2Unorm;

      case D3D9Format::R5G6B5:
        return MtlFormat::B5G6R5Unorm;

      case D3D9Format::A16B16G16R16F: {
        if (!m_parent->HasFormatsUnlocked()) {
          Logger::warn(str::format("D3D9SwapChainEx: Unexpected format: ", format));
          return MtlFormat::BGRA8Unorm;
        }

        return MtlFormat::RGBA16Float;
      }
    }
  }


  void D3D9SwapChainEx::UpdateWindowedRefreshRate() {
    if (!m_displayRefreshRateDirty || m_monitor)
      return;

    m_displayRefreshRate = 0.0;
    m_displayRefreshRateDirty = false;

    HMONITOR monitor = wsi::getWindowMonitor(m_window);

    if (!monitor)
      return;

    wsi::WsiMode mode = { };

    if (!wsi::getCurrentDisplayMode(monitor, &mode))
      return;

    if (mode.refreshRate.denominator) {
      m_displayRefreshRate = double(mode.refreshRate.numerator)
                           / double(mode.refreshRate.denominator);
    }
  }


  HRESULT D3D9SwapChainEx::EnterFullscreenMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    m_monitor = wsi::getDefaultMonitor();

    RECT rect = { };
    wsi::getDesktopCoordinates(m_monitor, &rect);

    wsi::saveWindowState(m_window, &m_windowState, true);

    if (m_fullscreenWindow != m_window) {
      m_fullscreenWindow   = m_window;
      m_savedWindowStyle   = ::GetWindowLongW(m_window, GWL_STYLE);
      m_savedWindowExStyle = ::GetWindowLongW(m_window, GWL_EXSTYLE);

      LONG style   = (m_savedWindowStyle | WS_POPUP | WS_SYSMENU)
                   & ~(WS_CAPTION | WS_THICKFRAME);
      LONG exStyle = m_savedWindowExStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);

      ::SetWindowLongW(m_window, GWL_STYLE,   style);
      ::SetWindowLongW(m_window, GWL_EXSTYLE, exStyle);
    }

    ::SetWindowPos(m_window, HWND_TOP,
      rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    m_displayRefreshRateDirty = true;

    m_parent->NotifyFullscreen(m_window, true);

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::LeaveFullscreenMode() {
    if (!wsi::isWindow(m_window))
      return D3DERR_INVALIDCALL;

    m_monitor = nullptr;

    if (m_fullscreenWindow == m_window) {
      LONG curStyle   = ::GetWindowLongW(m_window, GWL_STYLE)   & WS_VISIBLE;
      LONG curExStyle = ::GetWindowLongW(m_window, GWL_EXSTYLE) & WS_EX_TOPMOST;

      ::SetWindowLongW(m_window, GWL_STYLE,
        (m_savedWindowStyle   & ~WS_VISIBLE)     | curStyle);
      ::SetWindowLongW(m_window, GWL_EXSTYLE,
        (m_savedWindowExStyle & ~WS_EX_TOPMOST)  | curExStyle);

      ::SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER |
        SWP_NOMOVE | SWP_NOSIZE);

      m_fullscreenWindow = nullptr;
    }

    wsi::restoreWindowState(m_window, &m_windowState, false);

    m_parent->NotifyFullscreen(m_window, false);

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::ChangeDisplayMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3DDISPLAYMODEEX mode;

    if (pFullscreenDisplayMode) {
      mode = *pFullscreenDisplayMode;
    } else {
      mode.Width            = pPresentParams->BackBufferWidth;
      mode.Height           = pPresentParams->BackBufferHeight;
      mode.Format           = pPresentParams->BackBufferFormat;
      mode.RefreshRate      = pPresentParams->FullScreen_RefreshRateInHz;
      mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      mode.Size             = sizeof(D3DDISPLAYMODEEX);
    }

    wsi::WsiMode wsiMode = ConvertDisplayMode(mode);

    HMONITOR monitor = wsi::getDefaultMonitor();

    if (!wsi::setWindowMode(monitor, m_window, &m_windowState, wsiMode))
      return D3DERR_NOTAVAILABLE;

    m_displayRefreshRate = 0.0;

    if (wsi::getCurrentDisplayMode(monitor, &wsiMode)) {
      m_displayRefreshRate = double(wsiMode.refreshRate.numerator)
                           / double(wsiMode.refreshRate.denominator);
    }

    m_displayRefreshRateDirty = false;
    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::RestoreDisplayMode(HMONITOR hMonitor) {
    if (hMonitor == nullptr)
      return D3DERR_INVALIDCALL;

    if (!wsi::restoreDisplayMode())
      return D3DERR_NOTAVAILABLE;

    m_displayRefreshRateDirty = true;
    return D3D_OK;
  }

  void D3D9SwapChainEx::UpdatePresentRegion(const RECT* pSourceRect, const RECT* pDestRect) {
    const bool isWindowed = m_presentParams.Windowed;

    if (pSourceRect == nullptr || !isWindowed) {
      m_srcRect.top    = 0;
      m_srcRect.left   = 0;
      m_srcRect.right  = m_presentParams.BackBufferWidth;
      m_srcRect.bottom = m_presentParams.BackBufferHeight;
    }
    else
      m_srcRect = *pSourceRect;


    UINT width, height;
    wsi::getWindowSize(m_window, &width, &height);

    RECT dstRect;
    if (pDestRect == nullptr || !isWindowed) {
      dstRect.top    = 0;
      dstRect.left   = 0;
      dstRect.right  = LONG(width);
      dstRect.bottom = LONG(height);
    }
    else
      dstRect = *pDestRect;

    m_partialCopy =
       dstRect.left != 0
    || dstRect.top != 0
    || dstRect.right  - dstRect.left != LONG(width)
    || dstRect.bottom - dstRect.top  != LONG(height);

    m_swapchainExtent = { width, height };
    m_dstRect = dstRect;
  }

  void D3D9SwapChainEx::UpdatePresentParameters() {
    if (m_wctx && m_wctx->presenter) {
      MtlExtent2D cur = m_wctx->presenter->getImageExtent();

      if (!(cur == m_swapchainExtent)) {
        MtlPresenterDesc desc = { };
        desc.imageExtent = m_swapchainExtent;
        desc.imageFormat = GetSurfaceFormat();
        desc.imageCount  = m_presentParams.BackBufferCount + 1u;
        desc.vsync       = true;
        m_wctx->presenter->recreate(desc);
      }
    }
  }

  MtlExtent2D D3D9SwapChainEx::GetPresentExtent() {
    return m_swapchainExtent;
  }


  std::string D3D9SwapChainEx::GetApiName() {
    if (this->GetParent()->Is9On12Device())
      return this->GetParent()->IsExtended() ? "D3D9On12Ex" : "D3D9On12";

    return this->GetParent()->IsD3D8Compatible() ? "D3D8" :
           this->GetParent()->IsExtended() ? "D3D9Ex" : "D3D9";
  }


  bool D3D9SwapChainEx::IsDeviceReset(D3D9WindowContext* wctx) {
    uint32_t counter = m_parent->GetResetCounter();

    if (counter == wctx->deviceResetCounter)
      return false;

    wctx->deviceResetCounter = counter;
    return true;
  }

}
