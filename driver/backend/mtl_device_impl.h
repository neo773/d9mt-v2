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

#include <memory>
#include <mutex>
#include <unordered_map>

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"   // MtlQuery / MtlEvent / MtlQueryType

#include "wmt_handle.h"
#include "mtl_ring.h"
#include "mtl_submission.h"

namespace dxvk::backend {

  /* Metal 4 handle tags (additive to wmt_handle.h's set; kept here so that
   * API-neutral file stays untouched). */
  struct Mtl4CompilerTag;     using WmtCompiler     = WmtRef<Mtl4CompilerTag>;
  struct Mtl4AllocatorTag;    using WmtAllocator    = WmtRef<Mtl4AllocatorTag>;
  struct Mtl4ResidencySetTag; using WmtResidencySet = WmtRef<Mtl4ResidencySetTag>;
  struct Mtl4ArgTableTag;     using WmtArgTable     = WmtRef<Mtl4ArgTableTag>;

  /* Backend payloads. The seam's POD handle mirrors the owned handle so
   * frontend fast paths (binding) never need the downcast. */
  class MtlAllocationImpl : public MtlAllocation {
  public:
    WmtBuffer owned;
    class MtlDeviceImpl* residencyDevice = nullptr;   // for remove-at-free

    /* Frees the VirtualAlloc'd host backing (bytesNoCopy memory) and removes
     * the allocation from the device residency set. Runs only after the
     * retirement spine drops the last Rc, i.e. the GPU is done. */
    ~MtlAllocationImpl();
  };

  class MtlBufferImpl : public MtlBuffer {
  public:
    WmtBuffer owned;                    /* initial backing (also in storage) */
    MtlBufferCreateInfo info = { };     /* for rename re-creation */

    void bindDevice(MtlDevice* device) { m_device = device; }
  };

  class MtlTextureImpl : public MtlTexture {
  public:
    WmtTexture owned;
    uint64_t   gpuResourceId = 0u;
    MtlImageCreateInfo info  = { };
    class MtlDeviceImpl* residencyDevice = nullptr;   // for remove-at-free
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

    /* ONE device-lifetime residency set (kk model): every Metal allocation is
     * added at creation and removed at destruction, so the set always covers
     * exactly the live resources. It is NEVER wiped mid-frame — that was the
     * cause of non-resident GPU faults (a bound-but-not-re-touched resource
     * dropped out of the set). The context binds it per cmdbuf and does
     * commit+requestResidency per submit. */
    obj_handle_t residencySet() const { return m_residencySet.handle(); }
    void residencyAdd(obj_handle_t alloc) {
      if (!alloc) return;
      std::lock_guard<std::mutex> lock(m_residencyMutex);
      MTLResidencySet_addAllocation(m_residencySet.handle(), alloc);
    }
    void residencyRemove(obj_handle_t alloc) {
      if (!alloc) return;
      std::lock_guard<std::mutex> lock(m_residencyMutex);
      MTLResidencySet_removeAllocation(m_residencySet.handle(), alloc);
    }

    /* Cached MTLDepthStencilState keyed by the folded depth-stencil state
     * (setDepthStencilState is not a PSO key; the DSSO is bound dynamically). */
    obj_handle_t getDepthStencilState(const MtlDepthStencilState& ds);

    bool valid() const { return bool(m_device); }

  private:

    WmtDevice   m_device;
    WmtQueue    m_queue;      // MTL4CommandQueue
    WmtCompiler m_compiler;   // MTL4Compiler (PSO builds)

    std::unique_ptr<SubmissionTracker> m_tracker;
    std::unique_ptr<UploadRing>        m_ring;

    std::mutex m_samplerMutex;
    std::unordered_map<size_t, Rc<MtlSamplerState>> m_samplerCache;

    std::mutex m_dssoMutex;
    std::unordered_map<size_t, WmtDepthStencil> m_dssoCache;

    std::mutex      m_residencyMutex;
    WmtResidencySet m_residencySet;   // device-lifetime; add-at-alloc/remove-at-free

    friend class MtlContextImpl;

  };

  /* Inline (weak comdat) so TUs that construct MtlTextureImpl — e.g. the
   * presenter wrapping a drawable — don't each emit a conflicting strong
   * out-of-line definition. Defined here now that MtlDeviceImpl is complete. */
  inline MtlTextureImpl::~MtlTextureImpl() {
    if (residencyDevice)
      residencyDevice->residencyRemove(owned.handle());
  }

}
