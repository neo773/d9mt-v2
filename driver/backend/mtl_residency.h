#pragma once
/*
 * MtlResidencyTracker — the single owner of ALL MTLResidencySet state.
 *
 * INVARIANTS (Apple Metal 4 residency rules; SDK MTLResidencySet.h):
 *
 * 1. ONE driver-owned, device-lifetime residency set, attached to the
 *    MTL4CommandQueue at construction. Queue-level attach covers every
 *    command buffer committed to that queue, so nothing calls
 *    useResidencySet per cmdbuf. The set contains exactly the allocations
 *    the driver created that are alive OR still awaiting GPU retirement —
 *    never drawables, never borrowed handles.
 *
 * 2. addAllocation/removeAllocation mutate userspace only; commit() syncs
 *    the kernel side and is expensive. Mutations therefore batch under a
 *    dirty flag: commit runs at most once per queue submit (beforeSubmit)
 *    and once per retirement sweep (serialRetired) — and only when dirty.
 *    requestResidency is persistent: called once at set creation; later
 *    commits auto-wire newly added allocations.
 *
 * 3. Deferred destruction. A removal must not be committed until the GPU
 *    workload referencing the allocation has completed, and Metal 4
 *    command buffers do not retain resources. allocDestroyed() therefore
 *    takes ownership of the allocation's +1 handle reference (and its
 *    VirtualAlloc'd bytesNoCopy host backing, if any) and queues it
 *    against a submission serial; serialRetired() removes + commits,
 *    releases the Metal object, then frees the host backing — strictly in
 *    that order, and only once the serial has completed on the
 *    SubmissionTracker clock.
 *
 * 4. Drawables NEVER enter the driver set. Each CAMetalLayer owns a
 *    read-only residencySet that auto-tracks its drawable textures;
 *    attachLayer()/detachLayer() attach THAT set at queue level. Pinning
 *    drawable textures in a driver set blocks CoreAnimation from ever
 *    recycling them and starves nextDrawable into a permanent stall.
 *
 * 5. Residency sets are not thread safe. Every mutation here is guarded
 *    by one mutex (resource creation runs on app threads, submit and
 *    retirement on the CS thread).
 */

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

    /* Teardown contract: GPU idle (the device drains the last submitted
     * serial first). Flushes every pending removal, then detaches all
     * sets from the queue. */
    ~MtlResidencyTracker();

    MtlResidencyTracker             (const MtlResidencyTracker&) = delete;
    MtlResidencyTracker& operator = (const MtlResidencyTracker&) = delete;

    /* A driver-created allocation is now GPU-reachable: batched userspace
     * add, realized by the next beforeSubmit() commit (always before any
     * command buffer that could reference it is committed). */
    void allocCreated(obj_handle_t alloc);

    /* The last driver reference dropped. Takes ownership of the +1 handle
     * reference and of hostMem (VirtualAlloc'd bytesNoCopy backing, may be
     * null). lastUseSerial must bound every submission that could still
     * reference the allocation — the serial of the currently RECORDING
     * command buffer (SubmissionTracker::nextSerial) is always safe, since
     * any encoded reference lives in a cmdbuf with that serial or older. */
    void allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial);

    /* Realize pending adds before the queue commit (every allocation a
     * workload touches must be added AND committed before submit). */
    void beforeSubmit();

    /* Flush deferred removals whose serial has completed; batch all
     * removals into ONE commit. Call once per frame (endFrame) and on
     * drain paths (host-address-space retry). */
    void serialRetired(uint64_t completedSerial);

    /* Attach/detach a CAMetalLayer's own residency set at queue level.
     * Idempotent per layer. Returns false while the layer has no set yet
     * (its device is applied asynchronously) so the caller can retry. */
    bool attachLayer(obj_handle_t layer);
    void detachLayer(obj_handle_t layer);

    // --- diagnostics (observers only; no logic depends on them) ------------
    int64_t  netResident()     const { return int64_t(m_adds.load()) - int64_t(m_removes.load()); }
    uint64_t totalAdds()       const { return m_adds.load(); }
    uint64_t totalRemoves()    const { return m_removes.load(); }
    uint64_t pendingRemovals() const { return m_pendingCount.load(); }

  private:

    struct PendingRemoval {
      obj_handle_t alloc   = 0u;       /* owned +1 reference */
      void*        hostMem = nullptr;  /* owned VirtualAlloc'd backing */
      uint64_t     serial  = 0u;       /* removal gate */
    };

    obj_handle_t    m_queue = 0u;      /* borrowed; outlives the tracker */
    WmtResidencySet m_set;

    std::mutex                  m_mutex;
    bool                        m_dirty = false;   /* uncommitted userspace adds */
    std::vector<PendingRemoval> m_pending;
    std::vector<obj_handle_t>   m_attachedLayerSets;  /* borrowed handles */

    std::atomic<uint64_t> m_adds{0u};
    std::atomic<uint64_t> m_removes{0u};
    std::atomic<uint64_t> m_pendingCount{0u};

  };

}
