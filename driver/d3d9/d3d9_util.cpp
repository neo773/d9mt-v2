#include "d3d9_util.h"

#include "../util/util_win32_compat.h"

namespace dxvk {

  typedef HRESULT (STDMETHODCALLTYPE *D3DXDisassembleShader) (
    const void*      pShader,
          BOOL       EnableColorCode,
          char*      pComments,
          ID3DBlob** ppDisassembly);

  D3DXDisassembleShader g_pfnDisassembleShader = nullptr;

  HRESULT DisassembleShader(
    const void*      pShader,
          BOOL       EnableColorCode,
          char*      pComments,
          ID3DBlob** ppDisassembly) {
    if (g_pfnDisassembleShader == nullptr) {
      HMODULE d3d9x = LoadLibraryA("d3dx9.dll");

      if (d3d9x == nullptr)
        d3d9x = LoadLibraryA("d3dx9_43.dll");

      g_pfnDisassembleShader =
        reinterpret_cast<D3DXDisassembleShader>(GetProcAddress(d3d9x, "D3DXDisassembleShader"));
    }

    if (g_pfnDisassembleShader == nullptr)
      return D3DERR_INVALIDCALL;

    return g_pfnDisassembleShader(
      pShader,
      EnableColorCode,
      pComments,
      ppDisassembly);
  }


  HRESULT DecodeMultiSampleType(
          D3DMULTISAMPLE_TYPE       MultiSample,
          DWORD                     MultisampleQuality,
          uint32_t*                 pSampleCount) {
    uint32_t sampleCount = std::max<uint32_t>(MultiSample, 1u);

    if (MultiSample == D3DMULTISAMPLE_NONMASKABLE)
      sampleCount = 1u << MultisampleQuality;

    if (sampleCount > 16u)
      return D3DERR_INVALIDCALL;

    if (sampleCount & (sampleCount - 1))
      return D3DERR_NOTAVAILABLE;

    if (pSampleCount)
      *pSampleCount = sampleCount;

    return D3D_OK;
  }


  MtlFormat GetPackedDepthStencilFormat(D3D9Format Format) {
    switch (Format) {
    case D3D9Format::D15S1:
      return MtlFormat::Depth32Float_Stencil8;

    case D3D9Format::D16:
    case D3D9Format::D16_LOCKABLE:
    case D3D9Format::DF16:
      return MtlFormat::Depth16Unorm;

    case D3D9Format::D24X8:
    case D3D9Format::DF24:
      // Depth24Unorm_Stencil8 is Intel-only and asserts on texture creation on
      // Apple Silicon, where D24-class images are backed by Depth32Float instead;
      // the packed readback layout must match that actual image format.
      return MtlFormat::Depth32Float;

    case D3D9Format::D24X4S4:
    case D3D9Format::D24FS8:
    case D3D9Format::D24S8:
    case D3D9Format::INTZ:
      return MtlFormat::Depth32Float_Stencil8;

    case D3D9Format::D32:
    case D3D9Format::D32_LOCKABLE:
    case D3D9Format::D32F_LOCKABLE:
      return MtlFormat::Depth32Float;

    case D3D9Format::S8_LOCKABLE:
      return MtlFormat::Stencil8;

    default:
      return MtlFormat::Invalid;
    }
  }


  MtlTextureUsage GetImageFormatFeatures(DWORD Usage) {
    MtlTextureUsage features = MtlTextureUsageShaderRead;

    if (Usage & D3DUSAGE_DEPTHSTENCIL)
      features |= MtlTextureUsageRenderTarget;

    if (Usage & D3DUSAGE_RENDERTARGET)
      features |= MtlTextureUsageRenderTarget;

    return features;
  }


  MtlTextureUsage GetImageUsageFlags(DWORD Usage) {
    MtlTextureUsage usage = MtlTextureUsageShaderRead;

    if (Usage & D3DUSAGE_DEPTHSTENCIL)
      usage |= MtlTextureUsageRenderTarget;

    if (Usage & D3DUSAGE_RENDERTARGET)
      usage |= MtlTextureUsageRenderTarget;

    return usage;
  }


  uint32_t GetVertexCount(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
      default:
      case D3DPT_TRIANGLELIST:  return count * 3;
      case D3DPT_POINTLIST:     return count;
      case D3DPT_LINELIST:      return count * 2;
      case D3DPT_LINESTRIP:     return count + 1;
      case D3DPT_TRIANGLESTRIP: return count + 2;
      case D3DPT_TRIANGLEFAN:   return count + 2;
    }
  }


  MtlInputAssemblyState DecodeInputAssemblyState(D3DPRIMITIVETYPE type) {
    switch (type) {
      default:
      case D3DPT_TRIANGLELIST:
        return MtlInputAssemblyState(MtlPrimitiveTopology::TriangleList, false);

      case D3DPT_POINTLIST:
        return MtlInputAssemblyState(MtlPrimitiveTopology::PointList, false);

      case D3DPT_LINELIST:
        return MtlInputAssemblyState(MtlPrimitiveTopology::LineList, false);

      case D3DPT_LINESTRIP:
        return MtlInputAssemblyState(MtlPrimitiveTopology::LineStrip, false);

      case D3DPT_TRIANGLESTRIP:
        return MtlInputAssemblyState(MtlPrimitiveTopology::TriangleStrip, false);

      case D3DPT_TRIANGLEFAN:
        return MtlInputAssemblyState(MtlPrimitiveTopology::TriangleFan, false);
    }
  }


  MtlBlendFactor DecodeBlendFactor(D3DBLEND BlendFactor, bool IsAlpha) {
    switch (BlendFactor) {
      default:
      case D3DBLEND_ZERO:            return MtlBlendFactor::Zero;
      case D3DBLEND_ONE:             return MtlBlendFactor::One;
      case D3DBLEND_SRCCOLOR:        return MtlBlendFactor::SourceColor;
      case D3DBLEND_INVSRCCOLOR:     return MtlBlendFactor::OneMinusSourceColor;
      case D3DBLEND_SRCALPHA:        return MtlBlendFactor::SourceAlpha;
      case D3DBLEND_INVSRCALPHA:     return MtlBlendFactor::OneMinusSourceAlpha;
      case D3DBLEND_DESTALPHA:       return MtlBlendFactor::DestAlpha;
      case D3DBLEND_INVDESTALPHA:    return MtlBlendFactor::OneMinusDestAlpha;
      case D3DBLEND_DESTCOLOR:       return MtlBlendFactor::DestColor;
      case D3DBLEND_INVDESTCOLOR:    return MtlBlendFactor::OneMinusDestColor;
      case D3DBLEND_SRCALPHASAT:     return MtlBlendFactor::SourceAlphaSaturated;
      case D3DBLEND_BOTHSRCALPHA:    return MtlBlendFactor::SourceAlpha;
      case D3DBLEND_BOTHINVSRCALPHA: return MtlBlendFactor::OneMinusSourceAlpha;
      case D3DBLEND_BLENDFACTOR:     return IsAlpha ? MtlBlendFactor::BlendAlpha : MtlBlendFactor::BlendColor;
      case D3DBLEND_INVBLENDFACTOR:  return IsAlpha ? MtlBlendFactor::OneMinusBlendAlpha : MtlBlendFactor::OneMinusBlendColor;
      case D3DBLEND_SRCCOLOR2:       return MtlBlendFactor::Source1Color;
      case D3DBLEND_INVSRCCOLOR2:    return MtlBlendFactor::OneMinusSource1Color;
    }
  }


  MtlBlendOperation DecodeBlendOp(D3DBLENDOP BlendOp) {
    switch (BlendOp) {
      default:
      case D3DBLENDOP_ADD:          return MtlBlendOperation::Add;
      case D3DBLENDOP_SUBTRACT:     return MtlBlendOperation::Subtract;
      case D3DBLENDOP_REVSUBTRACT:  return MtlBlendOperation::ReverseSubtract;
      case D3DBLENDOP_MIN:          return MtlBlendOperation::Min;
      case D3DBLENDOP_MAX:          return MtlBlendOperation::Max;
    }
  }


  MtlCompareFunction DecodeCompareOp(D3DCMPFUNC Func) {
    switch (Func) {
      default:
      case D3DCMP_NEVER:        return MtlCompareFunction::Never;
      case D3DCMP_LESS:         return MtlCompareFunction::Less;
      case D3DCMP_EQUAL:        return MtlCompareFunction::Equal;
      case D3DCMP_LESSEQUAL:    return MtlCompareFunction::LessEqual;
      case D3DCMP_GREATER:      return MtlCompareFunction::Greater;
      case D3DCMP_NOTEQUAL:     return MtlCompareFunction::NotEqual;
      case D3DCMP_GREATEREQUAL: return MtlCompareFunction::GreaterEqual;
      case D3DCMP_ALWAYS:       return MtlCompareFunction::Always;
    }
  }


  MtlStencilOperation DecodeStencilOp(D3DSTENCILOP Op) {
    switch (Op) {
      default:
      case D3DSTENCILOP_KEEP:    return MtlStencilOperation::Keep;
      case D3DSTENCILOP_ZERO:    return MtlStencilOperation::Zero;
      case D3DSTENCILOP_REPLACE: return MtlStencilOperation::Replace;
      case D3DSTENCILOP_INCRSAT: return MtlStencilOperation::IncrementClamp;
      case D3DSTENCILOP_DECRSAT: return MtlStencilOperation::DecrementClamp;
      case D3DSTENCILOP_INVERT:  return MtlStencilOperation::Invert;
      case D3DSTENCILOP_INCR:    return MtlStencilOperation::IncrementWrap;
      case D3DSTENCILOP_DECR:    return MtlStencilOperation::DecrementWrap;
    }
  }


  MtlCullMode DecodeCullMode(D3DCULL Mode) {
    switch (Mode) {
      case D3DCULL_CW:   return MtlCullMode::Front;
      case D3DCULL_CCW:  return MtlCullMode::Back;
      default:
      case D3DCULL_NONE: return MtlCullMode::None;
    }
  }


  MtlTriangleFillMode DecodeFillMode(D3DFILLMODE Mode) {
    switch (Mode) {
      case D3DFILL_POINT:     return MtlTriangleFillMode::Lines;
      case D3DFILL_WIREFRAME: return MtlTriangleFillMode::Lines;
      default:
      case D3DFILL_SOLID:     return MtlTriangleFillMode::Fill;
    }
  }


  MtlIndexType DecodeIndexType(D3D9Format Format) {
    return Format == D3D9Format::INDEX16
                   ? MtlIndexType::Uint16
                   : MtlIndexType::Uint32;
  }


  MtlFormat DecodeDecltype(D3DDECLTYPE Type) {
    switch (Type) {
      case D3DDECLTYPE_FLOAT1:    return MtlFormat::R32Float;
      case D3DDECLTYPE_FLOAT2:    return MtlFormat::RG32Float;
      // Use the vertex-only RGB32Float marker (not a promotion to Float4): reading
      // this as Float4 previously left a stale w=0 instead of the required 1.0
      // default and broke D3D9 FLOAT3/XYZRHW positions (GTA IV 2D).
      case D3DDECLTYPE_FLOAT3:    return MtlFormat::RGB32Float;
      case D3DDECLTYPE_FLOAT4:    return MtlFormat::RGBA32Float;
      case D3DDECLTYPE_D3DCOLOR:  return MtlFormat::BGRA8Unorm;
      case D3DDECLTYPE_UBYTE4:    return MtlFormat::Invalid;
      case D3DDECLTYPE_SHORT2:    return MtlFormat::Invalid;
      case D3DDECLTYPE_SHORT4:    return MtlFormat::Invalid;
      case D3DDECLTYPE_UBYTE4N:   return MtlFormat::RGBA8Unorm;
      case D3DDECLTYPE_SHORT2N:   return MtlFormat::RG16Snorm;
      case D3DDECLTYPE_SHORT4N:   return MtlFormat::RGBA16Snorm;
      case D3DDECLTYPE_USHORT2N:  return MtlFormat::RG16Unorm;
      case D3DDECLTYPE_USHORT4N:  return MtlFormat::RGBA16Unorm;
      case D3DDECLTYPE_UDEC3:     return MtlFormat::Invalid;
      case D3DDECLTYPE_FLOAT16_2: return MtlFormat::RG16Float;
      case D3DDECLTYPE_FLOAT16_4: return MtlFormat::RGBA16Float;
      case D3DDECLTYPE_DEC3N:     return MtlFormat::Invalid;
      case D3DDECLTYPE_UNUSED:
      default:                    return MtlFormat::Invalid;
    }
  }

  void ConvertBox(D3DBOX box, MtlOrigin& offset, MtlSize& extent) {
    offset.x = box.Left;
    offset.y = box.Top;
    offset.z = box.Front;

    extent.width  = box.Right  - box.Left;
    extent.height = box.Bottom - box.Top;
    extent.depth  = box.Back   - box.Front;
  }


  void ConvertRect(RECT rect, MtlOrigin& offset, MtlSize& extent) {
    offset.x = rect.left;
    offset.y = rect.top;
    offset.z = 0;

    extent.width  = rect.right  - rect.left;
    extent.height = rect.bottom - rect.top;
    extent.depth  = 1;
  }


  void ConvertRect(RECT rect, MtlOffset2D& offset, MtlExtent2D& extent) {
    offset.x = rect.left;
    offset.y = rect.top;

    extent.width  = rect.right  - rect.left;
    extent.height = rect.bottom - rect.top;
  }


  uint32_t GetDecltypeSize(D3DDECLTYPE Type) {
    switch (Type) {
      case D3DDECLTYPE_FLOAT1:    return 1 * sizeof(float);
      case D3DDECLTYPE_FLOAT2:    return 2 * sizeof(float);
      case D3DDECLTYPE_FLOAT3:    return 3 * sizeof(float);
      case D3DDECLTYPE_FLOAT4:    return 4 * sizeof(float);
      case D3DDECLTYPE_D3DCOLOR:  return 1 * sizeof(DWORD);
      case D3DDECLTYPE_UBYTE4:    return 4 * sizeof(BYTE);
      case D3DDECLTYPE_SHORT2:    return 2 * sizeof(short);
      case D3DDECLTYPE_SHORT4:    return 4 * sizeof(short);
      case D3DDECLTYPE_UBYTE4N:   return 4 * sizeof(BYTE);
      case D3DDECLTYPE_SHORT2N:   return 2 * sizeof(short);
      case D3DDECLTYPE_SHORT4N:   return 4 * sizeof(short);
      case D3DDECLTYPE_USHORT2N:  return 2 * sizeof(short);
      case D3DDECLTYPE_USHORT4N:  return 4 * sizeof(short);
      case D3DDECLTYPE_UDEC3:     return 4;
      case D3DDECLTYPE_DEC3N:     return 4;
      case D3DDECLTYPE_FLOAT16_2: return 2 * 2;
      case D3DDECLTYPE_FLOAT16_4: return 4 * 2;
      default:                    return 0;
    }
  }


  uint32_t GetDecltypeCount(D3DDECLTYPE Type) {
    switch (Type) {
      case D3DDECLTYPE_FLOAT1:    return 1;
      case D3DDECLTYPE_FLOAT2:    return 2;
      case D3DDECLTYPE_FLOAT3:    return 3;
      case D3DDECLTYPE_FLOAT4:    return 4;
      case D3DDECLTYPE_D3DCOLOR:  return 4;
      case D3DDECLTYPE_UBYTE4:    return 4;
      case D3DDECLTYPE_SHORT2:    return 2;
      case D3DDECLTYPE_SHORT4:    return 4;
      case D3DDECLTYPE_UBYTE4N:   return 4;
      case D3DDECLTYPE_SHORT2N:   return 2;
      case D3DDECLTYPE_SHORT4N:   return 4;
      case D3DDECLTYPE_USHORT2N:  return 2;
      case D3DDECLTYPE_USHORT4N:  return 4;
      case D3DDECLTYPE_UDEC3:     return 3;
      case D3DDECLTYPE_DEC3N:     return 3;
      case D3DDECLTYPE_FLOAT16_2: return 2;
      case D3DDECLTYPE_FLOAT16_4: return 4;
      default:                    return 0;
    }
  }

}
