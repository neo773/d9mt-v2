#pragma once

#include <cstddef>

#include "mtl_types.h"
#include "mtl_resource.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/config/config.h"

// ---------------------------------------------------------------------------
// mtl_device.h — the resource-factory surface of the backend.
//
// This is the second half of the seam (mtl_context.h is command recording;
// this is object creation + lifetime). Method names deliberately mirror the
// DxvkDevice surface the d3d9 frontend calls, so ports stay token swaps.
//
// Semantics the frontend relies on (doc 01 §3, doc 02 §4):
//  - createBuffer/createImage return fully usable objects; images are
//    initialized (cleared) via the initializer path, not here.
//  - MtlBuffer::allocateStorage() is the MAP_DISCARD rename: returns a fresh
//    backing allocation and bumps the generation; the OLD backing must stay
//    alive until every submitted command buffer referencing it completes
//    (contract 3a — deadlock/corruption class; retirement runs on the
//    submission tracker, never stubbed).
//  - Samplers and image views are cached/pooled: D3D9 re-specifies them
//    constantly; creation must be a lookup in the steady state.
// ---------------------------------------------------------------------------

namespace dxvk {

  class MtlContext;
  class MtlQuery;                       // defined in mtl_context.h
  class MtlEvent;
  class MtlShaderFunction;              // defined in mtl_context.h
  enum class MtlQueryType : uint32_t;   // defined in mtl_context.h

  struct MtlBufferCreateInfo {
    uint64_t     size       = 0u;
    MtlUsage     usage      = MtlUsage::None;     // vertex/index/uniform/staging bits
    MtlCpuAccess cpuAccess  = MtlCpuAccess::None; // shared+write-combined vs private
  };

  struct MtlImageCreateInfo {
    MtlImageType  type        = MtlImageType::e2D;
    MtlFormat     format      = MtlFormat::Invalid;
    MtlExtent3D   extent      = { };
    uint32_t      mipLevels   = 1u;
    uint32_t      arrayLayers = 1u;   // 6 for cube
    uint32_t      sampleCount = 1u;
    MtlUsage      usage       = MtlUsage::None;   // sampled/renderTarget/depthStencil
    bool          cube        = false;
  };

  struct MtlSamplerKey {
    MtlSamplerFilter  magFilter  = MtlSamplerFilter::Nearest;
    MtlSamplerFilter  minFilter  = MtlSamplerFilter::Nearest;
    MtlSamplerMipMode mipMode    = MtlSamplerMipMode::Nearest;
    MtlSamplerAddressMode addressU = MtlSamplerAddressMode::Repeat;
    MtlSamplerAddressMode addressV = MtlSamplerAddressMode::Repeat;
    MtlSamplerAddressMode addressW = MtlSamplerAddressMode::Repeat;
    uint32_t          maxAnisotropy = 1u;
    float             mipLodBias = 0.f;
    float             minLod     = 0.f;
    float             maxLod     = 16.f;
    MtlCompareFunction compareOp = MtlCompareFunction::Never;
    bool              compareEnable = false;
    MtlBorderColor    borderColor = MtlBorderColor::TransparentBlack;

    size_t hash() const;
    bool eq(const MtlSamplerKey& other) const;
  };

  /**
   * \brief Backend device: owns the MTLDevice, queue, allocator and caches.
   *
   * Thread-safe for creation calls (the frontend creates resources from the
   * app thread while the CS thread records). Command recording itself is
   * single-threaded per MtlContext.
   */
  class MtlDevice : public RcObject {

  public:

    MtlDevice();
    virtual ~MtlDevice();

    // --- resource factories -------------------------------------------------
    virtual Rc<MtlBuffer> createBuffer(
            const MtlBufferCreateInfo&  info);

    virtual Rc<MtlTexture> createImage(
            const MtlImageCreateInfo&   info);

    /* Pooled: repeated keys return the cached view (doc 02 §2 view-pool). */
    virtual Rc<MtlTextureView> createImageView(
            const Rc<MtlTexture>&       image,
            const MtlImageViewKey&      key);

    /* Cached: bounded by D3D9 state space; lookup in steady state. */
    virtual Rc<MtlSamplerState> createSampler(
            const MtlSamplerKey&        key);

    // --- shader modules -----------------------------------------------------
    /* Wrap AIR metallib bytes (from the SM3->AIR compiler, doc 03/04) into a
     * bindable shader function via MTLDevice_newLibrary. Entry point is always
     * "main" (the AIR emitter's convention). Returns nullptr on failure. */
    virtual Rc<MtlShaderFunction> createShaderFunction(
            MtlShaderStage              stage,
            const void*                 metallib,
            size_t                      size);

    // --- contexts / submission ---------------------------------------------
    virtual Rc<MtlContext> createContext();

    /* GPU-completed submission serial (monotone). Query/event answers and
     * frame pacing derive from this — single source of truth. */
    virtual uint64_t getCompletedSerial();

    /* Serial the next submission will carry. */
    virtual uint64_t getNextSerial();

    /* Blocks until the GPU is idle. Teardown / device Reset only. */
    virtual void waitForIdle();

    // --- capabilities -------------------------------------------------------
    virtual bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const;

    virtual const MtlDeviceInfo& deviceInfo() const;

    /* The Discard rename: fresh backing for this buffer (contract 3a).
     * Old storage retirement is the backend's responsibility once the
     * frontend moves it into the CS invalidateBuffer command. */
    virtual Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer);

    /* Stage mask covering the graphics shader stages the device drives
     * (was DxvkDevice::getShaderPipelineStages, minus Vulkan semantics). */
    virtual MtlStageMask getShaderStages() const;

    /* Whether encoded GPU work may still touch the resource: its tracking
     * serial (stamped at CS encode time) has not completed yet. Only
     * meaningful after the CS thread has caught up with every chunk that
     * could use the resource (D3D9DeviceEx::WaitForResource syncs first). */
    bool isResourceInUse(const MtlResource& resource) {
      return resource.trackingSerial() > getCompletedSerial();
    }

    /* CPU-waits until the GPU is done with the resource (blocks until the
     * completed serial reaches the resource's tracking serial). The caller
     * must have flushed: an unsubmitted recording command buffer never
     * signals, so waiting on it without a flush is a stall. Lock-with-sync /
     * readback paths only — never per draw. */
    virtual void waitForResource(const MtlResource& resource);

    /* Staging upload space (was DxvkStagingBuffer allocation). */
    virtual Rc<MtlStagingBuffer> createStagingBuffer(MtlDeviceSize size);
    virtual MtlStagingBufferStats getStagingMemoryStatistics();

    /* Query / event objects (was DxvkDevice::createGpuQuery/createGpuEvent).
     * doc 05 §Queries: occlusion=visibility counter, timestamp=serial-based,
     * event=submission-serial compare. */
    virtual Rc<MtlQuery> createGpuQuery(MtlQueryType type);
    virtual Rc<MtlEvent> createGpuEvent();

    /* Timestamp counter frequency (was properties().limits.timestampPeriod). */
    virtual uint64_t getTimestampFrequency() const;

  protected:

    MtlDeviceInfo m_info = { };

  };

  inline Rc<MtlAllocation> MtlBuffer::allocateStorage() {
    return m_device ? m_device->renameBuffer(this) : nullptr;
  }

  inline Rc<MtlTextureView> MtlTexture::createView(const MtlImageViewKey& key) {
    return m_device ? m_device->createImageView(Rc<MtlTexture>(this), key) : nullptr;
  }

  inline MtlStagingBuffer::MtlStagingBuffer(const Rc<MtlDevice>& device, MtlDeviceSize size) {
    reinit(device, size);
  }

  /* Swap in a fresh backing buffer and reset the linear allocator. RcObject
   * deletes assignment, so a full staging buffer is recycled via this in-place
   * re-init rather than reassignment. The previous backing stays alive as long
   * as any outstanding slice still holds its Rc (GPU-in-flight uploads). */
  inline void MtlStagingBuffer::reinit(const Rc<MtlDevice>& device, MtlDeviceSize size) {
    MtlBufferCreateInfo info = { };
    info.size      = size;
    info.usage     = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Write;
    m_buffer = device ? device->createBuffer(info) : nullptr;
    m_size   = size;
    m_offset = 0u;
    m_stats  = { };
  }

  /**
   * \brief Adapter (was DxvkAdapter): one per MTLDevice.
   *
   * Apple Silicon = single GPU; enumeration exists for the D3D9 interface
   * shape, not because multiple adapters occur in practice.
   */
  class MtlAdapter : public RcObject {
  public:
    virtual ~MtlAdapter() { }

    virtual const MtlDeviceInfo& deviceInfo() const;

    virtual bool supportsFormat(MtlFormat format, MtlUsage usage) const;

    virtual Rc<MtlDevice> createDevice();

  protected:
    MtlDeviceInfo m_info = { };
  };

  /** \brief Instance (was DxvkInstance): adapter enumeration root. */
  class MtlInstance : public RcObject {
  public:
    MtlInstance();
    virtual ~MtlInstance();

    virtual uint32_t adapterCount() const;

    virtual Rc<MtlAdapter> enumAdapters(uint32_t index);

    /* Parsed dxvk.conf / d3d9.conf (was DxvkInstance::config). The interface
     * populates it at creation; the device reads it for D3D9Options. */
    const Config& config() const { return m_config; }
    void setConfig(const Config& config) { m_config = config; }

  protected:
    Config m_config;
  };

}
