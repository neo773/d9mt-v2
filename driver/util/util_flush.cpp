#include "util_flush.h"
#include "util_string.h"
#include "log/log.h"

namespace dxvk {

  GpuFlushTracker::GpuFlushTracker(GpuFlushType maxType)
  : m_maxType(maxType) {

  }

  bool GpuFlushTracker::considerFlush(
          GpuFlushType          flushType,
          uint64_t              chunkId,
          uint32_t              lastCompleteSubmissionId,
          uint64_t              estimatedCost) {
    constexpr uint32_t minPendingSubmissions = 2;

    constexpr uint32_t minChunkCount =  3u;
    constexpr uint32_t maxChunkCount = 20u;

    uint32_t chunkCount = uint32_t(chunkId - m_lastFlushChunkId);

    if (!chunkCount)
      return false;

    if (flushType > m_maxType)
      return estimatedCost >= GpuCostEstimate::MaxCostPerSubmission;

    flushType = std::min(flushType, m_lastMissedType);

    if (flushType != GpuFlushType::ImplicitSynchronization)
      m_lastMissedType = flushType;

    switch (flushType) {
      case GpuFlushType::ExplicitFlush: {
        return true;
      }

      case GpuFlushType::ImplicitStrongHint: {
        return chunkCount >= minChunkCount;
      }

      case GpuFlushType::ImplicitWeakHint: {
        if (chunkCount < 2 * minChunkCount)
          return false;

      } [[fallthrough]];

      case GpuFlushType::ImplicitSynchronization: {
        uint32_t pendingSubmissions = uint32_t(m_lastFlushSubmissionId - lastCompleteSubmissionId);

        if (pendingSubmissions < minPendingSubmissions)
          return true;

        uint32_t threshold = std::min(maxChunkCount, pendingSubmissions * minChunkCount);
        return chunkCount >= threshold;
      }

      case GpuFlushType::None:
        return false;
    }

    return false;
  }


  void GpuFlushTracker::notifyFlush(
          uint64_t              chunkId,
          uint64_t              submissionId) {
    m_lastMissedType = GpuFlushType::None;

    m_lastFlushChunkId = chunkId;
    m_lastFlushSubmissionId = submissionId;
  }

}
