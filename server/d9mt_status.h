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
 *                                          LAST, after iosurfaces are published).
 *                                          S3 v5: +1 per SESSION record executed
 *                                          (quiesce/begin ack — the server is
 *                                          process-lifetime and resets its state
 *                                          between client device sessions)
 *   offset   8  u64      heartbeat         monotonic, ~100 Hz while the
 *                                          PROCESS lives. V19: bumped by the
 *                                          executor watchdog thread on a
 *                                          fixed 10 ms timer — NEVER by the
 *                                          consuming path. Liveness is a
 *                                          property of the process, not of
 *                                          work arriving: an idle or
 *                                          record-wedged (but alive) server
 *                                          keeps heartbeating
 *   offset  16  u64      progress          consumed ring bytes (mirror of ring->tail)
 *   offset  24  u64      completed_serial  last completed submission serial
 *                                          (S1: stays 0 — no server-side execution)
 *   offset  32  u32      error             0 = ok; else enum d9mt_status_error
 *   offset  36  u32      exec_opcode       S3 v6 (was _pad0): wire opcode + 1
 *                                          of the record the executor is
 *                                          INSIDE right now; 0 = between
 *                                          records. A frozen heartbeat with a
 *                                          nonzero exec_opcode names the hung
 *                                          handler from the client side
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
 *   offset 232  u64      creates_rejected  v7: create_* records rejected by
 *                                          the sanity clamps (absurd extent/
 *                                          mips/layers/format/size); the
 *                                          wire id dummy-resolves forever
 *
 * --- v4 (present-timeline instrumentation, ADDITIVE — first 240 bytes
 *     unchanged) ------------------------------------------------------------
 * Per-frame server-side hop timestamps, a 16-slot ring indexed by
 * (frame_serial & 15). All values are mach_absolute_time converted to
 * NANOSECONDS (machine-global clock — valid across processes AND across the
 * Rosetta/native ISA boundary; the PE client converts its QPC readings into
 * this domain via the WMTWireWaitTimeTap calibration, see winemetal.h).
 * Write protocol (server): consumer thread invalidates tl_serial[i]=0 and
 * stores tl_consume_ns at the frame marker; the publish site (listener block
 * or inline) stores tl_gpu_ns/tl_publish_ns, THEN tl_serial[i] (release),
 * THEN frame_serial — a reader that observes frame_serial >= S and
 * tl_serial[i] == S may read the other two fields.
 *   offset 240  u64[16]  tl_serial         frame serial the slot describes (0 = invalid)
 *   offset 368  u64[16]  tl_consume_ns     t2: executor entered handlePresent (marker consumed)
 *   offset 496  u64[16]  tl_gpu_ns         t3: frame's GPU completion observed (listener entry)
 *   offset 624  u64[16]  tl_publish_ns     t4: immediately before the frame_serial release-store
 *
 * --- v5 (frame-credit flow control, ADDITIVE — first 752 bytes unchanged) --
 *   offset 752  u64      upl_staged        uploads staged (dst range live)
 *   offset 760  u64      upl_promoted      parked blits promoted to memcpy
 *   offset 768  u64      upl_flush_use     consuming-draw pass closes (~0)
 *
 * --- v6 (upload parking P2, ADDITIVE — first 776 bytes unchanged) ---------
 *   offset 776  u64      upl_tex_parked    texture blits parked to a boundary
 *   offset 784  u64      upl_tex_late      draws that sampled a texture whose
 *                                          newest upload is still parked
 *                                          (stale-by-one-pass, benign)
 *   offset 792  u64      upl_tex_dummy     binds dummy-resolved because the
 *                                          texture's FIRST content is still
 *                                          parked (defined sampling; real
 *                                          from the next pass)
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

#define D9MT_STATUS_VERSION 6u

/* v4 timeline ring depth: power of two, deeper than any realistic
 * client/server frame lag (the present path runs lockstep depth 1). */
#define D9MT_STATUS_TL_SLOTS 16

struct d9mt_status {
  d9mt_status_au64 epoch;            /*   0 */
  d9mt_status_au64 heartbeat;        /*   8 */
  d9mt_status_au64 progress;         /*  16 */
  d9mt_status_au64 completed_serial; /*  24 */
  d9mt_status_au32 error;            /*  32 */
  d9mt_status_au32 exec_opcode;      /*  36 (v6; was _pad0) */
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
  d9mt_status_au64 creates_rejected;          /* 232 (v7) */
  /* --- v4 (present-timeline instrumentation) --- */
  d9mt_status_au64 tl_serial[D9MT_STATUS_TL_SLOTS];     /* 240 */
  d9mt_status_au64 tl_consume_ns[D9MT_STATUS_TL_SLOTS]; /* 368 */
  d9mt_status_au64 tl_gpu_ns[D9MT_STATUS_TL_SLOTS];     /* 496 */
  d9mt_status_au64 tl_publish_ns[D9MT_STATUS_TL_SLOTS]; /* 624 */
  /* --- v5 (frame-credit flow control, ADDITIVE — first 752 bytes unchanged)
   * Staged-upload discipline observability (executor.mm; the pass-
   * preservation gates assert on these):
   *   upl_staged     uploads staged into the slab (destination range live)
   *   upl_promoted   parked slab blits promoted to a direct memcpy at a
   *                  consuming draw (range retired by then — the credit-
   *                  bounded common case)
   *   upl_flush_use  consuming-draw pass closes (the old always-close
   *                  behavior, now the never-fire backstop; target ~0) */
  d9mt_status_au64 upl_staged;    /* 752 */
  d9mt_status_au64 upl_promoted;  /* 760 */
  d9mt_status_au64 upl_flush_use; /* 768 */
  /* --- v6 (upload parking P2, ADDITIVE) --- */
  d9mt_status_au64 upl_tex_parked; /* 776 */
  d9mt_status_au64 upl_tex_late;   /* 784 */
  d9mt_status_au64 upl_tex_dummy;  /* 792 */
};

D9MT_STATUS_ASSERT(sizeof(struct d9mt_status) == 800);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, epoch) == 0);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, heartbeat) == 8);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, progress) == 16);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, completed_serial) == 24);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, error) == 32);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, exec_opcode) == 36);
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
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, creates_rejected) == 232);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, tl_serial) == 240);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, tl_consume_ns) == 368);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, tl_gpu_ns) == 496);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, tl_publish_ns) == 624);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_staged) == 752);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_promoted) == 760);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_flush_use) == 768);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_tex_parked) == 776);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_tex_late) == 784);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, upl_tex_dummy) == 792);

#endif /* D9MT_SERVER_STATUS_H */
