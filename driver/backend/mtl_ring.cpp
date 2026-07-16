#include "mtl_ring.h"

#include <windows.h>

#include <cstring>

namespace dxvk::backend {

UploadRing::UploadRing(obj_handle_t device, uint64_t chunkSize, MtlResidencyTracker* residency)
: m_device(device), m_chunkSize(chunkSize), m_residency(residency) {
  m_live.push_back(createChunk(m_chunkSize));
}

UploadRing::~UploadRing() {
  /* Ring teardown only happens at device teardown, after the GPU drained
   * (serial 0 = already complete). The tracker owns removal, buffer
   * release and the VirtualFree of each chunk's host backing. */
  for (auto& chunk : m_live) {
    if (m_residency)
      m_residency->allocDestroyed(chunk.buffer.detach(), chunk.cpuPtr, 0u);
  }
  for (auto& chunk : m_free) {
    if (m_residency)
      m_residency->allocDestroyed(chunk.buffer.detach(), chunk.cpuPtr, 0u);
  }
}

RingSlice UploadRing::allocate(uint64_t size, uint64_t align) {
  Chunk* cur = &m_live.front();

  uint64_t offset = (cur->used + align - 1u) & ~(align - 1u);

  if (offset + size > cur->size) {
    /* Current chunk exhausted: pull a retired chunk that fits, or grow.
     * The exhausted chunk stays in m_live until onSubmit stamps it. */
    Chunk next;

    for (size_t i = 0u; i < m_free.size(); i++) {
      if (m_free[i].size >= size) {
        next = std::move(m_free[i]);
        m_free.erase(m_free.begin() + i);
        break;
      }
    }

    if (!next.buffer)
      next = createChunk(size > m_chunkSize ? size : m_chunkSize);

    next.used = 0u;
    m_live.insert(m_live.begin(), std::move(next));
    cur = &m_live.front();
    offset = 0u;
  }

  cur->used = offset + size;

  RingSlice slice;
  slice.buffer = cur->buffer.handle();
  slice.offset = offset;
  slice.cpuPtr = reinterpret_cast<char*>(cur->cpuPtr) + offset;
  slice.gpuAddress = cur->gpuAddress + offset;
  return slice;
}

void UploadRing::onSubmit(uint64_t submitSerial) {
  for (auto& chunk : m_live)
    chunk.retireSerial = submitSerial;
}

void UploadRing::onComplete(uint64_t completedSerial) {
  /* Keep the bump target (front) live regardless of its stamp */
  for (size_t i = m_live.size(); i > 1u; i--) {
    Chunk& chunk = m_live[i - 1u];

    if (chunk.retireSerial <= completedSerial) {
      m_free.push_back(std::move(chunk));
      m_live.erase(m_live.begin() + (i - 1u));
    }
  }
}

UploadRing::Chunk UploadRing::createChunk(uint64_t minSize) {
  /* 32-bit PE rule: CPU-visible memory must live in OUR address space —
   * allocate here, unix wraps it via newBufferWithBytesNoCopy (v1 pattern;
   * unix-allocated host pointers are unreachable from a 32-bit process). */
  void* host = VirtualAlloc(nullptr, SIZE_T(minSize),
    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

  /* Rosetta first-touch pre-fault (v1 fix): the first write to a
   * newBufferWithBytesNoCopy backing under Rosetta x86_32 + WoW64 is
   * pathologically slow. Fault every page now, at chunk creation, so the
   * cliff never lands mid-frame when the bump allocator wraps into a fresh
   * chunk (push/CBV/AB slices allocate here every draw). */
  if (host)
    std::memset(host, 0, SIZE_T(minSize));

  WMTBufferInfo info = {};
  info.length = minSize;
  info.options = WMTResourceOptions(
      WMTResourceStorageModeShared
    | WMTResourceCPUCacheModeWriteCombined
    | WMTResourceHazardTrackingModeUntracked);
  info.memory.set(host);

  obj_handle_t handle = MTLDevice_newBuffer(m_device, &info);

  Chunk chunk;
  chunk.buffer = WmtBuffer::adopt(handle);
  chunk.cpuPtr = info.memory.get();
  chunk.gpuAddress = info.gpu_address;
  chunk.size = minSize;
  chunk.used = 0u;
  m_totalBytes += minSize;

  /* Ring memory is read by the GPU long after encoding — resident for the
   * chunk's lifetime like any other driver-created allocation. */
  if (m_residency)
    m_residency->allocCreated(handle);
  return chunk;
}

} // namespace dxvk::backend
