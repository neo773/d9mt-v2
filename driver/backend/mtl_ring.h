#pragma once

// Chunks retire by GPU-completed submission serial, not immediately after
// use — a live chunk may still be read by an in-flight command buffer.

#include <cstdint>
#include <vector>

#include "wmt_handle.h"
#include "mtl_residency.h"

namespace dxvk::backend {

struct RingSlice {
  uint64_t gpuAddress = 0u;
  void*    cpuPtr     = nullptr;
  obj_handle_t buffer = 0u;
  uint64_t offset     = 0u;
};

class UploadRing {

public:

  UploadRing(obj_handle_t device, uint64_t chunkSize, MtlResidencyTracker* residency);

  ~UploadRing();

  RingSlice allocate(uint64_t size, uint64_t align);

  void onSubmit(uint64_t submitSerial);

  void onComplete(uint64_t completedSerial);

  uint64_t allocatedBytes() const { return m_totalBytes; }

private:

  struct Chunk {
    WmtBuffer buffer;
    void*     cpuPtr = nullptr;
    uint64_t  gpuAddress = 0u;
    uint64_t  size = 0u;
    uint64_t  used = 0u;
    uint64_t  retireSerial = 0u;
  };

  obj_handle_t m_device;
  uint64_t m_chunkSize;
  MtlResidencyTracker* m_residency = nullptr;
  uint64_t m_totalBytes = 0u;

  std::vector<Chunk> m_live;
  std::vector<Chunk> m_free;

  Chunk createChunk(uint64_t minSize);

};

}
