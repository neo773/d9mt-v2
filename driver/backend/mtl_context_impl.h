#pragma once
/*
 * MtlContextImpl — the Metal backend behind the MtlContext seam.
 *
 * ARCHITECTURE (the load-bearing decisions; bodies follow from here):
 *
 * 1. Deferred pass model. bindRenderTargets() only records a pending pass;
 *    the encoder materializes lazily at the first draw/clear that needs it
 *    (materializePass()). Pending clears fold into the pass's loadActions
 *    when possible (clear-then-draw = free on TBDR), else flush as their
 *    own mini-pass. Offscreen passes split into their own command buffer
 *    when total encoded work crosses the watchdog budget (the v2 GPU-hang
 *    fix, generalized: split BEFORE the drawable is acquired, never after).
 *
 * 2. Draw-time resolve. draw() runs: ensurePass() -> resolvePso() ->
 *    flushBinding() -> flushDynamicState() -> encode draw into the CmdChunk
 *    (wmtcmd stream). Dirty bits gate each step:
 *      DirtyPso | DirtyVertexBuffers | DirtyResources | DirtyPush |
 *      DirtyViewport | DirtyDepthStencil | DirtyBlendColor | DirtyStencilRef
 *    PSO key folds {vs,fs cookie, vertex layout + strides, color formats +
 *    blend, topology class, sample count [, depth format while Metal 3]}.
 *    Same-key fast path = one memo compare (RenderPsoCache).
 *
 * 3. Snapshot binding (the bug-2 kill), METAL 4 argument tables. Per-draw
 *    uniform data (D3D9 constants via bindUniformBufferRange offsets) and
 *    push data are written into FRESH UploadRing slices at draw time; their
 *    gpuAddress is written into an MTL4ArgumentTable via setAddress. The
 *    table reads the address at the draw snapshot, so ring memory is never
 *    rewritten under a queued draw (exactly the Metal-3 chunk semantics).
 *    D3D9's separate VS/PS constant namespaces map to TWO argument tables
 *    (m_argTable[vertex], m_argTable[fragment]), each bound at its own stage
 *    via setArgumentTable:atStages: — this preserves the setVertexBuffer /
 *    setFragmentBuffer split that a single shared table would collide.
 *    Textures/samplers bind via setTexture(gpuResourceID)/setSamplerState.
 *
 * 4. Residency + keep-alive. track() holds an Rc until submit; every bound
 *    resource's trackingSerial is stamped at submit. ONE MTLResidencySet per
 *    context: bound resources addAllocation (flat dedup gate); commit +
 *    requestResidency before queue commit; cmdbuf useResidencySet. The set
 *    rebuilds at a frame boundary only once the prior epoch's serial retires
 *    (never evict an in-flight allocation).
 *
 * 5. Submission spine. beginRecording rotates a 3-deep MTL4CommandAllocator
 *    ring + resets the caught-up slot. submit(): endCommandBuffer, queue
 *    commit([cmdbuf]) (batched), queue signalEvent(serial) via
 *    SubmissionTracker::onSubmit, ring.onSubmit(serial), move keep-alive into
 *    tracker.retireAfter(serial). endFrame(): processRetirements +
 *    ring.onComplete(completedSerial). synchronizeWsi: queue waitForDrawable
 *    -> commit -> queue signalDrawable -> drawable present (contract 3e).
 *
 * 6. invalidateBuffer (contract 3a): the incoming Rc<MtlAllocation> (the
 *    OLD backing) goes straight into tracker.retireAfter(currentSerial).
 *    Nothing else — the frontend already rebound the new backing.
 *
 * 7. Queries (contract 3d): occlusion = per-pass visibility-result buffer
 *    slots (proven v2 design); EVENT/timestamp = submission-serial compare.
 *    All answers derive from SubmissionTracker; no bespoke fences.
 *
 * 8. Copies/blits: MTL4ComputeCommandEncoder copy* (Metal 4 has NO blit
 *    encoder); blitTextureView / generateMipmaps(filtered) go through the
 *    meta-blit render pipeline (build-time metallib, §6.6); resolveTexture
 *    full-subresource via pass store-action resolve (§6.5).
 *
 * 9. PSO creation goes through MTL4Compiler_newRenderPipelineState with a
 *    WMTMtl4RenderPipelineInfo: vertex/fragment LIBRARY handle + entry name
 *    ("main"), colors[] format+blend, sample count, vertex descriptor from
 *    the PsoVertex* structs. NO depth/stencil format (Metal 4 omits it from
 *    the pipeline descriptor), NO tessellation fields.
 */

#include <array>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_presenter.h"

#include "mtl_device_impl.h"
#include "mtl_pso_cache.h"

namespace dxvk::backend {

  /* Backend query object. Occlusion answers come from a visibility-result
   * buffer slot the context assigns at beginQuery; timestamp/event answers
   * come from the submission serial the query was recorded under (decision 7).
   * The frontend holds it as Rc<MtlQuery>; MtlDeviceImpl::createGpuQuery makes
   * the subclass so beginQuery can downcast. */
  class MtlBackendQuery : public MtlQuery {
  public:
    obj_handle_t visBuffer   = 0u;    /* shared visibility buffer (borrowed) */
    void*        visCpu      = nullptr;
    uint64_t     slotOffset  = 0u;    /* byte offset of this query's counter */
    uint64_t     serial      = 0u;    /* submission the query completes under */
    SubmissionTracker* tracker = nullptr;

    MtlGpuQueryStatus getData(MtlQueryData& out) override;
  };

  /* Backend GPU event / D3DQUERYTYPE_EVENT — serial compare (decision 7). */
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
    DirtyResources     = 1u << 3,   // textures/samplers/CBVs
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

    // --- MtlContext surface overrides --------------------------------------
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

    void draw(uint32_t count, const MtlDrawArgs* args) override;
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) override;

    // --- part 2: transfers / init / queries / lifetime ---------------------
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

    /* Backend-only wiring (not part of the seam): the swapchain REWRITE hands
     * the context its presenter so synchronizeWsi can drive scan-out. */
    void setPresenter(Rc<MtlPresenter> presenter) { m_presenter = std::move(presenter); }

  private:

    /* Argument-table slot map (verified against v2/air air_builder buffer
     * index assignment, decision 3). Per stage, the AIR shader reads D3D9
     * constant buffers at Metal buffer indices 0..2 (cF / clip|alphaTest /
     * point|misc) and pulls vertex stage-in from index 3+. VS and PS each own
     * their own argument table (m_argTable[stage]) so their 0..2 CBV spaces
     * never collide. Vertex streams bind at kVertexBufferBaseIndex + stream.
     * Textures/samplers use the per-stage table's setTexture/setSamplerState
     * resource-id slots. */
    static constexpr uint32_t kVertexBufferBaseIndex = 3u;
    /* Provisional per-draw push-constant buffer index (top of the 0..30 Metal
     * buffer table, clear of CBVs 0..2 and streams 3..18). */
    static constexpr uint32_t kPushConstantIndex = 20u;

    // --- per-stage bound state (flat D3D9 slot model) ----------------------
    struct StageBindings {
      std::array<Rc<MtlTextureView>,  16> textures;
      std::array<Rc<MtlSamplerState>, 16> samplers;
      struct CbvBinding {
        MtlBufferSlice slice;
        MtlDeviceSize  rangeOffset = 0;   // bindUniformBufferRange
        MtlDeviceSize  rangeLength = 0;
      };
      std::array<CbvBinding, 8> cbvs;
      std::array<uint8_t, 4096> pushData;
      uint32_t pushDirtyLo = 0, pushDirtyHi = 0;
    };

    struct VertexStream {
      MtlBufferSlice slice;
      uint32_t stride = 0;
    };

    struct PendingPass {
      MtlRenderTargets targets;
      bool active = false;             // encoder currently open
      bool pending = false;            // targets bound, encoder not yet open
      /* fold-in clears: per-attachment pending clear values */
      std::array<bool, 4> clearColor = { };
      std::array<MtlClearValue, 4> clearValues = { };
      bool clearDepth = false, clearStencil = false;
      float depthClear = 1.f; uint32_t stencilClear = 0;
      obj_handle_t encoder = 0;        // open MTL4RenderCommandEncoder
      uint64_t encodedDraws = 0;       // watchdog budget accounting
    };

    Rc<MtlDeviceImpl>  m_device;
    Rc<MtlPresenter>   m_presenter;    // set via synchronizeWsi plumbing

    WmtCommandBuffer   m_cmdbuf;       // open MTL4 command buffer (fresh/recording)
    PendingPass        m_pass;

    /* 3-deep MTL4CommandAllocator ring: rotate per frame, reset the slot we
     * are about to reuse (>= imageCount frames old, so GPU-idle). */
    std::array<WmtAllocator, 3> m_allocators;
    uint32_t     m_frameIndex   = 0u;
    obj_handle_t m_allocator    = 0u;  // current frame's allocator (borrowed)

    /* ONE residency set per context; per-stage argument tables (decision 3). */
    WmtResidencySet             m_residencySet;
    std::array<WmtArgTable, 2>  m_argTable;   // [vertex, fragment]
    uint64_t     m_residencyEpochSerial = 0u; // max serial that used the set

    uint32_t           m_dirty = DirtyEverything;

    // pipeline-fold state
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

    std::array<StageBindings, 2> m_bindings;   // [vertex, fragment]
    std::array<VertexStream, 16> m_vertexStreams;
    MtlBufferSlice m_indexBuffer;
    MtlIndexType   m_indexType = { };

    RenderPsoCache m_psoCache;

    // keep-alive until submit (moved into tracker.retireAfter at flush)
    std::vector<Rc<MtlResource>> m_keepAlive;

    // flat residency dedup (v1 ResidentSet pattern)
    std::vector<obj_handle_t> m_residentSet;

    uint64_t m_currentSerial = 0;

    // per-context visibility-result buffer for occlusion queries (decision 7).
    WmtBuffer m_visBuffer;
    void*     m_visCpu   = nullptr;
    uint64_t  m_visSlot  = 0u;         // next free 8-byte counter slot

    // --- core internal flow -------------------------------------------------
    void ensureCommandBuffer();        // rotate allocator, begin MTL4 cmdbuf, useResidencySet
    void materializePass();            // open MTL4 render encoder, fold clears
    void closePass();                  // endEncoding + release encoder
    bool prepareDraw();                // ensurePass + resolve + flush dirty steps
    bool resolvePso();                 // build key from folded state, bind PSO
    void bindArgumentTables();         // setArgumentTable per stage on the encoder
    void flushVertexBuffers();         // setAddress vertex streams into VS table
    void flushResources();             // CBVs/textures/samplers/push -> arg tables + ring
    void flushDynamicState();          // viewport/scissor/dsso/blend/raster setters
    void submit(bool waitCompleted);   // endCommandBuffer, commit, signalEvent, retire
    void makeResident(obj_handle_t handle);
    void residentTextureView(const Rc<MtlTextureView>& view);

    obj_handle_t computeEncoder();     // close pass, open a one-shot MTL4 compute encoder
    void         ensureVisBuffer();

    /* Metal 4 has no driver hazard tracking (SOURCE_OF_TRUTH §4c). Emit a
     * PRODUCER queue barrier on the OUTGOING encoder before endEncoding: its
     * `afterStages` writes are ordered ahead of ALL later queue stages, so a
     * subsequent encoder (draw vertex-fetch, RT sample, present/readback copy)
     * observes them. Coarse-but-correct; narrow the stage masks later. */
    void         emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages);

  };

}
