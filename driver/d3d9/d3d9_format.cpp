#include "d3d9_format.h"
#include "d3d9_names.h"


namespace dxvk {

  D3D9_MTL_FORMAT_MAPPING ConvertFormatUnfixed(D3D9Format Format) {
    switch (Format) {
      case D3D9Format::Unknown: return {};

      case D3D9Format::R8G8B8: return {};

      case D3D9Format::A8R8G8B8: return {
        MtlFormat::BGRA8Unorm,
        MtlFormat::BGRA8Unorm_sRGB,
        MtlAspectColor };

      case D3D9Format::X8R8G8B8: return {
        MtlFormat::BGRA8Unorm,
        MtlFormat::BGRA8Unorm_sRGB,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One }};

      case D3D9Format::R5G6B5: return {
        MtlFormat::B5G6R5Unorm,
        MtlFormat::Invalid,
        MtlAspectColor};

      case D3D9Format::X1R5G5B5: return {
        MtlFormat::BGR5A1Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One }};

      case D3D9Format::A1R5G5B5: return {
        MtlFormat::BGR5A1Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A4R4G4B4: return {
        MtlFormat::ABGR4Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::R3G3B2: return {};

      case D3D9Format::A8: return {
        MtlFormat::R8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Zero, MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::Red }};

      case D3D9Format::A8R3G3B2: return {};

      case D3D9Format::X4R4G4B4: return {
        MtlFormat::ABGR4Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A2B10G10R10: return {
        MtlFormat::RGB10A2Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A8B8G8R8: return {
        MtlFormat::RGBA8Unorm,
        MtlFormat::RGBA8Unorm_sRGB,
        MtlAspectColor };

      case D3D9Format::X8B8G8R8: return {
        MtlFormat::RGBA8Unorm,
        MtlFormat::RGBA8Unorm_sRGB,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One }};

      case D3D9Format::G16R16: return {
        MtlFormat::RG16Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::A2R10G10B10: return {
        MtlFormat::BGR10A2Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A16B16G16R16: return {
        MtlFormat::RGBA16Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A8P8: return {};

      case D3D9Format::P8: return {};

      case D3D9Format::L8: return {
        MtlFormat::R8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Red,
          MtlSwizzle::Red, MtlSwizzle::One }};

      case D3D9Format::A8L8: return {
        MtlFormat::RG8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Red,
          MtlSwizzle::Red, MtlSwizzle::Green }};

      case D3D9Format::A4L4: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Green, MtlSwizzle::Green,
          MtlSwizzle::Green, MtlSwizzle::Red }};

      case D3D9Format::V8U8: return {
        MtlFormat::RG8Snorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::L6V5U5: return {
        MtlFormat::B5G6R5Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_L6V5U5,
          MtlFormat::RGBA16Float } };

      case D3D9Format::X8L8V8U8: return {
        MtlFormat::BGRA8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One },
        { D3D9ConversionFormat_X8L8V8U8,
          MtlFormat::RGBA16Float } };

      case D3D9Format::Q8W8V8U8: return {
        MtlFormat::RGBA8Snorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::V16U16: return {
        MtlFormat::RG16Snorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::A2W10V10U10: return {
        MtlFormat::RGB10A2Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_A2W10V10U10,
          MtlFormat::RGBA16Float } };

      case D3D9Format::W11V11U10: return {
        MtlFormat::RG11B10Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One },
        { D3D9ConversionFormat_W11V11U10,
          MtlFormat::RGBA16Snorm } };

      case D3D9Format::UYVY: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_UYVY, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::R8G8_B8G8: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::YUY2: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_YUY2, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::G8R8_G8B8: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::DXT1: return {
        MtlFormat::BC1_RGBA,
        MtlFormat::BC1_RGBA_sRGB,
        MtlAspectColor };

      case D3D9Format::DXT2: return {
        MtlFormat::BC2_RGBA,
        MtlFormat::BC2_RGBA_sRGB,
        MtlAspectColor };

      case D3D9Format::DXT3: return {
        MtlFormat::BC2_RGBA,
        MtlFormat::BC2_RGBA_sRGB,
        MtlAspectColor };

      case D3D9Format::DXT4: return {
        MtlFormat::BC3_RGBA,
        MtlFormat::BC3_RGBA_sRGB,
        MtlAspectColor };

      case D3D9Format::DXT5: return {
        MtlFormat::BC3_RGBA,
        MtlFormat::BC3_RGBA_sRGB,
        MtlAspectColor };

      case D3D9Format::D16_LOCKABLE: return {
        MtlFormat::Depth16Unorm,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D32: return {};

      case D3D9Format::D15S1: return {};

      // Apple Silicon has no D24_Unorm_S8; mapping to it crashes MTLTextureDescriptor
      // validation, so D24-family depth/stencil formats map to 32F instead.
      case D3D9Format::D24S8: return {
        MtlFormat::Depth32Float_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil };

      case D3D9Format::D24X8: return {
        MtlFormat::Depth32Float,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D24X4S4: return {};

      case D3D9Format::D16: return {
        MtlFormat::Depth16Unorm,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D32F_LOCKABLE: return {
        MtlFormat::Depth32Float,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D24FS8: return {
        MtlFormat::Depth32Float_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil };

      case D3D9Format::D32_LOCKABLE: return {};

      case D3D9Format::S8_LOCKABLE: return {};

      case D3D9Format::L16: return {
        MtlFormat::R16Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Red,
          MtlSwizzle::Red, MtlSwizzle::One }};

      case D3D9Format::VERTEXDATA: return {
        MtlFormat::R8Uint,
        MtlFormat::Invalid,
        0 };

      case D3D9Format::INDEX16: return {
        MtlFormat::R16Uint,
        MtlFormat::Invalid,
        0 };

      case D3D9Format::INDEX32: return {
        MtlFormat::R32Uint,
        MtlFormat::Invalid,
        0 };

      case D3D9Format::Q16W16V16U16: return {
        MtlFormat::RGBA16Snorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::MULTI2_ARGB8: return {};

      case D3D9Format::R16F: return {
        MtlFormat::R16Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::One,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::G16R16F: return {
        MtlFormat::RG16Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::A16B16G16R16F: return {
        MtlFormat::RGBA16Float,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::R32F: return {
        MtlFormat::R32Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::One,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::G32R32F: return {
        MtlFormat::RG32Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::One, MtlSwizzle::One }};

      case D3D9Format::A32B32G32R32F: return {
        MtlFormat::RGBA32Float,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::CxV8U8: return {};

      case D3D9Format::A1: return {};

      case D3D9Format::A2B10G10R10_XR_BIAS: return {};

      case D3D9Format::BINARYBUFFER: return {
        MtlFormat::R8Uint,
        MtlFormat::Invalid,
        0 };

      case D3D9Format::ATI1: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red,  MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::One }};

      case D3D9Format::ATI2: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Green, MtlSwizzle::Red,
          MtlSwizzle::One,   MtlSwizzle::One }};

      case D3D9Format::INST: return {};

      case D3D9Format::DF24: return {
        MtlFormat::Depth32Float,
        MtlFormat::Invalid,
        MtlAspectDepth,
        { MtlSwizzle::Red,  MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::One }};

      case D3D9Format::DF16: return {
        MtlFormat::Depth16Unorm,
        MtlFormat::Invalid,
        MtlAspectDepth,
        { MtlSwizzle::Red,  MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::One }};

      case D3D9Format::NULL_FORMAT: return {};

      case D3D9Format::GET4: return {};

      case D3D9Format::GET1: return {};

      case D3D9Format::NVDB: return {};

      case D3D9Format::A2M1: return {};

      case D3D9Format::A2M0: return {};

      case D3D9Format::ATOC: return {};

      case D3D9Format::SSAA: return {};

      case D3D9Format::INTZ: return {
        MtlFormat::Depth32Float_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil,
        { MtlSwizzle::Red, MtlSwizzle::Red,
          MtlSwizzle::Red, MtlSwizzle::Red }};

      case D3D9Format::NV12: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_NV12, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::YV12: return {
        MtlFormat::Invalid,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_YV12, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::RAWZ: return {};


      case D3D9Format::EXT1: return {};

      case D3D9Format::FXT1: return {};

      case D3D9Format::GXT1: return {};

      case D3D9Format::HXT1: return {};


      case D3D9Format::AL16: return {};

      case D3D9Format::AR16: return {};

      case D3D9Format::R16:  return {};

      case D3D9Format::L16_FOURCC:  return {};

      default:
        Logger::warn(str::format("ConvertFormat: Unknown format encountered: ", Format));
        return {};
    }
  }

  D3D9_FORMAT_BLOCK_SIZE GetFormatAlignedBlockSize(D3D9Format Format) {
    switch (Format) {
      case D3D9Format::DXT1:
      case D3D9Format::DXT2:
      case D3D9Format::DXT3:
      case D3D9Format::DXT4:
      case D3D9Format::DXT5:
      case D3D9Format::ATI1:
      case D3D9Format::ATI2:
        return { 4, 4, 1 };

      case D3D9Format::YUY2:
      case D3D9Format::UYVY:
        return { 2, 1, 1 };

      default:
        return {};
    }
  }

  D3D9MtlFormatTable::D3D9MtlFormatTable(
            D3D9Adapter*     pParent,
      const D3D9Options&     options)
    : m_isExtended (false) {
    (void)pParent;
    (void)options;
    m_dfSupport           = true;
    m_x4r4g4b4Support     = true;
    m_w11v11u10Support    = false;
    m_d16lockableSupport  = true;
    m_d24s8Support        = true;
    m_d16s8Support        = true;
    m_d32flockableSupport = true;
    m_d24fs8Support       = true;
  }

  D3D9_MTL_FORMAT_MAPPING D3D9MtlFormatTable::GetFormatMapping(
          D3D9Format          Format) const {
    D3D9_MTL_FORMAT_MAPPING mapping = ConvertFormatUnfixed(Format);

    if (Format == D3D9Format::X4R4G4B4 && !m_x4r4g4b4Support)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::W11V11U10 && !m_w11v11u10Support)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::D16_LOCKABLE && !m_d16lockableSupport)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::DF16 && !m_dfSupport)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::DF24 && !m_dfSupport)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::D32F_LOCKABLE && !m_d32flockableSupport)
      return D3D9_MTL_FORMAT_MAPPING();

    if (Format == D3D9Format::D24FS8 && !m_d24fs8Support)
      return D3D9_MTL_FORMAT_MAPPING();


    return mapping;
  }


  const MtlFormatInfo* D3D9MtlFormatTable::GetUnsupportedFormatInfo(
    D3D9Format            Format) const {
    static const MtlFormatInfo r8b8g8        = { 3, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo r3g3b2        = { 1, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo x4r4g4b4      = { 2, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo a8r3g3b2      = { 2, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo a8p8          = { 2, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo p8            = { 1, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo w11v11u10     = { 4, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo cxv8u8        = { 2, 1, 1, MtlFormatFlagNone,    MtlAspectColor   };
    static const MtlFormatInfo d16_lockable  = { 2, 1, 1, MtlFormatFlagDepth,   MtlAspectDepth   };
    static const MtlFormatInfo d32f_lockable = { 4, 1, 1, MtlFormatFlagDepth,   MtlAspectDepth   };
    static const MtlFormatInfo d32_lockable  = { 4, 1, 1, MtlFormatFlagDepth,   MtlAspectDepth   };
    static const MtlFormatInfo s8_lockable   = { 1, 1, 1, MtlFormatFlagStencil, MtlAspectStencil };
    static const MtlFormatInfo unknown       = {};

    switch (Format) {
      case D3D9Format::R8G8B8:
        return &r8b8g8;

      case D3D9Format::R3G3B2:
        return &r3g3b2;

      case D3D9Format::X4R4G4B4:
        return &x4r4g4b4;

      case D3D9Format::A8R3G3B2:
        return &a8r3g3b2;

      case D3D9Format::A8P8:
        return &a8p8;

      case D3D9Format::P8:
        return &p8;

      case D3D9Format::W11V11U10:
        return &w11v11u10;


      case D3D9Format::CxV8U8:
        return &cxv8u8;


      case D3D9Format::D16_LOCKABLE:
        return &d16_lockable;

      case D3D9Format::D32F_LOCKABLE:
        return &d32f_lockable;

      case D3D9Format::D32_LOCKABLE:
        if (m_isExtended)
          return &d32_lockable;

        [[fallthrough]];

      case D3D9Format::S8_LOCKABLE:
        if (m_isExtended)
          return &s8_lockable;

        [[fallthrough]];

      default:
        return &unknown;
    }
  }


  bool D3D9MtlFormatTable::CheckImageFormatSupport(
          MtlFormat             Format) const {
    (void)Format;
    return true;
  }


  void D3D9MtlFormatTable::RefreshFormatSupport(
    const D3D9Adapter*          pParent) {
    (void)pParent;
  }

}
