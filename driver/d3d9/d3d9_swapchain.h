#pragma once

#include "d3d9_device_child.h"
#include "d3d9_device.h"
#include "d3d9_format.h"

#include "../mtl/mtl_presenter.h"

#include "../util/sync/sync_signal.h"
#include "../util/util_fps_limiter.h"

#include "../wsi/wsi_window.h"
#include "../wsi/wsi_monitor.h"

#include <vector>

namespace dxvk {

  class D3D9Surface;
  class D3D9SwapChainEx;

  struct D3D9WindowContext {
    Rc<MtlPresenter>               presenter;

    uint64_t                       frameId = D3D9DeviceEx::MaxFrameLatency;
    Rc<sync::Fence>                frameLatencySignal;

    uint32_t                       deviceResetCounter = 0u;
  };

  using D3D9SwapChainExBase = D3D9DeviceChild<IDirect3DSwapChain9Ex>;
  class D3D9SwapChainEx final : public D3D9SwapChainExBase {
    static constexpr uint32_t NumControlPoints = 256;

  public:

    D3D9SwapChainEx(
            D3D9DeviceEx*          pDevice,
            D3DPRESENT_PARAMETERS* pPresentParams,
      const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
            bool                   EnableLatencyTracking);

    ~D3D9SwapChainEx();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);

    HRESULT STDMETHODCALLTYPE Present(
      const RECT*    pSourceRect,
      const RECT*    pDestRect,
            HWND     hDestWindowOverride,
      const RGNDATA* pDirtyRegion,
            DWORD    dwFlags);

#ifdef _WIN32
    HRESULT PresentImageGDI(HWND Window);
#endif

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* pDestSurface);

    HRESULT STDMETHODCALLTYPE GetBackBuffer(
            UINT                iBackBuffer,
            D3DBACKBUFFER_TYPE  Type,
            IDirect3DSurface9** ppBackBuffer);

    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* pRasterStatus);

    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* pMode);

    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters);

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount);

    HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics);

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation);

    HRESULT Reset(
            D3DPRESENT_PARAMETERS* pPresentParams,
            D3DDISPLAYMODEEX*      pFullscreenDisplayMode);

    HRESULT WaitForVBlank();

    void    SetGammaRamp(
            DWORD         Flags,
      const D3DGAMMARAMP* pRamp);

    void    GetGammaRamp(D3DGAMMARAMP* pRamp);

    void    Invalidate(HWND hWindow);

    void SetCursorTexture(UINT Width, UINT Height, uint8_t* pCursorBitmap);

    void SetCursorPosition(int32_t X, int32_t Y, UINT Width, UINT Height);

    HRESULT SetDialogBoxMode(bool bEnableDialogs);

    D3D9Surface* GetBackBuffer(UINT iBackBuffer);

    const D3DPRESENT_PARAMETERS* GetPresentParams() const { return &m_presentParams; }

    void SyncFrameLatency();

    void DestroyBackBuffers();

    bool UpdateWindowCtx();

  private:

    enum BindingIds : uint32_t {
      Image = 0,
      Gamma = 1,
    };

    D3DPRESENT_PARAMETERS     m_presentParams;
    D3DGAMMARAMP              m_ramp;

    Rc<MtlDevice>             m_device;

    std::unordered_map<
      HWND,
      D3D9WindowContext>      m_presenters;

    D3D9WindowContext*        m_wctx = nullptr;

    std::vector<Com<D3D9Surface, false>> m_backBuffers;

    Com<D3D9Surface, false>   m_resolveBuffer;

    RECT                      m_srcRect;
    RECT                      m_dstRect;
    MtlExtent2D               m_swapchainExtent = { 0u, 0u };
    bool                      m_partialCopy = false;

    uint32_t                  m_frameLatencyCap = 0;

    HWND                      m_window   = nullptr;
    HMONITOR                  m_monitor  = nullptr;

    wsi::DxvkWindowState      m_windowState;

    HWND                      m_fullscreenWindow   = nullptr;
    LONG                      m_savedWindowStyle   = 0;
    LONG                      m_savedWindowExStyle = 0;

    double                    m_targetFrameRate = 0.0;

    // Used instead of presentAfterMinimumDuration: Apple documents that API as
    // holding drawables longer than necessary, which is fatal here since a held
    // drawable never retires while the layer is uncomposited.
    FpsLimiter                m_fpsLimiter;

    double                    m_displayRefreshRate = 0.0;
    bool                      m_displayRefreshRateDirty = true;

    bool                      m_warnedAboutGDIFallback = false;

    bool                      m_latencyTracking = false;
    Rc<MtlLatencyTracker>     m_latencyTracker = nullptr;

    void PresentImage(UINT PresentInterval);

    Rc<MtlPresenter> CreatePresenter(
            HWND                Window,
            Rc<sync::Signal>    Signal);

    HRESULT CreateBackBuffers(
            uint32_t            NumBackBuffers,
            DWORD               Flags);

    void DestroyLatencyTracker();

    void InitRamp();

    void UpdateTargetFrameRate(uint32_t SyncInterval);

    double GetPresentMinimumDuration(uint32_t SyncInterval) const;

    uint32_t GetActualFrameLatency();

    MtlFormat GetSurfaceFormat();

    void NormalizePresentParameters(D3DPRESENT_PARAMETERS* pPresentParams);

    void UpdateWindowedRefreshRate();

    HRESULT EnterFullscreenMode(
            D3DPRESENT_PARAMETERS*  pPresentParams,
      const D3DDISPLAYMODEEX*       pFullscreenDisplayMode);

    HRESULT LeaveFullscreenMode();

    HRESULT ChangeDisplayMode(
            D3DPRESENT_PARAMETERS*  pPresentParams,
      const D3DDISPLAYMODEEX*       pFullscreenDisplayMode);

    HRESULT RestoreDisplayMode(HMONITOR hMonitor);

    void UpdatePresentRegion(const RECT* pSourceRect, const RECT* pDestRect);

    void UpdatePresentParameters();

    MtlExtent2D GetPresentExtent();

    std::string GetApiName();

    bool IsDeviceReset(D3D9WindowContext* wctx);

    const Com<D3D9Surface, false>& GetFrontBuffer() const {
      return m_backBuffers.back();
    }

    bool SwapWithFrontBuffer() const {
      if (m_presentParams.SwapEffect == D3DSWAPEFFECT_COPY)
        return false;

      // Empirically, DISCARD with 1 backbuffer in windowed mode behaves like
      // COPY: blit to the front buffer again instead of swapping buffers.
      if (m_presentParams.SwapEffect == D3DSWAPEFFECT_DISCARD && m_presentParams.BackBufferCount == 1 && m_presentParams.Windowed)
        return false;

      return true;
    }
  };

}
