#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "../util/thread.h"
#include "../util/util_math.h"
#include "../util/util_flags.h"

#include "../mtl/mtl_context.h"

namespace dxvk {

  constexpr static size_t DxvkCsChunkSize = 16384;

  class DxvkCsCmd {

  public:

    virtual ~DxvkCsCmd() { }

    DxvkCsCmd* next() const {
      return m_next;
    }

    DxvkCsCmd** chain() {
      return &m_next;
    }

    virtual void exec(MtlContext* ctx) = 0;

  private:

    DxvkCsCmd* m_next = nullptr;

  };


  template<typename T>
  class DxvkCsTypedCmd : public DxvkCsCmd {

  public:

    DxvkCsTypedCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    DxvkCsTypedCmd             (DxvkCsTypedCmd&&) = delete;
    DxvkCsTypedCmd& operator = (DxvkCsTypedCmd&&) = delete;

    void exec(MtlContext* ctx) {
      m_command(ctx);
    }

  private:

    T m_command;

  };


  class DxvkCsDataBlock {
    friend class DxvkCsChunk;
  public:

    size_t count() const {
      return m_structCount;
    }

    void* first() {
      return reinterpret_cast<char*>(this) + m_dataOffset;
    }

    void* at(uint32_t idx) {
      return reinterpret_cast<char*>(this) + m_dataOffset + idx * uint32_t(m_structSize);
    }

  private:

    uint32_t m_dataOffset  = 0u;
    uint16_t m_structSize  = 0u;
    uint16_t m_structCount = 0u;

  };


  template<typename T, typename M>
  class DxvkCsDataCmd : public DxvkCsCmd {

  public:

    DxvkCsDataCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    ~DxvkCsDataCmd() {
      auto data = reinterpret_cast<M*>(m_data.first());

      for (size_t i = 0; i < m_data.count(); i++)
        data[i].~M();
    }

    DxvkCsDataCmd             (DxvkCsDataCmd&&) = delete;
    DxvkCsDataCmd& operator = (DxvkCsDataCmd&&) = delete;

    void exec(MtlContext* ctx) {
      m_command(ctx, reinterpret_cast<M*>(m_data.first()), m_data.count());
    }

    DxvkCsDataBlock* data() {
      return &m_data;
    }

  private:

    alignas(std::max(alignof(T), alignof(M)))
    T               m_command;
    DxvkCsDataBlock m_data;

  };


  enum class DxvkCsChunkFlag : uint32_t {
    SingleUse,
  };

  using DxvkCsChunkFlags = Flags<DxvkCsChunkFlag>;


  class DxvkCsChunk : public RcObject {

  public:

    DxvkCsChunk();
    ~DxvkCsChunk();

    bool empty() const {
      return m_commandOffset == 0;
    }

    template<typename T>
    bool push(T& command) {
      using FuncType = DxvkCsTypedCmd<T>;
      void* ptr = alloc<FuncType>(0u);

      if (unlikely(!ptr))
        return false;

      auto next = new (ptr) FuncType(std::move(command));
      append(next);
      return true;
    }

    template<typename T>
    bool push(T&& command) {
      return push(command);
    }

    template<typename M, typename T>
    DxvkCsDataBlock* pushCmd(T& command, size_t count) {
      size_t dataSize = count * sizeof(M);

      using FuncType = DxvkCsDataCmd<T, M>;
      void* ptr = alloc<FuncType>(dataSize);

      if (unlikely(!ptr))
        return nullptr;

      auto next = new (ptr) FuncType(std::move(command));
      append(next);

      auto block = next->data();
      block->m_dataOffset = reinterpret_cast<uintptr_t>(&m_data[m_commandOffset - dataSize])
                          - reinterpret_cast<uintptr_t>(block);
      block->m_structSize = sizeof(M);
      block->m_structCount = count;
      return block;
    }

    void* pushData(DxvkCsDataBlock* block, uint32_t count) {
      uint32_t dataSize = block->m_structSize * count;

      if (unlikely(m_commandOffset + dataSize > DxvkCsChunkSize))
        return nullptr;

      void* ptr = &m_data[m_commandOffset];
      m_commandOffset += dataSize;

      block->m_structCount += count;
      return ptr;
    }

    void init(DxvkCsChunkFlags flags);

    void executeAll(MtlContext* ctx);

    void reset();

  private:

    size_t m_commandOffset = 0;

    DxvkCsCmd*  m_head = nullptr;
    DxvkCsCmd** m_next = &m_head;

    DxvkCsChunkFlags m_flags;

    alignas(64)
    char m_data[DxvkCsChunkSize];

    template<typename T>
    void* alloc(size_t extra) {
      if (alignof(T) > alignof(DxvkCsCmd))
        m_commandOffset = dxvk::align(m_commandOffset, alignof(T));

      if (unlikely(m_commandOffset + sizeof(T) + extra > DxvkCsChunkSize))
        return nullptr;

      void* result = &m_data[m_commandOffset];
      m_commandOffset += sizeof(T) + extra;
      return result;
    }

    void append(DxvkCsCmd* cmd) {
      *m_next = cmd;
      m_next = cmd->chain();
    }

  };


  class DxvkCsChunkPool {

  public:

    DxvkCsChunkPool();
    ~DxvkCsChunkPool();

    DxvkCsChunkPool             (const DxvkCsChunkPool&) = delete;
    DxvkCsChunkPool& operator = (const DxvkCsChunkPool&) = delete;

    DxvkCsChunk* allocChunk(DxvkCsChunkFlags flags);

    void freeChunk(DxvkCsChunk* chunk);

  private:

    dxvk::mutex               m_mutex;
    std::vector<DxvkCsChunk*> m_chunks;

  };


  class DxvkCsChunkRef {

  public:

    DxvkCsChunkRef() { }
    DxvkCsChunkRef(
      DxvkCsChunk*      chunk,
      DxvkCsChunkPool*  pool)
    : m_chunk (chunk),
      m_pool  (pool) {
      this->incRef();
    }

    DxvkCsChunkRef(const DxvkCsChunkRef& other)
    : m_chunk (other.m_chunk),
      m_pool  (other.m_pool) {
      this->incRef();
    }

    DxvkCsChunkRef(DxvkCsChunkRef&& other)
    : m_chunk (other.m_chunk),
      m_pool  (other.m_pool) {
      other.m_chunk = nullptr;
      other.m_pool  = nullptr;
    }

    DxvkCsChunkRef& operator = (const DxvkCsChunkRef& other) {
      other.incRef();
      this->decRef();
      this->m_chunk = other.m_chunk;
      this->m_pool  = other.m_pool;
      return *this;
    }

    DxvkCsChunkRef& operator = (DxvkCsChunkRef&& other) {
      this->decRef();
      this->m_chunk = other.m_chunk;
      this->m_pool  = other.m_pool;
      other.m_chunk = nullptr;
      other.m_pool  = nullptr;
      return *this;
    }

    ~DxvkCsChunkRef() {
      this->decRef();
    }

    DxvkCsChunk* operator -> () const {
      return m_chunk;
    }

    explicit operator bool () const {
      return m_chunk != nullptr;
    }

  private:

    DxvkCsChunk*      m_chunk = nullptr;
    DxvkCsChunkPool*  m_pool  = nullptr;

    void incRef() const {
      if (m_chunk != nullptr)
        m_chunk->incRef();
    }

    void decRef() const {
      if (m_chunk != nullptr && m_chunk->decRef() == 0)
        m_pool->freeChunk(m_chunk);
    }

  };


  enum class DxvkCsQueue : uint32_t {
    Ordered       = 0,
    HighPriority  = 1,
  };


  struct DxvkCsQueuedChunk {
    DxvkCsChunkRef  chunk;
    uint64_t        seq;
  };


  struct DxvkCsChunkQueue {
    std::vector<DxvkCsQueuedChunk> queue;
    uint64_t                       seqDispatch = 0u;
  };


  class DxvkCsThread {

  public:

    constexpr static uint64_t SynchronizeAll = ~0ull;

    DxvkCsThread(
      const Rc<MtlDevice>&   device,
      const Rc<MtlContext>&  context);
    ~DxvkCsThread();

    uint64_t dispatchChunk(DxvkCsChunkRef&& chunk);

    void injectChunk(
            DxvkCsQueue       queue,
            DxvkCsChunkRef&&  chunk,
            bool              synchronize);

    void synchronize(uint64_t seq);

    uint64_t lastSequenceNumber() const {
      return m_seqOrdered.load();
    }

  private:

    Rc<MtlDevice>              m_device;
    Rc<MtlContext>             m_context;

    alignas(CACHE_LINE_SIZE)
    dxvk::mutex                 m_counterMutex;

    std::atomic<uint64_t>       m_seqHighPrio = { 0u };
    std::atomic<uint64_t>       m_seqOrdered  = { 0u };

    std::atomic<bool>           m_stopped     = { false };
    std::atomic<bool>           m_hasHighPrio = { false };

    alignas(CACHE_LINE_SIZE)
    dxvk::mutex                 m_mutex;
    dxvk::condition_variable    m_condOnAdd;
    dxvk::condition_variable    m_condOnSync;

    DxvkCsChunkQueue            m_queueOrdered;
    DxvkCsChunkQueue            m_queueHighPrio;

    dxvk::thread                m_thread;

    auto& getQueue(DxvkCsQueue which) {
      return which == DxvkCsQueue::Ordered
        ? m_queueOrdered : m_queueHighPrio;
    }

    auto& getCounter(DxvkCsQueue which) {
      return which == DxvkCsQueue::Ordered
        ? m_seqOrdered : m_seqHighPrio;
    }

    void threadFunc();

  };

}
