/*
 * test_feed.c — S1+S2+S3 test gate for d9mt-serverd.
 *
 * A minimal synthetic CLIENT, built x86_64 on purpose so the test crosses
 * the same Rosetta<->native boundary as the real wine client (PoC P1/P4).
 * Plain C11 + the IOSurface C API (pixel verification).
 *
 *   usage: test_feed <path-to-d9mt-serverd> [test_stub.metallib]
 *
 * Sequence:
 *   1. mkdtemp a shm dir; create `ring` (d9mt_wire_ring header + 8 MiB
 *      data), `status` (struct d9mt_status, zeroed) and `heap`
 *      (d9mt_wire_heap header + 1 MiB data), all mmap(MAP_SHARED).
 *   2. Produce 1000 valid wire records (5 opcode kinds incl. trailing
 *      payloads) into the ring, release-publishing head per record.
 *   3. posix_spawn the arm64 server with argv = {dir, "640", "480"}.
 *   4. Wait for epoch==1, progress == produced bytes, records==1000,
 *      decode_errors==0, 3 distinct nonzero IOSurfaceIDs, and a heartbeat
 *      that advances over 200 ms.
 *   S2/S3 frames (async PSO semantics: the FIRST frame touching a fresh
 *      pipeline skips its draws while the compile is in flight — counted in
 *      draws_skipped_pending_pso — and a follow-up frame draws for real):
 *      F1  red shaders + clear blue + 50 draws + present  -> skips counted
 *      F2  (after psos_built>=1) 50 draws + present       -> pixel is RED,
 *          read ONCE with no poll: frame publication is GPU-completion-gated
 *      F3  64x64 checkerboard heap upload + textured quad + present (warm)
 *      F4  textured quad + present -> pixel readback asserts checker cells
 *   COMPILE STORM: 64 distinct shader pairs (fresh libraries), each bound +
 *      drawn immediately (draws arrive faster than compiles can finish) +
 *      present. Asserts: the producer NEVER waited for ring space, frames
 *      keep presenting, draws_skipped_pending_pso grew; then — once
 *      psos_built caught up and psos_pending drained to 0 — a second wave of
 *      64 draws executes completely.
 *   5. Write the Detach record; server must consume it and exit 0 with
 *      consumer_exited==1; heap->consumed must equal the produced heap head
 *      (every region acked after the finish drain).
 * Exit 0 = PASS. Any deviation prints FAIL and exits 1.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <IOSurface/IOSurface.h>

#include "../driver/mtl/mtl_wire.h"
#include "d9mt_status.h"

extern char **environ;

#define RING_CAP (8u << 20)
#define HEAP_CAP (8u << 20)
#define FAIL(...)                                                              \
  do {                                                                         \
    fprintf(stderr, "FAIL: " __VA_ARGS__);                                     \
    fprintf(stderr, "\n");                                                     \
    return 1;                                                                  \
  } while (0)

static struct d9mt_wire_ring *g_ring;
static uint8_t *g_data;
static uint64_t g_head;    /* producer-local free-running cursor */
static uint64_t g_records; /* records emitted (final ring->records check) */
static uint64_t g_producer_waits; /* ring-full episodes (gate: must stay 0) */

static struct d9mt_wire_heap *g_heap;
static uint8_t *g_heap_data;
static uint64_t g_heap_head;

/* Producer-side space wait: the S3 gate asserts the consumer (async PSO
 * builds!) drains fast enough that this NEVER trips. Bounded so a dead
 * server fails loudly instead of hanging the gate. */
static int
waitspace(uint64_t need) {
  for (int i = 0; i < 2000; i++) {
    uint64_t tail = atomic_load_explicit(&g_ring->tail, memory_order_acquire);
    if (g_head + need - tail <= RING_CAP)
      return 0;
    if (i == 0)
      g_producer_waits++;
    usleep(5000);
  }
  return 1;
}

/* Write one record, publish head. Handles the never-straddle-the-wrap rule
 * with an op_pad header. */
static void
emit(const void *rec, uint16_t size) {
  uint64_t idx = g_head & (RING_CAP - 1u);
  if ((uint64_t)size > RING_CAP - idx) {
    if (waitspace(RING_CAP - idx)) {
      fprintf(stderr, "FAIL: ring space wait timed out (pad)\n");
      exit(1);
    }
    struct d9mt_wire_rec_hdr pad = {d9mt_wire_op_pad, 8, 0};
    memcpy(g_data + idx, &pad, sizeof(pad));
    g_head += RING_CAP - idx;
    idx = 0;
  }
  if (waitspace(size)) {
    fprintf(stderr, "FAIL: ring space wait timed out\n");
    exit(1);
  }
  memcpy(g_data + idx, rec, size);
  g_head += size;
  g_records++;
  atomic_store_explicit(&g_ring->head, g_head, memory_order_release);
}

/* Heap bump allocator (mirror of the client contract: 256-aligned regions,
 * never straddling the wrap; generation = free-running END cursor). Unlike
 * the real client (which stashes/drops on a full heap), the test WAITS for
 * the server's consumed ack — pushing 300 MB through an 8 MiB heap is the
 * direct assertion that acks keep flowing under load. A bounded timeout
 * fails loudly (acks stalled = the S3 v2 regression). */
static uint64_t g_heap_waits; /* full-heap episodes (backpressure worked) */

static uint64_t
heap_put(const void *bytes, uint64_t size, uint64_t *gen) {
  uint64_t need = (size + 255u) & ~255ull;
  uint64_t start = g_heap_head;
  uint64_t idx = start & (HEAP_CAP - 1u);
  if (need > HEAP_CAP - idx)
    start += HEAP_CAP - idx;
  for (int i = 0, waited = 0;; i++) {
    uint64_t consumed = atomic_load_explicit(&g_heap->consumed, memory_order_acquire);
    if (start + need - consumed <= HEAP_CAP)
      break;
    if (!waited) {
      g_heap_waits++;
      waited = 1;
    }
    if (i >= 15000) {
      fprintf(stderr, "FAIL: heap ack wait timed out (server acks not flowing)\n");
      exit(1);
    }
    usleep(1000);
  }
  memcpy(g_heap_data + (start & (HEAP_CAP - 1u)), bytes, size);
  g_heap_head = start + need;
  atomic_store_explicit(&g_heap->head, g_heap_head, memory_order_release);
  *gen = g_heap_head;
  return start;
}

/* Chunked create_shader emission (mirrors the client's chunking).
 * cbv_map: 8-entry CbvIndex -> AIR buffer-slot map (NULL = no CBVs). */
static void
emit_shader_map(uint64_t wire_id, uint64_t client_handle, uint32_t stage,
                const char *entry, const uint8_t *lib, size_t lib_size,
                const int8_t *cbv_map) {
  const uint32_t kChunk = 0xE000u;
  uint64_t off = 0;
  while (off < lib_size || lib_size == 0) {
    uint32_t chunk = lib_size - off > kChunk ? kChunk : (uint32_t)(lib_size - off);
    uint32_t payload = (chunk + 7u) & ~7u;
    uint16_t size = (uint16_t)(sizeof(struct d9mt_wire_rec_create_shader) + payload);
    uint8_t buf[sizeof(struct d9mt_wire_rec_create_shader) + 0xE008];
    memset(buf, 0, size);
    struct d9mt_wire_rec_create_shader *r = (void *)buf;
    r->hdr.opcode = d9mt_wire_op_create_shader;
    r->hdr.size = size;
    r->wire_id = wire_id;
    r->client_handle = client_handle;
    r->stage = stage;
    r->total_size = lib_size;
    r->chunk_offset = off;
    r->chunk_size = chunk;
    if (cbv_map)
      memcpy(r->cbv_slot_map, cbv_map, 8);
    else
      memset(r->cbv_slot_map, 0xFF, 8); /* -1: no CBVs */
    snprintf(r->entry, sizeof(r->entry), "%s", entry);
    memcpy(r + 1, lib + off, chunk);
    emit(buf, size);
    off += chunk;
    if (off >= lib_size)
      break;
  }
}

static void
emit_shader(uint64_t wire_id, uint64_t client_handle, uint32_t stage,
            const char *entry, const uint8_t *lib, size_t lib_size) {
  emit_shader_map(wire_id, client_handle, stage, entry, lib, lib_size, NULL);
}

static void
emit_bind_shader(uint32_t stage, uint64_t handle) {
  struct d9mt_wire_rec_bind_shader b = {
      {d9mt_wire_op_bind_shader, sizeof(b), 0}, stage, 0, handle};
  emit(&b, sizeof(b));
}

static void
emit_backbuffer_rt(void) {
  struct d9mt_wire_rec_set_render_targets_by_id rt;
  memset(&rt, 0, sizeof(rt));
  rt.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
  rt.hdr.size = sizeof(rt);
  rt.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
  rt.color[0].format = 80; /* BGRA8Unorm */
  rt.color[0].width = 640;
  rt.color[0].height = 480;
  rt.color[0].sample_count = 1;
  emit(&rt, sizeof(rt));
}

static void
emit_clear(float rr, float gg, float bb) {
  struct d9mt_wire_rec_clear_render_target_by_id clr;
  memset(&clr, 0, sizeof(clr));
  clr.hdr.opcode = d9mt_wire_op_clear_render_target_by_id;
  clr.hdr.size = sizeof(clr);
  clr.flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
  clr.clear_aspects = 0x1;
  clr.value.color[0] = rr;
  clr.value.color[1] = gg;
  clr.value.color[2] = bb;
  clr.value.color[3] = 1.f;
  emit(&clr, sizeof(clr));
}

static void
emit_draws(uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    struct {
      struct d9mt_wire_rec_draw rec;
      struct d9mt_wire_draw_args args[1];
    } dr = {{{d9mt_wire_op_draw, sizeof(dr), 0}, 1, 0}, {{3, 1, 0, 0}}};
    emit(&dr, sizeof(dr));
  }
}

static void
emit_present(uint64_t serial) {
  struct d9mt_wire_rec_present_frame pf = {
      {d9mt_wire_op_present_frame, sizeof(pf), 0}, serial, 0};
  emit(&pf, sizeof(pf));
}

static void
emit_session(uint32_t kind, uint32_t sess) {
  struct d9mt_wire_rec_session sr = {
      {d9mt_wire_op_session, sizeof(sr), 0}, kind, sess};
  emit(&sr, sizeof(sr));
}

static void
emit_upload_tex(uint64_t wire_id, uint64_t handle, uint32_t w, uint32_t h,
                const void *bytes, uint64_t size, uint32_t row_pitch) {
  uint64_t gen = 0;
  uint64_t hoff = heap_put(bytes, size, &gen);
  struct d9mt_wire_rec_upload_texture ut;
  memset(&ut, 0, sizeof(ut));
  ut.hdr.opcode = d9mt_wire_op_upload_texture;
  ut.hdr.size = sizeof(ut);
  ut.wire_id = wire_id;
  ut.client_handle = handle;
  ut.width = w;
  ut.height = h;
  ut.depth = 1;
  ut.row_pitch = row_pitch;
  ut.format = 80; /* BGRA8Unorm */
  ut.heap_offset = hoff;
  ut.heap_generation = gen;
  ut.data_size = size;
  emit(&ut, sizeof(ut));
}

/* Poll a status u64 until it reaches `want` (acquire). 10 ms steps. */
static int
wait_u64(d9mt_status_au64 *field, uint64_t want, int timeout_ms) {
  for (int i = 0; i * 10 <= timeout_ms; i++) {
    if (atomic_load_explicit(field, memory_order_acquire) >= want)
      return 0;
    usleep(10000);
  }
  return 1;
}

/* Draw-wave convergence (async creates + async PSOs): a draw wave both
 * PROMOTES ready libraries and KICKS pending PSO compiles — nothing lands a
 * pipeline until a draw attempts it (real games draw every frame, so this
 * self-heals; the test mimics that). Emits waves of one-draw pairs until a
 * FULL wave executes. Returns waves used, or -1 on timeout. */
static int
draw_wave_converge(struct d9mt_status *status, uint64_t *frame,
                   uint64_t base_handle, uint32_t pairs, int max_waves) {
  for (int w = 1; w <= max_waves; w++) {
    uint64_t before =
        atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    for (uint32_t i = 0; i < pairs; i++) {
      emit_bind_shader(0, base_handle + 2 * i);
      emit_bind_shader(1, base_handle + 2 * i + 1);
      emit_draws(1);
    }
    emit_present(++*frame);
    if (wait_u64(&status->frames, *frame, 30000))
      return -1;
    if (atomic_load_explicit(&status->draws_executed, memory_order_relaxed) -
            before == pairs)
      return w;
    usleep(50000); /* let the create worker / compiler land more */
  }
  return -1;
}

/* v9: converge until the screen shows the checker pattern sampled from
 * `wire` (binds + draws + presents until the async copy/blit lands).
 * Requires vsTex/fsTex bound. Returns 0 on success. */
static int
checker_converge(struct d9mt_status *status, uint32_t ids[3], uint64_t *frame,
                 uint64_t wire, const char *what) {
  for (int w = 1; w <= 30; w++) {
    struct d9mt_wire_rec_bind_texture_by_id b;
    memset(&b, 0, sizeof(b));
    b.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
    b.hdr.size = sizeof(b);
    b.stages = 0x2;
    b.slot = 0;
    b.image_wire_id = wire;
    b.format = 80;
    emit(&b, sizeof(b));
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 1.f);
    emit_draws(1);
    emit_present(++*frame);
    if (wait_u64(&status->frames, *frame, 15000))
      return -1;
    uint32_t slot = atomic_load_explicit(&status->present_slot, memory_order_acquire);
    IOSurfaceRef surf = IOSurfaceLookup(ids[slot]);
    if (!surf)
      return -1;
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(surf);
    size_t bpr = IOSurfaceGetBytesPerRow(surf);
    uint8_t a[4], bb[4], c[4];
    memcpy(a, base + 30 * bpr + 40 * 4, 4);
    memcpy(bb, base + 30 * bpr + 120 * 4, 4);
    memcpy(c, base + 30 * bpr + 200 * 4, 4);
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    CFRelease(surf);
    uint8_t redpx[4] = {0, 0, 255, 255}, greenpx[4] = {0, 255, 0, 255};
    int a_ok = !memcmp(a, redpx, 4) || !memcmp(a, greenpx, 4);
    int b_ok = !memcmp(bb, redpx, 4) || !memcmp(bb, greenpx, 4);
    int alternate = memcmp(a, bb, 4) != 0 && memcmp(a, c, 4) == 0;
    if (a_ok && b_ok && alternate) {
      printf("[test_feed] %s: checker verified (wave %d)\n", what, w);
      return 0;
    }
    usleep(30000);
  }
  return -1;
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

  void *ring_mem, *status_mem, *heap_mem;
  if (create_file(dir, "ring", sizeof(struct d9mt_wire_ring) + RING_CAP,
                  &ring_mem) ||
      create_file(dir, "status", sizeof(struct d9mt_status), &status_mem) ||
      create_file(dir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + HEAP_CAP,
                  &heap_mem))
    return 1;
  g_ring = ring_mem;
  g_data = (uint8_t *)ring_mem + sizeof(struct d9mt_wire_ring);
  struct d9mt_status *status = status_mem;
  g_heap = heap_mem;
  g_heap_data = (uint8_t *)heap_mem + D9MT_WIRE_HEAP_DATA_OFFSET;

  g_ring->capacity = RING_CAP;
  g_ring->data_offset = sizeof(struct d9mt_wire_ring);
  g_ring->magic = D9MT_WIRE_RING_MAGIC; /* last: header now valid */

  g_heap->capacity = HEAP_CAP;
  g_heap->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
  g_heap->magic = D9MT_WIRE_HEAP_MAGIC;

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
  if (wait_u64(&status->progress, produced, 5000))
    FAIL("timeout: progress=%" PRIu64 " want %" PRIu64,
         (uint64_t)atomic_load_explicit(&status->progress, memory_order_acquire),
         produced);
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

  if (atomic_load_explicit(&status->version, memory_order_acquire) != D9MT_STATUS_VERSION)
    FAIL("status version=%u want %u",
         atomic_load_explicit(&status->version, memory_order_acquire),
         D9MT_STATUS_VERSION);

  /* --- S3 v5: PROCESS-LIFETIME server, sessions instead of respawns. ---
   * SESSION 1 (the throwaway device): create + fill a texture, then die —
   * QUIESCE resets the server's tables (epoch bump ack), the SAME pid keeps
   * running, and SESSION 2 connects with a begin record. Later, a stale-id
   * probe binds wire 777 (session-1 identity) and must resolve the DUMMY —
   * never the old red texture (table reset, no aliasing). */
  {
    struct d9mt_wire_rec_create_texture s1t;
    memset(&s1t, 0, sizeof(s1t));
    s1t.hdr.opcode = d9mt_wire_op_create_texture;
    s1t.hdr.size = sizeof(s1t);
    s1t.wire_id = 777;
    s1t.client_handle = 0xF777;
    s1t.type = 1;
    s1t.format = 80;
    s1t.width = 64;
    s1t.height = 64;
    s1t.depth = 1;
    s1t.mip_levels = 1;
    s1t.array_layers = 1;
    s1t.sample_count = 1;
    emit(&s1t, sizeof(s1t));
    static uint8_t redtex[64 * 64 * 4];
    for (size_t k = 0; k < sizeof(redtex); k += 4) {
      redtex[k] = 0;       /* B */
      redtex[k + 1] = 0;   /* G */
      redtex[k + 2] = 255; /* R */
      redtex[k + 3] = 255; /* A */
    }
    emit_upload_tex(777, 0xF777, 64, 64, redtex, sizeof(redtex), 64 * 4);

    emit_session(0, 1); /* QUIESCE: session-1 context died */
    if (wait_u64(&status->epoch, 2, 5000))
      FAIL("quiesce #1 not acked (epoch=%" PRIu64 ")",
           (uint64_t)atomic_load_explicit(&status->epoch, memory_order_relaxed));
    int st1;
    if (waitpid(pid, &st1, WNOHANG) == pid)
      FAIL("server EXITED on quiesce (status 0x%x) — must be process-lifetime", st1);
    emit_session(1, 2); /* BEGIN: session-2 context connected */
    if (wait_u64(&status->epoch, 3, 5000))
      FAIL("session begin #2 not acked (epoch=%" PRIu64 ")",
           (uint64_t)atomic_load_explicit(&status->epoch, memory_order_relaxed));
    printf("[test_feed] sessions: quiesce+begin acked (epoch=3), same server pid %d\n",
           (int)pid);
  }

  /* --- S3 v7: poisoned + large-but-legal creates. A poisoned create must
   * be REJECTED + COUNTED with the consumer alive (the GTA hang was inside
   * the create_texture handler, execOp=27); big legal creates execute on
   * the create worker. --- */
  {
    struct d9mt_wire_rec_create_texture pt;
    memset(&pt, 0, sizeof(pt));
    pt.hdr.opcode = d9mt_wire_op_create_texture;
    pt.hdr.size = sizeof(pt);
    pt.wire_id = 999;
    pt.client_handle = 0xBAD1;
    pt.type = 1;
    pt.format = 80;
    pt.width = 1u << 30; /* absurd extent — the clamp must reject */
    pt.height = 1u << 30;
    pt.depth = 1;
    pt.mip_levels = 99;
    pt.array_layers = 1;
    pt.sample_count = 1;
    emit(&pt, sizeof(pt));

    struct d9mt_wire_rec_create_texture bigrt = pt; /* 2992x1934 RT — legal */
    bigrt.wire_id = 780;
    bigrt.client_handle = 0xB780;
    bigrt.width = 2992;
    bigrt.height = 1934;
    bigrt.mip_levels = 1;
    bigrt.usage = 0x20; /* RenderTarget */
    emit(&bigrt, sizeof(bigrt));

    struct d9mt_wire_rec_create_texture mipped = pt; /* 1024^2, full chain */
    mipped.wire_id = 781;
    mipped.client_handle = 0xB781;
    mipped.width = 1024;
    mipped.height = 1024;
    mipped.mip_levels = 11;
    mipped.usage = 0;
    emit(&mipped, sizeof(mipped));

    static uint8_t mip4[64 * 64 * 4];
    memset(mip4, 0x5A, sizeof(mip4));
    struct d9mt_wire_rec_upload_texture u4;
    memset(&u4, 0, sizeof(u4));
    u4.hdr.opcode = d9mt_wire_op_upload_texture;
    u4.hdr.size = sizeof(u4);
    u4.wire_id = 781;
    u4.client_handle = 0xB781;
    u4.mip = 4; /* 1024 >> 4 == 64 */
    u4.width = 64;
    u4.height = 64;
    u4.depth = 1;
    u4.row_pitch = 64 * 4;
    u4.format = 80;
    uint64_t g4 = 0;
    u4.heap_offset = heap_put(mip4, sizeof(mip4), &g4);
    u4.heap_generation = g4;
    u4.data_size = sizeof(mip4);
    emit(&u4, sizeof(u4));

    /* v8 pseudo-resource shapes (should never be EMITTED by the v8 client —
     * wire citizenship — but the server clamps stay as backstop): the run-8
     * killer "1x0x0 layers=0" and a garbage format. */
    struct d9mt_wire_rec_create_texture zt = pt;
    zt.wire_id = 998;
    zt.client_handle = 0xBAD2;
    zt.width = 1;
    zt.height = 0; /* zero dimension */
    zt.depth = 0;
    zt.mip_levels = 1;
    zt.array_layers = 0;
    emit(&zt, sizeof(zt));
    struct d9mt_wire_rec_create_texture ft2 = pt;
    ft2.wire_id = 997;
    ft2.client_handle = 0xBAD3;
    ft2.width = 4;
    ft2.height = 4;
    ft2.depth = 1;
    ft2.mip_levels = 1;
    ft2.array_layers = 1;
    ft2.format = 0x9999; /* outside the known enum */
    emit(&ft2, sizeof(ft2));

    if (wait_u64(&status->creates_rejected, 3, 5000))
      FAIL("poisoned creates not rejected (creates_rejected=%" PRIu64 " want 3)",
           (uint64_t)atomic_load_explicit(&status->creates_rejected, memory_order_relaxed));
    if (wait_u64(&status->progress, g_head, 5000))
      FAIL("consumer stalled after poisoned create (the execOp=27 hang shape)");
    if (atomic_load_explicit(&status->creates_rejected, memory_order_relaxed) != 3)
      FAIL("creates_rejected=%" PRIu64 " want exactly 3 (legal creates rejected?)",
           (uint64_t)atomic_load_explicit(&status->creates_rejected, memory_order_relaxed));
    printf("[test_feed] v8: poisoned/zero-dim/garbage-format creates rejected+"
           "counted, consumer alive; big legal creates on the worker\n");
  }

  const char *libpath = argc >= 3 ? argv[2] : NULL;
  uint8_t *lib = NULL;
  size_t lib_size = 0;
  if (libpath) {
    FILE *f = fopen(libpath, "rb");
    if (!f)
      FAIL("cannot open metallib %s", libpath);
    fseek(f, 0, SEEK_END);
    lib_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    lib = malloc(lib_size);
    if (fread(lib, 1, lib_size, f) != lib_size)
      FAIL("short read on %s", libpath);
    fclose(f);
  } else {
    fprintf(stderr, "[test_feed] WARN: no metallib argument — draw/texture/"
                    "storm gates SKIPPED (S1 checks only)\n");
  }

  /* no vertex fetch anywhere below: empty vertex layout, list topology */
  struct d9mt_wire_rec_set_vertex_layout vl = {
      {d9mt_wire_op_set_vertex_layout, sizeof(vl), 0}, 0, 0};
  emit(&vl, sizeof(vl));
  struct {
    struct d9mt_wire_rec_set_viewports rec;
    struct d9mt_wire_viewport vp;
  } vps = {{{d9mt_wire_op_set_viewports, sizeof(vps), 0}, 1, 0},
           {0.f, 0.f, 640.f, 480.f, 0.f, 1.f, 0, 0, 0, 0}};
  emit(&vps, sizeof(vps));
  struct d9mt_wire_rec_set_primitive_topology topo = {
      {d9mt_wire_op_set_primitive_topology, sizeof(topo), 0}, 3, 0};
  emit(&topo, sizeof(topo));

  uint64_t frame = 0;

  if (lib) {
    /* --- F1/F2: red draws under ASYNC PSO builds ------------------------- */
    emit_shader(100, 0xA001, 0, "vsMain", lib, lib_size);
    emit_shader(101, 0xA002, 1, "fsMain", lib, lib_size);
    emit_bind_shader(0, 0xA001);
    emit_bind_shader(1, 0xA002);
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 1.f);
    emit_draws(50);
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (exec_errors=%" PRIu64 ")",
           frame,
           (uint64_t)atomic_load_explicit(&status->exec_errors, memory_order_relaxed));
    uint64_t skip_pso =
        atomic_load_explicit(&status->draws_skipped_pending_pso, memory_order_relaxed);
    if (skip_pso < 1)
      FAIL("draws_skipped_pending_pso=%" PRIu64 " want >=1 (async PSO skip "
           "semantics)", skip_pso);
    printf("[test_feed] F1: presented with %" PRIu64 " pending-PSO skips "
           "(consumer never blocked)\n", skip_pso);

    /* converge: draw waves until the async library build + PSO compile land
     * (a wave is what kicks each stage — see draw_wave_converge) */
    int redWaves = -1;
    for (int w = 1; w <= 30; w++) {
      uint64_t before =
          atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
      emit_backbuffer_rt();
      emit_clear(0.f, 0.f, 1.f);
      emit_draws(50);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("timeout waiting for frames>=%" PRIu64, frame);
      if (atomic_load_explicit(&status->draws_executed, memory_order_relaxed) -
              before == 50) {
        redWaves = w;
        break;
      }
      usleep(50000);
    }
    if (redWaves < 0)
      FAIL("red draws never landed (library/PSO async pipeline stuck)");
    if (atomic_load_explicit(&status->psos_built, memory_order_relaxed) < 1)
      FAIL("psos_built=0 after executed draws");
    if (atomic_load_explicit(&status->frame_serial, memory_order_acquire) != frame)
      FAIL("frame_serial=%" PRIu64 " want %" PRIu64,
           (uint64_t)atomic_load_explicit(&status->frame_serial, memory_order_relaxed),
           frame);
    uint64_t execd = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);

    /* pixel proof, read ONCE with no poll: publication is completion-gated */
    uint32_t slot = atomic_load_explicit(&status->present_slot, memory_order_acquire);
    if (slot > 2)
      FAIL("present_slot=%u", slot);
    IOSurfaceRef surf = IOSurfaceLookup(ids[slot]);
    if (!surf)
      FAIL("IOSurfaceLookup(%u) failed", ids[slot]);
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *px = IOSurfaceGetBaseAddress(surf); /* BGRA */
    int red = px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255;
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    CFRelease(surf);
    if (!red)
      FAIL("F2 pixel not red on FIRST read (completion gate broken?)");
    printf("[test_feed] F2: slot %u red on first read — frame publication is "
           "GPU-gated, draws=%" PRIu64 "\n", slot, execd);

    /* --- F3/F4: texture content via the shared heap ---------------------- */
    struct d9mt_wire_rec_create_texture ct;
    memset(&ct, 0, sizeof(ct));
    ct.hdr.opcode = d9mt_wire_op_create_texture;
    ct.hdr.size = sizeof(ct);
    ct.wire_id = 200;
    ct.client_handle = 0xB001;
    ct.type = 1; /* 2D */
    ct.format = 80; /* BGRA8Unorm */
    ct.width = 64;
    ct.height = 64;
    ct.depth = 1;
    ct.mip_levels = 1;
    ct.array_layers = 1;
    ct.sample_count = 1;
    emit(&ct, sizeof(ct));

    /* 64x64 BGRA checkerboard, 8 px cells: red / green */
    static uint8_t checker[64 * 64 * 4];
    for (int y = 0; y < 64; y++) {
      for (int x = 0; x < 64; x++) {
        uint8_t *p = checker + (y * 64 + x) * 4;
        int redcell = ((x >> 3) + (y >> 3)) & 1;
        p[0] = 0;                       /* B */
        p[1] = redcell ? 0 : 255;       /* G */
        p[2] = redcell ? 255 : 0;       /* R */
        p[3] = 255;                     /* A */
      }
    }
    uint64_t gen = 0;
    uint64_t hoff = heap_put(checker, sizeof(checker), &gen);
    struct d9mt_wire_rec_upload_texture ut;
    memset(&ut, 0, sizeof(ut));
    ut.hdr.opcode = d9mt_wire_op_upload_texture;
    ut.hdr.size = sizeof(ut);
    ut.wire_id = 200;
    ut.client_handle = 0xB001;
    ut.width = 64;
    ut.height = 64;
    ut.depth = 1;
    ut.row_pitch = 64 * 4;
    ut.format = 80;
    ut.heap_offset = hoff;
    ut.heap_generation = gen;
    ut.data_size = sizeof(checker);
    emit(&ut, sizeof(ut));

    /* bind: op 3 (counted, view handle has no identity) + op 35 (executed) */
    struct d9mt_wire_rec_bind_texture bt = {
        {d9mt_wire_op_bind_texture, sizeof(bt), 0}, 0x2, 0, 0x2000, 0xABCD, 80, 1};
    emit(&bt, sizeof(bt));
    struct d9mt_wire_rec_bind_texture_by_id bti;
    memset(&bti, 0, sizeof(bti));
    bti.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
    bti.hdr.size = sizeof(bti);
    bti.stages = 0x2; /* fragment */
    bti.slot = 0;
    bti.image_wire_id = 200;
    bti.format = 80;
    emit(&bti, sizeof(bti));

    emit_shader(102, 0xA003, 0, "vsTex", lib, lib_size);
    emit_shader(103, 0xA004, 1, "fsTex", lib, lib_size);
    /* converge the textured quad (library build wave + PSO wave + draw) */
    if (draw_wave_converge(status, &frame, 0xA003, 1, 30) < 0)
      FAIL("textured draw never landed");
    /* checker (200) + the v7 mip-4 upload (781) both drained by now */
    if (wait_u64(&status->uploads_executed, 2, 15000))
      FAIL("uploads_executed=%" PRIu64 " want >=2 (stash drain broken)",
           (uint64_t)atomic_load_explicit(&status->uploads_executed, memory_order_relaxed));

    /* v7: creation is async (worker) — the converge wave may have drawn
     * before the content stash drained, with the bind resolved to the
     * dummy. Rebind (as real games do per draw) + one more frame, THEN
     * read pixels. */
    emit(&bt, sizeof(bt));
    struct d9mt_wire_rec_bind_texture_by_id bti2 = bti;
    emit(&bti2, sizeof(bti2));
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    emit_draws(1);
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (F4 rebind)", frame);

    slot = atomic_load_explicit(&status->present_slot, memory_order_acquire);
    surf = IOSurfaceLookup(ids[slot]);
    if (!surf)
      FAIL("IOSurfaceLookup(%u) failed (F4)", ids[slot]);
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(surf);
    size_t bpr = IOSurfaceGetBytesPerRow(surf);
    /* screen->texel: u=(x+0.5)/640 * 64 => x=40 -> texel 4 (cell 0),
     * x=120 -> texel 12 (cell 1), x=200 -> texel 20 (cell 2). Same row.
     * COPY before unlock — the base address dies with the lock. */
    uint8_t a[4], b[4], c[4];
    memcpy(a, base + 30 * bpr + 40 * 4, 4);
    memcpy(b, base + 30 * bpr + 120 * 4, 4);
    memcpy(c, base + 30 * bpr + 200 * 4, 4);
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    CFRelease(surf);
    uint8_t redpx[4] = {0, 0, 255, 255}, greenpx[4] = {0, 255, 0, 255};
    int a_ok = !memcmp(a, redpx, 4) || !memcmp(a, greenpx, 4);
    int b_ok = !memcmp(b, redpx, 4) || !memcmp(b, greenpx, 4);
    int alternate = memcmp(a, b, 4) != 0 && memcmp(a, c, 4) == 0;
    if (!a_ok || !b_ok)
      FAIL("F4 pixels are not checker colors (a=%02x%02x%02x%02x "
           "b=%02x%02x%02x%02x draws=%" PRIu64 " execErr=%" PRIu64
           " uploads=%" PRIu64 ")",
           a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
           (uint64_t)atomic_load_explicit(&status->draws_executed, memory_order_relaxed),
           (uint64_t)atomic_load_explicit(&status->exec_errors, memory_order_relaxed),
           (uint64_t)atomic_load_explicit(&status->uploads_executed, memory_order_relaxed));
    if (!alternate)
      FAIL("F4 pixels do not alternate with cell period (checker content "
           "missing; a=%02x%02x%02x%02x b=%02x%02x%02x%02x)",
           a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    printf("[test_feed] F4: slot %u checkerboard verified (texture content "
           "crossed the heap)\n", slot);

    /* --- v9: GPU copy crosses the wire. B (wire 201) is created (async,
     * worker), then copy A(200)->B lands (stashed while B builds, drained
     * at present), then a draw sampling B must show the checker. --- */
    struct d9mt_wire_rec_create_texture tb = ct;
    tb.wire_id = 201;
    tb.client_handle = 0xB201;
    emit(&tb, sizeof(tb));
    struct d9mt_wire_rec_copy_texture cp;
    memset(&cp, 0, sizeof(cp));
    cp.hdr.opcode = d9mt_wire_op_copy_texture;
    cp.hdr.size = sizeof(cp);
    cp.dst_wire_id = 201;
    cp.dst_handle = 0xB201;
    cp.src_wire_id = 200;
    cp.src_handle = 0xB001;
    cp.width = 64;
    cp.height = 64;
    cp.depth = 1;
    emit(&cp, sizeof(cp));
    if (checker_converge(status, ids, &frame, 201, "v9 copy A->B"))
      FAIL("wire copy A->B never produced the checker (copy did not cross)");

    /* --- v9: scaled meta-blit A -> C (128x128, 2x stretch, nearest). Same
     * on-screen pattern (cells double in texels, halve in screen scale). --- */
    struct d9mt_wire_rec_create_texture tc = ct;
    tc.wire_id = 202;
    tc.client_handle = 0xB202;
    tc.width = 128;
    tc.height = 128;
    emit(&tc, sizeof(tc));
    struct d9mt_wire_rec_blit_texture_view bl;
    memset(&bl, 0, sizeof(bl));
    bl.hdr.opcode = d9mt_wire_op_blit_texture_view;
    bl.hdr.size = sizeof(bl);
    bl.dst_wire_id = 202;
    bl.dst_handle = 0xB202;
    bl.src_wire_id = 200;
    bl.src_handle = 0xB001;
    bl.dst_format = 80;
    bl.src_format = 80;
    bl.dst_x0 = 0;
    bl.dst_y0 = 0;
    bl.dst_x1 = 128;
    bl.dst_y1 = 128;
    bl.uv[0] = 0.f;
    bl.uv[1] = 0.f;
    bl.uv[2] = 1.f;
    bl.uv[3] = 1.f;
    bl.filter = 0; /* nearest */
    emit(&bl, sizeof(bl));
    if (checker_converge(status, ids, &frame, 202, "v9 blit A->C (2x)"))
      FAIL("wire scaled blit A->C never produced the checker");

    /* --- v10: the UpdateTexture SHAPE — a partial sub-rect upload (dirty
     * box with an offset, exactly what D9MT-UPDTEX emits) into the LIVE
     * texture A: solid red 32x32 at texel (16,16). Inside the box two
     * points that would ALTERNATE under the checker must both be RED;
     * outside the box the checker must survive. --- */
    static uint8_t redbox[32 * 32 * 4];
    for (size_t k = 0; k < sizeof(redbox); k += 4) {
      redbox[k] = 0;
      redbox[k + 1] = 0;
      redbox[k + 2] = 255;
      redbox[k + 3] = 255;
    }
    {
      uint64_t bgen = 0;
      uint64_t boff = heap_put(redbox, sizeof(redbox), &bgen);
      struct d9mt_wire_rec_upload_texture ub;
      memset(&ub, 0, sizeof(ub));
      ub.hdr.opcode = d9mt_wire_op_upload_texture;
      ub.hdr.size = sizeof(ub);
      ub.wire_id = 200;
      ub.client_handle = 0xB001;
      ub.x = 16;
      ub.y = 16;
      ub.width = 32;
      ub.height = 32;
      ub.depth = 1;
      ub.row_pitch = 32 * 4;
      ub.format = 80;
      ub.heap_offset = boff;
      ub.heap_generation = bgen;
      ub.data_size = sizeof(redbox);
      emit(&ub, sizeof(ub));
    }
    int boxOk = 0;
    for (int w = 1; w <= 30 && !boxOk; w++) {
      struct d9mt_wire_rec_bind_texture_by_id rb;
      memset(&rb, 0, sizeof(rb));
      rb.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
      rb.hdr.size = sizeof(rb);
      rb.stages = 0x2;
      rb.slot = 0;
      rb.image_wire_id = 200;
      rb.format = 80;
      emit(&rb, sizeof(rb));
      emit_backbuffer_rt();
      emit_clear(0.f, 0.f, 1.f);
      emit_draws(1);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("timeout (v10 updtex-shape wave)");
      uint32_t s2 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
      IOSurfaceRef sf = IOSurfaceLookup(ids[s2]);
      if (!sf)
        FAIL("IOSurfaceLookup failed (v10)");
      IOSurfaceLock(sf, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *bs = IOSurfaceGetBaseAddress(sf);
      size_t br = IOSurfaceGetBytesPerRow(sf);
      uint8_t in1[4], in2[4], o1[4], o2[4];
      memcpy(in1, bs + 240 * br + 240 * 4, 4); /* texel (24,32) — in box */
      memcpy(in2, bs + 240 * br + 320 * 4, 4); /* texel (32,32) — in box */
      memcpy(o1, bs + 30 * br + 40 * 4, 4);    /* texel (4,4)  — outside */
      memcpy(o2, bs + 30 * br + 120 * 4, 4);   /* texel (12,4) — outside */
      IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, NULL);
      CFRelease(sf);
      uint8_t redpx2[4] = {0, 0, 255, 255}, greenpx2[4] = {0, 255, 0, 255};
      int insideRed = !memcmp(in1, redpx2, 4) && !memcmp(in2, redpx2, 4);
      int outsideChecker =
          (!memcmp(o1, redpx2, 4) || !memcmp(o1, greenpx2, 4)) &&
          (!memcmp(o2, redpx2, 4) || !memcmp(o2, greenpx2, 4)) &&
          memcmp(o1, o2, 4) != 0;
      if (insideRed && outsideChecker)
        boxOk = 1;
      else
        usleep(30000);
    }
    if (!boxOk)
      FAIL("v10 updtex-shape sub-rect upload never landed correctly");
    printf("[test_feed] v10 updtex-shape: offset sub-rect upload landed, "
           "checker preserved outside the box\n");

    /* --- v11: alternating backbuffers (the GTA rotation shape). Two
     * backbuffer wire ids alternate as the render target; the published
     * IOSurface must ALWAYS hold the last-drawn frame. Frame 3 drops the
     * FLAG and relies on the server learning the live id from the present
     * record (the id-substitution belt). Single pixel reads: publication
     * is completion-gated. --- */
    {
      struct AltStep {
        uint64_t id;
        uint32_t flags;
        float rr, gg, bb2;
        uint8_t expect[4]; /* BGRA */
      } steps[3] = {
        {900, D9MT_WIRE_RT_FLAG_BACKBUFFER, 1.f, 0.f, 0.f, {0, 0, 255, 255}},
        {901, D9MT_WIRE_RT_FLAG_BACKBUFFER, 0.f, 1.f, 0.f, {0, 255, 0, 255}},
        {901, 0u /* NO flag: id-learn from the last present */, 0.f, 0.f, 1.f,
         {255, 0, 0, 255}},
      };
      for (int i = 0; i < 3; i++) {
        struct d9mt_wire_rec_set_render_targets_by_id rt2;
        memset(&rt2, 0, sizeof(rt2));
        rt2.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt2.hdr.size = sizeof(rt2);
        rt2.color[0].image_wire_id = steps[i].id;
        rt2.color[0].flags = steps[i].flags;
        rt2.color[0].format = 80;
        rt2.color[0].width = 640;
        rt2.color[0].height = 480;
        rt2.color[0].sample_count = 1;
        emit(&rt2, sizeof(rt2));
        emit_clear(steps[i].rr, steps[i].gg, steps[i].bb2);
        struct d9mt_wire_rec_present_frame pf2 = {
            {d9mt_wire_op_present_frame, sizeof(pf2), 0}, ++frame, steps[i].id};
        emit(&pf2, sizeof(pf2));
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v11 alternating-bb: frame %d never presented", i);
        uint32_t s3v = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf2 = IOSurfaceLookup(ids[s3v]);
        if (!sf2)
          FAIL("v11: IOSurfaceLookup failed");
        IOSurfaceLock(sf2, kIOSurfaceLockReadOnly, NULL);
        uint8_t px2[4];
        memcpy(px2, IOSurfaceGetBaseAddress(sf2), 4);
        IOSurfaceUnlock(sf2, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf2);
        if (memcmp(px2, steps[i].expect, 4) != 0)
          FAIL("v11 alternating-bb frame %d (id=%llu flags=0x%x): pixel "
               "%02x%02x%02x%02x want %02x%02x%02x%02x — %s",
               i, (unsigned long long)steps[i].id, steps[i].flags,
               px2[0], px2[1], px2[2], px2[3],
               steps[i].expect[0], steps[i].expect[1], steps[i].expect[2],
               steps[i].expect[3],
               steps[i].flags ? "flagged substitution broken"
                              : "present-id learning broken");
      }
      printf("[test_feed] v11 alternating backbuffers: 3 frames (2 flagged + "
             "1 id-learned) all published correctly\n");
    }

    /* --- v11 MINIMAL CUBE: the reproducer's failure shape. Wire vertex
     * buffer (real fetched bytes) + wire index buffer + a constant matrix
     * through the push block + one indexed draw. A missing input (VB/IB
     * content, stride, constants) degenerates to ZERO pixels over a
     * perfect clear — exactly the cube/GTA dump pair. Assert the draw
     * PRODUCES pixels (center magenta) over the demo's clear color. --- */
    {
      /* VB: a half-size triangle in clip space (identity mvp) — must NOT
       * cover the corner probe */
      float verts[9] = {-0.5f, -0.5f, 0.f, 0.5f, -0.5f, 0.f, -0.5f, 0.5f, 0.f};
      struct d9mt_wire_rec_create_buffer cbv;
      memset(&cbv, 0, sizeof(cbv));
      cbv.hdr.opcode = d9mt_wire_op_create_buffer;
      cbv.hdr.size = sizeof(cbv);
      cbv.wire_id = 950;
      cbv.client_handle = 0xC950;
      cbv.size = sizeof(verts);
      emit(&cbv, sizeof(cbv));
      uint64_t vg = 0;
      uint64_t voff = heap_put(verts, sizeof(verts), &vg);
      struct d9mt_wire_rec_upload_buffer ubv;
      memset(&ubv, 0, sizeof(ubv));
      ubv.hdr.opcode = d9mt_wire_op_upload_buffer;
      ubv.hdr.size = sizeof(ubv);
      ubv.wire_id = 950;
      ubv.client_handle = 0xC950;
      ubv.size = sizeof(verts);
      ubv.heap_offset = voff;
      ubv.heap_generation = vg;
      emit(&ubv, sizeof(ubv));

      /* IB: one triangle, u16 */
      uint16_t idx[4] = {0, 1, 2, 0};
      struct d9mt_wire_rec_create_buffer cbi = cbv;
      cbi.wire_id = 951;
      cbi.client_handle = 0xC951;
      cbi.size = sizeof(idx);
      emit(&cbi, sizeof(cbi));
      uint64_t ig = 0;
      uint64_t ioff = heap_put(idx, sizeof(idx), &ig);
      struct d9mt_wire_rec_upload_buffer ubi = ubv;
      ubi.wire_id = 951;
      ubi.client_handle = 0xC951;
      ubi.size = sizeof(idx);
      ubi.heap_offset = ioff;
      ubi.heap_generation = ig;
      emit(&ubi, sizeof(ubi));

      /* shaders: cubeVs (stage_in + push-block matrix) / cubeFs (magenta) */
      emit_shader(952, 0xA005, 0, "cubeVs", lib, lib_size);
      emit_shader(953, 0xA006, 1, "cubeFs", lib, lib_size);
      emit_bind_shader(0, 0xA005);
      emit_bind_shader(1, 0xA006);

      /* vertex layout: one float3 attr at binding 0, stride 12 */
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vl2 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl2), 0}, 1, 1},
               {0, 0, 900 /* RGB32Float */, 0},
               {0, 12, 0, 0}};
      emit(&vl2, sizeof(vl2));

      struct d9mt_wire_rec_bind_vertex_buffer bvb;
      memset(&bvb, 0, sizeof(bvb));
      bvb.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
      bvb.hdr.size = sizeof(bvb);
      bvb.binding = 0;
      bvb.stride = 12;
      bvb.buf.buffer = 0xC950;
      bvb.buf.length = sizeof(verts);
      emit(&bvb, sizeof(bvb));

      struct d9mt_wire_rec_bind_index_buffer bib;
      memset(&bib, 0, sizeof(bib));
      bib.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bib.hdr.size = sizeof(bib);
      bib.buf.buffer = 0xC951;
      bib.buf.length = sizeof(idx);
      bib.index_type = 0; /* u16 */
      emit(&bib, sizeof(bib));

      /* the WVP matrix through the push block (identity; column-major).
       * stages=1 (vertex only) => the server shifts past the 32-byte
       * shared reservation, where CubePush.mvp reads it. */
      struct {
        struct d9mt_wire_rec_push_data rec;
        float mvp[16];
      } pd2 = {{{d9mt_wire_op_push_data, sizeof(pd2), 0}, 1, 0, 64, 0},
               {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
      emit(&pd2, sizeof(pd2));

      /* converge: clear to the demo's dark blue, one indexed draw, expect
       * MAGENTA at center (draw produced pixels) + dark blue at corner */
      int cubeOk = 0;
      for (int w = 1; w <= 30 && !cubeOk; w++) {
        struct d9mt_wire_rec_set_render_targets_by_id rt3;
        memset(&rt3, 0, sizeof(rt3));
        rt3.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt3.hdr.size = sizeof(rt3);
        rt3.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rt3.color[0].format = 80;
        rt3.color[0].width = 640;
        rt3.color[0].height = 480;
        rt3.color[0].sample_count = 1;
        emit(&rt3, sizeof(rt3));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } di = {{{d9mt_wire_op_draw_indexed, sizeof(di), 0}, 1, 0},
                {{3, 1, 0, 0, 0, 0}}};
        emit(&di, sizeof(di));
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v11 cube: frame never presented");
        uint32_t s4 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf3 = IOSurfaceLookup(ids[s4]);
        if (!sf3)
          FAIL("v11 cube: IOSurfaceLookup failed");
        IOSurfaceLock(sf3, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *b3 = IOSurfaceGetBaseAddress(sf3);
        size_t r3 = IOSurfaceGetBytesPerRow(sf3);
        uint8_t center[4], corner[4];
        memcpy(center, b3 + 280 * r3 + 266 * 4, 4); /* triangle centroid */
        memcpy(corner, b3 + 2 * r3 + 636 * 4, 4); /* top-right: outside tri */
        IOSurfaceUnlock(sf3, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf3);
        uint8_t magenta[4] = {255, 0, 255, 255}, dkblue[4] = {40, 16, 16, 255};
        if (!memcmp(center, magenta, 4) && !memcmp(corner, dkblue, 4)) {
          cubeOk = 1;
          printf("[test_feed] v11 minimal-cube: draw produced pixels over the "
                 "clear (wave %d) — VB/IB/push-matrix all crossed\n", w);
        } else if (w == 30) {
          FAIL("v11 minimal-cube: center=%02x%02x%02x%02x corner=%02x%02x%02x%02x"
               " — draw produced no/wrong pixels (the reproducer shape)",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3]);
        } else {
          usleep(30000);
        }
      }
    }

    /* --- V12 DEPTH-TESTED CUBE: the reproducer's depth chain. The real
     * demo runs with EnableAutoDepthStencil (D3DFMT_D24S8 -> wire format
     * 260 = Depth32Float_Stencil8, the client's D24S8 mapping),
     * D3DRS_ZENABLE=TRUE + ZFUNC default LESSEQUAL, and clears via
     * D3DCLEAR_ZBUFFER (aspects 0x2, depth 1.0) folded into the draw pass.
     * Draw A (triangle, LESSEQUAL vs cleared 1.0, write ON) must PRODUCE
     * pixels — depth garbage (missing clear) rejects it. Draw B (fullscreen
     * RED at z=0, LESS, write OFF) must be REJECTED over the triangle
     * (0<0 false: proves the test engages) and ACCEPTED elsewhere
     * (0<1.0: proves the clear VALUE landed). --- */
    {
      /* the auto depth-stencil: usage 0x40 (DepthStencil), private */
      struct d9mt_wire_rec_create_texture ct;
      memset(&ct, 0, sizeof(ct));
      ct.hdr.opcode = d9mt_wire_op_create_texture;
      ct.hdr.size = sizeof(ct);
      ct.wire_id = 960;
      ct.client_handle = 0xD960;
      ct.type = 1; /* 2D */
      ct.format = 260; /* Depth32Float_Stencil8 (client D24S8 mapping) */
      ct.width = 640;
      ct.height = 480;
      ct.depth = 1;
      ct.mip_levels = 1;
      ct.array_layers = 1;
      ct.sample_count = 1;
      ct.usage = 0x40; /* MtlUsage::DepthStencil */
      emit(&ct, sizeof(ct));

      int depthOk = 0;
      for (int w = 1; w <= 30 && !depthOk; w++) {
        /* RT: backbuffer color + the wire depth attachment */
        struct d9mt_wire_rec_set_render_targets_by_id rt4;
        memset(&rt4, 0, sizeof(rt4));
        rt4.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt4.hdr.size = sizeof(rt4);
        rt4.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rt4.color[0].format = 80;
        rt4.color[0].width = 640;
        rt4.color[0].height = 480;
        rt4.color[0].sample_count = 1;
        rt4.depth_stencil.image_wire_id = 960;
        rt4.depth_stencil.format = 260;
        rt4.depth_stencil.width = 640;
        rt4.depth_stencil.height = 480;
        rt4.depth_stencil.sample_count = 1;
        emit(&rt4, sizeof(rt4));

        /* D3D9 Clear(TARGET|ZBUFFER): two per-view clear_by_id records,
         * exactly the client's emission shape (depth first) */
        struct d9mt_wire_rec_clear_render_target_by_id zclr;
        memset(&zclr, 0, sizeof(zclr));
        zclr.hdr.opcode = d9mt_wire_op_clear_render_target_by_id;
        zclr.hdr.size = sizeof(zclr);
        zclr.image_wire_id = 960;
        zclr.clear_aspects = 0x2; /* depth only (D3DCLEAR_ZBUFFER) */
        zclr.value.depth = 1.f;
        emit(&zclr, sizeof(zclr));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);

        /* depth state A: ZENABLE + ZWRITEENABLE + LESSEQUAL (demo default) */
        struct d9mt_wire_rec_set_depth_stencil_state dss;
        memset(&dss, 0, sizeof(dss));
        dss.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
        dss.hdr.size = sizeof(dss);
        dss.depth_compare_op = 3; /* LessEqual */
        dss.depth_test = 1;
        dss.depth_write = 1;
        dss.front.compare_op = dss.back.compare_op = 7; /* Always */
        dss.front.read_mask = dss.back.read_mask = 0xFF;
        dss.front.write_mask = dss.back.write_mask = 0xFF;
        emit(&dss, sizeof(dss));

        /* draw A: the cube triangle (VB/IB/push data live from the v11
         * scenario; re-bind for wave independence) */
        emit_bind_shader(0, 0xA005);
        emit_bind_shader(1, 0xA006);
        struct {
          struct d9mt_wire_rec_set_vertex_layout rec;
          struct d9mt_wire_vertex_attr attr;
          struct d9mt_wire_vertex_layout lay;
        } vl4 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl4), 0}, 1, 1},
                 {0, 0, 900 /* RGB32Float */, 0},
                 {0, 12, 0, 0}};
        emit(&vl4, sizeof(vl4));
        struct d9mt_wire_rec_bind_vertex_buffer bvb2;
        memset(&bvb2, 0, sizeof(bvb2));
        bvb2.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
        bvb2.hdr.size = sizeof(bvb2);
        bvb2.binding = 0;
        bvb2.stride = 12;
        bvb2.buf.buffer = 0xC950;
        bvb2.buf.length = 36;
        emit(&bvb2, sizeof(bvb2));
        struct d9mt_wire_rec_bind_index_buffer bib2;
        memset(&bib2, 0, sizeof(bib2));
        bib2.hdr.opcode = d9mt_wire_op_bind_index_buffer;
        bib2.hdr.size = sizeof(bib2);
        bib2.buf.buffer = 0xC951;
        bib2.buf.length = 8;
        bib2.index_type = 0;
        emit(&bib2, sizeof(bib2));
        struct {
          struct d9mt_wire_rec_push_data rec;
          float mvp[16];
        } pd3 = {{{d9mt_wire_op_push_data, sizeof(pd3), 0}, 1, 0, 64, 0},
                 {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        emit(&pd3, sizeof(pd3));
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } di2 = {{{d9mt_wire_op_draw_indexed, sizeof(di2), 0}, 1, 0},
                 {{3, 1, 0, 0, 0, 0}}};
        emit(&di2, sizeof(di2));

        /* depth state B: LESS, write OFF — the fullscreen z=0 draw must be
         * rejected over the triangle (0<0) and accepted over the clear
         * (0<1). */
        dss.depth_compare_op = 1; /* Less */
        dss.depth_write = 0;
        emit(&dss, sizeof(dss));
        emit_bind_shader(0, 0xA001); /* vsMain: fullscreen, z=0 */
        emit_bind_shader(1, 0xA002); /* fsMain: solid red */
        struct d9mt_wire_rec_set_vertex_layout vl5 = {
            {d9mt_wire_op_set_vertex_layout, sizeof(vl5), 0}, 0, 0};
        emit(&vl5, sizeof(vl5));
        emit_draws(1);

        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v12 depth-cube: frame never presented");
        uint32_t s5 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf4 = IOSurfaceLookup(ids[s5]);
        if (!sf4)
          FAIL("v12 depth-cube: IOSurfaceLookup failed");
        IOSurfaceLock(sf4, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *b4 = IOSurfaceGetBaseAddress(sf4);
        size_t r4 = IOSurfaceGetBytesPerRow(sf4);
        uint8_t center[4], corner[4];
        memcpy(center, b4 + 280 * r4 + 266 * 4, 4); /* triangle centroid */
        memcpy(corner, b4 + 2 * r4 + 636 * 4, 4);   /* outside triangle */
        IOSurfaceUnlock(sf4, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf4);
        uint8_t magenta[4] = {255, 0, 255, 255}, red[4] = {0, 0, 255, 255};
        if (!memcmp(center, magenta, 4) && !memcmp(corner, red, 4)) {
          depthOk = 1;
          printf("[test_feed] v12 depth-cube: depth clear + LESSEQUAL accept "
                 "+ LESS reject all verified (wave %d)\n", w);
        } else if (w == 30) {
          FAIL("v12 depth-cube: center=%02x%02x%02x%02x (want magenta: depth-"
               "tested draw produced pixels + later draw rejected) corner="
               "%02x%02x%02x%02x (want red: fullscreen accepted vs cleared "
               "1.0) — the depth chain diverges",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3]);
        } else {
          usleep(30000);
        }
      }

      /* restore demo-neutral depth state (test off) for later scenarios */
      struct d9mt_wire_rec_set_depth_stencil_state dssOff;
      memset(&dssOff, 0, sizeof(dssOff));
      dssOff.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
      dssOff.hdr.size = sizeof(dssOff);
      dssOff.depth_compare_op = 7; /* Always */
      dssOff.front.compare_op = dssOff.back.compare_op = 7;
      dssOff.front.read_mask = dssOff.back.read_mask = 0xFF;
      dssOff.front.write_mask = dssOff.back.write_mask = 0xFF;
      emit(&dssOff, sizeof(dssOff));
    }

    /* --- V13 CBV CUBE: the REAL constant mechanism, as decoded from the
     * client emission (d3d9_constant_buffer.cpp + mtl_wire_context.cpp).
     * SetVertexShaderConstantF does NOT ride the push block — it lands in a
     * host-visible constant-buffer RING: one create_buffer + full-content
     * snapshot at first bind (bind_uniform_buffer at CbvIndex::
     * VSDynamicConstants = slot 4), then per-update upload_buffer content +
     * bind_uniform_buffer_range advancing the ring offset. The shader reads
     * the CBV at the AIR slot in create_shader.cbv_slot_map[4]. The v13
     * client fix (snapshotDirtyUniforms) emits exactly this shape; a server
     * that drops any link (upload into a bound CBV, range offset, slot map)
     * degenerates the draw to ZERO pixels — the reproducer shape. --- */
    {
      /* the constant ring buffer: create + zero snapshot (ensureStorage
       * Created's shape), matrix uploaded at a NON-ZERO ring offset below */
      uint8_t zeros[512] = {0};
      struct d9mt_wire_rec_create_buffer ccb;
      memset(&ccb, 0, sizeof(ccb));
      ccb.hdr.opcode = d9mt_wire_op_create_buffer;
      ccb.hdr.size = sizeof(ccb);
      ccb.wire_id = 970;
      ccb.client_handle = 0xC970;
      ccb.size = sizeof(zeros);
      emit(&ccb, sizeof(ccb));
      uint64_t zg = 0;
      uint64_t zoff = heap_put(zeros, sizeof(zeros), &zg);
      struct d9mt_wire_rec_upload_buffer ubz;
      memset(&ubz, 0, sizeof(ubz));
      ubz.hdr.opcode = d9mt_wire_op_upload_buffer;
      ubz.hdr.size = sizeof(ubz);
      ubz.wire_id = 970;
      ubz.client_handle = 0xC970;
      ubz.size = sizeof(zeros);
      ubz.heap_offset = zoff;
      ubz.heap_generation = zg;
      emit(&ubz, sizeof(ubz));

      /* cubeCbvVs: stage_in + float4x4 at AIR buffer 1, mapped from D3D9
       * CBV slot 4 (VSDynamicConstants) via cbv_slot_map */
      int8_t map[8] = {-1, -1, -1, -1, 1, -1, -1, -1};
      emit_shader_map(971, 0xA00B, 0, "cubeCbvVs", lib, lib_size, map);

      int cbvOk = 0;
      for (int w = 1; w <= 30 && !cbvOk; w++) {
        emit_bind_shader(0, 0xA00B);
        emit_bind_shader(1, 0xA006); /* cubeFs: magenta */
        struct {
          struct d9mt_wire_rec_set_vertex_layout rec;
          struct d9mt_wire_vertex_attr attr;
          struct d9mt_wire_vertex_layout lay;
        } vl6 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl6), 0}, 1, 1},
                 {0, 0, 900 /* RGB32Float */, 0},
                 {0, 12, 0, 0}};
        emit(&vl6, sizeof(vl6));
        struct d9mt_wire_rec_bind_vertex_buffer bvb3;
        memset(&bvb3, 0, sizeof(bvb3));
        bvb3.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
        bvb3.hdr.size = sizeof(bvb3);
        bvb3.binding = 0;
        bvb3.stride = 12;
        bvb3.buf.buffer = 0xC950; /* v11 triangle VB (same session) */
        bvb3.buf.length = 36;
        emit(&bvb3, sizeof(bvb3));
        struct d9mt_wire_rec_bind_index_buffer bib3;
        memset(&bib3, 0, sizeof(bib3));
        bib3.hdr.opcode = d9mt_wire_op_bind_index_buffer;
        bib3.hdr.size = sizeof(bib3);
        bib3.buf.buffer = 0xC951; /* v11 IB */
        bib3.buf.length = 8;
        bib3.index_type = 0;
        emit(&bib3, sizeof(bib3));

        /* frontend shape: whole-buffer bind at slot 4 ... */
        struct d9mt_wire_rec_bind_uniform_buffer bub;
        memset(&bub, 0, sizeof(bub));
        bub.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
        bub.hdr.size = sizeof(bub);
        bub.stages = 0x1; /* vertex */
        bub.slot = 4;     /* VSDynamicConstants */
        bub.buf.buffer = 0xC970;
        bub.buf.length = sizeof(zeros);
        emit(&bub, sizeof(bub));

        /* ... then the per-update pair: content upload + range advance to a
         * NON-ZERO ring offset (proves offset plumbing end to end) */
        float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        uint64_t mg = 0;
        uint64_t moff = heap_put(mvp, sizeof(mvp), &mg);
        struct d9mt_wire_rec_upload_buffer ubm = ubz;
        ubm.dst_offset = 256;
        ubm.size = sizeof(mvp);
        ubm.heap_offset = moff;
        ubm.heap_generation = mg;
        emit(&ubm, sizeof(ubm));
        struct d9mt_wire_rec_bind_uniform_buffer_range bur;
        memset(&bur, 0, sizeof(bur));
        bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
        bur.hdr.size = sizeof(bur);
        bur.stages = 0x1;
        bur.slot = 4;
        bur.offset = 256;
        bur.length = sizeof(mvp);
        emit(&bur, sizeof(bur));

        struct d9mt_wire_rec_set_render_targets_by_id rt5;
        memset(&rt5, 0, sizeof(rt5));
        rt5.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt5.hdr.size = sizeof(rt5);
        rt5.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rt5.color[0].format = 80;
        rt5.color[0].width = 640;
        rt5.color[0].height = 480;
        rt5.color[0].sample_count = 1;
        emit(&rt5, sizeof(rt5));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } di3 = {{{d9mt_wire_op_draw_indexed, sizeof(di3), 0}, 1, 0},
                 {{3, 1, 0, 0, 0, 0}}};
        emit(&di3, sizeof(di3));
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v13 cbv-cube: frame never presented");
        uint32_t s6 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf5 = IOSurfaceLookup(ids[s6]);
        if (!sf5)
          FAIL("v13 cbv-cube: IOSurfaceLookup failed");
        IOSurfaceLock(sf5, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *b5 = IOSurfaceGetBaseAddress(sf5);
        size_t r5 = IOSurfaceGetBytesPerRow(sf5);
        uint8_t center[4], corner[4];
        memcpy(center, b5 + 280 * r5 + 266 * 4, 4); /* triangle centroid */
        memcpy(corner, b5 + 2 * r5 + 636 * 4, 4);   /* outside triangle */
        IOSurfaceUnlock(sf5, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf5);
        uint8_t magenta[4] = {255, 0, 255, 255}, dkblue[4] = {40, 16, 16, 255};
        if (!memcmp(center, magenta, 4) && !memcmp(corner, dkblue, 4)) {
          cbvOk = 1;
          printf("[test_feed] v13 cbv-cube: constants through the REAL "
                 "mechanism (CBV slot 4 + upload_buffer + range offset) "
                 "produced pixels (wave %d)\n", w);
        } else if (w == 30) {
          FAIL("v13 cbv-cube: center=%02x%02x%02x%02x corner=%02x%02x%02x%02x"
               " — CBV constants did not reach the VS (upload/bind/range/"
               "slot-map chain broken)",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3]);
        } else {
          usleep(30000);
        }
      }

      /* --- v13.1 SECOND-FRAME constants: the per-frame update shape. A new
       * matrix (scale 0.5 + translate +0.5,+0.5) ships at a FRESH ring
       * offset (the frontend's per-frame Alloc advance) — the triangle must
       * MOVE: the old centroid returns to the clear color and the new
       * centroid turns magenta. A server that keeps the FIRST constants
       * (one-shot upload, stale range, blit misorder) leaves the triangle
       * in place — the frozen-constants failure shape. --- */
      int cbv2Ok = 0;
      for (int w = 1; w <= 30 && !cbv2Ok; w++) {
        /* x' = 0.5x + 0.5w, y' = 0.5y + 0.5w (column-major) — verts
         * (-0.5,-0.5)(0.5,-0.5)(-0.5,0.5) -> NDC (0.25,0.25)(0.75,0.25)
         * (0.25,0.75): pixels (400,180)(560,180)(400,60), centroid
         * (453,140); the old centroid (266,280) falls outside. */
        float mvp2[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0,
                          0, 0, 1, 0, 0.5f, 0.5f, 0, 1};
        uint64_t m2g = 0;
        uint64_t m2off = heap_put(mvp2, sizeof(mvp2), &m2g);
        struct d9mt_wire_rec_upload_buffer ubm2 = ubz;
        ubm2.dst_offset = 320; /* the ring advanced past the frame-1 alloc */
        ubm2.size = sizeof(mvp2);
        ubm2.heap_offset = m2off;
        ubm2.heap_generation = m2g;
        emit(&ubm2, sizeof(ubm2));
        struct d9mt_wire_rec_bind_uniform_buffer_range bur2;
        memset(&bur2, 0, sizeof(bur2));
        bur2.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
        bur2.hdr.size = sizeof(bur2);
        bur2.stages = 0x1;
        bur2.slot = 4;
        bur2.offset = 320;
        bur2.length = sizeof(mvp2);
        emit(&bur2, sizeof(bur2));

        struct d9mt_wire_rec_set_render_targets_by_id rt6;
        memset(&rt6, 0, sizeof(rt6));
        rt6.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt6.hdr.size = sizeof(rt6);
        rt6.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rt6.color[0].format = 80;
        rt6.color[0].width = 640;
        rt6.color[0].height = 480;
        rt6.color[0].sample_count = 1;
        emit(&rt6, sizeof(rt6));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } di4 = {{{d9mt_wire_op_draw_indexed, sizeof(di4), 0}, 1, 0},
                 {{3, 1, 0, 0, 0, 0}}};
        emit(&di4, sizeof(di4));
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v13.1 second-frame constants: frame never presented");
        uint32_t s7 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf6 = IOSurfaceLookup(ids[s7]);
        if (!sf6)
          FAIL("v13.1 second-frame constants: IOSurfaceLookup failed");
        IOSurfaceLock(sf6, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *b6 = IOSurfaceGetBaseAddress(sf6);
        size_t r6 = IOSurfaceGetBytesPerRow(sf6);
        uint8_t moved[4], old[4];
        memcpy(moved, b6 + 140 * r6 + 453 * 4, 4); /* new centroid */
        memcpy(old, b6 + 280 * r6 + 266 * 4, 4);   /* frame-1 centroid */
        IOSurfaceUnlock(sf6, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf6);
        uint8_t magenta2[4] = {255, 0, 255, 255}, dkblue2[4] = {40, 16, 16, 255};
        if (!memcmp(moved, magenta2, 4) && !memcmp(old, dkblue2, 4)) {
          cbv2Ok = 1;
          printf("[test_feed] v13.1 second-frame constants: new matrix at a "
                 "fresh ring offset MOVED the triangle (wave %d) — per-frame "
                 "constant updates apply\n", w);
        } else if (w == 30) {
          FAIL("v13.1 second-frame constants: moved=%02x%02x%02x%02x (want "
               "magenta) old=%02x%02x%02x%02x (want clear) — the server kept "
               "the FIRST constants (frozen-constants shape)",
               moved[0], moved[1], moved[2], moved[3],
               old[0], old[1], old[2], old[3]);
        } else {
          usleep(30000);
        }
      }
    }

    /* stale-id probe: wire 777 lived (RED-filled) in SESSION 1; the quiesce
     * reset the table, so this bind must resolve the 1x1 BLACK dummy. Red =
     * ALIASING (reset broken); blue (the clear) = draw skipped. Needs the
     * TEXTURED fullscreen pipeline (the cube scenario left its own bound). */
    emit_bind_shader(0, 0xA003);
    emit_bind_shader(1, 0xA004);
    { /* fullscreen shape needs no vertex fetch: empty layout again */
      struct d9mt_wire_rec_set_vertex_layout vl3 = {
          {d9mt_wire_op_set_vertex_layout, sizeof(vl3), 0}, 0, 0};
      emit(&vl3, sizeof(vl3));
    }
    struct d9mt_wire_rec_bind_texture_by_id stale;
    memset(&stale, 0, sizeof(stale));
    stale.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
    stale.hdr.size = sizeof(stale);
    stale.stages = 0x2;
    stale.slot = 0;
    stale.image_wire_id = 777;
    stale.format = 80;
    emit(&stale, sizeof(stale));
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 1.f); /* blue */
    emit_draws(1);
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (stale probe)", frame);
    slot = atomic_load_explicit(&status->present_slot, memory_order_acquire);
    surf = IOSurfaceLookup(ids[slot]);
    if (!surf)
      FAIL("IOSurfaceLookup(%u) failed (stale probe)", ids[slot]);
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    uint8_t sp[4];
    memcpy(sp, (const uint8_t *)IOSurfaceGetBaseAddress(surf) +
                   30 * IOSurfaceGetBytesPerRow(surf) + 40 * 4, 4);
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    CFRelease(surf);
    if (sp[2] == 255 && sp[1] == 0)
      FAIL("stale wire id ALIASED session-1 content (pixel red) — table reset broken");
    if (!(sp[0] == 0 && sp[1] == 0 && sp[2] == 0 && sp[3] == 255))
      FAIL("stale probe pixel %02x%02x%02x%02x want black dummy (draw skipped?)",
           sp[0], sp[1], sp[2], sp[3]);
    printf("[test_feed] stale-id probe: wire 777 dummy-resolved after reset "
           "(no aliasing)\n");

    /* --- COMPILE STORM: 64 distinct shader pairs, draws outrun compiles -- */
    uint64_t psos0 = atomic_load_explicit(&status->psos_built, memory_order_relaxed);
    uint64_t skip0 =
        atomic_load_explicit(&status->draws_skipped_pending_pso, memory_order_relaxed);
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    for (uint32_t i = 0; i < 64; i++) {
      /* same metallib bytes, DISTINCT client handles => 64 distinct
       * libraries server-side => 64 distinct PSO keys/compiles */
      emit_shader(1000 + 2 * i, 0xC000 + 2 * i, 0, "vsMain", lib, lib_size);
      emit_shader(1001 + 2 * i, 0xC001 + 2 * i, 1, "fsMain", lib, lib_size);
      emit_bind_shader(0, 0xC000 + 2 * i);
      emit_bind_shader(1, 0xC001 + 2 * i);
      emit_draws(1); /* immediately after the bind: compile still in flight */
    }
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 30000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (storm frame — ring "
           "stalled on compiles?)", frame);
    if (g_producer_waits)
      FAIL("producer waited for ring space %" PRIu64 " times during the storm",
           g_producer_waits);
    uint64_t skip1 =
        atomic_load_explicit(&status->draws_skipped_pending_pso, memory_order_relaxed);
    if (skip1 <= skip0)
      FAIL("no pending-PSO skips during the storm (%" PRIu64 " -> %" PRIu64 ")",
           skip0, skip1);
    printf("[test_feed] storm: frame presented while %" PRIu64 " draws skipped "
           "on in-flight compiles, producer never stalled\n", skip1 - skip0);

    /* drain: converge draw waves until all 64 pairs execute in one wave —
     * proves the skips STOP once the async creates + compiles land */
    int stormWaves = draw_wave_converge(status, &frame, 0xC000, 64, 30);
    if (stormWaves < 0)
      FAIL("storm never drained (async create/compile pipeline stuck)");
    if (atomic_load_explicit(&status->psos_built, memory_order_relaxed) < psos0 + 64)
      FAIL("psos_built=%" PRIu64 " want >=%" PRIu64 " after full storm wave",
           (uint64_t)atomic_load_explicit(&status->psos_built, memory_order_relaxed),
           psos0 + 64);
    for (int i = 0; i < 1000; i++) {
      if (atomic_load_explicit(&status->psos_pending, memory_order_relaxed) == 0)
        break;
      if (i == 999)
        FAIL("psos_pending stuck at %" PRIu64,
             (uint64_t)atomic_load_explicit(&status->psos_pending, memory_order_relaxed));
      usleep(10000);
    }
    if (atomic_load_explicit(&status->exec_errors, memory_order_relaxed))
      FAIL("exec_errors=%" PRIu64,
           (uint64_t)atomic_load_explicit(&status->exec_errors, memory_order_relaxed));
    if (atomic_load_explicit(&g_ring->stalls, memory_order_relaxed))
      FAIL("ring stalls=%" PRIu64 " (producer starved)",
           (uint64_t)atomic_load_explicit(&g_ring->stalls, memory_order_relaxed));
    printf("[test_feed] storm: 64 PSOs landed async, drained in %d waves, "
           "stalls=0\n", stormWaves);

    /* --- BOOT FLOOD (S3 v2): 200 shader LIBRARIES + 300 MB of texture
     * uploads arriving together — the real-game (GTA IV boot) profile that
     * starved the S2/S3.0 consumer in newLibraryWithData and overran the
     * heap. Asserts: the wire survives (producer never waits for ring
     * space), heap acks flow (heap_put's bounded ack wait never times out
     * while 300 MB cycles through an 8 MiB heap), frames keep presenting,
     * and the pending-create draw skips eventually DRAIN (a final wave
     * draws 100/100). --- */
    uint64_t psosF = atomic_load_explicit(&status->psos_built, memory_order_relaxed);
    uint64_t skipF =
        atomic_load_explicit(&status->draws_skipped_pending_pso, memory_order_relaxed);
    uint64_t uplF = atomic_load_explicit(&status->uploads_executed, memory_order_relaxed);

    struct d9mt_wire_rec_create_texture ft;
    memset(&ft, 0, sizeof(ft));
    ft.hdr.opcode = d9mt_wire_op_create_texture;
    ft.hdr.size = sizeof(ft);
    ft.wire_id = 300;
    ft.client_handle = 0xD001;
    ft.type = 1;
    ft.format = 80;
    ft.width = 512;
    ft.height = 512;
    ft.depth = 1;
    ft.mip_levels = 1;
    ft.array_layers = 1;
    ft.sample_count = 1;
    emit(&ft, sizeof(ft));

    static uint8_t megabyte[512 * 512 * 4]; /* 1 MiB BGRA source */
    for (size_t k = 0; k < sizeof(megabyte); k++)
      megabyte[k] = (uint8_t)(k * 2654435761u >> 24);

    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    for (uint32_t i = 0; i < 100; i++) {
      /* 2 libraries per pair x 100 = 200 libraries */
      emit_shader(2000 + 2 * i, 0xE000 + 2 * i, 0, "vsMain", lib, lib_size);
      emit_shader(2001 + 2 * i, 0xE001 + 2 * i, 1, "fsMain", lib, lib_size);
      emit_bind_shader(0, 0xE000 + 2 * i);
      emit_bind_shader(1, 0xE001 + 2 * i);
      emit_draws(1); /* library build in flight: pending-create skip */
      for (int j = 0; j < 3; j++) /* 3 x 1 MiB x 100 = 300 MB */
        emit_upload_tex(300, 0xD001, 512, 512, megabyte, sizeof(megabyte), 512 * 4);
      if (i % 10 == 9)
        emit_present(++frame);
    }
    if (wait_u64(&status->frames, frame, 60000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (flood frames — consumer "
           "starved?)", frame);
    if (g_producer_waits)
      FAIL("producer waited for ring space %" PRIu64 " times during the flood",
           g_producer_waits);
    uint64_t skipG =
        atomic_load_explicit(&status->draws_skipped_pending_pso, memory_order_relaxed);
    if (skipG <= skipF)
      FAIL("no pending-create/PSO skips during the flood (%" PRIu64 " -> %" PRIu64 ")",
           skipF, skipG);
    if (wait_u64(&status->uploads_executed, uplF + 300, 60000))
      FAIL("timeout: uploads_executed=%" PRIu64 " want %" PRIu64,
           (uint64_t)atomic_load_explicit(&status->uploads_executed, memory_order_relaxed),
           uplF + 300);
    printf("[test_feed] flood: 200 libraries + 300 MB through an 8 MiB heap — "
           "%" PRIu64 " full-heap waits (acks flowed), producer never ring-"
           "stalled, %" PRIu64 " pending skips\n",
           g_heap_waits, skipG - skipF);

    /* drain: converge until every flood pair draws in one wave —
     * draws_skipped stops growing, libraries + PSOs all landed */
    int floodWaves = draw_wave_converge(status, &frame, 0xE000, 100, 30);
    if (floodWaves < 0)
      FAIL("flood never drained (create worker / compiler stuck)");
    if (atomic_load_explicit(&status->psos_built, memory_order_relaxed) < psosF + 100)
      FAIL("psos_built=%" PRIu64 " want >=%" PRIu64 " after full flood wave",
           (uint64_t)atomic_load_explicit(&status->psos_built, memory_order_relaxed),
           psosF + 100);
    for (int i = 0; i < 1000; i++) {
      if (atomic_load_explicit(&status->psos_pending, memory_order_relaxed) == 0)
        break;
      if (i == 999)
        FAIL("psos_pending stuck at %" PRIu64,
             (uint64_t)atomic_load_explicit(&status->psos_pending, memory_order_relaxed));
      usleep(10000);
    }
    if (atomic_load_explicit(&status->exec_errors, memory_order_relaxed))
      FAIL("exec_errors=%" PRIu64 " after flood",
           (uint64_t)atomic_load_explicit(&status->exec_errors, memory_order_relaxed));
    if (atomic_load_explicit(&g_ring->stalls, memory_order_relaxed))
      FAIL("ring stalls=%" PRIu64 " after flood",
           (uint64_t)atomic_load_explicit(&g_ring->stalls, memory_order_relaxed));
    printf("[test_feed] flood drained in %d waves: +100 PSOs, wave drew "
           "100/100, skips stopped growing\n", floodWaves);

    /* --- third session: QUIESCE UNDER OUTSTANDING COMPILES — the v6
     * real-game deadlock shape. 64 fresh libraries land on the create
     * worker and the quiesce record arrives while it is mid-queue; the
     * handler must abandon the queue and ack WITHOUT any cross-thread
     * wait. A hang here fails the epoch wait and the [serverd] session
     * stage logs name the stuck stage. --- */
    for (uint32_t i = 0; i < 32; i++) {
      emit_shader(3000 + 2 * i, 0x7000 + 2 * i, 0, "vsMain", lib, lib_size);
      emit_shader(3001 + 2 * i, 0x7001 + 2 * i, 1, "fsMain", lib, lib_size);
      emit_bind_shader(0, 0x7000 + 2 * i);
      emit_bind_shader(1, 0x7001 + 2 * i);
      emit_draws(1); /* pending-create skips; keeps the table busy */
    }
    emit_session(0, 2); /* session-2 context died mid-compile-storm */
    if (wait_u64(&status->epoch, 4, 10000))
      FAIL("quiesce #2 not acked under outstanding compiles (epoch=%" PRIu64
           ") — DEADLOCK; check [serverd] session stage logs",
           (uint64_t)atomic_load_explicit(&status->epoch, memory_order_relaxed));
    int st2;
    if (waitpid(pid, &st2, WNOHANG) == pid)
      FAIL("server EXITED on quiesce #2 (status 0x%x)", st2);
    emit_session(1, 3); /* session-3 context connected */
    if (wait_u64(&status->epoch, 5, 5000))
      FAIL("session begin #3 not acked");
    if (atomic_load_explicit(&status->exec_opcode, memory_order_acquire) != 0)
      FAIL("exec_opcode=%u stuck nonzero after session churn",
           atomic_load_explicit(&status->exec_opcode, memory_order_relaxed));
    /* session 3: the create worker must still BUILD after the abandonment */
    emit_shader(4000, 0x8000, 0, "vsMain", lib, lib_size);
    emit_shader(4001, 0x8001, 1, "fsMain", lib, lib_size);
    if (draw_wave_converge(status, &frame, 0x8000, 1, 30) < 0)
      FAIL("post-abandon compile pipeline stuck (create worker dead?)");
    /* and the stream still consumes */
    for (uint32_t i = 0; i < 8; i++) {
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, i, 0};
      emit(&sr, sizeof(sr));
    }
    if (wait_u64(&status->progress, g_head, 5000))
      FAIL("session-3 records not consumed");
    printf("[test_feed] session 3: quiesced mid-storm (queue abandoned), "
           "reconnected, worker rebuilt + drew — same pid %d\n", (int)pid);
  }

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
  if (atomic_load_explicit(&g_ring->records, memory_order_relaxed) != g_records)
    FAIL("final records=%" PRIu64 " != emitted %" PRIu64 " (detach not counted?)",
         (uint64_t)atomic_load_explicit(&g_ring->records, memory_order_relaxed),
         g_records);
  /* heap ack: the finish drain completes every pending region */
  if (g_heap_head &&
      atomic_load_explicit(&g_heap->consumed, memory_order_acquire) != g_heap_head)
    FAIL("heap consumed=%" PRIu64 " != produced %" PRIu64 " (regions never acked)",
         (uint64_t)atomic_load_explicit(&g_heap->consumed, memory_order_relaxed),
         g_heap_head);
  printf("[test_feed] clean detach: exit 0, consumer_exited=1, records=%" PRIu64
         ", heap acked %" PRIu64 " B\n",
         g_records, g_heap_head);

  /* --- S3 v5 pid watchdog: a crashed client (no detach, no unlink) must
   * not leak the process-lifetime server — the server watches the client
   * pid embedded in the shm dir name (d9mt-<pid>-<seq>) and cleans its own
   * shm on death. --- */
  {
    pid_t child = fork();
    if (child == 0) {
      for (;;)
        pause(); /* stand-in client, killed below */
    }
    if (child < 0)
      FAIL("fork: %s", strerror(errno));
    char wdir[900];
    snprintf(wdir, sizeof(wdir), "%s/d9mt-%d-0", tmp ? tmp : "/tmp", (int)child);
    if (mkdir(wdir, 0700))
      FAIL("mkdir(%s): %s", wdir, strerror(errno));
    void *r2, *s2, *h2;
    const size_t wring = sizeof(struct d9mt_wire_ring) + (1u << 20);
    if (create_file(wdir, "ring", wring, &r2) ||
        create_file(wdir, "status", sizeof(struct d9mt_status), &s2) ||
        create_file(wdir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + (1u << 20), &h2))
      return 1;
    struct d9mt_wire_ring *wr = r2;
    wr->capacity = 1u << 20;
    wr->data_offset = sizeof(struct d9mt_wire_ring);
    wr->magic = D9MT_WIRE_RING_MAGIC;
    struct d9mt_wire_heap *wh = h2;
    wh->capacity = 1u << 20;
    wh->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
    wh->magic = D9MT_WIRE_HEAP_MAGIC;
    struct d9mt_status *ws = s2;

    pid_t wpid;
    char *wargv[] = {(char *)server_bin, wdir, "64", "64", NULL};
    if (posix_spawn(&wpid, server_bin, NULL, NULL, wargv, environ))
      FAIL("watchdog posix_spawn failed");
    for (int i = 0;; i++) {
      if (atomic_load_explicit(&ws->epoch, memory_order_acquire) == 1)
        break;
      int wst;
      if (waitpid(wpid, &wst, WNOHANG) == wpid)
        FAIL("watchdog server died before ready (0x%x)", wst);
      if (i >= 1000)
        FAIL("watchdog server not ready");
      usleep(10000);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    int wst = -1, wexited = 0;
    for (int i = 0; i < 300; i++) { /* pid check is ~10 Hz server-side */
      if (waitpid(wpid, &wst, WNOHANG) == wpid) {
        wexited = 1;
        break;
      }
      usleep(10000);
    }
    if (!wexited) {
      kill(wpid, SIGKILL);
      FAIL("server did NOT exit after client death (watchdog broken)");
    }
    if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0)
      FAIL("watchdog server exit status 0x%x", wst);
    char probe[1024];
    struct stat pst;
    snprintf(probe, sizeof(probe), "%s/ring", wdir);
    if (stat(probe, &pst) == 0)
      FAIL("watchdog: ring file NOT unlinked after client death");
    if (stat(wdir, &pst) == 0)
      FAIL("watchdog: shm dir NOT removed after client death");
    printf("[test_feed] pid watchdog: server exited + cleaned shm after "
           "client death\n");
  }

  /* --- S3 v8 wedge simulation: the create worker is artificially blocked
   * for 6 s inside its first texture create (D9MT_WEDGE_CREATE_MS test
   * hook). The consumer/heartbeat MUST stay decoupled: hb ticks, records
   * consume, presents complete, quiesce/begin ack — all while the worker
   * is wedged; the >5 s watchdog prints its WEDGED breadcrumb. --- */
  {
    char wdir[900];
    snprintf(wdir, sizeof(wdir), "%s/d9mt-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(wdir))
      FAIL("wedge mkdtemp: %s", strerror(errno));
    void *r3, *s3, *h3;
    if (create_file(wdir, "ring", sizeof(struct d9mt_wire_ring) + (1u << 20), &r3) ||
        create_file(wdir, "status", sizeof(struct d9mt_status), &s3) ||
        create_file(wdir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + (1u << 20), &h3))
      return 1;
    struct d9mt_wire_ring *wr = r3;
    wr->capacity = 1u << 20;
    wr->data_offset = sizeof(struct d9mt_wire_ring);
    wr->magic = D9MT_WIRE_RING_MAGIC;
    struct d9mt_wire_heap *wh = h3;
    wh->capacity = 1u << 20;
    wh->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
    wh->magic = D9MT_WIRE_HEAP_MAGIC;
    struct d9mt_status *ws = s3;

    /* rebase the producer globals onto this scenario's ring */
    g_ring = wr;
    g_data = (uint8_t *)r3 + sizeof(struct d9mt_wire_ring);
    g_head = 0;

    /* spawn with the wedge hook armed */
    int envc = 0;
    while (environ[envc])
      envc++;
    char **wenv = malloc(sizeof(char *) * (envc + 2));
    for (int i = 0; i < envc; i++)
      wenv[i] = environ[i];
    wenv[envc] = "D9MT_WEDGE_CREATE_MS=6000";
    wenv[envc + 1] = NULL;
    pid_t wpid;
    char *wargv[] = {(char *)server_bin, wdir, "64", "64", NULL};
    if (posix_spawn(&wpid, server_bin, NULL, NULL, wargv, wenv))
      FAIL("wedge posix_spawn failed");
    free(wenv);
    for (int i = 0;; i++) {
      if (atomic_load_explicit(&ws->epoch, memory_order_acquire) == 1)
        break;
      int wst;
      if (waitpid(wpid, &wst, WNOHANG) == wpid)
        FAIL("wedge server died before ready (0x%x)", wst);
      if (i >= 1000)
        FAIL("wedge server not ready");
      usleep(10000);
    }

    /* trip the wedge: one legal texture create */
    struct d9mt_wire_rec_create_texture wt;
    memset(&wt, 0, sizeof(wt));
    wt.hdr.opcode = d9mt_wire_op_create_texture;
    wt.hdr.size = sizeof(wt);
    wt.wire_id = 1;
    wt.client_handle = 0x1;
    wt.type = 1;
    wt.format = 80;
    wt.width = 64;
    wt.height = 64;
    wt.depth = 1;
    wt.mip_levels = 1;
    wt.array_layers = 1;
    wt.sample_count = 1;
    emit(&wt, sizeof(wt));
    usleep(200000); /* let the worker enter the wedge */

    /* 1. heartbeat keeps ticking while the worker is wedged */
    uint64_t whb0 = atomic_load_explicit(&ws->heartbeat, memory_order_acquire);
    usleep(500000);
    uint64_t whb1 = atomic_load_explicit(&ws->heartbeat, memory_order_acquire);
    if (whb1 <= whb0)
      FAIL("heartbeat FROZE while the worker was wedged (%" PRIu64 ") — "
           "consumer coupled to worker", whb0);

    /* 2. records keep consuming; binds to the building texture dummy-resolve */
    struct d9mt_wire_rec_bind_texture_by_id wb;
    memset(&wb, 0, sizeof(wb));
    wb.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
    wb.hdr.size = sizeof(wb);
    wb.stages = 0x2;
    wb.slot = 0;
    wb.image_wire_id = 1;
    wb.format = 80;
    emit(&wb, sizeof(wb));
    for (uint32_t i = 0; i < 100; i++) {
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, i, 0};
      emit(&sr, sizeof(sr));
    }
    if (wait_u64(&ws->progress, g_head, 3000))
      FAIL("consumer stalled while worker wedged");

    /* 3. presents complete during the wedge (sweep must not block) */
    emit_backbuffer_rt();
    emit_clear(1.f, 0.f, 0.f);
    emit_present(1);
    if (wait_u64(&ws->frames, 1, 5000))
      FAIL("present did not complete while worker wedged");

    /* 4. quiesce + begin ack during the wedge */
    emit_session(0, 1);
    if (wait_u64(&ws->epoch, 2, 3000))
      FAIL("quiesce did not ack while worker wedged");
    emit_session(1, 2);
    if (wait_u64(&ws->epoch, 3, 3000))
      FAIL("session begin did not ack while worker wedged");
    printf("[test_feed] wedge sim: hb ticked, %u records consumed, present + "
           "quiesce/begin acked — consumer provably decoupled\n", 103);

    /* let the wedge expire so the worker can drain, then detach cleanly */
    usleep(6000000);
    struct d9mt_wire_rec_nul wdetach = {{d9mt_wire_op_detach, 8, 0}};
    emit(&wdetach, sizeof(wdetach));
    int wst = -1, wexited = 0;
    for (int i = 0; i < 800; i++) {
      if (waitpid(wpid, &wst, WNOHANG) == wpid) {
        wexited = 1;
        break;
      }
      usleep(10000);
    }
    if (!wexited) {
      kill(wpid, SIGKILL);
      FAIL("wedge server did not exit after detach");
    }
    if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0)
      FAIL("wedge server exit status 0x%x", wst);
    char p3[1024];
    snprintf(p3, sizeof(p3), "%s/ring", wdir);
    unlink(p3);
    snprintf(p3, sizeof(p3), "%s/status", wdir);
    unlink(p3);
    snprintf(p3, sizeof(p3), "%s/heap", wdir);
    unlink(p3);
    rmdir(wdir);
  }

  /* teardown */
  char p[1024];
  snprintf(p, sizeof(p), "%s/ring", dir);
  unlink(p);
  snprintf(p, sizeof(p), "%s/status", dir);
  unlink(p);
  snprintf(p, sizeof(p), "%s/heap", dir);
  unlink(p);
  rmdir(dir);
  printf("PASS\n");
  return 0;
}
