/*
 * test_feed.c — S1 test gate for d9mt-serverd.
 *
 * A minimal synthetic CLIENT, built x86_64 on purpose so the test crosses
 * the same Rosetta<->native boundary as the real wine client (PoC P1/P4).
 * Plain C11, no frameworks.
 *
 *   usage: test_feed <path-to-d9mt-serverd>
 *
 * Sequence:
 *   1. mkdtemp a shm dir; create `ring` (d9mt_wire_ring header + 1 MiB data)
 *      and `status` (struct d9mt_status, zeroed), both mmap(MAP_SHARED).
 *   2. Produce 1000 valid wire records (5 opcode kinds incl. trailing
 *      payloads) into the ring, release-publishing head per record.
 *   3. posix_spawn the arm64 server with argv = {dir, "640", "480"}.
 *   4. Wait for epoch==1, progress == produced bytes, records==1000,
 *      decode_errors==0, 3 distinct nonzero IOSurfaceIDs, and a heartbeat
 *      that advances over 200 ms.
 *   5. Write the Detach record; server must consume it and exit 0 with
 *      consumer_exited==1.
 * Exit 0 = PASS. Any deviation prints FAIL and exits 1.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../driver/mtl/mtl_wire.h"
#include "d9mt_status.h"

extern char **environ;

#define RING_CAP (1u << 20)
#define FAIL(...)                                                              \
  do {                                                                         \
    fprintf(stderr, "FAIL: " __VA_ARGS__);                                     \
    fprintf(stderr, "\n");                                                     \
    return 1;                                                                  \
  } while (0)

static struct d9mt_wire_ring *g_ring;
static uint8_t *g_data;
static uint64_t g_head; /* producer-local free-running cursor */

/* Producer (single-threaded here): write one record, publish head. Handles
 * the never-straddle-the-wrap rule with an op_pad header (not hit with a
 * 1 MiB ring and ~34 KiB of records, but kept correct by construction). */
static void
emit(const void *rec, uint16_t size) {
  uint64_t idx = g_head & (RING_CAP - 1u);
  if ((uint64_t)size > RING_CAP - idx) {
    struct d9mt_wire_rec_hdr pad = {d9mt_wire_op_pad, 8, 0};
    memcpy(g_data + idx, &pad, sizeof(pad));
    g_head += RING_CAP - idx;
    idx = 0;
  }
  memcpy(g_data + idx, rec, size);
  g_head += size;
  atomic_store_explicit(&g_ring->head, g_head, memory_order_release);
}

static int
create_file(const char *dir, const char *name, size_t bytes, void **out) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  int fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)bytes)) {
    perror(path);
    return 1;
  }
  *out = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (*out == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  return 0;
}

int
main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_feed <path-to-d9mt-serverd>\n");
    return 2;
  }
  const char *server_bin = argv[1];

  /* 1. shm dir + files */
  const char *tmp = getenv("TMPDIR");
  char dir[900];
  snprintf(dir, sizeof(dir), "%s/d9mt-test-XXXXXX", tmp ? tmp : "/tmp");
  if (!mkdtemp(dir))
    FAIL("mkdtemp: %s", strerror(errno));
  printf("[test_feed] shm dir: %s\n", dir);

  void *ring_mem, *status_mem;
  if (create_file(dir, "ring", sizeof(struct d9mt_wire_ring) + RING_CAP,
                  &ring_mem) ||
      create_file(dir, "status", sizeof(struct d9mt_status), &status_mem))
    return 1;
  g_ring = ring_mem;
  g_data = (uint8_t *)ring_mem + sizeof(struct d9mt_wire_ring);
  struct d9mt_status *status = status_mem;

  g_ring->capacity = RING_CAP;
  g_ring->data_offset = sizeof(struct d9mt_wire_ring);
  g_ring->magic = D9MT_WIRE_RING_MAGIC; /* last: header now valid */

  /* 2. 1000 records, 5 kinds x 200 cycles (incl. trailing payloads) */
  for (uint32_t i = 0; i < 200; i++) {
    struct d9mt_wire_rec_set_stencil_reference sr = {
        {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, i, 0};
    emit(&sr, sizeof(sr));

    struct d9mt_wire_rec_set_blend_color bc = {
        {d9mt_wire_op_set_blend_color, sizeof(bc), 0}, {0.f, 0.5f, 1.f, 1.f}};
    emit(&bc, sizeof(bc));

    struct {
      struct d9mt_wire_rec_draw rec;
      struct d9mt_wire_draw_args args[1];
    } dr = {{{d9mt_wire_op_draw, sizeof(dr), 0}, 1, 0}, {{3, 1, 0, 0}}};
    emit(&dr, sizeof(dr));

    struct {
      struct d9mt_wire_rec_push_data rec;
      uint8_t payload[32];
    } pd = {{{d9mt_wire_op_push_data, sizeof(pd), 0}, 3, 0, 32, 0}, {0}};
    memset(pd.payload, (int)(i & 0xff), sizeof(pd.payload));
    emit(&pd, sizeof(pd));

    struct d9mt_wire_rec_bind_texture bt = {
        {d9mt_wire_op_bind_texture, sizeof(bt), 0}, 1, i & 15, 0x1000 + i,
        0xABCD0000 + i, 7, 1};
    emit(&bt, sizeof(bt));
  }
  const uint64_t produced = g_head;
  printf("[test_feed] produced 1000 records, %" PRIu64 " bytes\n", produced);

  /* 3. spawn the native server */
  pid_t pid;
  char *sargv[] = {(char *)server_bin, dir, "640", "480", NULL};
  int rc = posix_spawn(&pid, server_bin, NULL, NULL, sargv, environ);
  if (rc)
    FAIL("posix_spawn(%s): %s", server_bin, strerror(rc));

  /* 4a. epoch==1 within 10 s (Metal/IOSurface init included) */
  for (int i = 0;; i++) {
    if (atomic_load_explicit(&status->epoch, memory_order_acquire) == 1)
      break;
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid)
      FAIL("server died before ready (status 0x%x)", st);
    if (i >= 1000)
      FAIL("timeout waiting for epoch==1");
    usleep(10000);
  }
  printf("[test_feed] epoch=1 (server ready)\n");

  /* 4b. all produced bytes consumed within 5 s */
  for (int i = 0;; i++) {
    if (atomic_load_explicit(&status->progress, memory_order_acquire) ==
        produced)
      break;
    if (i >= 500)
      FAIL("timeout: progress=%" PRIu64 " want %" PRIu64,
           (uint64_t)atomic_load_explicit(&status->progress,
                                          memory_order_acquire),
           produced);
    usleep(10000);
  }
  uint64_t records =
      atomic_load_explicit(&g_ring->records, memory_order_relaxed);
  uint64_t errors =
      atomic_load_explicit(&g_ring->decode_errors, memory_order_relaxed);
  if (records != 1000)
    FAIL("records=%" PRIu64 " want 1000", records);
  if (errors)
    FAIL("decode_errors=%" PRIu64, errors);
  if (atomic_load_explicit(&status->error, memory_order_acquire))
    FAIL("status.error=%u",
         atomic_load_explicit(&status->error, memory_order_acquire));
  printf("[test_feed] progress=%" PRIu64 " records=1000 decode_errors=0\n",
         produced);

  /* 4c. three distinct nonzero IOSurface ids, serial 1 */
  uint32_t ids[3];
  for (int i = 0; i < 3; i++) {
    ids[i] = atomic_load_explicit(&status->iosurface_id[i],
                                  memory_order_acquire);
    if (!ids[i])
      FAIL("iosurface_id[%d]==0", i);
    if (atomic_load_explicit(&status->iosurface_serial[i],
                             memory_order_relaxed) != 1)
      FAIL("iosurface_serial[%d]!=1", i);
  }
  if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])
    FAIL("iosurface ids not distinct (%u %u %u)", ids[0], ids[1], ids[2]);
  printf("[test_feed] iosurface ids: %u %u %u\n", ids[0], ids[1], ids[2]);

  /* 4d. heartbeat advances while idle (~100 Hz => >= a few ticks in 200 ms) */
  uint64_t hb0 = atomic_load_explicit(&status->heartbeat, memory_order_acquire);
  usleep(200000);
  uint64_t hb1 = atomic_load_explicit(&status->heartbeat, memory_order_acquire);
  if (hb1 <= hb0)
    FAIL("heartbeat stuck at %" PRIu64, hb0);
  printf("[test_feed] heartbeat: %" PRIu64 " -> %" PRIu64 " over 200ms\n", hb0,
         hb1);

  /* 5. detach => server consumes it and exits 0 */
  struct d9mt_wire_rec_nul detach = {{d9mt_wire_op_detach, 8, 0}};
  emit(&detach, sizeof(detach));
  int st = -1, exited = 0;
  for (int i = 0; i < 500; i++) {
    if (waitpid(pid, &st, WNOHANG) == pid) {
      exited = 1;
      break;
    }
    usleep(10000);
  }
  if (!exited)
    FAIL("server did not exit after detach (kill %d)", (int)pid);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    FAIL("server exit status 0x%x", st);
  if (atomic_load_explicit(&g_ring->consumer_exited, memory_order_acquire) != 1)
    FAIL("consumer_exited not set");
  if (atomic_load_explicit(&status->progress, memory_order_acquire) != g_head)
    FAIL("final progress mismatch");
  if (atomic_load_explicit(&g_ring->records, memory_order_relaxed) != 1001)
    FAIL("final records != 1001 (detach not counted)");
  printf("[test_feed] clean detach: exit 0, consumer_exited=1, records=1001\n");

  /* teardown */
  char p[1024];
  snprintf(p, sizeof(p), "%s/ring", dir);
  unlink(p);
  snprintf(p, sizeof(p), "%s/status", dir);
  unlink(p);
  rmdir(dir);
  printf("PASS\n");
  return 0;
}
