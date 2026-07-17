/*
 * consumer.m — d9mt-serverd S1 consumer loop.
 *
 * Cross-process port of the in-process wire consumer
 * (bridge/unix/winemetal_unix.c wire_consumer_main): identical decode,
 * framing validation and error policy over the SPSC ring in
 * driver/mtl/mtl_wire.h — but the ring now lives in a MAP_SHARED file and
 * the producer is a Rosetta-translated wine process.
 *
 * Differences from the in-process consumer, all S1-deliberate:
 *   - PARK: no condvar/kick can cross the process boundary in S1, so an
 *     empty ring parks on a 1 ms poll (usleep). TODO(S2): kick channel
 *     (pipe/kqueue EVFILT_USER or os_sync futex) to kill the poll.
 *   - PROGRESS: ring->tail is mirrored into status->progress after every
 *     record — the client-visible consumption cursor.
 *   - LIVENESS: status->heartbeat ticks ~100 Hz whether draining or idle.
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
  uint64_t last_hb_ms = now_ms();
  uint64_t last_stat_ms = last_hb_ms;

  for (;;) {
    /* liveness first: heartbeat ~100 Hz, drain or idle */
    uint64_t now = now_ms();
    if (now - last_hb_ms >= 10) {
      atomic_fetch_add_explicit(&status->heartbeat, 1, memory_order_relaxed);
      last_hb_ms = now;
    }
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
      usleep(1000); /* S1 park: no cross-process kick channel yet */
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
         * count it and drop everything up to the published head. */
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
        if (hdr->opcode == d9mt_wire_op_detach)
          detached = 1;
        else
          d9mt_executor_record(hdr);
        tail += hdr->size;
      }
      /* Publish per record: releases ring space promptly; tail doubles as
       * the progress counter, mirrored client-visibly into status. */
      atomic_store_explicit(&ring->tail, tail, memory_order_release);
      atomic_store_explicit(&status->progress, tail, memory_order_release);
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

  atomic_fetch_add_explicit(&status->heartbeat, 1, memory_order_relaxed);
  atomic_store_explicit(&ring->consumer_exited, 1, memory_order_release);
  return 0;
}
