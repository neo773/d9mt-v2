#ifndef D9MT_MTL_BLOB_POOL_H
#define D9MT_MTL_BLOB_POOL_H

/*
 * mtl_blob_pool.h — P1 blob-slice allocator over the shared heap's BLOB
 * region (docs/VENUS_CONTRACT_DIFF.md, PORT MAP P1).
 *
 * One-to-one port of the Venus shmem-pool shape (vn_renderer_shmem /
 * vn_renderer_bo: {res_id, mmap_size, mmap_ptr} suballocated from a shared
 * mapping, mesa vn_renderer.h:11-29) with the PoC's rotation collapsed into
 * a bump cursor + retired-slice freelist. CPU-visible buffer backings are
 * carved FROM the client<->server shared heap file; the slice's pages ARE
 * the buffer on both sides — content never rides the command stream
 * (VK_EXT_command_serialization.txt:126-133, "pointers to mapped memory
 * object regions" are never serialized).
 *
 * THE CREDIT LAW (the PoC's own first failure, poc_blobmodel_writer.c:51-57:
 * reusing an offset before the consumer completed the frame that used it
 * reproduced the production corruption): a retired slice may be reused ONLY
 * once the server's GPU-COMPLETED frame serial (status frame_serial — the
 * completion-gated publication) covers the retire stamp. The existing
 * frame-credit mechanism (HANDOFF-FLOWCONTROL.md, N=2) bounds how far the
 * stamp can run ahead, so the freelist breathes within ~N presents.
 *
 * Pure logic, no OS deps, C11 + C++17, byte-identical behavior x86_64/arm64:
 * the client (mingw C++) and test_feed (clang C11) compile the SAME code, so
 * the premature-reuse gate in test_feed asserts the exact logic the driver
 * runs. Caller provides locking (single mutex in mtl_wire_context.cpp).
 *
 * Offsets are DATA-AREA-RELATIVE (like upload regions); the caller passes a
 * base >= the upload capacity, so a valid blob offset is never 0 and
 * create_buffer.heap_offset == 0 keeps meaning "no blob backing".
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apple host page size: newBufferWithBytesNoCopy demands page-aligned
 * backings, and the wine-side wrap of a slice needs the same alignment —
 * one granularity serves the client wrap, the server wrap and the pool. */
#define D9MT_BLOB_ALIGN 16384ull

/* Retired slices awaiting their completion serial. 1024 x 24 B; DISCARD
 * churn is bounded by the frame credits (~a few generations per logical
 * buffer), so overflow means a leak-class bug — count it, drop the entry
 * (the region is lost for the session, never corrupted). */
#define D9MT_BLOB_MAX_FREE 1024u

struct d9mt_blob_free {
  uint64_t off;
  uint64_t size;   /* D9MT_BLOB_ALIGN-rounded reservation */
  uint64_t serial; /* reusable once completed_serial >= this */
};

struct d9mt_blob_pool {
  uint64_t base;     /* data-area-relative start of the blob region */
  uint64_t capacity; /* blob region bytes */
  uint64_t bump;     /* next never-used byte (relative to base) */
  uint32_t nfree;
  uint32_t _pad;
  uint64_t drops;          /* failed allocs (region exhausted) */
  uint64_t freelist_drops; /* retires lost to a full freelist */
  struct d9mt_blob_free freelist[D9MT_BLOB_MAX_FREE];
};

static inline uint64_t d9mt_blob_round(uint64_t size) {
  return (size + D9MT_BLOB_ALIGN - 1ull) & ~(D9MT_BLOB_ALIGN - 1ull);
}

static inline void d9mt_blob_init(struct d9mt_blob_pool* p, uint64_t base,
                                  uint64_t capacity) {
  p->base = base;
  p->capacity = capacity;
  p->bump = 0;
  p->nfree = 0;
  p->drops = 0;
  p->freelist_drops = 0;
}

/* Allocate a slice for `size` bytes. `completed_serial` is the server's
 * GPU-completed frame serial (the status field); a freelist entry is
 * eligible ONLY when completed_serial >= its retire stamp — THE gate the
 * premature-reuse test locks in. Returns the data-area-relative offset or
 * UINT64_MAX. Exact-size reuse (the D3D9 rename shape: a logical buffer's
 * generations all share one size) keeps the scan trivial. */
static inline uint64_t d9mt_blob_alloc(struct d9mt_blob_pool* p, uint64_t size,
                                       uint64_t completed_serial) {
  const uint64_t need = d9mt_blob_round(size);
  if (need == 0 || need > p->capacity)
    return UINT64_MAX;
  for (uint32_t i = 0; i < p->nfree; i++) {
    if (p->freelist[i].size == need && p->freelist[i].serial <= completed_serial) {
      const uint64_t off = p->freelist[i].off;
      p->freelist[i] = p->freelist[--p->nfree]; /* swap-remove */
      return off;
    }
  }
  if (p->bump + need > p->capacity) {
    p->drops++;
    return UINT64_MAX; /* caller falls back to private backing + uploads */
  }
  const uint64_t off = p->base + p->bump;
  p->bump += need;
  return off;
}

/* Retire a slice. `serial` = the last frame marker that may reference it
 * (conservative: the marker of the frame being recorded at retire time);
 * 0 = provably never referenced (wire never up) -> immediately reusable. */
static inline void d9mt_blob_retire(struct d9mt_blob_pool* p, uint64_t off,
                                    uint64_t size, uint64_t serial) {
  const uint64_t need = d9mt_blob_round(size);
  if (need == 0 || off < p->base || off + need > p->base + p->capacity)
    return;
  if (p->nfree >= D9MT_BLOB_MAX_FREE) {
    p->freelist_drops++;
    return;
  }
  p->freelist[p->nfree].off = off;
  p->freelist[p->nfree].size = need;
  p->freelist[p->nfree].serial = serial;
  p->nfree++;
}

#ifdef __cplusplus
}
#endif

#endif /* D9MT_MTL_BLOB_POOL_H */
