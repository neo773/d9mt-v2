#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "mtl_types.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

// ---------------------------------------------------------------------------
// mtl_resource.h — Metal-first resource handles + the Rc<>-managed object types
// the MtlContext seam moves by reference. Shapes follow doc 02 §4 (resource-object
// anatomy) and the §2 replacement table:
//   Rc<DxvkImage>   -> MtlTextureHandle { tex, generation }
//   Rc<DxvkBuffer>  -> MtlBufferHandle  { buf, generation }
//   DxvkBufferSlice -> MtlBufferSlice   { Rc<MtlBuffer>, offset, length }
//   DxvkResourceAllocation (allocateStorage / Discard) -> MtlSuballocation ring
//   DxvkImageViewKey -> MtlImageViewKey (view-pool cache key)
//
// Discard/rename (D3D9 MAP_DISCARD, contract 3a) = per-resource suballocated ring
// bumping a generation counter; in-flight draws keep the old (handle,generation),
// new Locks write the new one. The retirement fence (completion handler) is
// deadlock-class if stubbed — PHASE-C wires it in the backend impl.
// ---------------------------------------------------------------------------

namespace dxvk {

  // Forward-declared resource objects (defined below). PHASE-C: real Metal object
  // ownership, residency-set registration, deferred destruction.
  class MtlResource;
  class MtlBuffer;
  class MtlTexture;
  class MtlTextureView;
  class MtlTexelBufferView;
  class MtlSamplerState;
  class MtlAllocation;

  // --- POD handles (generation-stamped) ------------------------------------
  struct MtlBufferHandle {
    MtlObjectHandle buf        = MtlNullHandle;   // id<MTLBuffer> token
    uint32_t        generation = 0u;

    explicit operator bool () const { return buf != MtlNullHandle; }
    bool operator == (const MtlBufferHandle& o) const {
      return buf == o.buf && generation == o.generation;
    }
    bool operator != (const MtlBufferHandle& o) const { return !(*this == o); }
  };

  struct MtlTextureHandle {
    MtlObjectHandle tex        = MtlNullHandle;   // id<MTLTexture> token
    uint32_t        generation = 0u;

    explicit operator bool () const { return tex != MtlNullHandle; }
    bool operator == (const MtlTextureHandle& o) const {
      return tex == o.tex && generation == o.generation;
    }
    bool operator != (const MtlTextureHandle& o) const { return !(*this == o); }
  };

  // The renamed backing slice handed out by Discard (was DxvkResourceAllocation).
  struct MtlSuballocation {
    MtlBufferHandle backing    = { };
    MtlDeviceSize   offset     = 0;
    MtlDeviceSize   size       = 0;
    uint32_t        generation = 0u;   // bumped on every Discard
  };

  // --- MtlBufferSlice (was DxvkBufferSlice) --------------------------------
  // (offset,length) view into a buffer; underpins Lock/Discard. Holds an Rc to
  // the logical buffer (keeps it alive through the recorded lambda, contract 3h)
  // AND a snapshot of the PHYSICAL backing (MtlAllocation) taken when the slice
  // is constructed. GPU consumers (bind flush, indexed draws) resolve addresses
  // through the SNAPSHOT, never through the live buffer: a Discard rename on
  // another thread must not retarget a draw that was recorded against the old
  // backing (record-time snapshot semantics — the DXVK physical-slice model).
  // The only sanctioned re-resolution is MtlContext::invalidateBuffer, which
  // refreshes still-bound slices on the CS thread AFTER assignStorage, in
  // command order (bind-once-then-Discard patterns).
  class MtlBufferSlice {

  public:

    MtlBufferSlice() = default;

    // Snapshots the buffer's current storage. Only safe where no rename can
    // race: never-renamed buffers (real/staging/mapping), CS-thread code, or
    // creation-time slices. Rename-aware record paths must use the explicit-
    // storage constructor below. Defined after MtlBuffer.
    MtlBufferSlice(Rc<MtlBuffer> buffer, MtlDeviceSize offset, MtlDeviceSize length);

    // Explicit physical backing: frontends that rename on the app thread
    // (Lock DISCARD, the UP ring) pass the allocation the CPU actually wrote —
    // the buffer's own storage() only catches up when the CS executes the
    // matching assignStorage. Defined after MtlAllocation.
    MtlBufferSlice(Rc<MtlBuffer> buffer, Rc<MtlAllocation> storage,
                   MtlDeviceSize offset, MtlDeviceSize length);

    const Rc<MtlBuffer>&     buffer()  const { return m_buffer; }
    const Rc<MtlAllocation>& storage() const { return m_storage; }
    MtlDeviceSize            offset()  const { return m_offset; }
    MtlDeviceSize            length()  const { return m_length; }
    bool                     defined() const { return m_buffer != nullptr; }

    // GPU address of (slice origin + extra) in the SNAPSHOTTED backing.
    // Defined after MtlAllocation.
    uint64_t gpuAddress(MtlDeviceSize extra = 0) const;

    // Bytes addressable from (slice origin + extra) to the end of the
    // snapshotted backing (MTL4 draw APIs take an accessible range).
    MtlDeviceSize addressableLength(MtlDeviceSize extra = 0) const;

    // CS-thread only: re-snapshot the buffer's CURRENT storage. Called by
    // invalidateBuffer after assignStorage so live bindings follow a rename;
    // already-recorded draws keep the snapshot they were recorded against.
    void refreshStorage();

    // Sub-view within this slice (was DxvkBufferSlice::subSlice). Offset is
    // relative to this slice's origin; the storage snapshot carries over.
    // Defined after MtlAllocation.
    MtlBufferSlice subSlice(MtlDeviceSize offset, MtlDeviceSize length) const;

    // CPU pointer at (slice origin + offset) into the SNAPSHOTTED backing
    // (was DxvkBufferSlice::mapPtr). Defined after MtlAllocation.
    void* mapPtr(MtlDeviceSize offset) const;

  private:

    Rc<MtlBuffer>     m_buffer  = nullptr;
    Rc<MtlAllocation> m_storage = nullptr;
    MtlDeviceSize     m_offset  = 0;
    MtlDeviceSize     m_length  = 0;

  };

  // --- Image-view cache key (mirrors DxvkImageViewKey, doc 02 §2) -----------
  // Metal newTextureViewWithPixelFormat:… keyed by {format, swizzle, mip range,
  // layer range, usage, aspects}. Used by the texture view pool.
  struct MtlImageViewKey {
    MtlFormat          format     = MtlFormat::Invalid;
    MtlSwizzleChannels swizzle    = { };
    uint32_t           baseMip    = 0;
    uint32_t           mipCount   = 1;
    uint32_t           baseLayer  = 0;
    uint32_t           layerCount = 1;
    MtlTextureUsage    usage      = MtlTextureUsageShaderRead;
    MtlAspectMask      aspects    = MtlAspectColor;
    // Dimensionality hint (was VkImageViewType); the backend can also derive it
    // from layerCount + texture type. PHASE-E: fold into the real view descriptor.
    MtlTextureViewType viewType   = MtlTextureViewType::e2D;
    uint32_t           packedSwizzle = 0;   // some callers set a packed swizzle directly

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

  // Texel-buffer view cache key (was DxvkBufferViewKey). Minimal; the swvp-emu
  // draw-count buffer is the only D3D9 user.
  struct MtlTexelBufferViewKey {
    MtlFormat     format = MtlFormat::Invalid;
    MtlDeviceSize offset = 0;
    MtlDeviceSize length = 0;
  };

  // --- Rc<>-managed resource objects ---------------------------------------
  // Minimal complete definitions so the seam links at this phase. Each carries its
  // POD handle; real Metal object lifetime + residency is PHASE-C.

  class MtlResource : public RcObject {
  public:
    virtual ~MtlResource() { }

    // Serial of the last command buffer whose ENCODED commands may read or
    // write this resource on the GPU (was DxvkPagedResource use tracking).
    // Contract: the backend context stamps this at ENCODE time (CS thread)
    // with the serial of the currently recording command buffer — for every
    // draw that fetches a bound buffer and every transfer/init/resolve on
    // either operand. The resource is GPU-idle once the device's completed
    // serial reaches this value. Stamps are monotonic (CS encodes in order);
    // the app thread reads concurrently, hence the atomic.
    uint64_t trackingSerial() const { return m_trackingSerial.load(std::memory_order_acquire); }
    void setTrackingSerial(uint64_t serial) { m_trackingSerial.store(serial, std::memory_order_release); }

  private:
    std::atomic<uint64_t> m_trackingSerial = { 0u };
  };

  // The renamed backing storage of a buffer (was DxvkResourceAllocation).
  // Owns one Metal buffer allocation; MAP_DISCARD swaps a fresh one in and the
  // OLD one rides an Rc into the CS command stream, retiring only after the
  // last submission that referenced it completes (contract 3a).
  class MtlAllocation : public RcObject {
  public:
    // Rc<MtlAllocation> deletes through THIS type while the backend subclass
    // (MtlAllocationImpl) owns the Metal object, the host backing and the
    // residency-set entry. RcObject has no virtual destructor, so without
    // this the subclass destructor never ran: no allocation was ever freed
    // or removed from the residency set (the zero-removals leak).
    virtual ~MtlAllocation() { }

    MtlBufferHandle handle     = { };       // backing id<MTLBuffer> token
    void*           mapPtr     = nullptr;   // shared-storage CPU pointer
    uint64_t        gpuAddress = 0u;
    MtlDeviceSize   size       = 0;
    // render-server S2 identity: 0 until the CS thread's wire producer emits
    // a create_buffer record at first wire use (MtlWireContext). Written and
    // read on the CS thread; the destructor (any thread) only reads it —
    // ordered by the final Rc release.
    uint64_t        wireId     = 0u;
    // render-server S3 v8: WIRE CITIZENSHIP stamped AT CREATION. Only
    // backings minted by the canonical device paths (createBuffer /
    // renameBuffer) with validated-complete info are wire-eligible;
    // hand-built wrappers and internal temporaries never get a wire id and
    // dummy-resolve server-side. Never inferred at emission time.
    bool            wireEligible = false;
  };

  class MtlDevice;

  class MtlBuffer : public MtlResource {
  public:
    MtlBufferHandle handle = { };   // mirrors m_storage->handle (fast path)
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

    // Allocates FRESH backing of the same size (the Discard rename).
    // Implemented by the backend device; declared here so the frontend
    // port is a token swap of DxvkBuffer::allocateStorage.
    Rc<MtlAllocation> allocateStorage();

    // Swaps in new backing, bumps the generation, returns the OLD storage
    // (moved into the CS invalidateBuffer command for retirement).
    Rc<MtlAllocation> assignStorage(Rc<MtlAllocation>&& next) {
      Rc<MtlAllocation> prev = std::move(m_storage);
      m_storage = std::move(next);
      handle = m_storage->handle;
      handle.generation = prev ? prev->handle.generation + 1u : 0u;
      return prev;
    }

  protected:
    Rc<MtlAllocation> m_storage = nullptr;
    MtlDevice*        m_device  = nullptr;   // backend fills; for allocateStorage

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

  // Image metadata the frontend reads back through image->info() (was
  // DxvkImageCreateInfo, the subset d3d9/ touches). Populated by
  // MtlDevice::createImage; PHASE-E: backend fills every field from the real
  // MTLTextureDescriptor.
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
    MtlTextureHandle texture        = { };   // primary
    MtlTextureHandle resolveTexture = { };   // MSAA resolve target (optional)
    MtlFormat        format         = MtlFormat::Invalid;
    MtlSize          extent         = { };
    MtlImageInfo     m_info         = { };
    // render-server S2 identity (see MtlAllocation::wireId).
    uint64_t         wireId         = 0u;
    // render-server S3 v8: wire citizenship (see MtlAllocation::wireEligible).
    // Set ONLY by MtlDeviceImpl::createImage after validating the create
    // info is complete (all extents > 0, base format in the known enum);
    // presenter drawable wraps / dump temporaries stay ineligible — their
    // half-initialized info poisoned the server (run-8 "1x0x0" record).
    bool             wireEligible   = false;

    const MtlImageInfo& info() const { return m_info; }

    const MtlFormatInfo* formatInfo() const { return lookupFormatInfo(m_info.format); }

    // Image-owned view creation (was DxvkImage::createView) — delegates to the
    // device view pool. Defined inline in mtl_device.h once MtlDevice is complete.
    Rc<MtlTextureView> createView(const MtlImageViewKey& key);

    MtlDevice* m_device = nullptr;   // backend fills; for createView

    // Extent of a given mip (was DxvkImage::mipLevelExtent). Halves per level,
    // clamped to 1; 3D depth also shrinks.
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
    Rc<MtlTexture>   image  = nullptr;   // keep the backing texture alive
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
    MtlObjectHandle sampler = MtlNullHandle;   // id<MTLSamplerState> token
    // render-server S2 identity (see MtlAllocation::wireId).
    uint64_t        wireId  = 0u;
  };


  // Linear staging allocator over a shared MtlBuffer (was DxvkStagingBuffer).
  // The frontend suballocates upload space; retirement rides the standard
  // allocation lifetime (slices hold Rc to the buffer).
  class MtlStagingBuffer : public RcObject {
  public:
    MtlStagingBuffer() = default;   // value member of D3D9DeviceEx (was DxvkStagingBuffer)
    MtlStagingBuffer(Rc<MtlBuffer> buffer, MtlDeviceSize size)
    : m_buffer(std::move(buffer)), m_size(size) { }

    // Device-driven ctor (was DxvkStagingBuffer(device, size)); allocates the
    // backing shared buffer from the device. Defined in mtl_device.h.
    MtlStagingBuffer(const Rc<MtlDevice>& device, MtlDeviceSize size);

    // In-place re-init with a fresh backing (RcObject deletes assignment).
    void reinit(const Rc<MtlDevice>& device, MtlDeviceSize size);

    MtlBufferSlice alloc(MtlDeviceSize size, MtlDeviceSize align = 256u) {
      MtlDeviceSize offset = (m_offset + align - 1u) & ~(align - 1u);
      if (offset + size > m_size)
        return MtlBufferSlice();   // caller allocates a fresh staging buffer
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

