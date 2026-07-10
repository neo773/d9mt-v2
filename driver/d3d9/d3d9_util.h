#pragma once

#define D3D11_NO_HELPERS

#include "d3d9_include.h"
#include "d3d9_caps.h"

#include "d3d9_format.h"

// ADAPT (doc 02 §2, line 6): the Decode{BlendFactor,BlendOp,Filter,MipFilter,
// AddressMode,CompareOp,StencilOp,CullMode,FillMode,IndexType,Decltype} decoders
// are a 1:1 Vk*->Mtl* retype — switch bodies unchanged, only the return type flips
// to the Mtl* enums. The former `#include "../dxvk/dxvk_device.h"` (which dragged in
// DxvkInputAssemblyState + <vulkan/vulkan.h>) is severed; the Mtl* value types come
// from mtl_types.h (pulled transitively via d3d9_format.h).
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
    // Old DirectX 6 HW feature that still exists...
    // Yuck!
    if (unlikely(State.Src == D3DBLEND_BOTHSRCALPHA)) {
      State.Src = D3DBLEND_SRCALPHA;
      State.Dst = D3DBLEND_INVSRCALPHA;
    }
    else if (unlikely(State.Src == D3DBLEND_BOTHINVSRCALPHA)) {
      State.Src = D3DBLEND_INVSRCALPHA;
      State.Dst = D3DBLEND_SRCALPHA;
    }
  }

  /**
   * @brief Returns whether or not the sampler index is valid
   *
   * @param Sampler Sampler index (according to the API)
   */
  inline bool InvalidSampler(DWORD Sampler) {
    if (Sampler >= caps::MaxTexturesPS && Sampler < D3DDMAPSAMPLER)
      return true;

    if (Sampler > D3DVERTEXTEXTURESAMPLER3)
      return true;

    return false;
  }

  /**
   * @brief The first sampler that belongs to the vertex shader according to our internal way of storing samplers
   */
  constexpr uint32_t FirstVSSamplerSlot = caps::MaxTexturesPS + 1;

  /**
   * @brief Remaps a sampler index by the API to an internal one
   *
   * Remaps the sampler index according to the way the API counts them to how we count and store them internally.
   *
   * @param Sampler Sampler index (according to API)
   * @return DWORD Sampler index (according to our internal way of storing samplers)
   */
  inline DWORD RemapSamplerState(DWORD Sampler) {
    if (Sampler >= D3DDMAPSAMPLER)
      Sampler = caps::MaxTexturesPS + (Sampler - D3DDMAPSAMPLER);

    return Sampler;
  }


  /**
   * @brief Returns whether the sampler belongs to the vertex shader.
   *
   * The displacement map sampler is part of a fixed function feature,
   * so it does not belong to the vertex shader.
   * @param Sampler Sampler index (according to our internal way of storing samplers)
   */
  constexpr bool IsVSSampler(uint32_t Sampler) {
    return Sampler >= FirstVSSamplerSlot;
  }

  /**
   * @brief Returns whether the sampler belongs to the pixel shader.
   *
   * The displacement map sampler is part of a fixed function feature,
   * so it does not belong to the pixel shader.
   * @param Sampler Sampler index (according to our internal way of storing samplers)
   */
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

  // Was VkSampleCountFlagBits* -> plain sample-count (uint32_t) out param.
  HRESULT DecodeMultiSampleType(
          D3DMULTISAMPLE_TYPE       MultiSample,
          DWORD                     MultisampleQuality,
          uint32_t*                 pSampleCount);

  MtlFormat GetPackedDepthStencilFormat(D3D9Format Format);

  // Was VkFormatFeatureFlags2 -> a Metal texture-usage mask (Metal does not split
  // "format features" from usage; the bits we care about are sampled/RT/DS).
  MtlTextureUsage GetImageFormatFeatures(DWORD Usage);

  MtlTextureUsage GetImageUsageFlags(DWORD Usage);

  inline MtlFormat PickSRGB(MtlFormat format, MtlFormat srgbFormat, bool srgb) {
    return srgb ? srgbFormat : format;
  }

  // Was VkShaderStageFlagBits -> MtlStageMask (0 == invalid, mirrors the old
  // VkShaderStageFlagBits(0) default).
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
    if (Matrix == nullptr) // Identity.
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

  // Retyped from the Vk-value LUT trick to an explicit switch: MtlSamplerAddressMode
  // numeric values differ from Vk (Repeat != 0), so the "free repeat for value 0"
  // shortcut no longer holds. Behaviour is identical.
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

  // Was VkFormat (vertex-attribute format). MtlFormat is a *pixel*-format enum and
  // cannot represent the D3D9 scaled vertex types (UBYTE4/SHORT2/SHORT4/UDEC3);
  // those rows return MtlFormat::Invalid with a PHASE-C note (a dedicated
  // MtlVertexFormat enum is the proper fix — same precedent as the 9 Invalid rows in
  // d3d9_format.cpp).
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
    // Only bother checking x, y as we don't have 3D blits.
    return offsets[1].x < offsets[0].x ||
           offsets[1].y < offsets[0].y ||
           offsets[0].x < 0 ||
           offsets[0].y < 0 ||
           uint32_t(offsets[1].x) > extent.width ||
           uint32_t(offsets[1].y) > extent.height;
  }

  /**
   * @brief Mirrors D3DTEXTURESTAGESTATETYPE but starts at 0
   */
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

  /**
   * @brief Remaps a texture stage type by the API to an internal one
   *
   * @param Type Texture stage type according to the API
   * @return D3D9TextureStageStateTypes Texture stage type according to our internal way of storing them
   */
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


// Missing in some versions of mingw headers
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
#endif /* _MSC_VER */

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
