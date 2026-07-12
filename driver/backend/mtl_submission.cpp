#include "mtl_submission.h"

#include <thread>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend {

SubmissionTracker::SubmissionTracker(obj_handle_t device) {
  m_event = WmtSharedEvent::adopt(MTLDevice_newSharedEvent(device));
}

uint64_t SubmissionTracker::onSubmit(obj_handle_t queue) {
  uint64_t serial = m_nextSerial++;
  MTL4CommandQueue_signalEvent(queue, m_event.handle(), serial);
  return serial;
}

uint64_t SubmissionTracker::completedSerial() {
  /* signaledValue is a unixlib round-trip; cache monotonically */
  uint64_t value = MTLSharedEvent_signaledValue(m_event.handle());

  if (value > m_completedCache)
    m_completedCache = value;

  return m_completedCache;
}

void SubmissionTracker::retireAfter(uint64_t serial, std::function<void()> release) {
  if (isComplete(serial)) {
    release();
    return;
  }

  std::lock_guard<std::mutex> lock(m_retireMutex);
  m_retirements.push_back({ serial, std::move(release) });
}

void SubmissionTracker::processRetirements() {
  /* Callable from the CS thread (endFrame) AND from an allocation-starved
   * app thread (MtlDeviceImpl host-memory retry) — take the ready callbacks
   * out under the lock, run them outside it (they do unix calls). */
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(m_retireMutex);
    if (m_retirements.empty())
      return;

    uint64_t completed = completedSerial();
    size_t write = 0u;

    for (size_t i = 0u; i < m_retirements.size(); i++) {
      if (m_retirements[i].serial <= completed) {
        ready.push_back(std::move(m_retirements[i].release));
      } else {
        if (write != i)
          m_retirements[write] = std::move(m_retirements[i]);
        write++;
      }
    }

    m_retirements.resize(write);
  }

  for (auto& release : ready)
    release();
}

void SubmissionTracker::waitFor(uint64_t serial) {
  if (isComplete(serial))
    return;
  /* Real blocking wait on the GPU-signaled shared event — NOT a yield spin
   * (a spin pinned a core and starved the CS thread). Bounded so a
   * faulted/never-signalled command buffer degrades to a logged stall
   * instead of an unkillable hang. Retry in 2s slices up to ~10s. */
  for (int i = 0; i < 5; i++) {
    if (MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), serial, 2000u)) {
      completedSerial();
      return;
    }
    completedSerial();
    if (isComplete(serial))
      return;
    Logger::warn(str::format("SubmissionTracker: GPU stall waiting serial ",
      serial, " (completed ", m_completedCache, ") — slice ", i + 1));
  }
  Logger::err(str::format("SubmissionTracker: GPU never signalled serial ",
    serial, " (completed ", m_completedCache, ") — abandoning wait"));
}

} // namespace dxvk::backend
