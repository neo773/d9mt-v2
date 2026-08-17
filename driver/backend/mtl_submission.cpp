#include "mtl_submission.h"

#include <thread>

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend {

SubmissionTracker::SubmissionTracker(obj_handle_t device) {
  m_event = WmtSharedEvent::adopt(MTLDevice_newSharedEvent(device));
  m_pump = dxvk::thread([this] { pumpThread(); });
}

SubmissionTracker::~SubmissionTracker() {
  drainAndStopPump();
}

void SubmissionTracker::drainAndStopPump() {
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    if (m_stop) {
      return;
    }
    m_stop = true;
  }
  m_retireCond.notify_all();
  m_pump.join();

  // Drain the GPU, then run every remaining retirement here: dropping one would leak captured Rcs after the residency tracker is gone.
  waitFor(lastSubmittedSerial());

  std::vector<Retirement> remaining;
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    remaining = std::move(m_retirements);
  }
  for (Retirement& r : remaining)
    r.release();
}

uint64_t SubmissionTracker::onSubmit(obj_handle_t queue) {
  uint64_t serial = m_nextSerial.fetch_add(1u, std::memory_order_acq_rel);
  MTL4CommandQueue_signalEvent(queue, m_event.handle(), serial);
  return serial;
}

uint64_t SubmissionTracker::completedSerial() {
  uint64_t value = MTLSharedEvent_signaledValue(m_event.handle());

  uint64_t cached = m_completedCache.load(std::memory_order_acquire);
  while (value > cached
      && !m_completedCache.compare_exchange_weak(cached, value,
            std::memory_order_acq_rel, std::memory_order_acquire)) { }

  return value > cached ? value : cached;
}

void SubmissionTracker::retireAfter(uint64_t serial, std::function<void()> release) {
  if (isComplete(serial)) {
    release();
    return;
  }

  { std::lock_guard<std::mutex> lock(m_retireMutex);
    m_retirements.push_back({ serial, std::move(release) });
  }
  m_retireCond.notify_one();
}

void SubmissionTracker::processRetirements() {
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

void SubmissionTracker::pumpThread() {
  env::setThreadName("d9mt-retire");

  std::unique_lock<std::mutex> lock(m_retireMutex);

  while (true) {
    m_retireCond.wait(lock, [this] {
      return m_stop || !m_retirements.empty();
    });

    if (m_stop)
      return;

    uint64_t target = ~0ull;
    for (const Retirement& r : m_retirements)
      target = r.serial < target ? r.serial : target;

    lock.unlock();

    if (!isComplete(target))
      MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), target, 500u);

    processRetirements();

    lock.lock();
  }
}

void SubmissionTracker::waitFor(uint64_t serial) {
  if (isComplete(serial))
    return;
  // Bounded blocking wait, not a spin, so a never-signalled command buffer degrades to a logged stall instead of a hang.
  for (int i = 0; i < 5; i++) {
    if (MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), serial, 2000u)) {
      completedSerial();
      return;
    }
    completedSerial();
    if (isComplete(serial))
      return;
    Logger::warn(str::format("SubmissionTracker: GPU stall waiting serial ",
      serial, " (completed ", completedSerial(), ") — slice ", i + 1));
  }
  Logger::err(str::format("SubmissionTracker: GPU never signalled serial ",
    serial, " (completed ", completedSerial(), ") — abandoning wait"));
}

}
