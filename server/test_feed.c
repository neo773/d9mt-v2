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
#define HEAP_CAP (1u << 20)
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
 * never straddling the wrap; generation = free-running END cursor). */
static uint64_t
heap_put(const void *bytes, uint64_t size, uint64_t *gen) {
  uint64_t need = (size + 255u) & ~255ull;
  uint64_t start = g_heap_head;
  uint64_t idx = start & (HEAP_CAP - 1u);
  if (need > HEAP_CAP - idx)
    start += HEAP_CAP - idx;
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

    if (wait_u64(&status->psos_built, 1, 30000))
      FAIL("timeout waiting for psos_built>=1");
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 1.f);
    emit_draws(50);
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64, frame);
    uint64_t execd = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
    if (execd < 50)
      FAIL("draws_executed=%" PRIu64 " want >=50 after compile landed", execd);
    if (atomic_load_explicit(&status->frame_serial, memory_order_acquire) != frame)
      FAIL("frame_serial=%" PRIu64 " want %" PRIu64,
           (uint64_t)atomic_load_explicit(&status->frame_serial, memory_order_relaxed),
           frame);

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
    emit_bind_shader(0, 0xA003);
    emit_bind_shader(1, 0xA004);
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    emit_draws(1);
    emit_present(++frame); /* F3: warms the textured PSO */
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (F3)", frame);
    if (wait_u64(&status->psos_built, 2, 30000))
      FAIL("timeout waiting for psos_built>=2 (textured PSO)");

    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    emit_draws(1);
    emit_present(++frame); /* F4: textured quad with the PSO ready */
    if (wait_u64(&status->frames, frame, 15000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (F4)", frame);
    if (atomic_load_explicit(&status->uploads_executed, memory_order_relaxed) < 1)
      FAIL("uploads_executed=0 (heap upload never executed)");

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

    /* compiles drain: 64 new pipelines, none left in flight */
    if (wait_u64(&status->psos_built, psos0 + 64, 60000))
      FAIL("timeout: psos_built=%" PRIu64 " want %" PRIu64,
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

    /* second wave: every pair draws for real now */
    uint64_t exec0 = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
    emit_backbuffer_rt();
    emit_clear(0.f, 0.f, 0.f);
    for (uint32_t i = 0; i < 64; i++) {
      emit_bind_shader(0, 0xC000 + 2 * i);
      emit_bind_shader(1, 0xC001 + 2 * i);
      emit_draws(1);
    }
    emit_present(++frame);
    if (wait_u64(&status->frames, frame, 30000))
      FAIL("timeout waiting for frames>=%" PRIu64 " (storm wave 2)", frame);
    uint64_t exec1 = atomic_load_explicit(&status->draws_executed, memory_order_relaxed);
    if (exec1 - exec0 != 64)
      FAIL("storm wave 2: draws_executed delta %" PRIu64 " want 64",
           exec1 - exec0);
    if (atomic_load_explicit(&status->exec_errors, memory_order_relaxed))
      FAIL("exec_errors=%" PRIu64,
           (uint64_t)atomic_load_explicit(&status->exec_errors, memory_order_relaxed));
    if (atomic_load_explicit(&g_ring->stalls, memory_order_relaxed))
      FAIL("ring stalls=%" PRIu64 " (producer starved)",
           (uint64_t)atomic_load_explicit(&g_ring->stalls, memory_order_relaxed));
    printf("[test_feed] storm: %" PRIu64 " PSOs landed async, wave 2 drew all "
           "64, stalls=0\n",
           (uint64_t)atomic_load_explicit(&status->psos_built, memory_order_relaxed) - psos0);
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
