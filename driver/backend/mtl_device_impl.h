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

    uint64_t m_lastSubmittedSerial = 0u;

    friend class MtlContextImpl;

  };

}
