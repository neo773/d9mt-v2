#include "d3d9_interface.h"

#include "d3d9_monitor.h"
#include "d3d9_caps.h"
#include "d3d9_device.h"
#include "d3d9_bridge.h"
#include "d3d9_window.h"

#include "../util/util_singleton.h"

#include <algorithm>

namespace dxvk {

  Singleton<MtlInstance> g_dxvkInstance;

  D3D9InterfaceEx::D3D9InterfaceEx(bool bExtended, const D3D9ON12_ARGS* pOverrideList, uint32_t OverrideCount)
    : m_instance    ( g_dxvkInstance.acquire() )
    , m_d3d8Bridge  ( this )
    , m_extended    ( bExtended )
    , m_d3d9Options ( nullptr, m_instance->config() ) {

#ifdef _WIN32
    if (m_d3d9Options.enumerateByDisplays) {
      DISPLAY_DEVICEA device = { };
      device.cb = sizeof(device);

      uint32_t adapterOrdinal = 0;
      uint32_t i = 0;
      while (::EnumDisplayDevicesA(nullptr, i++, &device, 0)) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
          continue;

        if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
          continue;

        Rc<MtlAdapter> adapter = adapterOrdinal >= m_instance->adapterCount()
          ? m_instance->enumAdapters(0)
          : m_instance->enumAdapters(adapterOrdinal);

        if (adapter != nullptr) {
          const auto* d3d9On12Args = Find9On12Args(adapter, pOverrideList, OverrideCount);
          m_adapters.emplace_back(new D3D9Adapter(this, d3d9On12Args, m_instance, adapter, adapterOrdinal++, i - 1));
        }
      }
    }
    else
#endif
    {
      const uint32_t adapterCount = m_instance->adapterCount();
      m_adapters.reserve(adapterCount);

      for (uint32_t i = 0; i < adapterCount; i++) {
        const auto* d3d9On12Args = Find9On12Args(m_instance->enumAdapters(i), pOverrideList, OverrideCount);
        m_adapters.emplace_back(new D3D9Adapter(this, d3d9On12Args, m_instance, m_instance->enumAdapters(i), i, 0));
      }
    }

#ifdef _WIN32
    if (m_d3d9Options.dpiAware) {
      Logger::info("Process set as DPI aware");
      SetProcessDPIAware();
    }
#endif

    if (unlikely(m_d3d9Options.shaderModel == 0))
      Logger::warn("D3D9InterfaceEx: WARNING! Fixed-function exclusive mode is enabled.");

    if (d9mtTrace())
      Logger::info(str::format("ITRACE D3D9InterfaceEx CTOR this=", this, " ext=", m_extended));
  }


  D3D9InterfaceEx::~D3D9InterfaceEx() {
    if (d9mtTrace())
      Logger::info(str::format("ITRACE D3D9InterfaceEx DTOR this=", this));
    g_dxvkInstance.release();
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3D9)
     || (m_extended && riid == __uuidof(IDirect3D9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDxvkD3D8InterfaceBridge)) {
      *ppvObject = ref(&m_d3d8Bridge);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDirect3D9), riid)) {
      Logger::warn("D3D9InterfaceEx::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::RegisterSoftwareDevice(void* pInitializeFunction) {
    Logger::warn("D3D9InterfaceEx::RegisterSoftwareDevice: Stub");
    return D3D_OK;
  }


  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterCount() {
    return UINT(m_adapters.size());
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterIdentifier(
          UINT                    Adapter,
          DWORD                   Flags,
          D3DADAPTER_IDENTIFIER9* pIdentifier) {
    if (d9mtTrace()) Logger::info(str::format("ITRACE GetAdapterIdentifier a=", Adapter));
    if (auto* adapter = GetAdapter(Adapter)) {
      HRESULT hr = adapter->GetAdapterIdentifier(Flags, pIdentifier);
      if (d9mtTrace() && SUCCEEDED(hr))
        Logger::info(str::format("ITRACE   id Driver='", pIdentifier->Driver,
          "' Desc='", pIdentifier->Description, "' Vendor=", pIdentifier->VendorId,
          " Device=", pIdentifier->DeviceId));
      return hr;
    }
    return D3DERR_INVALIDCALL;
  }


  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
    D3DDISPLAYMODEFILTER filter;
    filter.Size             = sizeof(D3DDISPLAYMODEFILTER);
    filter.Format           = Format;
    filter.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

    return this->GetAdapterModeCountEx(Adapter, &filter);
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
    if (d9mtTrace()) Logger::info(str::format("ITRACE GetAdapterDisplayMode a=", Adapter));
    if (auto* adapter = GetAdapter(Adapter)) {
      D3DDISPLAYMODEEX modeEx = { };
      modeEx.Size = sizeof(D3DDISPLAYMODEEX);
      HRESULT hr = adapter->GetAdapterDisplayModeEx(&modeEx, nullptr);

      if (FAILED(hr))
        return hr;

      pMode->Width       = modeEx.Width;
      pMode->Height      = modeEx.Height;
      pMode->RefreshRate = modeEx.RefreshRate;
      pMode->Format      = modeEx.Format;

      return D3D_OK;
    }

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceType(
          UINT       Adapter,
          D3DDEVTYPE DevType,
          D3DFORMAT  AdapterFormat,
          D3DFORMAT  BackBufferFormat,
          BOOL       bWindowed) {
    if (auto* adapter = GetAdapter(Adapter)) {
      HRESULT hr = adapter->CheckDeviceType(
        DevType, EnumerateFormat(AdapterFormat),
        EnumerateFormat(BackBufferFormat), bWindowed);
      if (d9mtTrace()) Logger::info(str::format("ITRACE CheckDeviceType a=", Adapter, " dev=", DevType, " af=", AdapterFormat, " bb=", BackBufferFormat, " win=", bWindowed, " -> 0x", std::hex, uint32_t(hr), std::dec));
      return hr;
    }

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceFormat(
          UINT            Adapter,
          D3DDEVTYPE      DeviceType,
          D3DFORMAT       AdapterFormat,
          DWORD           Usage,
          D3DRESOURCETYPE RType,
          D3DFORMAT       CheckFormat) {
    if (auto* adapter = GetAdapter(Adapter)) {
      HRESULT hr = adapter->CheckDeviceFormat(
        DeviceType, EnumerateFormat(AdapterFormat),
        Usage, RType, EnumerateFormat(CheckFormat));
      if (d9mtTrace()) Logger::info(str::format("ITRACE CheckDeviceFormat fmt=", uint32_t(CheckFormat),
        " usage=", Usage, " rtype=", uint32_t(RType), " -> 0x", std::hex, uint32_t(hr), std::dec));
      return hr;
    }
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceMultiSampleType(
          UINT                Adapter,
          D3DDEVTYPE          DeviceType,
          D3DFORMAT           SurfaceFormat,
          BOOL                Windowed,
          D3DMULTISAMPLE_TYPE MultiSampleType,
          DWORD*              pQualityLevels) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceMultiSampleType(
        DeviceType, EnumerateFormat(SurfaceFormat),
        Windowed, MultiSampleType,
        pQualityLevels);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDepthStencilMatch(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DFORMAT  AdapterFormat,
          D3DFORMAT  RenderTargetFormat,
          D3DFORMAT  DepthStencilFormat) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDepthStencilMatch(
        DeviceType, EnumerateFormat(AdapterFormat),
        EnumerateFormat(RenderTargetFormat),
        EnumerateFormat(DepthStencilFormat));

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CheckDeviceFormatConversion(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DFORMAT  SourceFormat,
          D3DFORMAT  TargetFormat) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->CheckDeviceFormatConversion(
        DeviceType, EnumerateFormat(SourceFormat),
        EnumerateFormat(TargetFormat));

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetDeviceCaps(
          UINT       Adapter,
          D3DDEVTYPE DeviceType,
          D3DCAPS9*  pCaps) {
    if (d9mtTrace()) Logger::info(str::format("ITRACE GetDeviceCaps a=", Adapter, " type=", DeviceType));
    if (auto* adapter = GetAdapter(Adapter)) {
      HRESULT hr = adapter->GetDeviceCaps(DeviceType, pCaps);
      if (d9mtTrace() && SUCCEEDED(hr) && pCaps)
        Logger::info(str::format("ITRACE   caps PS=", pCaps->PixelShaderVersion,
          " VS=", pCaps->VertexShaderVersion, " MaxTexW=", pCaps->MaxTextureWidth,
          " Caps2=", pCaps->Caps2, " DevCaps=", pCaps->DevCaps));
      return hr;
    }
    return D3DERR_INVALIDCALL;
  }


  HMONITOR STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterMonitor(UINT Adapter) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetMonitor();

    return nullptr;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDevice(
          UINT                   Adapter,
          D3DDEVTYPE             DeviceType,
          HWND                   hFocusWindow,
          DWORD                  BehaviorFlags,
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          IDirect3DDevice9**     ppReturnedDeviceInterface) {
    return this->CreateDeviceEx(
      Adapter,
      DeviceType,
      hFocusWindow,
      BehaviorFlags,
      pPresentationParameters,
      nullptr,
      reinterpret_cast<IDirect3DDevice9Ex**>(ppReturnedDeviceInterface));
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModes(
          UINT            Adapter,
          D3DFORMAT       Format,
          UINT            Mode,
          D3DDISPLAYMODE* pMode) {
    if (pMode == nullptr)
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEFILTER filter;
    filter.Format           = Format;
    filter.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    filter.Size             = sizeof(D3DDISPLAYMODEFILTER);

    D3DDISPLAYMODEEX modeEx = { };
    modeEx.Size = sizeof(D3DDISPLAYMODEEX);
    HRESULT hr = this->EnumAdapterModesEx(Adapter, &filter, Mode, &modeEx);

    if (d9mtTrace()) Logger::info(str::format("ITRACE EnumAdapterModes fmt=", uint32_t(Format),
      " mode=", Mode, " -> 0x", std::hex, uint32_t(hr), std::dec,
      SUCCEEDED(hr) ? str::format(" ", modeEx.Width, "x", modeEx.Height, "@", modeEx.RefreshRate) : ""));

    if (FAILED(hr))
      return hr;

    pMode->Width       = modeEx.Width;
    pMode->Height      = modeEx.Height;
    pMode->RefreshRate = modeEx.RefreshRate;
    pMode->Format      = modeEx.Format;

    return D3D_OK;
  }

  UINT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterModeCountEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER* pFilter) {
    if (auto* adapter = GetAdapter(Adapter)) {
      UINT n = adapter->GetAdapterModeCountEx(pFilter);
      if (d9mtTrace()) Logger::info(str::format("ITRACE GetAdapterModeCount a=", Adapter, " fmt=", pFilter ? pFilter->Format : 0, " -> ", n));
      return n;
    }

    return 0;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModesEx(
          UINT                  Adapter,
    const D3DDISPLAYMODEFILTER* pFilter,
          UINT                  Mode,
          D3DDISPLAYMODEEX*     pMode) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->EnumAdapterModesEx(pFilter, Mode, pMode);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterDisplayModeEx(
          UINT                Adapter,
          D3DDISPLAYMODEEX*   pMode,
          D3DDISPLAYROTATION* pRotation) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterDisplayModeEx(pMode, pRotation);

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDeviceEx(
          UINT                   Adapter,
          D3DDEVTYPE             DeviceType,
          HWND                   hFocusWindow,
          DWORD                  BehaviorFlags,
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          IDirect3DDevice9Ex**   ppReturnedDeviceInterface) {
    if (d9mtTrace()) Logger::info(str::format("ITRACE CreateDeviceEx a=", Adapter, " type=", DeviceType,
      " behavior=0x", std::hex, BehaviorFlags, std::dec));
    InitReturnPtr(ppReturnedDeviceInterface);

    if (unlikely(ppReturnedDeviceInterface  == nullptr
              || pPresentationParameters    == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(DeviceType == D3DDEVTYPE_SW))
      return D3DERR_INVALIDCALL;

    if (unlikely(BehaviorFlags & D3DCREATE_PUREDEVICE &&
               !(BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING)))
      return D3DERR_INVALIDCALL;

    // REF/NULLREF never support hardware vertex processing, so force software VP.
    if (unlikely(DeviceType == D3DDEVTYPE_REF || DeviceType == D3DDEVTYPE_NULLREF)) {
      BehaviorFlags &= ~D3DCREATE_MIXED_VERTEXPROCESSING
                     & ~D3DCREATE_PUREDEVICE
                     & ~D3DCREATE_HARDWARE_VERTEXPROCESSING;
      BehaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }

    HRESULT hr;
    // Some titles create a NULLREF device with invalid parameters and expect it to succeed.
    if (likely(DeviceType != D3DDEVTYPE_NULLREF)) {
      hr = ValidatePresentationParameters(pPresentationParameters);

      if (unlikely(FAILED(hr)))
        return hr;
    }

    auto* adapter = GetAdapter(Adapter);

    if (adapter == nullptr)
      return D3DERR_INVALIDCALL;

    auto dxvkAdapter = adapter->GetDXVKAdapter();

    try {
      auto dxvkDevice = dxvkAdapter->createDevice();

      auto* device = new D3D9DeviceEx(
        this,
        adapter,
        DeviceType,
        hFocusWindow,
        BehaviorFlags,
        dxvkDevice);

      if (!pPresentationParameters->Windowed)
        ActivateFocusWindow(hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow);

      hr = device->InitialReset(pPresentationParameters, pFullscreenDisplayMode);

      if (unlikely(FAILED(hr)))
        return hr;

      *ppReturnedDeviceInterface = ref(device);
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::GetAdapterLUID(UINT Adapter, LUID* pLUID) {
    if (auto* adapter = GetAdapter(Adapter))
      return adapter->GetAdapterLUID(pLUID);

    return D3DERR_INVALIDCALL;
  }


  HRESULT D3D9InterfaceEx::ValidatePresentationParametersEx(
    const D3DPRESENT_PARAMETERS* pPresentationParameters,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!pPresentationParameters->Windowed && pFullscreenDisplayMode == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(pPresentationParameters->Windowed && pFullscreenDisplayMode != nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!pPresentationParameters->Windowed &&
                  (pPresentationParameters->BackBufferWidth  != pFullscreenDisplayMode->Width
                || pPresentationParameters->BackBufferHeight != pFullscreenDisplayMode->Height)))
      return D3DERR_INVALIDCALL;

    return ValidatePresentationParameters(pPresentationParameters);
  }


  HRESULT D3D9InterfaceEx::ValidatePresentationParameters(
    const D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    if (m_extended) {
      if (unlikely(pPresentationParameters->SwapEffect > D3DSWAPEFFECT_FLIPEX))
        return D3DERR_INVALIDCALL;

      if (unlikely(pPresentationParameters->BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX_EX))
        return D3DERR_INVALIDCALL;
    } else {
      if (unlikely(pPresentationParameters->SwapEffect > D3DSWAPEFFECT_COPY))
        return D3DERR_INVALIDCALL;

      if (unlikely(pPresentationParameters->BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX))
        return D3DERR_INVALIDCALL;
    }

    if (unlikely(!pPresentationParameters->SwapEffect))
      return D3DERR_INVALIDCALL;

    if (unlikely(!IsD3D8Compatible()
              && pPresentationParameters->SwapEffect == D3DSWAPEFFECT_COPY
              && pPresentationParameters->BackBufferCount > 1))
      return D3DERR_INVALIDCALL;

    if (unlikely(!pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_TWO
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_THREE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_FOUR
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;

    if (unlikely(pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;

    return D3D_OK;
  }


  const D3D9ON12_ARGS* D3D9InterfaceEx::Find9On12Args(
    const Rc<MtlAdapter>& Adapter,
    const D3D9ON12_ARGS*   pOverrides,
          uint32_t         OverrideCount) {
    (void)Adapter;
    const D3D9ON12_ARGS* arg = nullptr;

    for (uint32_t i = 0u; i < OverrideCount; i++) {
      if (!arg)
        arg = &pOverrides[i];
    }

    return arg;
  }

}
