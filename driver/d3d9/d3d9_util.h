#pragma once

#define D3D11_NO_HELPERS

#include "d3d9_include.h"
#include "d3d9_caps.h"

#include "d3d9_format.h"

#include "../mtl/mtl_types.h"

#include "../util/util_matrix.h"
#include "../util/util_misc.h"

#include <d3dcommon.h>

namespace dxvk {

  struct D3D9ShaderMasks {
    uint32_t samplerMask;
    uint32_t rtMask;
  };

  struct D3D9BlendState {
    D3DBLEND   Src;
    D3DBLEND   Dst;
    D3DBLENDOP Op;
  };

  inline void FixupBlendState(D3D9BlendState& State) {
    // BOTHSRCALPHA/BOTHINVSRCALPHA are a legacy DX6 blend mode with no direct
    // Metal equivalent; remap to the SRC/INV pair they were meant to emulate.
    if (unlikely(State.Src == D3DBLEND_BOTHSRCALPHA)) {
      State.Src = D3DBLEND_SRCALPHA;
      State.Dst = D3DBLEND_INVSRCALPHA;
    }
    else if (unlikely(State.Src == D3DBLEND_BOTHINVSRCALPHA)) {
      State.Src = D3DBLEND_INVSRCALPHA;
      State.Dst = D3DBLEND_SRCALPHA;
    }
  }

  inline bool InvalidSampler(DWORD Sampler) {
    if (Sampler >= caps::MaxTexturesPS && Sampler < D3DDMAPSAMPLER)
      return true;

    if (Sampler > D3DVERTEXTEXTURESAMPLER3)
      return true;

    return false;
  }

  constexpr uint32_t FirstVSSamplerSlot = caps::MaxTexturesPS + 1;

  inline DWORD RemapSamplerState(DWORD Sampler) {
    if (Sampler >= D3DDMAPSAMPLER)
      Sampler = caps::MaxTexturesPS + (Sampler - D3DDMAPSAMPLER);

    return Sampler;
  }


  constexpr bool IsVSSampler(uint32_t Sampler) {
    return Sampler >= FirstVSSamplerSlot;
  }

  constexpr bool IsPSSampler(uint32_t Sampler) {
    return Sampler <= caps::MaxTexturesPS;
  }


  template <typename T, typename J>
  void CastRefPrivate(J* ptr, bool AddRef) {
    if (ptr == nullptr)
      return;

    T* castedPtr = reinterpret_cast<T*>(ptr);
    AddRef ? castedPtr->AddRefPrivate() : castedPtr->ReleasePrivate();
  }

  HRESULT DisassembleShader(
    const void*      pShader,
          BOOL       EnableColorCode,
          char*      pComments,
          ID3DBlob** ppDisassembly);

  HRESULT DecodeMultiSampleType(
          D3DMULTISAMPLE_TYPE       MultiSample,
          DWORD                     MultisampleQuality,
          uint32_t*                 pSampleCount);

  MtlFormat GetPackedDepthStencilFormat(D3D9Format Format);

  MtlTextureUsage GetImageFormatFeatures(DWORD Usage);

  MtlTextureUsage GetImageUsageFlags(DWORD Usage);

  inline MtlFormat PickSRGB(MtlFormat format, MtlFormat srgbFormat, bool srgb) {
    return srgb ? srgbFormat : format;
  }

  constexpr MtlStageMask GetShaderStage(D3D9ShaderType ShaderType) {
    switch (ShaderType) {
      case D3D9ShaderType::VertexShader: return MtlStageVertex;
      case D3D9ShaderType::PixelShader:  return MtlStageFragment;
      default:                           return MtlStageMask(0);
    }
  }

  inline uint32_t GetTransformIndex(D3DTRANSFORMSTATETYPE Type) {
    if (Type == D3DTS_VIEW)
      return 0;

    if (Type == D3DTS_PROJECTION)
      return 1;

    if (Type >= D3DTS_TEXTURE0 && Type <= D3DTS_TEXTURE7)
      return 2 + (Type - D3DTS_TEXTURE0);

    return 10 + (Type - D3DTS_WORLD);
  }

  inline Matrix4 ConvertMatrix(const D3DMATRIX* Matrix) {
    if (Matrix == nullptr)
      return Matrix4();

    return Matrix4(Matrix->m);
  }

  uint32_t GetVertexCount(D3DPRIMITIVETYPE type, UINT count);

  MtlInputAssemblyState DecodeInputAssemblyState(D3DPRIMITIVETYPE type);

  MtlBlendFactor DecodeBlendFactor(D3DBLEND BlendFactor, bool IsAlpha);

  MtlBlendOperation DecodeBlendOp(D3DBLENDOP BlendOp);

  inline MtlSamplerFilter DecodeFilter(D3DTEXTUREFILTERTYPE Filter) {
    return Filter > D3DTEXF_POINT ? MtlSamplerFilter::Linear : MtlSamplerFilter::Nearest;
  }

  inline MtlSamplerMipFilter DecodeMipFilter(D3DTEXTUREFILTERTYPE Filter) {
    return Filter > D3DTEXF_POINT ? MtlSamplerMipFilter::Linear : MtlSamplerMipFilter::Nearest;
  }

  inline MtlSamplerAddressMode DecodeAddressMode(D3DTEXTUREADDRESS Mode) {
    switch (Mode) {
      case D3DTADDRESS_WRAP:       return MtlSamplerAddressMode::Repeat;
      case D3DTADDRESS_MIRROR:     return MtlSamplerAddressMode::MirrorRepeat;
      case D3DTADDRESS_CLAMP:      return MtlSamplerAddressMode::ClampToEdge;
      case D3DTADDRESS_BORDER:     return MtlSamplerAddressMode::ClampToBorderColor;
      case D3DTADDRESS_MIRRORONCE: return MtlSamplerAddressMode::MirrorClampToEdge;
      default:                     return MtlSamplerAddressMode::Repeat;
    }
  }

  MtlCompareFunction DecodeCompareOp(D3DCMPFUNC Func);

  MtlStencilOperation DecodeStencilOp(D3DSTENCILOP Op);

  MtlCullMode DecodeCullMode(D3DCULL Mode);

  MtlTriangleFillMode DecodeFillMode(D3DFILLMODE Mode);

  MtlIndexType DecodeIndexType(D3D9Format Format);

  MtlFormat DecodeDecltype(D3DDECLTYPE Type);

  uint32_t GetDecltypeSize(D3DDECLTYPE Type);

  uint32_t GetDecltypeCount(D3DDECLTYPE Type);

  void ConvertBox(D3DBOX box, MtlOrigin& offset, MtlSize& extent);

  void ConvertRect(RECT rect, MtlOrigin& offset, MtlSize& extent);

  void ConvertRect(RECT rect, MtlOffset2D& offset, MtlExtent2D& extent);

  inline float GetDepthBufferRValue(MtlFormat Format, int32_t vendorId, bool exact, bool forceUnorm) {
    switch (Format) {
      case MtlFormat::Depth16Unorm:
        return (vendorId == 0x10de && !exact) ? float(1 << 15) : float(1 << 16);

      case MtlFormat::Depth24Unorm_Stencil8:
        return (vendorId == 0x10de && !exact) ? float(1 << 23) : float(1 << 24);

      default:
      case MtlFormat::Depth32Float_Stencil8:
      case MtlFormat::Depth32Float:
        return forceUnorm ? float(1 << 24) : float(1 << 23);
    }
  }

  template<typename T>
  UINT CompactSparseList(T* pData, UINT Mask) {
    uint32_t count = 0;

    for (uint32_t id : bit::BitMask(Mask))
      pData[count++] = pData[id];

    return count;
  }

  inline bool IsPoolManaged(D3DPOOL Pool) {
    return Pool == D3DPOOL_MANAGED || Pool == D3DPOOL_MANAGED_EX;
  }

  inline D3DRENDERSTATETYPE ColorWriteIndex(uint32_t i) {
    return D3DRENDERSTATETYPE(i ? D3DRENDERSTATETYPE(D3DRS_COLORWRITEENABLE1 + i - 1) : D3DRS_COLORWRITEENABLE);
  }

  inline bool AreFormatsSimilar(D3D9Format srcFormat, D3D9Format dstFormat) {
    return (srcFormat == dstFormat)
        || (srcFormat == D3D9Format::A8B8G8R8 && dstFormat == D3D9Format::X8B8G8R8)
        || (srcFormat == D3D9Format::A8R8G8B8 && dstFormat == D3D9Format::X8R8G8B8)
        || (srcFormat == D3D9Format::A1R5G5B5 && dstFormat == D3D9Format::X1R5G5B5)
        || (srcFormat == D3D9Format::A4R4G4B4 && dstFormat == D3D9Format::X4R4G4B4);
  }

  inline bool IsBlitRegionInvalid(MtlOffset3D offsets[2], MtlSize extent) {
    return offsets[1].x < offsets[0].x ||
           offsets[1].y < offsets[0].y ||
           offsets[0].x < 0 ||
           offsets[0].y < 0 ||
           uint32_t(offsets[1].x) > extent.width ||
           uint32_t(offsets[1].y) > extent.height;
  }

  enum D3D9TextureStageStateTypes : uint32_t
  {
      DXVK_TSS_COLOROP        =  0,
      DXVK_TSS_COLORARG1      =  1,
      DXVK_TSS_COLORARG2      =  2,
      DXVK_TSS_ALPHAOP        =  3,
      DXVK_TSS_ALPHAARG1      =  4,
      DXVK_TSS_ALPHAARG2      =  5,
      DXVK_TSS_BUMPENVMAT00   =  6,
      DXVK_TSS_BUMPENVMAT01   =  7,
      DXVK_TSS_BUMPENVMAT10   =  8,
      DXVK_TSS_BUMPENVMAT11   =  9,
      DXVK_TSS_TEXCOORDINDEX  = 10,
      DXVK_TSS_BUMPENVLSCALE  = 21,
      DXVK_TSS_BUMPENVLOFFSET = 22,
      DXVK_TSS_TEXTURETRANSFORMFLAGS = 23,
      DXVK_TSS_COLORARG0      = 25,
      DXVK_TSS_ALPHAARG0      = 26,
      DXVK_TSS_RESULTARG      = 27,
      DXVK_TSS_CONSTANT       = 31,
      DXVK_TSS_COUNT          = 32
  };

  constexpr uint32_t DXVK_TSS_TCI_PASSTHRU                      = 0x00000000;
  constexpr uint32_t DXVK_TSS_TCI_CAMERASPACENORMAL             = 0x00010000;
  constexpr uint32_t DXVK_TSS_TCI_CAMERASPACEPOSITION           = 0x00020000;
  constexpr uint32_t DXVK_TSS_TCI_CAMERASPACEREFLECTIONVECTOR   = 0x00030000;
  constexpr uint32_t DXVK_TSS_TCI_SPHEREMAP                     = 0x00040000;

  inline D3D9TextureStageStateTypes RemapTextureStageStateType(D3DTEXTURESTAGESTATETYPE Type) {
    return D3D9TextureStageStateTypes(Type - 1);
  }

}


inline bool operator == (const D3DVIEWPORT9& a, const D3DVIEWPORT9& b) {
  return a.X      == b.X      &&
         a.Y      == b.Y      &&
         a.Width  == b.Width  &&
         a.Height == b.Height &&
         a.MinZ   == b.MinZ   &&
         a.MaxZ   == b.MaxZ;
}

inline bool operator != (const D3DVIEWPORT9& a, const D3DVIEWPORT9& b) {
  return !(a == b);
}


#ifndef _MSC_VER
inline bool operator == (const RECT& a, const RECT& b) {
  return a.left   == b.left  &&
         a.right  == b.right &&
         a.top    == b.top   &&
         a.bottom == b.bottom;
}

inline bool operator != (const RECT& a, const RECT& b) {
  return !(a == b);
}
#endif

inline bool operator == (const POINT& a, const POINT& b) {
  return a.x == b.x && a.y == b.y;
}

inline bool operator != (const POINT& a, const POINT& b) {
  return !(a == b);
}

inline bool operator == (const D3DDISPLAYMODEEX& a, const D3DDISPLAYMODEEX& b) {
  return a.Size             == b.Size             &&
         a.Width            == b.Width            &&
         a.Height           == b.Height           &&
         a.RefreshRate      == b.RefreshRate      &&
         a.Format           == b.Format           &&
         a.ScanLineOrdering == b.ScanLineOrdering;
}
