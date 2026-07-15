#pragma once
/*
 * PE-side builder for the winemetal command stream.
 *
 * Encoder commands are not calls — they are wmtcmd_* structs chained
 * through `next` pointers and handed to the unix side in ONE
 * MTL*CommandEncoder_encodeCommands crossing, which replays them into the
 * real Metal encoder natively. This is winemetal's core perf design; at
 * thousands of draws per frame the per-call thunk tax would dominate.
 *
 * Lifetime: the unix side consumes the structs synchronously during
 * encodeCommands, so chunk memory recycles immediately after the flush —
 * no GPU-lifetime tracking needed here (unlike UploadRing memory, which
 * the GPU reads later).
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include "../../bridge/winemetal.h"

namespace dxvk::backend {

class CmdChunk {
  constexpr static size_t BlockSize = 64u * 1024u;

public:

  CmdChunk() = default;

  /* Appends a command struct; returns a stable pointer to fill in.
   * T must begin with struct wmtcmd_base (type + next). */
  template<typename T>
  T* append(uint16_t cmdType) {
    void* mem = alloc(sizeof(T), alignof(T));
    T* cmd = static_cast<T*>(mem);
    std::memset(cmd, 0, sizeof(T));

    auto* base = reinterpret_cast<wmtcmd_base*>(cmd);
    base->type = cmdType;
    base->next.set(nullptr);

    if (m_tail)
      m_tail->next.set(base);
    else
      m_head = base;

    m_tail = base;
    return cmd;
  }

  const wmtcmd_base* head() const { return m_head; }

  bool empty() const { return m_head == nullptr; }

  /* Bytes parked in the arena since the last reset. Callers cap per-flush
   * memory with this — flushing at any command boundary is order-preserving
   * (the stream replays in append order across flushes). */
  size_t approxBytes() const { return m_activeBlock * BlockSize + m_blockUsed; }

  /* Recycle all memory; valid immediately after encodeCommands returns. */
  void reset() {
    m_head = nullptr;
    m_tail = nullptr;
    m_blockUsed = 0u;
    m_activeBlock = 0u;
  }

private:

  wmtcmd_base* m_head = nullptr;
  wmtcmd_base* m_tail = nullptr;

  std::vector<std::vector<uint8_t>> m_blocks;
  size_t m_activeBlock = 0u;
  size_t m_blockUsed = 0u;

  void* alloc(size_t size, size_t align) {
    size_t offset = (m_blockUsed + align - 1u) & ~(align - 1u);

    if (m_blocks.empty() || m_activeBlock >= m_blocks.size()
     || offset + size > BlockSize) {
      if (m_activeBlock + 1u >= m_blocks.size())
        m_blocks.emplace_back(BlockSize);
      if (!m_blocks.empty() && m_blockUsed != 0u)
        m_activeBlock++;
      if (m_activeBlock >= m_blocks.size())
        m_blocks.emplace_back(BlockSize);
      offset = 0u;
    }

    m_blockUsed = offset + size;
    return m_blocks[m_activeBlock].data() + offset;
  }

};

} // namespace dxvk::backend
