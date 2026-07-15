#include "mtl_residency.h"

#include <windows.h>

#include "../util/log/log.h"

namespace dxvk::backend {

  MtlResidencyTracker::MtlResidencyTracker(obj_handle_t device, obj_handle_t queue)
  : m_queue(queue) {
    WMTResidencySetInfo info = {};
    m_set = WmtResidencySet::adopt(MTLDevice_newResidencySet(device, &info));

    if (!m_set) {
      Logger::err("MtlResidencyTracker: newResidencySet failed");
      return;
    }

    /* requestResidency is persistent (invariant 2): once, here; every
     * later commit auto-wires newly added allocations. */
    MTLResidencySet_requestResidency(m_set.handle());

    /* Queue-level attach covers all command buffers committed to the
     * queue (invariant 1; 32-sets-per-queue limit applies). */
    MTL4CommandQueue_addResidencySet(m_queue, m_set.handle());
  }

  MtlResidencyTracker::~MtlResidencyTracker() {
    /* GPU idle by contract — every pending serial is complete. */
    serialRetired(~0ull);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_set)
      MTL4CommandQueue_removeResidencySet(m_queue, m_set.handle());
    for (obj_handle_t set : m_attachedLayerSets)
      MTL4CommandQueue_removeResidencySet(m_queue, set);
    m_attachedLayerSets.clear();
  }

  void MtlResidencyTracker::allocCreated(obj_handle_t alloc) {
    if (!alloc || !m_set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    MTLResidencySet_addAllocation(m_set.handle(), alloc);
    m_dirty = true;
    m_adds++;
  }

  void MtlResidencyTracker::allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial) {
    if (!alloc) {
      /* No Metal object to unpin — free the host backing directly. */
      if (hostMem)
        VirtualFree(hostMem, 0, MEM_RELEASE);
      return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back({ alloc, hostMem, lastUseSerial });
    m_pendingCount = m_pending.size();
  }

  void MtlResidencyTracker::beforeSubmit() {
    if (!m_set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dirty)
      return;
    MTLResidencySet_commit(m_set.handle());
    m_dirty = false;
  }

  void MtlResidencyTracker::serialRetired(uint64_t completedSerial) {
    if (!m_set)
      return;

    std::vector<PendingRemoval> due;

    { std::lock_guard<std::mutex> lock(m_mutex);
      if (m_pending.empty())
        return;

      size_t write = 0u;
      for (size_t i = 0u; i < m_pending.size(); i++) {
        if (m_pending[i].serial <= completedSerial) {
          due.push_back(m_pending[i]);
        } else {
          if (write != i)
            m_pending[write] = m_pending[i];
          write++;
        }
      }
      m_pending.resize(write);
      m_pendingCount = write;

      if (due.empty())
        return;

      /* Batched userspace removals + ONE commit (invariant 2). Every due
       * serial has completed, so committing the removals cannot evict
       * anything the GPU still references (invariant 3). */
      for (const PendingRemoval& p : due)
        MTLResidencySet_removeAllocation(m_set.handle(), p.alloc);
      MTLResidencySet_commit(m_set.handle());
      m_dirty = false;
      m_removes += due.size();
    }

    /* Final teardown outside the lock (unix round-trips). Per entry the
     * order is: set removal (above) -> object release -> host free, so
     * the Metal buffer dies before its bytesNoCopy memory. */
    for (const PendingRemoval& p : due) {
      NSObject_release(p.alloc);
      if (p.hostMem)
        VirtualFree(p.hostMem, 0, MEM_RELEASE);
    }
  }

  bool MtlResidencyTracker::attachLayer(obj_handle_t layer) {
    if (!layer)
      return false;

    /* Borrowed: the layer owns its set (invariant 4); the queue retains
     * it for the duration of the attach. Unavailable (0) until the layer
     * has a device. */
    obj_handle_t set = MetalLayer_residencySet(layer);
    if (!set)
      return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (obj_handle_t s : m_attachedLayerSets) {
      if (s == set)
        return true;
    }
    MTL4CommandQueue_addResidencySet(m_queue, set);
    m_attachedLayerSets.push_back(set);
    return true;
  }

  void MtlResidencyTracker::detachLayer(obj_handle_t layer) {
    if (!layer)
      return;

    obj_handle_t set = MetalLayer_residencySet(layer);
    if (!set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0u; i < m_attachedLayerSets.size(); i++) {
      if (m_attachedLayerSets[i] == set) {
        MTL4CommandQueue_removeResidencySet(m_queue, set);
        m_attachedLayerSets.erase(m_attachedLayerSets.begin() + i);
        return;
      }
    }
  }

}
