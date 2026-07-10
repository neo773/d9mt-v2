#include "d3d9_format.h"
#include "d3d9_names.h"

// Retargeted from DXVK's d3d9_format.cpp. The D3D9Format switch structure is
// verbatim; each Vk* payload is retyped to the nearest MtlFormat (doc 02 §2:
// "mechanical VkFormat -> WMTPixelFormat retype of ~30 rows"). Rows whose exact
// Metal pixel format is not yet in mtl_types.h's working subset return
// MtlFormat::Invalid with a `// PHASE-C:` note rather than inventing enum values.
// The Vulkan format-support probing (DxvkAdapter::getFormatFeatures) is Phase C.

namespace dxvk {

  // It is also worth noting that the msb/lsb-ness is flipped between VK and D3D9.
  D3D9_MTL_FORMAT_MAPPING ConvertFormatUnfixed(D3D9Format Format) {
    switch (Format) {
      case D3D9Format::Unknown: return {};

      case D3D9Format::R8G8B8: return {}; // Unsupported

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
        MtlFormat::BGR5A1Unorm,   // PHASE-C: D3D9 A1R5G5B5 packing vs MTL BGR5A1
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One }};

      case D3D9Format::A1R5G5B5: return {
        MtlFormat::BGR5A1Unorm,   // PHASE-C: verify A1R5G5B5 channel order
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A4R4G4B4: return {
        MtlFormat::ABGR4Unorm,    // PHASE-C: verify A4R4G4B4 channel order
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::R3G3B2: return {}; // Unsupported

      case D3D9Format::A8: return {
        MtlFormat::R8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Zero, MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::Red }};

      case D3D9Format::A8R3G3B2: return {}; // Unsupported

      case D3D9Format::X4R4G4B4: return {
        MtlFormat::ABGR4Unorm,    // PHASE-C: verify A4R4G4B4 channel order
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A2B10G10R10: return {
        MtlFormat::RGB10A2Unorm, // The A2 is out of place here. This should be investigated.
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
        MtlFormat::BGR10A2Unorm,  // PHASE-C: A2R10G10B10 vs MTL BGR10A2 ordering
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A16B16G16R16: return {
        MtlFormat::RGBA16Unorm,
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::A8P8: return {}; // Unsupported

      case D3D9Format::P8: return {}; // Unsupported

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
        MtlFormat::Invalid,       // PHASE-C: no MTL R4G4 (VK_FORMAT_R4G4_UNORM_PACK8)
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
        // Any PACK16 format will do...
        MtlFormat::B5G6R5Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_L6V5U5,
        // Convert -> float (this is a mixed snorm and unorm type)
          MtlFormat::RGBA16Float } };

      case D3D9Format::X8L8V8U8: return {
        MtlFormat::BGRA8Unorm,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One },
        { D3D9ConversionFormat_X8L8V8U8,
        // Convert -> float (this is a mixed snorm and unorm type)
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
        // Convert -> float (this is a mixed snorm and unorm type)
          MtlFormat::RGBA16Float } };

      case D3D9Format::W11V11U10: return {
        MtlFormat::RG11B10Float,
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::One },
        { D3D9ConversionFormat_W11V11U10,
        // can't use B10G11R11 because this is a snorm type
          MtlFormat::RGBA16Snorm } };

      case D3D9Format::UYVY: return {
        MtlFormat::Invalid,       // PHASE-C: YUV 4:2:2 (VK_FORMAT_G8B8G8R8_422_UNORM)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_UYVY, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::R8G8_B8G8: return {
        MtlFormat::Invalid,       // PHASE-C: YUV 4:2:2 (VK_FORMAT_G8B8G8R8_422_UNORM)
        MtlFormat::Invalid,
        MtlAspectColor };

      case D3D9Format::YUY2: return {
        MtlFormat::Invalid,       // PHASE-C: YUV 4:2:2 (VK_FORMAT_G8B8G8R8_422_UNORM)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_YUY2, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::G8R8_G8B8: return {
        MtlFormat::Invalid,       // PHASE-C: YUV 4:2:2 (VK_FORMAT_B8G8R8G8_422_UNORM)
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

      case D3D9Format::D32: return {}; // Unsupported (everywhere)

      case D3D9Format::D15S1: return {}; // Unsupported (everywhere)

      case D3D9Format::D24S8: return {
        MtlFormat::Depth24Unorm_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil };

      case D3D9Format::D24X8: return {
        MtlFormat::Depth24Unorm_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D24X4S4: return {}; // Unsupported (everywhere)

      case D3D9Format::D16: return {
        MtlFormat::Depth16Unorm,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D32F_LOCKABLE: return {
        MtlFormat::Depth32Float,
        MtlFormat::Invalid,
        MtlAspectDepth };

      case D3D9Format::D24FS8: return {
        MtlFormat::Depth24Unorm_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil };

      case D3D9Format::D32_LOCKABLE: return {}; // Unsupported (everywhere)

      case D3D9Format::S8_LOCKABLE: return {}; // Unsupported (everywhere)

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

      case D3D9Format::MULTI2_ARGB8: return {}; // Unsupported

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

      case D3D9Format::CxV8U8: return {}; // Unsupported

      case D3D9Format::A1: return {}; // Unsupported

      case D3D9Format::A2B10G10R10_XR_BIAS: return {}; // Unsupported (everywhere)

      case D3D9Format::BINARYBUFFER: return {
        MtlFormat::R8Uint,
        MtlFormat::Invalid,
        0 };

      case D3D9Format::ATI1: return {
        MtlFormat::Invalid,       // PHASE-C: BC4 (VK_FORMAT_BC4_UNORM_BLOCK)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red,  MtlSwizzle::Zero,
          MtlSwizzle::Zero, MtlSwizzle::One }};

      case D3D9Format::ATI2: return {
        MtlFormat::Invalid,       // PHASE-C: BC5 (VK_FORMAT_BC5_UNORM_BLOCK)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Green, MtlSwizzle::Red,
          MtlSwizzle::One,   MtlSwizzle::One }};

      case D3D9Format::INST: return {}; // Driver hack, handled elsewhere

      case D3D9Format::DF24: return {
        MtlFormat::Depth24Unorm_Stencil8,
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

      case D3D9Format::NULL_FORMAT: return {}; // Driver hack, handled elsewhere

      case D3D9Format::GET4: return {}; // Unsupported

      case D3D9Format::GET1: return {}; // Unsupported

      case D3D9Format::NVDB: return {}; // Driver hack, handled elsewhere

      case D3D9Format::A2M1: return {}; // Driver hack, handled elsewhere

      case D3D9Format::A2M0: return {}; // Driver hack, handled elsewhere

      case D3D9Format::ATOC: return {}; // Driver hack, handled elsewhere

      case D3D9Format::SSAA: return {}; // Driver hack, handled elsewhere

      case D3D9Format::INTZ: return {
        MtlFormat::Depth24Unorm_Stencil8,
        MtlFormat::Invalid,
        MtlAspectDepth | MtlAspectStencil,
        { MtlSwizzle::Red, MtlSwizzle::Red,
          MtlSwizzle::Red, MtlSwizzle::Red }};

      case D3D9Format::NV12: return {
        MtlFormat::Invalid,       // PHASE-C: 2-plane 420 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_NV12, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::YV12: return {
        MtlFormat::Invalid,       // PHASE-C: 3-plane 420 (VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
        MtlFormat::Invalid,
        MtlAspectColor,
        { MtlSwizzle::Red, MtlSwizzle::Green,
          MtlSwizzle::Blue, MtlSwizzle::Alpha },
        { D3D9ConversionFormat_YV12, MtlFormat::BGRA8Unorm }
      };

      case D3D9Format::RAWZ: return {}; // Unsupported

      // EXT1, FXT1, GXT1 and HXT1 are checked for support
      // by D3D9 SAGE engine games (e.g. Command & Conquer 3)

      case D3D9Format::EXT1: return {}; // Unsupported (everywhere)

      case D3D9Format::FXT1: return {}; // Unsupported (everywhere)

      case D3D9Format::GXT1: return {}; // Unsupported (everywhere)

      case D3D9Format::HXT1: return {}; // Unsupported (everywhere)

      // AL16 and R16 FOURCCs are often checked for support by
      // various D3D8 and early D3D9 games. AR16 and L16 (FOURCC)
      // are also checked for support, but to a lesser extent.

      case D3D9Format::AL16: return {}; // Unsupported (everywhere)

      case D3D9Format::AR16: return {}; // Unsupported (everywhere)

      case D3D9Format::R16:  return {}; // Unsupported (everywhere)

      case D3D9Format::L16_FOURCC:  return {}; // Unsupported (everywhere)

      default:
        Logger::warn(str::format("ConvertFormat: Unknown format encountered: ", Format));
        return {}; // Unsupported
    }
  }

  // Block size of formats that require some form of alignment
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
        return {}; // Irrelevant or unknown block size
    }
  }

  D3D9MtlFormatTable::D3D9MtlFormatTable(
            D3D9Adapter*     pParent,
      const D3D9Options&     options)
    : m_isExtended (false) {   // PHASE-C: pParent->IsExtended()
    // PHASE-C: real vendor + Metal format-support probing. Was gated on
    // DxvkGpuVendor + DxvkAdapter::getFormatFeatures. Stubbed permissive so the
    // state tracker sees all D3D9 formats as supported for now.
    (void)pParent;
    (void)options;
    m_dfSupport           = true;
    m_x4r4g4b4Support     = true;
    m_w11v11u10Support    = false;   // W11V11U10 is only supported by D3D8
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

    // PHASE-C: DXVK remapped D24_UNORM_S8_UINT -> D32_SFLOAT(_S8) and
    // D16_UNORM_S8_UINT -> D24/D32 fallbacks when the GPU lacked native support.
    // Metal exposes Depth24Unorm_Stencil8 support at runtime via the device query;
    // wire the fallback (mapping.FormatColor rewrite) once caps probing lands.

    return mapping;
  }


  const MtlFormatInfo* D3D9MtlFormatTable::GetUnsupportedFormatInfo(
    D3D9Format            Format) const {
    // MtlFormatInfo{ elementSize, blockSize, aspects, planeCount }
    static const MtlFormatInfo r8b8g8        = { 3, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo r3g3b2        = { 1, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo x4r4g4b4      = { 2, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo a8r3g3b2      = { 2, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo a8p8          = { 2, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo p8            = { 1, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo w11v11u10     = { 4, { 1, 1, 1 }, MtlAspectColor,   1 };
    static const MtlFormatInfo cxv8u8        = { 2, { 1, 1, 1 }, MtlAspectColor,   1 };
    // Unsupported and potentially unsupported lockable depth formats need to be
    // listed here in order to allow for the creation of offscreen plain surfaces
    static const MtlFormatInfo d16_lockable  = { 2, { 1, 1, 1 }, MtlAspectDepth,   1 };
    static const MtlFormatInfo d32f_lockable = { 4, { 1, 1, 1 }, MtlAspectDepth,   1 };
    static const MtlFormatInfo d32_lockable  = { 4, { 1, 1, 1 }, MtlAspectDepth,   1 };
    static const MtlFormatInfo s8_lockable   = { 1, { 1, 1, 1 }, MtlAspectStencil, 1 };
    static const MtlFormatInfo unknown       = {};

    switch (Format) {
      case D3D9Format::R8G8B8:
        return &r8b8g8;

      case D3D9Format::R3G3B2:
        return &r3g3b2;

      // potentially unsupported through a config option
      case D3D9Format::X4R4G4B4:
        return &x4r4g4b4;

      case D3D9Format::A8R3G3B2:
        return &a8r3g3b2;

      case D3D9Format::A8P8:
        return &a8p8;

      case D3D9Format::P8:
        return &p8;

      // only supported by D3D8
      case D3D9Format::W11V11U10:
        return &w11v11u10;

      // MULTI2_ARGB8 -> Don't have a clue what this is.

      case D3D9Format::CxV8U8:
        return &cxv8u8;

      // A1 -> Doesn't map nicely here cause it's not byte aligned.
      // Gonna just pretend that doesn't exist until something
      // depends on that.

      // only supported on AMD
      case D3D9Format::D16_LOCKABLE:
        return &d16_lockable;

      // unsupported on Intel
      case D3D9Format::D32F_LOCKABLE:
        return &d32f_lockable;

      // only considered on d3d9Ex interfaces
      case D3D9Format::D32_LOCKABLE:
        if (m_isExtended)
          return &d32_lockable;

        [[fallthrough]];

      // only considered on d3d9Ex interfaces
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
    // PHASE-C: query the Metal device for format capabilities (was
    // DxvkAdapter::getFormatFeatures against linear/optimal tiling feature flags).
    (void)Format;
    return true;
  }


  void D3D9MtlFormatTable::RefreshFormatSupport(
    const D3D9Adapter*          pParent) {
    // PHASE-C: W11V11U10 support depends on D3D8 compatibility (pParent->IsD3D8Compatible()).
    (void)pParent;
  }

}
