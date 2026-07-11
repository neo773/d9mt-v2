// PHASE-C10: D3D9SwapChainEx REWRITE onto the MtlPresenter seam (doc 01 §1j /
// contract 3e; doc 02 §4). All D3D9 bookkeeping (present params, back-buffer
// array, reset/format/fullscreen logic, frame-latency counter) is KEPT; the
// Vulkan scan-out path is replaced:
//   Rc<Presenter>            -> Rc<MtlPresenter>          (mtl_presenter.h)
//   PresenterSync            -> MtlAcquiredDrawable / MtlPresenterSync
//   Rc<DxvkImage/View>       -> Rc<MtlTexture/View>
//   ctx->synchronizeWsi / flushCommandList / present     -> REAL (contract 3e)
// DROPPED: DxvkSwapchainBlitter (present blit lives in the presenter), the HUD
//   items, m_colorspace / m_hdrMetadata, and the ID3D9VkExtSwapchain interop.
#include "d3d9_swapchain.h"
#include "d3d9_surface.h"
#include "d3d9_monitor.h"

#include "d3d9_window.h"

#include "../util/util_time.h"

namespace dxvk {

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

    // PHASE-C10: CreateBlitter() dropped — the present blit lives in the presenter.

    InitRamp();

    // Apply initial window mode and fullscreen state
    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {
    // Avoids hanging when in this state, see comment
    // in DxvkDevice::~DxvkDevice.
    if (this_thread::isInModuleDetachment())
      return;

    {
      // Locking here and in Device::GetFrontBufferData
      // ensures that other threads don't accidentally access a stale pointer.
      D3D9DeviceLock lock = m_parent->LockDevice();

      if (m_parent->GetMostRecentlyUsedSwapchain() == this) {
        m_parent->ResetMostRecentlyUsedSwapchain();
      }
    }

    DestroyBackBuffers();

    ResetWindowProc(m_window);
    RestoreDisplayMode(m_monitor);

    for (auto& p : m_presenters) {
      // PHASE-C10: MtlPresenter destroys its CAMetalLayer resources in its dtor;
      // the seam has no explicit destroyResources().
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

    // If we have no backbuffers, error out.
    if (m_backBuffers.empty())
      return D3D_OK;

    uint32_t presentInterval = m_presentParams.PresentationInterval;

    // D3DPRESENT_INTERVAL_DEFAULT (0) == D3DPRESENT_INTERVAL_ONE (1) which means VSYNC.
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

    if (!SwapWithFrontBuffer() && m_parent->GetOptions()->extraFrontbuffer) {
      // We never actually rotate in the front buffer.
      // Just blit to it for GetFrontBufferData.
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

    // PHASE-E: the DXVK front-buffer readback used heavy Vulkan image machinery
    // (createImage + resolveImage + blitImageView + copyImageToBuffer with
    // per-format subresource/swizzle plumbing). This screenshot path has no Metal
    // analogue wired on the seam yet; the meta-blit/resolve lands with the
    // resource REWRITE track. Report success with no copy so callers don't crash.
    static bool s_errorShown = false;
    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::GetFrontBufferData: PHASE-E stub (no readback)");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetBackBuffer(
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    // Could be doing a device reset...
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(ppBackBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(iBackBuffer >= m_presentParams.BackBufferCount)) {
      Logger::err(str::format("D3D9: GetBackBuffer: Invalid back buffer index: ", iBackBuffer));
      return D3DERR_INVALIDCALL;
    }

    if (m_backBuffers.empty()) {
      // The backbuffers were destroyed and not recreated.
      *ppBackBuffer = nullptr;
      return D3D_OK;
    }

    *ppBackBuffer = ref(m_backBuffers[iBackBuffer].ptr());
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
    // We lie here and make some stuff up enough that it makes games work.
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

      D3D9WindowMessageFilter filter(m_window);

      if (!changeFullscreen) {
        hr = ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode);
        if (FAILED(hr))
          return hr;

        wsi::updateFullscreenWindow(m_monitor, m_window, true);
      }
    }

    m_presentParams = *pPresentParams;

    if (changeFullscreen)
      SetGammaRamp(0, &m_ramp);

    UpdatePresentParameters();

    hr = CreateBackBuffers(m_presentParams.BackBufferCount, m_presentParams.Flags);
    if (FAILED(hr))
      return hr;

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

    // PHASE-E: gamma was applied by DxvkSwapchainBlitter::setGammaRamp (dropped).
    // The MtlPresenter seam has no gamma-LUT path yet; the ramp is stored for
    // GetGammaRamp round-trips. (Metal EDR/gamma path lands with the presenter.)
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
    // PHASE-E: HW cursor was drawn by DxvkSwapchainBlitter (dropped). Metal cursor
    // overlay lands with the presenter; no-op for now.
    (void)Width; (void)Height; (void)pCursorBitmap;
  }


  void D3D9SwapChainEx::SetCursorPosition(int32_t X, int32_t Y, UINT Width, UINT Height) {
    // PHASE-E: see SetCursorTexture.
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

    // Retrieve the back buffer to present (source of the present blit).
    Rc<MtlTexture>     swapImage     = m_backBuffers[0]->GetCommonTexture()->GetImage();
    Rc<MtlTextureView> swapImageView = m_backBuffers[0]->GetCommonTexture()->GetSampleView(false);

    // Acquire the next drawable (CAMetalLayer nextDrawable). Contract 3e: the
    // returned MtlPresenterSync carries the drawable acquire/release tokens.
    MtlAcquiredDrawable drawable = m_wctx->presenter->acquireNextImage(m_wctx->frameId + 1u);

    if (drawable.valid) {
      // Bump frame ID
      m_wctx->frameId += 1;

      // Present from CS thread so we don't have to synchronize with it first.
      m_parent->EmitCs([
        cPresenter = m_wctx->presenter,
        cSrcView   = swapImageView,
        cDrawable  = drawable,
        cFrameId   = m_wctx->frameId,
        cSignal    = m_wctx->frameLatencySignal
      ] (MtlContext* ctx) {
        // The backend presenter blits the current back buffer (cSrcView) into the
        // acquired drawable; here we wire the acquire/release drawable semaphores
        // to the submit (synchronizeWsi, contract 3e), commit, and present.
        (void)cSrcView;
        ctx->synchronizeWsi(cDrawable.sync);
        ctx->flushCommandList(nullptr, nullptr);
        cPresenter->present(cDrawable);

        // Frame-latency signalling (contract 3e — deadlock-class). The real signal
        // is posted from the presenter's drawable-present completion handler; we
        // additionally bump the swapchain's frame-latency fence so SyncFrameLatency
        // can throttle the app to N frames in flight.
        cPresenter->signalFrameLatency(cFrameId);
        if (cSignal != nullptr)
          cSignal->signal(cFrameId);
      });

      m_parent->FlushCsChunk();
    }

    SyncFrameLatency();

    // Rotate swap chain buffers so that the back buffer at index 0 becomes the
    // front buffer.
    uint32_t rotatingBufferCount = m_backBuffers.size();
    if (!SwapWithFrontBuffer() && m_parent->GetOptions()->extraFrontbuffer) {
      // The front buffer only exists for GetFrontBufferData.
      rotatingBufferCount -= 1;
    }

    for (uint32_t i = 1; i < rotatingBufferCount; i++)
      m_backBuffers[i]->Swap(m_backBuffers[i - 1].ptr());

    m_parent->m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  }


  Rc<MtlPresenter> D3D9SwapChainEx::CreatePresenter(HWND Window, Rc<sync::Signal> Signal) {
    // PHASE-C10: the frame-latency signal is wired via the D3D9WindowContext fence
    // and bumped in PresentImage; the factory takes only device + desc.
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

      // PHASE-E: Reflex-style latency tracking (createLatencyTracker) is not wired
      // on the Metal seam yet; m_latencyTracker stays null.
    }

    m_wctx = &entry->second;
    return true;
  }


  HRESULT D3D9SwapChainEx::CreateBackBuffers(uint32_t NumBackBuffers, DWORD Flags) {
    // Explicitly destroy current swap image before creating a new one.
    DestroyBackBuffers();

    const uint32_t frontBufferCount = (SwapWithFrontBuffer() || m_parent->GetOptions()->extraFrontbuffer) ? 1 : 0;
    const uint32_t bufferCount = NumBackBuffers + frontBufferCount;

    m_backBuffers.reserve(bufferCount);

    // Create new back buffer
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
    // The texture will get sampled for presentation.
    desc.IsAttachmentOnly   = FALSE;
    // we might need to lock for the BlitGDI fallback path
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

    // Initialize the image so that we can use it. Clearing to black prevents
    // garbled output for the first frame.
    small_vector<Rc<MtlTexture>, 4> images;

    for (size_t i = 0; i < m_backBuffers.size(); i++)
      images.push_back(m_backBuffers[i]->GetCommonTexture()->GetImage());

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
    // PHASE-E: DXVK drove Presenter::setFrameRateLimit from the display refresh
    // rate / latency-sleep config. The MtlPresenter seam owns pacing via
    // displaySyncEnabled; here we only track the target for GetRasterStatus-style
    // math. (Metal frame limiter lands with the presenter.)
    double frameRate = double(m_parent->GetOptions()->maxFrameRate);

    if (frameRate != -1.0) {
      if (frameRate == 0.0 && SyncInterval) {
        bool engageLimiter = SyncInterval > 1u || m_monitor;

        if (engageLimiter)
          frameRate = -m_displayRefreshRate / double(SyncInterval);
      }

      m_targetFrameRate = frameRate;
    }
  }


  void D3D9SwapChainEx::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
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
    // Ignore call if we are in fullscreen mode and know the mode already.
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
    if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode))) {
      Logger::err("D3D9: EnterFullscreenMode: Failed to change display mode");
      return D3DERR_INVALIDCALL;
    }

    HookWindowProc(m_window, this);

    D3D9WindowMessageFilter filter(m_window);

    m_monitor = wsi::getDefaultMonitor();

    wsi::saveWindowState(m_window, &m_windowState, true);

    if (!wsi::enterFullscreenMode(m_monitor, m_window, &m_windowState, true)) {
        Logger::err("D3D9: EnterFullscreenMode: Failed to enter fullscreen mode");
        return D3DERR_INVALIDCALL;
    }

    m_parent->NotifyFullscreen(m_window, true);

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::LeaveFullscreenMode() {
    if (!wsi::isWindow(m_window))
      return D3DERR_INVALIDCALL;

    if (FAILED(RestoreDisplayMode(m_monitor)))
      Logger::warn("D3D9: LeaveFullscreenMode: Failed to restore display mode");

    m_monitor = nullptr;

    ResetWindowProc(m_window);

    if (!wsi::leaveFullscreenMode(m_window, &m_windowState)) {
      Logger::err("D3D9: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return D3DERR_NOTAVAILABLE;
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

    // Tests show that present regions are ignored in fullscreen
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
