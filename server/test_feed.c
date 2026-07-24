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
#include <mach/mach_time.h>

#include "../driver/mtl/mtl_blob_pool.h"
#include "../driver/mtl/mtl_futex.h"
#include "../driver/mtl/mtl_wire.h"
#include "d9mt_status.h"

extern char **environ;

#define RING_CAP (8u << 20)
#define HEAP_CAP (8u << 20)
/* P1 blob region appended to the heap file (mtl_wire.h layout: both sides
 * derive it from file size vs header capacity). Holds the blob-backed
 * VB/CB slices of the P1 gates — content written CLIENT-SIDE, zero
 * upload records. */
#define BLOB_CAP (1u << 20)
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
static struct d9mt_wire_query_ring *g_qring; /* occlusion results (in heap file) */

/* Producer-side space wait, mirroring the production unix-side wait
 * (unixcall 217 WMTWireWaitSpace): register in ring->space_waiters
 * (seq_cst, BEFORE the re-check — pairs with the consumer's post-tail-store
 * fence + waiter load), then futex-park on the consumer's tail cursor.
 * The S3 gate asserts the consumer (async PSO builds!) drains fast enough
 * that this NEVER trips. Bounded (~10 s) so a dead server fails loudly
 * instead of hanging the gate. */
static int
waitspace(uint64_t need) {
  uint64_t tail = atomic_load_explicit(&g_ring->tail, memory_order_acquire);
  if (g_head + need - tail <= RING_CAP)
    return 0;
  g_producer_waits++;
  atomic_fetch_add_explicit(&g_ring->space_waiters, 1, memory_order_seq_cst);
  int rc = 1;
  for (int i = 0; i < 200; i++) { /* 200 x 50 ms backstop slices = 10 s */
    tail = atomic_load_explicit(&g_ring->tail, memory_order_acquire);
    if (g_head + need - tail <= RING_CAP) {
      rc = 0;
      break;
    }
    d9mt_futex_wait(&g_ring->tail, tail, 50ull * 1000u * 1000u /* 50 ms */);
  }
  atomic_fetch_sub_explicit(&g_ring->space_waiters, 1, memory_order_relaxed);
  return rc;
}

/* Lost-wake test hook: suppress exactly one empty->nonempty kick so the
 * consumer's bounded timeout backstop is what consumes the record. */
static int g_skip_next_kick;

/* Write one record, publish head, kick. Handles the never-straddle-the-wrap
 * rule with an op_pad header. Kick protocol mirrors the client: wake the
 * consumer's &head park ONLY on the empty->nonempty transition (tail sampled
 * == the pre-record head); the kernel's value-compare in the consumer's wait
 * makes a wake-before-park race harmless. */
static void
emit(const void *rec, uint16_t size) {
  const uint64_t prev_head = g_head;
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
  if (atomic_load_explicit(&g_ring->tail, memory_order_relaxed) == prev_head) {
    if (g_skip_next_kick)
      g_skip_next_kick = 0;
    else
      d9mt_futex_wake_one(&g_ring->head);
  }
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

/* Argument-buffer shader emission (the REAL client's translated binding model):
 * ab=1, ab_word_count AB words (2 bytes each: {u8 kind, u8 reg}) prefix the
 * library at chunk_offset 0 (padded to 8), ab_table_index names the AB slot.
 * ab_words points to n_words*2 bytes. Single-chunk (test libs are small). */
static void
emit_shader_ab(uint64_t wire_id, uint64_t client_handle, uint32_t stage,
               const char *entry, const uint8_t *lib, size_t lib_size,
               const uint8_t *ab_words, uint32_t n_words,
               uint32_t ab_table_index) {
  uint32_t abBytes = n_words * 2u;
  uint32_t abPad = (abBytes + 7u) & ~7u;
  uint32_t payload = (uint32_t)((lib_size + 7u) & ~7u);
  uint16_t size =
      (uint16_t)(sizeof(struct d9mt_wire_rec_create_shader) + abPad + payload);
  uint8_t buf[sizeof(struct d9mt_wire_rec_create_shader) + 64 + 0xE008];
  memset(buf, 0, size);
  struct d9mt_wire_rec_create_shader *r = (void *)buf;
  r->hdr.opcode = d9mt_wire_op_create_shader;
  r->hdr.size = size;
  r->wire_id = wire_id;
  r->client_handle = client_handle;
  r->stage = stage;
  r->ab = 1;
  r->ab_word_count = n_words;
  r->ab_table_index = ab_table_index;
  r->total_size = lib_size;
  r->chunk_offset = 0;
  r->chunk_size = (uint32_t)lib_size;
  memset(r->cbv_slot_map, 0xFF, 8);
  snprintf(r->entry, sizeof(r->entry), "%s", entry);
  memcpy((uint8_t *)(r + 1), ab_words, abBytes);
  memcpy((uint8_t *)(r + 1) + abPad, lib, lib_size);
  emit(buf, size);
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

/* --- occlusion-query transport (HANDOFF-QUERIES.md) ----------------------- */

static void
emit_query(uint16_t op, uint64_t qid) {
  struct d9mt_wire_rec_query q = {{op, sizeof(q), 0}, qid};
  emit(&q, sizeof(q));
}

/* Client-protocol poll of the shared result ring (mtl_wire.h):
 * 1 = available (*samples/*serial filled), 0 = pending, -1 = evicted. */
static int
query_poll(uint64_t qid, uint64_t *samples, uint64_t *serial) {
  struct d9mt_wire_query_slot *slot =
      &g_qring->slots[qid & (D9MT_WIRE_QUERY_SLOTS - 1u)];
  uint64_t got = atomic_load_explicit(&slot->id, memory_order_acquire);
  if (got == qid) {
    uint64_t s = atomic_load_explicit(&slot->samples, memory_order_relaxed);
    uint64_t ser = atomic_load_explicit(&slot->serial, memory_order_relaxed);
    if (atomic_load_explicit(&slot->id, memory_order_acquire) == qid) {
      *samples = s;
      *serial = ser;
      return 1;
    }
    return 0;
  }
  return got > qid ? -1 : 0;
}

/* Bounded availability wait (the D3D9 GetData poll loop, test edition).
 * Returns 0 on available, nonzero on timeout/eviction. */
static int
query_wait(uint64_t qid, uint64_t *samples, uint64_t *serial, int timeout_ms) {
  for (int i = 0; i < timeout_ms; i++) {
    int rc = query_poll(qid, samples, serial);
    if (rc == 1)
      return 0;
    if (rc < 0)
      return 2;
    usleep(1000);
  }
  return 1;
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

/* qsort comparator for the kernel-wake latency gate percentiles */
static int
cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
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
      create_file(dir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + HEAP_CAP + BLOB_CAP,
                  &heap_mem))
    return 1;
  g_ring = ring_mem;
  g_data = (uint8_t *)ring_mem + sizeof(struct d9mt_wire_ring);
  struct d9mt_status *status = status_mem;
  g_heap = heap_mem;
  g_heap_data = (uint8_t *)heap_mem + D9MT_WIRE_HEAP_DATA_OFFSET;
  g_qring = (struct d9mt_wire_query_ring *)((uint8_t *)heap_mem +
                                            D9MT_WIRE_QUERY_RING_OFFSET);

  /* P1: PRE-FAULT the blob region (non-sparse) BEFORE the server spawns —
   * its newBufferWithBytesNoCopy wrap wires pages at wrap time; pages
   * faulted in later are invisible to the GPU (the S1 sparse-file finding;
   * the PoC's proven precondition). Mirrors winemetal_unix.c wire attach. */
  memset(g_heap_data + HEAP_CAP, 0, BLOB_CAP);
  msync(g_heap_data + HEAP_CAP, BLOB_CAP, MS_SYNC);

  g_ring->capacity = RING_CAP;
  g_ring->data_offset = sizeof(struct d9mt_wire_ring);
  g_ring->magic = D9MT_WIRE_RING_MAGIC; /* last: header now valid */

  g_heap->capacity = HEAP_CAP; /* upload capacity; blob = file size - this */
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

    /* --- persistent-backbuffer contract: the client's BLIT/COPY present
     * model (d3d9_swapchain.cpp) guarantees back-buffer content SURVIVES
     * presents — apps compose incrementally (a few tiles per frame) with no
     * clears between. Three frames each draw a DIFFERENT viewport quadrant
     * into the backbuffer, clear only before frame 1; the THIRD published
     * surface must contain ALL THREE quadrants. Fails against rotating-RT
     * behavior (each pool slot would hold only its own frame's quadrant). */
    {
      static const float quad[3][2] = {{0.f, 0.f}, {320.f, 0.f}, {0.f, 240.f}};
      emit_bind_shader(0, 0xA001); /* vsMain/fsMain: solid red triangle */
      emit_bind_shader(1, 0xA002);
      for (int i = 0; i < 3; i++) {
        emit_backbuffer_rt();
        if (i == 0)
          emit_clear(0.f, 0.f, 1.f); /* frame 1 ONLY: blue background */
        struct {
          struct d9mt_wire_rec_set_viewports rec;
          struct d9mt_wire_viewport vp;
        } qvp = {{{d9mt_wire_op_set_viewports, sizeof(qvp), 0}, 1, 0},
                 {quad[i][0], quad[i][1], 320.f, 240.f, 0.f, 1.f, 0, 0, 0, 0}};
        emit(&qvp, sizeof(qvp));
        emit_draws(1);
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("persistent-bb: frame %d never presented", i);
      }
      uint32_t ps = atomic_load_explicit(&status->present_slot, memory_order_acquire);
      IOSurfaceRef psf = IOSurfaceLookup(ids[ps]);
      if (!psf)
        FAIL("persistent-bb: IOSurfaceLookup failed");
      IOSurfaceLock(psf, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *pbase = IOSurfaceGetBaseAddress(psf);
      size_t pbpr = IOSurfaceGetBytesPerRow(psf);
      uint8_t tl[4], tr[4], bl[4], br[4];
      memcpy(tl, pbase + 120 * pbpr + 160 * 4, 4); /* frame-1 quadrant */
      memcpy(tr, pbase + 120 * pbpr + 480 * 4, 4); /* frame-2 quadrant */
      memcpy(bl, pbase + 360 * pbpr + 160 * 4, 4); /* frame-3 quadrant */
      memcpy(br, pbase + 360 * pbpr + 480 * 4, 4); /* untouched background */
      IOSurfaceUnlock(psf, kIOSurfaceLockReadOnly, NULL);
      CFRelease(psf);
      uint8_t redq[4] = {0, 0, 255, 255}, blueq[4] = {255, 0, 0, 255};
      if (memcmp(bl, redq, 4) != 0)
        FAIL("persistent-bb: frame-3's OWN quadrant missing "
             "(%02x%02x%02x%02x) — draw/publish broken outright", bl[0],
             bl[1], bl[2], bl[3]);
      if (memcmp(tl, redq, 4) != 0 || memcmp(tr, redq, 4) != 0)
        FAIL("persistent-bb: earlier quadrants LOST (tl=%02x%02x%02x%02x "
             "tr=%02x%02x%02x%02x) — backbuffer content did not survive "
             "presents (rotating render target)", tl[0], tl[1], tl[2], tl[3],
             tr[0], tr[1], tr[2], tr[3]);
      if (memcmp(br, blueq, 4) != 0)
        FAIL("persistent-bb: background clobbered (%02x%02x%02x%02x) — "
             "frame-1 clear did not survive", br[0], br[1], br[2], br[3]);
      /* restore the full viewport for the scenarios below */
      struct {
        struct d9mt_wire_rec_set_viewports rec;
        struct d9mt_wire_viewport vp;
      } fvp = {{{d9mt_wire_op_set_viewports, sizeof(fvp), 0}, 1, 0},
               {0.f, 0.f, 640.f, 480.f, 0.f, 1.f, 0, 0, 0, 0}};
      emit(&fvp, sizeof(fvp));
      printf("[test_feed] persistent-backbuffer contract: 3 incremental "
             "quadrants all present in the third published surface\n");
    }

    /* --- OCCLUSION-QUERY TRANSPORT GATES (HANDOFF-QUERIES.md) -------------
     * vsMain/fsMain are still bound (fullscreen red triangle), the full
     * 640x480 viewport was just restored, no depth attachment — a draw's
     * samplesPassed equals its rasterized pixel count. Results arrive via
     * the shared d9mt_wire_query_ring inside the heap file; publication is
     * GPU-completion-gated server-side. */
    {
      if (atomic_load_explicit(&g_qring->magic, memory_order_acquire) !=
          D9MT_WIRE_QUERY_RING_MAGIC)
        FAIL("query gate: result-ring magic missing (server never "
             "initialized the heap-file query ring)");

      uint64_t samples = 0, qserial = 0;

      /* (a1) empty bracket -> exactly 0 samples: a query that counted
       * nothing must answer 0, not the all-passed stub */
      emit_backbuffer_rt();
      emit_query(d9mt_wire_op_begin_query, 9001);
      emit_query(d9mt_wire_op_end_query, 9001);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("query gate: frame (a1) never presented");
      if (query_wait(9001, &samples, &qserial, 5000))
        FAIL("query gate: empty-bracket result never published");
      if (samples != 0)
        FAIL("query gate: empty bracket counted %llu samples (want 0)",
             (unsigned long long)samples);

      /* (a2) draw scissored to 1x1 -> exactly 1 sample: the counter follows
       * raster state (a scissored-out draw contributes nothing) */
      {
        struct {
          struct d9mt_wire_rec_set_viewports rec;
          struct d9mt_wire_viewport vp;
        } svp = {{{d9mt_wire_op_set_viewports, sizeof(svp), 0}, 1, 0},
                 {0.f, 0.f, 640.f, 480.f, 0.f, 1.f, 5, 5, 1, 1}};
        emit(&svp, sizeof(svp));
      }
      emit_query(d9mt_wire_op_begin_query, 9002);
      emit_draws(1);
      emit_query(d9mt_wire_op_end_query, 9002);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("query gate: frame (a2) never presented");
      if (query_wait(9002, &samples, &qserial, 5000))
        FAIL("query gate: 1x1-scissor result never published");
      if (samples != 1)
        FAIL("query gate: 1x1-scissor draw counted %llu samples (want 1)",
             (unsigned long long)samples);
      { /* restore full scissor */
        struct {
          struct d9mt_wire_rec_set_viewports rec;
          struct d9mt_wire_viewport vp;
        } rvp = {{{d9mt_wire_op_set_viewports, sizeof(rvp), 0}, 1, 0},
                 {0.f, 0.f, 640.f, 480.f, 0.f, 1.f, 0, 0, 0, 0}};
        emit(&rvp, sizeof(rvp));
      }

      /* (a3) fullscreen draw -> samples == the exact pixel area */
      emit_query(d9mt_wire_op_begin_query, 9003);
      emit_draws(1);
      emit_query(d9mt_wire_op_end_query, 9003);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("query gate: frame (a3) never presented");
      if (query_wait(9003, &samples, &qserial, 5000))
        FAIL("query gate: fullscreen result never published");
      if (samples != 640ull * 480ull)
        FAIL("query gate: fullscreen draw counted %llu samples (want %llu)",
             (unsigned long long)samples, 640ull * 480ull);

      /* (b) bracket spanning a PASS SPLIT: draw, rebind the same target
       * (closes the pass; the second draw materializes a fresh encoder),
       * draw again — the two per-encoder segments must SUM: 2x the area */
      emit_query(d9mt_wire_op_begin_query, 9004);
      emit_draws(1);
      emit_backbuffer_rt(); /* pass split mid-bracket */
      emit_draws(1);
      emit_query(d9mt_wire_op_end_query, 9004);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("query gate: frame (b) never presented");
      if (query_wait(9004, &samples, &qserial, 5000))
        FAIL("query gate: pass-split result never published");
      if (samples != 2ull * 640ull * 480ull)
        FAIL("query gate: pass-split bracket counted %llu samples (want %llu "
             "= 2 fullscreen segments summed)",
             (unsigned long long)samples, 2ull * 640ull * 480ull);

      /* (c) NO EARLY LIES: a bracket recorded into a NEVER-SUBMITTED frame
       * must stay pending (the result requires the covering submission's
       * completion signal); it publishes only after the present submits,
       * with a serial the completion counter has actually reached */
      emit_query(d9mt_wire_op_begin_query, 9005);
      emit_draws(1);
      emit_query(d9mt_wire_op_end_query, 9005);
      usleep(300000); /* watchdog sweeps every 10 ms — plenty of chances to lie */
      if (query_poll(9005, &samples, &qserial) != 0)
        FAIL("query gate: result published BEFORE its submission was even "
             "committed (early lie)");
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("query gate: frame (c) never presented");
      if (query_wait(9005, &samples, &qserial, 5000))
        FAIL("query gate: post-submit result never published");
      if (samples != 640ull * 480ull)
        FAIL("query gate: gated result counted %llu samples (want %llu)",
             (unsigned long long)samples, 640ull * 480ull);
      /* the status completed_serial mirror updates on the frame-publication
       * path — poll briefly (the RESULT was gated on the event itself; this
       * only validates the published serial names a real submission) */
      uint64_t completed = 0;
      for (int i = 0; i < 2000; i++) {
        completed = atomic_load_explicit(&status->completed_serial,
                                         memory_order_acquire);
        if (qserial != 0 && completed >= qserial)
          break;
        usleep(1000);
      }
      if (qserial == 0 || qserial > completed)
        FAIL("query gate: published serial %llu never became GPU-complete "
             "(completed_serial=%llu)",
             (unsigned long long)qserial, (unsigned long long)completed);

      printf("[test_feed] occlusion-query transport: empty=0, scissor1x1=1, "
             "fullscreen=307200, pass-split=614400 (segments summed), "
             "completion-gated (pending pre-submit; serial %llu <= "
             "completed %llu)\n",
             (unsigned long long)qserial, (unsigned long long)completed);
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

    /* --- V17 GAME-CREATED DS + DEFERRED DEPTH-SAMPLE: the GTA shape.
     * CreateDepthStencilSurface mints a FRESH attachment-only DS texture
     * (usage 0x40, fmt 260) that is (1) attached + depth-cleared to a
     * non-trivial value, (2) written by a depth-tested draw, then (3) bound
     * as a SAMPLED depth texture in a later pass (deferred lighting reads
     * depth) with the readback asserting the stored depth crossed the pass
     * boundary. A server that loses the create (session/citizenship bugs),
     * races the async create worker at set_rt (healed by the v17
     * late-resolve), drops the DS store action, or refuses depth-format
     * shader reads goes BLACK here — the lit-world blocker. --- */
    {
      struct d9mt_wire_rec_create_texture ct7;
      memset(&ct7, 0, sizeof(ct7));
      ct7.hdr.opcode = d9mt_wire_op_create_texture;
      ct7.hdr.size = sizeof(ct7);
      ct7.wire_id = 962;
      ct7.client_handle = 0xD962;
      ct7.type = 1; /* 2D */
      ct7.format = 260; /* Depth32Float_Stencil8 (client D24S8 mapping) */
      ct7.width = 640;
      ct7.height = 480;
      ct7.depth = 1;
      ct7.mip_levels = 1;
      ct7.array_layers = 1;
      ct7.sample_count = 1;
      ct7.usage = 0x40; /* MtlUsage::DepthStencil ONLY: attachment-only */
      emit(&ct7, sizeof(ct7));

      /* the deferred reader: vsTex (uv fullscreen, wire 102/0xA003 already
       * live) + fsDepth (depth2d sample -> grayscale) */
      emit_shader(964, 0xA00D, 1, "fsDepth", lib, lib_size);

      int dsOk = 0;
      for (int w = 1; w <= 30 && !dsOk; w++) {
        /* pass 1: backbuffer color + the game DS; depth clear 0.6 (a
         * non-trivial value distinguishable from 0/1); triangle at z=0
         * writes depth through LESSEQUAL */
        struct d9mt_wire_rec_set_render_targets_by_id rt7;
        memset(&rt7, 0, sizeof(rt7));
        rt7.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rt7.hdr.size = sizeof(rt7);
        rt7.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rt7.color[0].format = 80;
        rt7.color[0].width = 640;
        rt7.color[0].height = 480;
        rt7.color[0].sample_count = 1;
        rt7.depth_stencil.image_wire_id = 962;
        rt7.depth_stencil.format = 260;
        rt7.depth_stencil.width = 640;
        rt7.depth_stencil.height = 480;
        rt7.depth_stencil.sample_count = 1;
        emit(&rt7, sizeof(rt7));

        struct d9mt_wire_rec_clear_render_target_by_id zclr7;
        memset(&zclr7, 0, sizeof(zclr7));
        zclr7.hdr.opcode = d9mt_wire_op_clear_render_target_by_id;
        zclr7.hdr.size = sizeof(zclr7);
        zclr7.image_wire_id = 962;
        zclr7.clear_aspects = 0x2;
        zclr7.value.depth = 0.6f;
        emit(&zclr7, sizeof(zclr7));
        emit_clear(0.f, 0.f, 0.f);

        struct d9mt_wire_rec_set_depth_stencil_state dss7;
        memset(&dss7, 0, sizeof(dss7));
        dss7.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
        dss7.hdr.size = sizeof(dss7);
        dss7.depth_compare_op = 3; /* LessEqual */
        dss7.depth_test = 1;
        dss7.depth_write = 1;
        dss7.front.compare_op = dss7.back.compare_op = 7;
        dss7.front.read_mask = dss7.back.read_mask = 0xFF;
        dss7.front.write_mask = dss7.back.write_mask = 0xFF;
        emit(&dss7, sizeof(dss7));

        emit_bind_shader(0, 0xA005); /* cubeVs: half triangle, z=0 */
        emit_bind_shader(1, 0xA006);
        struct {
          struct d9mt_wire_rec_set_vertex_layout rec;
          struct d9mt_wire_vertex_attr attr;
          struct d9mt_wire_vertex_layout lay;
        } vl7 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl7), 0}, 1, 1},
                 {0, 0, 900 /* RGB32Float */, 0},
                 {0, 12, 0, 0}};
        emit(&vl7, sizeof(vl7));
        struct d9mt_wire_rec_bind_vertex_buffer bvb7;
        memset(&bvb7, 0, sizeof(bvb7));
        bvb7.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
        bvb7.hdr.size = sizeof(bvb7);
        bvb7.binding = 0;
        bvb7.stride = 12;
        bvb7.buf.buffer = 0xC950;
        bvb7.buf.length = 36;
        emit(&bvb7, sizeof(bvb7));
        struct d9mt_wire_rec_bind_index_buffer bib7;
        memset(&bib7, 0, sizeof(bib7));
        bib7.hdr.opcode = d9mt_wire_op_bind_index_buffer;
        bib7.hdr.size = sizeof(bib7);
        bib7.buf.buffer = 0xC951;
        bib7.buf.length = 8;
        bib7.index_type = 0;
        emit(&bib7, sizeof(bib7));
        struct {
          struct d9mt_wire_rec_push_data rec;
          float mvp[16];
        } pd7 = {{{d9mt_wire_op_push_data, sizeof(pd7), 0}, 1, 0, 64, 0},
                 {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        emit(&pd7, sizeof(pd7));
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } di7 = {{{d9mt_wire_op_draw_indexed, sizeof(di7), 0}, 1, 0},
                 {{3, 1, 0, 0, 0, 0}}};
        emit(&di7, sizeof(di7));

        /* pass 2: DEFERRED READ — backbuffer only (DS unbound), the SAME DS
         * bound as a fragment depth texture, fullscreen depth-visualize */
        struct d9mt_wire_rec_set_depth_stencil_state dss7b = dss7;
        dss7b.depth_compare_op = 7; /* Always */
        dss7b.depth_test = 0;
        dss7b.depth_write = 0;
        emit(&dss7b, sizeof(dss7b));
        struct d9mt_wire_rec_bind_texture_by_id bt7;
        memset(&bt7, 0, sizeof(bt7));
        bt7.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
        bt7.hdr.size = sizeof(bt7);
        bt7.stages = 0x2; /* fragment */
        bt7.slot = 0;
        bt7.image_wire_id = 962;
        bt7.format = 260;
        emit(&bt7, sizeof(bt7));
        emit_backbuffer_rt();
        emit_bind_shader(0, 0xA003); /* vsTex */
        emit_bind_shader(1, 0xA00D); /* fsDepth */
        struct d9mt_wire_rec_set_vertex_layout vl7b = {
            {d9mt_wire_op_set_vertex_layout, sizeof(vl7b), 0}, 0, 0};
        emit(&vl7b, sizeof(vl7b));
        emit_draws(1);

        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v17 game-DS: frame never presented");
        uint32_t s7 = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sf7 = IOSurfaceLookup(ids[s7]);
        if (!sf7)
          FAIL("v17 game-DS: IOSurfaceLookup failed");
        IOSurfaceLock(sf7, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *b7 = IOSurfaceGetBaseAddress(sf7);
        size_t r7 = IOSurfaceGetBytesPerRow(sf7);
        uint8_t center7[4], corner7[4];
        memcpy(center7, b7 + 240 * r7 + 266 * 4, 4); /* inside triangle under
                                                        either vertical
                                                        orientation */
        memcpy(corner7, b7 + 2 * r7 + 636 * 4, 4);   /* never covered */
        IOSurfaceUnlock(sf7, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sf7);
        /* corner: cleared depth 0.6 -> ~153 grayscale (non-trivial: neither
         * 0 nor 255). center: triangle wrote z=0 -> ~0. */
        int cornerOk = corner7[0] > 140 && corner7[0] < 166 &&
                       corner7[1] > 140 && corner7[1] < 166 &&
                       corner7[2] > 140 && corner7[2] < 166;
        int centerOk = center7[0] < 20 && center7[1] < 20 && center7[2] < 20;
        if (cornerOk && centerOk) {
          dsOk = 1;
          printf("[test_feed] v17 game-DS: create->attach->depth-write->"
                 "sample chain verified (corner=%u center=%u, wave %d)\n",
                 corner7[0], center7[0], w);
        } else if (w == 30) {
          FAIL("v17 game-DS: corner=%02x%02x%02x%02x (want ~0.6*255 gray: "
               "cleared depth stored + sampled) center=%02x%02x%02x%02x "
               "(want ~0: triangle depth written) — the game-created DS "
               "attach/store/sample chain diverges (the deferred-lighting "
               "black-world shape)",
               corner7[0], corner7[1], corner7[2], corner7[3],
               center7[0], center7[1], center7[2], center7[3]);
        } else {
          usleep(30000);
        }
      }

      /* restore demo-neutral depth state for later scenarios */
      struct d9mt_wire_rec_set_depth_stencil_state dss7off;
      memset(&dss7off, 0, sizeof(dss7off));
      dss7off.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
      dss7off.hdr.size = sizeof(dss7off);
      dss7off.depth_compare_op = 7; /* Always */
      dss7off.front.compare_op = dss7off.back.compare_op = 7;
      dss7off.front.read_mask = dss7off.back.read_mask = 0xFF;
      dss7off.front.write_mask = dss7off.back.write_mask = 0xFF;
      emit(&dss7off, sizeof(dss7off));
      /* unbind the depth texture so later scenarios see slot 0 fresh */
      struct d9mt_wire_rec_bind_texture_by_id bt7off;
      memset(&bt7off, 0, sizeof(bt7off));
      bt7off.hdr.opcode = d9mt_wire_op_bind_texture_by_id;
      bt7off.hdr.size = sizeof(bt7off);
      bt7off.stages = 0x2;
      bt7off.slot = 0;
      bt7off.image_wire_id = 0;
      emit(&bt7off, sizeof(bt7off));
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

      /* --- v14 THREE-FRAME rotating-offset repro: mirror the D3D9 constant
       * ring EXACTLY (d3d9_constant_buffer.cpp Alloc + snapshotDirtyUniforms).
       * bind_uniform_buffer ONCE at slot 4 (the frontend's CreateBuffer), then
       * per frame: bind_uniform_buffer_range(offset) THEN upload_buffer(dst=
       * offset) THEN draw — the client's Alloc-emits-range / draw-emits-upload
       * order. Three DISTINCT matrices at rotating offsets 0,256,512 must move
       * the triangle TWICE (A->B->C). A server that binds the CBV gpu-address
       * once and never re-applies the rotating range offset freezes on matrix
       * A (reads offset 0 forever) — frame B's assert then fails. --- */
      {
        uint8_t zeros2[1024] = {0};
        struct d9mt_wire_rec_create_buffer ccb2;
        memset(&ccb2, 0, sizeof(ccb2));
        ccb2.hdr.opcode = d9mt_wire_op_create_buffer;
        ccb2.hdr.size = sizeof(ccb2);
        ccb2.wire_id = 972;
        ccb2.client_handle = 0xC972;
        ccb2.size = sizeof(zeros2);
        emit(&ccb2, sizeof(ccb2));
        uint64_t z2g = 0;
        uint64_t z2off = heap_put(zeros2, sizeof(zeros2), &z2g);
        struct d9mt_wire_rec_upload_buffer ubz2;
        memset(&ubz2, 0, sizeof(ubz2));
        ubz2.hdr.opcode = d9mt_wire_op_upload_buffer;
        ubz2.hdr.size = sizeof(ubz2);
        ubz2.wire_id = 972;
        ubz2.client_handle = 0xC972;
        ubz2.size = sizeof(zeros2);
        ubz2.heap_offset = z2off;
        ubz2.heap_generation = z2g;
        emit(&ubz2, sizeof(ubz2));

        /* the three matrices (column-major) and their triangle centroids on a
         * 640x480 backbuffer (verts (-0.5,-0.5)(0.5,-0.5)(-0.5,0.5)):
         *   A identity                          -> (266,280)
         *   B scale0.5 + translate(+0.5,+0.5)   -> (453,140)
         *   C scale0.5 + translate(-0.5,-0.5)   -> (133,380) */
        float mA[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float mB[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0,
                        0, 0, 1, 0, 0.5f, 0.5f, 0, 1};
        float mC[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0,
                        0, 0, 1, 0, -0.5f, -0.5f, 0, 1};
        struct { float *m; uint64_t off; int cx, cy; int px, py; } fr[3] = {
            {mA, 0, 266, 280, 453, 140},   /* frame A: centroid A, B must clear */
            {mB, 256, 453, 140, 266, 280}, /* frame B: centroid B, A must clear */
            {mC, 512, 133, 380, 453, 140}, /* frame C: centroid C, B must clear */
        };
        uint8_t magenta3[4] = {255, 0, 255, 255}, dkblue3[4] = {40, 16, 16, 255};
        int boundOnce = 0;
        for (int f = 0; f < 3; f++) {
          int okF = 0;
          for (int w = 1; w <= 30 && !okF; w++) {
            emit_bind_shader(0, 0xA00B);
            emit_bind_shader(1, 0xA006);
            struct {
              struct d9mt_wire_rec_set_vertex_layout rec;
              struct d9mt_wire_vertex_attr attr;
              struct d9mt_wire_vertex_layout lay;
            } vl7 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl7), 0}, 1, 1},
                     {0, 0, 900, 0},
                     {0, 12, 0, 0}};
            emit(&vl7, sizeof(vl7));
            struct d9mt_wire_rec_bind_vertex_buffer bvb4;
            memset(&bvb4, 0, sizeof(bvb4));
            bvb4.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
            bvb4.hdr.size = sizeof(bvb4);
            bvb4.binding = 0;
            bvb4.stride = 12;
            bvb4.buf.buffer = 0xC950;
            bvb4.buf.length = 36;
            emit(&bvb4, sizeof(bvb4));
            struct d9mt_wire_rec_bind_index_buffer bib4;
            memset(&bib4, 0, sizeof(bib4));
            bib4.hdr.opcode = d9mt_wire_op_bind_index_buffer;
            bib4.hdr.size = sizeof(bib4);
            bib4.buf.buffer = 0xC951;
            bib4.buf.length = 8;
            bib4.index_type = 0;
            emit(&bib4, sizeof(bib4));

            /* bind_uniform_buffer ONCE (the frontend binds the whole buffer at
             * create; subsequent frames only advance the range) */
            if (!boundOnce) {
              struct d9mt_wire_rec_bind_uniform_buffer bub2;
              memset(&bub2, 0, sizeof(bub2));
              bub2.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
              bub2.hdr.size = sizeof(bub2);
              bub2.stages = 0x1;
              bub2.slot = 4;
              bub2.buf.buffer = 0xC972;
              bub2.buf.length = sizeof(zeros2);
              emit(&bub2, sizeof(bub2));
              boundOnce = 1;
            }

            /* client order: RANGE first (Alloc), THEN upload (draw time) */
            struct d9mt_wire_rec_bind_uniform_buffer_range bur3;
            memset(&bur3, 0, sizeof(bur3));
            bur3.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
            bur3.hdr.size = sizeof(bur3);
            bur3.stages = 0x1;
            bur3.slot = 4;
            bur3.offset = fr[f].off;
            bur3.length = 64;
            emit(&bur3, sizeof(bur3));

            uint64_t m3g = 0;
            uint64_t m3off = heap_put(fr[f].m, 64, &m3g);
            struct d9mt_wire_rec_upload_buffer ubm3 = ubz2;
            ubm3.dst_offset = fr[f].off;
            ubm3.size = 64;
            ubm3.heap_offset = m3off;
            ubm3.heap_generation = m3g;
            emit(&ubm3, sizeof(ubm3));

            struct d9mt_wire_rec_set_render_targets_by_id rt7;
            memset(&rt7, 0, sizeof(rt7));
            rt7.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
            rt7.hdr.size = sizeof(rt7);
            rt7.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
            rt7.color[0].format = 80;
            rt7.color[0].width = 640;
            rt7.color[0].height = 480;
            rt7.color[0].sample_count = 1;
            emit(&rt7, sizeof(rt7));
            emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
            struct {
              struct d9mt_wire_rec_draw_indexed rec;
              struct d9mt_wire_draw_indexed_args args[1];
            } di5 = {{{d9mt_wire_op_draw_indexed, sizeof(di5), 0}, 1, 0},
                     {{3, 1, 0, 0, 0, 0}}};
            emit(&di5, sizeof(di5));
            emit_present(++frame);
            if (wait_u64(&status->frames, frame, 15000))
              FAIL("v14 rotating-offset: frame %d never presented", f);
            uint32_t s8 = atomic_load_explicit(&status->present_slot,
                                               memory_order_acquire);
            IOSurfaceRef sf7 = IOSurfaceLookup(ids[s8]);
            if (!sf7)
              FAIL("v14 rotating-offset: IOSurfaceLookup failed");
            IOSurfaceLock(sf7, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *b7 = IOSurfaceGetBaseAddress(sf7);
            size_t r7 = IOSurfaceGetBytesPerRow(sf7);
            uint8_t here[4], prev[4];
            memcpy(here, b7 + fr[f].cy * r7 + fr[f].cx * 4, 4);
            memcpy(prev, b7 + fr[f].py * r7 + fr[f].px * 4, 4);
            IOSurfaceUnlock(sf7, kIOSurfaceLockReadOnly, NULL);
            CFRelease(sf7);
            if (!memcmp(here, magenta3, 4) && !memcmp(prev, dkblue3, 4)) {
              okF = 1;
              printf("[test_feed] v14 rotating-offset frame %d (off=%llu): "
                     "triangle at its matrix's centroid (%d,%d), prior "
                     "position clear (wave %d)\n",
                     f, (unsigned long long)fr[f].off, fr[f].cx, fr[f].cy, w);
            } else if (w == 30) {
              FAIL("v14 rotating-offset frame %d (off=%llu): "
                   "centroid(%d,%d)=%02x%02x%02x%02x (want magenta) "
                   "prior(%d,%d)=%02x%02x%02x%02x (want clear) — the server "
                   "froze on an earlier matrix (rotating range offset not "
                   "applied per draw)",
                   f, (unsigned long long)fr[f].off, fr[f].cx, fr[f].cy,
                   here[0], here[1], here[2], here[3], fr[f].px, fr[f].py,
                   prev[0], prev[1], prev[2], prev[3]);
            } else {
              usleep(30000);
            }
          }
        }
        printf("[test_feed] v14 rotating-offset: three matrices at offsets "
               "0/256/512 moved the triangle twice — per-draw range offset "
               "tracks the client ring\n");

        /* --- v14 ARGUMENT-BUFFER path: the SAME three-frame rotation, but the
         * vertex shader uses the translated AB binding model the REAL client
         * emits (air_builder shaders are ALL ab=1). The server's fn->ab draw
         * flush must rebuild the CBV handle word with the ROTATING range offset
         * every draw; a stale word freezes the triangle on matrix A. The
         * classic gate above cannot cover this path. --- */
        uint8_t abw[2] = {0 /*Cbv*/, 4 /*D3D9 slot 4*/};
        emit_shader_ab(973, 0xA00C, 0, "cubeCbvAbVs", lib, lib_size,
                       abw, 1, 21);
        for (int f = 0; f < 3; f++) {
          int okF = 0;
          for (int w = 1; w <= 30 && !okF; w++) {
            emit_bind_shader(0, 0xA00C); /* AB vertex shader */
            emit_bind_shader(1, 0xA006); /* cubeFs magenta (classic) */
            struct {
              struct d9mt_wire_rec_set_vertex_layout rec;
              struct d9mt_wire_vertex_attr attr;
              struct d9mt_wire_vertex_layout lay;
            } vl8 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl8), 0}, 1, 1},
                     {0, 0, 900, 0},
                     {0, 12, 0, 0}};
            emit(&vl8, sizeof(vl8));
            struct d9mt_wire_rec_bind_vertex_buffer bvb5;
            memset(&bvb5, 0, sizeof(bvb5));
            bvb5.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
            bvb5.hdr.size = sizeof(bvb5);
            bvb5.binding = 0;
            bvb5.stride = 12;
            bvb5.buf.buffer = 0xC950;
            bvb5.buf.length = 36;
            emit(&bvb5, sizeof(bvb5));
            struct d9mt_wire_rec_bind_index_buffer bib5;
            memset(&bib5, 0, sizeof(bib5));
            bib5.hdr.opcode = d9mt_wire_op_bind_index_buffer;
            bib5.hdr.size = sizeof(bib5);
            bib5.buf.buffer = 0xC951;
            bib5.buf.length = 8;
            bib5.index_type = 0;
            emit(&bib5, sizeof(bib5));

            /* CBV still bound at slot 4 (bind_uniform_buffer above); advance
             * the range THEN re-ship the matrix, client order. */
            struct d9mt_wire_rec_bind_uniform_buffer_range bur4;
            memset(&bur4, 0, sizeof(bur4));
            bur4.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
            bur4.hdr.size = sizeof(bur4);
            bur4.stages = 0x1;
            bur4.slot = 4;
            bur4.offset = fr[f].off;
            bur4.length = 64;
            emit(&bur4, sizeof(bur4));

            uint64_t m4g = 0;
            uint64_t m4off = heap_put(fr[f].m, 64, &m4g);
            struct d9mt_wire_rec_upload_buffer ubm4 = ubz2;
            ubm4.dst_offset = fr[f].off;
            ubm4.size = 64;
            ubm4.heap_offset = m4off;
            ubm4.heap_generation = m4g;
            emit(&ubm4, sizeof(ubm4));

            struct d9mt_wire_rec_set_render_targets_by_id rt8;
            memset(&rt8, 0, sizeof(rt8));
            rt8.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
            rt8.hdr.size = sizeof(rt8);
            rt8.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
            rt8.color[0].format = 80;
            rt8.color[0].width = 640;
            rt8.color[0].height = 480;
            rt8.color[0].sample_count = 1;
            emit(&rt8, sizeof(rt8));
            emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
            struct {
              struct d9mt_wire_rec_draw_indexed rec;
              struct d9mt_wire_draw_indexed_args args[1];
            } di6 = {{{d9mt_wire_op_draw_indexed, sizeof(di6), 0}, 1, 0},
                     {{3, 1, 0, 0, 0, 0}}};
            emit(&di6, sizeof(di6));
            emit_present(++frame);
            if (wait_u64(&status->frames, frame, 15000))
              FAIL("v14 AB rotating-offset: frame %d never presented", f);
            uint32_t s9 = atomic_load_explicit(&status->present_slot,
                                               memory_order_acquire);
            IOSurfaceRef sf8 = IOSurfaceLookup(ids[s9]);
            if (!sf8)
              FAIL("v14 AB rotating-offset: IOSurfaceLookup failed");
            IOSurfaceLock(sf8, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *b8 = IOSurfaceGetBaseAddress(sf8);
            size_t r8b = IOSurfaceGetBytesPerRow(sf8);
            uint8_t here[4], prev[4];
            memcpy(here, b8 + fr[f].cy * r8b + fr[f].cx * 4, 4);
            memcpy(prev, b8 + fr[f].py * r8b + fr[f].px * 4, 4);
            IOSurfaceUnlock(sf8, kIOSurfaceLockReadOnly, NULL);
            CFRelease(sf8);
            if (!memcmp(here, magenta3, 4) && !memcmp(prev, dkblue3, 4)) {
              okF = 1;
              printf("[test_feed] v14 AB rotating-offset frame %d (off=%llu): "
                     "AB-model draw tracked its matrix at (%d,%d), prior clear "
                     "(wave %d)\n",
                     f, (unsigned long long)fr[f].off, fr[f].cx, fr[f].cy, w);
            } else if (w == 30) {
              FAIL("v14 AB rotating-offset frame %d (off=%llu): "
                   "centroid(%d,%d)=%02x%02x%02x%02x (want magenta) "
                   "prior(%d,%d)=%02x%02x%02x%02x (want clear) — the AB draw "
                   "flush froze on an earlier matrix (CBV handle word not "
                   "rebuilt with the rotating range offset)",
                   f, (unsigned long long)fr[f].off, fr[f].cx, fr[f].cy,
                   here[0], here[1], here[2], here[3], fr[f].px, fr[f].py,
                   prev[0], prev[1], prev[2], prev[3]);
            } else {
              usleep(30000);
            }
          }
        }
        printf("[test_feed] v14 AB rotating-offset: the translated argument-"
               "buffer draw path tracks the rotating range offset too\n");

        /* --- v14 MID-PASS rotation: TWO draws in ONE frame with a range+upload
         * BETWEEN them. The mid-pass upload's ensureXfer CLOSES the render pass
         * (HANDOFF-S3 V13 honest-flag); the second draw re-materializes it and
         * must re-flush the CBV at the ADVANCED offset. Both matrices must land
         * (triangle A AND triangle B visible) — a server that keeps draw-1's
         * offset for draw-2 draws A twice and B's centroid stays clear. --- */
        int midOk = 0;
        for (int w = 1; w <= 30 && !midOk; w++) {
          emit_bind_shader(0, 0xA00B); /* classic cubeCbvVs */
          emit_bind_shader(1, 0xA006);
          struct {
            struct d9mt_wire_rec_set_vertex_layout rec;
            struct d9mt_wire_vertex_attr attr;
            struct d9mt_wire_vertex_layout lay;
          } vl9 = {{{d9mt_wire_op_set_vertex_layout, sizeof(vl9), 0}, 1, 1},
                   {0, 0, 900, 0},
                   {0, 12, 0, 0}};
          emit(&vl9, sizeof(vl9));
          struct d9mt_wire_rec_bind_vertex_buffer bvb6;
          memset(&bvb6, 0, sizeof(bvb6));
          bvb6.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
          bvb6.hdr.size = sizeof(bvb6);
          bvb6.binding = 0;
          bvb6.stride = 12;
          bvb6.buf.buffer = 0xC950;
          bvb6.buf.length = 36;
          emit(&bvb6, sizeof(bvb6));
          struct d9mt_wire_rec_bind_index_buffer bib6;
          memset(&bib6, 0, sizeof(bib6));
          bib6.hdr.opcode = d9mt_wire_op_bind_index_buffer;
          bib6.hdr.size = sizeof(bib6);
          bib6.buf.buffer = 0xC951;
          bib6.buf.length = 8;
          bib6.index_type = 0;
          emit(&bib6, sizeof(bib6));

          /* draw 1 at offset 0 (matrix A) */
          struct d9mt_wire_rec_bind_uniform_buffer_range bA;
          memset(&bA, 0, sizeof(bA));
          bA.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bA.hdr.size = sizeof(bA);
          bA.stages = 0x1; bA.slot = 4; bA.offset = 0; bA.length = 64;
          emit(&bA, sizeof(bA));
          uint64_t gA = 0; uint64_t oA = heap_put(mA, 64, &gA);
          struct d9mt_wire_rec_upload_buffer uA = ubz2;
          uA.dst_offset = 0; uA.size = 64; uA.heap_offset = oA; uA.heap_generation = gA;
          emit(&uA, sizeof(uA));
          struct d9mt_wire_rec_set_render_targets_by_id rt9;
          memset(&rt9, 0, sizeof(rt9));
          rt9.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
          rt9.hdr.size = sizeof(rt9);
          rt9.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
          rt9.color[0].format = 80; rt9.color[0].width = 640;
          rt9.color[0].height = 480; rt9.color[0].sample_count = 1;
          emit(&rt9, sizeof(rt9));
          emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dA = {{{d9mt_wire_op_draw_indexed, sizeof(dA), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dA, sizeof(dA));

          /* mid-pass: advance the range + re-ship matrix B, then draw 2 (no
           * new RT/clear — same frame, same targets) */
          struct d9mt_wire_rec_bind_uniform_buffer_range bB = bA;
          bB.offset = 256;
          emit(&bB, sizeof(bB));
          uint64_t gB = 0; uint64_t oB = heap_put(mB, 64, &gB);
          struct d9mt_wire_rec_upload_buffer uB = ubz2;
          uB.dst_offset = 256; uB.size = 64; uB.heap_offset = oB; uB.heap_generation = gB;
          emit(&uB, sizeof(uB));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dB = {{{d9mt_wire_op_draw_indexed, sizeof(dB), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dB, sizeof(dB));
          emit_present(++frame);
          if (wait_u64(&status->frames, frame, 15000))
            FAIL("v14 mid-pass rotation: frame never presented");
          uint32_t sA = atomic_load_explicit(&status->present_slot,
                                             memory_order_acquire);
          IOSurfaceRef sfA = IOSurfaceLookup(ids[sA]);
          if (!sfA)
            FAIL("v14 mid-pass rotation: IOSurfaceLookup failed");
          IOSurfaceLock(sfA, kIOSurfaceLockReadOnly, NULL);
          const uint8_t *bA2 = IOSurfaceGetBaseAddress(sfA);
          size_t rA = IOSurfaceGetBytesPerRow(sfA);
          uint8_t cA[4], cB[4];
          memcpy(cA, bA2 + 280 * rA + 266 * 4, 4); /* triangle A centroid */
          memcpy(cB, bA2 + 140 * rA + 453 * 4, 4); /* triangle B centroid */
          IOSurfaceUnlock(sfA, kIOSurfaceLockReadOnly, NULL);
          CFRelease(sfA);
          if (!memcmp(cA, magenta3, 4) && !memcmp(cB, magenta3, 4)) {
            midOk = 1;
            printf("[test_feed] v14 mid-pass rotation: both draws in one frame "
                   "rendered their own matrix (A@266,280 + B@453,140) across "
                   "the ensureXfer pass close (wave %d)\n", w);
          } else if (w == 30) {
            FAIL("v14 mid-pass rotation: A=%02x%02x%02x%02x B=%02x%02x%02x%02x "
                 "(both want magenta) — the second draw did not pick up the "
                 "advanced range offset after the mid-pass pass close",
                 cA[0], cA[1], cA[2], cA[3], cB[0], cB[1], cB[2], cB[3]);
          } else {
            usleep(30000);
          }
        }
      }
    }

    /* --- V15 UPLOAD-FLOOD + MID-PASS-STAGING GATES -----------------------
     * Real D3D9 games bind a FRESH constant-ring slice per draw; the v13
     * re-ship emits one upload_buffer per draw. The server used to execute
     * each via ensureXfer, CLOSING the open render pass (~292 passes/frame
     * on GTA, ~60 expected). V15 memcpys non-referenced ranges straight
     * into the shared backing — these gates pin the pass count AND the
     * bytes. */
    {
      uint8_t magenta[4] = {255, 0, 255, 255}, dkblue[4] = {40, 16, 16, 255};

      /* the flood constant ring: 500 slices x 256 B */
      struct d9mt_wire_rec_create_buffer cfb;
      memset(&cfb, 0, sizeof(cfb));
      cfb.hdr.opcode = d9mt_wire_op_create_buffer;
      cfb.hdr.size = sizeof(cfb);
      cfb.wire_id = 973;
      cfb.client_handle = 0xC973;
      cfb.size = 131072;
      emit(&cfb, sizeof(cfb));

      /* rebind the cbv-cube pipeline state (belt: independent of what the
       * v14 sub-scenarios left bound) */
      emit_bind_shader(0, 0xA00B);
      emit_bind_shader(1, 0xA006);
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vlF = {{{d9mt_wire_op_set_vertex_layout, sizeof(vlF), 0}, 1, 1},
               {0, 0, 900 /* RGB32Float */, 0},
               {0, 12, 0, 0}};
      emit(&vlF, sizeof(vlF));
      struct d9mt_wire_rec_bind_vertex_buffer bvbF;
      memset(&bvbF, 0, sizeof(bvbF));
      bvbF.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
      bvbF.hdr.size = sizeof(bvbF);
      bvbF.binding = 0;
      bvbF.stride = 12;
      bvbF.buf.buffer = 0xC950;
      bvbF.buf.length = 36;
      emit(&bvbF, sizeof(bvbF));
      struct d9mt_wire_rec_bind_index_buffer bibF;
      memset(&bibF, 0, sizeof(bibF));
      bibF.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bibF.hdr.size = sizeof(bibF);
      bibF.buf.buffer = 0xC951;
      bibF.buf.length = 8;
      bibF.index_type = 0;
      emit(&bibF, sizeof(bibF));
      struct d9mt_wire_rec_bind_uniform_buffer bubF;
      memset(&bubF, 0, sizeof(bubF));
      bubF.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
      bubF.hdr.size = sizeof(bubF);
      bubF.stages = 0x1;
      bubF.slot = 4;
      bubF.buf.buffer = 0xC973;
      bubF.buf.length = 131072;
      emit(&bubF, sizeof(bubF));

      /* offscreen matrix (w=1, x translated out of clip) and the visible
       * final matrix (v13.1 shape: centroid 453,140; 266,280 stays clear) */
      float mOff[16] = {0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 1, 0, 5.f, 0, 0, 1};
      float mVis[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};

      /* warm-up frame: PSO + fill state settled before measuring */
      {
        uint64_t wg = 0;
        uint64_t woff = heap_put(mOff, sizeof(mOff), &wg);
        struct d9mt_wire_rec_upload_buffer ubw;
        memset(&ubw, 0, sizeof(ubw));
        ubw.hdr.opcode = d9mt_wire_op_upload_buffer;
        ubw.hdr.size = sizeof(ubw);
        ubw.wire_id = 973;
        ubw.client_handle = 0xC973;
        ubw.dst_offset = 0;
        ubw.size = sizeof(mOff);
        ubw.heap_offset = woff;
        ubw.heap_generation = wg;
        emit(&ubw, sizeof(ubw));
        struct d9mt_wire_rec_bind_uniform_buffer_range burW;
        memset(&burW, 0, sizeof(burW));
        burW.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
        burW.hdr.size = sizeof(burW);
        burW.stages = 0x1;
        burW.slot = 4;
        burW.offset = 0;
        burW.length = sizeof(mOff);
        emit(&burW, sizeof(burW));
        struct d9mt_wire_rec_set_render_targets_by_id rtW;
        memset(&rtW, 0, sizeof(rtW));
        rtW.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rtW.hdr.size = sizeof(rtW);
        rtW.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rtW.color[0].format = 80;
        rtW.color[0].width = 640;
        rtW.color[0].height = 480;
        rtW.color[0].sample_count = 1;
        emit(&rtW, sizeof(rtW));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } dW = {{{d9mt_wire_op_draw_indexed, sizeof(dW), 0}, 1, 0},
                {{3, 1, 0, 0, 0, 0}}};
        emit(&dW, sizeof(dW));
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v15 flood warm-up: frame never presented");
      }

      /* --- GATE (a) FLOOD: 500 draws, each with a FRESH 256 B constant
       * slice (bind_range + upload_buffer + draw — the exact client
       * emission order). Passes must stay O(scene): ONE materialized pass,
       * not one per upload. Every upload must EXECUTE (bytes land), and the
       * FINAL draw's constants must be the ones on screen. --- */
      {
        uint64_t enc0 = atomic_load_explicit(&status->encoders, memory_order_relaxed);
        uint64_t drw0 = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
        uint64_t upl0 = atomic_load_explicit(&status->uploads_executed, memory_order_relaxed);
        struct d9mt_wire_rec_set_render_targets_by_id rtF;
        memset(&rtF, 0, sizeof(rtF));
        rtF.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rtF.hdr.size = sizeof(rtF);
        rtF.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rtF.color[0].format = 80;
        rtF.color[0].width = 640;
        rtF.color[0].height = 480;
        rtF.color[0].sample_count = 1;
        emit(&rtF, sizeof(rtF));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        for (uint32_t i = 1; i <= 500; i++) {
          const float *m = (i == 500) ? mVis : mOff;
          uint64_t mg = 0;
          uint64_t moff = heap_put(m, 64, &mg);
          struct d9mt_wire_rec_bind_uniform_buffer_range bur;
          memset(&bur, 0, sizeof(bur));
          bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bur.hdr.size = sizeof(bur);
          bur.stages = 0x1;
          bur.slot = 4;
          bur.offset = 256ull * i;
          bur.length = 64;
          emit(&bur, sizeof(bur));
          struct d9mt_wire_rec_upload_buffer ub;
          memset(&ub, 0, sizeof(ub));
          ub.hdr.opcode = d9mt_wire_op_upload_buffer;
          ub.hdr.size = sizeof(ub);
          ub.wire_id = 973;
          ub.client_handle = 0xC973;
          ub.dst_offset = 256ull * i;
          ub.size = 64;
          ub.heap_offset = moff;
          ub.heap_generation = mg;
          emit(&ub, sizeof(ub));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dF = {{{d9mt_wire_op_draw_indexed, sizeof(dF), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dF, sizeof(dF));
        }
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 20000))
          FAIL("v15 flood: frame never presented");
        uint64_t encD = atomic_load_explicit(&status->encoders, memory_order_relaxed) - enc0;
        uint64_t drwD = atomic_load_explicit(&status->draws_executed, memory_order_relaxed) - drw0;
        uint64_t uplD = atomic_load_explicit(&status->uploads_executed, memory_order_relaxed) - upl0;
        if (drwD != 500)
          FAIL("v15 flood: %" PRIu64 "/500 draws executed", drwD);
        if (uplD != 500)
          FAIL("v15 flood: %" PRIu64 "/500 uploads executed", uplD);
        if (encD > 4)
          FAIL("v15 flood: %" PRIu64 " render passes for 500 uploading draws "
               "(want O(scene) ~1 — per-upload pass shatter is back)", encD);
        uint32_t sF = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sfF = IOSurfaceLookup(ids[sF]);
        if (!sfF)
          FAIL("v15 flood: IOSurfaceLookup failed");
        IOSurfaceLock(sfF, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bF = IOSurfaceGetBaseAddress(sfF);
        size_t rF = IOSurfaceGetBytesPerRow(sfF);
        uint8_t last[4], off[4];
        memcpy(last, bF + 140 * rF + 453 * 4, 4); /* final matrix centroid */
        memcpy(off, bF + 280 * rF + 266 * 4, 4);  /* offscreen matrices' spot */
        IOSurfaceUnlock(sfF, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sfF);
        if (memcmp(last, magenta, 4) || memcmp(off, dkblue, 4))
          FAIL("v15 flood: last=%02x%02x%02x%02x (want magenta) "
               "off=%02x%02x%02x%02x (want clear) — the 500th slice's bytes "
               "did not land where its draw reads",
               last[0], last[1], last[2], last[3], off[0], off[1], off[2], off[3]);
        printf("[test_feed] v15 flood: 500 fresh-slice uploading draws -> "
               "%" PRIu64 " render pass(es), 500/500 draws + uploads, final "
               "slice on screen\n", encD);
      }

      /* --- GATE (b) MID-PASS UPLOAD: two draws in ONE pass, the second's
       * constants uploaded BETWEEN them. The first draw's already-encoded
       * state must be unaffected (triangle A at its centroid), the second
       * must see the new bytes (triangle B at its centroid), and the pass
       * must NOT split. --- */
      {
        uint64_t enc0 = atomic_load_explicit(&status->encoders, memory_order_relaxed);
        float mA[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        struct d9mt_wire_rec_set_render_targets_by_id rtM;
        memset(&rtM, 0, sizeof(rtM));
        rtM.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rtM.hdr.size = sizeof(rtM);
        rtM.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rtM.color[0].format = 80;
        rtM.color[0].width = 640;
        rtM.color[0].height = 480;
        rtM.color[0].sample_count = 1;
        emit(&rtM, sizeof(rtM));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        for (int d = 0; d < 2; d++) {
          const float *m = d == 0 ? mA : mVis;
          uint64_t mg = 0;
          uint64_t moff = heap_put(m, 64, &mg);
          struct d9mt_wire_rec_bind_uniform_buffer_range bur;
          memset(&bur, 0, sizeof(bur));
          bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bur.hdr.size = sizeof(bur);
          bur.stages = 0x1;
          bur.slot = 4;
          bur.offset = 129024ull + 256ull * d; /* fresh, past the flood */
          bur.length = 64;
          emit(&bur, sizeof(bur));
          struct d9mt_wire_rec_upload_buffer ub;
          memset(&ub, 0, sizeof(ub));
          ub.hdr.opcode = d9mt_wire_op_upload_buffer;
          ub.hdr.size = sizeof(ub);
          ub.wire_id = 973;
          ub.client_handle = 0xC973;
          ub.dst_offset = 129024ull + 256ull * d;
          ub.size = 64;
          ub.heap_offset = moff;
          ub.heap_generation = mg;
          emit(&ub, sizeof(ub));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dM = {{{d9mt_wire_op_draw_indexed, sizeof(dM), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dM, sizeof(dM));
        }
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("v15 mid-pass upload: frame never presented");
        uint64_t encD = atomic_load_explicit(&status->encoders, memory_order_relaxed) - enc0;
        uint32_t sM = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sfM = IOSurfaceLookup(ids[sM]);
        if (!sfM)
          FAIL("v15 mid-pass upload: IOSurfaceLookup failed");
        IOSurfaceLock(sfM, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bM = IOSurfaceGetBaseAddress(sfM);
        size_t rM = IOSurfaceGetBytesPerRow(sfM);
        uint8_t cA[4], cB[4];
        memcpy(cA, bM + 280 * rM + 266 * 4, 4); /* triangle A centroid */
        memcpy(cB, bM + 140 * rM + 453 * 4, 4); /* triangle B centroid */
        IOSurfaceUnlock(sfM, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sfM);
        if (memcmp(cA, magenta, 4) || memcmp(cB, magenta, 4))
          FAIL("v15 mid-pass upload: A=%02x%02x%02x%02x B=%02x%02x%02x%02x "
               "(both want magenta) — mid-pass staged bytes broke a draw",
               cA[0], cA[1], cA[2], cA[3], cB[0], cB[1], cB[2], cB[3]);
        if (encD > 2)
          FAIL("v15 mid-pass upload: %" PRIu64 " render passes for one "
               "two-draw pass (mid-pass upload split it)", encD);
        printf("[test_feed] v15 mid-pass upload: both draws correct across a "
               "mid-pass constant upload, %" PRIu64 " pass(es)\n", encD);
      }

      /* --- GATE (c) PARTIAL DS CLEAR: full depth clear to 1.0, then a
       * SCISSORED partial clear (left half) to 0.0, then a fullscreen
       * triangle at z=0.5 with LESS: rejected inside the rect (clear color
       * shows), accepted outside (magenta). A dropped partial clear leaves
       * depth=1.0 everywhere -> magenta on both probes. Fill PSO builds
       * async -> wave retries (stash+drain covers wave 1). --- */
      {
        float mZ[16] = {8, 0, 0, 0, 0, 8, 0, 0, 0, 0, 1, 0, 2.f, 2.f, 0.5f, 1};
        int pcOk = 0;
        for (int w = 1; w <= 30 && !pcOk; w++) {
          struct d9mt_wire_rec_set_render_targets_by_id rtD;
          memset(&rtD, 0, sizeof(rtD));
          rtD.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
          rtD.hdr.size = sizeof(rtD);
          rtD.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
          rtD.color[0].format = 80;
          rtD.color[0].width = 640;
          rtD.color[0].height = 480;
          rtD.color[0].sample_count = 1;
          rtD.depth_stencil.image_wire_id = 960; /* v12 depth texture */
          rtD.depth_stencil.format = 260;
          rtD.depth_stencil.width = 640;
          rtD.depth_stencil.height = 480;
          rtD.depth_stencil.sample_count = 1;
          emit(&rtD, sizeof(rtD));
          emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
          struct d9mt_wire_rec_clear_image_view civ;
          memset(&civ, 0, sizeof(civ));
          civ.hdr.opcode = d9mt_wire_op_clear_image_view;
          civ.hdr.size = sizeof(civ);
          civ.wire_id = 960;
          civ.format = 260;
          civ.width = 640;
          civ.height = 480;
          civ.aspects = 0x2; /* depth */
          civ.value.depth = 1.0f;
          emit(&civ, sizeof(civ)); /* full: load-action mini-pass (V12) */
          civ.width = 320;         /* PARTIAL: left half to 0.0 */
          civ.value.depth = 0.0f;
          emit(&civ, sizeof(civ));
          struct d9mt_wire_rec_set_depth_stencil_state dss;
          memset(&dss, 0, sizeof(dss));
          dss.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
          dss.hdr.size = sizeof(dss);
          dss.depth_compare_op = 1; /* Less */
          dss.depth_test = 1;
          dss.front.compare_op = dss.back.compare_op = 7;
          dss.front.read_mask = dss.back.read_mask = 0xFF;
          dss.front.write_mask = dss.back.write_mask = 0xFF;
          emit(&dss, sizeof(dss));
          uint64_t mg = 0;
          uint64_t moff = heap_put(mZ, 64, &mg);
          struct d9mt_wire_rec_bind_uniform_buffer_range bur;
          memset(&bur, 0, sizeof(bur));
          bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bur.hdr.size = sizeof(bur);
          bur.stages = 0x1;
          bur.slot = 4;
          bur.offset = 130048;
          bur.length = 64;
          emit(&bur, sizeof(bur));
          struct d9mt_wire_rec_upload_buffer ub;
          memset(&ub, 0, sizeof(ub));
          ub.hdr.opcode = d9mt_wire_op_upload_buffer;
          ub.hdr.size = sizeof(ub);
          ub.wire_id = 973;
          ub.client_handle = 0xC973;
          ub.dst_offset = 130048;
          ub.size = 64;
          ub.heap_offset = moff;
          ub.heap_generation = mg;
          emit(&ub, sizeof(ub));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dD = {{{d9mt_wire_op_draw_indexed, sizeof(dD), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dD, sizeof(dD));
          emit_present(++frame);
          if (wait_u64(&status->frames, frame, 15000))
            FAIL("v15 partial DS clear: frame never presented");
          uint32_t sD = atomic_load_explicit(&status->present_slot, memory_order_acquire);
          IOSurfaceRef sfD = IOSurfaceLookup(ids[sD]);
          if (!sfD)
            FAIL("v15 partial DS clear: IOSurfaceLookup failed");
          IOSurfaceLock(sfD, kIOSurfaceLockReadOnly, NULL);
          const uint8_t *bD = IOSurfaceGetBaseAddress(sfD);
          size_t rD = IOSurfaceGetBytesPerRow(sfD);
          uint8_t in[4], out[4];
          memcpy(in, bD + 240 * rD + 160 * 4, 4);  /* inside cleared rect */
          memcpy(out, bD + 240 * rD + 480 * 4, 4); /* outside the rect */
          IOSurfaceUnlock(sfD, kIOSurfaceLockReadOnly, NULL);
          CFRelease(sfD);
          if (!memcmp(in, dkblue, 4) && !memcmp(out, magenta, 4)) {
            pcOk = 1;
            printf("[test_feed] v15 partial DS clear: scissored depth clear "
                   "rejected the draw inside the rect, accepted outside "
                   "(wave %d)\n", w);
          } else if (w == 30) {
            FAIL("v15 partial DS clear: in=%02x%02x%02x%02x (want clear: "
                 "depth 0.0 rejects z=0.5) out=%02x%02x%02x%02x (want "
                 "magenta: depth 1.0 accepts) — partial DS clear still "
                 "dropped/misplaced",
                 in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
          } else {
            usleep(30000);
          }
        }
        /* restore demo-neutral depth state + color-only RT for later
         * scenarios */
        struct d9mt_wire_rec_set_depth_stencil_state dssOff2;
        memset(&dssOff2, 0, sizeof(dssOff2));
        dssOff2.hdr.opcode = d9mt_wire_op_set_depth_stencil_state;
        dssOff2.hdr.size = sizeof(dssOff2);
        dssOff2.depth_compare_op = 7;
        dssOff2.front.compare_op = dssOff2.back.compare_op = 7;
        dssOff2.front.read_mask = dssOff2.back.read_mask = 0xFF;
        dssOff2.front.write_mask = dssOff2.back.write_mask = 0xFF;
        emit(&dssOff2, sizeof(dssOff2));
        struct d9mt_wire_rec_set_render_targets_by_id rtR;
        memset(&rtR, 0, sizeof(rtR));
        rtR.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rtR.hdr.size = sizeof(rtR);
        rtR.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rtR.color[0].format = 80;
        rtR.color[0].width = 640;
        rtR.color[0].height = 480;
        rtR.color[0].sample_count = 1;
        emit(&rtR, sizeof(rtR));
      }
    }

    /* --- V16 RING-WRAP RACE GATE: a tiny 512 B constant ring (two 256 B
     * slices) wraps every second draw, 4 draws/frame x 3 frames emitted
     * BACK-TO-BACK with NO completion wait between frames — the GPU runs
     * 1-3 frames behind while later uploads land on offsets that
     * still-executing draws read. Every draw's matrix places its triangle
     * in its OWN grid cell (row=frame, col=draw); a draw that observes a
     * LATER upload at its wrapped offset renders into the wrong cell and
     * leaves its own cell clear. The law under test: no mutation of
     * GPU-visible memory until completed_serial covers its last reader
     * (executor.mm rangeLive/markBufRef + the parked-blit ordering).
     * Run 20x — races are timing-shaped. --- */
    {
      uint8_t magentaW[4] = {255, 0, 255, 255}, dkblueW[4] = {40, 16, 16, 255};
      struct d9mt_wire_rec_create_buffer cwb;
      memset(&cwb, 0, sizeof(cwb));
      cwb.hdr.opcode = d9mt_wire_op_create_buffer;
      cwb.hdr.size = sizeof(cwb);
      cwb.wire_id = 975;
      cwb.client_handle = 0xC975;
      cwb.size = 512;
      emit(&cwb, sizeof(cwb));

      /* cbv-cube pipeline (same PSO the v15 gates warmed) */
      emit_bind_shader(0, 0xA00B);
      emit_bind_shader(1, 0xA006);
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vlW = {{{d9mt_wire_op_set_vertex_layout, sizeof(vlW), 0}, 1, 1},
               {0, 0, 900, 0},
               {0, 12, 0, 0}};
      emit(&vlW, sizeof(vlW));
      struct d9mt_wire_rec_bind_vertex_buffer bvbW;
      memset(&bvbW, 0, sizeof(bvbW));
      bvbW.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
      bvbW.hdr.size = sizeof(bvbW);
      bvbW.binding = 0;
      bvbW.stride = 12;
      bvbW.buf.buffer = 0xC950;
      bvbW.buf.length = 36;
      emit(&bvbW, sizeof(bvbW));
      struct d9mt_wire_rec_bind_index_buffer bibW;
      memset(&bibW, 0, sizeof(bibW));
      bibW.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bibW.hdr.size = sizeof(bibW);
      bibW.buf.buffer = 0xC951;
      bibW.buf.length = 8;
      bibW.index_type = 0;
      emit(&bibW, sizeof(bibW));
      struct d9mt_wire_rec_bind_uniform_buffer bubW;
      memset(&bubW, 0, sizeof(bubW));
      bubW.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
      bubW.hdr.size = sizeof(bubW);
      bubW.stages = 0x1;
      bubW.slot = 4;
      bubW.buf.buffer = 0xC975;
      bubW.buf.length = 512;
      emit(&bubW, sizeof(bubW));

      const float sW = 0.3f;
      for (int iter = 0; iter < 20; iter++) {
        for (int f = 0; f < 3; f++) {
          struct d9mt_wire_rec_set_render_targets_by_id rtW2;
          memset(&rtW2, 0, sizeof(rtW2));
          rtW2.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
          rtW2.hdr.size = sizeof(rtW2);
          rtW2.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
          rtW2.color[0].format = 80;
          rtW2.color[0].width = 640;
          rtW2.color[0].height = 480;
          rtW2.color[0].sample_count = 1;
          emit(&rtW2, sizeof(rtW2));
          if (f == 0) /* frames 1/2 accumulate onto the persistent BB */
            emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
          for (int d = 0; d < 4; d++) {
            const int k = f * 4 + d;
            const uint64_t off = (uint64_t)(k % 2) * 256u; /* THE WRAP */
            const float tx = -0.75f + 0.5f * (float)d;
            const float ty = 0.6f - 0.6f * (float)f;
            float mW[16] = {sW, 0, 0, 0, 0, sW, 0, 0,
                            0, 0, 1, 0, tx, ty, 0, 1};
            struct d9mt_wire_rec_bind_uniform_buffer_range burW2;
            memset(&burW2, 0, sizeof(burW2));
            burW2.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
            burW2.hdr.size = sizeof(burW2);
            burW2.stages = 0x1;
            burW2.slot = 4;
            burW2.offset = off;
            burW2.length = 64;
            emit(&burW2, sizeof(burW2));
            uint64_t mg = 0;
            uint64_t moff = heap_put(mW, 64, &mg);
            struct d9mt_wire_rec_upload_buffer ubW2;
            memset(&ubW2, 0, sizeof(ubW2));
            ubW2.hdr.opcode = d9mt_wire_op_upload_buffer;
            ubW2.hdr.size = sizeof(ubW2);
            ubW2.wire_id = 975;
            ubW2.client_handle = 0xC975;
            ubW2.dst_offset = off;
            ubW2.size = 64;
            ubW2.heap_offset = moff;
            ubW2.heap_generation = mg;
            emit(&ubW2, sizeof(ubW2));
            struct {
              struct d9mt_wire_rec_draw_indexed rec;
              struct d9mt_wire_draw_indexed_args args[1];
            } dWr = {{{d9mt_wire_op_draw_indexed, sizeof(dWr), 0}, 1, 0},
                     {{3, 1, 0, 0, 0, 0}}};
            emit(&dWr, sizeof(dWr));
          }
          emit_present(++frame); /* NO wait — keep the GPU frames behind */
        }
        if (wait_u64(&status->frames, frame, 20000))
          FAIL("v16 wrap race (iter %d): frames never presented", iter);
        uint32_t sWr = atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sfW = IOSurfaceLookup(ids[sWr]);
        if (!sfW)
          FAIL("v16 wrap race (iter %d): IOSurfaceLookup failed", iter);
        IOSurfaceLock(sfW, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bW = IOSurfaceGetBaseAddress(sfW);
        size_t rW = IOSurfaceGetBytesPerRow(sfW);
        uint8_t bg[4];
        memcpy(bg, bW + 5 * rW + 5 * 4, 4); /* outside every cell */
        int badK = -1;
        uint8_t badPix[4] = {0, 0, 0, 0};
        for (int k = 0; k < 12 && badK < 0; k++) {
          const float tx = -0.75f + 0.5f * (float)(k % 4);
          const float ty = 0.6f - 0.6f * (float)(k / 4);
          /* triangle centroid of scaled verts: (tx - s/6, ty - s/6) */
          const int px = (int)((tx - sW / 6.f + 1.f) * 320.f);
          const int py = (int)((1.f - (ty - sW / 6.f)) * 240.f);
          uint8_t c[4];
          memcpy(c, bW + py * rW + px * 4, 4);
          if (memcmp(c, magentaW, 4)) {
            badK = k;
            memcpy(badPix, c, 4);
          }
        }
        IOSurfaceUnlock(sfW, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sfW);
        if (badK >= 0)
          FAIL("v16 wrap race (iter %d): frame %d draw %d cell empty "
               "(%02x%02x%02x%02x, want magenta) — the draw read ANOTHER "
               "draw's constants at its wrapped ring offset (memcpy/blit "
               "raced encoded-but-incomplete GPU work)",
               iter, badK / 4, badK % 4,
               badPix[0], badPix[1], badPix[2], badPix[3]);
        if (memcmp(bg, dkblueW, 4))
          FAIL("v16 wrap race (iter %d): background %02x%02x%02x%02x not the "
               "clear color — geometry landed outside its cell",
               iter, bg[0], bg[1], bg[2], bg[3]);
      }
      printf("[test_feed] v16 wrap race: 20 iterations x 12 wrapping-offset "
             "draws across 3 unwaited frames — every draw rendered its OWN "
             "constants\n");
    }

    /* --- P1 BLOB GATE 0: the credit-law freelist (mtl_blob_pool.h — the
     * EXACT code the driver runs, compiled into this x86_64 binary). The
     * deliberate PREMATURE-REUSE probe: a slice retired under serial S must
     * NOT be handed out while completed < S — reusing it early is the PoC's
     * documented first failure (an offset reused before the consumer
     * completed the frame that used it reproduced the production
     * corruption). --- */
    {
      static struct d9mt_blob_pool pool; /* ~24 KiB — keep off the stack */
      d9mt_blob_init(&pool, HEAP_CAP, BLOB_CAP);
      uint64_t a = d9mt_blob_alloc(&pool, 100, 0);
      uint64_t b = d9mt_blob_alloc(&pool, 100, 0);
      if (a != (uint64_t)HEAP_CAP || b != (uint64_t)HEAP_CAP + D9MT_BLOB_ALIGN)
        FAIL("blob pool: bump layout wrong (a=%llu b=%llu)",
             (unsigned long long)a, (unsigned long long)b);
      d9mt_blob_retire(&pool, a, 100, 5); /* last reader = frame 5 */
      uint64_t c = d9mt_blob_alloc(&pool, 100, 4); /* completed = 4 < 5 */
      if (c == a)
        FAIL("blob pool: PREMATURE REUSE — slice retired under serial 5 "
             "handed out at completed 4 (the credit-law gate is broken)");
      if (c != (uint64_t)HEAP_CAP + 2 * D9MT_BLOB_ALIGN)
        FAIL("blob pool: expected fresh bump slice, got %llu",
             (unsigned long long)c);
      uint64_t d = d9mt_blob_alloc(&pool, 100, 5); /* completed covers 5 */
      if (d != a)
        FAIL("blob pool: covered slice NOT reused (got %llu, want %llu)",
             (unsigned long long)d, (unsigned long long)a);
      /* wire-down semantics: retire under serial 0 = provably reader-free */
      d9mt_blob_retire(&pool, b, 100, 0);
      if (d9mt_blob_alloc(&pool, 100, 0) != b)
        FAIL("blob pool: serial-0 retire not immediately reusable");
      printf("[test_feed] blob pool: premature reuse BLOCKED at "
             "completed<retire, granted at completed>=retire\n");
    }

    /* --- P1 BLOB GATE 0b: production-shaped SOAK (the street signature that
     * exhausted the 128 MiB region: blob= plateaued, blobFb= climbed).
     * Model, at 3x the measured street rate, 1000 frames, completion
     * THROTTLED 3 frames behind allocation:
     *   - scene-load burst (f=100): 6800 slices that stay LIVE (the street
     *     working set);
     *   - scene TRANSITION (f=500): the whole burst retires AT ONCE (floods
     *     the freelist — the old 1024 cap dropped 5.7k entries = leaked the
     *     region) and re-allocates with DIFFERENT sizes (the old exact-size-
     *     only reuse stranded every retired byte);
     *   - steady churn: 81 allocs/frame (3x street ~27), held 2 frames.
     * Asserts: zero pool drops (blobFb==0 equivalent), zero freelist drops
     * (no leak), and NO premature reuse (every granted slot is FREE or
     * cooling with serial <= completed) — checked per 16 KiB slot. --- */
    {
      enum { SOAK_SLOTS = 32768 };            /* 512 MiB / 16 KiB */
      static struct d9mt_blob_pool spool;     /* ~192 KiB, off the stack */
      static uint8_t  slotLive[SOAK_SLOTS];   /* 0=free 1=live 2=cooling */
      static uint64_t slotSerial[SOAK_SLOTS];
      static uint64_t burstOff[6800];
      static uint64_t churnOff[2][81], churnSz[2][81];
      const uint64_t soakBase = 128ull << 20;
      const uint64_t soakCap  = 512ull << 20;
      d9mt_blob_init(&spool, soakBase, soakCap);
      memset(slotLive, 0, sizeof(slotLive));
      uint64_t burstSz = 16384u; /* first scene: 16 KiB slices */
      int burstLive = 0;
      for (uint64_t f = 1; f <= 1000; f++) {
        const uint64_t completed = f > 3 ? f - 3 : 0; /* throttled consumer */
        /* helper-free alloc+assert (C: inline the checks) */
        #define SOAK_ALLOC(szv, outoff) do {                                   \
          uint64_t _off = d9mt_blob_alloc(&spool, (szv), completed);           \
          if (_off == UINT64_MAX)                                              \
            FAIL("blob soak: pool MISS at frame %llu (drops=%llu flDrops=%llu" \
                 " bump=%llu nfree=%u) — the fallback flood would return",     \
                 (unsigned long long)f, (unsigned long long)spool.drops,       \
                 (unsigned long long)spool.freelist_drops,                     \
                 (unsigned long long)spool.bump, spool.nfree);                 \
          uint64_t _s0 = (_off - soakBase) / D9MT_BLOB_ALIGN;                  \
          uint64_t _sn = d9mt_blob_round(szv) / D9MT_BLOB_ALIGN;               \
          for (uint64_t _s = _s0; _s < _s0 + _sn; _s++) {                      \
            if (slotLive[_s] == 1 ||                                           \
                (slotLive[_s] == 2 && slotSerial[_s] > completed))             \
              FAIL("blob soak: PREMATURE/ALIASED grant frame %llu slot %llu "  \
                   "(state=%u serial=%llu completed=%llu)",                    \
                   (unsigned long long)f, (unsigned long long)_s,              \
                   slotLive[_s], (unsigned long long)slotSerial[_s],           \
                   (unsigned long long)completed);                             \
            slotLive[_s] = 1;                                                  \
          }                                                                    \
          (outoff) = _off;                                                     \
        } while (0)
        #define SOAK_RETIRE(offv, szv) do {                                    \
          d9mt_blob_retire(&spool, (offv), (szv), f + 1);                      \
          uint64_t _s0 = ((offv) - soakBase) / D9MT_BLOB_ALIGN;                \
          uint64_t _sn = d9mt_blob_round(szv) / D9MT_BLOB_ALIGN;               \
          for (uint64_t _s = _s0; _s < _s0 + _sn; _s++) {                      \
            slotLive[_s] = 2;                                                  \
            slotSerial[_s] = f + 1;                                            \
          }                                                                    \
        } while (0)
        if (f == 100 || f == 500) {
          if (burstLive) { /* scene transition: mass retire, then new sizes */
            for (int i = 0; i < 6800; i++)
              SOAK_RETIRE(burstOff[i], burstSz);
            burstSz = 32768u; /* the next scene's slices are a DIFFERENT size */
          }
          for (int i = 0; i < 6800; i++)
            SOAK_ALLOC(burstSz, burstOff[i]);
          burstLive = 1;
        }
        /* steady churn: retire the batch held 2 frames, allocate this one */
        const int slotIdx = (int)(f & 1u);
        if (f > 2) {
          for (int i = 0; i < 81; i++)
            SOAK_RETIRE(churnOff[slotIdx][i], churnSz[slotIdx][i]);
        }
        for (int i = 0; i < 81; i++) {
          const uint64_t sz = i < 60 ? 16384u : (i < 75 ? 65536u : 262144u);
          churnSz[slotIdx][i] = sz;
          SOAK_ALLOC(sz, churnOff[slotIdx][i]);
        }
        #undef SOAK_ALLOC
        #undef SOAK_RETIRE
      }
      if (spool.drops != 0 || spool.freelist_drops != 0)
        FAIL("blob soak: drops=%llu flDrops=%llu after clean run",
             (unsigned long long)spool.drops,
             (unsigned long long)spool.freelist_drops);
      printf("[test_feed] blob soak: 1000 frames @3x street rate + 2 scene "
             "bursts (6800 slices, size shift 16K->32K), throttled "
             "completion — 0 misses, 0 freelist drops, 0 premature grants\n");
    }

    /* --- P1 BLOB GATE A: the PoC scenario through the REAL server
     * (poc_blobmodel ported onto the wire): blob-backed VB + CB created
     * with {heap_offset,size}, per-frame slices written CLIENT-SIDE into
     * the shared blob region, draws at rotating offsets, offsets REUSED
     * across completion (12 frames, rotation depth 4 => every slice reused
     * 3x), pixel-asserted per frame. ZERO upload_buffer records — the
     * pages are the buffer (VENUS_CONTRACT_DIFF P1).
     * Staleness is detectable per carrier: the vertex slice bakes the cell
     * COLUMN, the constant slice carries the cell ROW — a stale slice at a
     * reused offset paints the wrong cell and leaves the frame's own cell
     * on the clear color. --- */
    {
      uint8_t magentaB[4] = {255, 0, 255, 255}, dkblueB[4] = {40, 16, 16, 255};
      const uint64_t upl0 =
          atomic_load_explicit(&status->uploads_executed, memory_order_relaxed);
      /* blob CB: 4 x 256 B rotating matrix slices at blob offset 0 */
      const uint64_t cbBlobOff = (uint64_t)HEAP_CAP;
      /* blob VB: 4 x 256 B rotating vertex slices, next 16 KiB page */
      const uint64_t vbBlobOff = (uint64_t)HEAP_CAP + D9MT_BLOB_ALIGN;
      struct d9mt_wire_rec_create_buffer cBb;
      memset(&cBb, 0, sizeof(cBb));
      cBb.hdr.opcode = d9mt_wire_op_create_buffer;
      cBb.hdr.size = sizeof(cBb);
      cBb.wire_id = 990;
      cBb.client_handle = 0xC990;
      cBb.size = 1024;
      cBb.heap_offset = cbBlobOff; /* THE blob create */
      emit(&cBb, sizeof(cBb));
      struct d9mt_wire_rec_create_buffer cBv = cBb;
      cBv.wire_id = 991;
      cBv.client_handle = 0xC991;
      cBv.size = 1024;
      cBv.heap_offset = vbBlobOff;
      emit(&cBv, sizeof(cBv));

      /* cbv-cube pipeline (same PSO the v15/v16 gates warmed) */
      emit_bind_shader(0, 0xA00B);
      emit_bind_shader(1, 0xA006);
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vlB = {{{d9mt_wire_op_set_vertex_layout, sizeof(vlB), 0}, 1, 1},
               {0, 0, 900 /* RGB32Float */, 0},
               {0, 12, 0, 0}};
      emit(&vlB, sizeof(vlB));
      struct d9mt_wire_rec_bind_index_buffer bibB;
      memset(&bibB, 0, sizeof(bibB));
      bibB.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bibB.hdr.size = sizeof(bibB);
      bibB.buf.buffer = 0xC951; /* legacy IB (u16 0,1,2) — IBs are P1-eligible
                                 * too; the gate pins VB+CB, the PoC pair */
      bibB.buf.length = 8;
      bibB.index_type = 0;
      emit(&bibB, sizeof(bibB));
      struct d9mt_wire_rec_bind_uniform_buffer bubB;
      memset(&bubB, 0, sizeof(bubB));
      bubB.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
      bubB.hdr.size = sizeof(bubB);
      bubB.stages = 0x1;
      bubB.slot = 4;
      bubB.buf.buffer = 0xC990;
      bubB.buf.length = 1024;
      emit(&bubB, sizeof(bubB));

      const float sB = 0.3f;
      for (int n = 0; n < 12; n++) {
        const int slot = n % 4;         /* rotation: offsets reused at n-4 */
        const int col = n % 4, row = n / 4;
        const float tx = -0.75f + 0.5f * (float)col;
        const float ty = 0.6f - 0.6f * (float)row;
        /* THE LAW (poc_blobmodel_writer.c): an offset may not be reused
         * until completion covers its last reader. The per-frame waits
         * below run this gate in lockstep (0 frames ahead — well inside
         * the N=2 credit bound); assert it before every REUSING write so a
         * pacing regression shows up loudly instead of as corruption. */
        if (n >= 4) {
          uint64_t fr =
              atomic_load_explicit(&status->frames, memory_order_acquire);
          if (fr < (uint64_t)frame)
            FAIL("blob rotation: slice reuse ahead of completion "
                 "(frames=%llu, last present=%llu)",
                 (unsigned long long)fr, (unsigned long long)frame);
        }
        /* client-side slice writes — NO records, NO heap_put: */
        /* vertex slice: base triangle scaled by sB, COLUMN translate baked */
        float vbv[9] = {
          (-0.5f) * sB + tx, (-0.5f) * sB, 0.f,
          ( 0.5f) * sB + tx, (-0.5f) * sB, 0.f,
          (-0.5f) * sB + tx, ( 0.5f) * sB, 0.f};
        memcpy(g_heap_data + vbBlobOff + (uint64_t)slot * 256u, vbv, sizeof(vbv));
        /* constant slice: identity with the ROW translate */
        float mB[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, ty, 0, 1};
        memcpy(g_heap_data + cbBlobOff + (uint64_t)slot * 256u, mB, sizeof(mB));

        struct d9mt_wire_rec_set_render_targets_by_id rtB;
        memset(&rtB, 0, sizeof(rtB));
        rtB.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
        rtB.hdr.size = sizeof(rtB);
        rtB.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
        rtB.color[0].format = 80;
        rtB.color[0].width = 640;
        rtB.color[0].height = 480;
        rtB.color[0].sample_count = 1;
        emit(&rtB, sizeof(rtB));
        if (n == 0)
          emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        struct d9mt_wire_rec_bind_vertex_buffer bvbB;
        memset(&bvbB, 0, sizeof(bvbB));
        bvbB.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
        bvbB.hdr.size = sizeof(bvbB);
        bvbB.binding = 0;
        bvbB.stride = 12;
        bvbB.buf.buffer = 0xC991;
        bvbB.buf.offset = (uint64_t)slot * 256u; /* draw at the slice offset */
        bvbB.buf.length = 36;
        emit(&bvbB, sizeof(bvbB));
        struct d9mt_wire_rec_bind_uniform_buffer_range burB;
        memset(&burB, 0, sizeof(burB));
        burB.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
        burB.hdr.size = sizeof(burB);
        burB.stages = 0x1;
        burB.slot = 4;
        burB.offset = (uint64_t)slot * 256u;
        burB.length = 64;
        emit(&burB, sizeof(burB));
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } dB = {{{d9mt_wire_op_draw_indexed, sizeof(dB), 0}, 1, 0},
                {{3, 1, 0, 0, 0, 0}}};
        emit(&dB, sizeof(dB));
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("blob rotation: frame %d never presented", n);
        /* per-frame pixel assert: THIS frame's cell carries this frame's
         * client-written slices (centroid of the baked-column triangle
         * under the row matrix — same formula as v16) */
        uint32_t sBl =
            atomic_load_explicit(&status->present_slot, memory_order_acquire);
        IOSurfaceRef sfB = IOSurfaceLookup(ids[sBl]);
        if (!sfB)
          FAIL("blob rotation: IOSurfaceLookup failed (frame %d)", n);
        IOSurfaceLock(sfB, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bB = IOSurfaceGetBaseAddress(sfB);
        size_t rB = IOSurfaceGetBytesPerRow(sfB);
        const int pxB = (int)((tx - sB / 6.f + 1.f) * 320.f);
        const int pyB = (int)((1.f - (ty - sB / 6.f)) * 240.f);
        uint8_t cell[4], bgB[4];
        memcpy(cell, bB + pyB * rB + pxB * 4, 4);
        memcpy(bgB, bB + 5 * rB + 5 * 4, 4);
        IOSurfaceUnlock(sfB, kIOSurfaceLockReadOnly, NULL);
        CFRelease(sfB);
        if (memcmp(cell, magentaB, 4))
          FAIL("blob rotation: frame %d (slot %d, reuse #%d) cell "
               "%02x%02x%02x%02x not magenta — the draw did not read this "
               "frame's client-written blob slice",
               n, slot, n / 4, cell[0], cell[1], cell[2], cell[3]);
        if (memcmp(bgB, dkblueB, 4))
          FAIL("blob rotation: frame %d background wrong — stale slice "
               "painted outside its cell", n);
      }
      const uint64_t uplD =
          atomic_load_explicit(&status->uploads_executed, memory_order_relaxed) - upl0;
      if (uplD != 0)
        FAIL("blob rotation: %llu upload records executed (want 0 — blob "
             "content must never ride the wire)", (unsigned long long)uplD);
      printf("[test_feed] blob rotation: 12 frames, 4-slice rotation (every "
             "offset reused 3x across completion), per-frame pixel-asserted, "
             "0 upload records\n");
    }

    /* --- P1 BLOB GATE B: the V15 flood, blob edition. 500 draws, each
     * binding a FRESH 256 B blob CB slice written client-side. Old law
     * (v15): passes O(scene) AND uploads executed. New law: passes stay
     * O(scene) AND upl records ~0 — the upload class is GONE for blob
     * buffers, not merely absorbed. --- */
    {
      uint8_t magentaF[4] = {255, 0, 255, 255}, dkblueF[4] = {40, 16, 16, 255};
      const uint64_t fcbBlobOff = (uint64_t)HEAP_CAP + 2 * D9MT_BLOB_ALIGN;
      struct d9mt_wire_rec_create_buffer cFb;
      memset(&cFb, 0, sizeof(cFb));
      cFb.hdr.opcode = d9mt_wire_op_create_buffer;
      cFb.hdr.size = sizeof(cFb);
      cFb.wire_id = 992;
      cFb.client_handle = 0xC992;
      cFb.size = 131072; /* 500 x 256 B slices, one generation */
      cFb.heap_offset = fcbBlobOff;
      emit(&cFb, sizeof(cFb));
      emit_bind_shader(0, 0xA00B);
      emit_bind_shader(1, 0xA006);
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vlBF = {{{d9mt_wire_op_set_vertex_layout, sizeof(vlBF), 0}, 1, 1},
                {0, 0, 900, 0},
                {0, 12, 0, 0}};
      emit(&vlBF, sizeof(vlBF));
      struct d9mt_wire_rec_bind_vertex_buffer bvbF2;
      memset(&bvbF2, 0, sizeof(bvbF2));
      bvbF2.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
      bvbF2.hdr.size = sizeof(bvbF2);
      bvbF2.binding = 0;
      bvbF2.stride = 12;
      bvbF2.buf.buffer = 0xC950; /* legacy static VB (identity-space tri) */
      bvbF2.buf.length = 36;
      emit(&bvbF2, sizeof(bvbF2));
      struct d9mt_wire_rec_bind_index_buffer bibF2;
      memset(&bibF2, 0, sizeof(bibF2));
      bibF2.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bibF2.hdr.size = sizeof(bibF2);
      bibF2.buf.buffer = 0xC951;
      bibF2.buf.length = 8;
      bibF2.index_type = 0;
      emit(&bibF2, sizeof(bibF2));
      struct d9mt_wire_rec_bind_uniform_buffer bubF2;
      memset(&bubF2, 0, sizeof(bubF2));
      bubF2.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
      bubF2.hdr.size = sizeof(bubF2);
      bubF2.stages = 0x1;
      bubF2.slot = 4;
      bubF2.buf.buffer = 0xC992;
      bubF2.buf.length = 131072;
      emit(&bubF2, sizeof(bubF2));

      float mOffB[16] = {0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 1, 0, 5.f, 0, 0, 1};
      float mVisB[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};
      const uint64_t enc0 =
          atomic_load_explicit(&status->encoders, memory_order_relaxed);
      const uint64_t drw0 =
          atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
      const uint64_t upl0F =
          atomic_load_explicit(&status->uploads_executed, memory_order_relaxed);
      struct d9mt_wire_rec_set_render_targets_by_id rtBF;
      memset(&rtBF, 0, sizeof(rtBF));
      rtBF.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
      rtBF.hdr.size = sizeof(rtBF);
      rtBF.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
      rtBF.color[0].format = 80;
      rtBF.color[0].width = 640;
      rtBF.color[0].height = 480;
      rtBF.color[0].sample_count = 1;
      emit(&rtBF, sizeof(rtBF));
      emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
      for (uint32_t i = 0; i < 500; i++) {
        /* fresh slice, written client-side; NOTHING emitted for content */
        memcpy(g_heap_data + fcbBlobOff + 256ull * i,
               (i == 499) ? mVisB : mOffB, 64);
        struct d9mt_wire_rec_bind_uniform_buffer_range burF2;
        memset(&burF2, 0, sizeof(burF2));
        burF2.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
        burF2.hdr.size = sizeof(burF2);
        burF2.stages = 0x1;
        burF2.slot = 4;
        burF2.offset = 256ull * i;
        burF2.length = 64;
        emit(&burF2, sizeof(burF2));
        struct {
          struct d9mt_wire_rec_draw_indexed rec;
          struct d9mt_wire_draw_indexed_args args[1];
        } dF2 = {{{d9mt_wire_op_draw_indexed, sizeof(dF2), 0}, 1, 0},
                 {{3, 1, 0, 0, 0, 0}}};
        emit(&dF2, sizeof(dF2));
      }
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 20000))
        FAIL("blob flood: frame never presented");
      const uint64_t encD =
          atomic_load_explicit(&status->encoders, memory_order_relaxed) - enc0;
      const uint64_t drwD =
          atomic_load_explicit(&status->draws_executed, memory_order_relaxed) - drw0;
      const uint64_t uplDF =
          atomic_load_explicit(&status->uploads_executed, memory_order_relaxed) - upl0F;
      if (drwD != 500)
        FAIL("blob flood: %llu/500 draws executed", (unsigned long long)drwD);
      if (uplDF != 0)
        FAIL("blob flood: %llu upload records executed (want 0 — no records "
             "at all for blob buffers)", (unsigned long long)uplDF);
      if (encD > 4)
        FAIL("blob flood: %llu render passes for 500 blob-slice draws "
             "(want O(scene) ~1)", (unsigned long long)encD);
      uint32_t sF2 =
          atomic_load_explicit(&status->present_slot, memory_order_acquire);
      IOSurfaceRef sfF2 = IOSurfaceLookup(ids[sF2]);
      if (!sfF2)
        FAIL("blob flood: IOSurfaceLookup failed");
      IOSurfaceLock(sfF2, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *bF2 = IOSurfaceGetBaseAddress(sfF2);
      size_t rF2 = IOSurfaceGetBytesPerRow(sfF2);
      uint8_t lastP[4], offP[4];
      memcpy(lastP, bF2 + 140 * rF2 + 453 * 4, 4); /* final matrix centroid */
      memcpy(offP, bF2 + 280 * rF2 + 266 * 4, 4);  /* offscreen matrices' spot */
      IOSurfaceUnlock(sfF2, kIOSurfaceLockReadOnly, NULL);
      CFRelease(sfF2);
      if (memcmp(lastP, magentaF, 4) || memcmp(offP, dkblueF, 4))
        FAIL("blob flood: last=%02x%02x%02x%02x (want magenta) "
             "off=%02x%02x%02x%02x (want clear)",
             lastP[0], lastP[1], lastP[2], lastP[3],
             offP[0], offP[1], offP[2], offP[3]);
      printf("[test_feed] blob flood: 500 fresh blob slices -> %llu render "
             "pass(es), 500/500 draws, 0 upload records, final slice on "
             "screen\n", (unsigned long long)encD);
    }

    /* --- FLOW-CONTROL GATE (b): UPLOAD STORM IN A PASS (pass preservation)
     * The GTA street failure: ~1,085 of ~1,170 passes/frame were staged-
     * upload flush closes (useFlush) — every constant-ring wrap whose old
     * readers had not GPU-completed forced a close at the consuming draw.
     * With frame credits bounding the lag, those ranges retire by
     * consumption time and prepareDraw PROMOTES the parked blit to a
     * direct memcpy instead of closing the pass.
     * Shape: frame A's draws REFERENCE 16 constant slices; frame B
     * overwrites the SAME slices while A may still be in flight (the
     * staged path), then — after A publishes, which is GPU-completion by
     * definition — draws consume them. Whether an upload staged or went
     * direct (a race the gate runs 10x to cover both), the invariant is
     * identical: passes stay O(frames) and upl_flush_use stays 0. --- */
    {
      uint8_t magenta[4] = {255, 0, 255, 255}, dkblue[4] = {40, 16, 16, 255};
      float mOff[16] = {0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 1, 0, 5.f, 0, 0, 1};
      float mVis[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};

      /* rebind the cbv-cube pipeline (belt: independent of v16 leftovers) */
      emit_bind_shader(0, 0xA00B);
      emit_bind_shader(1, 0xA006);
      struct {
        struct d9mt_wire_rec_set_vertex_layout rec;
        struct d9mt_wire_vertex_attr attr;
        struct d9mt_wire_vertex_layout lay;
      } vlP = {{{d9mt_wire_op_set_vertex_layout, sizeof(vlP), 0}, 1, 1},
               {0, 0, 900 /* RGB32Float */, 0},
               {0, 12, 0, 0}};
      emit(&vlP, sizeof(vlP));
      struct d9mt_wire_rec_bind_vertex_buffer bvbP;
      memset(&bvbP, 0, sizeof(bvbP));
      bvbP.hdr.opcode = d9mt_wire_op_bind_vertex_buffer;
      bvbP.hdr.size = sizeof(bvbP);
      bvbP.binding = 0;
      bvbP.stride = 12;
      bvbP.buf.buffer = 0xC950;
      bvbP.buf.length = 36;
      emit(&bvbP, sizeof(bvbP));
      struct d9mt_wire_rec_bind_index_buffer bibP;
      memset(&bibP, 0, sizeof(bibP));
      bibP.hdr.opcode = d9mt_wire_op_bind_index_buffer;
      bibP.hdr.size = sizeof(bibP);
      bibP.buf.buffer = 0xC951;
      bibP.buf.length = 8;
      bibP.index_type = 0;
      emit(&bibP, sizeof(bibP));
      struct d9mt_wire_rec_bind_uniform_buffer bubP;
      memset(&bubP, 0, sizeof(bubP));
      bubP.hdr.opcode = d9mt_wire_op_bind_uniform_buffer;
      bubP.hdr.size = sizeof(bubP);
      bubP.stages = 0x1;
      bubP.slot = 4;
      bubP.buf.buffer = 0xC973;
      bubP.buf.length = 131072;
      emit(&bubP, sizeof(bubP));

      struct d9mt_wire_rec_set_render_targets_by_id rtP;
      memset(&rtP, 0, sizeof(rtP));
      rtP.hdr.opcode = d9mt_wire_op_set_render_targets_by_id;
      rtP.hdr.size = sizeof(rtP);
      rtP.color[0].flags = D9MT_WIRE_RT_FLAG_BACKBUFFER;
      rtP.color[0].format = 80;
      rtP.color[0].width = 640;
      rtP.color[0].height = 480;
      rtP.color[0].sample_count = 1;

      /* setup frame: all 16 slices known-offscreen (mOff) */
      for (uint32_t i = 1; i <= 16; i++) {
        uint64_t sg = 0;
        uint64_t soff = heap_put(mOff, 64, &sg);
        struct d9mt_wire_rec_upload_buffer sub;
        memset(&sub, 0, sizeof(sub));
        sub.hdr.opcode = d9mt_wire_op_upload_buffer;
        sub.hdr.size = sizeof(sub);
        sub.wire_id = 973;
        sub.client_handle = 0xC973;
        sub.dst_offset = 256ull * i;
        sub.size = 64;
        sub.heap_offset = soff;
        sub.heap_generation = sg;
        emit(&sub, sizeof(sub));
      }
      emit(&rtP, sizeof(rtP));
      emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
      emit_present(++frame);
      if (wait_u64(&status->frames, frame, 15000))
        FAIL("flow-control storm: setup frame never presented");

      uint64_t enc0 = atomic_load_explicit(&status->encoders, memory_order_relaxed);
      uint64_t drw0 = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
      uint64_t upl0 = atomic_load_explicit(&status->uploads_executed, memory_order_relaxed);
      uint64_t flu0 = atomic_load_explicit(&status->upl_flush_use, memory_order_relaxed);
      uint64_t stg0 = atomic_load_explicit(&status->upl_staged, memory_order_relaxed);
      uint64_t pro0 = atomic_load_explicit(&status->upl_promoted, memory_order_relaxed);

      for (int it = 0; it < 10; it++) {
        /* frame A: 16 draws REFERENCE the slices (no uploads) */
        emit(&rtP, sizeof(rtP));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        for (uint32_t i = 1; i <= 16; i++) {
          struct d9mt_wire_rec_bind_uniform_buffer_range bur;
          memset(&bur, 0, sizeof(bur));
          bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bur.hdr.size = sizeof(bur);
          bur.stages = 0x1;
          bur.slot = 4;
          bur.offset = 256ull * i;
          bur.length = 64;
          emit(&bur, sizeof(bur));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dA = {{{d9mt_wire_op_draw_indexed, sizeof(dA), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dA, sizeof(dA));
        }
        emit_present(++frame); /* NO wait: A's completion races B's uploads */
        uint64_t frameA = frame;

        /* frame B: overwrite the SAME slices now (refs from A may be live
         * -> staged), wait for A's publication (== GPU completion), then
         * consume with draws — the promotion path. */
        emit(&rtP, sizeof(rtP));
        emit_clear(16.f / 255.f, 16.f / 255.f, 40.f / 255.f);
        for (uint32_t i = 1; i <= 16; i++) {
          const float *m = (it == 9 && i == 16) ? mVis : mOff;
          uint64_t mg = 0;
          uint64_t moff = heap_put(m, 64, &mg);
          struct d9mt_wire_rec_upload_buffer ub;
          memset(&ub, 0, sizeof(ub));
          ub.hdr.opcode = d9mt_wire_op_upload_buffer;
          ub.hdr.size = sizeof(ub);
          ub.wire_id = 973;
          ub.client_handle = 0xC973;
          ub.dst_offset = 256ull * i;
          ub.size = 64;
          ub.heap_offset = moff;
          ub.heap_generation = mg;
          emit(&ub, sizeof(ub));
        }
        if (wait_u64(&status->frames, frameA, 15000))
          FAIL("flow-control storm (iter %d): frame A never presented", it);
        for (uint32_t i = 1; i <= 16; i++) {
          struct d9mt_wire_rec_bind_uniform_buffer_range bur;
          memset(&bur, 0, sizeof(bur));
          bur.hdr.opcode = d9mt_wire_op_bind_uniform_buffer_range;
          bur.hdr.size = sizeof(bur);
          bur.stages = 0x1;
          bur.slot = 4;
          bur.offset = 256ull * i;
          bur.length = 64;
          emit(&bur, sizeof(bur));
          struct {
            struct d9mt_wire_rec_draw_indexed rec;
            struct d9mt_wire_draw_indexed_args args[1];
          } dB = {{{d9mt_wire_op_draw_indexed, sizeof(dB), 0}, 1, 0},
                  {{3, 1, 0, 0, 0, 0}}};
          emit(&dB, sizeof(dB));
        }
        emit_present(++frame);
        if (wait_u64(&status->frames, frame, 15000))
          FAIL("flow-control storm (iter %d): frame B never presented", it);
      }

      uint64_t encD = atomic_load_explicit(&status->encoders, memory_order_relaxed) - enc0;
      uint64_t drwD = atomic_load_explicit(&status->draws_executed, memory_order_relaxed) - drw0;
      uint64_t uplD = atomic_load_explicit(&status->uploads_executed, memory_order_relaxed) - upl0;
      uint64_t fluD = atomic_load_explicit(&status->upl_flush_use, memory_order_relaxed) - flu0;
      uint64_t stgD = atomic_load_explicit(&status->upl_staged, memory_order_relaxed) - stg0;
      uint64_t proD = atomic_load_explicit(&status->upl_promoted, memory_order_relaxed) - pro0;
      if (drwD != 320)
        FAIL("flow-control storm: %" PRIu64 "/320 draws executed", drwD);
      if (uplD != 160)
        FAIL("flow-control storm: %" PRIu64 "/160 uploads executed", uplD);
      if (fluD != 0)
        FAIL("flow-control storm: %" PRIu64 " consuming-draw pass closes "
             "(useFlush) — parked blits did not promote", fluD);
      if (encD > 24) /* 20 frames x 1 pass + slack */
        FAIL("flow-control storm: %" PRIu64 " render passes for 20 frames "
             "(want O(frames) ~20 — staged uploads shattered passes)", encD);
      uint32_t sP = atomic_load_explicit(&status->present_slot, memory_order_acquire);
      IOSurfaceRef sfP = IOSurfaceLookup(ids[sP]);
      if (!sfP)
        FAIL("flow-control storm: IOSurfaceLookup failed");
      IOSurfaceLock(sfP, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *bP = IOSurfaceGetBaseAddress(sfP);
      size_t rP = IOSurfaceGetBytesPerRow(sfP);
      uint8_t lastP[4], offP[4];
      memcpy(lastP, bP + 140 * rP + 453 * 4, 4); /* mVis centroid */
      memcpy(offP, bP + 280 * rP + 266 * 4, 4);  /* offscreen spot */
      IOSurfaceUnlock(sfP, kIOSurfaceLockReadOnly, NULL);
      CFRelease(sfP);
      if (memcmp(lastP, magenta, 4) || memcmp(offP, dkblue, 4))
        FAIL("flow-control storm: last=%02x%02x%02x%02x (want magenta) "
             "off=%02x%02x%02x%02x (want clear) — promoted bytes did not land "
             "where the consuming draw reads",
             lastP[0], lastP[1], lastP[2], lastP[3],
             offP[0], offP[1], offP[2], offP[3]);
      printf("[test_feed] flow-control storm: 10x overwrite-then-consume "
             "frames -> %" PRIu64 " passes, useFlush=0 (stg=%" PRIu64
             " promoted=%" PRIu64 "), all draws + final bytes correct\n",
             encD, stgD, proD);
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

  /* --- KERNEL-WAKE GATES (os_sync_wait_on_address, mtl_futex.h) ----------
   * (a) wake latency: the consumer parks on &ring->head; every kicked
   *     record must be dequeued with p50 < 200 us over 1k records (the old
   *     1 ms poll park was ms-class). The waker here is an x86_64 (Rosetta-
   *     translated) process and the waiter is native arm64, across a
   *     MAP_SHARED file — exactly the production client/server pair, so a
   *     PASS also proves the SHARED flag crosses the ISA boundary.
   * (b) publish notice: a present is followed by a futex wait on
   *     status->frame_serial; the executor's post-publish wake must land
   *     (arm64 waker -> x86_64 waiter, the reverse direction).
   * (c) lost wake: one record emitted with its kick suppressed must still
   *     be consumed within the consumer's 1 ms timeout backstop (plus
   *     scheduling margin) and the stream must continue. */
  {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    enum { LAT_N = 1000 };
    static uint64_t lat_ns[LAT_N];
    for (int i = 0; i < LAT_N; i++) {
      usleep(400); /* drain + park: the consumer futex-parks when empty */
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, (uint32_t)i, 0};
      const uint64_t t0 = mach_absolute_time();
      emit(&sr, sizeof(sr)); /* head release-store + kick inside */
      const uint64_t want = g_head;
      for (;;) { /* tight observation spin: adds ~ns, not the ms under test */
        if (atomic_load_explicit(&status->progress, memory_order_acquire) >=
            want)
          break;
        if ((mach_absolute_time() - t0) * tb.numer / tb.denom >
            2000000000ull)
          FAIL("wake-latency gate: record %d not consumed within 2 s", i);
      }
      lat_ns[i] = (mach_absolute_time() - t0) * tb.numer / tb.denom;
    }
    qsort(lat_ns, LAT_N, sizeof(lat_ns[0]), cmp_u64);
    const uint64_t p50 = lat_ns[LAT_N / 2], p90 = lat_ns[(LAT_N * 9) / 10],
                   mx = lat_ns[LAT_N - 1];
    printf("[test_feed] kernel-wake latency over %d records: p50=%" PRIu64
           "us p90=%" PRIu64 "us max=%" PRIu64 "us\n",
           LAT_N, p50 / 1000u, p90 / 1000u, mx / 1000u);
    if (p50 >= 200000ull)
      FAIL("kernel-wake p50 %" PRIu64 " us >= 200 us gate (poll-class — "
           "wakes not landing; check [d9mt_futex] fallback warnings)",
           p50 / 1000u);

    /* (b) publish notice: present -> futex-wait on frame_serial */
    {
      const uint64_t target =
          atomic_load_explicit(&status->frame_serial, memory_order_acquire) +
          1u;
      const uint64_t t0 = mach_absolute_time();
      emit_backbuffer_rt();
      emit_clear(0.f, 1.f, 0.f);
      emit_present(target);
      int ok = 0;
      for (;;) {
        uint64_t fs = atomic_load_explicit(&status->frame_serial,
                                           memory_order_acquire);
        if (fs >= target) {
          ok = 1;
          break;
        }
        if ((mach_absolute_time() - t0) * tb.numer / tb.denom >
            5000000000ull)
          break;
        d9mt_futex_wait(&status->frame_serial, fs,
                        100ull * 1000u * 1000u /* 100 ms backstop slice */);
      }
      if (!ok)
        FAIL("publish-notice gate: frame_serial %" PRIu64 " never published/"
             "woken", target);
      printf("[test_feed] publish-notice wake: serial %" PRIu64
             " observed %" PRIu64 " us after present emit (GPU-completion-"
             "gated, so ms-class is expected)\n",
             target,
             (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000u);
    }

    /* (c) lost-wake resilience: suppress one kick, the 1 ms backstop must
     * consume the record anyway — bounded degradation, never a hang. */
    {
      usleep(2000); /* ensure the consumer is parked (empty ring) */
      g_skip_next_kick = 1;
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, 0xBEEFu, 0};
      const uint64_t t0 = mach_absolute_time();
      emit(&sr, sizeof(sr)); /* kick swallowed by the hook */
      const uint64_t want = g_head;
      for (;;) {
        if (atomic_load_explicit(&status->progress, memory_order_acquire) >=
            want)
          break;
        if ((mach_absolute_time() - t0) * tb.numer / tb.denom >
            1000000000ull)
          FAIL("lost-wake gate: backstop did not consume the record in 1 s");
        usleep(200);
      }
      const uint64_t rec_ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
      if (rec_ns >= 50000000ull)
        FAIL("lost-wake gate: recovery %" PRIu64 " us exceeds the 50 ms bound"
             " (1 ms backstop + margin)", rec_ns / 1000u);
      /* ...and the stream continues on the wake path afterwards */
      struct d9mt_wire_rec_set_stencil_reference sr2 = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr2), 0}, 0xCAFEu, 0};
      emit(&sr2, sizeof(sr2));
      if (wait_u64(&status->progress, g_head, 1000))
        FAIL("lost-wake gate: stream stalled after the suppressed kick");
      printf("[test_feed] lost-wake backstop: recovered in %" PRIu64
             " us (1 ms park timeout), stream continued\n", rec_ns / 1000u);
    }
  }

  /* --- PRESENT-TIMELINE GATE (instrumentation proof, headless) ------------
   * Drives the present pipeline in the production LOCKSTEP loop (present ->
   * park on status->frame_serial until THIS frame publishes — the same
   * pacing model the client's acquireServerFrame implements, one serial
   * tighter: production waits for N-1, this waits for N) and decodes the
   * status v4 tl ring into the same segment line the live client prints.
   * All timestamps are mach ns in ONE process — no clock calibration and no
   * wine/PE emit path (emit= here is the raw ring write + kick, not a
   * thunk), so these numbers are the wine-free floor of the pipeline.
   * Segments per frame serial N:
   *   emit    = t1-t0  clear+marker record write (+kick)
   *   consume = t2-t1  marker cross-process latency + consumer dispatch
   *   srvGpu  = t3-t2  final-copy encode + commit + GPU + listener dispatch
   *   publish = t4-t3  publication store (~0 by construction)
   *   notice  = t5-t4  publish -> this process observed frame_serial >= N
   * Asserts: every frame publishes; tl slots match their serial (lockstep
   * cannot recycle a 16-deep ring); t2<=t3<=t4 monotonic. */
  {
    mach_timebase_info_data_t tb2;
    mach_timebase_info(&tb2);
    enum { TLN = 512, TLW = 256 };
    static uint64_t tlseg[5][TLW];
    static const char *tlname[5] =
        {"emit", "consume", "srvGpu", "publish", "notice"};
    uint64_t tserial =
        atomic_load_explicit(&status->frame_serial, memory_order_acquire);
    uint32_t missed_tl = 0, nwin = 0;
    for (int i = 0; i < TLN; i++) {
      const uint64_t target = ++tserial;
      const uint64_t t0 = mach_absolute_time() * tb2.numer / tb2.denom;
      emit_clear(0.1f, 0.2f, 0.3f);
      emit_present(target);
      const uint64_t t1 = mach_absolute_time() * tb2.numer / tb2.denom;
      uint64_t t5 = 0;
      for (;;) {
        uint64_t fs = atomic_load_explicit(&status->frame_serial,
                                           memory_order_acquire);
        if (fs >= target) {
          t5 = mach_absolute_time() * tb2.numer / tb2.denom;
          break;
        }
        if (mach_absolute_time() * tb2.numer / tb2.denom - t0 > 5000000000ull)
          FAIL("timeline gate: frame %" PRIu64 " never published", target);
        d9mt_futex_wait(&status->frame_serial, fs,
                        100ull * 1000u * 1000u /* 100 ms backstop slice */);
      }
      const uint32_t ti = (uint32_t)(target & (D9MT_STATUS_TL_SLOTS - 1u));
      if (atomic_load_explicit(&status->tl_serial[ti],
                               memory_order_acquire) != target) {
        missed_tl++;
        continue;
      }
      const uint64_t t2 = atomic_load_explicit(&status->tl_consume_ns[ti],
                                               memory_order_relaxed);
      const uint64_t t3 = atomic_load_explicit(&status->tl_gpu_ns[ti],
                                               memory_order_relaxed);
      const uint64_t t4 = atomic_load_explicit(&status->tl_publish_ns[ti],
                                               memory_order_relaxed);
      if (t2 == 0 || t3 < t2 || t4 < t3)
        FAIL("timeline gate: non-monotonic hops t2=%" PRIu64 " t3=%" PRIu64
             " t4=%" PRIu64 " (frame %" PRIu64 ")", t2, t3, t4, target);
      const uint32_t w = (uint32_t)(i % TLW);
      tlseg[0][w] = (t1 - t0) / 1000u;
      tlseg[1][w] = t2 > t1 ? (t2 - t1) / 1000u : 0u;
      tlseg[2][w] = (t3 - t2) / 1000u;
      tlseg[3][w] = (t4 - t3) / 1000u;
      tlseg[4][w] = t5 > t4 ? (t5 - t4) / 1000u : 0u;
      if (w == TLW - 1u) { /* one decoded line per 256 frames, live-format */
        char line[512];
        size_t off = 0;
        for (int s = 0; s < 5; s++) {
          uint64_t tmp[TLW], sum = 0;
          memcpy(tmp, tlseg[s], sizeof(tmp));
          qsort(tmp, TLW, sizeof(tmp[0]), cmp_u64);
          for (int k = 0; k < TLW; k++)
            sum += tmp[k];
          off += (size_t)snprintf(line + off, sizeof(line) - off,
                                  " %s=%" PRIu64 "/%" PRIu64, tlname[s],
                                  sum / TLW, tmp[(TLW * 95) / 100]);
        }
        printf("[test_feed] timeline win=%d us(avg/p95):%s\n", TLW, line);
        nwin++;
      }
    }
    if (missed_tl)
      FAIL("timeline gate: %u/%d tl slots mismatched their serial (lockstep "
           "cannot recycle a 16-deep ring — write protocol broken)",
           missed_tl, TLN);
    if (nwin != TLN / TLW)
      FAIL("timeline gate: %u windows printed, want %d", nwin, TLN / TLW);
    printf("[test_feed] present-timeline gate: %d lockstep presents, all tl "
           "slots matched, hops monotonic\n", TLN);
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

  /* --- V19 liveness gates: the mutual-starvation wedge shape. The
   * heartbeat is WATCHDOG-owned (liveness is a property of the process,
   * not of work arriving), so (a) an idle server heartbeats forever and
   * the client's dead-server detector can never trip on silence, and
   * (b) a load-then-lull-then-load sequence — the exact wedge trigger —
   * flows on the second storm instead of starving. --- */
  {
    char ldir[900];
    snprintf(ldir, sizeof(ldir), "%s/d9mt-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(ldir))
      FAIL("liveness mkdtemp: %s", strerror(errno));
    void *r4, *s4, *h4;
    if (create_file(ldir, "ring", sizeof(struct d9mt_wire_ring) + RING_CAP, &r4) ||
        create_file(ldir, "status", sizeof(struct d9mt_status), &s4) ||
        create_file(ldir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + (1u << 20), &h4))
      return 1;
    struct d9mt_wire_ring *lr = r4;
    lr->capacity = RING_CAP;
    lr->data_offset = sizeof(struct d9mt_wire_ring);
    lr->magic = D9MT_WIRE_RING_MAGIC;
    struct d9mt_wire_heap *lh = h4;
    lh->capacity = 1u << 20;
    lh->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
    lh->magic = D9MT_WIRE_HEAP_MAGIC;
    struct d9mt_status *ls = s4;

    /* rebase the producer globals onto this scenario's ring */
    g_ring = lr;
    g_data = (uint8_t *)r4 + sizeof(struct d9mt_wire_ring);
    g_head = 0;

    pid_t lpid;
    char *largv[] = {(char *)server_bin, ldir, "64", "64", NULL};
    if (posix_spawn(&lpid, server_bin, NULL, NULL, largv, environ))
      FAIL("liveness posix_spawn failed");
    for (int i = 0;; i++) {
      if (atomic_load_explicit(&ls->epoch, memory_order_acquire) == 1)
        break;
      int lst;
      if (waitpid(lpid, &lst, WNOHANG) == lpid)
        FAIL("liveness server died before ready (0x%x)", lst);
      if (i >= 1000)
        FAIL("liveness server not ready");
      usleep(10000);
    }

    /* (a) idle-liveness: attach, send NOTHING for 10 s. The heartbeat must
     * advance EVERY second (the client detector needs a 3 s freeze to even
     * consider death, so per-second advance proves it cannot trip), and
     * the V19 dead predicate's other half (non-empty ring unconsumed) must
     * stay false — the ring is empty the whole time. */
    for (int sec = 0; sec < 10; sec++) {
      uint64_t h0 = atomic_load_explicit(&ls->heartbeat, memory_order_acquire);
      usleep(1000000);
      uint64_t h1 = atomic_load_explicit(&ls->heartbeat, memory_order_acquire);
      if (h1 == h0)
        FAIL("idle-liveness: heartbeat FROZEN during idle second %d "
             "(hb=%" PRIu64 ") — idle server would be misdeclared dead", sec, h0);
      uint64_t ltail = atomic_load_explicit(&lr->tail, memory_order_acquire);
      uint64_t lhead = atomic_load_explicit(&lr->head, memory_order_acquire);
      if (ltail != lhead)
        FAIL("idle-liveness: ring not empty while idle (tail=%" PRIu64
             " head=%" PRIu64 ")", ltail, lhead);
    }
    printf("[test_feed] idle-liveness: 10 s of silence — heartbeat advanced "
           "every second, dead-server predicate never true\n");

    /* (b) load-lull-load: storm, 5 s of silence (the old wedge's kill
     * window: consumer idles, the consumer-owned heartbeat froze, client
     * disabled the wire), storm again — the second storm MUST flow. */
    for (uint32_t i = 0; i < 2000; i++) {
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, i, 0};
      emit(&sr, sizeof(sr));
    }
    if (wait_u64(&ls->progress, g_head, 5000))
      FAIL("load-lull-load: first storm did not consume");
    uint64_t lullHb0 = atomic_load_explicit(&ls->heartbeat, memory_order_acquire);
    usleep(5000000);
    uint64_t lullHb1 = atomic_load_explicit(&ls->heartbeat, memory_order_acquire);
    if (lullHb1 - lullHb0 < 100)
      FAIL("load-lull-load: heartbeat nearly froze through the lull "
           "(+%" PRIu64 " ticks in 5 s, want >=100)", lullHb1 - lullHb0);
    for (uint32_t i = 0; i < 2000; i++) {
      struct d9mt_wire_rec_set_stencil_reference sr = {
          {d9mt_wire_op_set_stencil_reference, sizeof(sr), 0}, i, 0};
      emit(&sr, sizeof(sr));
    }
    if (wait_u64(&ls->progress, g_head, 5000))
      FAIL("load-lull-load: SECOND storm did not flow (post-lull starvation)");
    printf("[test_feed] load-lull-load: storm / 5 s silence / storm — second "
           "storm flowed, hb +%" PRIu64 " through the lull\n",
           lullHb1 - lullHb0);

    /* clean detach + reap */
    struct d9mt_wire_rec_nul ldetach = {{d9mt_wire_op_detach, 8, 0}};
    emit(&ldetach, sizeof(ldetach));
    int lst = -1, lexited = 0;
    for (int i = 0; i < 800; i++) {
      if (waitpid(lpid, &lst, WNOHANG) == lpid) {
        lexited = 1;
        break;
      }
      usleep(10000);
    }
    if (!lexited) {
      kill(lpid, SIGKILL);
      FAIL("liveness server did not exit after detach");
    }
    if (!WIFEXITED(lst) || WEXITSTATUS(lst) != 0)
      FAIL("liveness server exit status 0x%x", lst);
    char p4[1024];
    snprintf(p4, sizeof(p4), "%s/ring", ldir);
    unlink(p4);
    snprintf(p4, sizeof(p4), "%s/status", ldir);
    unlink(p4);
    snprintf(p4, sizeof(p4), "%s/heap", ldir);
    unlink(p4);
    rmdir(ldir);
  }

  /* --- FLOW-CONTROL GATE (a): SUSTAINED OVERLOAD (frame-credit pacing).
   * A producer that would run unthrottled feeds a consumer throttled to
   * 4 ms/frame (D9MT_TEST_FRAME_DELAY_US — far slower than the feeder's
   * unpaced ~µs/frame, i.e. >5x overload). The feeder mirrors the client's
   * credit rule (acquireServerFrame): before emitting marker n, park on
   * status->frame_serial until it reaches n - N (N = 2; frame_serial == 0
   * exempt — boot). Asserts:
   *   - the backlog (markers emitted - frames GPU-completed) never exceeds
   *     N once publication starts (the credit bound);
   *   - the emission loop itself is paced to the CONSUMER's rate (wall
   *     clock >= ~frames x delay — an unpaced feeder finishes in ms);
   *   - no stall: every credit park resolves well inside the liveness
   *     budget; every frame publishes (no deadlock);
   *   - frames render correctly: the final frame's clear color lands. --- */
  {
    const uint64_t kCredits = 2;
    const uint32_t kDelayUs = 4000;
    const uint64_t kFrames = 150;
    char odir[900];
    snprintf(odir, sizeof(odir), "%s/d9mt-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(odir))
      FAIL("overload mkdtemp: %s", strerror(errno));
    void *r5, *s5, *h5;
    if (create_file(odir, "ring", sizeof(struct d9mt_wire_ring) + RING_CAP, &r5) ||
        create_file(odir, "status", sizeof(struct d9mt_status), &s5) ||
        create_file(odir, "heap", D9MT_WIRE_HEAP_DATA_OFFSET + (1u << 20), &h5))
      return 1;
    struct d9mt_wire_ring *orr = r5;
    orr->capacity = RING_CAP;
    orr->data_offset = sizeof(struct d9mt_wire_ring);
    orr->magic = D9MT_WIRE_RING_MAGIC;
    struct d9mt_wire_heap *oh = h5;
    oh->capacity = 1u << 20;
    oh->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
    oh->magic = D9MT_WIRE_HEAP_MAGIC;
    struct d9mt_status *os = s5;

    /* rebase the producer globals onto this scenario's ring */
    g_ring = orr;
    g_data = (uint8_t *)r5 + sizeof(struct d9mt_wire_ring);
    g_head = 0;

    /* spawn with the throttle hook armed */
    int envc = 0;
    while (environ[envc])
      envc++;
    char **oenv = malloc(sizeof(char *) * (envc + 2));
    for (int i = 0; i < envc; i++)
      oenv[i] = environ[i];
    oenv[envc] = "D9MT_TEST_FRAME_DELAY_US=4000";
    oenv[envc + 1] = NULL;
    pid_t opid;
    char *oargv[] = {(char *)server_bin, odir, "64", "64", NULL};
    if (posix_spawn(&opid, server_bin, NULL, NULL, oargv, oenv))
      FAIL("overload posix_spawn failed");
    free(oenv);
    for (int i = 0;; i++) {
      if (atomic_load_explicit(&os->epoch, memory_order_acquire) == 1)
        break;
      int ost;
      if (waitpid(opid, &ost, WNOHANG) == opid)
        FAIL("overload server died before ready (0x%x)", ost);
      if (i >= 1000)
        FAIL("overload server not ready");
      usleep(10000);
    }

    emit_backbuffer_rt();
    /* prime the pump: the fs == 0 exemption is a BOOT window (the client
     * behaves identically — nothing is ever gated on a server that has not
     * published once). Emit frame 1 and wait for its publication so the
     * paced loop below measures pure steady state. */
    emit_clear(1.f, 0.f, 0.f);
    emit_present(1);
    if (wait_u64(&os->frames, 1, 15000))
      FAIL("overload: first frame never published (throttled server dead?)");
    struct timespec ot0, ot1;
    clock_gettime(CLOCK_MONOTONIC, &ot0);
    uint64_t maxBacklog = 0, creditParks = 0;
    for (uint64_t n = 2; n <= kFrames; n++) {
      if (n > kCredits) {
        /* the client's credit rule, mirrored (mtl_wire_context.cpp
         * acquireServerFrame): park until frame_serial >= n - N */
        const uint64_t target = n - kCredits;
        int parked = 0, waitedMs = 0;
        for (;;) {
          uint64_t fs =
              atomic_load_explicit(&os->frame_serial, memory_order_acquire);
          if (fs == 0 || fs >= target)
            break;
          if (!parked) {
            parked = 1;
            creditParks++;
          }
          if (waitedMs >= 5000)
            FAIL("overload: credit park for frame %" PRIu64 " stalled >5 s "
                 "(fs=%" PRIu64 ") — pacing wedged", n, fs);
          d9mt_futex_wait(&os->frame_serial, fs, 100ull * 1000000ull);
          waitedMs += 100; /* upper bound; a wake returns early */
        }
      }
      /* per-frame color; the FINAL frame is pure blue for the readback */
      if (n == kFrames)
        emit_clear(0.f, 0.f, 1.f);
      else
        emit_clear((n & 1) ? 1.f : 0.f, (n & 1) ? 0.f : 1.f, 0.f);
      emit_present(n);
      uint64_t fs2 =
          atomic_load_explicit(&os->frame_serial, memory_order_acquire);
      /* fs2 == 0: boot window (the rule exempts it, exactly like the
       * client) — the bound claim is for steady state */
      if (fs2 != 0 && n - fs2 > maxBacklog)
        maxBacklog = n - fs2;
    }
    clock_gettime(CLOCK_MONOTONIC, &ot1);
    uint64_t emitMs = (uint64_t)((int64_t)(ot1.tv_sec - ot0.tv_sec) * 1000ll +
                      ((int64_t)ot1.tv_nsec - (int64_t)ot0.tv_nsec) / 1000000ll);
    /* no deadlock: every frame publishes */
    if (wait_u64(&os->frames, kFrames, 30000))
      FAIL("overload: only %" PRIu64 "/%" PRIu64 " frames published",
           (uint64_t)atomic_load_explicit(&os->frames, memory_order_relaxed),
           kFrames);
    if (maxBacklog > kCredits)
      FAIL("overload: backlog reached %" PRIu64 " frames (credit bound %" PRIu64
           ") — the producer outran the completed serial", maxBacklog, kCredits);
    /* pacing: >=75%% of the throttled consumer's wall clock (unpaced
     * emission of 150 markers takes ms) */
    if (emitMs < (kFrames * kDelayUs / 1000u) * 3u / 4u)
      FAIL("overload: emission loop took %" PRIu64 " ms for %" PRIu64
           " frames x %u us consumer — pacing did not engage", emitMs,
           kFrames, kDelayUs);
    /* the final frame's pixels: publication is completion-gated, so the
     * published slot must hold the LAST clear color (pure blue) */
    {
      uint32_t oslot =
          atomic_load_explicit(&os->present_slot, memory_order_acquire);
      uint32_t oid =
          atomic_load_explicit(&os->iosurface_id[oslot], memory_order_acquire);
      IOSurfaceRef osf = IOSurfaceLookup(oid);
      if (!osf)
        FAIL("overload: IOSurfaceLookup(%u) failed", oid);
      IOSurfaceLock(osf, kIOSurfaceLockReadOnly, NULL);
      uint8_t opx[4];
      memcpy(opx, (const uint8_t *)IOSurfaceGetBaseAddress(osf) +
                      16 * IOSurfaceGetBytesPerRow(osf) + 16 * 4, 4);
      IOSurfaceUnlock(osf, kIOSurfaceLockReadOnly, NULL);
      CFRelease(osf);
      if (!(opx[0] == 255 && opx[1] == 0 && opx[2] == 0 && opx[3] == 255))
        FAIL("overload: final frame %02x%02x%02x%02x want pure blue (BGRA "
             "ff0000ff) — a paced stream must still render every frame",
             opx[0], opx[1], opx[2], opx[3]);
    }
    printf("[test_feed] overload pacing: %" PRIu64 " frames vs a %u us/frame "
           "consumer -> %" PRIu64 " ms emission (%" PRIu64 " credit parks), "
           "max backlog %" PRIu64 " (bound %" PRIu64 "), final frame correct\n",
           kFrames, kDelayUs, emitMs, creditParks, maxBacklog, kCredits);

    /* clean detach + reap */
    struct d9mt_wire_rec_nul odetach = {{d9mt_wire_op_detach, 8, 0}};
    emit(&odetach, sizeof(odetach));
    int ost = -1, oexited = 0;
    for (int i = 0; i < 800; i++) {
      if (waitpid(opid, &ost, WNOHANG) == opid) {
        oexited = 1;
        break;
      }
      usleep(10000);
    }
    if (!oexited) {
      kill(opid, SIGKILL);
      FAIL("overload server did not exit after detach");
    }
    if (!WIFEXITED(ost) || WEXITSTATUS(ost) != 0)
      FAIL("overload server exit status 0x%x", ost);
    char p5[1024];
    snprintf(p5, sizeof(p5), "%s/ring", odir);
    unlink(p5);
    snprintf(p5, sizeof(p5), "%s/status", odir);
    unlink(p5);
    snprintf(p5, sizeof(p5), "%s/heap", odir);
    unlink(p5);
    rmdir(odir);
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
