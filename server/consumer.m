/*
 * consumer.m — d9mt-serverd S1 consumer loop.
 *
 * Cross-process port of the in-process wire consumer
 * (bridge/unix/winemetal_unix.c wire_consumer_main): identical decode,
 * framing validation and error policy over the SPSC ring in
 * driver/mtl/mtl_wire.h — but the ring now lives in a MAP_SHARED file and
 * the producer is a Rosetta-translated wine process.
 *
 * Differences from the in-process consumer:
 *   - PARK: an empty ring parks on a kernel futex wait on &ring->head
 *     (os_sync_wait_on_address SHARED — mtl_futex.h owns the discipline).
 *     The producer's kick (unixcall 212) wakes the address after its head
 *     release-store; the 1 ms timeout backstop equals the old poll latency
 *     on a lost wake and keeps the stat/pid cadences below intact.
 *   - PROGRESS: ring->tail is mirrored into status->progress after every
 *     record — the client-visible consumption cursor.
 *   - LIVENESS (V19): status->heartbeat is NOT owned here. The executor's
 *     watchdog thread bumps it every 10 ms unconditionally — liveness is a
 *     property of the process, not of work arriving. This loop owning it
 *     was the mutual-starvation bug: any long record handler froze the
 *     heartbeat, the client latched dead-server and stopped producing, the
 *     ring drained, and both sides starved forever while healthy.
 *   - EXIT: Detach opcode (courtesy end-of-stream), ring->shutdown flag, or
 *     the ring file disappearing (client died / tore down without detach —
 *     checked ~10 Hz via stat) all end the loop cleanly.
 *
 * Phase S2: every framed record is handed to the executor
 * (d9mt_executor_record — executor.mm), which EXECUTES it against the
 * server's own Metal 4 device (dual-execution validation: the client still
 * executes everything locally too). Counting/validation semantics from S1
 * are unchanged; executor problems count into status v2 exec_errors and
 * never abort the stream.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "d9mt_server.h"
#include "../driver/mtl/mtl_futex.h"

static uint64_t
now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int
d9mt_consumer_run(struct d9mt_wire_ring *ring, struct d9mt_status *status,
                  const char *ring_path, pid_t client_pid) {
  uint8_t *data = (uint8_t *)ring + ring->data_offset;
  const uint64_t cap = ring->capacity;
  uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
  uint64_t op_counts[d9mt_wire_op_count];
  memset(op_counts, 0, sizeof(op_counts));

  int detached = 0;
  int unlinked = 0;
  int client_dead = 0;
  uint64_t last_stat_ms = now_ms();

  /* EVERY pass of this loop — a kick, a 1 ms park timeout, or falling out
   * of a drain burst — does exactly this (V19 audit of the wake path):
   *   1. ~10 Hz (stat-clocked): ring file unlink check + client PID
   *      liveness check — the two exit-without-detach fallbacks.
   *   2. exit-flag check (unlinked/detached/client_dead breaks the loop).
   *   3. reload ring->head (acquire); empty => shutdown-flag check, then
   *      re-park; nonempty => drain.
   * NOT here (V19): the heartbeat — the executor watchdog owns it. */
  for (;;) {
    uint64_t now = now_ms();
    /* client-death fallback: ring file unlinked => exit (~10 Hz check).
     * S3 v5 (process-lifetime server): also watch the CLIENT PID — the
     * server survives every device/context churn and owns its own cleanup
     * when the client process dies (crash or exit without teardown). */
    if (now - last_stat_ms >= 100) {
      struct stat st;
      if (stat(ring_path, &st) != 0) {
        fprintf(stderr, "[serverd] ring file gone, exiting\n");
        unlinked = 1;
      }
      if (client_pid && kill(client_pid, 0) != 0 && errno == ESRCH) {
        fprintf(stderr, "[serverd] client pid %d gone, exiting + cleaning shm\n",
                (int)client_pid);
        client_dead = 1;
      }
      last_stat_ms = now;
    }
    if (unlinked || detached || client_dead)
      break;

    uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head == tail) {
      if (atomic_load_explicit(&ring->shutdown, memory_order_acquire))
        break;
      /* Kernel park: the producer kicks &head after its head release-store
       * (value-compare in the kernel closes the race with this wait). The
       * 1 ms backstop bounds a lost wake to the old poll latency and keeps
       * the unlink+pid checks (~10 Hz) above alive. */
      d9mt_futex_wait(&ring->head, head, 1000u * 1000u /* 1 ms */);
      continue;
    }

    while (tail != head) {
      uint64_t idx = tail & (cap - 1u);
      const struct d9mt_wire_rec_hdr *hdr =
          (const struct d9mt_wire_rec_hdr *)(data + idx);
      if (hdr->opcode == d9mt_wire_op_pad) {
        /* skip-to-ring-end marker; its size field is ignored by contract */
        tail += cap - idx;
      } else if (hdr->opcode >= d9mt_wire_op_count ||
                 hdr->size < sizeof(*hdr) || (hdr->size & 7u) ||
                 (uint64_t)hdr->size > cap - idx ||
                 head - tail < (uint64_t)hdr->size) {
        /* A malformed stream cannot be resynced (lengths chain the walk):
         * count it and drop everything up to the published head. S3 v11:
         * NAMED — this is status error code 4 (srvErr=4), and the drop
         * loses every record in [tail, head): creates/uploads emitted in
         * that window never resolve again (dummy binds forever). */
        { static int n = 0;
          if (n++ < 8)
            fprintf(stderr, "[serverd] DECODE ERROR #%d: opcode=%u size=%u at "
                            "tail=%llu head=%llu (dropping %llu bytes to head)\n",
                    n, hdr->opcode, hdr->size, (unsigned long long)tail,
                    (unsigned long long)head, (unsigned long long)(head - tail)); }
        atomic_fetch_add_explicit(&ring->decode_errors, 1,
                                  memory_order_relaxed);
        atomic_store_explicit(&status->error, d9mt_status_err_decode,
                              memory_order_release);
        tail = head;
      } else {
        op_counts[hdr->opcode] += 1;
        atomic_fetch_add_explicit(&ring->records, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&ring->bytes, hdr->size,
                                  memory_order_relaxed);
        /* S2: execute BEFORE publishing tail — the record's ring bytes must
         * stay valid while the executor reads them (the producer may reuse
         * the space the moment tail advances past it). */
        if (hdr->opcode == d9mt_wire_op_detach) {
          detached = 1;
        } else {
          /* S3 v6 self-naming breadcrumb: opcode+1 while INSIDE a handler.
           * A frozen heartbeat with nonzero exec_opcode names the hung
           * record from the client's PROF line. */
          atomic_store_explicit(&status->exec_opcode,
                                (uint32_t)hdr->opcode + 1u,
                                memory_order_relaxed);
          d9mt_executor_record(hdr);
          atomic_store_explicit(&status->exec_opcode, 0, memory_order_relaxed);
        }
        tail += hdr->size;
      }
      /* Publish per record: releases ring space promptly; tail doubles as
       * the progress counter, mirrored client-visibly into status. */
      atomic_store_explicit(&ring->tail, tail, memory_order_release);
      atomic_store_explicit(&status->progress, tail, memory_order_release);
      /* Space-waiter handoff (mtl_futex.h contract): the seq_cst fence
       * pairs with the waiter's seq_cst registration in unixcall 217 —
       * either we see the registration and wake, or the waiter's re-check
       * sees the tail store above. The load is ~always 0 (waiters exist
       * only on ring-full), so the hot loop pays one fence, no syscall. */
      atomic_thread_fence(memory_order_seq_cst);
      if (atomic_load_explicit(&ring->space_waiters, memory_order_relaxed))
        d9mt_futex_wake_all(&ring->tail);
      if (detached)
        break;
    }
  }

  /* close the frame/GPU work before reporting (bounded drain inside) */
  d9mt_executor_finish();

  /* S3 v5: a dead client cannot unlink — the server cleans its own shm */
  if (client_dead) {
    char path[1024];
    size_t n = strlen(ring_path);
    if (n < sizeof(path) && n >= 5) {
      snprintf(path, sizeof(path), "%s", ring_path);
      unlink(path); /* .../ring */
      path[n - 4] = 0;
      char f[1024];
      snprintf(f, sizeof(f), "%sstatus", path);
      unlink(f);
      snprintf(f, sizeof(f), "%sheap", path);
      unlink(f);
      path[n - 5] = 0; /* strip trailing '/' too */
      rmdir(path);
    }
  }

  /* final counters (mirrors the in-process consumer's exit report) */
  fprintf(stderr,
          "[serverd] consumer exit (%s): records=%llu bytes=%llu "
          "decode_errors=%llu\n",
          detached ? "detach"
                   : (unlinked ? "unlink"
                               : (client_dead ? "client-dead" : "shutdown")),
          (unsigned long long)atomic_load_explicit(&ring->records,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&ring->bytes,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&ring->decode_errors,
                                                   memory_order_relaxed));
  for (unsigned op = 0; op < d9mt_wire_op_count; op++)
    if (op_counts[op])
      fprintf(stderr, "[serverd]   op %3u: %llu\n", op,
              (unsigned long long)op_counts[op]);

  /* one farewell tick: executor_finish above joined the watchdog (the V19
   * heartbeat owner), so this is the last heartbeat this process writes */
  atomic_fetch_add_explicit(&status->heartbeat, 1, memory_order_relaxed);
  atomic_store_explicit(&ring->consumer_exited, 1, memory_order_release);
  /* Release any producer parked on tail: it re-checks consumer_exited and
   * fails fast instead of burning its bounded timeout. */
  d9mt_futex_wake_all(&ring->tail);
  return 0;
}
