#pragma once
/*
 * MtlWireContext — THIN RECORDER (ALL-IN cutover; docs/NATIVE_BACKEND_PLAN.md
 * phase 2, server/HANDOFF-ALLIN.md).
 *
 * The SERVER is the sole renderer. Hot-loop calls (binds / state setters /
 * draws / clears / eligible copies-blits-resolves-mips) SERIALIZE ONLY as POD
 * records into a PE-allocated SPSC ring drained by the render server — they
 * no longer forward to the inner MtlContextImpl. Dual execution is dead;
 * rollback is git revert (pre-ALL-IN commits), not a flag.
 *
 * m_inner survives as a RESOURCE-AND-PRESENT provider. The kept forwards,
 * each with its one-line WHY:
 *  - beginRecording/endFrame/flushCommandList/synchronizeWsi(+latency):
 *    frame lifecycle — the CS loop, allocator pacing, submission spine and
 *    the Metal 4 present handshake still run client-side (synchronizeWsi
 *    forwards only with a real drawable; a null drawable = skip-present,
 *    marker-only).
 *  - beginExternalRendering: external-overlay seam rides the local queue.
 *  - copyTexture/blitTextureView (pseudo-resource sides only) +
 *    presentUpscale: THE present blit — drawable dst / server-IOSurface src
 *    cannot cross the wire; eligible<->eligible ops serialize only.
 *  - resolveTexture/generateMipmaps (pseudo-resource sides only): same rule.
 *  - copyTextureToBuffer: the READBACK executor (dump harness reads the
 *    wrapped server IOSurface through it); rendered-RT sources are stale
 *    client-side and are counted+named (rbStale, HANDOFF debt).
 *  - copyBuffer/copyBufferToTexture/uploadBuffer: upload machinery — staging
 *    bytes must still land in the local backings/mirrors that the heap
 *    snapshots, shadow re-binds and Lock reads consume.
 *  - initBuffer/initImage/invalidateBuffer/ensureTextureUsage: resource
 *    allocation — backing creation/rename/usage promotion must happen at the
 *    CS position (wire-id assignment + mapPtr staging + info mirroring hang
 *    off the same objects).
 *  - writeTimestamp/signal/signalGpuEvent/track: completion spine — the
 *    frontend's staging/submission fences and EVENT queries ride LOCAL
 *    submission serials; a signal that never fires deadlocks the frontend.
 *    (beginQuery/endQuery do NOT forward: occlusion is stubbed all-passed —
 *    the draws it would count execute on the server; see HANDOFF-ALLIN.)
 *  - debug labels: annotate the residual local command buffers; free.
 *
 * The wrapper is ALWAYS installed (MtlDeviceImpl::createContext) — the
 * transport is default behavior of this branch, not env-gated; a broken
 * transport is reverted via git, not flags.
 *
 * Threading: all MtlContext calls arrive on the CS thread (single
 * producer). One MtlWireContext at a time claims the ring (concurrent
 * contexts forward without serializing); destruction RELEASES the claim so
 * the D3D9 boot pattern — throwaway caps device destroyed before the real
 * device — re-attaches with a fresh server instead of killing the wire for
 * the process (S3 v3).
 */

#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_wire.h"

namespace dxvk::backend {

  class MtlWireContext final : public MtlContext {

  public:

    MtlWireContext(const Rc<MtlDevice>& device, Rc<MtlContext>&& inner);
    ~MtlWireContext();

    /* --- lifecycle / submission (KEPT forwards: frame/present machinery) --- */
    void beginRecording() override;
    void endFrame() override;
    void flushCommandList(const char* reason, MtlSubmitStatus* status) override;
    Rc<MtlCommandBuffer> beginExternalRendering() override;
    void synchronizeWsi(MtlPresenterSync sync) override;
    void beginLatencyTracking(const Rc<MtlLatencyTracker>& tracker, uint64_t frameId) override;
    void endLatencyTracking(const Rc<MtlLatencyTracker>& tracker) override;

    /* --- shaders & pipeline state (hot: serialize only) --------------------- */
    void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) override;
    void setFunctionConstants(MtlPipeline pipeline, uint32_t index, uint32_t count, const void* data) override;
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

    /* --- slot-based argument-table binding (hot: serialize only) ------------ */
    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) override;
    void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) override;
    void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) override;
    void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) override;
    void bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length) override;
    void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) override;
    void bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view) override;
    void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) override;
    void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) override;

    /* --- render targets / barriers (hot: serialize only) -------------------- */
    void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) override;
    void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope) override;

    /* --- draws (hot: serialize only) ------------------------------------------ */
    void draw(uint32_t count, const MtlDrawArgs* args) override;
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) override;

    /* --- clears (hot: serialize only) ------------------------------------------ */
    void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard) override;
    void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value) override;

    /* --- copies / blits / resolves / mips (serialize when wire-eligible;
     *     forward only for pseudo-resource sides = present machinery,
     *     plus copyTextureToBuffer = the readback executor) ---------------- */
    void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) override;
    void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void noteUnshippedTextureConversion(const Rc<MtlTexture>& dst,
                                        uint32_t conversionType) override;
    void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format) override;
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil) override;
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter) override;
    bool presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) override;
    void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter) override;
    void transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope) override;

    /* --- init / upload / discard (KEPT forwards: resource allocation +
     *     upload machinery) ---------------------------------------------------- */
    void initBuffer(const Rc<MtlBuffer>& buffer) override;
    void initImage(const Rc<MtlTexture>& image) override;
    void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) override;
    void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) override;
    void noteBufferWrite(const Rc<MtlAllocation>& storage,
                         MtlDeviceSize offset, MtlDeviceSize length) override;
    bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage) override;

    /* --- queries (occlusion begin/end: STUBBED all-passed, no forward;
     *     writeTimestamp: kept — local submission spine) --------------------- */
    void beginQuery(const Rc<MtlQuery>& query) override;
    void endQuery(const Rc<MtlQuery>& query) override;
    void writeTimestamp(const Rc<MtlQuery>& query) override;

    /* --- signals / events (KEPT forwards: frontend completion spine) ---------- */
    void signal(const Rc<MtlSignal>& signal, uint64_t value) override;
    void signalGpuEvent(const Rc<MtlEvent>& event) override;

    /* --- lifetime / residency (KEPT forward: submission keep-alive) ------------- */
    void track(Rc<MtlResource>&& resource) override;

    /* --- debug labels (KEPT forwards: residual local command buffers) ------------ */
    void beginDebugLabel(const char* label) override;
    void endDebugLabel() override;
    void insertDebugLabel(const char* label) override;

  private:

    Rc<MtlContext> m_inner;

    /* S3 v5: the TRANSPORT (ring + heap regions, producer cursors, server
     * session) is PROCESS-LIFETIME state — file-scope in the .cpp (struct
     * WireTransport g_wt) — because D3D9 device lifetimes are chaotic
     * (throwaway boot devices, overlapping lifetimes). A context only holds
     * its CLAIM state: whether it is the current producer, or is waiting to
     * become one. Context death QUIESCES the server (state reset record);
     * the server itself survives until the client process dies. */
    bool m_claimed = false;   /* this context is the current wire producer */
    /* Constructed while the claim was HELD (overlapping device lifetimes).
     * Retries the claim once per present (tryLateClaim); cleared by the
     * first full connect/attach attempt — claim-held is the ONLY retryable
     * state, transport failures stay session-permanent. */
    bool m_wantClaim = false;

    /* S3 rebind shadows: which LOGICAL buffer sits in each wire-visible
     * binding, so invalidateBuffer (a Discard rename, which the frontend
     * never follows with a re-bind) can emit refreshed bind records — the
     * server otherwise keeps drawing from the retired backing (S2 flag). */
    struct BufShadow {
      Rc<MtlBuffer> buffer;
      uint64_t      offset = 0u;
      uint64_t      length = 0u;
      uint32_t      stride = 0u; /* vertex streams */
      uint32_t      type   = 0u; /* index buffer: MtlIndexType */
      bool          dirty  = false; /* UBO: (re)bound since the last draw-time
                                     * content snapshot (S3 v13) */
      /* V15 re-ship dedup: the exact range+content last shipped from this
       * shadow. A rebind of the same slice with unchanged bytes (fnv64) is
       * heap traffic the server already holds — skip it. Only updated when
       * the emit actually landed (heapAlloc can drop on a full heap). */
      uint64_t      lastShipWire = 0u;
      uint64_t      lastShipOff  = 0u;
      uint64_t      lastShipLen  = 0u;
      uint64_t      lastShipSum  = 0u;
    };
    BufShadow m_streamShadow[16];
    BufShadow m_uboShadow[2][8];
    BufShadow m_indexShadow;

    /* S3 v13 THE ZERO-PIXEL-DRAW FIX: host-visible UBO content (the D3D9
     * constant-buffer ring — SetVertexShaderConstantF lands there) never
     * crossed the wire. On UMA the frontend writes constants through mapPtr
     * with NO uploadBuffer call (D3D9ConstantBuffer::SetupStreamCommand
     * early-outs when !m_useDma), so the server kept the create-time snapshot
     * (zeros) forever -> WVP = 0 -> every vertex collapses -> draws rasterize
     * nothing. Draw emission is the sound tap: the frontend writes constants
     * (app thread) BEFORE queueing the draw, so the CS thread executing the
     * draw observes the final bytes. snapshotDirtyUniforms() re-ships every
     * UBO range (re)bound since the last draw. */
    void snapshotDirtyUniforms();

    /* S3 v13 observability: per-draw input checksums (draws #0-3 + a late
     * burst #240-243 to defeat server-side blit lag) + the draw-#1 vertex-
     * layout dump; the server logs the identical format from ITS resolved
     * state for eyeball diffing. Shipped-content sums live FILE-SCOPE
     * (g_uploadSums, process-lifetime like the transport) so device churn
     * cannot lose them. */
    void logWireDraw();
    uint32_t m_dbgDrawLog = 0u;
    /* client mirror of the per-stage push block (what the server's
     * push_data decode materializes), for pushsum parity logging. */
    uint8_t  m_pushShadow[2][4096] = {};
    uint32_t m_pushShadowExtent[2] = {};
    std::vector<MtlVertexAttr>   m_attrShadow;
    std::vector<MtlVertexLayout> m_layoutShadow;

    /* S3 v2 lazy re-upload: texture content whose heap region could not be
     * allocated (heap full during a boot flood) is stashed here — the bytes
     * would otherwise be lost forever (staging recycles). Retried at the
     * image's next bind_texture_by_id and once per present. V18: crossing
     * the SOFT threshold (kMaxPendingUploadBytes) first engages a bounded
     * ack-wait (heapAllocWait); exact same-box re-uploads replace their
     * stashed predecessor; only the HARD cap drops — counted + named
     * (PROF texDrop=), because a dropped upload is a permanently black
     * server texture. */
    struct PendingTexUpload {
      Rc<MtlTexture>              image;
      d9mt_wire_rec_upload_texture rec; /* heap fields refreshed at retry */
      std::vector<uint8_t>        bytes;
    };
    std::vector<PendingTexUpload> m_pendingTexUploads;
    uint64_t                      m_pendingTexBytes = 0u;

    /* Reserves size = sizeof(T) + trailing, 8-aligned, contiguous, zeroed,
     * with hdr.opcode/size filled. Returns nullptr when the transport is
     * inactive (caller skips serialization). endRecord() publishes + kicks. */
    template<typename T>
    T* beginRecord(uint16_t op, uint32_t trailing = 0u);
    void* reserveBytes(uint16_t op, uint32_t bytes);
    void endRecord();
    bool waitForSpace(uint64_t need);

    /* --- S3 v5: claim lifecycle (claim must be HELD by the caller) ---------
     * attachWire = FIRST attach in the process: region alloc + spawn +
     * V4-verified handshake. connectWire = attach to the ALREADY-RUNNING
     * process-lifetime server: liveness check + session-begin record.
     * tryLateClaim runs once per present while m_wantClaim: CAS the claim;
     * on success run ONE connect/attach attempt and replay the shadowed
     * bindings (the frontend never re-binds state it set before the wire
     * existed). */
    bool attachWire();
    bool connectWire();
    void acquireWire();
    void tryLateClaim();
    void emitShadowBinds(const MtlBuffer* only);
    bool wired() const; /* this context is the producer AND the transport lives */

    /* --- S2 cold-record emission (CS thread) -------------------------------
     * Lazy create-at-first-wire-use: each ensure* checks the resource's
     * wireId (0 = not on the wire yet), emits the create record and assigns
     * the id — so a create always precedes the first record referencing the
     * resource, with no cross-thread ring producers. Destroys arrive from
     * arbitrary-thread destructors via wire::queueDestroy (mtl_wire_emit.h)
     * and are drained at the top of reserveBytes. */
    bool     ensureBufferCreated(const MtlBufferSlice& slice);
    bool     ensureStorageCreated(const Rc<MtlAllocation>& storage);
    uint64_t ensureTextureCreated(const Rc<MtlTexture>& image);
    void     ensureShaderCreated(const Rc<MtlShaderFunction>& fn);
    void     ensureSamplerCreated(const Rc<MtlSamplerState>& sampler);
    void     emitQueuedDestroys();

    /* --- S3 shared-heap producer (CS thread) --------------------------------
     * Copies `bytes` of `src` into a fresh 256-aligned heap region and
     * returns its free-running start offset (genOut = the region's END
     * cursor, the ack generation). NEVER stalls: a full heap (server behind
     * on acks) drops the upload (heap->dropped++) and returns UINT64_MAX —
     * the server keeps its previous content for that resource. */
    uint64_t heapAlloc(const void* src, uint64_t bytes, uint64_t* genOut,
                       bool quiet = false);
    /* V18: bounded ack-wait twin of heapAlloc — TEXTURE content only (one-
     * shot bytes; a drop is a permanently black server texture). Engaged
     * when the lazy stash would cross its soft threshold. */
    uint64_t heapAllocWait(const void* src, uint64_t bytes, uint64_t* genOut);
    /* Returns true when the record actually shipped (false = full-heap drop
     * or unclaimed wire) — V15 re-ship dedup keys off this. */
    bool     emitUploadBuffer(uint64_t wireId, uint64_t clientHandle,
                              uint64_t dstOff, const void* src, uint64_t size);
    void     emitUploadTextureRecord(const d9mt_wire_rec_upload_texture& fields,
                                     uint64_t heapOff, uint64_t gen);
    void     retryPendingTexUploads(const MtlTexture* image);
    void     emitBindTextureById(MtlStageMask stages, uint32_t slot,
                                 const Rc<MtlTextureView>& view);

    static void fillBufferRef(d9mt_wire_buffer_ref& dst, const MtlBufferSlice& slice);
    static void fillRtRef(d9mt_wire_rt_ref& dst, const Rc<MtlTextureView>& view);
  };

}
