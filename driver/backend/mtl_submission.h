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
 */

#include <cstdint>
#include <functional>
#include <vector>

#include "wmt_handle.h"

namespace dxvk::backend {

class SubmissionTracker {

public:

  explicit SubmissionTracker(obj_handle_t device);

  /* Serial the NEXT submission will carry. */
  uint64_t nextSerial() const { return m_nextSerial; }

  /* Encodes the GPU-side signal into the command buffer and returns the
   * serial it will signal. Call once per cmdbuf, last before commit. */
  uint64_t onSubmit(obj_handle_t cmdbuf);

  /* Latest serial the GPU has fully completed (reads the shared event). */
  uint64_t completedSerial();

  /* True once the given serial has completed. */
  bool isComplete(uint64_t serial) {
    return serial <= completedSerial();
  }

  /* Defers destruction of a resource until the given serial completes.
   * The callback runs on the CS thread inside processRetirements(). */
  void retireAfter(uint64_t serial, std::function<void()> release);

  /* Runs pending retirements against the current completed serial.
   * Call once per frame (and on any explicit sync point). */
  void processRetirements();

  /* CPU-blocks until the serial completes. ONLY for teardown/Reset —
   * never on the per-draw path. */
  void waitFor(uint64_t serial);

private:

  struct Retirement {
    uint64_t serial;
    std::function<void()> release;
  };

  WmtSharedEvent m_event;
  uint64_t m_nextSerial = 1u;
  uint64_t m_completedCache = 0u;

  std::vector<Retirement> m_retirements;

};

} // namespace dxvk::backend
