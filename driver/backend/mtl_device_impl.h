#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"

#include "wmt_handle.h"
#include "mtl_residency.h"
#include "mtl_ring.h"
#include "mtl_submission.h"

namespace dxvk::backend {

  struct Mtl4CompilerTag;     using WmtCompiler     = WmtRef<Mtl4CompilerTag>;
  struct Mtl4AllocatorTag;    using WmtAllocator    = WmtRef<Mtl4AllocatorTag>;
  struct Mtl4ArgTableTag;     using WmtArgTable     = WmtRef<Mtl4ArgTableTag>;

  class MtlAllocationImpl : public MtlAllocation {
  public:
    WmtBuffer owned;
    class MtlDeviceImpl* residencyDevice = nullptr;

    ~MtlAllocationImpl();
  };

  class MtlBufferImpl : public MtlBuffer {
  public:
    WmtBuffer owned;
    MtlBufferCreateInfo info = { };

    void bindDevice(MtlDevice* device) { m_device = device; }

    void recycleStorage(Rc<MtlAllocation>&& alloc) {
      if (alloc == nullptr)
        return;
      std::lock_guard<std::mutex> lock(m_recycleMutex);
      if (m_recycled.size() < MaxRecycledBackings)
        m_recycled.push_back(std::move(alloc));
    }

    Rc<MtlAllocation> takeRecycledStorage() {
      std::lock_guard<std::mutex> lock(m_recycleMutex);
      if (m_recycled.empty())
        return nullptr;
      Rc<MtlAllocation> alloc = std::move(m_recycled.back());
      m_recycled.pop_back();
      return alloc;
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
    class MtlDeviceImpl* residencyDevice = nullptr;
    ~MtlTextureImpl();
  };

  class MtlTextureViewImpl : public MtlTextureView {
  public:
    WmtTexture owned;
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

    void     waitForResource(const MtlResource& resource) override;

    void*    hostAlloc(uint64_t size);

    bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const override;

    Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer) override;

    Rc<MtlQuery> createGpuQuery(MtlQueryType type) override;
    Rc<MtlEvent> createGpuEvent() override;

    obj_handle_t wmtDevice()   const { return m_device.handle(); }
    obj_handle_t wmtQueue()    const { return m_queue.handle(); }
    obj_handle_t wmtCompiler() const { return m_compiler.handle(); }

    SubmissionTracker& submissions() { return *m_tracker; }
    UploadRing&        uploadRing()  { return *m_ring; }

    MtlResidencyTracker& residency() { return *m_residency; }

    obj_handle_t getDepthStencilState(const MtlDepthStencilState& ds);

    bool valid() const { return bool(m_device); }

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
    WmtQueue    m_queue;
    WmtCompiler m_compiler;

    // Declaration order is load-bearing: destruction runs in reverse, so the
    // ring hands its chunks to the tracker before the tracker detaches, and
    // the tracker still has a live queue underneath it.
    std::unique_ptr<SubmissionTracker>   m_tracker;
    std::unique_ptr<MtlResidencyTracker> m_residency;
    std::unique_ptr<UploadRing>          m_ring;

    std::mutex m_samplerMutex;
    std::unordered_map<size_t, Rc<MtlSamplerState>> m_samplerCache;

    std::mutex m_dssoMutex;
    std::unordered_map<size_t, WmtDepthStencil> m_dssoCache;

    friend class MtlContextImpl;

  };

  // Defined inline (weak comdat) so every TU constructing MtlTextureImpl
  // shares one definition instead of emitting conflicting strong symbols.
  inline MtlTextureImpl::~MtlTextureImpl() {
    if (residencyDevice && owned) {
      residencyDevice->residency().allocDestroyed(
        owned.detach(), nullptr,
        residencyDevice->submissions().nextSerial());
    }
  }

}
