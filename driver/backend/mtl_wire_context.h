#pragma once
/*
 * MtlWireContext — Phase 1 (echo mode) of the native-backend transport
 * (docs/NATIVE_BACKEND_PLAN.md).
 *
 * Wraps the real MtlContextImpl and implements the full MtlContext seam by
 * forwarding EVERY call to the inner context (behavior unchanged) and, for
 * the HOT-LOOP subset (binds / state setters / clears / draws — see
 * driver/mtl/mtl_wire.h), ALSO serializing the call as a POD record into a
 * PE-allocated SPSC ring drained by the unix-side consumer thread
 * (bridge/unix/winemetal_unix.c), which decodes/validates/counts and
 * discards. This proves ring/atomics/kick-park under a real game at full
 * draw rate with zero behavioral risk. Cold calls (creates, copies, blits,
 * queries, init/upload, frame lifecycle) forward only — no records.
 *
 * The wrapper is ALWAYS installed (MtlDeviceImpl::createContext) — the
 * transport is default behavior of this branch, not env-gated; a broken
 * transport is reverted via git, not flags.
 *
 * Threading: all MtlContext calls arrive on the CS thread (single
 * producer). Only the FIRST MtlWireContext in the process claims the ring
 * (multi-device games create several contexts); later instances forward
 * without serializing.
 *
 * Phase 2 recompiles the hot-loop state machine natively against these
 * records; the Metal objects are already unix-side behind obj_handle_t, so
 * no resource-ownership migration is needed.
 */

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_wire.h"

namespace dxvk::backend {

  class MtlWireContext final : public MtlContext {

  public:

    MtlWireContext(const Rc<MtlDevice>& device, Rc<MtlContext>&& inner);
    ~MtlWireContext();

    /* --- lifecycle / submission (cold: forward only) ----------------------- */
    void beginRecording() override;
    void endFrame() override;
    void flushCommandList(const char* reason, MtlSubmitStatus* status) override;
    Rc<MtlCommandBuffer> beginExternalRendering() override;
    void synchronizeWsi(MtlPresenterSync sync) override;
    void beginLatencyTracking(const Rc<MtlLatencyTracker>& tracker, uint64_t frameId) override;
    void endLatencyTracking(const Rc<MtlLatencyTracker>& tracker) override;

    /* --- shaders & pipeline state (hot: forward + serialize) --------------- */
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

    /* --- slot-based argument-table binding (hot) ---------------------------- */
    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) override;
    void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) override;
    void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) override;
    void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) override;
    void bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length) override;
    void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) override;
    void bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view) override;
    void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) override;
    void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) override;

    /* --- render targets / barriers (bindRenderTargets hot) ------------------ */
    void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) override;
    void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope) override;

    /* --- draws (hot) --------------------------------------------------------- */
    void draw(uint32_t count, const MtlDrawArgs* args) override;
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) override;

    /* --- clears (clearRenderTarget hot) --------------------------------------- */
    void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard) override;
    void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value) override;

    /* --- copies / blits / resolves / mips (cold: forward only) ---------------- */
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
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil) override;
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter) override;
    bool presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) override;
    void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter) override;
    void transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope) override;

    /* --- init / upload / discard (cold) ---------------------------------------- */
    void initBuffer(const Rc<MtlBuffer>& buffer) override;
    void initImage(const Rc<MtlTexture>& image) override;
    void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) override;
    void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) override;
    bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage) override;

    /* --- queries (cold) ---------------------------------------------------------- */
    void beginQuery(const Rc<MtlQuery>& query) override;
    void endQuery(const Rc<MtlQuery>& query) override;
    void writeTimestamp(const Rc<MtlQuery>& query) override;

    /* --- signals / events (cold) ---------------------------------------------------- */
    void signal(const Rc<MtlSignal>& signal, uint64_t value) override;
    void signalGpuEvent(const Rc<MtlEvent>& event) override;

    /* --- lifetime / residency (cold) ---------------------------------------------------- */
    void track(Rc<MtlResource>&& resource) override;

    /* --- debug labels (cold) --------------------------------------------------------------- */
    void beginDebugLabel(const char* label) override;
    void endDebugLabel() override;
    void insertDebugLabel(const char* label) override;

  private:

    Rc<MtlContext> m_inner;

    /* ring (only the claiming instance has these non-null) */
    uint8_t*        m_ringMem = nullptr;
    d9mt_wire_ring* m_ring    = nullptr;
    uint8_t*        m_data    = nullptr;
    uint64_t        m_cap     = 0u;
    uint64_t        m_head    = 0u;    /* local producer cursor mirror */
    bool            m_active  = false; /* serializing */
    bool            m_kick    = false; /* empty->nonempty seen for the pending record */
    uint32_t        m_pending = 0u;    /* bytes reserved by beginRecord */

    /* Reserves size = sizeof(T) + trailing, 8-aligned, contiguous, zeroed,
     * with hdr.opcode/size filled. Returns nullptr when the transport is
     * inactive (caller skips serialization). endRecord() publishes + kicks. */
    template<typename T>
    T* beginRecord(uint16_t op, uint32_t trailing = 0u);
    void* reserveBytes(uint16_t op, uint32_t bytes);
    void endRecord();
    bool waitForSpace(uint64_t need);

    static void fillBufferRef(d9mt_wire_buffer_ref& dst, const MtlBufferSlice& slice);
    static void fillRtRef(d9mt_wire_rt_ref& dst, const Rc<MtlTextureView>& view);
  };

}
