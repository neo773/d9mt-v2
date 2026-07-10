#pragma once
/*
 * Per-frame upload / argument ring.
 *
 * THE core primitive behind two production bug classes:
 *  - contract 3a (doc 01): MAP_DISCARD buffer renaming — old backing must
 *    stay alive until the GPU retires the command buffer that reads it.
 *  - argument-snapshot semantics (doc 00 §2): every draw's argument table /
 *    push data / per-draw constants get a FRESH slice, so later draws can
 *    never overwrite memory an earlier draw's encoded commands reference
 *    (the "every draw reads the last draw's constants" black-screen).
 *
 * Design: one chain of shared WMTBuffers carved by bump allocation. Retirement
 * is fence-based: the backend stamps each chunk with the submission serial of
 * the last command buffer that references it; chunks recycle only once the
 * GPU-completed serial passes that stamp (tracked via MTLSharedEvent).
 * Exhaustion grows the chain — allocation NEVER blocks the CS thread and
 * NEVER reuses a live chunk (v2's 4MB fixed arena hung the GPU; growth +
 * completion-stamp retirement is the correct shape).
 */

#include <cstdint>
#include <vector>

#include "wmt_handle.h"

namespace dxvk::backend {

struct RingSlice {
  uint64_t gpuAddress = 0u;  /* device address for argument buffers / AB slots */
  void*    cpuPtr     = nullptr;
  obj_handle_t buffer = 0u;  /* backing WMTBuffer (borrowed, for setBuffer) */
  uint64_t offset     = 0u;
};

class UploadRing {

public:

  UploadRing(obj_handle_t device, uint64_t chunkSize);

  ~UploadRing();

  /* Bump-allocates a slice. align must be a power of two. Growth on
   * exhaustion; never blocks, never recycles a chunk the GPU may read. */
  RingSlice allocate(uint64_t size, uint64_t align);

  /* Called at submission: stamps every chunk touched since the last call
   * with this submission serial. */
  void onSubmit(uint64_t submitSerial);

  /* Called when the GPU-completed serial advances: chunks whose stamp is
   * <= completedSerial return to the free list. */
  void onComplete(uint64_t completedSerial);

  uint64_t allocatedBytes() const { return m_totalBytes; }

private:

  struct Chunk {
    WmtBuffer buffer;
    void*     cpuPtr = nullptr;
    uint64_t  gpuAddress = 0u;
    uint64_t  size = 0u;
    uint64_t  used = 0u;
    uint64_t  retireSerial = 0u; /* last submission that referenced it */
  };

  obj_handle_t m_device;
  uint64_t m_chunkSize;
  uint64_t m_totalBytes = 0u;

  std::vector<Chunk> m_live;   /* [0] = current bump target */
  std::vector<Chunk> m_free;

  Chunk createChunk(uint64_t minSize);

};

} // namespace dxvk::backend
