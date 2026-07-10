#pragma once

// PHASE-C6: ported off Vulkan onto the Mtl seam (doc 05 §Buffers — constant-buffer
// ring row: usage UniformBuffer, cpuAccess Write).
//   Rc<DxvkBuffer>             -> Rc<MtlBuffer>
//   Rc<DxvkResourceAllocation> -> Rc<MtlAllocation>   (the Discard rename ring)
//   VkDeviceSize               -> MtlDeviceSize
//   VkBufferUsageFlags         -> MtlUsage
//   VkShaderStageFlags         -> MtlStageMask
//   VkMemoryPropertyFlags      -> MtlCpuAccess
// The Determine*() bodies (d3d9_constant_buffer.cpp) do the semantic rewrite; this
// header is a pure token retype. Removed from the Vulkan-gate whitelist.
#include "../mtl/mtl_device.h"

#include "../util/util_math.h"

#include "d3d9_include.h"
#include "d3d9_shader.h"

namespace dxvk {

  class D3D9DeviceEx;

  /**
   * \brief Constant buffer
   */
  class D3D9ConstantBuffer {
    static constexpr MtlDeviceSize ShaderConstSize = 1024ull << 10;
    static constexpr MtlDeviceSize MiscSize        =   64ull << 10;

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

    /**
     * \brief Queries alignment
     *
     * Useful to pad copies with initialized data.
     * \returns Data alignment
     */
    MtlDeviceSize GetAlignment() const {
      return m_align;
    }

    /**
     * \brief Allocates a given amount of memory
     *
     * \note: Requires that all data is written to the buffer before
     * submitting the next set of commands to the CS chunk, otherwise
     * the GPU may read stale data.
     * \param [in] size Number of bytes to allocate
     * \returns Map pointer of the allocated region
     */
    void* Alloc(MtlDeviceSize size);

    /**
     * \brief Allocates typed data
     *
     * \param [in] count Number of items to allocate
     * \returns Allocated data slice
     */
    template<typename T>
    T* AllocTyped(size_t Count) {
      return reinterpret_cast<T*>(Alloc(Count * sizeof(T)));
    }

    /**
     * \brief Resets current stream command
     *
     * This \e must unconditionally be called when the
     * device's current CS chunk gets submitted to the
     * worker to avoid accessing stale data.
     */
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
