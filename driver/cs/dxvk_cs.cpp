#include "dxvk_cs.h"

#include "../util/util_env.h"
#include "../util/util_error.h"
#include "../util/log/log.h"

namespace dxvk {

  DxvkCsChunk::DxvkCsChunk() {

  }


  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }


  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(MtlContext* ctx) {
    auto cmd = m_head;

    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      m_commandOffset = 0;

      while (cmd != nullptr) {
        auto next = cmd->next();
        cmd->exec(ctx);
        cmd->~DxvkCsCmd();
        cmd = next;
      }

      m_head = nullptr;
      m_next = &m_head;
    } else {
      while (cmd != nullptr) {
        cmd->exec(ctx);
        cmd = cmd->next();
      }
    }
  }


  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }

    m_head = nullptr;
    m_next = &m_head;

    m_commandOffset = 0;
  }


  DxvkCsChunkPool::DxvkCsChunkPool() {

  }


  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (DxvkCsChunk* chunk : m_chunks)
      delete chunk;
  }


  DxvkCsChunk* DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<dxvk::mutex> lock(m_mutex);

      if (m_chunks.size() != 0) {
        chunk = m_chunks.back();
        m_chunks.pop_back();
      }
    }

    if (!chunk)
      chunk = new DxvkCsChunk();

    chunk->init(flags);
    return chunk;
  }


  void DxvkCsChunkPool::freeChunk(DxvkCsChunk* chunk) {
    chunk->reset();

    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_chunks.push_back(chunk);
  }


  DxvkCsThread::DxvkCsThread(
    const Rc<MtlDevice>&   device,
    const Rc<MtlContext>&  context)
  : m_device(device), m_context(context),
    m_thread([this] { threadFunc(); }) {

  }


  DxvkCsThread::~DxvkCsThread() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

    m_condOnAdd.notify_one();
    m_thread.join();
  }


  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    uint64_t seq;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      seq = ++m_queueOrdered.seqDispatch;

      auto& entry = m_queueOrdered.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = seq;

      m_condOnAdd.notify_one();
    }

    return seq;
  }


  void DxvkCsThread::injectChunk(DxvkCsQueue queue, DxvkCsChunkRef&& chunk, bool synchronize) {
    uint64_t timeline = 0u;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      auto& q = getQueue(queue);

      if (synchronize)
        timeline = ++q.seqDispatch;

      auto& entry = q.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = timeline;

      m_condOnAdd.notify_one();

      if (queue == DxvkCsQueue::HighPriority) {
        m_hasHighPrio.store(true);
      }
    }

    if (synchronize) {
      std::unique_lock<dxvk::mutex> lock(m_counterMutex);

      m_condOnSync.wait(lock, [this, queue, timeline] {
        return getCounter(queue).load() >= timeline;
      });
    }
  }


  void DxvkCsThread::synchronize(uint64_t seq) {
    if (seq > m_seqOrdered.load()) {
      if (seq == SynchronizeAll)
        seq = m_queueOrdered.seqDispatch;

      { std::unique_lock<dxvk::mutex> lock(m_counterMutex);
        m_condOnSync.wait(lock, [this, seq] {
          return m_seqOrdered.load() >= seq;
        });
      }
    }
  }


  void DxvkCsThread::threadFunc() {
    env::setThreadName("dxvk-cs");

    std::vector<DxvkCsQueuedChunk> ordered;
    std::vector<DxvkCsQueuedChunk> highPrio;

    try {
      while (!m_stopped.load()) {
        { std::unique_lock<dxvk::mutex> lock(m_mutex);

          auto pred = [this] { return
              !m_queueOrdered.queue.empty()
              || !m_queueHighPrio.queue.empty()
              || m_stopped.load();
          };

          if (unlikely(!pred())) {
            m_condOnAdd.wait(lock, [&] {
              return pred();
            });
          }

          std::swap(ordered, m_queueOrdered.queue);
          std::swap(highPrio, m_queueHighPrio.queue);

          m_hasHighPrio.store(false);
        }

        size_t orderedIndex = 0u;
        size_t highPrioIndex = 0u;

        while (highPrioIndex < highPrio.size() || orderedIndex < ordered.size()) {
          if (highPrioIndex >= highPrio.size() && m_hasHighPrio.load()) {
            highPrio.clear();
            highPrioIndex = 0u;

            std::unique_lock<dxvk::mutex> lock(m_mutex);
            std::swap(highPrio, m_queueHighPrio.queue);

            m_hasHighPrio.store(false);
          }

          bool isHighPrio = highPrioIndex < highPrio.size();
          auto& entry = isHighPrio ? highPrio[highPrioIndex++] : ordered[orderedIndex++];

          entry.chunk->executeAll(m_context.ptr());

          if (entry.seq) {
            std::lock_guard lock(m_counterMutex);

            auto& counter = isHighPrio ? m_seqHighPrio : m_seqOrdered;
            counter.store(entry.seq);

            m_condOnSync.notify_one();
          }

          entry.chunk = DxvkCsChunkRef();
        }

        ordered.clear();
        highPrio.clear();
      }
    } catch (const DxvkError& e) {
      Logger::err("Exception on CS thread!");
      Logger::err(e.message());
    }
  }

}
