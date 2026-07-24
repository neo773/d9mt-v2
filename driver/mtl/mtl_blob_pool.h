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
 * Pure logic, no OS deps beyond libc realloc/free (freelist storage — see
 * below), C11 + C++17, byte-identical behavior x86_64/arm64: the client
 * (mingw C++) and test_feed (clang C11) compile the SAME code, so the
 * premature-reuse gate in test_feed asserts the exact logic the driver
 * runs. Caller provides locking (single mutex in mtl_wire_context.cpp).
 *
 * Offsets are DATA-AREA-RELATIVE (like upload regions); the caller passes a
 * base >= the upload capacity, so a valid blob offset is never 0 and
 * create_buffer.heap_offset == 0 keeps meaning "no blob backing".
 */

#include <stdint.h>
#include <stdlib.h> /* realloc/free: growable freelist storage */

#ifdef __cplusplus
extern "C" {
#endif

/* Apple host page size: newBufferWithBytesNoCopy demands page-aligned
 * backings, and the wine-side wrap of a slice needs the same alignment —
 * one granularity serves the client wrap, the server wrap and the pool. */
#define D9MT_BLOB_ALIGN 16384ull

/* Initial freelist storage (entries). Storage DOUBLES on demand — there is
 * NO fixed cap. History of this number: two fixed caps (1024, then 8192)
 * were each "sized for the measured worst case" and each was then exceeded
 * by a live scene-transition mass retire (6.8k, then 12.6k/window street
 * retires) — overflow entries were silently DROPPED, permanently leaking
 * their region bytes for the session (the exhaustion signature: blob=
 * plateaus, blobFb= climbs forever, useFlush pass-close flood returns).
 * Entry drops are now IMPOSSIBLE by construction: retire grows the storage
 * instead of discarding, and the OOM last resort merges into an adjacent
 * entry (bytes preserved). freelist_drops stays as the never-fire tripwire. */
#define D9MT_BLOB_FREE_INITIAL 1024u

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
  uint32_t fcap;           /* freelist storage capacity (grows, never caps) */
  uint64_t drops;          /* failed allocs (region exhausted) */
  uint64_t freelist_drops; /* never-fire: OOM + isolated slice only */
  struct d9mt_blob_free* freelist; /* heap-owned, realloc-doubled */
};

static inline uint64_t d9mt_blob_round(uint64_t size) {
  return (size + D9MT_BLOB_ALIGN - 1ull) & ~(D9MT_BLOB_ALIGN - 1ull);
}

/* Init/reset. The struct must be ZERO-INITIALIZED before the first call
 * (every user is a static/global — the driver's g_blobPool, test_feed's
 * static pools); re-init keeps already-grown freelist storage (no leak,
 * no shrink) and only resets the accounting. */
static inline void d9mt_blob_init(struct d9mt_blob_pool* p, uint64_t base,
                                  uint64_t capacity) {
  p->base = base;
  p->capacity = capacity;
  p->bump = 0;
  p->nfree = 0;
  p->drops = 0;
  p->freelist_drops = 0;
}

/* Release freelist storage (tests / teardown; the driver's pool lives for
 * the process). Safe on a zero-initialized never-used pool. */
static inline void d9mt_blob_destroy(struct d9mt_blob_pool* p) {
  free(p->freelist);
  p->freelist = 0;
  p->fcap = 0;
  p->nfree = 0;
}

/* Ensure room for one more freelist entry: double the storage when full.
 * Returns 0 only on realloc failure (OOM) — the existing entries are
 * untouched in that case (realloc contract). */
static inline int d9mt_blob_free_reserve(struct d9mt_blob_pool* p) {
  if (p->nfree < p->fcap)
    return 1;
  {
    const uint32_t ncap = p->fcap ? p->fcap * 2u : D9MT_BLOB_FREE_INITIAL;
    void* n = realloc(p->freelist,
                      (size_t)ncap * sizeof(struct d9mt_blob_free));
    if (!n)
      return 0;
    p->freelist = (struct d9mt_blob_free*)n;
    p->fcap = ncap;
  }
  return 1;
}

/* Allocate a slice for `size` bytes. `completed_serial` is the server's
 * GPU-completed frame serial (the status field); a freelist entry is
 * eligible ONLY when completed_serial >= its retire stamp — THE gate the
 * premature-reuse test locks in. Returns the data-area-relative offset or
 * UINT64_MAX.
 *
 * Reuse policy, two passes over the freelist:
 *  1. exact size (the D3D9 rename shape: a logical buffer's generations all
 *     share one size) — the steady-state hit, O(n) scan, no fragmentation;
 *  2. SPLIT-FIT: the smallest covered entry with size > need gives up its
 *     front `need` bytes and keeps the remainder (same retire serial —
 *     conservative and correct). Without this, scene transitions strand the
 *     region: retired sizes stop matching the next scene's request sizes and
 *     every alloc after bump exhaustion misses despite free bytes (measured:
 *     the fallback-flood signature blobFb= climbing while blob= is flat). */
static inline int d9mt_blob_coalesce_covered(struct d9mt_blob_pool* p,
                                             uint64_t completed_serial);

static inline uint64_t d9mt_blob_alloc(struct d9mt_blob_pool* p, uint64_t size,
                                       uint64_t completed_serial) {
  const uint64_t need = d9mt_blob_round(size);
  if (need == 0 || need > p->capacity)
    return UINT64_MAX;
  for (uint32_t attempt = 0; attempt < 2u; attempt++) {
    for (uint32_t i = 0; i < p->nfree; i++) {
      if (p->freelist[i].size == need && p->freelist[i].serial <= completed_serial) {
        const uint64_t off = p->freelist[i].off;
        p->freelist[i] = p->freelist[--p->nfree]; /* swap-remove */
        return off;
      }
    }
    if (p->bump + need <= p->capacity) {
      const uint64_t off = p->base + p->bump;
      p->bump += need;
      return off;
    }
    /* bump exhausted: split the smallest covered larger entry */
    {
      uint32_t best = UINT32_MAX;
      for (uint32_t i = 0; i < p->nfree; i++) {
        if (p->freelist[i].size > need && p->freelist[i].serial <= completed_serial &&
            (best == UINT32_MAX || p->freelist[i].size < p->freelist[best].size))
          best = i;
      }
      if (best != UINT32_MAX) {
        const uint64_t off = p->freelist[best].off;
        p->freelist[best].off += need;
        p->freelist[best].size -= need;
        return off;
      }
    }
    /* last resort, once: defragment the covered population and retry */
    if (attempt != 0 || !d9mt_blob_coalesce_covered(p, completed_serial))
      break;
  }
  p->drops++;
  return UINT64_MAX; /* caller falls back to private backing + uploads */
}

/* Retire a slice. `serial` = the last frame marker that may reference it
 * (conservative: the marker of the frame being recorded at retire time);
 * 0 = provably never referenced (wire never up) -> immediately reusable.
 *
 * NO merging happens here: a fresh retire is (almost) always UNCOVERED, and
 * merging it into an older entry would drag that entry's bytes back behind
 * the max serial forever (measured in the soak gate: a rolling never-covered
 * mega-block that starves every reuse and burns the bump cursor to the cap).
 * Coalescing runs at ALLOC time among COVERED entries only
 * (d9mt_blob_coalesce_covered). A top-of-bump block also must NOT return to
 * the bump cursor here — bump allocs are ungated and the serial may be
 * uncovered (the PoC's premature-reuse class); the covered-coalesce pass
 * reclaims the frontier safely instead.
 *
 * ENTRY DROPS ARE IMPOSSIBLE BY CONSTRUCTION: the freelist storage grows
 * (doubling realloc) instead of overflowing — two successive fixed caps
 * (1024, 8192) were each exceeded by live scene-transition mass retires and
 * each leaked the dropped regions for the session. On realloc failure (OOM
 * — the only path with no storage), the slice merges into an ADJACENT
 * entry, taking the max serial: bytes are preserved, only the neighbor's
 * reuse is delayed. That IS the retire-merge hazard the soak prohibited as
 * a normal path (an uncovered fresh serial dragging older bytes behind it),
 * which is why it is the OOM-only last resort, never the steady state.
 * freelist_drops can fire only on OOM AND a slice with no neighbor on
 * either side — the never-fire tripwire (PROF blobFlDrop=). */
static inline void d9mt_blob_retire(struct d9mt_blob_pool* p, uint64_t off,
                                    uint64_t size, uint64_t serial) {
  const uint64_t need = d9mt_blob_round(size);
  if (need == 0 || off < p->base || off + need > p->base + p->capacity)
    return;
  if (!d9mt_blob_free_reserve(p)) {
    /* OOM last resort: coalesce-into-neighbor (never discard). */
    for (uint32_t i = 0; i < p->nfree; i++) {
      if (p->freelist[i].off + p->freelist[i].size == off) {
        p->freelist[i].size += need; /* neighbor below: extend upward */
        if (serial > p->freelist[i].serial)
          p->freelist[i].serial = serial;
        return;
      }
      if (off + need == p->freelist[i].off) {
        p->freelist[i].off = off; /* neighbor above: extend downward */
        p->freelist[i].size += need;
        if (serial > p->freelist[i].serial)
          p->freelist[i].serial = serial;
        return;
      }
    }
    p->freelist_drops++; /* OOM + isolated slice: unreachable in practice */
    return;
  }
  p->freelist[p->nfree].off = off;
  p->freelist[p->nfree].size = need;
  p->freelist[p->nfree].serial = serial;
  p->nfree++;
}

/* Pressure-path defragmentation: merge adjacent COVERED entries (all serials
 * <= completed, so the merged serial stays covered — never delays reuse) and
 * return covered blocks touching the bump frontier straight to the cursor
 * (safe: covered = provably reader-free). O(n^2) worst case, called ONLY
 * when exact/bump/split all missed — self-limiting (it collapses nfree). */
static inline int d9mt_blob_coalesce_covered(struct d9mt_blob_pool* p,
                                             uint64_t completed_serial) {
  int merged_any = 0;
  uint32_t i = 0;
  while (i < p->nfree) {
    if (p->freelist[i].serial > completed_serial) {
      i++;
      continue;
    }
    int merged = 0;
    for (uint32_t j = 0; j < p->nfree; j++) {
      if (j == i || p->freelist[j].serial > completed_serial)
        continue;
      if (p->freelist[i].off + p->freelist[i].size == p->freelist[j].off) {
        p->freelist[i].size += p->freelist[j].size;
        if (p->freelist[j].serial > p->freelist[i].serial)
          p->freelist[i].serial = p->freelist[j].serial;
        p->freelist[j] = p->freelist[--p->nfree];
        merged = merged_any = 1;
        break;
      }
    }
    if (!merged) {
      /* covered block ending at the bump frontier: give the bytes back */
      if (p->freelist[i].off + p->freelist[i].size == p->base + p->bump) {
        p->bump -= p->freelist[i].size;
        p->freelist[i] = p->freelist[--p->nfree];
        merged_any = 1;
        continue; /* re-examine the swapped-in entry at index i */
      }
      i++;
    }
  }
  return merged_any;
}

#ifdef __cplusplus
}
#endif

#endif /* D9MT_MTL_BLOB_POOL_H */
