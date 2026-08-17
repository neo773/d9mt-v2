#pragma once

// Drawables never enter this set — pinning them would block CoreAnimation
// from recycling them and stall nextDrawable permanently. Removals defer
// until the GPU retires the referencing serial, since Metal 4 command
// buffers don't retain resources.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "wmt_handle.h"

namespace dxvk::backend {

  struct Mtl4ResidencySetTag; using WmtResidencySet = WmtRef<Mtl4ResidencySetTag>;

  class MtlResidencyTracker {

  public:

    MtlResidencyTracker(obj_handle_t device, obj_handle_t queue);

    ~MtlResidencyTracker();

    MtlResidencyTracker             (const MtlResidencyTracker&) = delete;
    MtlResidencyTracker& operator = (const MtlResidencyTracker&) = delete;

    void allocCreated(obj_handle_t alloc);

    void allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial);

    void beforeSubmit();

    void serialRetired(uint64_t completedSerial);

    bool attachLayer(obj_handle_t layer);
    void detachLayer(obj_handle_t layer);

    int64_t  netResident()     const { return int64_t(m_adds.load()) - int64_t(m_removes.load()); }
    uint64_t totalAdds()       const { return m_adds.load(); }
    uint64_t totalRemoves()    const { return m_removes.load(); }
    uint64_t pendingRemovals() const { return m_pendingCount.load(); }

  private:

    struct PendingRemoval {
      obj_handle_t alloc   = 0u;
      void*        hostMem = nullptr;
      uint64_t     serial  = 0u;
    };

    obj_handle_t    m_queue = 0u;
    WmtResidencySet m_set;

    std::mutex                  m_mutex;
    bool                        m_dirty = false;
    std::vector<PendingRemoval> m_pending;
    std::vector<obj_handle_t>   m_attachedLayerSets;

    std::atomic<uint64_t> m_adds{0u};
    std::atomic<uint64_t> m_removes{0u};
    std::atomic<uint64_t> m_pendingCount{0u};

  };

}
