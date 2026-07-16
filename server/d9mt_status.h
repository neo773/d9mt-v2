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

struct d9mt_status {
  d9mt_status_au64 epoch;            /*   0 */
  d9mt_status_au64 heartbeat;        /*   8 */
  d9mt_status_au64 progress;         /*  16 */
  d9mt_status_au64 completed_serial; /*  24 */
  d9mt_status_au32 error;            /*  32 */
  uint32_t _pad0;                    /*  36 */
  d9mt_status_au32 iosurface_id[D9MT_STATUS_SLOTS];     /* 40 */
  d9mt_status_au64 iosurface_serial[D9MT_STATUS_SLOTS]; /* 72 */
};

D9MT_STATUS_ASSERT(sizeof(struct d9mt_status) == 136);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, epoch) == 0);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, heartbeat) == 8);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, progress) == 16);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, completed_serial) == 24);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, error) == 32);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, iosurface_id) == 40);
D9MT_STATUS_ASSERT(offsetof(struct d9mt_status, iosurface_serial) == 72);

#endif /* D9MT_SERVER_STATUS_H */
