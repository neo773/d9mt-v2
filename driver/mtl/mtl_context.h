#pragma once

#include <cstdint>
#include <array>
#include <vector>

#include "mtl_types.h"
#include "mtl_resource.h"
#include "mtl_device.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/sync/sync_signal.h"   // sync::Signal / sync::Fence (CPU-visible signal)

// ---------------------------------------------------------------------------
// mtl_context.h — the frontend<->backend seam.
//
// MtlContext is the drop-in replacement for DxvkContext (doc 01): every deferred
// lambda the D3D9 frontend records into a CS chunk takes `MtlContext* ctx`, and
// the CS worker replays them here. This surface — ~40 core methods reachable from
// src/d3d9/ — IS the entire backend contract. Replace DxvkContext with an
// MtlContext of the same shape and the frontend compiles unchanged.
//
// This header is the v0 sketch from doc 01 §4 AS AMENDED by the §6 open-question
// resolutions (final for v0):
//   §6.1 setViewports takes RT-pixel coordinates only (no Retina scale here).
//   §6.2 setLogicOpState — CUT (D3D9 has no logic ops).
//   §6.3 setDepthBounds — accepted + no-op (log once).
//   §6.4 vertex stride is part of the render-PSO key (stride change => PSO-dirty).
//   §6.5 resolveTexture partial region = resolve-full-then-copy (backend impl).
//   §6.6 blitTextureView scaled = internal meta-blit metallib pipeline.
//
// ZERO Vulkan. Binding is slot-based argument tables with snapshot-at-dispatch
// semantics (§1d); sync is explicit (barriers + shared events); lifetime is
// residency-set + completion-handler retirement (contract 3h).
//
// Implementations live in mtl_context_stub.cpp (log-and-return) for this phase;
// PHASE-C ports the real winemetal-backed backend.
// ---------------------------------------------------------------------------

namespace dxvk {

  // --- Rc<>-managed backend objects the seam references --------------------
  // Minimal complete definitions so things link. PHASE-C: real payloads.

  // MtlDevice (resource factory half of the seam) lives in mtl_device.h.

  // Wraps an MTL4LibraryFunctionDescriptor over our AIR metallib — no MTLFunction,
  // no SPIR-V (doc 01 §1b, doc 03 §5).
  class MtlShaderFunction : public RcObject {
  public:
    MtlShaderStage  stage = MtlShaderStage::Vertex;
    MtlObjectHandle library = MtlNullHandle;
    // AIR entry-point name inside `library`. MSC-compiled app shaders use
    // "main"; the hand-authored fixed-function ubershaders use "ffVsMain"/
    // "ffFsMain" (Metal reserves "main" for hand-written MSL). resolvePso
    // forwards this to the render-PSO builder.
    const char*     entryName = "main";
    // PHASE-C: reflection (DxvkShaderMetadata-equivalent).

    // CbvIndex -> AIR Metal buffer-slot map (SOURCE_OF_TRUTH §4b bug 1). The
    // frontend binds constant buffers at raw CbvIndex; the AIR shader reads them
    // at these Metal [[buffer(n)]] slots. -1 = the shader does not declare that
    // buffer. Index by D3D9 CbvIndex (0..7); consumed by the arg-table flush.
    // Argument-buffer shaders (abWords non-empty): the values are AB WORD
    // indices instead of Metal buffer slots.
    std::array<int8_t, 8> cbvSlotMap = { { -1, -1, -1, -1, -1, -1, -1, -1 } };

    // Argument-buffer binding model: the shader reads its resources as 8-byte
    // handle words (gpuAddress for CBVs, gpuResourceID for textures/samplers)
    // from ONE buffer bound at abTableIndex. abWords[i] describes word i; the
    // backend flush writes the words into an UploadRing slice and appends a
    // single table setAddress per stage per draw instead of per-slot calls.
    // Empty = classic per-slot binding (FF ubershaders until converted).
    enum class AbWordKind : uint8_t { Cbv = 0, Texture = 1, Sampler = 2 };
    struct AbWord {
      AbWordKind kind;
      uint8_t    reg;   /* D3D9 register: CbvIndex / t# / s# */
    };
    uint8_t             abTableIndex = 21u;
    std::vector<AbWord> abWords;
  };

  class MtlCommandBuffer : public RcObject { };   // MTL4CommandBuffer

  // --- query / event result surface (doc 05 §Queries) --------------------
  // OCCLUSION -> visibility-buffer counter; TIMESTAMP -> serial-based; EVENT ->
  // submission-serial compare. (was DxvkGpuQuery / DxvkGpuEvent + DxvkQueryData.)
  enum class MtlQueryType : uint32_t { Occlusion, Timestamp };
  enum class MtlGpuQueryStatus : uint32_t { Invalid, Pending, Available, Failed };
  enum class MtlGpuEventStatus : uint32_t { Invalid, Pending, Signaled };

  union MtlQueryData {
    struct { uint64_t samplesPassed; } occlusion;
    struct { uint64_t time; }          timestamp;
  };

  class MtlQuery : public RcObject {         // occlusion / timestamp
  public:
    MtlQueryType type = MtlQueryType::Occlusion;
    // Poll the result. PHASE-C: read the visibility/counter buffer after the
    // referencing command buffer completes; stub returns Available with zeros.
    virtual MtlGpuQueryStatus getData(MtlQueryData& out);
  };

  class MtlEvent : public RcObject {         // GPU MTLEvent / D3DQUERYTYPE_EVENT
  public:
    // PHASE-C: MTLSharedEvent value vs completed serial; stub -> Signaled.
    virtual MtlGpuEventStatus test();
  };

  // CPU-visible signal the GPU bumps on completion (was DxvkSignal = sync::Signal).
  // Aliased to the util CPU signal so the frontend's Rc<sync::Fence> staging /
  // submission fences pass straight into ctx->signal().
  using MtlSignal = sync::Signal;
  class MtlLatencyTracker: public RcObject {      // Reflex-style markers (thin)
  public:
    // Whether the frontend should emit per-draw latency markers. PHASE-E: Reflex.
    bool needsAutoMarkers() const { return false; }
  };

  // --- render-target binding set (was DxvkRenderTargets) -------------------
  struct MtlRenderTargets {
    static constexpr uint32_t MaxColorTargets = 4;
    Rc<MtlTextureView> color[MaxColorTargets] = { };
    Rc<MtlTextureView> depthStencil           = { };
  };

  /**
   * \brief Metal-first immediate-mode command recorder
   *
   * Same shape as DxvkContext; the CS worker calls these from replayed lambdas.
   */
  class MtlContext : public RcObject {

  public:

    explicit MtlContext(const Rc<MtlDevice>& device);
    virtual ~MtlContext();

    // --- lifecycle / submission (§1j, contracts 3e,3i) ---------------------
    virtual void beginRecording();
    virtual void endFrame();
    virtual void flushCommandList(const char* reason, MtlSubmitStatus* status);   // commit + new cmdbuf
    virtual Rc<MtlCommandBuffer> beginExternalRendering();                        // full state invalidate
    virtual void synchronizeWsi(MtlPresenterSync sync);                          // (!) present acquire/release
    virtual void beginLatencyTracking(const Rc<MtlLatencyTracker>&, uint64_t frameId);
    virtual void endLatencyTracking(const Rc<MtlLatencyTracker>&);

    // --- shaders & pipeline state (folds into memoized render PSO) (§1b,1c)-
    // The render PSO is resolved lazily at draw from folded state; its key
    // EXCLUDES depth/stencil attachment formats (Metal 4 defers them). PSO compile
    // is async (MTL4Compiler) — never a blocking compile on the CS thread.
    virtual void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn);
    virtual void setFunctionConstants(MtlPipeline pipeline, uint32_t index, uint32_t count, const void* data);
    virtual void setVertexLayout(uint32_t nAttr, const struct MtlVertexAttr* attrs,
                         uint32_t nLayout, const struct MtlVertexLayout* layouts);
    virtual void setPrimitiveTopology(MtlPrimitiveTopology topology);
    virtual void setViewports(uint32_t n, const MtlViewport* viewports);          // §6.1 RT-pixel coords only
    virtual void setRasterizerState(const MtlRasterizerState& rs);
    virtual void setMultisampleState(const MtlMultisampleState& ms);
    virtual void setDepthStencilState(const MtlDepthStencilState& ds);            // cached MTLDepthStencilState
    virtual void setBlendMode(uint32_t rt, const MtlBlendMode& blend);
    // §6.2 setLogicOpState — CUT (D3D9 has no logic ops; DxvkContext only had it
    //      because it also served D3D11). Intentionally absent from this surface.
    // dynamic (no PSO rebuild):
    virtual void setBlendColor(MtlColor4 color);
    virtual void setStencilReference(uint32_t reference);
    virtual void setDepthBias(MtlDepthBias bias);
    virtual void setDepthBounds(MtlDepthBounds bounds);                           // §6.3 accepted + no-op

    // --- slot-based argument-table binding (§1d, contract 3h) -------------
    // §6.4: stride is baked into the Metal vertex descriptor => part of the PSO
    //       memo key. A stride change marks the render PSO dirty; a binding change
    //       alone does not.
    // Buffer binds take PHYSICAL references: the slice's storage snapshot
    // (captured at record time) is what draws resolve — the live buffer is
    // never consulted, so an app-thread Discard rename cannot retarget
    // already-recorded draws. A rename reaches a still-bound slice only via
    // invalidateBuffer's CS-time refresh, in command order.
    virtual void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride);
    virtual void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride);
    virtual void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type);
    virtual void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice);
    virtual void bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length);
    virtual void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view);
    virtual void bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view);
    virtual void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler);
    virtual void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data); // <=4KB inline

    // --- render targets (§1e, contract 3f) ---------------------------------
    virtual void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop); // pending pass, lazy encoder
    virtual void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope);

    // --- draws (§1a) — resolve PSO/arg-tables/dynamic state, open encoder ---
    virtual void draw(uint32_t count, const MtlDrawArgs* args);
    virtual void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args);

    // --- clears (§1f) ------------------------------------------------------
    virtual void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard);
    virtual void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value);

    // --- copies / blits / resolves / mips (§1g) ----------------------------
    virtual void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    virtual void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes);
    virtual void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    virtual void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format);
    virtual void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil); // §6.5
    virtual void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter);                        // §6.6 meta-blit pipeline
    virtual void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter);
    virtual void transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope); // mostly no-op

    // --- init / upload / discard (§1h, contracts 3a,3c,3g) -----------------
    virtual void initBuffer(const Rc<MtlBuffer>& buffer);                        // zero-fill
    virtual void initImage(const Rc<MtlTexture>& image);                        // clear-to-black
    // (!) Discard/rename commit point, ordered against draws by the command
    // stream: retargets still-bound slices of `buffer` to its new storage and
    // retires the OLD backing (recycle pool once its serial completes).
    virtual void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice);
    virtual void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size);
    virtual bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage);   // realloc+blit if needed

    // --- queries (§1i, contract 3d) ----------------------------------------
    virtual void beginQuery(const Rc<MtlQuery>& query);
    virtual void endQuery(const Rc<MtlQuery>& query);
    virtual void writeTimestamp(const Rc<MtlQuery>& query);                      // (!) EVENT query => shared event

    // --- signals / events (§1j, contracts 3d,3e) ---------------------------
    virtual void signal(const Rc<MtlSignal>& signal, uint64_t value);           // (!) CPU-visible, on completion
    virtual void signalGpuEvent(const Rc<MtlEvent>& event);

    // --- lifetime / residency (contract 3h) --------------------------------
    virtual void track(Rc<MtlResource>&& resource);                             // residency + keep-alive

    // --- debug labels (§1k) ------------------------------------------------
    virtual void beginDebugLabel(const char* label);
    virtual void endDebugLabel();
    virtual void insertDebugLabel(const char* label);

  private:

    Rc<MtlDevice> m_device;

  };

  // --- vertex layout descriptors (was DxvkVertexInput arrays) --------------
  // Referenced by setVertexLayout; §6.4 strides participate in the PSO key.
  struct MtlVertexAttr {
    uint32_t  location   = 0;   // shader attribute index
    uint32_t  binding    = 0;   // which vertex buffer
    MtlFormat format     = MtlFormat::Invalid;
    uint32_t  offset     = 0;   // byte offset within the vertex
  };

  struct MtlVertexLayout {
    uint32_t binding    = 0;
    uint32_t stride     = 0;    // PSO-key participant
    bool     perInstance = false;
    uint32_t divisor    = 0;
  };

}
