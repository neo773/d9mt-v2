#pragma once

#include <windows.h>
#include "../util/config/config.h"


MIDL_INTERFACE("D3D9D3D8-42A9-4C1E-AA97-BEEFCAFE2000")
IDxvkD3D8Bridge : public IUnknown {

  #ifdef DXVK_D3D9_NAMESPACE
    using IDirect3DSurface9 = d3d9::IDirect3DSurface9;
    using D3DFORMAT = d3d9::D3DFORMAT;
  #endif

  virtual HRESULT UpdateTextureFromBuffer(
      IDirect3DSurface9*        pDestSurface,
      IDirect3DSurface9*        pSrcSurface,
      const RECT*               pSrcRect,
      const POINT*              pDestPoint) = 0;

  virtual bool IsSupportedSurfaceFormat(D3DFORMAT Format) = 0;
};

MIDL_INTERFACE("D3D9D3D8-A407-773E-18E9-CAFEBEEF3000")
IDxvkD3D8InterfaceBridge : public IUnknown {
  virtual void EnableD3D8CompatibilityMode() = 0;

  virtual const dxvk::Config* GetConfig() const = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(IDxvkD3D8Bridge, 0xD3D9D3D8, 0x42A9, 0x4C1E, 0xAA, 0x97, 0xBE, 0xEF, 0xCA, 0xFE, 0x20, 0x00);
__CRT_UUID_DECL(IDxvkD3D8InterfaceBridge, 0xD3D9D3D8, 0xA407, 0x773E, 0x18, 0xE9, 0xCA, 0xFE, 0xBE, 0xEF, 0x30, 0x00);
#endif

namespace dxvk {

  class D3D9DeviceEx;
  class D3D9InterfaceEx;

  class DxvkD3D8Bridge : public IDxvkD3D8Bridge {

  public:

    DxvkD3D8Bridge(D3D9DeviceEx* pDevice);

    ~DxvkD3D8Bridge();

    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void** ppvObject);

    HRESULT UpdateTextureFromBuffer(
        IDirect3DSurface9*        pDestSurface,
        IDirect3DSurface9*        pSrcSurface,
        const RECT*               pSrcRect,
        const POINT*              pDestPoint);

    bool IsSupportedSurfaceFormat(D3DFORMAT Format);

  private:

    D3D9DeviceEx* m_device;

  };

  class DxvkD3D8InterfaceBridge : public IDxvkD3D8InterfaceBridge {

  public:

    DxvkD3D8InterfaceBridge(D3D9InterfaceEx* pObject);

    ~DxvkD3D8InterfaceBridge();

    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void** ppvObject);

    void EnableD3D8CompatibilityMode();

    const Config* GetConfig() const;

  protected:

    D3D9InterfaceEx* m_interface;

  };

}
