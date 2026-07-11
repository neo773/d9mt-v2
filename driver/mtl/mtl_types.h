#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// mtl_types.h — Metal-first value type set for the D3D9 state-tracker seam.
//
// This is the "replacement map" payload from doc 02 (v2/docs/rewrite/
// 02-type-and-file-map.md §2) and the enum/value types the MtlContext v0 sketch
// signatures use (doc 01 §4). ZERO Vulkan: every Vk* type that reached src/d3d9/
// has a Metal-native POD here.
//
// The enum decoders in d3d9_util.h (Decode{BlendFactor,BlendOp,Filter,...}) are a
// 1:1 Vk*->Mtl* retype (doc 02 §1, line 6 of the summary); their switch bodies are
// unchanged, only the return type flips to the Mtl* enums below. Numeric values
// mirror winemetal's WMT* enums (v2/third_party/winemetal/winemetal.h) and the
// underlying MTL* constants, so a later `using MtlFormat = WMTPixelFormat;` swap is
// value-compatible. Defined standalone here to keep the driver compile gate
// independent of the winemetal ABI header at this phase (doc-sanctioned fallback).
//
// NOTE (namespace): the DXVK d3d9 frontend is `namespace dxvk`; to keep the
// extraction thesis "frontend compiles unchanged" (DxvkContext -> MtlContext is a
// pure rename), all Mtl* seam types live in `namespace dxvk` too. This deviates
// from the doc 01 §4 sketch's `namespace d9mt` — see EXTRACTION-LOG.md.
// ---------------------------------------------------------------------------

namespace dxvk {

  // Opaque winemetal object handle (WMTObject/obj_handle_t is a uint64 token that
  // names an id<MTL...> on the unix side). PHASE-C: real handle plumbing.
  using MtlObjectHandle = uint64_t;
  constexpr MtlObjectHandle MtlNullHandle = 0ull;

  using MtlDeviceSize = uint64_t;   // was VkDeviceSize

  // --- shader stages / pipeline (doc 01 §4) --------------------------------
  enum class MtlShaderStage : uint32_t {
    Vertex   = 0,
    Fragment = 1,
  };

  using MtlStageMask = uint32_t;    // bit per MtlShaderStage
  constexpr MtlStageMask MtlStageVertex   = 1u << 0;
  constexpr MtlStageMask MtlStageFragment = 1u << 1;

  enum class MtlPipeline : uint32_t {
    Graphics = 0,                   // D3D9 has no compute path through this seam
  };

  // --- index / primitive types --------------------------------------------
  enum class MtlIndexType : uint32_t {   // was VkIndexType
    Uint16 = 0,
    Uint32 = 1,
  };

  // Metal splits the topology *class* (PSO) from the *type* (draw). This enum is
  // the D3D9-facing set; fans are emulated at record time.
  enum class MtlPrimitiveTopology : uint32_t {
    PointList     = 0,
    LineList      = 1,
    LineStrip     = 2,
    TriangleList  = 3,
    TriangleStrip = 4,
    TriangleFan   = 5,   // emulated (no native Metal fan)
  };

  // Was DxvkInputAssemblyState { VkPrimitiveTopology, primitiveRestart }. Metal
  // splits topology *class* (PSO) from *type* (draw); this carries the D3D9-facing
  // topology plus the primitive-restart flag (unused on Metal strips — always false
  // for D3D9, which has no restart index). Produced by DecodeInputAssemblyState.
  struct MtlInputAssemblyState {
    MtlInputAssemblyState() = default;
    MtlInputAssemblyState(MtlPrimitiveTopology t, bool restart)
    : topology(t), primitiveRestart(restart) { }
    MtlPrimitiveTopology topology         = MtlPrimitiveTopology::TriangleList;
    bool                 primitiveRestart = false;
  };

  // --- enum-decoder outputs (d3d9_util.h Decode*) --------------------------
  // Values mirror MTLCompareFunction.
  enum class MtlCompareFunction : uint32_t {
    Never        = 0,
    Less         = 1,
    Equal        = 2,
    LessEqual    = 3,
    Greater      = 4,
    NotEqual     = 5,
    GreaterEqual = 6,
    Always       = 7,
  };

  // Values mirror MTLBlendFactor.
  enum class MtlBlendFactor : uint32_t {
    Zero                     = 0,
    One                      = 1,
    SourceColor              = 2,
    OneMinusSourceColor      = 3,
    SourceAlpha              = 4,
    OneMinusSourceAlpha      = 5,
    DestColor                = 6,
    OneMinusDestColor        = 7,
    DestAlpha                = 8,
    OneMinusDestAlpha        = 9,
    SourceAlphaSaturated     = 10,
    BlendColor               = 11,
    OneMinusBlendColor       = 12,
    BlendAlpha               = 13,
    OneMinusBlendAlpha       = 14,
    Source1Color             = 15,
    OneMinusSource1Color     = 16,
    Source1Alpha             = 17,
    OneMinusSource1Alpha     = 18,
  };

  // Values mirror MTLBlendOperation.
  enum class MtlBlendOperation : uint32_t {
    Add             = 0,
    Subtract        = 1,
    ReverseSubtract = 2,
    Min             = 3,
    Max             = 4,
  };

  // Values mirror MTLStencilOperation.
  enum class MtlStencilOperation : uint32_t {
    Keep           = 0,
    Zero           = 1,
    Replace        = 2,
    IncrementClamp = 3,
    DecrementClamp = 4,
    Invert         = 5,
    IncrementWrap  = 6,
    DecrementWrap  = 7,
  };

  enum class MtlCullMode : uint32_t {    // MTLCullMode
    None  = 0,
    Front = 1,
    Back  = 2,
  };

  enum class MtlTriangleFillMode : uint32_t {   // MTLTriangleFillMode
    Fill  = 0,
    Lines = 1,
  };

  enum class MtlWinding : uint32_t {     // MTLWinding
    Clockwise        = 0,
    CounterClockwise = 1,
  };

  enum class MtlSamplerAddressMode : uint32_t {  // MTLSamplerAddressMode
    ClampToEdge        = 0,
    MirrorClampToEdge  = 1,
    Repeat             = 2,
    MirrorRepeat       = 3,
    ClampToZero        = 4,
    ClampToBorderColor = 5,
  };

  enum class MtlSamplerFilter : uint32_t {   // MTLSamplerMinMagFilter / blit filter
    Nearest = 0,
    Linear  = 1,
  };

  enum class MtlSamplerMipFilter : uint32_t {
    NotMipmapped = 0,
    Nearest      = 1,
    Linear       = 2,
  };

  // --- format ---------------------------------------------------------------
  // Mirrors winemetal WMTPixelFormat (uint32 values). Only a working subset is
  // enumerated; d3d9_format.cpp's mapping table (ADAPT) fills the rest.
  // IsValid flips from `!= VK_FORMAT_UNDEFINED` to `!= MtlFormat::Invalid`.
  enum class MtlFormat : uint32_t {
    Invalid            = 0,
    A8Unorm            = 1,
    R8Unorm            = 10,
    R8Unorm_sRGB       = 11,
    R8Snorm            = 12,
    R8Uint             = 13,
    R8Sint             = 14,
    R16Unorm           = 20,
    R16Snorm           = 22,
    R16Uint            = 23,
    R16Sint            = 24,
    R16Float           = 25,
    RG8Unorm           = 30,
    RG8Unorm_sRGB      = 31,
    RG8Snorm           = 32,
    B5G6R5Unorm        = 40,
    A1BGR5Unorm        = 41,
    ABGR4Unorm         = 42,
    BGR5A1Unorm        = 43,
    R32Uint            = 53,
    R32Sint            = 54,
    R32Float           = 55,
    RG16Unorm          = 60,
    RG16Snorm          = 62,
    RG16Float          = 65,
    RGBA8Unorm         = 70,
    RGBA8Unorm_sRGB    = 71,
    RGBA8Snorm         = 72,
    BGRA8Unorm         = 80,
    BGRA8Unorm_sRGB    = 81,
    RGB10A2Unorm       = 90,
    RG11B10Float       = 92,
    RGB9E5Float        = 93,
    BGR10A2Unorm       = 94,
    RG32Uint           = 103,
    RG32Float          = 105,
    RGBA16Unorm        = 110,
    RGBA16Snorm        = 112,
    RGBA16Float        = 115,
    RGBA32Float        = 125,
    // depth / stencil
    Depth16Unorm       = 250,
    Depth32Float       = 252,
    Stencil8           = 253,
    Depth24Unorm_Stencil8 = 255,
    Depth32Float_Stencil8 = 260,
    // block-compressed (DXT/BCn)
    BC1_RGBA           = 130,
    BC1_RGBA_sRGB      = 131,
    BC2_RGBA           = 132,
    BC2_RGBA_sRGB      = 133,
    BC3_RGBA           = 134,
    BC3_RGBA_sRGB      = 135,
    // PHASE-C: complete the table to full WMTPixelFormat parity in d3d9_format.cpp.
  };

  // Plane / aspect selector — replaces VkImageAspectFlags (depth vs stencil vs
  // color, plane index for planar formats). Bitmask.
  using MtlAspectMask = uint32_t;
  constexpr MtlAspectMask MtlAspectColor   = 1u << 0;
  constexpr MtlAspectMask MtlAspectDepth   = 1u << 1;
  constexpr MtlAspectMask MtlAspectStencil = 1u << 2;

  // Component swizzle — replaces VkComponentMapping. Values mirror
  // MTLTextureSwizzle (0=Zero,1=One,2..5=RGBA).
  enum class MtlSwizzle : uint8_t {
    Zero = 0, One = 1, Red = 2, Green = 3, Blue = 4, Alpha = 5,
  };

  struct MtlSwizzleChannels {
    MtlSwizzle r = MtlSwizzle::Red;
    MtlSwizzle g = MtlSwizzle::Green;
    MtlSwizzle b = MtlSwizzle::Blue;
    MtlSwizzle a = MtlSwizzle::Alpha;

    bool operator == (const MtlSwizzleChannels& o) const {
      return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator != (const MtlSwizzleChannels& o) const { return !(*this == o); }
  };

  // --- geometry PODs (replace VkExtent*/VkOffset*/VkImageSubresource*) ------
  struct MtlOrigin {   // was VkOffset3D
    int32_t x = 0, y = 0, z = 0;
  };

  struct MtlSize {     // was VkExtent3D
    uint32_t width = 0, height = 0, depth = 0;
  };

  struct MtlOffset3D { // signed 3D point for blit corners
    int32_t x = 0, y = 0, z = 0;
  };

  struct MtlExtent2D {
    uint32_t width = 0, height = 0;
  };

  struct MtlOffset2D { // was VkOffset2D
    int32_t x = 0, y = 0;
  };

  struct MtlRect {     // MTLScissorRect
    uint32_t x = 0, y = 0, width = 0, height = 0;
  };

  // was VkImageSubresourceLayers { aspect, mip, baseLayer, layerCount }
  struct MtlSubresourceLayers {
    MtlAspectMask aspects    = MtlAspectColor;
    uint32_t      mipLevel   = 0;
    uint32_t      baseLayer  = 0;
    uint32_t      layerCount = 1;
  };

  // was VkImageSubresourceRange
  struct MtlSubresourceRange {
    MtlAspectMask aspects    = MtlAspectColor;
    uint32_t      baseMip    = 0;
    uint32_t      mipCount   = 1;
    uint32_t      baseLayer  = 0;
    uint32_t      layerCount = 1;
  };

  // --- viewport / raster / MSAA / depth-stencil / blend --------------------
  struct MtlViewport {   // MTLViewport + MTLScissorRect pair
    // Coordinates are render-target pixels only. Retina/backing-scale is applied
    // exclusively in the swapchain/present layer (doc 01 §6 resolution 1).
    float originX = 0.0f, originY = 0.0f;
    float width   = 0.0f, height  = 0.0f;
    float znear   = 0.0f, zfar    = 1.0f;
    MtlRect scissor = { };
  };

  struct MtlRasterizerState {
    MtlCullMode         cullMode  = MtlCullMode::None;
    MtlTriangleFillMode fillMode  = MtlTriangleFillMode::Fill;
    MtlWinding          frontFace = MtlWinding::Clockwise;
    bool                depthClip = true;   // MTLDepthClipMode::Clip vs Clamp
  };

  struct MtlMultisampleState {
    uint32_t sampleCount      = 1;
    uint32_t sampleMask       = 0xFFFFFFFFu;
    bool     alphaToCoverage  = false;
    bool     alphaToOne       = false;
  };

  struct MtlStencilState {
    MtlStencilOperation failOp      = MtlStencilOperation::Keep;
    MtlStencilOperation depthFailOp = MtlStencilOperation::Keep;
    MtlStencilOperation passOp      = MtlStencilOperation::Keep;
    MtlCompareFunction  compareOp   = MtlCompareFunction::Always;
    uint32_t            readMask    = 0xFFu;
    uint32_t            writeMask   = 0xFFu;
  };

  struct MtlDepthStencilState {   // -> cached MTLDepthStencilState (NOT a PSO key)
    MtlCompareFunction depthCompareOp = MtlCompareFunction::Always;
    bool               depthWrite     = false;
    bool               stencilEnable  = false;
    MtlStencilState    front = { };
    MtlStencilState    back  = { };
  };

  // Per-color-attachment blend descriptor (folds into the render PSO key).
  struct MtlBlendMode {
    bool              enable        = false;
    MtlBlendFactor    colorSrc      = MtlBlendFactor::One;
    MtlBlendFactor    colorDst      = MtlBlendFactor::Zero;
    MtlBlendOperation colorOp       = MtlBlendOperation::Add;
    MtlBlendFactor    alphaSrc      = MtlBlendFactor::One;
    MtlBlendFactor    alphaDst      = MtlBlendFactor::Zero;
    MtlBlendOperation alphaOp       = MtlBlendOperation::Add;
    uint32_t          writeMask     = 0xFu;   // RGBA bits
  };

  struct MtlColor4 {   // blend constant / clear color
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
  };

  struct MtlDepthBias {   // setDepthBias:slopeScale:clamp:
    float constantFactor = 0.0f;
    float slopeFactor    = 0.0f;
    float clamp          = 0.0f;
  };

  struct MtlDepthBounds {   // no native Metal analogue — accepted + no-op (doc §6.3)
    bool  enable   = false;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
  };

  // --- clears ---------------------------------------------------------------
  struct MtlClearValue {   // was VkClearValue (union). Kept as a plain struct.
    MtlColor4 color   = { };
    float     depth   = 1.0f;
    uint32_t  stencil = 0;
  };

  // --- draws (was VkDrawIndirectCommand / VkDrawIndexedIndirectCommand) -----
  struct MtlDrawArgs {          // mutable EmitCsCmd block — layout is load-bearing
    uint32_t vertexCount   = 0;
    uint32_t instanceCount = 0;
    uint32_t firstVertex   = 0;
    uint32_t firstInstance = 0;
  };

  struct MtlDrawIndexedArgs {
    uint32_t indexCount    = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex    = 0;
    int32_t  vertexOffset  = 0;
    uint32_t firstInstance = 0;
  };

  // --- copies / resolves ----------------------------------------------------
  enum class MtlResolveMode : uint32_t {   // was VkResolveModeFlagBits
    None       = 0,
    SampleZero = 1,
    Average    = 2,
    Min        = 3,
    Max        = 4,
  };

  struct MtlResolveRegion {   // was VkImageResolve
    MtlSubresourceLayers dstSubresource = { };
    MtlOrigin            dstOffset      = { };
    MtlSubresourceLayers srcSubresource = { };
    MtlOrigin            srcOffset      = { };
    MtlSize              extent         = { };
  };

  // --- barriers (Metal 4 explicit; Metal 3 auto-tracks -> impl no-ops) ------
  // Replaces VkPipelineStageFlags / VkAccessFlags at the barrier interface points
  // (doc 01 contract 3b). Scope bits map to MTL4VisibilityOptions / render-encoder
  // memoryBarrierWithScope: on the Metal-4 bridge.
  using MtlBarrierScope = uint32_t;
  constexpr MtlBarrierScope MtlScopeBuffers    = 1u << 0;
  constexpr MtlBarrierScope MtlScopeTextures   = 1u << 1;
  constexpr MtlBarrierScope MtlScopeRenderTgt  = 1u << 2;

  // --- render targets -------------------------------------------------------
  using MtlFeedbackLoopMask = MtlAspectMask;   // RT-as-texture hazard aspects

  // --- resource usage (was VkImageUsageFlags / VkBufferUsageFlags) ----------
  using MtlTextureUsage = uint32_t;   // MTLTextureUsage bits
  constexpr MtlTextureUsage MtlTextureUsageShaderRead   = 1u << 0;
  constexpr MtlTextureUsage MtlTextureUsageShaderWrite  = 1u << 1;
  constexpr MtlTextureUsage MtlTextureUsageRenderTarget = 1u << 2;
  constexpr MtlTextureUsage MtlTextureUsagePixelView    = 1u << 3;

  // --- present / submit / sync tokens --------------------------------------
  // PHASE-C: real payloads. Present acquire/release semaphores (contract 3e) and
  // command-buffer submit status (contract 3i) are deadlock-class — wire fully in
  // the swapchain REWRITE track.
  struct MtlPresenterSync {
    MtlObjectHandle acquire = MtlNullHandle;   // drawable wait
    MtlObjectHandle release = MtlNullHandle;   // drawable signal
    uint64_t        frameId = 0;
  };

  struct MtlSubmitStatus {
    uint64_t submissionId = 0;
    bool     completed    = false;
  };


  // --- device factory surface types (mtl_device.h) --------------------------

  enum class MtlUsage : uint32_t {
    None          = 0u,
    VertexBuffer  = 1u << 0,
    IndexBuffer   = 1u << 1,
    UniformBuffer = 1u << 2,
    Staging       = 1u << 3,
    Sampled       = 1u << 4,
    RenderTarget  = 1u << 5,
    DepthStencil  = 1u << 6,
  };

  inline MtlUsage operator | (MtlUsage a, MtlUsage b) {
    return MtlUsage(uint32_t(a) | uint32_t(b));
  }
  inline bool operator & (MtlUsage a, MtlUsage b) {
    return (uint32_t(a) & uint32_t(b)) != 0u;
  }

  enum class MtlCpuAccess : uint32_t {
    None  = 0u,        // GPU-private
    Write = 1u,        // shared, write-combined (upload)
    Read  = 2u,        // shared, cached (readback)
  };

  enum class MtlImageType : uint32_t {
    e1D = 0u,
    e2D = 1u,
    e3D = 2u,
  };

  // Was VkImageViewType — the sampler-slot view dimensionality recorded by shader
  // analysis (d3d9_shader_analysis) and consumed when binding textures. Values
  // mirror the Vk enum so the D3D9 decode logic copies unchanged.
  enum class MtlTextureViewType : uint32_t {
    e1D        = 0u,
    e2D        = 1u,
    e3D        = 2u,
    eCube      = 3u,
    e1DArray   = 4u,
    e2DArray   = 5u,
    eCubeArray = 6u,
  };

  struct MtlExtent3D {
    uint32_t width  = 0u;
    uint32_t height = 0u;
    uint32_t depth  = 1u;
  };

  enum class MtlSamplerMipMode : uint32_t {  // MTLSamplerMipFilter
    None    = 0u,
    Nearest = 1u,
    Linear  = 2u,
  };

  enum class MtlBorderColor : uint32_t {     // MTLSamplerBorderColor
    TransparentBlack = 0u,
    OpaqueBlack      = 1u,
    OpaqueWhite      = 2u,
  };


  // --- format metadata (was DxvkFormatInfo) ----------------------------------
  enum MtlFormatFlag : uint32_t {
    MtlFormatFlagNone       = 0u,
    MtlFormatFlagCompressed = 1u << 0,   // BC block formats
    MtlFormatFlagDepth      = 1u << 1,
    MtlFormatFlagStencil    = 1u << 2,
    MtlFormatFlagPacked     = 1u << 3,
  };

  struct MtlFormatInfo {
    uint32_t elementSize = 0u;   // bytes per texel (or per block if compressed)
    uint32_t blockWidth  = 1u;   // texels per block
    uint32_t blockHeight = 1u;
    uint32_t flags       = MtlFormatFlagNone;
    MtlAspectMask aspects = MtlAspectColor;
  };

  // Defined in mtl_format.cpp; never returns nullptr (Invalid yields zeros).
  const MtlFormatInfo* lookupFormatInfo(MtlFormat format);

  struct MtlStagingBufferStats {
    uint64_t allocated = 0u;   // lifetime bytes allocated
    uint64_t used      = 0u;   // bytes in flight
  };

  // PHASE-C7: PCI vendor IDs the D3D9 adapter reports (was DxvkGpuVendor,
  // dxvk_adapter.h). Pure identity values, no Vulkan. The frontend advertises a
  // synthetic GPU (AMD RX 6700 XT by default) to games, so these are the values
  // it compares/reports against, not a real Metal-device property.
  enum class MtlGpuVendor : uint32_t {
    Amd    = 0x1002,
    Nvidia = 0x10de,
    Intel  = 0x8086,
  };

  struct MtlDeviceInfo {
    char         name[256]        = { };
    uint64_t     maxBufferLength  = 0u;
    uint32_t     maxTextureSize2D = 16384u;
    uint32_t     maxAnisotropy    = 16u;
    bool         supportsBCFormats = true;
    // Synthetic identity the adapter reports (backend fills or leaves default).
    MtlGpuVendor vendor           = MtlGpuVendor::Amd;
    uint32_t     deviceId         = 0x73df;   // RX 6700 XT
    uint32_t     maxSampleCount   = 8u;        // MSAA cap (color+depth, 2D)
    float        maxPointSize     = 256.0f;
  };

}
