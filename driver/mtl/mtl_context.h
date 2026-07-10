#pragma once

#include <cstdint>

#include "mtl_types.h"
#include "mtl_resource.h"
#include "mtl_device.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

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
    // PHASE-C: reflection (DxvkShaderMetadata-equivalent), entry name.
  };

  class MtlCommandBuffer : public RcObject { };   // MTL4CommandBuffer
  class MtlQuery         : public RcObject { };   // occlusion / timestamp / event
  class MtlSignal        : public RcObject { };   // CPU-visible MTLSharedEvent
  class MtlEvent         : public RcObject { };   // GPU MTLEvent
  class MtlLatencyTracker: public RcObject { };   // Reflex-style markers (thin)

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
    ~MtlContext();

    // --- lifecycle / submission (§1j, contracts 3e,3i) ---------------------
    void beginRecording();
    void endFrame();
    void flushCommandList(const char* reason, MtlSubmitStatus* status);   // commit + new cmdbuf
    Rc<MtlCommandBuffer> beginExternalRendering();                        // full state invalidate
    void synchronizeWsi(MtlPresenterSync sync);                          // (!) present acquire/release
    void beginLatencyTracking(const Rc<MtlLatencyTracker>&, uint64_t frameId);
    void endLatencyTracking(const Rc<MtlLatencyTracker>&);

    // --- shaders & pipeline state (folds into memoized render PSO) (§1b,1c)-
    // The render PSO is resolved lazily at draw from folded state; its key
    // EXCLUDES depth/stencil attachment formats (Metal 4 defers them). PSO compile
    // is async (MTL4Compiler) — never a blocking compile on the CS thread.
    void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn);
    void setFunctionConstants(MtlPipeline pipeline, uint32_t index, uint32_t count, const void* data);
    void setVertexLayout(uint32_t nAttr, const struct MtlVertexAttr* attrs,
                         uint32_t nLayout, const struct MtlVertexLayout* layouts);
    void setPrimitiveTopology(MtlPrimitiveTopology topology);
    void setViewports(uint32_t n, const MtlViewport* viewports);          // §6.1 RT-pixel coords only
    void setRasterizerState(const MtlRasterizerState& rs);
    void setMultisampleState(const MtlMultisampleState& ms);
    void setDepthStencilState(const MtlDepthStencilState& ds);            // cached MTLDepthStencilState
    void setBlendMode(uint32_t rt, const MtlBlendMode& blend);
    // §6.2 setLogicOpState — CUT (D3D9 has no logic ops; DxvkContext only had it
    //      because it also served D3D11). Intentionally absent from this surface.
    // dynamic (no PSO rebuild):
    void setBlendColor(MtlColor4 color);
    void setStencilReference(uint32_t reference);
    void setDepthBias(MtlDepthBias bias);
    void setDepthBounds(MtlDepthBounds bounds);                           // §6.3 accepted + no-op

    // --- slot-based argument-table binding (§1d, contract 3h) -------------
    // §6.4: stride is baked into the Metal vertex descriptor => part of the PSO
    //       memo key. A stride change marks the render PSO dirty; a binding change
    //       alone does not.
    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride);
    void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride);
    void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type);
    void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice);
    void bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length);
    void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view);
    void bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view);
    void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler);
    void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data); // <=4KB inline

    // --- render targets (§1e, contract 3f) ---------------------------------
    void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop); // pending pass, lazy encoder
    void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope);

    // --- draws (§1a) — resolve PSO/arg-tables/dynamic state, open encoder ---
    void draw(uint32_t count, const MtlDrawArgs* args);
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args);

    // --- clears (§1f) ------------------------------------------------------
    void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard);
    void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value);

    // --- copies / blits / resolves / mips (§1g) ----------------------------
    void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes);
    void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format);
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil); // §6.5
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter);                        // §6.6 meta-blit pipeline
    void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter);
    void transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope); // mostly no-op

    // --- init / upload / discard (§1h, contracts 3a,3c,3g) -----------------
    void initBuffer(const Rc<MtlBuffer>& buffer);                        // zero-fill
    void initImage(const Rc<MtlTexture>& image);                        // clear-to-black
    void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice); // (!) discard/rename ring
    void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size);
    bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage);   // realloc+blit if needed

    // --- queries (§1i, contract 3d) ----------------------------------------
    void beginQuery(const Rc<MtlQuery>& query);
    void endQuery(const Rc<MtlQuery>& query);
    void writeTimestamp(const Rc<MtlQuery>& query);                      // (!) EVENT query => shared event

    // --- signals / events (§1j, contracts 3d,3e) ---------------------------
    void signal(const Rc<MtlSignal>& signal, uint64_t value);           // (!) CPU-visible, on completion
    void signalGpuEvent(const Rc<MtlEvent>& event);

    // --- lifetime / residency (contract 3h) --------------------------------
    void track(Rc<MtlResource>&& resource);                             // residency + keep-alive

    // --- debug labels (§1k) ------------------------------------------------
    void beginDebugLabel(const char* label);
    void endDebugLabel();
    void insertDebugLabel(const char* label);

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
