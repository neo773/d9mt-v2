#ifndef D9MT_SERVER_STATUS_H
#define D9MT_SERVER_STATUS_H

/*
 * d9mt_status.h — POD layout of the `status` shared-memory file
 * (docs/RENDER_SERVER_SPEC.md, "Shared memory layout" #3).
 *
 * Server -> client one-way publication block. Lives at offset 0 of the
 * mmap(MAP_SHARED) `status` file inside the shm directory. The client polls
 * these counters (heartbeat staleness = server death); the server is the
 * only writer, the client the only reader — plain acquire/release atomics,
 * no locks, ever.
 *
 * ABI rules follow driver/mtl/mtl_wire.h: fixed-width fields only, explicit
 * padding, natural alignment, static_asserts on size AND offsets evaluated
 * by both compilers (this header compiles as C11 and C++17, x86_64 and
 * arm64 — the layout must be byte-identical across the ISA boundary).
 *
 * Exact layout (total 136 bytes):
 *   offset   0  u64      epoch             0 = booting, 1 = ready (release-stored
 *                                          LAST, after iosurfaces are published)
 *   offset   8  u64      heartbeat         monotonic, ~100 Hz while alive
 *   offset  16  u64      progress          consumed ring bytes (mirror of ring->tail)
 *   offset  24  u64      completed_serial  last completed submission serial
 *                                          (S1: stays 0 — no server-side execution)
 *   offset  32  u32      error             0 = ok; else enum d9mt_status_error
 *   offset  36  u32      _pad0
 *   offset  40  u32[8]   iosurface_id      IOSurfaceID per swapchain slot (0 = empty)
 *   offset  72  u64[8]   iosurface_serial  generation/frame serial per slot
 *
 * --- v2 (render-server Phase S2, ADDITIVE — first 136 bytes unchanged) ----
 *   offset 136  u32      version           2 (release-stored by the server at
 *                                          boot, before epoch; 0 = pre-S2 server)
 *   offset 140  u32      present_slot      IOSurface slot of the LAST presented
 *                                          frame (valid once frames > 0)
 *   offset 144  u64      frame_serial      client present serial of the last
 *                                          executed presentFrame record
 *   offset 152  u64      frames            presentFrame records executed
 *   offset 160  u64      encoders          MTL4 render encoders opened
 *   offset 168  u64      draws_executed    draws encoded into a pass
 *   offset 176  u64      draws_skipped     draws dropped (no PSO/shader/target)
 *   offset 184  u64      psos_built        MTL4Compiler pipeline builds
 *   offset 192  u64      exec_errors       non-fatal executor errors (never
 *                                          crashes the stream; see executor.mm)
 *
 * --- v3 (render-server Phase S3, ADDITIVE — first 200 bytes unchanged) ----
 *   offset 200  u64      draws_skipped_pending_pso
 *                                          subset of draws_skipped dropped
 *                                          ONLY because async work was still
 *                                          in flight: a PSO compile or (v2)
 *                                          a shader library build on the
 *                                          create worker
 *   offset 208  u64      psos_pending      async pipeline builds in flight
 *   offset 216  u64      uploads_executed  upload_buffer/upload_texture
 *                                          records blitted from the heap
 *   offset 224  u64      heap_consumed     mirror of d9mt_wire_heap.consumed
 *                                          (generation ack; observability)
 */

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#include <atomic>
#define D9MT_STATUS_ASSERT(x) static_assert(x, #x)
typedef std::atomic<uint64_t> d9mt_status_au64;
typedef std::atomic<uint32_t> d9mt_status_au32;
#else
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#define D9MT_STATUS_ASSERT(x) _Static_assert(x, #x)
typedef _Atomic uint64_t d9mt_status_au64;
typedef _Atomic uint32_t d9mt_status_au32;
#endif

D9MT_STATUS_ASSERT(sizeof(d9mt_status_au64) == 8);
D9MT_STATUS_ASSERT(sizeof(d9mt_status_au32) == 4);

#define D9MT_STATUS_SLOTS 8

/* error codes (status.error) */
enum d9mt_status_error {
  d9mt_status_ok = 0,
  d9mt_status_err_ring_map = 1,     /* ring file missing/short/bad header */
  d9mt_status_err_metal = 2,        /* no Metal device / texture wrap failed */
  d9mt_status_err_iosurface = 3,    /* IOSurfaceCreate failed */
  d9mt_status_err_decode = 4,       /* wire decode error (stream dropped) */
  d9mt_status_err_clear_verify = 5  /* GPU clear readback mismatch */
};

#define D9MT_STATUS_VERSION 3u

struct d9mt_status {
  d9mt_status_au64 epoch;            /*   0 */
  d9mt_status_au64 heartbeat;        /*   8 */
  d9mt_status_au64 progress;         /*  16 */
  d9mt_status_au64 completed_serial; /*  24 */
  d9mt_status_au32 error;            /*  32 */
  uint32_t _pad0;                    /*  36 */
  d9mt_status_au32 iosurface_id[D9MT_STATUS_SLOTS];     /* 40 */
  d9mt_status_au64 iosurface_serial[D9MT_STATUS_SLOTS]; /* 72 */
  /* --- v2 (Phase S2) --- */
  d9mt_status_au32 version;          /* 136 */
  d9mt_status_au32 present_slot;     /* 140 */
  d9mt_status_au64 frame_serial;     /* 144 */
  d9mt_status_au64 frames;           /* 152 */
  d9mt_status_au64 encoders;         /* 160 */
  d9mt_status_au64 draws_executed;   /* 168 */
  d9mt_status_au64 draws_skipped;    /* 176 */
  d9mt_status_au64 psos_built;       /* 184 */
  d9mt_status_au64 exec_errors;      /* 192 */
  /* --- v3 (Phase S3) --- */
  d9mt_status_au64 draws_skipped_pending_pso; /* 200 */
  d9mt_status_au64 psos_pending;              /* 208 */
  d9mt_status_au64 uploads_executed;          /* 216 */
  d9mt_status_au64 heap_consumed;             /* 224 */
};

D9MT_STATUS_ASSERT(sizeof(struct d9mt_status) == 232);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, epoch) == 0);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, heartbeat) == 8);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, progress) == 16);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, completed_serial) == 24);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, error) == 32);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, iosurface_id) == 40);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, iosurface_serial) == 72);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, version) == 136);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, present_slot) == 140);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, frame_serial) == 144);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, frames) == 152);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, encoders) == 160);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, draws_executed) == 168);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, draws_skipped) == 176);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, psos_built) == 184);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, exec_errors) == 192);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, draws_skipped_pending_pso) == 200);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, psos_pending) == 208);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, uploads_executed) == 216);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, heap_consumed) == 224);

#endif /* D9MT_SERVER_STATUS_H */
