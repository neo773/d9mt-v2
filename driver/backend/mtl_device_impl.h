#pragma once
/*
 * Real MtlDevice on winemetal.
 *
 * Owns the WMTDevice + command queue, the submission tracker (retirement
 * spine), the upload ring, and the sampler/view caches. Resource objects
 * are backend subclasses of the seam types so shared headers stay stable:
 * the frontend sees Rc<MtlBuffer>/Rc<MtlTexture>; the backend downcasts to
 * reach the owned winemetal handles.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"   // MtlQuery / MtlEvent / MtlQueryType

#include "wmt_handle.h"
#include "mtl_residency.h"
#include "mtl_ring.h"
#include "mtl_submission.h"
#include "mtl_wire_emit.h"

namespace dxvk::backend {

  /* Metal 4 handle tags (additive to wmt_handle.h's set; kept here so that
   * API-neutral file stays untouched). */
  struct Mtl4CompilerTag;     using WmtCompiler     = WmtRef<Mtl4CompilerTag>;
  struct Mtl4AllocatorTag;    using WmtAllocator    = WmtRef<Mtl4AllocatorTag>;
  struct Mtl4ArgTableTag;     using WmtArgTable     = WmtRef<Mtl4ArgTableTag>;

  /* Backend payloads. The seam's POD handle mirrors the owned handle so
   * frontend fast paths (binding) never need the downcast. */
  class MtlAllocationImpl : public MtlAllocation {
  public:
    WmtBuffer owned;
    class MtlDeviceImpl* residencyDevice = nullptr;   // deferred-destruction route

    /* Hands the Metal object, the VirtualAlloc'd host backing and the
     * residency-set entry to the device's MtlResidencyTracker, which tears
     * them down only after every submission that could reference the
     * allocation has completed (mtl_residency.h invariant 3). */
    ~MtlAllocationImpl();
  };

  class MtlBufferImpl : public MtlBuffer {
  public:
    WmtBuffer owned;                    /* initial backing (also in storage) */
    MtlBufferCreateInfo info = { };     /* for rename re-creation */

    void bindDevice(MtlDevice* device) { m_device = device; }

    /* Discard-rename recycling (DXVK's per-buffer slice cache): a retired
     * backing returns here via the retirement spine — only AFTER the last
     * submission referencing it completed — and renameBuffer() hands it out
     * again instead of minting a fresh Metal buffer per DISCARD. Bounded:
     * overflow drops the Rc, which routes the allocation into deferred
     * destruction (~MtlAllocationImpl). Cross-thread: renames run on app
     * threads, recycling on the retirement pump / CS thread. */
    void recycleStorage(Rc<MtlAllocation>&& alloc) {
      if (alloc == nullptr)
        return;
      /* P1 credit law: LOCAL completion put this backing here, but the
       * SERVER may still be up to N frames behind — stamp the last frame
       * marker that can reference the blob pages; takeRecycledStorage gates
       * reuse on the server's completed serial (the premature-reuse class
       * the PoC reproduced). */
      if (alloc->blobOffset != ~0ull)
        alloc->blobRetireStamp = wire::blobStamp();
      std::lock_guard<std::mutex> lock(m_recycleMutex);
      if (m_recycled.size() < MaxRecycledBackings)
        m_recycled.push_back(std::move(alloc));
    }

    Rc<MtlAllocation> takeRecycledStorage() {
      std::lock_guard<std::mutex> lock(m_recycleMutex);
      for (size_t i = m_recycled.size(); i-- > 0u; ) {
        /* P1: a blob-backed recycled entry is reusable only once the
         * server's completed frame serial covers its retire stamp; skip
         * not-yet-covered entries (renameBuffer mints a fresh slice — the
         * frame credits bound how long an entry waits to ~N presents). */
        if (m_recycled[i]->blobOffset != ~0ull &&
            !wire::blobReusable(m_recycled[i]->blobRetireStamp))
          continue;
        Rc<MtlAllocation> alloc = std::move(m_recycled[i]);
        m_recycled.erase(m_recycled.begin() + ptrdiff_t(i));
        return alloc;
      }
      return nullptr;
    }

  private:
    static constexpr size_t MaxRecycledBackings = 4u;

    std::mutex m_recycleMutex;
    std::vector<Rc<MtlAllocation>> m_recycled;
  };

  class MtlTextureImpl : public MtlTexture {
  public:
    WmtTexture owned;
    uint64_t   gpuResourceId = 0u;
    MtlImageCreateInfo info  = { };
    class MtlDeviceImpl* residencyDevice = nullptr;   // deferred-destruction route
    ~MtlTextureImpl();   // inline-defined after MtlDeviceImpl below
  };

  class MtlTextureViewImpl : public MtlTextureView {
  public:
    WmtTexture owned;             /* view object; base kept alive via image */
    uint64_t   gpuResourceId = 0u;
  };

  class MtlSamplerStateImpl : public MtlSamplerState {
  public:
    WmtSampler owned;
    uint64_t   gpuResourceId = 0u;
    /* render-server S2: the creation key, kept so the wire producer can emit
     * create_sampler lazily at first bindSampler (the server rebuilds the
     * sampler from the key — sampler objects have no byte payload). */
    MtlSamplerKey key = { };
  };

  class MtlDeviceImpl : public MtlDevice {

  public:

    MtlDeviceImpl();
    ~MtlDeviceImpl();

    Rc<MtlBuffer> createBuffer(
            const MtlBufferCreateInfo&  info) override;

    Rc<MtlTexture> createImage(
            const MtlImageCreateInfo&   info) override;

    Rc<MtlTextureView> createImageView(
            const Rc<MtlTexture>&       image,
            const MtlImageViewKey&      key) override;

    Rc<MtlSamplerState> createSampler(
            const MtlSamplerKey&        key) override;

    Rc<MtlShaderFunction> createShaderFunction(
            MtlShaderStage              stage,
            const void*                 metallib,
            size_t                      size) override;

    Rc<MtlContext> createContext() override;

    uint64_t getCompletedSerial() override;
    uint64_t getNextSerial() override;
    void     waitForIdle() override;

    /* Blocking wait on the submission tracker's shared event for the
     * resource's tracking serial (never a spin). */
    void     waitForResource(const MtlResource& resource) override;

    /* VirtualAlloc with a drain-and-retry fallback when the 32-bit address
     * space is exhausted by in-flight buffer backings. */
    void*    hostAlloc(uint64_t size);

    bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const override;

    Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer) override;

    Rc<MtlQuery> createGpuQuery(MtlQueryType type) override;
    Rc<MtlEvent> createGpuEvent() override;

    // --- backend-internal ----------------------------------------------------
    obj_handle_t wmtDevice()   const { return m_device.handle(); }
    obj_handle_t wmtQueue()    const { return m_queue.handle(); }     // MTL4CommandQueue
    obj_handle_t wmtCompiler() const { return m_compiler.handle(); }  // MTL4Compiler

    SubmissionTracker& submissions() { return *m_tracker; }
    UploadRing&        uploadRing()  { return *m_ring; }

    /* Single owner of all residency state (set membership, batched
     * commits, deferred destruction, drawable-layer sets) — see
     * mtl_residency.h for the invariants. */
    MtlResidencyTracker& residency() { return *m_residency; }

    /* Cached MTLDepthStencilState keyed by the folded depth-stencil state
     * (setDepthStencilState is not a PSO key; the DSSO is bound dynamically). */
    obj_handle_t getDepthStencilState(const MtlDepthStencilState& ds);

    bool valid() const { return bool(m_device); }

    /* DIAG observers: cumulative creation counts by source, so the endFrame
     * DIAG line can attribute residency churn (buffers vs renames vs images). */
    uint64_t diagBuffersCreated() const { return m_diagBuffersCreated.load(); }
    uint64_t diagBuffersRenamed() const { return m_diagBuffersRenamed.load(); }
    uint64_t diagBuffersRecycled() const { return m_diagBuffersRecycled.load(); }
    uint64_t diagImagesCreated()  const { return m_diagImagesCreated.load(); }

  private:

    std::atomic<uint64_t> m_diagBuffersCreated { 0u };
    std::atomic<uint64_t> m_diagBuffersRenamed { 0u };
    std::atomic<uint64_t> m_diagBuffersRecycled { 0u };
    std::atomic<uint64_t> m_diagImagesCreated  { 0u };

    WmtDevice   m_device;
    WmtQueue    m_queue;      // MTL4CommandQueue
    WmtCompiler m_compiler;   // MTL4Compiler (PSO builds)

    /* Declaration order is load-bearing: destruction runs in reverse, so
     * the ring hands its chunks to the tracker before the tracker flushes
     * and detaches, and the tracker still has a live queue underneath. */
    std::unique_ptr<SubmissionTracker>   m_tracker;
    std::unique_ptr<MtlResidencyTracker> m_residency;
    std::unique_ptr<UploadRing>          m_ring;

    std::mutex m_samplerMutex;
    std::unordered_map<size_t, Rc<MtlSamplerState>> m_samplerCache;

    std::mutex m_dssoMutex;
    std::unordered_map<size_t, WmtDepthStencil> m_dssoCache;

    friend class MtlContextImpl;

  };

  /* Inline (weak comdat) so TUs that construct MtlTextureImpl — e.g. the
   * presenter wrapping a drawable — don't each emit a conflicting strong
   * out-of-line definition. Defined here now that MtlDeviceImpl is complete.
   * Drawable wrappers own nothing (borrowed handle, no residencyDevice) and
   * fall through; owned textures ride the deferred-destruction queue. */
  inline MtlTextureImpl::~MtlTextureImpl() {
    /* render-server S2: destroy queued BEFORE the Metal object can be
     * released (deferred destruction below) — so the server-side destroy
     * always precedes any create that reuses the handle value. */
    if (wireId)
      wire::queueDestroy(wireId, texture.tex);
    if (residencyDevice && owned) {
      residencyDevice->residency().allocDestroyed(
        owned.detach(), nullptr,
        residencyDevice->submissions().nextSerial());
    }
  }

}
