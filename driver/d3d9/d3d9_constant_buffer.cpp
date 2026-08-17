#include "d3d9_constant_buffer.h"
#include "d3d9_device.h"


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
    bufferInfo.usage     = m_usage;
    bufferInfo.cpuAccess = m_memType;

    if (m_useDma) {
      auto cpuInfo = bufferInfo;
      cpuInfo.usage     = MtlUsage::Staging;
      cpuInfo.cpuAccess = m_memType;

      m_cpuBuffer = dxvkDevice->createBuffer(cpuInfo);

      auto gpuInfo = bufferInfo;
      gpuInfo.cpuAccess = MtlCpuAccess::None;

      m_gpuBuffer = dxvkDevice->createBuffer(gpuInfo);
    } else {
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
      m_streamCmd->size = Offset + Size - m_streamCmd->offset;
    } else {
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
    return std::max<MtlDeviceSize>(CACHE_LINE_SIZE, 256u);
  }


  MtlCpuAccess D3D9ConstantBuffer::DetermineMemoryType(
          Kind                CbvType) {
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

    return false;
  }

}
