#ifndef D9MT_SERVER_INTERNAL_H
#define D9MT_SERVER_INTERNAL_H

/* d9mt-serverd internal interfaces (server/ only; not part of the wire ABI). */

#include "../driver/mtl/mtl_wire.h"
#include "d9mt_status.h"

/* iosurface_pool.m: create D9MT_SERVER_SURFACES BGRA8 IOSurfaces of
 * width x height, wrap each as an MTLTexture, GPU-clear each to a distinct
 * solid color, verify the clear via CPU readback, publish IOSurfaceIDs into
 * status->iosurface_id[i] (serial=1). Returns 0 on success, else sets
 * status->error and returns nonzero. */
#define D9MT_SERVER_SURFACES 3
int d9mt_iosurface_pool_init(struct d9mt_status *status, uint32_t width,
                             uint32_t height);

/* consumer.m: the S1 drain loop. Decodes/validates/counts wire records,
 * mirrors ring->tail into status->progress, heartbeats ~100 Hz, exits on the
 * Detach opcode, ring->shutdown, or unlink of ring_path. Returns 0 on clean
 * detach/shutdown/unlink exit. */
int d9mt_consumer_run(struct d9mt_wire_ring *ring, struct d9mt_status *status,
                      const char *ring_path);

#endif /* D9MT_SERVER_INTERNAL_H */
