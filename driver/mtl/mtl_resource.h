#pragma once

#include <cstdint>

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
  // (offset,length) view into a buffer; underpins Lock/Discard. Holds an Rc to the
  // logical buffer so a slice moved into a recorded lambda keeps it alive to chunk
  // execution (contract 3h move-ownership).
  class MtlBufferSlice {

  public:

    MtlBufferSlice() = default;

    MtlBufferSlice(Rc<MtlBuffer> buffer, MtlDeviceSize offset, MtlDeviceSize length)
    : m_buffer(std::move(buffer)), m_offset(offset), m_length(length) { }

    const Rc<MtlBuffer>& buffer() const { return m_buffer; }
    MtlDeviceSize        offset() const { return m_offset; }
    MtlDeviceSize        length() const { return m_length; }
    bool                 defined() const { return m_buffer != nullptr; }

  private:

    Rc<MtlBuffer> m_buffer = nullptr;
    MtlDeviceSize m_offset = 0;
    MtlDeviceSize m_length = 0;

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

    bool operator == (const MtlImageViewKey& o) const {
      return format == o.format && swizzle == o.swizzle
          && baseMip == o.baseMip && mipCount == o.mipCount
          && baseLayer == o.baseLayer && layerCount == o.layerCount
          && usage == o.usage && aspects == o.aspects;
    }
    bool operator != (const MtlImageViewKey& o) const { return !(*this == o); }
  };

  // --- Rc<>-managed resource objects ---------------------------------------
  // Minimal complete definitions so the seam links at this phase. Each carries its
  // POD handle; real Metal object lifetime + residency is PHASE-C.

  class MtlResource : public RcObject {
  public:
    virtual ~MtlResource() { }
  };

  class MtlBuffer : public MtlResource {
  public:
    MtlBufferHandle handle = { };
    MtlDeviceSize   size   = 0;
  };

  class MtlTexture : public MtlResource {
  public:
    MtlTextureHandle texture        = { };   // primary
    MtlTextureHandle resolveTexture = { };   // MSAA resolve target (optional)
    MtlFormat        format         = MtlFormat::Invalid;
    MtlSize          extent         = { };
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
  };

  // The fresh backing slice produced by allocateStorage() during Discard.
  class MtlAllocation : public RcObject {
  public:
    MtlSuballocation slice = { };
  };

}
