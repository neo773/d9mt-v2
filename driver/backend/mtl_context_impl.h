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
#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_presenter.h"

#include "mtl_cmd_chunk.h"
#include "mtl_device_impl.h"
#include "mtl_pso_cache.h"

namespace dxvk::backend {

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

    /* ... MtlContext surface overrides (all ~55) ... */

  private:

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

    // --- core internal flow -------------------------------------------------
    void ensureCommandBuffer();
    void materializePass();            // open encoder, fold clears into load actions
    void closePass();                  // flush chunk -> encodeCommands -> endEncoding
    bool resolvePso();                 // build key from folded state, memo lookup
    void flushVertexBuffers();
    void flushResources();             // textures/samplers/CBVs -> wmtcmd sets + ring snapshots
    void flushDynamicState();
    void submit(bool present);         // tracker.onSubmit, ring.onSubmit, retirements
    void makeResident(obj_handle_t handle);

  };

}
