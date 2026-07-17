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

/* Chunked create_shader emission (mirrors the client's chunking). */
static void
emit_shader(uint64_t wire_id, uint64_t client_handle, uint32_t stage,
            const char *entry, const uint8_t *lib, size_t lib_size) {
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

    if (wait_u64(&status->creates_rejected, 1, 5000))
      FAIL("poisoned create_texture not rejected (creates_rejected=%" PRIu64 ")",
           (uint64_t)atomic_load_explicit(&status->creates_rejected, memory_order_relaxed));
    if (wait_u64(&status->progress, g_head, 5000))
      FAIL("consumer stalled after poisoned create (the execOp=27 hang shape)");
    if (atomic_load_explicit(&status->creates_rejected, memory_order_relaxed) != 1)
      FAIL("creates_rejected=%" PRIu64 " want exactly 1 (legal creates rejected?)",
           (uint64_t)atomic_load_explicit(&status->creates_rejected, memory_order_relaxed));
    printf("[test_feed] v7: poisoned create rejected+counted, consumer alive; "
           "2992x1934 RT + 1024^2 mipped queued to the worker\n");
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

    /* stale-id probe: wire 777 lived (RED-filled) in SESSION 1; the quiesce
     * reset the table, so this bind must resolve the 1x1 BLACK dummy. Red =
     * ALIASING (reset broken); blue (the clear) = draw skipped. */
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
