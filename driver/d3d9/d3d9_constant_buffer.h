#pragma once

#include "../mtl/mtl_device.h"

#include "../util/util_math.h"

#include "d3d9_include.h"
#include "d3d9_shader.h"

namespace dxvk {

  class D3D9DeviceEx;

  class D3D9ConstantBuffer {
    static constexpr MtlDeviceSize ShaderConstSize = 1024ull << 10;
    static constexpr MtlDeviceSize MiscSize        = 1024ull << 10;

    using Kind = D3D9ShaderResourceMapping::CbvIndex;
  public:

    struct StreamCommand {
      MtlDeviceSize offset = 0u;
      MtlDeviceSize size = 0u;
    };

    D3D9ConstantBuffer();

    D3D9ConstantBuffer(
            D3D9DeviceEx*         pDevice,
            Kind                  CbvType);

    ~D3D9ConstantBuffer();

    MtlDeviceSize GetAlignment() const {
      return m_align;
    }

    void* Alloc(MtlDeviceSize size);

    template<typename T>
    T* AllocTyped(size_t Count) {
      return reinterpret_cast<T*>(Alloc(Count * sizeof(T)));
    }

    void ResetStreamCommand() {
      m_streamCmd = nullptr;
    }

  private:

    D3D9DeviceEx*         m_device  = nullptr;

    Kind                  m_kind    = {};
    MtlDeviceSize         m_size    = 0ull;
    MtlUsage              m_usage   = MtlUsage::None;
    MtlStageMask          m_stages  = 0u;
    MtlDeviceSize         m_align   = 0ull;
    MtlCpuAccess          m_memType = MtlCpuAccess::None;
    MtlDeviceSize         m_offset  = 0ull;
    bool                  m_useDma  = false;

    Rc<MtlBuffer>        m_cpuBuffer = nullptr;
    Rc<MtlBuffer>        m_gpuBuffer = nullptr;

    Rc<MtlAllocation>    m_cpuSlice = nullptr;

    StreamCommand*        m_streamCmd = nullptr;

    Rc<MtlAllocation> CreateBuffer();

    void SetupStreamCommand(
            MtlDeviceSize       Offset,
            MtlDeviceSize       Size,
            bool                Discard);

    static MtlDeviceSize DetermineSize(
            Kind                CbvType);

    static MtlUsage DetermineUsage(
            D3D9DeviceEx*       pDevice,
            Kind                CbvType,
            MtlDeviceSize       Size);

    static MtlStageMask DetermineStages(
            Kind                CbvType);

    static MtlDeviceSize DetermineAlignment(
            D3D9DeviceEx*       pDevice,
            MtlUsage            Usage);

    static MtlCpuAccess DetermineMemoryType(
            Kind                CbvType);

    static bool DetermineDmaUsage(
            D3D9DeviceEx*       pDevice,
            Kind                CbvType);

  };

}
