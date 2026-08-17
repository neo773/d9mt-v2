#include <cstring>

#include "mtl_device.h"

#include "../wsi/wsi_platform.h"
#include "mtl_context.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/config/config.h"

namespace dxvk {

  size_t MtlSamplerKey::hash() const {
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

  Rc<MtlShaderFunction> MtlDevice::createShaderFunction(
    MtlShaderStage stage, const void* metallib, size_t size) {
    Logger::warn("MtlDevice::createShaderFunction: stub");
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
    (void) resource;
    Logger::warn("MtlDevice::waitForResource: stub");
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

  MtlGpuQueryStatus MtlQuery::getData(MtlQueryData& out) {
    out = MtlQueryData{};
    return MtlGpuQueryStatus::Available;
  }

  MtlGpuEventStatus MtlEvent::test() {
    return MtlGpuEventStatus::Signaled;
  }

  Rc<MtlQuery> MtlDevice::createGpuQuery(MtlQueryType type) {
    Rc<MtlQuery> q = new MtlQuery();
    q->type = type;
    return q;
  }

  Rc<MtlEvent> MtlDevice::createGpuEvent() {
    return new MtlEvent();
  }

  uint64_t MtlDevice::getTimestampFrequency() const {
    return 1000000000ull;
  }

  const MtlDeviceInfo& MtlAdapter::deviceInfo() const {
    return m_info;
  }

  bool MtlAdapter::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }

  Rc<MtlDevice> MtlAdapter::createDevice() {
    extern Rc<MtlDevice> backendCreateDevice();
    Rc<MtlDevice> device = backendCreateDevice();
    if (device == nullptr) {
      Logger::err("MtlAdapter::createDevice: backend device unavailable");
      return new MtlDevice();
    }
    return device;
  }

  MtlInstance::MtlInstance() {
    wsi::init();

    m_config.merge(Config::getUserConfig());
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_config.logOptions();
  }

  MtlInstance::~MtlInstance() {
    wsi::quit();
  }

  uint32_t MtlInstance::adapterCount() const {
    return 1u;
  }

  Rc<MtlAdapter> MtlInstance::enumAdapters(uint32_t index) {
    return index == 0u ? new MtlAdapter() : nullptr;
  }

}
