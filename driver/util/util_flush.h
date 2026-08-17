#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {

  struct GpuCostEstimate {
    static constexpr uint64_t Dispatch              = 4u;
    static constexpr uint64_t DispatchIndirect      = 5u;
    static constexpr uint64_t RenderPass            = 10u;
    static constexpr uint64_t Transfer              = 2u;

    static constexpr uint64_t MaxCostPerSubmission  = 1'500u;
  };


  enum class GpuFlushType : uint32_t {
    ExplicitFlush           = 0,
    ImplicitSynchronization = 1,
    ImplicitStrongHint      = 2,
    ImplicitWeakHint        = 3,

    None                    = ~0u
  };


  class GpuFlushTracker {

  public:

    GpuFlushTracker(GpuFlushType maxAllowed);

    GpuFlushType getPendingType() const {
      return m_lastMissedType;
    }

    bool considerFlush(
            GpuFlushType          flushType,
            uint64_t              chunkId,
            uint32_t              lastCompleteSubmissionId,
            uint64_t              estimatedCost);

    void notifyFlush(
            uint64_t              chunkId,
            uint64_t              submissionId);

  private:

    GpuFlushType  m_maxType               = GpuFlushType::ImplicitWeakHint;
    GpuFlushType  m_lastMissedType        = GpuFlushType::None;

    uint64_t      m_lastFlushChunkId      = 0ull;
    uint64_t      m_lastFlushSubmissionId = 0ull;

  };

}
