#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "../util/util_env.h"

namespace dxvk {

  class DxvkPageAllocator {

  public:

    constexpr static uint32_t PageBits = 16;
    constexpr static uint64_t PageSize = 1u << PageBits;

    constexpr static uint32_t ChunkPageBits = 12u;
    constexpr static uint32_t ChunkPageMask = (1u << ChunkPageBits) - 1u;

    constexpr static uint32_t ChunkAddressBits = ChunkPageBits + PageBits;
    constexpr static uint64_t ChunkAddressMask = (1u << ChunkAddressBits) - 1u;

    constexpr static uint64_t MaxChunkSize = 1u << ChunkAddressBits;


    DxvkPageAllocator();

    ~DxvkPageAllocator();

    uint32_t chunkCount() const {
      return uint32_t(m_chunks.size());
    }

    uint32_t pageCount(uint32_t chunkIndex) const {
      return m_chunks.at(chunkIndex).pageCount;
    }

    uint32_t pagesUsed(uint32_t chunkIndex) const {
      return m_chunks.at(chunkIndex).pagesUsed;
    }

    bool chunkIsAvailable(uint32_t chunkIndex) const {
      return !m_chunks.at(chunkIndex).disabled;
    }

    int64_t alloc(uint64_t size, uint64_t alignment);

    int32_t allocPages(uint32_t count, uint32_t alignment);

    bool free(uint64_t address, uint64_t size);

    bool freePages(uint32_t index, uint32_t count);

    uint32_t addChunk(uint64_t size);

    void removeChunk(uint32_t chunkIndex);

    void killChunk(uint32_t chunkIndex);

    void reviveChunk(uint32_t chunkIndex);

    uint32_t reviveChunks();

    void getPageAllocationMask(uint32_t chunkIndex, uint32_t* pageMask) const;

  private:

    struct ChunkInfo {
      uint32_t  pageCount = 0u;
      uint32_t  pagesUsed = 0u;
      int32_t   nextChunk = -1;
      bool      disabled  = false;
    };

    struct PageRange {
      uint32_t  index = 0u;
      uint32_t  count = 0u;
    };

    std::vector<PageRange>  m_freeList;
    std::vector<int32_t>    m_freeListLutByPage;

    std::vector<ChunkInfo>  m_chunks;
    int32_t                 m_freeChunk = -1;

    int32_t searchFreeList(uint32_t count);

    void addLutEntry(const PageRange& range, int32_t index);

    void removeLutEntry(const PageRange& range);

    void insertFreeRange(PageRange newRange, int32_t currentIndex);

  };


  class DxvkPoolAllocator {
    using MaskType = std::conditional_t<env::is32BitHostPlatform(), uint32_t, uint64_t>;

    constexpr static uint32_t MaskBits = sizeof(MaskType) * 8u;

    constexpr static uint32_t MaxCapacityBits = 8u;
    constexpr static uint32_t MaxCapacity = 1u << MaxCapacityBits;

    constexpr static uint32_t MasksPerPage = MaxCapacity / MaskBits;
  public:

    constexpr static uint64_t MinSize = DxvkPageAllocator::PageSize >> MaxCapacityBits;

    constexpr static uint64_t MaxSize = DxvkPageAllocator::PageSize >> 1u;

    DxvkPoolAllocator(DxvkPageAllocator& pageAllocator);

    ~DxvkPoolAllocator();

    int64_t alloc(uint64_t size);

    bool free(uint64_t address, uint64_t size);

  private:

    struct PageList {
      int32_t   head = -1;
      int32_t   tail = -1;
    };

    struct PageInfo {
      MaskType  pool = 0u;
      int32_t   prev = -1;
      int32_t   next = -1;
    };

    struct PagePool {
      int32_t   nextPool  = -1;
      uint16_t  freeMask  = 0u;
      uint16_t  usedMask  = 0u;

      std::array<MaskType, MasksPerPage> subPools = { };
    };

    DxvkPageAllocator*      m_pageAllocator = nullptr;

    std::vector<PageInfo>   m_pageInfos;
    std::vector<PagePool>   m_pagePools;
    int32_t                 m_freePool = -1;

    std::array<PageList, MaxCapacityBits> m_pageLists = { };

    int32_t allocPage(uint32_t listIndex);

    bool freePage(uint32_t pageIndex, uint32_t listIndex);

    void addPageToList(uint32_t pageIndex, uint32_t listIndex);

    void removePageFromList(uint32_t pageIndex, uint32_t listIndex);

    uint32_t allocPagePool(uint32_t capacity);

    void freePagePool(uint32_t poolIndex);

    static uint32_t computeListIndex(uint64_t size);

    static uint32_t computePoolCapacity(uint32_t index);

    static uint64_t computeByteAddress(uint32_t page, uint32_t index, uint32_t list);

    static uint32_t computePageIndexFromByteAddress(uint64_t address);

    static uint32_t computeItemIndexFromByteAddress(uint64_t address, uint32_t list);

  };

}
