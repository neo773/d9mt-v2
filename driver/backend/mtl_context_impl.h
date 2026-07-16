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
 *    (vertex, fragment), each bound at its own stage via
 *    setArgumentTable:atStages:. Metal snapshots an argument table's contents
 *    at each ENCODED draw (MTL4RenderCommandEncoder.h, setArgumentTable:
 *    atStages:), so one table pair is safely mutated between draws within an
 *    encoder; the pair rotates once per materialized pass (retire-by-serial
 *    ring), NOT per draw — rotation only protects a pair from being rewritten
 *    while an earlier submission may still be encoding against it.
 *    Textures/samplers bind via setTexture(gpuResourceID)/setSamplerState.
 *    ALL of these per-draw writes (table slots, dynamic state, the draw)
 *    are appended as wmtcmd_mtl4_* structs into a CmdChunk and replayed
 *    unix-side in ONE MTL4RenderCommandEncoder_encodeCommands crossing per
 *    pass (or per arena cap) — never one thunk call per operation. Batching
 *    only moves the writes into the stream; the rotation contract holds
 *    because the replay writes table N's slots before the draw that binds
 *    table N, and N is not reused until its serial retires.
 *    Table writes are per-slot dirty-diffed: argument tables are persistent
 *    objects, so a rotated-in table still holds whatever was last written to
 *    it — each pair carries a shadow of its contents (ArgTableShadow) and
 *    only slots whose value differs are re-appended. Because the pair stays
 *    bound for the whole pass, the shadow carries forward across draws and
 *    the diff writes only the slots that actually changed since the previous
 *    draw. Draws with NO binding-shaped dirty state skip table writes
 *    entirely (prepareDraw gate).
 *
 * 4. Residency + keep-alive. Residency lives entirely in the device's
 *    MtlResidencyTracker (mtl_residency.h): every driver-created allocation
 *    joins the ONE queue-attached set at creation and leaves via the
 *    serial-deferred destruction queue; drawables ride the CAMetalLayer's
 *    own residency set (attached by the presenter). The context only
 *    drives the frame hooks — beforeSubmit() in submit(), serialRetired()
 *    in endFrame(). track() holds an Rc until submit; every tracked
 *    resource's trackingSerial is stamped at submit.
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

    /* Backend-only wiring (not part of the seam): the swapchain REWRITE hands
     * the context its presenter so synchronizeWsi can drive scan-out. */
    void setPresenter(Rc<MtlPresenter> presenter) { m_presenter = std::move(presenter); }

  private:

    /* Argument-table slot map (verified against v2/air air_builder buffer
     * index assignment, decision 3). Per stage, the AIR shader reads D3D9
     * constant buffers at Metal buffer indices 0..2 (cF / clip|alphaTest /
     * point|misc) and pulls vertex stage-in from index 3+. VS and PS each own
     * their own argument table per draw so their 0..2 CBV spaces never
     * collide. Vertex streams bind at kVertexBufferBaseIndex + stream.
     * Textures/samplers use the per-stage table's setTexture/setSamplerState
     * resource-id slots. */
    static constexpr uint32_t kVertexBufferBaseIndex = 3u;
    /* Provisional per-draw push-constant buffer index (top of the 0..30 Metal
     * buffer table, clear of CBVs 0..2 and streams 3..18). */
    static constexpr uint32_t kPushConstantIndex = 20u;
    /* Argument-buffer binding model (MtlShaderFunction::abWords non-empty):
     * the stage's textures/samplers/CBVs are 8-byte handle words in ONE ring
     * slice bound at this table index — flushResources writes the words
     * CPU-side and appends a single setAddress instead of per-slot calls. */
    static constexpr uint32_t kAbIndex = 21u;
    /* Bytes reserved at the front of each stage's push buffer for the shared
     * D3D9SharedPushData block (matches DXVK MaxSharedPushDataSize). Per-stage
     * push blocks (vs/ffvs/ffps) start immediately after. */
    static constexpr uint32_t kPushSharedBlockSize = 32u;
    /* Mid-pass wmtcmd flush cap: bounds the PE-side arena (CmdChunk blocks
     * never shrink). Flushing at a command boundary is order-preserving, so
     * the cap is purely a memory knob, not a correctness one. 4 MiB: the
     * PE->unix crossing costs ~14-17µs, so fewer, larger replays win — the
     * AB swap also shrank per-draw struct volume, so this now covers most
     * passes in one crossing. */
    static constexpr size_t kCmdChunkFlushBytes = 1u << 22;

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
      /* High watermark of push bytes EVER written on this stage. The upload
       * copies [0, pushExtent), not [0, pushDirtyHi): a partial update (e.g.
       * only the shared alphaRef block, bytes 0..16) must still republish the
       * per-stage block at kPushSharedBlockSize+ — a shorter fresh slice
       * would leave the shader's cFSize/pointSize reads past the slice end. */
      uint32_t pushExtent = 0;
      uint64_t pushGpuAddress = 0u;
      /* Per-slot dirty bitmasks (bit i = slot i touched since the last
       * flushResources). The coarse DirtyResources bit says "walk the
       * bindings"; these say WHICH slots to visit — the frontend touches
       * ~1-2 slots per draw, so visiting all 16+16+8 slots per stage per
       * draw was the dominant CPU cost (appendTable* ~97k calls/frame).
       * Set by bindTexture/bindSampler/bindUniformBuffer(Range) and by
       * invalidateBuffer's CBV retarget; consumed (cleared) by
       * flushResources. Iterating only set bits is correct ONLY while the
       * bound argument-table pair persists (its ArgTableShadow mirrors the
       * table, and untouched slots already hold their correct value from a
       * prior write to that same physical table). Whenever a DIFFERENT
       * physical pair rotates in (prepareDraw's !m_argTablesBound branch)
       * every mask is forced to ~0 so the fresh pair is fully reconciled;
       * a PSO change forces the CBV walk wide (shader-derived cbvSlotMap
       * remaps d3d9 slot -> AIR slot, so an untouched CBV may need a write
       * at a NEW table index). Start ~0: first flush reconciles everything. */
      uint32_t dirtyTextures = ~0u;   // 16 texture slots
      uint32_t dirtySamplers = ~0u;   // 16 sampler slots
      uint32_t dirtyCbvs     = ~0u;   // 8 CBV slots
      /* Argument-buffer path: gpuAddress of the stage's current handle-word
       * slice (0 = none built yet). Rebuilt by flushResources whenever any
       * binding mask is dirty or the SHADER changed (the word layout is
       * shader-derived); re-appended cheaply (shadow-diffed) otherwise. */
      uint64_t abGpuAddress = 0u;
      /* Persistent CPU copy of the stage's handle words. Rebuilds patch only
       * the words whose (kind,reg) is dirty, then memcpy the whole shadow
       * into the fresh ring slice — ring memory recycles, so it can never be
       * read back to carry clean words forward. abFnId (shader library
       * cookie) detects word-LAYOUT changes, which force a full recompute. */
      std::vector<uint64_t> abShadowWords;
      uint64_t abFnId = 0u;
      /* Which binding model consumed the dirty masks last (0 = per-slot,
       * 1 = argument buffer). The two paths clear the same masks, so a
       * model switch (FF ubershader <-> translated shader) must force a
       * full reconcile or the incoming path misses changes the outgoing
       * one consumed. */
      uint8_t abActive = 0u;
    };

    /* Shadow of one argument table's PERSISTENT contents — the value the
     * replayed stream last wrote to each slot. Tables are created with
     * initialize_bindings=1, so a zero-initialized shadow exactly mirrors a
     * fresh table. flushVertexBuffers/flushResources diff each desired slot
     * value against this and append ONLY slots that differ.
     *
     * Correctness is value-based: the table stores raw gpuAddress /
     * MTLResourceID values, and a skipped write leaves the table holding the
     * exact value we would have written — the GPU dereferences the same
     * address/id either way, so a slot the shader reads always holds the
     * correct resource at draw time, whether written this draw or surviving
     * from a prior write to this same physical table. This also subsumes the
     * unbound-slot clear invariant (a stale binding must never leak into a
     * later draw that reads that slot): null is a value like any other — a
     * slot that WAS non-null and is now unbound diffs to a 0-write; a slot
     * that is already null is not re-cleared.
     *
     * The shadow is exact only because (a) these tables are written
     * EXCLUSIVELY through the appendTable* helpers (the blit path has its own
     * pool), and (b) every appended write is replayed (flushCmdChunk) before
     * any draw that follows it in the stream — the chunk is never dropped
     * unreplayed. */
    struct ArgTableShadow {
      std::array<uint64_t, 31> bufferGpuAddress = { };
      std::array<uint64_t, 16> textureId = { };
      std::array<uint64_t, 16> samplerId = { };
    };

    struct ArgTablePair {
      std::array<WmtArgTable, 2>    table;
      std::array<ArgTableShadow, 2> shadow;  // [vertex, fragment] table contents
      uint64_t serial = 0u;   // submission serial of this pair's last use
    };

    struct VertexStream {
      MtlBufferSlice slice;
      uint32_t stride = 0;
    };

    /* Meta-blit argument table (blitTextureView): one buffer (BlitParams) +
     * one texture + one sampler, in a DEDICATED ring so a blit never advances
     * or rewrites the per-draw D3D9 table pool under its in-flight draws.
     * Same retire-by-serial discipline as ArgTablePair. */
    struct BlitArgTable {
      WmtArgTable table;
      uint64_t serial = 0u;   // submission serial of this table's last use
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

    /* Per-pass wmtcmd batch (mtl_cmd_chunk.h): the draw hot path appends
     * wmtcmd_mtl4_* structs here instead of crossing the PE->unix boundary
     * per encoder call; flushCmdChunk() replays the batch into m_pass.encoder
     * in ONE MTL4RenderCommandEncoder_encodeCommands crossing. INVARIANT:
     * non-empty only while a pass is open — flushed at closePass, at the
     * arena cap (prepareDraw), and before any DIRECT call on the pass
     * encoder (emitGraphicsBarrier), so stream and direct ops stay ordered. */
    CmdChunk           m_cmdChunk;

    /* 3-deep MTL4CommandAllocator ring: rotate per frame, reset the slot we
     * are about to reuse (>= imageCount frames old, so GPU-idle). */
    std::array<WmtAllocator, 3> m_allocators;
    std::array<uint64_t, 3>     m_allocatorSerial = { };  // last serial submitted per slot
    uint32_t     m_frameIndex   = 0u;
    uint32_t     m_allocatorSlot = 0u; // ring slot of m_allocator
    obj_handle_t m_allocator    = 0u;  // current frame's allocator (borrowed)

    /* Per-PASS argument tables. Metal snapshots an argument table's contents
     * at each encoded draw (MTL4RenderCommandEncoder.h), so mutating ONE pair
     * between draws on the same encoder is well-defined. The ring's
     * retire-by-serial discipline protects the real hazard: a pair must not
     * be rewritten while an earlier SUBMISSION'S encoding may still reference
     * it. Rotated once per materialized pass, never per draw — the pair's
     * ArgTableShadow then persists across the pass's draws so the per-slot
     * diff (appendTable*) writes only slots that actually change. */
    std::vector<ArgTablePair> m_argTables;
    uint32_t                 m_argTableCursor = 0u;

    /* Per-draw trim state (perf):
     * - m_lastStampSerial: the GPU-use stamps (setTrackingSerial) for the
     *   bound vertex/index buffers are already == m_currentSerial unless the
     *   serial moved or a stream/index (re)bind happened — skip the whole
     *   stamp loop on the (common) unchanged draw.
     * - m_lastEncodedPso: skip appending a setPSO command when the resolved
     *   PSO is the one already encoded on this encoder.
     * - m_dynValid + shadows: fresh encoders have no dynamic state, so the
     *   first flush appends everything; afterwards only pieces that changed
     *   are re-appended. All reset at materializePass (new encoder). */
    uint64_t     m_lastStampSerial = ~0ull;
    obj_handle_t m_lastEncodedPso  = 0u;
    bool         m_dynValid        = false;
    MtlViewport  m_lastViewport    = {};
    MtlRect      m_lastScissor     = {};
    obj_handle_t m_lastDsso        = 0u;
    uint32_t     m_lastStencilRef  = 0u;
    MtlColor4    m_lastBlendColor  = {};

    /* DIAG (observers): draws whose bound slice snapshot diverged from the
     * buffer's live storage at flush time — the recorded-vs-live retargeting
     * bug class; must stay 0 with in-order renames + refresh. */
    uint64_t s_diagVbStale  = 0u;
    uint64_t s_diagCbvStale = 0u;
    uint32_t                 m_currentArgTable = 0u;
    /* setArgumentTable has been encoded on the CURRENT pass encoder for
     * m_currentArgTable and the pair's serial is stamped for the current
     * cmdbuf. Cleared whenever a fresh encoder opens (materializePass) so
     * the first draw of each pass rotates + binds exactly once. */
    bool                     m_argTablesBound = false;

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
    /* Per-stream dirty bitmask, same contract as StageBindings::dirty*:
     * set by bindVertexBuffer(Range) / invalidateBuffer's stream retarget,
     * consumed by flushVertexBuffers, forced ~0 on table rotation. */
    uint32_t                     m_dirtyVertexStreams = ~0u;
    MtlBufferSlice m_indexBuffer;
    MtlIndexType   m_indexType = { };

    RenderPsoCache m_psoCache;

    // keep-alive until submit (moved into tracker.retireAfter at flush)
    std::vector<Rc<MtlResource>> m_keepAlive;

    uint64_t m_currentSerial = 0;

    // per-context visibility-result buffer for occlusion queries (decision 7).
    WmtBuffer m_visBuffer;
    void*     m_visCpu   = nullptr;
    uint64_t  m_visSlot  = 0u;         // next free 8-byte counter slot

    /* Meta-blit pipeline (decision 8 / §6.6): build-time metallib
     * (mtl_blit_metallib.h), lazily wrapped at first blitTextureView. PSOs
     * ride m_psoCache (keyed by the blit library cookies + dst format);
     * samplers ride the device sampler cache ([0]=Nearest, [1]=Linear). */
    Rc<MtlShaderFunction>      m_blitVs, m_blitFs;
    Rc<MtlShaderFunction>      m_fillFs;   /* solid-color variant (clearImageView) */
    Rc<MtlShaderFunction>      m_depthClearVs; /* depth-writing variant (scissored DS clears) */
    std::array<Rc<MtlSamplerState>, 2> m_blitSamplers;
    bool                       m_blitInitFailed = false;
    std::vector<BlitArgTable>  m_blitArgTables;
    uint32_t                   m_blitArgCursor = 0u;
    void clearDepthStencilRect(const Rc<MtlTextureView>& view, MtlOrigin offset,
                               MtlSize extent, MtlAspectMask aspects, MtlClearValue value);

    // --- core internal flow -------------------------------------------------
    void ensureCommandBuffer();        // begin MTL4 cmdbuf on the current allocator
    void materializePass();            // open MTL4 render encoder, fold clears
    void flushPendingClears();         // materialize folded clears with no draw
    void closePass();                  // flush wmtcmd batch, endEncoding + release
    void flushCmdChunk();              // replay + recycle the batched wmtcmd stream

    /* wmtcmd appenders for the argument-table hot path (one struct each;
     * table handle rides in the command — MTL4ArgumentTable is not an
     * encoder object, so table writes and draws share one ordered stream).
     * Each diffs against the table's shadow and appends only on change; the
     * shadow is updated at append time, which matches table contents at draw
     * time because the stream replays writes before the draws that follow
     * them (ArgTableShadow contract). */
    void appendTableAddress(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuAddress, uint32_t index);
    void appendTableTexture(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    void appendTableSampler(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    bool prepareDraw();                // ensurePass + resolve + flush dirty steps
    bool resolvePso();                 // build key from folded state, bind PSO

    /* PSO input snapshot (mtld3d's pipeline_state.rs trick, adapted): a
     * cheap identity of everything the RenderPsoKey is derived FROM. A
     * 4-entry MRU of (snapshot -> pso) short-circuits the 600-byte key
     * build + FNV + map probe for the A<->B material alternation that
     * dominates DirtyPso draws. Generation counters stand in for deep
     * vector/array contents; every setter that mutates key inputs bumps
     * its counter. D9MT_NO_PSOMRU=1 disables. */
    struct PsoSnapshot {
      const void* vs = nullptr;      /* shader identity (device-lifetime objects) */
      const void* fs = nullptr;
      uint32_t colorFmt[4] = { };    /* attachment formats BY VALUE (0 = unbound) —
                                      * never pointers; a recycled view address
                                      * with a different format would false-hit */
      uint32_t layoutGen = 0u, strideGen = 0u, blendGen = 0u;
      uint32_t msaaPacked = 0u, topoClass = 0u, pad = 0u;
    };
    PsoSnapshot  m_psoMruKey[4] = { };
    obj_handle_t m_psoMruPso[4] = { };
    uint32_t     m_psoMruCursor = 0u;
    uint32_t     m_layoutGen = 0u, m_strideGen = 0u, m_blendGen = 0u;
    ArgTablePair createArgumentTablePair() const;
    void selectArgumentTablesForPass();
    void bindArgumentTables();         // setArgumentTable per stage on the encoder
    void flushVertexBuffers();         // setAddress vertex streams into VS table
    void flushResources();             // CBVs/textures/samplers/push -> arg tables + ring
    void flushDynamicState();          // viewport/scissor/dsso/blend/raster setters
    void submit(bool waitCompleted);   // endCommandBuffer, commit, signalEvent, retire

    obj_handle_t computeEncoder();     // close pass, open a one-shot MTL4 compute encoder
    void         ensureVisBuffer();

    bool         ensureBlitPipeline(); // lazy meta-blit shader/sampler init
    obj_handle_t selectBlitArgTable(); // retired-or-fresh table from the blit ring

    /* Metal 4 has no driver hazard tracking (SOURCE_OF_TRUTH §4c). Emit a
     * PRODUCER queue barrier on the OUTGOING encoder before endEncoding: its
     * `afterStages` writes are ordered ahead of ALL later queue stages, so a
     * subsequent encoder (draw vertex-fetch, RT sample, present/readback copy)
     * observes them. Coarse-but-correct; narrow the stage masks later. */
    void         emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages);

  };

}
