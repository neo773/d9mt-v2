#pragma once

#include <cstdint>

namespace dxvk {

  using MtlObjectHandle = uint64_t;
  constexpr MtlObjectHandle MtlNullHandle = 0ull;

  using MtlDeviceSize = uint64_t;

  enum class MtlShaderStage : uint32_t {
    Vertex   = 0,
    Fragment = 1,
  };

  using MtlStageMask = uint32_t;
  constexpr MtlStageMask MtlStageVertex   = 1u << 0;
  constexpr MtlStageMask MtlStageFragment = 1u << 1;

  enum class MtlPipeline : uint32_t {
    Graphics = 0,
  };

  enum class MtlIndexType : uint32_t {
    Uint16 = 0,
    Uint32 = 1,
  };

  enum class MtlPrimitiveTopology : uint32_t {
    PointList     = 0,
    LineList      = 1,
    LineStrip     = 2,
    TriangleList  = 3,
    TriangleStrip = 4,
    TriangleFan   = 5,
  };

  struct MtlInputAssemblyState {
    MtlInputAssemblyState() = default;
    MtlInputAssemblyState(MtlPrimitiveTopology t, bool restart)
    : topology(t), primitiveRestart(restart) { }
    MtlPrimitiveTopology topology         = MtlPrimitiveTopology::TriangleList;
    bool                 primitiveRestart = false;
  };

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

  enum class MtlBlendOperation : uint32_t {
    Add             = 0,
    Subtract        = 1,
    ReverseSubtract = 2,
    Min             = 3,
    Max             = 4,
  };

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

  enum class MtlCullMode : uint32_t {
    None  = 0,
    Front = 1,
    Back  = 2,
  };

  enum class MtlTriangleFillMode : uint32_t {
    Fill  = 0,
    Lines = 1,
  };

  enum class MtlWinding : uint32_t {
    Clockwise        = 0,
    CounterClockwise = 1,
  };

  enum class MtlSamplerAddressMode : uint32_t {
    ClampToEdge        = 0,
    MirrorClampToEdge  = 1,
    Repeat             = 2,
    MirrorRepeat       = 3,
    ClampToZero        = 4,
    ClampToBorderColor = 5,
  };

  enum class MtlSamplerFilter : uint32_t {
    Nearest = 0,
    Linear  = 1,
  };

  enum class MtlSamplerMipFilter : uint32_t {
    NotMipmapped = 0,
    Nearest      = 1,
    Linear       = 2,
  };

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
    // Vertex-attribute-only marker (not a real pixel format): a D3D9 FLOAT3
    // attribute must stay 3-wide rather than being promoted to Float4, or the
    // shader's w reads stale bytes instead of defaulting to 1.0 per D3D9 rules.
    RGB32Float         = 900,
    Depth16Unorm       = 250,
    Depth32Float       = 252,
    Stencil8           = 253,
    Depth24Unorm_Stencil8 = 255,
    Depth32Float_Stencil8 = 260,
    BC1_RGBA           = 130,
    BC1_RGBA_sRGB      = 131,
    BC2_RGBA           = 132,
    BC2_RGBA_sRGB      = 133,
    BC3_RGBA           = 134,
    BC3_RGBA_sRGB      = 135,
  };

  using MtlAspectMask = uint32_t;
  constexpr MtlAspectMask MtlAspectColor   = 1u << 0;
  constexpr MtlAspectMask MtlAspectDepth   = 1u << 1;
  constexpr MtlAspectMask MtlAspectStencil = 1u << 2;

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

  struct MtlOrigin {
    int32_t x = 0, y = 0, z = 0;
    bool operator == (const MtlOrigin& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator != (const MtlOrigin& o) const { return !(*this == o); }
  };

  struct MtlExtent3D;

  struct MtlSize {
    uint32_t width = 0, height = 0, depth = 0;

    MtlSize() = default;
    MtlSize(uint32_t w, uint32_t h, uint32_t d) : width(w), height(h), depth(d) { }
    MtlSize(const MtlExtent3D& e);

    bool operator == (const MtlSize& o) const {
      return width == o.width && height == o.height && depth == o.depth;
    }
    bool operator != (const MtlSize& o) const { return !(*this == o); }
  };

  struct MtlOffset3D {
    int32_t x = 0, y = 0, z = 0;
  };

  struct MtlExtent2D {
    uint32_t width = 0, height = 0;
    bool operator == (const MtlExtent2D& o) const { return width == o.width && height == o.height; }
    bool operator != (const MtlExtent2D& o) const { return !(*this == o); }
  };

  struct MtlOffset2D {
    int32_t x = 0, y = 0;
  };

  struct MtlRect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
  };

  struct MtlSubresourceLayers {
    MtlAspectMask aspects    = MtlAspectColor;
    uint32_t      mipLevel   = 0;
    uint32_t      baseLayer  = 0;
    uint32_t      layerCount = 1;
  };

  struct MtlSubresourceRange {
    MtlAspectMask aspects    = MtlAspectColor;
    uint32_t      baseMip    = 0;
    uint32_t      mipCount   = 1;
    uint32_t      baseLayer  = 0;
    uint32_t      layerCount = 1;
  };

  struct MtlViewport {
    float originX = 0.0f, originY = 0.0f;
    float width   = 0.0f, height  = 0.0f;
    float znear   = 0.0f, zfar    = 1.0f;
    MtlRect scissor = { };
  };

  struct MtlRasterizerState {
    MtlCullMode         cullMode    = MtlCullMode::None;
    MtlTriangleFillMode fillMode    = MtlTriangleFillMode::Fill;
    MtlWinding          frontFace   = MtlWinding::Clockwise;
    bool                depthClip   = true;
    bool                flatShading = false;
    uint32_t            sampleCount = 0;

    void setCullMode(MtlCullMode m)         { cullMode = m; }
    void setPolygonMode(MtlTriangleFillMode m) { fillMode = m; }
    void setFrontFace(MtlWinding w)         { frontFace = w; }
    void setDepthClip(bool e)               { depthClip = e; }
    void setFlatShading(bool e)             { flatShading = e; }
    void setSampleCount(uint32_t n)         { sampleCount = n; }
  };

  struct MtlMultisampleState {
    uint32_t sampleCount      = 1;
    uint32_t sampleMask       = 0xFFFFFFFFu;
    bool     alphaToCoverage  = false;
    bool     alphaToOne       = false;

    void setSampleCount(uint32_t n)     { sampleCount = n; }
    void setSampleMask(uint32_t m)      { sampleMask = m; }
    void setAlphaToCoverage(bool e)     { alphaToCoverage = e; }
  };

  struct MtlStencilState {
    MtlStencilOperation failOp      = MtlStencilOperation::Keep;
    MtlStencilOperation depthFailOp = MtlStencilOperation::Keep;
    MtlStencilOperation passOp      = MtlStencilOperation::Keep;
    MtlCompareFunction  compareOp   = MtlCompareFunction::Always;
    uint32_t            readMask    = 0xFFu;
    uint32_t            writeMask   = 0xFFu;

    void setFailOp(MtlStencilOperation o)      { failOp = o; }
    void setPassOp(MtlStencilOperation o)      { passOp = o; }
    void setDepthFailOp(MtlStencilOperation o) { depthFailOp = o; }
    void setCompareOp(MtlCompareFunction o)    { compareOp = o; }
    void setCompareMask(uint32_t m)            { readMask = m; }
    void setWriteMask(uint32_t m)              { writeMask = m; }
  };

  struct MtlDepthStencilState {
    MtlCompareFunction depthCompareOp = MtlCompareFunction::Always;
    bool               depthTest      = false;
    bool               depthWrite     = false;
    bool               stencilEnable  = false;
    MtlStencilState    front = { };
    MtlStencilState    back  = { };

    void setDepthTest(bool e)                { depthTest = e; }
    void setDepthWrite(bool e)               { depthWrite = e; }
    void setStencilTest(bool e)              { stencilEnable = e; }
    void setDepthCompareOp(MtlCompareFunction o) { depthCompareOp = o; }
    void setStencilOpFront(const MtlStencilState& s) { front = s; }
    void setStencilOpBack(const MtlStencilState& s)  { back = s; }
    void normalize() { }
  };

  struct MtlBlendMode {
    bool              enable        = false;
    MtlBlendFactor    colorSrc      = MtlBlendFactor::One;
    MtlBlendFactor    colorDst      = MtlBlendFactor::Zero;
    MtlBlendOperation colorOp       = MtlBlendOperation::Add;
    MtlBlendFactor    alphaSrc      = MtlBlendFactor::One;
    MtlBlendFactor    alphaDst      = MtlBlendFactor::Zero;
    MtlBlendOperation alphaOp       = MtlBlendOperation::Add;
    uint32_t          writeMask     = 0xFu;

    void setBlendEnable(bool e)          { enable = e; }
    void setWriteMask(uint32_t m)        { writeMask = m & 0xFu; }
    void setColorOp(MtlBlendFactor s, MtlBlendFactor d, MtlBlendOperation o) { colorSrc = s; colorDst = d; colorOp = o; }
    void setAlphaOp(MtlBlendFactor s, MtlBlendFactor d, MtlBlendOperation o) { alphaSrc = s; alphaDst = d; alphaOp = o; }
    MtlBlendFactor    colorSrcFactor() const { return colorSrc; }
    MtlBlendFactor    colorDstFactor() const { return colorDst; }
    MtlBlendOperation colorBlendOp()   const { return colorOp; }
    MtlBlendFactor    alphaSrcFactor() const { return alphaSrc; }
    MtlBlendFactor    alphaDstFactor() const { return alphaDst; }
    MtlBlendOperation alphaBlendOp()   const { return alphaOp; }
    void normalize() { }
  };

  struct MtlColor4 {
    union {
      struct { float r, g, b, a; };
      float    float32[4];
      uint32_t uint32[4];
      int32_t  int32[4];
    };
    MtlColor4() : r(0.0f), g(0.0f), b(0.0f), a(0.0f) { }
    MtlColor4(float R, float G, float B, float A) : r(R), g(G), b(B), a(A) { }
  };

  struct MtlDepthBias {
    float constantFactor = 0.0f;
    float slopeFactor    = 0.0f;
    float clamp          = 0.0f;
  };

  struct MtlDepthBounds {
    bool  enable   = false;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
  };

  struct MtlClearValue {
    MtlColor4 color   = { };
    float     depth   = 1.0f;
    uint32_t  stencil = 0;
  };

  struct MtlDrawArgs {
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

  enum class MtlResolveMode : uint32_t {
    None       = 0,
    SampleZero = 1,
    Average    = 2,
    Min        = 3,
    Max        = 4,
  };

  struct MtlResolveRegion {
    MtlSubresourceLayers dstSubresource = { };
    MtlOrigin            dstOffset      = { };
    MtlSubresourceLayers srcSubresource = { };
    MtlOrigin            srcOffset      = { };
    MtlSize              extent         = { };
  };

  using MtlBarrierScope = uint32_t;
  constexpr MtlBarrierScope MtlScopeBuffers    = 1u << 0;
  constexpr MtlBarrierScope MtlScopeTextures   = 1u << 1;
  constexpr MtlBarrierScope MtlScopeRenderTgt  = 1u << 2;

  using MtlFeedbackLoopMask = MtlAspectMask;

  using MtlTextureUsage = uint32_t;
  constexpr MtlTextureUsage MtlTextureUsageShaderRead   = 1u << 0;
  constexpr MtlTextureUsage MtlTextureUsageShaderWrite  = 1u << 1;
  constexpr MtlTextureUsage MtlTextureUsageRenderTarget = 1u << 2;
  constexpr MtlTextureUsage MtlTextureUsagePixelView    = 1u << 3;

  struct MtlPresenterSync {
    MtlObjectHandle acquire = MtlNullHandle;
    MtlObjectHandle release = MtlNullHandle;
    MtlObjectHandle drawable = MtlNullHandle;
    uint64_t        frameId = 0;
    double          minDuration = 0.0;
  };

  struct MtlSubmitStatus {
    uint64_t submissionId = 0;
    bool     completed    = false;
  };


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
    None  = 0u,
    Write = 1u,
    Read  = 2u,
  };

  enum class MtlImageType : uint32_t {
    e1D = 0u,
    e2D = 1u,
    e3D = 2u,
  };

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

    MtlExtent3D() = default;
    MtlExtent3D(uint32_t w, uint32_t h, uint32_t d) : width(w), height(h), depth(d) { }
    MtlExtent3D(const MtlSize& s) : width(s.width), height(s.height), depth(s.depth) { }

    bool operator == (const MtlExtent3D& o) const {
      return width == o.width && height == o.height && depth == o.depth;
    }
    bool operator != (const MtlExtent3D& o) const { return !(*this == o); }
  };

  inline MtlSize::MtlSize(const MtlExtent3D& e)
  : width(e.width), height(e.height), depth(e.depth) { }

  enum class MtlSamplerMipMode : uint32_t {
    None    = 0u,
    Nearest = 1u,
    Linear  = 2u,
  };

  enum class MtlBorderColor : uint32_t {
    TransparentBlack = 0u,
    OpaqueBlack      = 1u,
    OpaqueWhite      = 2u,
  };


  enum MtlFormatFlag : uint32_t {
    MtlFormatFlagNone       = 0u,
    MtlFormatFlagCompressed = 1u << 0,
    MtlFormatFlagDepth      = 1u << 1,
    MtlFormatFlagStencil    = 1u << 2,
    MtlFormatFlagPacked     = 1u << 3,
    MtlFormatFlagMultiPlane = 1u << 4,
  };

  struct MtlFormatPlane {
    uint32_t elementSize = 0u;
    uint32_t blockWidth  = 1u;
    uint32_t blockHeight = 1u;
  };

  struct MtlFormatInfo {
    uint32_t elementSize = 0u;
    uint32_t blockWidth  = 1u;
    uint32_t blockHeight = 1u;
    uint32_t flags       = MtlFormatFlagNone;
    MtlAspectMask aspects = MtlAspectColor;
    MtlFormatPlane planes[1] = { };
  };

  const MtlFormatInfo* lookupFormatInfo(MtlFormat format);

  struct MtlStagingBufferStats {
    uint64_t allocated = 0u;
    uint64_t used      = 0u;
  };

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
    MtlGpuVendor vendor           = MtlGpuVendor::Amd;
    uint32_t     deviceId         = 0x73df;
    uint32_t     maxSampleCount   = 8u;
    float        maxPointSize     = 256.0f;
  };

}
