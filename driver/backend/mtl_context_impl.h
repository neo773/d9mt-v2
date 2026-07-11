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
 * 3. Snapshot binding (the bug-2 kill). Per-draw uniform data (D3D9
 *    constants via bindUniformBufferRange offsets), push data, and any
 *    argument-table block are written into FRESH UploadRing slices at draw
 *    time. Encoded commands reference ring memory that is never rewritten;
 *    ring chunks recycle only after the submission completes. Textures/
 *    samplers bind as direct wmtcmd setTexture/setSampler slots (flat
 *    D3D9 slot model — no descriptor indirection on Metal 3).
 *
 * 4. Residency + keep-alive. track() holds an Rc until submit; every bound
 *    resource's trackingSerial is stamped at submit. A flat open-addressing
 *    dedup set (v1's proven ~1.2ms win) gates useResource emission.
 *
 * 5. Submission spine. flushCommandList(): close encoder, encodeSignalEvent
 *    via SubmissionTracker::onSubmit, commit, ring.onSubmit(serial), move
 *    keep-alive set into tracker.retireAfter(serial, ...). endFrame():
 *    processRetirements + ring.onComplete(completedSerial). synchronizeWsi:
 *    presenter acquire/present + frame-latency signal (contract 3e — wired
 *    end-to-end from day one; never a stub).
 *
 * 6. invalidateBuffer (contract 3a): the incoming Rc<MtlAllocation> (the
 *    OLD backing) goes straight into tracker.retireAfter(currentSerial).
 *    Nothing else — the frontend already rebound the new backing.
 *
 * 7. Queries (contract 3d): occlusion = per-pass visibility-result buffer
 *    slots (proven v2 design); EVENT/timestamp = submission-serial compare.
 *    All answers derive from SubmissionTracker; no bespoke fences.
 *
 * 8. Copies/blits: blit encoder via wmtcmd blit stream; blitTextureView /
 *    generateMipmaps(filtered) go through the meta-blit render pipeline
 *    (build-time metallib, §6.6); resolveTexture full-subresource via pass
 *    store-action resolve, partial = resolve-to-scratch + copy (§6.5).
 *
 * 9. PSO creation goes through the BRIDGE's vertex-descriptor entry
 *    (MTLDevice_newRenderPipelineStateWithVertexDescriptor, code 153).
 *    max_tessellation_factor MUST be >= 1 (Metal validation; we set 64).
 */

#include <array>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_presenter.h"

#include "mtl_cmd_chunk.h"
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

    /* Argument-table slot map (verified against v2/air air_render_test.mm +
     * airconv manifest, decision 3). The AIR shaders read D3D9 constant
     * buffers at Metal buffer indices 0..2 (cF / clip|alphaTest / point|misc)
     * and pull vertex stage-in from index 3+. So vertex streams bind at
     * kVertexBufferBaseIndex + streamIndex, never colliding with the CBV
     * slots. Textures use the fragment texture table; samplers/vertex-texture
     * fetch need the argument-buffer TLAB path this per-slot ABI lacks
     * (documented no-op below). */
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
      obj_handle_t encoder = 0;        // open MTLRenderCommandEncoder
      uint64_t encodedDraws = 0;       // watchdog budget accounting
    };

    Rc<MtlDeviceImpl>  m_device;
    Rc<MtlPresenter>   m_presenter;    // set via synchronizeWsi plumbing

    WmtCommandBuffer   m_cmdbuf;       // open command buffer
    CmdChunk           m_chunk;        // wmtcmd stream for the open encoder
    PendingPass        m_pass;

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

    // cached MTLFunction handles keyed by owning library (avoids per-draw
    // newFunction churn; released on context teardown).
    std::unordered_map<obj_handle_t, WmtFunction> m_fnCache;

    // per-context visibility-result buffer for occlusion queries (decision 7).
    WmtBuffer m_visBuffer;
    void*     m_visCpu   = nullptr;
    uint64_t  m_visSlot  = 0u;         // next free 8-byte counter slot

    // --- core internal flow -------------------------------------------------
    void ensureCommandBuffer();
    void materializePass();            // open encoder, fold clears into load actions
    void closePass();                  // flush chunk -> encodeCommands -> endEncoding
    bool prepareDraw();                // ensurePass + resolve + flush dirty steps
    bool resolvePso();                 // build key from folded state, memo lookup
    void flushVertexBuffers();
    void flushResources();             // textures/samplers/CBVs -> wmtcmd sets + ring snapshots
    void flushDynamicState();
    void submit(bool present);         // tracker.onSubmit, ring.onSubmit, retirements
    void makeResident(obj_handle_t handle);

    obj_handle_t resolveFunction(const Rc<MtlShaderFunction>& fn);
    uint32_t     currentDepthFormat() const;   // WMTPixelFormat of the pass depth view
    void         ensureVisBuffer();
    void         encodeBlit(const wmtcmd_base* head);   // close pass, run one blit chunk

  };

}
