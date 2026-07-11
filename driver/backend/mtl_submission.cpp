#include "mtl_submission.h"

#include <thread>

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

  m_retirements.push_back({ serial, std::move(release) });
}

void SubmissionTracker::processRetirements() {
  if (m_retirements.empty())
    return;

  uint64_t completed = completedSerial();
  size_t write = 0u;

  for (size_t i = 0u; i < m_retirements.size(); i++) {
    if (m_retirements[i].serial <= completed) {
      m_retirements[i].release();
    } else {
      if (write != i)
        m_retirements[write] = std::move(m_retirements[i]);
      write++;
    }
  }

  m_retirements.resize(write);
}

void SubmissionTracker::waitFor(uint64_t serial) {
  /* Teardown/Reset only. Poll-yield is acceptable there; the render path
   * never calls this. Frame pacing uses setWin32EventAtValue in the
   * presenter instead (real wait, no spin). */
  while (!isComplete(serial))
    std::this_thread::yield();
}

} // namespace dxvk::backend
