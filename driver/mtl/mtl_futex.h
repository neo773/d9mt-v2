#ifndef D9MT_MTL_FUTEX_H
#define D9MT_MTL_FUTEX_H

/*
 * mtl_futex.h — canonical cross-process futex discipline for the d9mt wire
 * (os_sync_wait_on_address, macOS 14.4+, libSystem syscall wrappers).
 *
 * UNIX/SERVER SIDES ONLY. Users: server/consumer.m + server/executor.mm
 * (d9mt-serverd, arm64), bridge/unix/winemetal_unix.c (wine unix .so,
 * x86_64 under Rosetta — os_sync_* are plain libSystem syscall wrappers and
 * work under translation), server/test_feed.c (x86_64 gate). The i686 PE
 * producer can NOT call macOS APIs — it reaches these waits through the
 * unixcall 217 thunk (bridge/winemetal_thunks.h struct unixcall_wire_wait).
 *
 * Every address handed to these helpers lives in MAP_SHARED shm (the ring /
 * status files), so the SHARED flag is hardcoded: wait and wake match on the
 * file offset, not the per-process VA.
 *
 * CANONICAL DISCIPLINE (every call site follows this shape; the kernel
 * compares *addr against `expected` under its own lock, so a wake that races
 * the wait is never lost — the wait returns immediately when the value
 * already moved):
 *
 *     for (;;) {
 *       v = load-acquire(addr);
 *       if (condition(v)) break;                 // re-check FIRST
 *       if (deadline passed) break;              // bounded, always
 *       d9mt_futex_wait(addr, v, slice_ns);      // sleeps only if *addr==v
 *     }
 *
 * MEMORY-ORDERING CONTRACT (matches the ring ABI in mtl_wire.h):
 *   - head (producer -> consumer): record bytes are written BEFORE
 *     head.store(release); the consumer head.load(acquire)s before reading
 *     bytes. The producer's kick (wake on &head) is issued AFTER the head
 *     store in program order — the wake is an opaque syscall (a compiler
 *     barrier), and the kernel reads the LIVE value for its compare, so no
 *     extra fence is needed between store and wake.
 *   - tail (consumer -> producer): tail.store(release), then
 *     atomic_thread_fence(seq_cst), then load space_waiters; the waiter
 *     registers with fetch_add(seq_cst) BEFORE its acquire re-check of tail.
 *     This store/load handshake (both sides seq_cst-ordered around the
 *     flag) guarantees: either the consumer sees the registration and wakes,
 *     or the waiter's re-check sees the new tail — a missed pairing needs
 *     both sides to lose, which the total order forbids.
 *   - frame_serial (server -> client): the executor release-stores the
 *     serial, then wake_all unconditionally (one publish per frame — a
 *     no-waiter wake is a cheap ENOENT).
 *
 * TIMEOUT BACKSTOP: every wait is bounded. A lost/mis-paired wake degrades
 * to the caller's timeout — which is chosen to be at or under the latency of
 * the polling loop it replaced — and NEVER a hang: heartbeat/watchdog
 * semantics are preserved exactly.
 *
 * FALLBACK: if the syscall itself fails (unexpected errno — e.g. an OS
 * where the SHARED flag misbehaves), the wait degrades to a bounded usleep
 * of the same duration (capped at 1 ms so callers keep their re-check
 * cadence) and logs ONCE. The API is unchanged; the swap is this one
 * function body.
 */

#if defined(_WIN32) || defined(__MINGW32__)
#error "mtl_futex.h is unix/server-side only — the PE producer waits through unixcall 217"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <os/os_sync_wait_on_address.h>

/* Wait until *addr != expected, a wake arrives, or timeout_ns expires.
 * addr must point at a MAP_SHARED 8-byte atomic (ring head/tail, status
 * frame_serial). Returns 0 on wake/value-change/spurious, -1 on timeout.
 * The caller ALWAYS re-checks its condition — this is a hint, not a lock. */
static inline int
d9mt_futex_wait(void *addr, uint64_t expected, uint64_t timeout_ns) {
  if (timeout_ns == 0u)
    timeout_ns = 1u; /* 0 is EINVAL by contract */
  int rc = os_sync_wait_on_address_with_timeout(
      addr, expected, 8, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
      OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns);
  if (rc >= 0)
    return 0;
  if (errno == ETIMEDOUT)
    return -1;
  if (errno == EINTR || errno == EFAULT)
    return 0; /* spurious: caller re-checks the condition */
  /* Unexpected failure: degrade to a bounded sleep (documented fallback) so
   * the caller's re-check loop still advances at poll cadence. Logged once
   * per process so a gate run can't silently pass on the fallback path. */
  {
    static int s_warned /* = 0 */;
    if (!s_warned) {
      s_warned = 1;
      fprintf(stderr,
              "[d9mt_futex] os_sync_wait_on_address failed (errno=%d) — "
              "degrading to bounded usleep polls\n", errno);
    }
  }
  {
    uint64_t us = timeout_ns / 1000u;
    if (us > 1000u)
      us = 1000u;
    usleep(us ? (useconds_t)us : 1u);
  }
  return 0;
}

/* Wake one/all waiters parked on addr. Store (release) the observed value
 * FIRST, then wake — never reorder (the call itself is a compiler barrier).
 * Waking with no waiter parked is a cheap ENOENT; ignored by design. */
static inline void
d9mt_futex_wake_one(void *addr) {
  (void)os_sync_wake_by_address_any(addr, 8, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

static inline void
d9mt_futex_wake_all(void *addr) {
  (void)os_sync_wake_by_address_all(addr, 8, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

#endif /* D9MT_MTL_FUTEX_H */
