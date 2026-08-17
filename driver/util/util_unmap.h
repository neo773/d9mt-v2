#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "../dxvk/dxvk_allocator.h"

#include "thread.h"

#if defined(_WIN32) && !defined(_WIN64)
  #define DXVK_USE_UNMAPPABLE_MEMORY
#endif

namespace dxvk {

  struct DxvkMemoryFileStats {
    uint64_t memoryAllocated = 0u;
    uint64_t memoryUsed = 0u;
    uint64_t memoryMapped = 0u;
  };

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
  class MemoryFile {

  public:

    MemoryFile() = default;

    MemoryFile(size_t size);

    MemoryFile(MemoryFile&& other);
    MemoryFile& operator = (MemoryFile&& other);

    ~MemoryFile();

    size_t granularity() const;

    void* map(size_t offset, size_t size);

    void unmap(void* ptr);

    explicit operator bool () const {
      return m_data.size != 0u;
    }

  private:

    struct Data {
      size_t size = 0u;

      #ifdef DXVK_USE_UNMAPPABLE_MEMORY
      HANDLE mapping = INVALID_HANDLE_VALUE;
      #else
      void* memory = nullptr;
      #endif
    };

    Data m_data;

    void destroy();

    void create(size_t size);

  };


  class MemoryFilePool {
    static constexpr size_t DefaultChunkSize = 16ull << 20u;
  public:

    MemoryFilePool();

    ~MemoryFilePool();

    MemoryFilePool(const MemoryFilePool&) = delete;
    MemoryFilePool& operator = (const MemoryFilePool&) = delete;

    int32_t alloc(size_t size);

    void free(int32_t allocation);

    void* map(int32_t allocation);

    void unmap(int32_t allocation);

    DxvkMemoryFileStats getStats() const {
      DxvkMemoryFileStats result = {};
      result.memoryAllocated = m_memoryAllocated.load(std::memory_order_relaxed);
      result.memoryUsed = m_memoryUsed.load(std::memory_order_relaxed);
      result.memoryMapped = m_memoryMapped.load(std::memory_order_relaxed);
      return result;
    }

  private:

    struct PageInfo {
      void*       mapPtr    = nullptr;
      size_t      mapCount  = 0u;
    };

    struct ChunkInfo {
      MemoryFile  file = {};
      size_t      size = 0u;

      std::vector<PageInfo> pages;
    };

    struct AllocInfo {
      MemoryFile  file      = {};
      uint64_t    address   = 0u;
      size_t      size      = 0u;
      void*       mapPtr    = nullptr;
      size_t      mapCount  = 0u;
    };

    dxvk::mutex m_mutex;

    DxvkPageAllocator m_pageAllocator;
    DxvkPoolAllocator m_poolAllocator;

    std::atomic<uint64_t> m_memoryAllocated = { 0u };
    std::atomic<uint64_t> m_memoryUsed = { 0u };
    std::atomic<uint64_t> m_memoryMapped = { 0u };

    std::vector<ChunkInfo>  m_chunks;
    std::vector<PageInfo>   m_pageInfo;
    std::vector<AllocInfo>  m_allocations;
    std::vector<int32_t>    m_freeList;

    void addChunk(size_t minSize);

    void freeChunk(uint32_t index);

  };


  class MemoryFileRegion {

  public:

    MemoryFileRegion() = default;

    MemoryFileRegion(MemoryFilePool& pool, size_t size)
    : m_pool      (&pool)
    , m_allocation(pool.alloc(size))
    , m_mapPtr    (nullptr) { }

    ~MemoryFileRegion() {
      free();
    }

    MemoryFileRegion(MemoryFileRegion&& other)
    : m_pool        (std::exchange(other.m_pool,        nullptr))
    , m_allocation  (std::exchange(other.m_allocation,  -1))
    , m_mapPtr      (std::exchange(other.m_mapPtr,      nullptr)) {

    }

    MemoryFileRegion& operator = (MemoryFileRegion&& other) {
      free();

      m_pool        = std::exchange(other.m_pool,        nullptr);
      m_allocation  = std::exchange(other.m_allocation,  -1);
      m_mapPtr      = std::exchange(other.m_mapPtr,      nullptr);
      return *this;
    }

    void* map() {
      if (!m_mapPtr && m_allocation >= 0)
        m_mapPtr = m_pool->map(m_allocation);

      return m_mapPtr;
    }

    void unmap() {
      if (m_mapPtr) {
        m_pool->unmap(m_allocation);
        m_mapPtr = nullptr;
      }
    }

    bool isMapped() const {
      return m_mapPtr != nullptr;
    }

    explicit operator bool () const {
      return m_allocation > 0;;
    }

  private:

    MemoryFilePool* m_pool        = nullptr;
    int32_t         m_allocation  = -1;
    void*           m_mapPtr      = nullptr;

    void free() {
      if (m_allocation >= 0) {
        if (m_mapPtr)
          m_pool->unmap(m_allocation);

        m_pool->free(m_allocation);
      }
    }

  };
#else
  class MemoryFilePool {

  public:

    DxvkMemoryFileStats getStats() const {
      return DxvkMemoryFileStats();
    }

  };

  class MemoryFileRegion {

  public:

    MemoryFileRegion() = default;

    MemoryFileRegion(MemoryFilePool& pool, size_t size)
    : m_data(std::malloc(size)) { }

    ~MemoryFileRegion() {
      if (m_data)
        std::free(m_data);
    }

    MemoryFileRegion(MemoryFileRegion&& other)
    : m_data(std::exchange(other.m_data, nullptr)) { }

    MemoryFileRegion& operator = (MemoryFileRegion&& other) {
      if (m_data)
        std::free(m_data);

      m_data = std::exchange(other.m_data, nullptr);
      return *this;
    }

    void* map() {
      return m_data;
    }

    void unmap() { }

    bool isMapped() const {
      return true;
    }

    explicit operator bool () const {
      return m_data != nullptr;
    }

  private:

    void* m_data = nullptr;

  };
#endif

}
