#ifndef D9MT_SERVER_INTERNAL_H
#define D9MT_SERVER_INTERNAL_H

/* d9mt-serverd internal interfaces (server/ only; not part of the wire ABI).
 * Shared between C11 (main.m/consumer.m), ObjC and ObjC++ (executor.mm) —
 * everything here is a C ABI. */

#include "../driver/mtl/mtl_wire.h"
#include "d9mt_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iosurface_pool.m: create D9MT_SERVER_SURFACES BGRA8 IOSurfaces of
 * width x height, wrap each as an MTLTexture (render target + shader read),
 * publish IOSurfaceIDs into status->iosurface_id[i] (serial=1). S2: the GPU
 * liveness clear moved into the executor (METAL 4 ONLY invariant — the pool
 * no longer touches a command queue). Returns 0 on success, else sets
 * status->error and returns nonzero. */
#define D9MT_SERVER_SURFACES 3
int d9mt_iosurface_pool_init(struct d9mt_status *status, uint32_t width,
                             uint32_t height);

/* Recreate the pool at a new size (S2: the client's backbuffer size arrives
 * with the first flagged setRenderTargetsById; the spawn-time default may be
 * wrong). Publishes fresh IOSurfaceIDs + bumped generation serials. The
 * caller (executor) must have drained the GPU of references to the OLD
 * textures first. Returns 0 on success. */
int d9mt_iosurface_pool_resize(struct d9mt_status *status, uint32_t width,
                               uint32_t height);

uint32_t d9mt_pool_width(void);
uint32_t d9mt_pool_height(void);

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLTexture;
id<MTLDevice> d9mt_pool_device(void);
id<MTLTexture> d9mt_pool_texture(int slot);
#endif

/* executor.mm: the Phase S2/S3 record executor — a native, direct-ObjC
 * METAL 4 port of the driver backend's hot-loop state machine (state folding,
 * async PSO cache via MTL4Compiler, argument tables, pass management, draw
 * encoding, shared-heap content uploads).
 * Opaque handle; single-threaded (called only from the consumer thread) —
 * except PSO completion handlers and the frame-completion listener, which
 * only touch their own entry + status atomics.
 *   init:   MTL4 queue/compiler/event setup + the IOSurface liveness clear
 *           (sets status->error + returns nonzero on failure). heap may be
 *           NULL (no shared heap: uploads never arrive / are dropped);
 *           heap_bytes is the FULL mapped file size (header + data).
 *   record: execute one wire record (never fails the stream; problems count
 *           into status v2 exec_errors / draws_skipped).
 *   finish: close any open pass/frame, commit, bounded GPU drain. */
int d9mt_executor_init(struct d9mt_status *status, struct d9mt_wire_heap *heap,
                       uint64_t heap_bytes);
void d9mt_executor_record(const struct d9mt_wire_rec_hdr *hdr);
void d9mt_executor_finish(void);

/* consumer.m: the drain loop (S1 counting + S2 execution dispatch). Decodes/
 * validates wire records, hands each to the executor, mirrors ring->tail into
 * status->progress, heartbeats ~100 Hz, exits on the Detach opcode,
 * ring->shutdown, or unlink of ring_path. Returns 0 on clean exit. */
int d9mt_consumer_run(struct d9mt_wire_ring *ring, struct d9mt_status *status,
                      const char *ring_path);

#ifdef __cplusplus
}
#endif

#endif /* D9MT_SERVER_INTERNAL_H */
