#include "mtl_device_impl.h"

#include <windows.h>

#include "mtl_context_impl.h"
#include "../mtl/mtl_context.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  namespace {

    constexpr uint64_t UploadRingChunkSize = 4u << 20;

    WMTPixelFormat wmtFormat(MtlFormat format) {
      /* MtlFormat mirrors WMTPixelFormat values by construction */
      return WMTPixelFormat(uint32_t(format));
    }

    WMTTextureType wmtTextureType(const MtlImageCreateInfo& info) {
      if (info.cube)
        return info.arrayLayers > 6u ? WMTTextureTypeCubeArray : WMTTextureTypeCube;

      switch (info.type) {
        case MtlImageType::e1D:
          return info.arrayLayers > 1u ? WMTTextureType1DArray : WMTTextureType1D;
        case MtlImageType::e3D:
          return WMTTextureType3D;
        default:
        case MtlImageType::e2D:
          if (info.sampleCount > 1u)
            return WMTTextureType2DMultisample;
          return info.arrayLayers > 1u ? WMTTextureType2DArray : WMTTextureType2D;
      }
    }

    WMTResourceOptions storageOptions(MtlCpuAccess access) {
      switch (access) {
        case MtlCpuAccess::Write:
          return WMTResourceOptions(
              WMTResourceStorageModeShared
            | WMTResourceCPUCacheModeWriteCombined
            | WMTResourceHazardTrackingModeUntracked);
        case MtlCpuAccess::Read:
          return WMTResourceOptions(
              WMTResourceStorageModeShared
            | WMTResourceHazardTrackingModeUntracked);
        default:
        case MtlCpuAccess::None:
          return WMTResourceOptions(
              WMTResourceStorageModePrivate
            | WMTResourceHazardTrackingModeUntracked);
      }
    }

    uint32_t textureUsage(MtlUsage usage) {
      uint32_t result = 0u;

      if (usage & MtlUsage::Sampled)
        result |= WMTTextureUsageShaderRead;
      if (usage & MtlUsage::RenderTarget)
        result |= WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead;
      if (usage & MtlUsage::DepthStencil)
        result |= WMTTextureUsageRenderTarget;

      /* swizzled views (A8/L8-class formats) need PixelFormatView */
      result |= WMTTextureUsagePixelFormatView;
      return result;
    }

  }

  MtlDeviceImpl::MtlDeviceImpl() {
    obj_handle_t devices = WMTCopyAllDevices();
    obj_handle_t device = NSArray_object(devices, 0u);
    NSObject_release(devices);

    if (!device) {
      Logger::err("MtlDeviceImpl: no Metal device");
      return;
    }

    m_device = WmtDevice::adopt(device);
    /* METAL 4 ONLY (SOURCE_OF_TRUTH §2): the queue is an MTL4CommandQueue and
     * PSOs build through an MTL4Compiler. No MTLCommandQueue path exists. */
    m_queue = WmtQueue::adopt(MTLDevice_newMTL4CommandQueue(m_device.handle()));
    m_compiler = WmtCompiler::adopt(MTLDevice_newMTL4Compiler(m_device.handle()));

    m_tracker = std::make_unique<SubmissionTracker>(m_device.handle());
    /* All residency state lives in the tracker (mtl_residency.h): one
     * queue-attached set, add-at-alloc, serial-deferred destruction. */
    m_residency = std::make_unique<MtlResidencyTracker>(m_device.handle(), m_queue.handle());
    m_ring = std::make_unique<UploadRing>(m_device.handle(), UploadRingChunkSize, m_residency.get());

    m_info.maxBufferLength = MTLDevice_recommendedMaxWorkingSetSize(m_device.handle());
  }

  MtlDeviceImpl::~MtlDeviceImpl() {
    /* Teardown order (member dtors run AFTER this body, in reverse
     * declaration order: ring, residency, tracker): the pump thread must be
     * stopped and every retirement callback drained FIRST — those callbacks
     * reach into the residency tracker (allocDestroyed), which must still
     * be alive when they run. drainAndStopPump also waits the GPU idle. */
    if (m_tracker)
      m_tracker->drainAndStopPump();
    /* ~MtlResidencyTracker then flushes all deferred destructions
     * (serialRetired(~0) under its GPU-idle contract, now satisfied). */
  }

  void* MtlDeviceImpl::hostAlloc(uint64_t size) {
    void* p = VirtualAlloc(nullptr, SIZE_T(size),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (unlikely(!p)) {
      /* 32-bit address space exhausted by in-flight backings. Drain the GPU
       * and run the retirement spine (thread-safe) so retired allocations
       * VirtualFree their memory, then retry once. */
      Logger::warn("MtlDeviceImpl: host allocation failed — draining GPU and retrying");
      waitForIdle();
      if (m_tracker)
        m_tracker->processRetirements();
      /* The retirements just dropped the last Rcs; flush the completed
       * deferred destructions so their VirtualAlloc'd backings free NOW. */
      if (m_tracker && m_residency)
        m_residency->serialRetired(m_tracker->completedSerial());
      p = VirtualAlloc(nullptr, SIZE_T(size),
          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!p)
        Logger::err("MtlDeviceImpl: host allocation failed after drain");
    }
    return p;
  }

  MtlAllocationImpl::~MtlAllocationImpl() {
    if (residencyDevice) {
      /* Deferred destruction (mtl_residency.h invariant 3): the tracker
       * removes the set entry, releases the Metal buffer and frees the
       * bytesNoCopy host backing — in that order — once every submission
       * that could reference this allocation has completed. The recording
       * serial bounds that (encoded references live in the current or an
       * older command buffer). */
      residencyDevice->residency().allocDestroyed(
        owned.detach(), mapPtr,
        residencyDevice->submissions().nextSerial());
      mapPtr = nullptr;
      return;
    }
    /* Never registered with a device: release directly. */
    owned.reset();
    if (mapPtr) {
      VirtualFree(mapPtr, 0, MEM_RELEASE);
      mapPtr = nullptr;
    }
  }

  Rc<MtlBuffer> MtlDeviceImpl::createBuffer(const MtlBufferCreateInfo& info) {
    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = info.size;
    bufferInfo.options = storageOptions(info.cpuAccess);
    /* 32-bit PE rule: CPU-visible backing allocated in OUR space */
    bufferInfo.memory.set(info.cpuAccess != MtlCpuAccess::None
      ? hostAlloc(info.size)
      : nullptr);

    obj_handle_t handle = MTLDevice_newBuffer(m_device.handle(), &bufferInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createBuffer failed");
      return nullptr;
    }

    Rc<MtlBufferImpl> buffer = new MtlBufferImpl();
    buffer->owned = WmtBuffer::adopt(handle);
    buffer->size = info.size;
    buffer->info = info;
    buffer->bindDevice(this);

    Rc<MtlAllocationImpl> storage = new MtlAllocationImpl();
    storage->owned = WmtBuffer::retain(handle);
    storage->handle.buf = MtlObjectHandle(handle);
    storage->mapPtr = bufferInfo.memory.get();
    storage->gpuAddress = bufferInfo.gpu_address;
    storage->size = info.size;
    storage->residencyDevice = this;
    m_residency->allocCreated(handle);   // resident from creation to retirement
    buffer->assignStorage(std::move(storage));
    m_diagBuffersCreated++;
    return buffer;
  }

  Rc<MtlTexture> MtlDeviceImpl::createImage(const MtlImageCreateInfo& info) {
    WMTTextureInfo texInfo = {};
    texInfo.pixel_format = wmtFormat(info.format);
    texInfo.width = info.extent.width;
    texInfo.height = info.extent.height;
    texInfo.depth = info.extent.depth;
    texInfo.type = wmtTextureType(info);
    /* Metal's arrayLength counts array *slices*, and for cube types the 6 faces
     * are implicit (NOT slices): MTLTextureTypeCube requires arrayLength == 1,
     * MTLTextureTypeCubeArray requires arrayLength == number-of-cubes. D3D9
     * passes arrayLayers == 6*cubeCount, so divide by 6 for cube types. Non-cube
     * array textures use arrayLayers directly; everything needs arrayLength >= 1. */
    uint32_t arrayLength = info.arrayLayers > 0u ? info.arrayLayers : 1u;
    if (info.cube)
      arrayLength = arrayLength / 6u > 0u ? arrayLength / 6u : 1u;
    texInfo.array_length = arrayLength;
    texInfo.mipmap_level_count = info.mipLevels;
    texInfo.sample_count = info.sampleCount;
    texInfo.usage = WMTTextureUsage(textureUsage(info.usage));
    texInfo.options = storageOptions(MtlCpuAccess::None);

    obj_handle_t handle = MTLDevice_newTexture(m_device.handle(), &texInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createImage failed");
      return nullptr;
    }

    Rc<MtlTextureImpl> texture = new MtlTextureImpl();
    texture->owned = WmtTexture::adopt(handle);
    texture->texture.tex = MtlObjectHandle(handle);
    texture->texture.generation = 0u;
    texture->format = info.format;
    texture->extent = info.extent;
    texture->gpuResourceId = texInfo.gpu_resource_id;
    texture->info = info;
    /* Base-class metadata the frontend reads via image->info() and
     * mipLevelExtent() — unset it reports 1x1 extents (UpdateTexture upload
     * regions collapse to a single texel: black game textures). */
    texture->m_info.type        = info.type;
    texture->m_info.format      = info.format;
    texture->m_info.extent      = info.extent;
    texture->m_info.mipLevels   = info.mipLevels ? info.mipLevels : 1u;
    texture->m_info.numLayers   = info.arrayLayers ? info.arrayLayers : 1u;
    texture->m_info.sampleCount = info.sampleCount ? info.sampleCount : 1u;
    texture->m_info.usage       = MtlTextureUsage(textureUsage(info.usage));
    texture->residencyDevice = this;
    m_residency->allocCreated(handle);   // resident from creation to retirement
    m_diagImagesCreated++;
    return texture;
  }

  Rc<MtlTextureView> MtlDeviceImpl::createImageView(
    const Rc<MtlTexture>& image, const MtlImageViewKey& key) {
    auto* impl = static_cast<MtlTextureImpl*>(image.ptr());

    WMTTextureSwizzleChannels swizzle;
    if (key.usage & MtlTextureUsageRenderTarget) {
      /* Metal strips RenderTarget usage from texture views with a
       * non-identity swizzle — an RTV/DSV built with the sampling swizzle
       * (e.g. X8R8G8B8's {R,G,B,One}) silently loses attachability and every
       * render pass on it is invalid. Attachments always use identity. */
      swizzle.r = WMTTextureSwizzleRed;
      swizzle.g = WMTTextureSwizzleGreen;
      swizzle.b = WMTTextureSwizzleBlue;
      swizzle.a = WMTTextureSwizzleAlpha;
    } else {
      swizzle.r = WMTTextureSwizzle(key.swizzle.r);
      swizzle.g = WMTTextureSwizzle(key.swizzle.g);
      swizzle.b = WMTTextureSwizzle(key.swizzle.b);
      swizzle.a = WMTTextureSwizzle(key.swizzle.a);
    }

    MtlImageCreateInfo viewShape = impl->info;
    viewShape.arrayLayers = key.layerCount;

    uint64_t gpuResourceId = 0u;
    obj_handle_t handle = MTLTexture_newTextureView(
      impl->owned.handle(),
      wmtFormat(key.format),
      wmtTextureType(viewShape),
      key.baseMip, key.mipCount,
      key.baseLayer, key.layerCount,
      swizzle,
      &gpuResourceId);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createImageView failed");
      return nullptr;
    }

    Rc<MtlTextureViewImpl> view = new MtlTextureViewImpl();
    view->owned = WmtTexture::adopt(handle);
    view->view.tex = MtlObjectHandle(handle);
    view->key = key;
    view->image = image;
    view->gpuResourceId = gpuResourceId;
    return view;
  }

  Rc<MtlSamplerState> MtlDeviceImpl::createSampler(const MtlSamplerKey& key) {
    size_t hash = key.hash();

    { std::lock_guard<std::mutex> lock(m_samplerMutex);
      auto entry = m_samplerCache.find(hash);
      if (entry != m_samplerCache.end())
        return entry->second;
    }

    WMTSamplerInfo samplerInfo = {};
    samplerInfo.min_filter = WMTSamplerMinMagFilter(key.minFilter);
    samplerInfo.mag_filter = WMTSamplerMinMagFilter(key.magFilter);
    samplerInfo.mip_filter = WMTSamplerMipFilter(key.mipMode);
    samplerInfo.s_address_mode = WMTSamplerAddressMode(key.addressU);
    samplerInfo.t_address_mode = WMTSamplerAddressMode(key.addressV);
    samplerInfo.r_address_mode = WMTSamplerAddressMode(key.addressW);
    samplerInfo.border_color = WMTSamplerBorderColor(key.borderColor);
    samplerInfo.compare_function = key.compareEnable
      ? WMTCompareFunction(key.compareOp)
      : WMTCompareFunctionNever;
    samplerInfo.lod_min_clamp = key.minLod;
    samplerInfo.lod_max_clamp = key.maxLod;
    samplerInfo.max_anisotroy = key.maxAnisotropy;
    samplerInfo.normalized_coords = true;
    samplerInfo.support_argument_buffers = true;

    obj_handle_t handle = MTLDevice_newSamplerState(m_device.handle(), &samplerInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createSampler failed");
      return nullptr;
    }

    Rc<MtlSamplerStateImpl> sampler = new MtlSamplerStateImpl();
    sampler->owned = WmtSampler::adopt(handle);
    sampler->sampler = MtlObjectHandle(handle);
    sampler->gpuResourceId = samplerInfo.gpu_resource_id;

    std::lock_guard<std::mutex> lock(m_samplerMutex);
    return m_samplerCache.emplace(hash, sampler).first->second;
  }

  Rc<MtlAllocation> MtlDeviceImpl::renameBuffer(MtlBuffer* buffer) {
    auto* impl = static_cast<MtlBufferImpl*>(buffer);

    /* Recycle a retired backing first (returned via the retirement spine,
     * so its last referencing submission has completed): steady-state
     * DISCARD renaming must not mint a Metal buffer + VirtualAlloc + set
     * mutation per Lock. */
    if (Rc<MtlAllocation> recycled = impl->takeRecycledStorage()) {
      m_diagBuffersRecycled++;
      return recycled;
    }

    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = impl->size;
    bufferInfo.options = storageOptions(impl->info.cpuAccess);
    bufferInfo.memory.set(impl->info.cpuAccess != MtlCpuAccess::None
      ? hostAlloc(impl->size)
      : nullptr);

    obj_handle_t handle = MTLDevice_newBuffer(m_device.handle(), &bufferInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: renameBuffer failed");
      return nullptr;
    }

    Rc<MtlAllocationImpl> storage = new MtlAllocationImpl();
    storage->owned = WmtBuffer::adopt(handle);
    storage->handle.buf = MtlObjectHandle(handle);
    storage->mapPtr = bufferInfo.memory.get();
    storage->gpuAddress = bufferInfo.gpu_address;
    storage->size = impl->size;
    storage->residencyDevice = this;
    m_residency->allocCreated(handle);   // renamed backing resident until it retires
    m_diagBuffersRenamed++;
    return storage;
  }

  Rc<MtlShaderFunction> MtlDeviceImpl::createShaderFunction(
    MtlShaderStage stage, const void* metallib, size_t size) {
    if (!metallib || !size)
      return nullptr;

    // Wrap the AIR metallib bytes in a dispatch_data and hand them to
    // -newLibraryWithData: (doc 04). The unix side copies out of the given
    // native pointer, so the transient buffer only needs to outlive this call.
    obj_handle_t data = DispatchData_alloc_init(uint64_t(uintptr_t(metallib)), uint64_t(size));
    if (!data) {
      Logger::err("MtlDeviceImpl::createShaderFunction: DispatchData_alloc_init failed");
      return nullptr;
    }

    obj_handle_t err = 0u;
    obj_handle_t library = MTLDevice_newLibrary(m_device.handle(), data, &err);
    NSObject_release(data);

    if (!library) {
      Logger::err("MtlDeviceImpl::createShaderFunction: newLibraryWithData failed");
      if (err)
        NSObject_release(err);
      return nullptr;
    }

    auto fn = new MtlShaderFunction();
    fn->stage   = stage;
    fn->library = MtlObjectHandle(library);
    return fn;
  }

  Rc<MtlContext> MtlDeviceImpl::createContext() {
    return new MtlContextImpl(Rc<MtlDeviceImpl>(this));
  }

  Rc<MtlQuery> MtlDeviceImpl::createGpuQuery(MtlQueryType type) {
    Rc<MtlBackendQuery> q = new MtlBackendQuery();
    q->type = type;
    return q;
  }

  Rc<MtlEvent> MtlDeviceImpl::createGpuEvent() {
    return new MtlBackendEvent();
  }

  obj_handle_t MtlDeviceImpl::getDepthStencilState(const MtlDepthStencilState& ds) {
    size_t hash;
    { const uint8_t* p = reinterpret_cast<const uint8_t*>(&ds);
      uint64_t h = 0xcbf29ce484222325ull;
      for (size_t i = 0u; i < sizeof(ds); i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
      hash = size_t(h ^ (h >> 32));
    }

    { std::lock_guard<std::mutex> lock(m_dssoMutex);
      auto entry = m_dssoCache.find(hash);
      if (entry != m_dssoCache.end())
        return entry->second.handle();
    }

    WMTDepthStencilInfo info = {};
    info.depth_compare_function = WMTCompareFunction(ds.depthCompareOp);
    info.depth_write_enabled    = ds.depthWrite;

    auto fillStencil = [&](WMTStencilInfo& s, const MtlStencilState& m) {
      s.enabled                  = ds.stencilEnable;
      s.depth_stencil_pass_op    = WMTStencilOperation(m.passOp);
      s.stencil_fail_op          = WMTStencilOperation(m.failOp);
      s.depth_fail_op            = WMTStencilOperation(m.depthFailOp);
      s.stencil_compare_function = WMTCompareFunction(m.compareOp);
      s.write_mask               = uint8_t(m.writeMask);
      s.read_mask                = uint8_t(m.readMask);
    };
    fillStencil(info.front_stencil, ds.front);
    fillStencil(info.back_stencil,  ds.back);

    obj_handle_t handle = MTLDevice_newDepthStencilState(m_device.handle(), &info);

    std::lock_guard<std::mutex> lock(m_dssoMutex);
    return m_dssoCache.emplace(hash, WmtDepthStencil::adopt(handle)).first->second.handle();
  }

  uint64_t MtlDeviceImpl::getCompletedSerial() {
    return m_tracker ? m_tracker->completedSerial() : 0u;
  }

  uint64_t MtlDeviceImpl::getNextSerial() {
    return m_tracker ? m_tracker->nextSerial() : 1u;
  }

  void MtlDeviceImpl::waitForIdle() {
    /* Drain everything submitted so far. The tracker owns the submission
     * serials — the device-side m_lastSubmittedSerial was never updated and
     * made this a silent no-op (CPU readbacks raced the GPU copy). */
    if (m_tracker)
      m_tracker->waitFor(m_tracker->lastSubmittedSerial());
  }

  void MtlDeviceImpl::waitForResource(const MtlResource& resource) {
    /* Blocking shared-event wait for the resource's last encode serial.
     * A serial from a still-recording command buffer only signals after the
     * flush the caller (D3D9DeviceEx::WaitForResource) already issued. */
    if (m_tracker)
      m_tracker->waitFor(resource.trackingSerial());
  }

  bool MtlDeviceImpl::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }


}

namespace dxvk {

  /* seam-visible factory (declared extern in mtl_device_stub.cpp) */
  Rc<MtlDevice> backendCreateDevice() {
    Rc<backend::MtlDeviceImpl> device = new backend::MtlDeviceImpl();
    return device->valid() ? Rc<MtlDevice>(device) : nullptr;
  }

}
