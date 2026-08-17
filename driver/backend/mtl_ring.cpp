#include "mtl_ring.h"

#include <windows.h>

#include <cstring>

namespace dxvk::backend {

UploadRing::UploadRing(obj_handle_t device, uint64_t chunkSize, MtlResidencyTracker* residency)
: m_device(device), m_chunkSize(chunkSize), m_residency(residency) {
  m_live.push_back(createChunk(m_chunkSize));
}

UploadRing::~UploadRing() {
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
  // Keep the bump target (front) live regardless of its stamp.
  for (size_t i = m_live.size(); i > 1u; i--) {
    Chunk& chunk = m_live[i - 1u];

    if (chunk.retireSerial <= completedSerial) {
      m_free.push_back(std::move(chunk));
      m_live.erase(m_live.begin() + (i - 1u));
    }
  }
}

UploadRing::Chunk UploadRing::createChunk(uint64_t minSize) {
  // CPU-visible memory must live in our address space (32-bit process); unix-side pointers are unreachable here.
  void* host = VirtualAlloc(nullptr, SIZE_T(minSize),
    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

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

  if (m_residency)
    m_residency->allocCreated(handle);
  return chunk;
}

}
