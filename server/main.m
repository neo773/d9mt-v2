/*
 * main.m — d9mt-serverd entry point (render server, Phase S2).
 *
 * Native arm64 helper process (docs/RENDER_SERVER_SPEC.md). Spawned by the
 * client (wine/x86_64) with argv[1] = the shm directory it created:
 *   <dir>/ring    d9mt_wire_ring control block + data (driver/mtl/mtl_wire.h)
 *   <dir>/status  struct d9mt_status (server/d9mt_status.h), server-written
 *
 * usage: d9mt-serverd <shm-dir> [width height]     (default 2992x1934)
 *
 * Boot order matters for the client handshake: map ring, map status, create
 * the IOSurface pool (IDs published into status), init the executor (MTL4
 * queue/compiler + the GPU liveness clear; publishes status version=2),
 * THEN store epoch=1 (release) — the client's "server ready" gate.
 * Everything after that is the consumer loop, which now EXECUTES records;
 * exit code 0 = clean detach/shutdown/unlink.
 */

#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "d9mt_server.h"

static void *
map_file(const char *dir, const char *name, size_t min_bytes, size_t *out_bytes) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "[serverd] open(%s): %s\n", path, strerror(errno));
    return NULL;
  }
  struct stat st = {0};
  if (fstat(fd, &st) || (size_t)st.st_size < min_bytes) {
    fprintf(stderr, "[serverd] %s too small (%lld < %zu)\n", path,
            (long long)st.st_size, min_bytes);
    close(fd);
    return NULL;
  }
  void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, 0);
  close(fd); /* mapping keeps the file alive; fd not needed */
  if (p == MAP_FAILED) {
    fprintf(stderr, "[serverd] mmap(%s): %s\n", path, strerror(errno));
    return NULL;
  }
  if (out_bytes)
    *out_bytes = (size_t)st.st_size;
  return p;
}

int
main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: d9mt-serverd <shm-dir> [width height]\n");
    return 2;
  }
  const char *dir = argv[1];
  uint32_t width = 2992, height = 1934;
  if (argc >= 4) {
    width = (uint32_t)strtoul(argv[2], NULL, 10);
    height = (uint32_t)strtoul(argv[3], NULL, 10);
    if (!width || !height) {
      fprintf(stderr, "[serverd] bad size %s x %s\n", argv[2], argv[3]);
      return 2;
    }
  }

  /* status first so boot failures are reportable through status->error */
  struct d9mt_status *status =
      map_file(dir, "status", sizeof(struct d9mt_status), NULL);
  if (!status)
    return 2; /* nothing to report through — client times out on epoch */

  size_t ring_bytes = 0;
  struct d9mt_wire_ring *ring =
      map_file(dir, "ring", sizeof(struct d9mt_wire_ring), &ring_bytes);
  if (!ring || ring->magic != D9MT_WIRE_RING_MAGIC || ring->capacity == 0 ||
      (ring->capacity & (ring->capacity - 1u)) ||
      ring->data_offset != sizeof(struct d9mt_wire_ring) ||
      ring_bytes < ring->data_offset + ring->capacity) {
    fprintf(stderr, "[serverd] bad ring header\n");
    atomic_store_explicit(&status->error, d9mt_status_err_ring_map,
                          memory_order_release);
    return 2;
  }

  /* QoS discipline (P-core-preferred): every frame-critical server thread —
   * this one runs the ring consumer AND the executor's encode work — must
   * declare USER_INTERACTIVE at start, or macOS schedules the helper like a
   * background daemon onto E-cores. No hard pinning exists; QoS is the
   * mechanism. S3: any additional thread (kick listener, encode workers)
   * repeats this line. */
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

  int rc;
  @autoreleasepool {
    /* IOSurface pool (ids published into status), then the S2 executor
     * (MTL4 stack + the GPU liveness clear + status v2 version field). */
    rc = d9mt_iosurface_pool_init(status, width, height);
    if (!rc)
      rc = d9mt_executor_init(status);
  }
  if (rc)
    return 2; /* status->error already set */

  /* Ready: the client's spawn gate. Everything above is published. */
  atomic_store_explicit(&status->heartbeat, 1, memory_order_release);
  atomic_store_explicit(&status->epoch, 1, memory_order_release);
  fprintf(stderr, "[serverd] ready: pid=%d dir=%s ring=%llu B surfaces=%ux%u\n",
          getpid(), dir, (unsigned long long)ring->capacity, width, height);

  char ring_path[1024];
  snprintf(ring_path, sizeof(ring_path), "%s/ring", dir);
  rc = d9mt_consumer_run(ring, status, ring_path);

  fprintf(stderr, "[serverd] exit rc=%d\n", rc);
  return rc;
}
