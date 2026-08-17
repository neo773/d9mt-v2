#pragma once

#include <cstddef>

#include "mtl_types.h"
#include "mtl_resource.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/config/config.h"

namespace dxvk {

  class MtlContext;
  class MtlQuery;
  class MtlEvent;
  class MtlShaderFunction;
  enum class MtlQueryType : uint32_t;

  struct MtlBufferCreateInfo {
    uint64_t     size       = 0u;
    MtlUsage     usage      = MtlUsage::None;
    MtlCpuAccess cpuAccess  = MtlCpuAccess::None;
  };

  struct MtlImageCreateInfo {
    MtlImageType  type        = MtlImageType::e2D;
    MtlFormat     format      = MtlFormat::Invalid;
    MtlExtent3D   extent      = { };
    uint32_t      mipLevels   = 1u;
    uint32_t      arrayLayers = 1u;
    uint32_t      sampleCount = 1u;
    MtlUsage      usage       = MtlUsage::None;
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

  class MtlDevice : public RcObject {

  public:

    MtlDevice();
    virtual ~MtlDevice();

    virtual Rc<MtlBuffer> createBuffer(
            const MtlBufferCreateInfo&  info);

    virtual Rc<MtlTexture> createImage(
            const MtlImageCreateInfo&   info);

    virtual Rc<MtlTextureView> createImageView(
            const Rc<MtlTexture>&       image,
            const MtlImageViewKey&      key);

    virtual Rc<MtlSamplerState> createSampler(
            const MtlSamplerKey&        key);

    virtual Rc<MtlShaderFunction> createShaderFunction(
            MtlShaderStage              stage,
            const void*                 metallib,
            size_t                      size);

    virtual Rc<MtlContext> createContext();

    virtual uint64_t getCompletedSerial();

    virtual uint64_t getNextSerial();

    virtual void waitForIdle();

    virtual bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const;

    virtual const MtlDeviceInfo& deviceInfo() const;

    virtual Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer);

    virtual MtlStageMask getShaderStages() const;

    bool isResourceInUse(const MtlResource& resource) {
      return resource.trackingSerial() > getCompletedSerial();
    }

    // Blocks until the completed serial catches up to the resource's tracking
    // serial. An unsubmitted command buffer never signals, so calling this
    // without flushing first stalls forever.
    virtual void waitForResource(const MtlResource& resource);

    virtual Rc<MtlStagingBuffer> createStagingBuffer(MtlDeviceSize size);
    virtual MtlStagingBufferStats getStagingMemoryStatistics();

    virtual Rc<MtlQuery> createGpuQuery(MtlQueryType type);
    virtual Rc<MtlEvent> createGpuEvent();

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

  class MtlAdapter : public RcObject {
  public:
    virtual ~MtlAdapter() { }

    virtual const MtlDeviceInfo& deviceInfo() const;

    virtual bool supportsFormat(MtlFormat format, MtlUsage usage) const;

    virtual Rc<MtlDevice> createDevice();

  protected:
    MtlDeviceInfo m_info = { };
  };

  class MtlInstance : public RcObject {
  public:
    MtlInstance();
    virtual ~MtlInstance();

    virtual uint32_t adapterCount() const;

    virtual Rc<MtlAdapter> enumAdapters(uint32_t index);

    const Config& config() const { return m_config; }
    void setConfig(const Config& config) { m_config = config; }

  protected:
    Config m_config;
  };

}
