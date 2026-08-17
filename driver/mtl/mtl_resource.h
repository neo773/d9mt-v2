#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "mtl_types.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class MtlResource;
  class MtlBuffer;
  class MtlTexture;
  class MtlTextureView;
  class MtlTexelBufferView;
  class MtlSamplerState;
  class MtlAllocation;

  struct MtlBufferHandle {
    MtlObjectHandle buf        = MtlNullHandle;
    uint32_t        generation = 0u;

    explicit operator bool () const { return buf != MtlNullHandle; }
    bool operator == (const MtlBufferHandle& o) const {
      return buf == o.buf && generation == o.generation;
    }
    bool operator != (const MtlBufferHandle& o) const { return !(*this == o); }
  };

  struct MtlTextureHandle {
    MtlObjectHandle tex        = MtlNullHandle;
    uint32_t        generation = 0u;

    explicit operator bool () const { return tex != MtlNullHandle; }
    bool operator == (const MtlTextureHandle& o) const {
      return tex == o.tex && generation == o.generation;
    }
    bool operator != (const MtlTextureHandle& o) const { return !(*this == o); }
  };

  struct MtlSuballocation {
    MtlBufferHandle backing    = { };
    MtlDeviceSize   offset     = 0;
    MtlDeviceSize   size       = 0;
    uint32_t        generation = 0u;
  };

  // Holds an Rc to the logical buffer plus a snapshot of its physical backing
  // taken at construction. Binds and draws resolve addresses through that
  // snapshot, never through the live buffer, so a Discard rename on another
  // thread cannot retarget a draw already recorded against the old backing.
  // The only sanctioned re-resolution is MtlContext::invalidateBuffer, which
  // refreshes still-bound slices on the CS thread after assignStorage.
  class MtlBufferSlice {

  public:

    MtlBufferSlice() = default;

    MtlBufferSlice(Rc<MtlBuffer> buffer, MtlDeviceSize offset, MtlDeviceSize length);

    MtlBufferSlice(Rc<MtlBuffer> buffer, Rc<MtlAllocation> storage,
                   MtlDeviceSize offset, MtlDeviceSize length);

    const Rc<MtlBuffer>&     buffer()  const { return m_buffer; }
    const Rc<MtlAllocation>& storage() const { return m_storage; }
    MtlDeviceSize            offset()  const { return m_offset; }
    MtlDeviceSize            length()  const { return m_length; }
    bool                     defined() const { return m_buffer != nullptr; }

    uint64_t gpuAddress(MtlDeviceSize extra = 0) const;

    MtlDeviceSize addressableLength(MtlDeviceSize extra = 0) const;

    void refreshStorage();

    MtlBufferSlice subSlice(MtlDeviceSize offset, MtlDeviceSize length) const;

    void* mapPtr(MtlDeviceSize offset) const;

  private:

    Rc<MtlBuffer>     m_buffer  = nullptr;
    Rc<MtlAllocation> m_storage = nullptr;
    MtlDeviceSize     m_offset  = 0;
    MtlDeviceSize     m_length  = 0;

  };

  struct MtlImageViewKey {
    MtlFormat          format     = MtlFormat::Invalid;
    MtlSwizzleChannels swizzle    = { };
    uint32_t           baseMip    = 0;
    uint32_t           mipCount   = 1;
    uint32_t           baseLayer  = 0;
    uint32_t           layerCount = 1;
    MtlTextureUsage    usage      = MtlTextureUsageShaderRead;
    MtlAspectMask      aspects    = MtlAspectColor;
    MtlTextureViewType viewType   = MtlTextureViewType::e2D;
    uint32_t           packedSwizzle = 0;

    static uint32_t packSwizzle(MtlSwizzleChannels s) {
      return (uint32_t(s.r) << 0) | (uint32_t(s.g) << 8)
           | (uint32_t(s.b) << 16) | (uint32_t(s.a) << 24);
    }

    bool operator == (const MtlImageViewKey& o) const {
      return format == o.format && swizzle == o.swizzle
          && baseMip == o.baseMip && mipCount == o.mipCount
          && baseLayer == o.baseLayer && layerCount == o.layerCount
          && usage == o.usage && aspects == o.aspects
          && viewType == o.viewType && packedSwizzle == o.packedSwizzle;
    }
    bool operator != (const MtlImageViewKey& o) const { return !(*this == o); }
  };

  struct MtlTexelBufferViewKey {
    MtlFormat     format = MtlFormat::Invalid;
    MtlDeviceSize offset = 0;
    MtlDeviceSize length = 0;
  };

  class MtlResource : public RcObject {
  public:
    virtual ~MtlResource() { }

    uint64_t trackingSerial() const { return m_trackingSerial.load(std::memory_order_acquire); }
    void setTrackingSerial(uint64_t serial) { m_trackingSerial.store(serial, std::memory_order_release); }

  private:
    std::atomic<uint64_t> m_trackingSerial = { 0u };
  };

  class MtlAllocation : public RcObject {
  public:
    // RcObject has no virtual destructor; without this override the backend
    // subclass's destructor never runs, so allocations are never freed or
    // removed from the residency set.
    virtual ~MtlAllocation() { }

    MtlBufferHandle handle     = { };
    void*           mapPtr     = nullptr;
    uint64_t        gpuAddress = 0u;
    MtlDeviceSize   size       = 0;
  };

  class MtlDevice;

  class MtlBuffer : public MtlResource {
  public:
    MtlBufferHandle handle = { };
    MtlDeviceSize   size   = 0;

    void* mapPtr(MtlDeviceSize offset) const {
      return m_storage && m_storage->mapPtr
        ? reinterpret_cast<char*>(m_storage->mapPtr) + offset
        : nullptr;
    }

    uint64_t gpuAddress(MtlDeviceSize offset) const {
      return m_storage ? m_storage->gpuAddress + offset : 0u;
    }

    const Rc<MtlAllocation>& storage() const {
      return m_storage;
    }

    Rc<MtlAllocation> allocateStorage();

    Rc<MtlAllocation> assignStorage(Rc<MtlAllocation>&& next) {
      Rc<MtlAllocation> prev = std::move(m_storage);
      m_storage = std::move(next);
      handle = m_storage->handle;
      handle.generation = prev ? prev->handle.generation + 1u : 0u;
      return prev;
    }

  protected:
    Rc<MtlAllocation> m_storage = nullptr;
    MtlDevice*        m_device  = nullptr;

    friend class MtlDevice;
  };

  inline MtlBufferSlice::MtlBufferSlice(Rc<MtlBuffer> buffer, MtlDeviceSize offset, MtlDeviceSize length)
  : m_buffer(std::move(buffer)), m_offset(offset), m_length(length) {
    if (m_buffer != nullptr)
      m_storage = m_buffer->storage();
  }

  inline MtlBufferSlice::MtlBufferSlice(Rc<MtlBuffer> buffer, Rc<MtlAllocation> storage,
                                        MtlDeviceSize offset, MtlDeviceSize length)
  : m_buffer(std::move(buffer)), m_storage(std::move(storage)),
    m_offset(offset), m_length(length) { }

  inline uint64_t MtlBufferSlice::gpuAddress(MtlDeviceSize extra) const {
    return m_storage != nullptr ? m_storage->gpuAddress + m_offset + extra : 0u;
  }

  inline MtlDeviceSize MtlBufferSlice::addressableLength(MtlDeviceSize extra) const {
    if (m_storage == nullptr)
      return 0u;
    MtlDeviceSize origin = m_offset + extra;
    return m_storage->size > origin ? m_storage->size - origin : 0u;
  }

  inline void MtlBufferSlice::refreshStorage() {
    if (m_buffer != nullptr)
      m_storage = m_buffer->storage();
  }

  inline MtlBufferSlice MtlBufferSlice::subSlice(MtlDeviceSize offset, MtlDeviceSize length) const {
    return MtlBufferSlice(m_buffer, m_storage, m_offset + offset, length);
  }

  inline void* MtlBufferSlice::mapPtr(MtlDeviceSize offset) const {
    return m_storage != nullptr && m_storage->mapPtr
      ? reinterpret_cast<char*>(m_storage->mapPtr) + m_offset + offset
      : nullptr;
  }

  struct MtlImageInfo {
    MtlImageType       type          = MtlImageType::e2D;
    MtlFormat          format        = MtlFormat::Invalid;
    MtlExtent3D        extent        = { };
    uint32_t           mipLevels     = 1u;
    uint32_t           numLayers     = 1u;
    uint32_t           sampleCount   = 1u;
    MtlTextureUsage    usage         = MtlTextureUsageShaderRead;
    MtlSwizzleChannels unpackSwizzle = { };
  };

  class MtlTexture : public MtlResource {
  public:
    MtlTextureHandle texture        = { };
    MtlTextureHandle resolveTexture = { };
    MtlFormat        format         = MtlFormat::Invalid;
    MtlSize          extent         = { };
    MtlImageInfo     m_info         = { };

    const MtlImageInfo& info() const { return m_info; }

    const MtlFormatInfo* formatInfo() const { return lookupFormatInfo(m_info.format); }

    Rc<MtlTextureView> createView(const MtlImageViewKey& key);

    MtlDevice* m_device = nullptr;

    MtlExtent3D mipLevelExtent(uint32_t mip) const {
      MtlExtent3D e = m_info.extent;
      e.width  = e.width  > (1u << mip) ? e.width  >> mip : 1u;
      e.height = e.height > (1u << mip) ? e.height >> mip : 1u;
      e.depth  = m_info.type == MtlImageType::e3D && e.depth > (1u << mip) ? e.depth >> mip : e.depth;
      return e;
    }
  };

  class MtlTextureView : public MtlResource {
  public:
    MtlTextureHandle view   = { };
    MtlImageViewKey  key    = { };
    Rc<MtlTexture>   image  = nullptr;
  };

  class MtlTexelBufferView : public MtlResource {
  public:
    MtlBufferHandle backing = { };
    MtlFormat       format  = MtlFormat::Invalid;
    MtlDeviceSize   offset  = 0;
    MtlDeviceSize   length  = 0;
  };

  class MtlSamplerState : public MtlResource {
  public:
    MtlObjectHandle sampler = MtlNullHandle;
  };


  class MtlStagingBuffer : public RcObject {
  public:
    MtlStagingBuffer() = default;
    MtlStagingBuffer(Rc<MtlBuffer> buffer, MtlDeviceSize size)
    : m_buffer(std::move(buffer)), m_size(size) { }

    MtlStagingBuffer(const Rc<MtlDevice>& device, MtlDeviceSize size);

    void reinit(const Rc<MtlDevice>& device, MtlDeviceSize size);

    MtlBufferSlice alloc(MtlDeviceSize size, MtlDeviceSize align = 256u) {
      MtlDeviceSize offset = (m_offset + align - 1u) & ~(align - 1u);
      if (offset + size > m_size)
        return MtlBufferSlice();
      m_offset = offset + size;
      m_stats.used += size;
      m_stats.allocated += size;
      return MtlBufferSlice(m_buffer, offset, size);
    }

    void reset() { m_offset = 0u; m_stats.used = 0u; }

    const MtlStagingBufferStats& stats() const { return m_stats; }

  private:
    Rc<MtlBuffer> m_buffer;
    MtlDeviceSize m_size = 0u;
    MtlDeviceSize m_offset = 0u;
    MtlStagingBufferStats m_stats = { };
  };

}
