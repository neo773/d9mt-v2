#include "d3d9_constant_buffer.h"
#include "d3d9_device.h"

// PHASE-C6 STAGE-2: ported off Vulkan onto the Mtl seam (doc 05 §Buffers
// constant-buffer ring; doc 01 §1d bindUniformBuffer(Range) / §1h invalidateBuffer,
// uploadBuffer). The 4 EmitCs/EmitCsCmd sites map straight onto MtlContext. The
// Vulkan usage/stage/access/memory split dies in the Determine*() bodies.

namespace dxvk {

  D3D9ConstantBuffer::D3D9ConstantBuffer() {

  }


  D3D9ConstantBuffer::D3D9ConstantBuffer(
          D3D9DeviceEx*         pDevice,
          Kind                  CbvType)
  : m_device    (pDevice)
  , m_kind      (CbvType)
  , m_size      (DetermineSize(CbvType))
  , m_usage     (DetermineUsage(pDevice, CbvType, m_size))
  , m_stages    (DetermineStages(CbvType))
  , m_align     (DetermineAlignment(pDevice, m_usage))
  , m_memType   (DetermineMemoryType(CbvType))
  , m_useDma    (DetermineDmaUsage(pDevice, CbvType)) {

  }


  D3D9ConstantBuffer::~D3D9ConstantBuffer() {

  }


  void* D3D9ConstantBuffer::Alloc(MtlDeviceSize size) {
    if (unlikely(!m_cpuBuffer))
      m_cpuSlice = CreateBuffer();

    size = align(size, m_align);

    if (m_offset + size > m_size) {
      m_cpuSlice = m_cpuBuffer->allocateStorage();

      m_device->EmitCs([
        cBinding    = uint32_t(m_kind),
        cStages     = m_stages,
        cCpuBuffer  = m_cpuBuffer,
        cNewSlice   = m_cpuSlice,
        cSize       = size
      ] (MtlContext* ctx) mutable {
        Rc<MtlAllocation> oldSlice =
          cCpuBuffer->assignStorage(std::move(cNewSlice));
        ctx->invalidateBuffer(cCpuBuffer, std::move(oldSlice));
        ctx->bindUniformBufferRange(cStages, cBinding, 0u, cSize);
      });

      SetupStreamCommand(0u, size, true);

      m_offset = size;
      return m_cpuSlice->mapPtr;
    } else {
      m_device->EmitCs([
        cBinding  = uint32_t(m_kind),
        cStages   = m_stages,
        cOffset   = m_offset,
        cSize     = size
      ] (MtlContext* ctx) {
        ctx->bindUniformBufferRange(cStages, cBinding, cOffset, cSize);
      });

      SetupStreamCommand(m_offset, size, false);

      void* mapPtr = reinterpret_cast<char*>(m_cpuSlice->mapPtr) + m_offset;
      m_offset += size;
      return mapPtr;
    }
  }


  Rc<MtlAllocation> D3D9ConstantBuffer::CreateBuffer() {
    auto dxvkDevice = m_device->GetDXVKDevice();

    MtlBufferCreateInfo bufferInfo;
    bufferInfo.size      = align(m_size, m_align);
    bufferInfo.usage     = m_usage;              // UniformBuffer
    bufferInfo.cpuAccess = m_memType;            // Write (Read for cached kinds)

    if (m_useDma) {
      // Create the CPU buffer as a pure staging buffer.
      auto cpuInfo = bufferInfo;
      cpuInfo.usage     = MtlUsage::Staging;
      cpuInfo.cpuAccess = m_memType;

      m_cpuBuffer = dxvkDevice->createBuffer(cpuInfo);

      // Create the GPU buffer as the actual uniform buffer (device-private).
      auto gpuInfo = bufferInfo;
      gpuInfo.cpuAccess = MtlCpuAccess::None;

      m_gpuBuffer = dxvkDevice->createBuffer(gpuInfo);
    } else {
      // Create a single buffer for both CPU writes and GPU reads.
      m_cpuBuffer = dxvkDevice->createBuffer(bufferInfo);
      m_gpuBuffer = m_cpuBuffer;
    }

    m_device->EmitCs([
      cBinding  = uint32_t(m_kind),
      cStages   = m_stages,
      cSlice    = MtlBufferSlice(m_gpuBuffer, 0u, m_gpuBuffer->size)
    ] (MtlContext* ctx) mutable {
      ctx->bindUniformBuffer(cStages, cBinding, std::move(cSlice));
    });

    return m_cpuBuffer->storage();
  }


  void D3D9ConstantBuffer::SetupStreamCommand(
          MtlDeviceSize       Offset,
          MtlDeviceSize       Size,
          bool                Discard) {
    if (!m_useDma)
      return;

    if (m_streamCmd && !Discard) {
      // Simply adjust the data size for the existing command
      m_streamCmd->size = Offset + Size - m_streamCmd->offset;
    } else {
      // Need to discard GPU buffer as well so that we can safely
      // use the asynchronous transfer queue for uploads.
      auto block = m_device->EmitCsCmd<StreamCommand>(D3D9CmdType::None, 1u, [
        cCpuBuffer = m_cpuBuffer,
        cGpuBuffer = m_gpuBuffer,
        cDiscard   = Discard
      ] (MtlContext* ctx, const StreamCommand* cmd, uint32_t) {
        if (cDiscard) {
          Rc<MtlAllocation> oldGpuSlice =
            cGpuBuffer->assignStorage(cGpuBuffer->allocateStorage());
          ctx->invalidateBuffer(cGpuBuffer, std::move(oldGpuSlice));
        }

        ctx->uploadBuffer(cGpuBuffer, cmd->offset, cCpuBuffer, cmd->offset, cmd->size);
      });

      m_streamCmd = new (block->first()) StreamCommand();
      m_streamCmd->offset = Offset;
      m_streamCmd->size = Size;
    }
  }


  MtlDeviceSize D3D9ConstantBuffer::DetermineSize(
          Kind                CbvType) {
    switch (CbvType) {
      case Kind::VSStaticConstants:
      case Kind::VSDynamicConstants:
      case Kind::PSStaticConstants:
        return ShaderConstSize;

      default:
        return MiscSize;
    }
  }


  MtlUsage D3D9ConstantBuffer::DetermineUsage(
          D3D9DeviceEx*       pDevice,
          Kind                CbvType,
          MtlDeviceSize       Size) {
    // doc 05 §Buffers row 3: constant-buffer ring => UniformBuffer.
    // PHASE-E: verify — SWVP/vertex-blend CBs added a storage-buffer fallback
    // (nonuniform indexing / large size); no Storage usage bit in the seam yet.
    return MtlUsage::UniformBuffer;
  }


  MtlStageMask D3D9ConstantBuffer::DetermineStages(
          Kind                CbvType) {
    switch (CbvType) {
      case Kind::VSClipPlanes:
      case Kind::VSFixedFunction:
      case Kind::VSVertexBlendData:
      case Kind::VSStaticConstants:
      case Kind::VSDynamicConstants:
        return MtlStageVertex;

      case Kind::PSShared:
      case Kind::PSStaticConstants:
        return MtlStageFragment;

      // FF spec data feeds both the VS and PS ubershaders.
      case Kind::SpecData:
        return MtlStageVertex | MtlStageFragment;

      case Kind::Count:
        break;
    }

    return 0u;
  }


  MtlDeviceSize D3D9ConstantBuffer::DetermineAlignment(
          D3D9DeviceEx*       pDevice,
          MtlUsage            Usage) {
    // At least a full cache line for CPU write perf; 256B covers the Metal
    // constant-buffer offset alignment (was minUniform/StorageBufferOffsetAlignment
    // + robustness alignment). PHASE-E: query the real device constant alignment.
    return std::max<MtlDeviceSize>(CACHE_LINE_SIZE, 256u);
  }


  MtlCpuAccess D3D9ConstantBuffer::DetermineMemoryType(
          Kind                CbvType) {
    // Host-visible|coherent => Write. Cached (read-friendly) for the misc buffers.
    bool useCached = CbvType == Kind::VSClipPlanes
                  || CbvType == Kind::VSFixedFunction
                  || CbvType == Kind::VSVertexBlendData
                  || CbvType == Kind::PSShared;

    return useCached ? MtlCpuAccess::Read : MtlCpuAccess::Write;
  }


  bool D3D9ConstantBuffer::DetermineDmaUsage(
          D3D9DeviceEx*       pDevice,
          Kind                CbvType) {
    auto option = pDevice->GetOptions()->deviceLocalConstantBuffers;

    if (option != Tristate::Auto)
      return option == Tristate::True;

    // Apple Silicon is UMA (no discrete GPU) => no DMA staging split by default.
    return false;
  }

}
