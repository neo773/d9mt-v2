#pragma once
/*
 * Submission serial tracking — the retirement spine.
 *
 * Every command-buffer submission gets a monotonically increasing serial,
 * signaled GPU-side through one MTLSharedEvent. Everything lifetime-shaped
 * hangs off it:
 *  - UploadRing chunk recycling (never reuse memory a live cmdbuf reads)
 *  - MAP_DISCARD renamed-backing retirement (contract 3a: old allocation
 *    releases only after the last cmdbuf that referenced it completes)
 *  - D3DQUERYTYPE_EVENT GetData (contract 3d: answer = has the serial the
 *    query was issued under passed? — never a CPU spin on a stub)
 *  - frame-latency pacing (contract 3e feeds off completed serials)
 *
 * Four deadlock classes, one primitive. v1/v2 history: every one of these
 * was independently stubbed at some point and every stub deadlocked or
 * corrupted; they must share this single source of truth.
 *
 * Retirement delivery: callbacks run wherever completion is observed —
 * the CS thread (submit/endFrame) AND a dedicated pump thread that blocks
 * on the shared event for the lowest pending serial. The pump exists
 * because retirement must NOT depend on future frontend activity: an app
 * thread blocked on a fence that a retirement callback signals (staging
 * throttle during loads, no presents in flight) would otherwise deadlock —
 * the CS thread idles, nobody observes the GPU finishing, the signal never
 * fires. Callbacks must therefore be safe off the CS thread (they already
 * were: the host-memory retry path runs them on app threads).
 */

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

  /* Serial the NEXT submission will carry. */
  uint64_t nextSerial() const { return m_nextSerial.load(std::memory_order_acquire); }

  /* Serial of the most recent submission (0 = nothing submitted yet). */
  uint64_t lastSubmittedSerial() const { return nextSerial() - 1u; }

  /* diag: pending (not-yet-retired) callback count. */
  size_t retirementCount() { std::lock_guard<std::mutex> l(m_retireMutex); return m_retirements.size(); }

  /* Enqueues the GPU-side signal onto the MTL4 command queue and returns the
   * serial it will signal. METAL 4: MTL4CommandBuffer has no
   * encodeSignalEvent — the signal is a queue-timeline op, so call this
   * once immediately AFTER MTL4CommandQueue_commit for the cmdbuf. */
  uint64_t onSubmit(obj_handle_t queue);

  /* Latest serial the GPU has fully completed (reads the shared event). */
  uint64_t completedSerial();

  /* True once the given serial has completed. */
  bool isComplete(uint64_t serial) {
    return serial <= completedSerial();
  }

  /* Defers destruction of a resource until the given serial completes.
   * The callback runs from processRetirements() — on the CS thread or the
   * pump thread, whichever observes completion first (each callback runs
   * exactly once; it must not assume a particular thread). */
  void retireAfter(uint64_t serial, std::function<void()> release);

  /* Runs pending retirements against the current completed serial.
   * Called per submit/frame on the CS thread and by the pump. */
  void processRetirements();

  /* CPU-blocks until the serial completes. ONLY for teardown/Reset/
   * Lock-with-sync — never on the per-draw path. */
  void waitFor(uint64_t serial);

  /* Teardown gate: drains the GPU, joins the pump thread and runs every
   * remaining retirement on the calling thread. MUST run before the
   * objects retirement callbacks touch (residency tracker, upload ring)
   * are destroyed — after this, no callback runs asynchronously. */
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

} // namespace dxvk::backend
