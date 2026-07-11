#include "d3d9_common_buffer.h"

#include "d3d9_device.h"
#include "d3d9_util.h"

// PHASE-C6 STAGE-2: ported off Vulkan onto the Mtl seam (doc 05 §Buffers).
// Only CreateBuffer()/CreateStagingBuffer() are semantic rewrites — the Vulkan
// usage/stage/access split dies here, folded into MtlBufferCreateInfo{usage,
// cpuAccess}. Everything else is D3D9 logic copied byte-identical.

namespace dxvk {

  D3D9CommonBuffer::D3D9CommonBuffer(
          D3D9DeviceEx*      pDevice,
    const D3D9_BUFFER_DESC*  pDesc)
    : m_parent ( pDevice ), m_desc ( *pDesc ),
      m_mapMode(DetermineMapMode(pDevice->GetOptions())) {
    m_buffer = CreateBuffer();
    if (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      m_stagingBuffer = CreateStagingBuffer();

    m_allocation = GetMapBuffer()->storage();

    if (m_desc.Pool != D3DPOOL_DEFAULT)
      m_dirtyRange = D3D9Range(0, m_desc.Size);

    m_uploadAtDraw = m_parent->GetOptions()->forceDrawTimeBufferUpload;
  }

  D3D9CommonBuffer::~D3D9CommonBuffer() {
    if (m_desc.Pool == D3DPOOL_DEFAULT)
      m_parent->DecrementLosableCounter();
  }


  HRESULT D3D9CommonBuffer::Lock(
          UINT   OffsetToLock,
          UINT   SizeToLock,
          void** ppbData,
          DWORD  Flags) {
    return m_parent->LockBuffer(
      this,
      OffsetToLock,
      SizeToLock,
      ppbData,
      Flags);
  }


  HRESULT D3D9CommonBuffer::Unlock() {
    return m_parent->UnlockBuffer(this);
  }


  HRESULT D3D9CommonBuffer::ValidateBufferProperties(const D3D9_BUFFER_DESC* pDesc, const bool IsExtended) {
    if (unlikely(pDesc->Size == 0))
      return D3DERR_INVALIDCALL;

    // Neither vertex nor index buffers can be created in D3DPOOL_SCRATCH
    // or in D3DPOOL_MANAGED with D3DUSAGE_DYNAMIC. On extended devices,
    // D3DPOOL_MANAGED can not be used at all, regardless of usage flags.
    if (unlikely(pDesc->Pool == D3DPOOL_SCRATCH
             || (pDesc->Pool == D3DPOOL_MANAGED && (IsExtended ||
                                                    pDesc->Usage & D3DUSAGE_DYNAMIC))))
      return D3DERR_INVALIDCALL;

    // D3DUSAGE_AUTOGENMIPMAP, D3DUSAGE_DEPTHSTENCIL and D3DUSAGE_RENDERTARGET
    // are not permitted on index or vertex buffers.
    if (unlikely((pDesc->Usage & D3DUSAGE_AUTOGENMIPMAP)
              || (pDesc->Usage & D3DUSAGE_DEPTHSTENCIL)
              || (pDesc->Usage & D3DUSAGE_RENDERTARGET)))
      return D3DERR_INVALIDCALL;

    return D3D_OK;
  }


  void D3D9CommonBuffer::PreLoad() {
    if (IsPoolManaged(m_desc.Pool)) {
      auto lock = m_parent->LockDevice();

      if (NeedsUpload())
        m_parent->FlushBuffer(this);
    }
  }


  D3D9_COMMON_BUFFER_MAP_MODE D3D9CommonBuffer::DetermineMapMode(const D3D9Options* options) const {
    if (m_desc.Pool != D3DPOOL_DEFAULT)
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    if (!(m_desc.Usage & D3DUSAGE_DYNAMIC))
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    if (!options->allowDirectBufferMapping)
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    return D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
  }


  Rc<MtlBuffer> D3D9CommonBuffer::CreateBuffer() const {
    // doc 05 §Buffers row 1: DEFAULT-pool VB/IB. Vulkan usage/stage/access
    // masks die; only the bind-slot usage + CPU storage mode survive.
    MtlBufferCreateInfo info;
    info.size      = m_desc.Size;
    info.usage     = MtlUsage::None;
    info.cpuAccess = MtlCpuAccess::None;

    if (m_desc.Type == D3DRTYPE_VERTEXBUFFER) {
      info.usage = info.usage | MtlUsage::VertexBuffer;
      // PHASE-E: verify — SWVP emu writes VB via a GPU pass (was STORAGE_BUFFER +
      // geometry-stage write). No Storage usage bit in the seam yet; VertexBuffer
      // usage is blit/compute-capable on Metal so writes still land.
    }
    else if (m_desc.Type == D3DRTYPE_INDEXBUFFER) {
      info.usage = info.usage | MtlUsage::IndexBuffer;
    }

    if (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT) {
      // Directly-mapped dynamic buffer: shared storage with a CPU pointer.
      // (HOST_VISIBLE|HOST_COHERENT[|HOST_CACHED] => Write; Read iff readable.)
      info.cpuAccess = MtlCpuAccess::Write;

      if ((m_desc.Usage & D3DUSAGE_WRITEONLY) == 0
        || DoPerDrawUpload()
        || m_parent->CanOnlySWVP()
        || m_parent->GetOptions()->cachedWriteOnlyBuffers) {
        // Never use uncached memory on devices that support SWVP because we
        // might end up reading from it.
        info.cpuAccess = MtlCpuAccess::Read;
      }
    }
    else {
      // The device-local real buffer, fed through staging (DEVICE_LOCAL only).
      info.cpuAccess = MtlCpuAccess::None;
    }

    return m_parent->GetDXVKDevice()->createBuffer(info);
  }


  Rc<MtlBuffer> D3D9CommonBuffer::CreateStagingBuffer() const {
    // doc 05 §Buffers row 2: SYSTEMMEM/MANAGED backing the GPU never binds.
    MtlBufferCreateInfo info;
    info.size      = m_desc.Size;
    info.usage     = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Write;

    if (!(m_desc.Usage & D3DUSAGE_WRITEONLY))
      info.cpuAccess = MtlCpuAccess::Read;

    return m_parent->GetDXVKDevice()->createBuffer(info);
  }

}
