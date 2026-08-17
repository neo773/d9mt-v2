#include <algorithm>
#include <utility>

#include "dxvk_allocator.h"

#include "../util/util_bit.h"
#include "../util/util_likely.h"

namespace dxvk {

  DxvkPageAllocator::DxvkPageAllocator() {

  }


  DxvkPageAllocator::~DxvkPageAllocator() {

  }


  int64_t DxvkPageAllocator::alloc(uint64_t size, uint64_t alignment) {
    uint32_t pageCount = (size + PageSize - 1u) / PageSize;
    uint32_t pageAlign = (alignment + PageSize - 1u) / PageSize;

    return std::max<int64_t>(-1, int64_t(allocPages(pageCount, pageAlign)) * int64_t(PageSize));
  }


  int32_t DxvkPageAllocator::allocPages(uint32_t count, uint32_t alignment) {
    int32_t index = searchFreeList(count);

    while (index--) {
      PageRange entry = m_freeList[index];

      uint32_t chunkIndex = entry.index >> ChunkPageBits;

      if (unlikely(m_chunks[chunkIndex].disabled))
        continue;

      if (likely(!(entry.index & (alignment - 1u)))) {
        uint32_t pageIndex = entry.index;

        entry.index += count;
        entry.count -= count;

        insertFreeRange(entry, index);

        m_chunks[chunkIndex].pagesUsed += count;
        return pageIndex;
      } else {
        uint32_t pageIndex = align(entry.index, alignment);

        if (pageIndex + count > entry.index + entry.count)
          continue;

        PageRange prevRange = { };
        prevRange.index = entry.index;
        prevRange.count = pageIndex - entry.index;

        insertFreeRange(prevRange, index);

        PageRange nextRange = { };
        nextRange.index = pageIndex + count;
        nextRange.count = entry.index + entry.count - nextRange.index;

        if (nextRange.count)
          insertFreeRange(nextRange, -1);

        m_chunks[chunkIndex].pagesUsed += count;

        return pageIndex;
      }
    }

    return -1;
  }


  bool DxvkPageAllocator::free(uint64_t address, uint64_t size) {
    uint32_t pageIndex = address / PageSize;
    uint32_t pageCount = (size + PageSize - 1u) / PageSize;

    return freePages(pageIndex, pageCount);
  }


  bool DxvkPageAllocator::freePages(uint32_t index, uint32_t count) {
    int32_t prevRange = -1;
    int32_t nextRange = -1;

    if (index & ChunkPageMask)
      prevRange = m_freeListLutByPage[index - 1];

    if ((index + count) & ChunkPageMask)
      nextRange = m_freeListLutByPage[index + count];

    if (prevRange < 0) {
      if (nextRange < 0) {
        PageRange range = { };
        range.index = index;
        range.count = count;

        insertFreeRange(range, -1);
      } else {
        PageRange range = m_freeList[nextRange];
        range.index = index;
        range.count += count;

        insertFreeRange(range, nextRange);
      }
    } else if (nextRange < 0) {
      PageRange range = m_freeList[prevRange];
      range.count += count;

      insertFreeRange(range, prevRange);
    } else {
      PageRange prev = m_freeList[prevRange];
      PageRange next = m_freeList[nextRange];

      PageRange mergedRange = { };
      mergedRange.index = prev.index;
      mergedRange.count = next.index + next.count - prev.index;

      PageRange emptyRange = { };

      insertFreeRange(emptyRange, std::max(prevRange, nextRange));
      insertFreeRange(mergedRange, std::min(prevRange, nextRange));
    }

    uint32_t chunkIndex = index >> ChunkPageBits;
    return !(m_chunks[chunkIndex].pagesUsed -= count);
  }


  uint32_t DxvkPageAllocator::addChunk(uint64_t size) {
    int32_t chunkIndex = m_freeChunk;

    if (chunkIndex < 0) {
      chunkIndex = m_chunks.size();

      m_freeListLutByPage.resize((chunkIndex + 1u) << ChunkPageBits, -1);
      m_chunks.emplace_back();
    }

    auto& chunk = m_chunks[chunkIndex];
    m_freeChunk = chunk.nextChunk;

    chunk.pageCount = size / PageSize;
    chunk.pagesUsed = 0u;
    chunk.nextChunk = -1;
    chunk.disabled = false;

    PageRange pageRange = { };
    pageRange.index = uint32_t(chunkIndex) << ChunkPageBits;
    pageRange.count = chunk.pageCount;

    insertFreeRange(pageRange, -1);

    return uint32_t(chunkIndex);
  }


  void DxvkPageAllocator::removeChunk(uint32_t chunkIndex) {
    auto& chunk = m_chunks[chunkIndex];
    chunk.pageCount = 0u;
    chunk.pagesUsed = 0u;
    chunk.nextChunk = std::exchange(m_freeChunk, int32_t(chunkIndex));
    chunk.disabled = true;

    uint32_t pageIndex = chunkIndex << ChunkPageBits;

    PageRange pageRange = { };
    pageRange.index = pageIndex;
    pageRange.count = 0;

    insertFreeRange(pageRange, m_freeListLutByPage[pageIndex]);
  }


  void DxvkPageAllocator::killChunk(uint32_t chunkIndex) {
    m_chunks[chunkIndex].disabled = true;
  }


  void DxvkPageAllocator::reviveChunk(uint32_t chunkIndex) {
    m_chunks[chunkIndex].disabled = false;
  }


  uint32_t DxvkPageAllocator::reviveChunks() {
    uint32_t count = 0u;

    for (uint32_t i = 0; i < m_chunks.size(); i++) {
      if (m_chunks[i].pageCount && m_chunks[i].disabled) {
        m_chunks[i].disabled = false;
        count += 1u;
      }
    }

    return count;
  }


  void DxvkPageAllocator::getPageAllocationMask(uint32_t chunkIndex, uint32_t* pageMask) const {
    const auto& chunk = m_chunks[chunkIndex];

    uint32_t fullCount = chunk.pageCount / 32u;
    uint32_t lastCount = chunk.pageCount % 32u;

    for (uint32_t i = 0; i < fullCount; i++)
      pageMask[i] = ~0u;

    if (lastCount)
      pageMask[fullCount] = (1u << lastCount) - 1u;

    for (PageRange range : m_freeList) {
      if ((range.index >> ChunkPageBits) != chunkIndex)
        continue;

      range.index &= ChunkPageMask;

      uint32_t index = range.index / 32u;
      uint32_t shift = range.index % 32u;

      if (shift + range.count < 32u) {
        pageMask[index] ^= ((1u << range.count) - 1u) << shift;
      } else {
        if (shift) {
          pageMask[index++] ^= ~0u << shift;
          range.count -= 32u - shift;
        }

        while (range.count >= 32u) {
          pageMask[index++] = 0u;
          range.count -= 32u;
        }

        if (range.count)
          pageMask[index++] &= ~0u << range.count;
      }
    }
  }


  int32_t DxvkPageAllocator::searchFreeList(uint32_t count) {
    if (unlikely(m_freeList.empty()))
      return 0u;

    uint32_t lo = 0u;
    uint32_t hi = m_freeList.size();

    if (count <= m_freeList.back().count)
      return int32_t(hi);

    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2u;

      if (count <= m_freeList[mid].count)
        lo = mid + 1;
      else
        hi = mid;
    }

    return int32_t(lo);
  }


  void DxvkPageAllocator::addLutEntry(const PageRange& range, int32_t index) {
    m_freeListLutByPage[range.index] = index;
    m_freeListLutByPage[range.index + range.count - 1u] = index;
  }


  void DxvkPageAllocator::removeLutEntry(const PageRange& range) {
    m_freeListLutByPage[range.index] = -1;
    m_freeListLutByPage[range.index + range.count - 1u] = -1;
  }


  void DxvkPageAllocator::insertFreeRange(PageRange newRange, int32_t currentIndex) {
    size_t count = m_freeList.size();
    size_t index = size_t(currentIndex);

    if (unlikely(currentIndex < 0)) {
      m_freeList.emplace_back();
      index = count++;
    }

    PageRange oldRange = m_freeList[index];

    if (likely(oldRange.count))
      removeLutEntry(oldRange);

    if (newRange.count < oldRange.count) {
      while (index + 1u < count) {
        PageRange next = m_freeList[index + 1u];

        if (newRange.count >= next.count)
          break;

        addLutEntry(next, index);
        m_freeList[index++] = next;
      }
    } else if (newRange.count > oldRange.count) {
      while (index) {
        PageRange prev = m_freeList[index - 1u];

        if (newRange.count <= prev.count)
          break;

        addLutEntry(prev, index);
        m_freeList[index--] = prev;
      }
    }

    if (newRange.count) {
      m_freeList[index] = newRange;
      addLutEntry(newRange, index);
    } else {
      m_freeList.pop_back();
    }
  }



  DxvkPoolAllocator::DxvkPoolAllocator(DxvkPageAllocator& pageAllocator)
  : m_pageAllocator(&pageAllocator) {

  }


  DxvkPoolAllocator::~DxvkPoolAllocator() {

  }


  int64_t DxvkPoolAllocator::alloc(uint64_t size) {
    uint32_t listIndex = computeListIndex(size);
    uint32_t poolCapacity = computePoolCapacity(listIndex);

    int32_t pageIndex = m_pageLists[listIndex].head;

    if (likely(pageIndex >= 0)) {
      uint32_t chunkIndex = uint32_t(pageIndex) >> DxvkPageAllocator::ChunkPageBits;

      if (unlikely(!m_pageAllocator->chunkIsAvailable(chunkIndex))) {
        int32_t nextIndex = pageIndex;

        do {
          removePageFromList(nextIndex, listIndex);
          addPageToList(nextIndex, listIndex);

          nextIndex = m_pageLists[listIndex].head;
          chunkIndex = uint32_t(nextIndex) >> DxvkPageAllocator::ChunkPageBits;
        } while (nextIndex != pageIndex && !m_pageAllocator->chunkIsAvailable(chunkIndex));

        pageIndex = nextIndex != pageIndex ? nextIndex : -1;
      }
    }

    if (unlikely(pageIndex < 0)) {
      if ((pageIndex = allocPage(listIndex)) < 0)
        return -1;

      PageInfo& page = m_pageInfos[pageIndex];

      if (likely(poolCapacity <= MaskBits)) {
        page.pool = (MaskType(2) << (poolCapacity - 1u)) - 2u;
      } else {
        page.pool = allocPagePool(poolCapacity);
      }

      return computeByteAddress(pageIndex, 0, listIndex);
    }

    if (likely(poolCapacity <= MaskBits)) {
      PageInfo& page = m_pageInfos[pageIndex];

      uint32_t itemIndex = bit::tzcnt(page.pool);
      page.pool &= page.pool - 1u;

      if (unlikely(!page.pool))
        removePageFromList(pageIndex, listIndex);

      return computeByteAddress(pageIndex, itemIndex, listIndex);
    } else {
      PageInfo& page = m_pageInfos[pageIndex];
      PagePool& pool = m_pagePools[page.pool];

      uint32_t maskIndex = bit::tzcnt(uint32_t(pool.freeMask));
      MaskType maskBit = MaskType(1) << maskIndex;

      pool.usedMask |= maskBit;

      MaskType& mask = pool.subPools[maskIndex];
      uint32_t itemIndex = bit::tzcnt(mask) + maskIndex * MaskBits;

      if (!(mask &= mask - 1u)) {
        pool.freeMask &= ~maskBit;

        if (unlikely(!pool.freeMask))
          removePageFromList(pageIndex, listIndex);
      }

      return computeByteAddress(pageIndex, itemIndex, listIndex);
    }
  }


  bool DxvkPoolAllocator::free(uint64_t address, uint64_t size) {
    uint32_t listIndex = computeListIndex(size);

    uint32_t pageIndex = computePageIndexFromByteAddress(address);
    uint32_t itemIndex = computeItemIndexFromByteAddress(address, listIndex);

    uint32_t poolCapacity = computePoolCapacity(listIndex);

    if (likely(poolCapacity <= MaskBits)) {
      PageInfo& page = m_pageInfos[pageIndex];

      if (unlikely(!page.pool))
        addPageToList(pageIndex, listIndex);

      page.pool |= MaskType(1) << itemIndex;

      if (unlikely(bit::tzcnt(page.pool + 1u) >= poolCapacity))
        return freePage(pageIndex, listIndex);

      return false;
    } else {
      PageInfo& page = m_pageInfos[pageIndex];
      PagePool& pool = m_pagePools[page.pool];

      if (unlikely(!pool.freeMask))
        addPageToList(pageIndex, listIndex);

      uint32_t maskIndex = itemIndex / MaskBits;
      MaskType maskBit = MaskType(1) << maskIndex;

      MaskType& mask = pool.subPools[maskIndex];
      mask |= MaskType(1) << (itemIndex % MaskBits);

      pool.freeMask |= maskBit;

      if (!(mask + 1u)) {
        pool.usedMask &= ~maskBit;

        if (unlikely(!pool.usedMask)) {
          freePagePool(page.pool);
          return freePage(pageIndex, listIndex);
        }
      }

      return false;
    }
  }


  int32_t DxvkPoolAllocator::allocPage(uint32_t listIndex) {
    int32_t pageIndex = m_pageAllocator->allocPages(1u, 1u);

    if (unlikely(pageIndex < 0))
      return -1;

    if (unlikely(uint32_t(pageIndex) >= m_pageInfos.size())) {
      uint32_t chunkCount = (pageIndex >> DxvkPageAllocator::ChunkPageBits) + 1u;
      m_pageInfos.resize(chunkCount << DxvkPageAllocator::ChunkPageBits);
    }

    addPageToList(pageIndex, listIndex);
    return pageIndex;
  }


  bool DxvkPoolAllocator::freePage(uint32_t pageIndex, uint32_t listIndex) {
    removePageFromList(pageIndex, listIndex);

    return m_pageAllocator->freePages(pageIndex, 1u);
  }


  void DxvkPoolAllocator::addPageToList(uint32_t pageIndex, uint32_t listIndex) {
    PageInfo& page = m_pageInfos[pageIndex];
    page.prev = m_pageLists[listIndex].tail;

    if (page.prev >= 0)
      m_pageInfos[page.prev].next = pageIndex;
    else
      m_pageLists[listIndex].head = pageIndex;

    m_pageLists[listIndex].tail = pageIndex;
  }


  void DxvkPoolAllocator::removePageFromList(uint32_t pageIndex, uint32_t listIndex) {
    PageInfo& page = m_pageInfos[pageIndex];

    if (page.prev >= 0)
      m_pageInfos[page.prev].next = page.next;
    else
      m_pageLists[listIndex].head = page.next;

    if (page.next >= 0)
      m_pageInfos[page.next].prev = page.prev;
    else
      m_pageLists[listIndex].tail = page.prev;

    page.prev = -1;
    page.next = -1;
  }


  uint32_t DxvkPoolAllocator::allocPagePool(uint32_t capacity) {
    PagePool* pool;
    uint32_t poolIndex;

    if (unlikely(m_freePool < 0)) {
      pool = &m_pagePools.emplace_back();
      pool->subPools.fill(MaskType(-1));

      poolIndex = uint32_t(m_pagePools.size() - 1u);
    } else {
      pool = &m_pagePools[m_freePool];
      poolIndex = std::exchange(m_freePool, pool->nextPool);
    }

    uint32_t maskCount = capacity / MaskBits;
    pool->freeMask = (1u << maskCount) - 1u;
    pool->usedMask = 1u;
    pool->subPools[0] = MaskType(-2);
    return poolIndex;
  }


  void DxvkPoolAllocator::freePagePool(uint32_t poolIndex) {
    PagePool* pool = &m_pagePools[poolIndex];
    pool->nextPool = m_freePool;

    m_freePool = poolIndex;
  }


  uint32_t DxvkPoolAllocator::computeListIndex(uint64_t size) {
    size = std::max(size, MinSize);

    return bit::lzcnt(uint32_t(size) - 1u) - (33u - DxvkPageAllocator::PageBits);
  }


  uint32_t DxvkPoolAllocator::computePoolCapacity(uint32_t index) {
    return 2u << index;
  }


  uint64_t DxvkPoolAllocator::computeByteAddress(uint32_t page, uint32_t index, uint32_t list) {
    uint32_t shift = DxvkPageAllocator::PageBits - 1u - list;
    return DxvkPageAllocator::PageSize * uint64_t(page) + (index << shift);
  }


  uint32_t DxvkPoolAllocator::computePageIndexFromByteAddress(uint64_t address) {
    return address / DxvkPageAllocator::PageSize;
  }


  uint32_t DxvkPoolAllocator::computeItemIndexFromByteAddress(uint64_t address, uint32_t list) {
    uint32_t shift = DxvkPageAllocator::PageBits - 1u - list;
    return (address & (DxvkPageAllocator::PageSize - 1u)) >> shift;
  }

}
