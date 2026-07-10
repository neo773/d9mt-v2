#include <cstring>

#include "mtl_device.h"
#include "mtl_context.h"

#include "../util/log/log.h"

// Log-and-return stub so the frontend port compiles/links before the real
// backend (driver/backend) is wired in. Every method that returns an object
// returns an empty default so misuse is loud, not silently wrong.

namespace dxvk {

  size_t MtlSamplerKey::hash() const {
    // FNV-1a over the POD bytes; padding is zeroed by default initializers.
    // Computed in 64-bit, folded for 32-bit size_t (i686 PE).
    const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof(*this); i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
    return size_t(h ^ (h >> 32));
  }

  bool MtlSamplerKey::eq(const MtlSamplerKey& other) const {
    return !std::memcmp(this, &other, sizeof(*this));
  }

  MtlDevice::MtlDevice() { }
  MtlDevice::~MtlDevice() { }

  Rc<MtlBuffer> MtlDevice::createBuffer(const MtlBufferCreateInfo& info) {
    Logger::warn("MtlDevice::createBuffer: stub");
    return nullptr;
  }

  Rc<MtlTexture> MtlDevice::createImage(const MtlImageCreateInfo& info) {
    Logger::warn("MtlDevice::createImage: stub");
    return nullptr;
  }

  Rc<MtlTextureView> MtlDevice::createImageView(
    const Rc<MtlTexture>& image, const MtlImageViewKey& key) {
    Logger::warn("MtlDevice::createImageView: stub");
    return nullptr;
  }

  Rc<MtlSamplerState> MtlDevice::createSampler(const MtlSamplerKey& key) {
    Logger::warn("MtlDevice::createSampler: stub");
    return nullptr;
  }

  Rc<MtlContext> MtlDevice::createContext() {
    Logger::warn("MtlDevice::createContext: stub");
    return nullptr;
  }

  uint64_t MtlDevice::getCompletedSerial() {
    return 0u;
  }

  uint64_t MtlDevice::getNextSerial() {
    return 1u;
  }

  void MtlDevice::waitForIdle() {
    Logger::warn("MtlDevice::waitForIdle: stub");
  }

  bool MtlDevice::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }

  const MtlDeviceInfo& MtlDevice::deviceInfo() const {
    return m_info;
  }

  Rc<MtlAllocation> MtlDevice::renameBuffer(MtlBuffer* buffer) {
    Logger::warn("MtlDevice::renameBuffer: stub");
    return nullptr;
  }

  MtlStageMask MtlDevice::getShaderStages() const {
    return MtlStageVertex | MtlStageFragment;
  }

  void MtlDevice::waitForResource(const MtlResource& resource) {
    while (getCompletedSerial() < resource.trackingSerial()) { }
  }

  Rc<MtlStagingBuffer> MtlDevice::createStagingBuffer(MtlDeviceSize size) {
    MtlBufferCreateInfo info = { };
    info.size = size;
    info.usage = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Write;
    Rc<MtlBuffer> buffer = createBuffer(info);
    return buffer != nullptr ? new MtlStagingBuffer(std::move(buffer), size) : nullptr;
  }

  MtlStagingBufferStats MtlDevice::getStagingMemoryStatistics() {
    return MtlStagingBufferStats();
  }

  const MtlDeviceInfo& MtlAdapter::deviceInfo() const {
    return m_info;
  }

  bool MtlAdapter::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }

  Rc<MtlDevice> MtlAdapter::createDevice() {
    Logger::warn("MtlAdapter::createDevice: stub");
    return new MtlDevice();
  }

  uint32_t MtlInstance::adapterCount() const {
    return 1u;
  }

  Rc<MtlAdapter> MtlInstance::enumAdapters(uint32_t index) {
    return index == 0u ? new MtlAdapter() : nullptr;
  }

}
