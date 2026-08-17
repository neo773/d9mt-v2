#pragma once

#include <array>
#include <cstdlib>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_presenter.h"

#include "mtl_cmd_chunk.h"
#include "mtl_device_impl.h"
#include "mtl_pso_cache.h"

namespace dxvk::backend {

  inline bool deferredPassesEnabled() {
    static const bool on = std::getenv("D9MT_DEFERRED") != nullptr;
    return on;
  }

  class MtlBackendQuery : public MtlQuery {
  public:
    obj_handle_t visBuffer   = 0u;
    void*        visCpu      = nullptr;
    uint64_t     slotOffset  = 0u;
    uint64_t     serial      = 0u;
    SubmissionTracker* tracker = nullptr;

    MtlGpuQueryStatus getData(MtlQueryData& out) override;
  };

  class MtlBackendEvent : public MtlEvent {
  public:
    uint64_t     serial  = 0u;
    SubmissionTracker* tracker = nullptr;

    MtlGpuEventStatus test() override;
  };

  enum ContextDirtyBit : uint32_t {
    DirtyPso           = 1u << 0,
    DirtyVertexBuffers = 1u << 1,
    DirtyIndexBuffer   = 1u << 2,
    DirtyResources     = 1u << 3,
    DirtyPush          = 1u << 4,
    DirtyViewport      = 1u << 5,
    DirtyDepthStencil  = 1u << 6,
    DirtyBlendColor    = 1u << 7,
    DirtyStencilRef    = 1u << 8,
    DirtyDepthBias     = 1u << 9,
    DirtyRasterizer    = 1u << 10,
    DirtyEverything    = ~0u,
  };

  class MtlContextImpl : public MtlContext {

  public:

    explicit MtlContextImpl(Rc<MtlDeviceImpl> device);
    ~MtlContextImpl();

    void beginRecording() override;
    void endFrame() override;
    void flushCommandList(const char* reason, MtlSubmitStatus* status) override;
    void synchronizeWsi(MtlPresenterSync sync) override;

    void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) override;
    void setVertexLayout(uint32_t nAttr, const MtlVertexAttr* attrs,
                         uint32_t nLayout, const MtlVertexLayout* layouts) override;
    void setPrimitiveTopology(MtlPrimitiveTopology topology) override;
    void setViewports(uint32_t n, const MtlViewport* viewports) override;
    void setRasterizerState(const MtlRasterizerState& rs) override;
    void setMultisampleState(const MtlMultisampleState& ms) override;
    void setDepthStencilState(const MtlDepthStencilState& ds) override;
    void setBlendMode(uint32_t rt, const MtlBlendMode& blend) override;
    void setBlendColor(MtlColor4 color) override;
    void setStencilReference(uint32_t reference) override;
    void setDepthBias(MtlDepthBias bias) override;
    void setDepthBounds(MtlDepthBounds bounds) override;

    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) override;
    void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) override;
    void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) override;
    void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) override;
    void bindUniformBufferRange(MtlStageMask stages, uint32_t slot,
                                MtlDeviceSize offset, MtlDeviceSize length) override;
    void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) override;
    void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) override;
    void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) override;

    void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) override;
    void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope) override;
    void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard) override;
    void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value) override;

    void draw(uint32_t count, const MtlDrawArgs* args) override;
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) override;

    void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) override;
    void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format) override;
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter) override;
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil) override;
    void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter) override;

    void initBuffer(const Rc<MtlBuffer>& buffer) override;
    void initImage(const Rc<MtlTexture>& image) override;
    void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) override;
    void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) override;
    bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage) override;

    void beginQuery(const Rc<MtlQuery>& query) override;
    void endQuery(const Rc<MtlQuery>& query) override;
    void writeTimestamp(const Rc<MtlQuery>& query) override;

    void signal(const Rc<MtlSignal>& signal, uint64_t value) override;
    void signalGpuEvent(const Rc<MtlEvent>& event) override;

    void track(Rc<MtlResource>&& resource) override;

    void beginDebugLabel(const char* label) override;
    void endDebugLabel() override;
    void insertDebugLabel(const char* label) override;

    void setPresenter(Rc<MtlPresenter> presenter) { m_presenter = std::move(presenter); }

  private:

    static constexpr uint32_t kVertexBufferBaseIndex = 3u;
    static constexpr uint32_t kPushConstantIndex = 20u;
    static constexpr uint32_t kAbIndex = 21u;
    static constexpr uint32_t kPushSharedBlockSize = 32u;
    static constexpr size_t kCmdChunkFlushBytes = 1u << 22;

    struct StageBindings {
      std::array<Rc<MtlTextureView>,  16> textures;
      std::array<Rc<MtlSamplerState>, 16> samplers;
      struct CbvBinding {
        MtlBufferSlice slice;
        MtlDeviceSize  rangeOffset = 0;
        MtlDeviceSize  rangeLength = 0;
      };
      std::array<CbvBinding, 8> cbvs;
      std::array<uint8_t, 4096> pushData;
      uint32_t pushDirtyLo = 0, pushDirtyHi = 0;
      uint32_t pushExtent = 0;
      uint64_t pushGpuAddress = 0u;
      uint32_t dirtyTextures = ~0u;
      uint32_t dirtySamplers = ~0u;
      uint32_t dirtyCbvs     = ~0u;
      uint64_t abGpuAddress = 0u;
      std::vector<uint64_t> abShadowWords;
      uint64_t abFnId = 0u;
      uint8_t abActive = 0u;
    };

    struct ArgTableShadow {
      std::array<uint64_t, 31> bufferGpuAddress = { };
      std::array<uint64_t, 16> textureId = { };
      std::array<uint64_t, 16> samplerId = { };
    };

    struct ArgTablePair {
      std::array<WmtArgTable, 2>    table;
      std::array<ArgTableShadow, 2> shadow;
      uint64_t serial = 0u;
    };

    struct VertexStream {
      MtlBufferSlice slice;
      uint32_t stride = 0;
    };

    struct BlitArgTable {
      WmtArgTable table;
      uint64_t serial = 0u;
    };

    struct PendingPass {
      MtlRenderTargets targets;
      bool active = false;
      bool pending = false;
      std::array<bool, 4> clearColor = { };
      std::array<MtlClearValue, 4> clearValues = { };
      bool clearDepth = false, clearStencil = false;
      float depthClear = 1.f; uint32_t stencilClear = 0;
      obj_handle_t encoder = 0;
      uint64_t encodedDraws = 0;
    };

    struct FrameEntry {
      FrameEntry() = default;
      // Moves must stay noexcept: std::function's move ctor isn't noexcept
      // under C++17, so an implicit move here would push vector growth onto
      // the copy path, deep-copying a parked CmdChunk's freed-block chain.
      FrameEntry(FrameEntry&&) noexcept = default;
      FrameEntry& operator = (FrameEntry&&) noexcept = default;

      enum class Kind : uint8_t { Render, Direct };
      Kind kind = Kind::Render;

      MtlRenderTargets targets;
      std::array<bool, 4> clearColor = { };
      std::array<MtlClearValue, 4> clearValues = { };
      bool clearDepth = false, clearStencil = false;
      float depthClear = 1.f;
      uint32_t stencilClear = 0u;
      uint64_t draws = 0u;
      std::vector<CmdChunk> chunks;
      std::array<uint8_t, 4> colorStore = { };
      uint8_t depthStore = 0u, stencilStore = 0u;

      std::function<void(obj_handle_t cmdbuf)> encode;
    };

    Rc<MtlDeviceImpl>  m_device;
    Rc<MtlPresenter>   m_presenter;

    WmtCommandBuffer   m_cmdbuf;
    PendingPass        m_pass;

    CmdChunk           m_cmdChunk;

    std::vector<FrameEntry> m_frameEntries;
    std::vector<CmdChunk>   m_chunkPool;
    std::unordered_set<const void*> m_frameSampled;
    std::unordered_set<const void*> m_everTransferRead;
    bool m_frameBoundaryFlush = false;

    std::array<WmtAllocator, 3> m_allocators;
    std::array<uint64_t, 3>     m_allocatorSerial = { };
    uint32_t     m_frameIndex   = 0u;
    uint32_t     m_allocatorSlot = 0u;
    obj_handle_t m_allocator    = 0u;

    std::vector<ArgTablePair> m_argTables;
    uint32_t                 m_argTableCursor = 0u;

    uint64_t     m_lastStampSerial = ~0ull;
    obj_handle_t m_lastEncodedPso  = 0u;
    bool         m_dynValid        = false;
    MtlViewport  m_lastViewport    = {};
    MtlRect      m_lastScissor     = {};
    obj_handle_t m_lastDsso        = 0u;
    uint32_t     m_lastStencilRef  = 0u;
    MtlColor4    m_lastBlendColor  = {};

    uint64_t s_diagVbStale  = 0u;
    uint64_t s_diagCbvStale = 0u;
    uint32_t                 m_currentArgTable = 0u;
    bool                     m_argTablesBound = false;

    uint32_t           m_dirty = DirtyEverything;

    Rc<MtlShaderFunction> m_vs, m_fs;
    std::vector<MtlVertexAttr>   m_vertexAttrs;
    std::vector<MtlVertexLayout> m_vertexLayouts;
    MtlPrimitiveTopology m_topology = { };
    std::array<MtlBlendMode, 4> m_blend = { };
    MtlMultisampleState m_msaa = { };
    MtlRasterizerState  m_rasterizer = { };
    MtlDepthStencilState m_depthStencil = { };
    MtlViewport m_viewport = { };
    MtlColor4   m_blendColor = { };
    uint32_t    m_stencilRef = 0;
    MtlDepthBias m_depthBias = { };

    std::array<StageBindings, 2> m_bindings;
    std::array<VertexStream, 16> m_vertexStreams;
    uint32_t                     m_dirtyVertexStreams = ~0u;
    MtlBufferSlice m_indexBuffer;
    MtlIndexType   m_indexType = { };

    RenderPsoCache m_psoCache;

    std::vector<Rc<MtlResource>> m_keepAlive;

    uint64_t m_currentSerial = 0;

    WmtBuffer m_visBuffer;
    void*     m_visCpu   = nullptr;
    uint64_t  m_visSlot  = 0u;

    Rc<MtlShaderFunction>      m_blitVs, m_blitFs;
    Rc<MtlShaderFunction>      m_fillFs;
    Rc<MtlShaderFunction>      m_depthClearVs;
    std::array<Rc<MtlSamplerState>, 2> m_blitSamplers;
    bool                       m_blitInitFailed = false;
    std::vector<BlitArgTable>  m_blitArgTables;
    uint32_t                   m_blitArgCursor = 0u;

    // Metal has no fan primitive: one shared index buffer holds the fan
    // pattern, and a fan of N vertices consumes its first 3*(N-2) indices.
    static constexpr uint32_t  kFanMaxVerts = 16386u;
    Rc<MtlBuffer>              m_fanIndices;
    bool ensureFanIndices();

    bool presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) override;
    std::map<std::array<uint32_t, 6>, obj_handle_t> m_fxScalers;
    bool m_fxChecked = false, m_fxUnsupported = false;
    void clearDepthStencilRect(const Rc<MtlTextureView>& view, MtlOrigin offset,
                               MtlSize extent, MtlAspectMask aspects, MtlClearValue value);

    void ensureCommandBuffer();
    void materializePass();
    void flushPendingClears();
    void closePass();
    void flushCmdChunk();

    void recordPassEntry();
    CmdChunk takeChunk();
    void prepareDirectEncode(uint32_t passCloseCounter);
    void encodeDirect(std::function<void(obj_handle_t)>&& fn);
    void noteTransferRead(const MtlTexture* tex);
    void flushFramePasses();

    void appendTableAddress(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuAddress, uint32_t index);
    void appendTableTexture(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    void appendTableSampler(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    bool prepareDraw();
    bool resolvePso();

    struct PsoSnapshot {
      const void* vs = nullptr;
      const void* fs = nullptr;
      uint32_t colorFmt[4] = { };
      uint32_t layoutGen = 0u, strideGen = 0u, blendGen = 0u;
      uint32_t msaaPacked = 0u, topoClass = 0u, pad = 0u;
    };
    PsoSnapshot  m_psoMruKey[4] = { };
    obj_handle_t m_psoMruPso[4] = { };
    uint32_t     m_psoMruCursor = 0u;
    uint32_t     m_layoutGen = 0u, m_strideGen = 0u, m_blendGen = 0u;
    ArgTablePair createArgumentTablePair() const;
    void selectArgumentTablesForPass();
    void bindArgumentTables();
    void flushVertexBuffers();
    void flushResources();
    void flushDynamicState();
    void submit(bool waitCompleted);

    void         ensureVisBuffer();

    bool         ensureBlitPipeline();
    obj_handle_t selectBlitArgTable();

    // Metal 4 has no driver hazard tracking: a producer barrier on the
    // outgoing encoder orders its writes ahead of all later queue stages so
    // a subsequent encoder observes them.
    void         emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages);

  };

}
