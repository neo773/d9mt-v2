#pragma once

// A dedicated pump thread observes GPU completion independently of the CS
// thread — an app thread blocked on a fence that only a retirement callback
// can signal would otherwise deadlock waiting for CS activity that never comes.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "wmt_handle.h"

#include "../util/thread.h"

namespace dxvk::backend {

class SubmissionTracker {

public:

  explicit SubmissionTracker(obj_handle_t device);

  ~SubmissionTracker();

  uint64_t nextSerial() const { return m_nextSerial.load(std::memory_order_acquire); }

  uint64_t lastSubmittedSerial() const { return nextSerial() - 1u; }

  size_t retirementCount() { std::lock_guard<std::mutex> l(m_retireMutex); return m_retirements.size(); }

  uint64_t onSubmit(obj_handle_t queue);

  uint64_t completedSerial();

  bool isComplete(uint64_t serial) {
    return serial <= completedSerial();
  }

  void retireAfter(uint64_t serial, std::function<void()> release);

  void processRetirements();

  void waitFor(uint64_t serial);

  void drainAndStopPump();

private:

  struct Retirement {
    uint64_t serial;
    std::function<void()> release;
  };

  WmtSharedEvent m_event;
  std::atomic<uint64_t> m_nextSerial = { 1u };
  std::atomic<uint64_t> m_completedCache = { 0u };

  std::mutex m_retireMutex;
  std::condition_variable m_retireCond;
  std::vector<Retirement> m_retirements;
  bool m_stop = false;

  dxvk::thread m_pump;

  void pumpThread();

};

}
