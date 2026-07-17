/*
 * MtlWireContext — PE-side producer of the native-backend wire transport
 * (Phase 1, echo mode). See mtl_wire_context.h and driver/mtl/mtl_wire.h
 * for the design and the ring/kick/park contracts.
 */

#include "mtl_wire_context.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "mtl_device_impl.h"
#include "mtl_wire_emit.h"

#include "../mtl/mtl_format.h"

#include "../../bridge/winemetal.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  namespace {
    /* 16 MiB of record space (S3). S1 evidence: ~665 KB/frame in-game, but
     * boot bursts (shader storms: chunked metallibs + creates) are far
     * larger, and the S2 executor consumes slower than the S1 counter — the
     * 4 MiB ring filled during GTA IV boot and tripped the watchdog. */
    constexpr uint64_t WireRingCapacity = 16ull << 20;

    /* S3 shared heap: 256 MiB sparse file (RENDER_SERVER_SPEC layout #2).
     * The PE region is address space, the FILE stays sparse until written. */
    constexpr uint64_t WireHeapCapacity = 256ull << 20;

    /* S3 v2 lazy re-upload stash cap: texture bytes waiting for heap space.
     * Beyond this the content is dropped for good (stale server texture). */
    constexpr uint64_t kMaxPendingUploadBytes = 96ull << 20;

    /* Stall watchdog (S3): one window is the old ~1 s spin bound; the
     * transport survives WireStallWindows consecutive full-ring windows
     * (warn each) before the permanent disable — boot bursts ride through,
     * the dead-server safety net stays. */
    constexpr uint32_t WireStallWindows = 5u;

    /* One ring at a time; a second LIVE context forwards without recording.
     * S3 v3: the claiming context's dtor RELEASES the claim (device churn:
     * a throwaway boot device must not kill the wire for the process), so a
     * later context re-attaches with a fresh server. */
    std::atomic<bool> g_wireClaimed = { false };

    /* True once WireRing_attach has been issued (profiler stats source). */
    std::atomic<bool> g_wireAttached = { false };

    /* S3 v4: dead-server latch. presentObserved (app thread) tracks the
     * server heartbeat; frozen for >3 s while in SERVER mode = dead server,
     * and the CS producer disables instead of streaming into a dead ring
     * until it fills. Reset per attach. */
    std::atomic<bool> g_serverMode = { false };
    std::atomic<bool> g_serverDead = { false };
    std::atomic<uint64_t> g_lastHb = { 0u };
    std::atomic<int64_t> g_lastHbChangeMs = { 0 };
    constexpr int64_t kServerDeadMs = 3000;

    int64_t monotonicMs() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /* PE-side attach counter (log correlation with the unix "attach #N"). */
    std::atomic<uint32_t> g_claimSeq = { 0u };

    /* --- S2 cold-record side channels (mtl_wire_emit.h) ------------------- */

    /* Monotonic wire identity (spec: client-assigned u64). 0 = "no wire
     * identity"; ids start at 1. */
    std::atomic<uint64_t> g_nextWireId = { 1u };

    /* destroy_resource queue: filled by arbitrary-thread destructors,
     * drained by the CS producer at the top of reserveBytes. Bounded: once
     * the transport is dead nothing drains, so overflow clears the queue
     * (the server is gone or will resync via handle-map misses). */
    std::mutex g_destroyMutex;
    std::vector<std::pair<uint64_t, uint64_t>> g_pendingDestroys;
    std::atomic<bool> g_destroysPending = { false };
    constexpr size_t kMaxPendingDestroys = 65536u;

    /* Current backbuffer image (app thread writes from PresentImage; CS
     * producer reads). Rc keeps it alive across the hand-off. */
    std::mutex g_backbufferMutex;
    Rc<MtlTexture> g_backbuffer;

    Rc<MtlTexture> currentBackbuffer() {
      std::lock_guard<std::mutex> lock(g_backbufferMutex);
      return g_backbuffer;
    }

    /* PROF: local presents vs server-produced frames (wire::presentObserved). */
    std::atomic<uint64_t> g_localPresents = { 0u };
    std::atomic<uint64_t> g_srvPace = { 0u };

    /* create_shader chunking: metallib payloads exceed the uint16 record
     * size; keep chunks comfortably under the 0xFFF8 reserve cap. */
    constexpr uint32_t kShaderChunkBytes = 0xE000u;

    /* S3: MIRROR of computeBufferImagePitch (mtl_context_transfer.cpp) — the
     * wire's upload_texture pitches must match what the local execution
     * encodes, byte for byte. rowAlign/sliceAlign are ALIGNMENTS. */
    void wirePitch(MtlFormat format, MtlSize extent,
                   MtlDeviceSize rowAlign, MtlDeviceSize sliceAlign,
                   MtlDeviceSize& bytesPerRow, MtlDeviceSize& bytesPerImage,
                   MtlDeviceSize& totalBytes) {
      const MtlFormatInfo* fi = lookupFormatInfo(format);
      const uint32_t elementSize = fi->elementSize ? fi->elementSize : 4u;
      const uint32_t bw = fi->blockWidth  ? fi->blockWidth  : 1u;
      const uint32_t bh = fi->blockHeight ? fi->blockHeight : 1u;
      auto alignUp = [](MtlDeviceSize v, MtlDeviceSize a) {
        return a > 1u ? (v + a - 1u) / a * a : v;
      };
      const MtlDeviceSize rows = (extent.height + bh - 1u) / bh;
      bytesPerRow   = alignUp(MtlDeviceSize((extent.width + bw - 1u) / bw) * elementSize, rowAlign);
      bytesPerImage = extent.depth > 1u
        ? alignUp(bytesPerRow * rows, sliceAlign)
        : 0u;
      totalBytes = extent.depth > 1u ? bytesPerImage * extent.depth
                                     : bytesPerRow * rows;
    }
  }

  MtlWireContext::MtlWireContext(const Rc<MtlDevice>& device, Rc<MtlContext>&& inner)
  : MtlContext(device), m_inner(std::move(inner)) {
    bool expected = false;
    if (!g_wireClaimed.compare_exchange_strong(expected, true)) {
      Logger::info("MtlWireContext: ring already claimed, forwarding only");
      return;
    }

    /* Page-aligned region (VirtualAlloc, not new[]): the unix side of
     * WireRing_attach relocates it into a MAP_SHARED `ring` file mapped
     * MAP_FIXED over this exact VA — the pointer stays valid for this
     * (possibly 32-bit) PE process while the arm64 server maps the same
     * file. See docs/RENDER_SERVER_SPEC.md, Phase S1. */
    const uint64_t total = sizeof(d9mt_wire_ring) + WireRingCapacity;
    m_ringMem = static_cast<uint8_t*>(::VirtualAlloc(nullptr, SIZE_T(total),
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_ringMem) {
      Logger::err("MtlWireContext: ring allocation failed, forwarding only");
      return;
    }

    /* S3: the shared heap region (content transport). Optional — a failed
     * reserve degrades to a ring-only attach (uploads never emitted). */
    const uint64_t heapTotal = D9MT_WIRE_HEAP_DATA_OFFSET + WireHeapCapacity;
    m_heapMem = static_cast<uint8_t*>(::VirtualAlloc(nullptr, SIZE_T(heapTotal),
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_heapMem)
      Logger::warn("MtlWireContext: heap region reserve failed, content uploads disabled");

    uint32_t heapOk = 0u;
    const uint32_t mode = WireRing_attach(m_ringMem, total,
      m_heapMem, m_heapMem ? heapTotal : 0u, &heapOk);
    if (mode == WMTWireAttachFailed) {
      ::VirtualFree(m_ringMem, 0, MEM_RELEASE);
      m_ringMem = nullptr;
      if (m_heapMem) {
        ::VirtualFree(m_heapMem, 0, MEM_RELEASE);
        m_heapMem = nullptr;
      }
      Logger::err("MtlWireContext: wire attach failed, forwarding only");
      return;
    }

    if (heapOk && m_heapMem) {
      m_heap = reinterpret_cast<d9mt_wire_heap*>(m_heapMem);
      m_heapData = m_heapMem + D9MT_WIRE_HEAP_DATA_OFFSET;
      m_heapCap = WireHeapCapacity;
    } else if (m_heapMem) {
      /* plain memory nobody consumes — release it, run ring-only */
      ::VirtualFree(m_heapMem, 0, MEM_RELEASE);
      m_heapMem = nullptr;
    }

    /* The unix side initialized the control block (in the shm file or, on
     * fallback, in this plain region). The producer code is identical in
     * both modes. */
    m_ring = reinterpret_cast<d9mt_wire_ring*>(m_ringMem);
    m_data = m_ringMem + sizeof(d9mt_wire_ring);
    m_cap = WireRingCapacity;
    m_head = 0u;
    m_active = true;
    m_hasConsumer = mode == WMTWireAttachServer;

    /* S3 v4: reset the dead-server latch for this attach epoch */
    g_serverDead.store(false, std::memory_order_relaxed);
    g_lastHb.store(0u, std::memory_order_relaxed);
    g_lastHbChangeMs.store(monotonicMs(), std::memory_order_relaxed);
    g_serverMode.store(m_hasConsumer, std::memory_order_release);

    const uint32_t claim = g_claimSeq.fetch_add(1u, std::memory_order_relaxed);
    g_wireAttached.store(true, std::memory_order_release);
    if (m_hasConsumer) {
      Logger::info(str::format("MtlWireContext: render server attached (claim #",
        claim, "), shm ring=", WireRingCapacity >> 20, " MiB, heap=",
        m_heap ? (WireHeapCapacity >> 20) : 0u, " MiB (dual execution)"));
    } else {
      Logger::warn(str::format("MtlWireContext: no render server (claim #", claim,
        "), local ring=", WireRingCapacity >> 20,
        " MiB with no consumer (disables on first ring-full)"));
    }
  }

  MtlWireContext::~MtlWireContext() {
    if (m_ring == nullptr)
      return;

    if (m_active) {
      if (beginRecord<d9mt_wire_rec_nul>(d9mt_wire_op_detach))
        endRecord();
    }
    m_ring->shutdown.store(1u, std::memory_order_release);
    WireRing_kick();

    /* Unix side, bounded < 1 s: waits <=500 ms for the server to drain
     * (consumer_exited / progress == head), SIGTERMs + reaps it if still
     * alive, unlinks the shm dir. Only after that is the region released —
     * its munmap drops the last reference to the unlinked ring file. */
    g_wireAttached.store(false, std::memory_order_release);
    WireRing_detach();
    ::VirtualFree(m_ringMem, 0, MEM_RELEASE);
    m_ringMem = nullptr;
    m_ring = nullptr;
    if (m_heapMem) {
      /* heap file was unlinked by detach; this drops the last reference */
      ::VirtualFree(m_heapMem, 0, MEM_RELEASE);
      m_heapMem = nullptr;
      m_heap = nullptr;
    }
    m_active = false;

    /* S3 v3: RELEASE the claim — device churn (D3D9 boot pattern: throwaway
     * caps device destroyed before the real device) must not kill the wire
     * for the process. The next MtlWireContext re-attaches from scratch:
     * fresh ring/heap regions, fresh shm dir (attach-seq suffix), fresh
     * d9mt-serverd (<1 s boot). Cross-epoch state is cleared here: queued
     * destroys reference resources the NEXT server never saw (its resource
     * table starts empty), and the backbuffer Rc pins a dead device's image.
     * Resource wireIds are per-device objects and die with the device; a
     * (theoretical) survivor would carry a stale id and simply never resolve
     * server-side — defined, dummy-bound. */
    { std::lock_guard<std::mutex> lock(g_destroyMutex);
      g_pendingDestroys.clear();
      g_destroysPending.store(false, std::memory_order_release);
    }
    { std::lock_guard<std::mutex> lock(g_backbufferMutex);
      g_backbuffer = nullptr;
    }
    g_serverMode.store(false, std::memory_order_release);
    g_wireClaimed.store(false, std::memory_order_release);
  }

  /* --- ring producer ------------------------------------------------------ */

  bool MtlWireContext::waitForSpace(uint64_t need) {
    uint64_t tail = m_ring->tail.load(std::memory_order_acquire);
    if (m_head - tail + need <= m_cap)
      return true;

    /* Ring full: the consumer is behind (S3: async PSO builds keep it
     * flowing, but bursts still happen) or dead. Kick defensively, then spin
     * in WINDOWS: each ~1 s window that ends still-full logs a warn (the
     * PROF near-miss trail) and counts a stall episode; only
     * WireStallWindows consecutive full windows — a consumer that made NO
     * space for the whole budget — trip the PERMANENT disable (the
     * dead-server safety net). */

    /* No server attached (WMTWireAttachLocal): nothing will EVER consume, so
     * a full ring is final — degrade to forward-only immediately instead of
     * spinning out the bound. */
    if (!m_hasConsumer) {
      m_ring->stalls.fetch_add(1u, std::memory_order_relaxed);
      Logger::warn("MtlWireContext: ring full with no consumer, disabling wire transport");
      m_active = false;
      return false;
    }
    WireRing_kick();

    for (uint32_t window = 1u; window <= WireStallWindows; window++) {
      m_ring->stalls.fetch_add(1u, std::memory_order_relaxed);
      const uint64_t tailAtStart = m_ring->tail.load(std::memory_order_acquire);
      WMTWireStats hbStart = { };
      WireRing_stats(&hbStart);
      for (uint32_t spin = 0u; spin < (1u << 20); spin++) {
        tail = m_ring->tail.load(std::memory_order_acquire);
        if (m_head - tail + need <= m_cap) {
          if (window > 1u)
            Logger::warn(str::format("MtlWireContext: ring recovered after ",
              window, " stall windows"));
          return true;
        }
        if ((spin & 0x3Fu) == 0x3Fu)
          ::Sleep(0);
      }
      /* S3 v4: a live-but-slow consumer heartbeats through a stall (~100
       * ticks per window); frozen heartbeat + zero tail progress = DEAD
       * server — disable NOW instead of burning the full window budget. */
      if (m_ring->tail.load(std::memory_order_acquire) == tailAtStart) {
        WMTWireStats hbEnd = { };
        WireRing_stats(&hbEnd);
        if (hbEnd.heartbeat == hbStart.heartbeat) {
          Logger::err(str::format("MtlWireContext: ring stalled with FROZEN "
            "heartbeat (hb=", hbEnd.heartbeat,
            ") — server dead, disabling wire transport"));
          m_active = false;
          return false;
        }
      }
      Logger::warn(str::format("MtlWireContext: ring stall window ", window,
        "/", WireStallWindows, " (~1s each, consumer behind)"));
      /* consumer moved but the record still doesn't fit: it is ALIVE and the
       * producer is parked, so space converges — only zero-progress windows
       * count toward the disable ("consecutive" semantics). */
      if (m_ring->tail.load(std::memory_order_acquire) != tailAtStart)
        window = 0u;
    }

    Logger::err(str::format("MtlWireContext: ring stalled for ",
      WireStallWindows, " consecutive windows (~", WireStallWindows,
      "s), disabling wire transport"));
    m_active = false;
    return false;
  }

  void* MtlWireContext::reserveBytes(uint16_t op, uint32_t bytes) {
    if (!m_active)
      return nullptr;

    /* S3 v4: never stream into a dead ring. The present path latches
     * g_serverDead when the server heartbeat freezes for >3 s — cheaper and
     * far earlier than waiting for a 16 MiB ring to fill. Permanent. */
    if (m_hasConsumer && g_serverDead.load(std::memory_order_acquire)) {
      Logger::err("MtlWireContext: server heartbeat frozen >3s — disabling wire transport (dead server)");
      m_active = false;
      return nullptr;
    }

    /* S2: drain queued destroy_resource records first — a destroy is queued
     * only after the resource's last possible use, so emitting it before the
     * NEXT record preserves use-before-destroy order on the wire (and
     * destroy-before-recreate for reused handle values). */
    if (!m_draining && g_destroysPending.load(std::memory_order_acquire))
      emitQueuedDestroys();

    bytes = (bytes + 7u) & ~7u;
    if (bytes > 0xFFF8u) /* uint16 size field; no hot record comes close */
      return nullptr;

    /* Sample emptiness BEFORE appending: tail == head means the consumer
     * has drained everything and may be parked -> kick after publish. */
    m_kick = m_ring->tail.load(std::memory_order_acquire) == m_head;

    uint64_t idx = m_head & (m_cap - 1u);
    uint64_t room = m_cap - idx;
    if (bytes > room) {
      /* wrap: pad record covers the remainder to the ring end */
      if (!waitForSpace(room))
        return nullptr;
      auto* pad = reinterpret_cast<d9mt_wire_rec_hdr*>(m_data + idx);
      pad->opcode = uint16_t(d9mt_wire_op_pad);
      pad->size = uint16_t(sizeof(d9mt_wire_rec_hdr));
      pad->_pad0 = 0u;
      m_head += room;
      idx = 0u;
    }

    if (!waitForSpace(bytes))
      return nullptr;

    void* rec = m_data + idx;
    std::memset(rec, 0, bytes);
    auto* hdr = static_cast<d9mt_wire_rec_hdr*>(rec);
    hdr->opcode = op;
    hdr->size = uint16_t(bytes);
    m_pending = bytes;
    return rec;
  }

  template<typename T>
  T* MtlWireContext::beginRecord(uint16_t op, uint32_t trailing) {
    return static_cast<T*>(reserveBytes(op, uint32_t(sizeof(T)) + trailing));
  }

  void MtlWireContext::endRecord() {
    if (!m_pending)
      return;
    m_head += m_pending;
    m_pending = 0u;
    m_ring->head.store(m_head, std::memory_order_release);
    if (m_kick) {
      m_kick = false;
      WireRing_kick();
    }
  }

  void MtlWireContext::fillBufferRef(d9mt_wire_buffer_ref& dst, const MtlBufferSlice& slice) {
    dst.buffer = slice.storage() != nullptr ? slice.storage()->handle.buf : 0u;
    dst.gpu_address = slice.gpuAddress();
    dst.offset = slice.offset();
    dst.length = slice.length();
  }

  void MtlWireContext::fillRtRef(d9mt_wire_rt_ref& dst, const Rc<MtlTextureView>& view) {
    if (view == nullptr) {
      dst.view = 0u;
      dst.format = 0u;
      dst.width = 0u;
      dst.height = 0u;
      dst.sample_count = 0u;
      return;
    }
    dst.view = view->view.tex;
    dst.format = uint32_t(view->key.format);
    if (view->image != nullptr) {
      const MtlImageInfo& info = view->image->info();
      dst.width = info.extent.width;
      dst.height = info.extent.height;
      dst.sample_count = info.sampleCount;
    } else {
      dst.width = 0u;
      dst.height = 0u;
      dst.sample_count = 0u;
    }
  }

  /* --- S2 cold-record emission (CS thread; see mtl_wire_context.h) --------- */

  void MtlWireContext::emitQueuedDestroys() {
    m_draining = true;
    std::vector<std::pair<uint64_t, uint64_t>> pending;
    { std::lock_guard<std::mutex> lock(g_destroyMutex);
      pending.swap(g_pendingDestroys);
      g_destroysPending.store(false, std::memory_order_release);
    }
    for (const auto& d : pending) {
      auto* r = beginRecord<d9mt_wire_rec_destroy_resource>(d9mt_wire_op_destroy_resource);
      if (!r)
        break; /* transport died mid-drain; the rest is moot */
      r->wire_id = d.first;
      r->client_handle = d.second;
      endRecord();
    }
    m_draining = false;
  }

  bool MtlWireContext::ensureBufferCreated(const MtlBufferSlice& slice) {
    return ensureStorageCreated(slice.storage());
  }

  bool MtlWireContext::ensureStorageCreated(const Rc<MtlAllocation>& storage) {
    if (storage == nullptr)
      return false;
    if (storage->wireId)
      return true;
    auto* r = beginRecord<d9mt_wire_rec_create_buffer>(d9mt_wire_op_create_buffer);
    if (!r)
      return false;
    /* id assigned only on successful emission: a 0 wireId resource simply
     * never resolves server-side (defined: dummy-bound). */
    storage->wireId = g_nextWireId.fetch_add(1u, std::memory_order_relaxed);
    r->wire_id = storage->wireId;
    r->client_handle = storage->handle.buf;
    r->size = storage->size;
    r->usage = 0u;       /* informational; not tracked at slice level */
    r->cpu_access = 0u;
    r->heap_offset = 0u; /* content rides upload_buffer records instead */
    endRecord();
    /* S3: initial content snapshot for host-visible backings — static VB/IB
     * bytes are Lock-written BEFORE the first bind (which is when this
     * create fires), so this catches most one-shot geometry. Private
     * backings (mapPtr null) get content only through uploadBuffer taps. */
    if (m_heap && storage->mapPtr && storage->size)
      emitUploadBuffer(storage->wireId, storage->handle.buf, 0u,
                       storage->mapPtr, storage->size);
    return true;
  }

  /* --- S3 shared-heap producer (see mtl_wire_context.h) -------------------- */

  uint64_t MtlWireContext::heapAlloc(const void* src, uint64_t bytes, uint64_t* genOut,
                                     bool quiet) {
    if (m_heap == nullptr || src == nullptr || bytes == 0u)
      return UINT64_MAX;
    const uint64_t need = (bytes + 255u) & ~255ull;
    if (need > m_heapCap / 2u)
      return UINT64_MAX; /* pathological single region; keep the ring live */

    /* regions never straddle the wrap: skip to the next capacity boundary */
    uint64_t start = m_heapHead;
    const uint64_t idx = start & (m_heapCap - 1u);
    if (need > m_heapCap - idx)
      start += m_heapCap - idx;

    const uint64_t consumed = m_heap->consumed.load(std::memory_order_acquire);
    if (start + need - consumed > m_heapCap) {
      /* full — the server has not acked enough regions. NEVER stall the CS
       * thread on bulk data: fail the allocation (texture content goes to
       * the lazy re-upload stash; buffer content is dropped) and count it.
       * quiet = a RETRY probing for space: no double-count, no re-warn. */
      if (!quiet) {
        m_heap->dropped.fetch_add(1u, std::memory_order_relaxed);
        if (!m_heapWarned) {
          m_heapWarned = true;
          Logger::warn("MtlWireContext: shared heap full, stalling content uploads (server acks behind)");
        }
      }
      return UINT64_MAX;
    }

    std::memcpy(m_heapData + (start & (m_heapCap - 1u)), src, size_t(bytes));
    m_heapHead = start + need;
    /* informational (debug); ORDERING rides the ring's head release-store:
     * the record referencing this region publishes after the memcpy above */
    m_heap->head.store(m_heapHead, std::memory_order_release);
    *genOut = m_heapHead;
    return start;
  }

  void MtlWireContext::emitUploadBuffer(uint64_t wireId, uint64_t clientHandle,
                                        uint64_t dstOff, const void* src, uint64_t size) {
    uint64_t gen = 0u;
    const uint64_t off = heapAlloc(src, size, &gen);
    if (off == UINT64_MAX)
      return;
    if (auto* r = beginRecord<d9mt_wire_rec_upload_buffer>(d9mt_wire_op_upload_buffer)) {
      r->wire_id = wireId;
      r->client_handle = clientHandle;
      r->dst_offset = dstOff;
      r->size = size;
      r->heap_offset = off;
      r->heap_generation = gen;
      endRecord();
    }
  }

  void MtlWireContext::emitUploadTextureRecord(const d9mt_wire_rec_upload_texture& fields,
                                               uint64_t heapOff, uint64_t gen) {
    auto* r = beginRecord<d9mt_wire_rec_upload_texture>(d9mt_wire_op_upload_texture);
    if (!r)
      return;
    const d9mt_wire_rec_hdr hdr = r->hdr;
    *r = fields;
    r->hdr = hdr;
    r->heap_offset = heapOff;
    r->heap_generation = gen;
    endRecord();
  }

  /* S3 v2 lazy re-upload: content stashed on a full heap gets another shot —
   * per-image at its next bind (upload-before-bind keeps the server's
   * hasContent gate satisfied by draw time) and image==nullptr sweeps
   * everything once per present. FIFO order is preserved (later stashes of
   * the same subresource must land after earlier ones); the first region
   * that still doesn't fit ends the sweep. */
  void MtlWireContext::retryPendingTexUploads(const MtlTexture* image) {
    if (m_pendingTexUploads.empty() || m_heap == nullptr || !m_active)
      return;
    for (size_t i = 0u; i < m_pendingTexUploads.size(); ) {
      PendingTexUpload& p = m_pendingTexUploads[i];
      if (image != nullptr && p.image.ptr() != image) {
        i++;
        continue;
      }
      uint64_t gen = 0u;
      const uint64_t off = heapAlloc(p.bytes.data(), p.bytes.size(), &gen, true);
      if (off == UINT64_MAX)
        return; /* heap still full — keep order, retry later */
      emitUploadTextureRecord(p.rec, off, gen);
      m_pendingTexBytes -= p.bytes.size();
      m_pendingTexUploads.erase(m_pendingTexUploads.begin() + ptrdiff_t(i));
    }
  }

  void MtlWireContext::emitBindTextureById(MtlStageMask stages, uint32_t slot,
                                           const Rc<MtlTextureView>& view) {
    uint64_t id = 0u;
    if (view != nullptr && view->image != nullptr) {
      id = ensureTextureCreated(view->image);
      retryPendingTexUploads(view->image.ptr());
    }
    auto* r = beginRecord<d9mt_wire_rec_bind_texture_by_id>(d9mt_wire_op_bind_texture_by_id);
    if (!r)
      return;
    r->stages = stages;
    r->slot = slot;
    r->image_wire_id = id;
    if (id) {
      r->format = uint32_t(view->key.format);
      r->view_type = uint32_t(view->key.viewType);
      r->base_mip = view->key.baseMip;
      r->mip_count = view->key.mipCount;
      r->base_layer = view->key.baseLayer;
      r->layer_count = view->key.layerCount;
    }
    endRecord();
  }

  /* Discard rename follow-up: the frontend never re-binds after a rename
   * (that is the point of invalidateBuffer), so the WIRE must re-emit every
   * binding that referenced the renamed logical buffer — otherwise the
   * server keeps drawing from the retired backing (HANDOFF-S2 flag 4). */
  void MtlWireContext::rebindAfterInvalidate(const Rc<MtlBuffer>& buffer) {
    for (uint32_t i = 0u; i < 16u; i++) {
      BufShadow& sh = m_streamShadow[i];
      if (sh.buffer.ptr() != buffer.ptr())
        continue;
      MtlBufferSlice slice(sh.buffer, sh.offset, sh.length); /* CS thread: snapshots CURRENT storage */
      ensureBufferCreated(slice);
      if (auto* r = beginRecord<d9mt_wire_rec_bind_vertex_buffer>(d9mt_wire_op_bind_vertex_buffer)) {
        r->binding = i;
        r->stride = sh.stride;
        fillBufferRef(r->buf, slice);
        endRecord();
      }
    }
    for (uint32_t st = 0u; st < 2u; st++) {
      for (uint32_t slot = 0u; slot < 8u; slot++) {
        BufShadow& sh = m_uboShadow[st][slot];
        if (sh.buffer.ptr() != buffer.ptr())
          continue;
        MtlBufferSlice slice(sh.buffer, sh.offset, sh.length);
        ensureBufferCreated(slice);
        if (auto* r = beginRecord<d9mt_wire_rec_bind_uniform_buffer>(d9mt_wire_op_bind_uniform_buffer)) {
          r->stages = 1u << st;
          r->slot = slot;
          fillBufferRef(r->buf, slice);
          endRecord();
        }
      }
    }
    if (m_indexShadow.buffer.ptr() == buffer.ptr()) {
      MtlBufferSlice slice(m_indexShadow.buffer, m_indexShadow.offset, m_indexShadow.length);
      ensureBufferCreated(slice);
      if (auto* r = beginRecord<d9mt_wire_rec_bind_index_buffer>(d9mt_wire_op_bind_index_buffer)) {
        fillBufferRef(r->buf, slice);
        r->index_type = m_indexShadow.type;
        endRecord();
      }
    }
  }

  uint64_t MtlWireContext::ensureTextureCreated(const Rc<MtlTexture>& image) {
    if (image == nullptr)
      return 0u;
    if (image->wireId)
      return image->wireId;
    /* All driver textures are MtlTextureImpl (device + presenter); its
     * `info` member is the ORIGINAL MtlImageCreateInfo (MtlUsage bits +
     * cube), which the record mirrors. */
    auto* impl = static_cast<MtlTextureImpl*>(image.ptr());
    auto* r = beginRecord<d9mt_wire_rec_create_texture>(d9mt_wire_op_create_texture);
    if (!r)
      return 0u;
    image->wireId = g_nextWireId.fetch_add(1u, std::memory_order_relaxed);
    r->wire_id = image->wireId;
    r->client_handle = image->texture.tex;
    r->type = uint32_t(impl->info.type);
    r->format = uint32_t(impl->info.format != MtlFormat::Invalid
      ? impl->info.format : image->info().format);
    r->width = impl->info.extent.width ? impl->info.extent.width
                                       : image->info().extent.width;
    r->height = impl->info.extent.height ? impl->info.extent.height
                                         : image->info().extent.height;
    r->depth = impl->info.extent.depth ? impl->info.extent.depth : 1u;
    r->mip_levels = impl->info.mipLevels;
    r->array_layers = impl->info.arrayLayers;
    r->sample_count = impl->info.sampleCount;
    r->usage = uint32_t(impl->info.usage);
    r->cube = impl->info.cube ? 1u : 0u;
    endRecord();
    return image->wireId;
  }

  void MtlWireContext::ensureShaderCreated(const Rc<MtlShaderFunction>& fn) {
    if (fn == nullptr || fn->wireId)
      return;
    if (!m_active || fn->wireMetallib.empty()) {
      /* transport dead (or nothing to send): drop the byte copy for good */
      std::vector<uint8_t>().swap(fn->wireMetallib);
      return;
    }
    const std::vector<uint8_t>& bytes = fn->wireMetallib;
    const uint64_t id = g_nextWireId.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t abCount = uint32_t(fn->abWords.size());
    uint64_t off = 0u;
    bool ok = true;
    while (off < bytes.size()) {
      const uint32_t chunk = bytes.size() - off > kShaderChunkBytes
        ? kShaderChunkBytes : uint32_t(bytes.size() - off);
      const uint32_t abBytes = off == 0u ? ((abCount * 2u + 7u) & ~7u) : 0u;
      auto* r = beginRecord<d9mt_wire_rec_create_shader>(
        d9mt_wire_op_create_shader, abBytes + ((chunk + 7u) & ~7u));
      if (!r) {
        ok = false;
        break;
      }
      r->wire_id = id;
      r->client_handle = fn->library;
      r->stage = uint32_t(fn->stage);
      r->ab = fn->abWords.empty() ? 0u : 1u;
      r->total_size = bytes.size();
      r->chunk_offset = off;
      r->chunk_size = chunk;
      r->ab_word_count = off == 0u ? abCount : 0u;
      r->ab_table_index = fn->abTableIndex;
      std::memcpy(r->cbv_slot_map, fn->cbvSlotMap.data(), 8u);
      const char* entry = fn->entryName ? fn->entryName : "main";
      std::strncpy(r->entry, entry, sizeof(r->entry) - 1u);
      auto* tail = reinterpret_cast<uint8_t*>(r + 1);
      if (off == 0u) {
        for (uint32_t i = 0u; i < abCount; i++) {
          tail[i * 2u]      = uint8_t(fn->abWords[i].kind);
          tail[i * 2u + 1u] = fn->abWords[i].reg;
        }
      }
      std::memcpy(tail + abBytes, bytes.data() + off, chunk);
      endRecord();
      off += chunk;
    }
    if (ok)
      fn->wireId = id;
    std::vector<uint8_t>().swap(fn->wireMetallib);
  }

  void MtlWireContext::ensureSamplerCreated(const Rc<MtlSamplerState>& sampler) {
    if (sampler == nullptr || sampler->wireId)
      return;
    auto* impl = static_cast<MtlSamplerStateImpl*>(sampler.ptr());
    auto* r = beginRecord<d9mt_wire_rec_create_sampler>(d9mt_wire_op_create_sampler);
    if (!r)
      return;
    sampler->wireId = g_nextWireId.fetch_add(1u, std::memory_order_relaxed);
    r->wire_id = sampler->wireId;
    r->client_handle = sampler->sampler;
    r->mag_filter = uint32_t(impl->key.magFilter);
    r->min_filter = uint32_t(impl->key.minFilter);
    r->mip_mode = uint32_t(impl->key.mipMode);
    r->address_u = uint32_t(impl->key.addressU);
    r->address_v = uint32_t(impl->key.addressV);
    r->address_w = uint32_t(impl->key.addressW);
    r->max_anisotropy = impl->key.maxAnisotropy;
    r->mip_lod_bias = impl->key.mipLodBias;
    r->min_lod = impl->key.minLod;
    r->max_lod = impl->key.maxLod;
    r->compare_op = uint32_t(impl->key.compareOp);
    r->compare_enable = impl->key.compareEnable ? 1u : 0u;
    r->border_color = uint32_t(impl->key.borderColor);
    endRecord();
  }

  /* --- lifecycle / submission (cold: forward only) ------------------------- */

  void MtlWireContext::beginRecording() {
    m_inner->beginRecording();
  }

  void MtlWireContext::endFrame() {
    m_inner->endFrame();
  }

  void MtlWireContext::flushCommandList(const char* reason, MtlSubmitStatus* status) {
    m_inner->flushCommandList(reason, status);
  }

  Rc<MtlCommandBuffer> MtlWireContext::beginExternalRendering() {
    return m_inner->beginExternalRendering();
  }

  void MtlWireContext::synchronizeWsi(MtlPresenterSync sync) {
    /* S2: THE present marker. One presentFrame per present, carrying the
     * client's monotonic serial + the backbuffer image's wire id; the server
     * commits its frame, publishes {slot, serial} in status v2 and rotates
     * its IOSurface slot. Emitted BEFORE the forward so the server sees the
     * frame boundary at the same stream position the client executes it. */
    if (m_active) {
      /* S3 v2: once-per-frame sweep of content waiting for heap space */
      retryPendingTexUploads(nullptr);
      Rc<MtlTexture> bb = currentBackbuffer();
      uint64_t bbId = bb != nullptr ? ensureTextureCreated(bb) : 0u;
      if (auto* r = beginRecord<d9mt_wire_rec_present_frame>(d9mt_wire_op_present_frame)) {
        r->frame_serial = ++m_presentSerial;
        r->backbuffer_wire_id = bbId;
        endRecord();
      }
    }
    m_inner->synchronizeWsi(sync);
  }

  void MtlWireContext::beginLatencyTracking(const Rc<MtlLatencyTracker>& tracker, uint64_t frameId) {
    m_inner->beginLatencyTracking(tracker, frameId);
  }

  void MtlWireContext::endLatencyTracking(const Rc<MtlLatencyTracker>& tracker) {
    m_inner->endLatencyTracking(tracker);
  }

  /* --- shaders & pipeline state -------------------------------------------- */

  void MtlWireContext::bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) {
    /* S2: first bind emits the chunked create_shader (metallib bytes +
     * cbvSlotMap/abWords metadata, all final by bind time) and frees the
     * client-side byte copy either way. */
    if (fn != nullptr && m_ring != nullptr)
      ensureShaderCreated(fn);
    if (auto* r = beginRecord<d9mt_wire_rec_bind_shader>(d9mt_wire_op_bind_shader)) {
      r->stage = uint32_t(stage);
      r->library = fn != nullptr ? fn->library : 0u;
      endRecord();
    }
    m_inner->bindShader(stage, std::move(fn));
  }

  void MtlWireContext::setFunctionConstants(MtlPipeline pipeline, uint32_t index, uint32_t count, const void* data) {
    m_inner->setFunctionConstants(pipeline, index, count, data);
  }

  void MtlWireContext::setVertexLayout(uint32_t nAttr, const MtlVertexAttr* attrs,
                                       uint32_t nLayout, const MtlVertexLayout* layouts) {
    uint32_t trailing = nAttr * uint32_t(sizeof(d9mt_wire_vertex_attr))
                      + nLayout * uint32_t(sizeof(d9mt_wire_vertex_layout));
    if (auto* r = beginRecord<d9mt_wire_rec_set_vertex_layout>(d9mt_wire_op_set_vertex_layout, trailing)) {
      r->n_attr = nAttr;
      r->n_layout = nLayout;
      auto* wa = reinterpret_cast<d9mt_wire_vertex_attr*>(r + 1);
      for (uint32_t i = 0u; i < nAttr; i++) {
        wa[i].location = attrs[i].location;
        wa[i].binding = attrs[i].binding;
        wa[i].format = uint32_t(attrs[i].format);
        wa[i].offset = attrs[i].offset;
      }
      auto* wl = reinterpret_cast<d9mt_wire_vertex_layout*>(wa + nAttr);
      for (uint32_t i = 0u; i < nLayout; i++) {
        wl[i].binding = layouts[i].binding;
        wl[i].stride = layouts[i].stride;
        wl[i].per_instance = layouts[i].perInstance ? 1u : 0u;
        wl[i].divisor = layouts[i].divisor;
      }
      endRecord();
    }
    m_inner->setVertexLayout(nAttr, attrs, nLayout, layouts);
  }

  void MtlWireContext::setPrimitiveTopology(MtlPrimitiveTopology topology) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_primitive_topology>(d9mt_wire_op_set_primitive_topology)) {
      r->topology = uint32_t(topology);
      endRecord();
    }
    m_inner->setPrimitiveTopology(topology);
  }

  void MtlWireContext::setViewports(uint32_t n, const MtlViewport* viewports) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_viewports>(d9mt_wire_op_set_viewports,
          n * uint32_t(sizeof(d9mt_wire_viewport)))) {
      r->count = n;
      auto* wv = reinterpret_cast<d9mt_wire_viewport*>(r + 1);
      for (uint32_t i = 0u; i < n; i++) {
        wv[i].origin_x = viewports[i].originX;
        wv[i].origin_y = viewports[i].originY;
        wv[i].width = viewports[i].width;
        wv[i].height = viewports[i].height;
        wv[i].znear = viewports[i].znear;
        wv[i].zfar = viewports[i].zfar;
        wv[i].scissor_x = viewports[i].scissor.x;
        wv[i].scissor_y = viewports[i].scissor.y;
        wv[i].scissor_w = viewports[i].scissor.width;
        wv[i].scissor_h = viewports[i].scissor.height;
      }
      endRecord();
    }
    m_inner->setViewports(n, viewports);
  }

  void MtlWireContext::setRasterizerState(const MtlRasterizerState& rs) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_rasterizer_state>(d9mt_wire_op_set_rasterizer_state)) {
      r->cull_mode = uint32_t(rs.cullMode);
      r->fill_mode = uint32_t(rs.fillMode);
      r->front_face = uint32_t(rs.frontFace);
      r->depth_clip = rs.depthClip ? 1u : 0u;
      r->flat_shading = rs.flatShading ? 1u : 0u;
      r->sample_count = rs.sampleCount;
      endRecord();
    }
    m_inner->setRasterizerState(rs);
  }

  void MtlWireContext::setMultisampleState(const MtlMultisampleState& ms) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_multisample_state>(d9mt_wire_op_set_multisample_state)) {
      r->sample_count = ms.sampleCount;
      r->sample_mask = ms.sampleMask;
      r->alpha_to_coverage = ms.alphaToCoverage ? 1u : 0u;
      r->alpha_to_one = ms.alphaToOne ? 1u : 0u;
      endRecord();
    }
    m_inner->setMultisampleState(ms);
  }

  namespace {
    void fillStencilFace(d9mt_wire_stencil_face& dst, const MtlStencilState& src) {
      dst.fail_op = uint32_t(src.failOp);
      dst.depth_fail_op = uint32_t(src.depthFailOp);
      dst.pass_op = uint32_t(src.passOp);
      dst.compare_op = uint32_t(src.compareOp);
      dst.read_mask = src.readMask;
      dst.write_mask = src.writeMask;
    }
  }

  void MtlWireContext::setDepthStencilState(const MtlDepthStencilState& ds) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_depth_stencil_state>(d9mt_wire_op_set_depth_stencil_state)) {
      r->depth_compare_op = uint32_t(ds.depthCompareOp);
      r->depth_test = ds.depthTest ? 1u : 0u;
      r->depth_write = ds.depthWrite ? 1u : 0u;
      r->stencil_enable = ds.stencilEnable ? 1u : 0u;
      fillStencilFace(r->front, ds.front);
      fillStencilFace(r->back, ds.back);
      endRecord();
    }
    m_inner->setDepthStencilState(ds);
  }

  void MtlWireContext::setBlendMode(uint32_t rt, const MtlBlendMode& blend) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_blend_mode>(d9mt_wire_op_set_blend_mode)) {
      r->rt = rt;
      r->enable = blend.enable ? 1u : 0u;
      r->color_src = uint32_t(blend.colorSrc);
      r->color_dst = uint32_t(blend.colorDst);
      r->color_op = uint32_t(blend.colorOp);
      r->alpha_src = uint32_t(blend.alphaSrc);
      r->alpha_dst = uint32_t(blend.alphaDst);
      r->alpha_op = uint32_t(blend.alphaOp);
      r->write_mask = blend.writeMask;
      endRecord();
    }
    m_inner->setBlendMode(rt, blend);
  }

  void MtlWireContext::setBlendColor(MtlColor4 color) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_blend_color>(d9mt_wire_op_set_blend_color)) {
      r->color[0] = color.r;
      r->color[1] = color.g;
      r->color[2] = color.b;
      r->color[3] = color.a;
      endRecord();
    }
    m_inner->setBlendColor(color);
  }

  void MtlWireContext::setStencilReference(uint32_t reference) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_stencil_reference>(d9mt_wire_op_set_stencil_reference)) {
      r->reference = reference;
      endRecord();
    }
    m_inner->setStencilReference(reference);
  }

  void MtlWireContext::setDepthBias(MtlDepthBias bias) {
    if (auto* r = beginRecord<d9mt_wire_rec_set_depth_bias>(d9mt_wire_op_set_depth_bias)) {
      r->constant_factor = bias.constantFactor;
      r->slope_factor = bias.slopeFactor;
      r->clamp = bias.clamp;
      endRecord();
    }
    m_inner->setDepthBias(bias);
  }

  void MtlWireContext::setDepthBounds(MtlDepthBounds bounds) {
    m_inner->setDepthBounds(bounds);
  }

  /* --- slot-based argument-table binding ------------------------------------ */

  void MtlWireContext::bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) {
    if (m_active)
      ensureBufferCreated(slice);
    if (auto* r = beginRecord<d9mt_wire_rec_bind_vertex_buffer>(d9mt_wire_op_bind_vertex_buffer)) {
      r->binding = binding;
      r->stride = stride;
      fillBufferRef(r->buf, slice);
      endRecord();
    }
    if (m_ring != nullptr && binding < 16u) {
      BufShadow& sh = m_streamShadow[binding];
      sh.buffer = slice.buffer();
      sh.offset = slice.offset();
      sh.length = slice.length();
      sh.stride = stride;
    }
    m_inner->bindVertexBuffer(binding, std::move(slice), stride);
  }

  void MtlWireContext::bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_vertex_buffer_range>(d9mt_wire_op_bind_vertex_buffer_range)) {
      r->binding = binding;
      r->stride = stride;
      r->offset = offset;
      r->length = length;
      endRecord();
    }
    if (m_ring != nullptr && binding < 16u) {
      BufShadow& sh = m_streamShadow[binding];
      sh.offset = offset;
      sh.length = length;
      sh.stride = stride;
    }
    m_inner->bindVertexBufferRange(binding, offset, length, stride);
  }

  void MtlWireContext::bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) {
    if (m_active)
      ensureBufferCreated(slice);
    if (auto* r = beginRecord<d9mt_wire_rec_bind_index_buffer>(d9mt_wire_op_bind_index_buffer)) {
      fillBufferRef(r->buf, slice);
      r->index_type = uint32_t(type);
      endRecord();
    }
    if (m_ring != nullptr) {
      m_indexShadow.buffer = slice.buffer();
      m_indexShadow.offset = slice.offset();
      m_indexShadow.length = slice.length();
      m_indexShadow.type = uint32_t(type);
    }
    m_inner->bindIndexBuffer(std::move(slice), type);
  }

  void MtlWireContext::bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) {
    if (m_active)
      ensureBufferCreated(slice);
    if (auto* r = beginRecord<d9mt_wire_rec_bind_uniform_buffer>(d9mt_wire_op_bind_uniform_buffer)) {
      r->stages = stages;
      r->slot = slot;
      fillBufferRef(r->buf, slice);
      endRecord();
    }
    if (m_ring != nullptr && slot < 8u) {
      for (uint32_t st = 0u; st < 2u; st++) {
        if (!(stages & (1u << st)))
          continue;
        BufShadow& sh = m_uboShadow[st][slot];
        sh.buffer = slice.buffer();
        sh.offset = slice.offset();
        sh.length = slice.length();
      }
    }
    m_inner->bindUniformBuffer(stages, slot, std::move(slice));
  }

  void MtlWireContext::bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_uniform_buffer_range>(d9mt_wire_op_bind_uniform_buffer_range)) {
      r->stages = stages;
      r->slot = slot;
      r->offset = offset;
      r->length = length;
      endRecord();
    }
    if (m_ring != nullptr && slot < 8u) {
      for (uint32_t st = 0u; st < 2u; st++) {
        if (!(stages & (1u << st)))
          continue;
        m_uboShadow[st][slot].offset = offset;
        m_uboShadow[st][slot].length = length;
      }
    }
    m_inner->bindUniformBufferRange(stages, slot, offset, length);
  }

  void MtlWireContext::bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_texture>(d9mt_wire_op_bind_texture)) {
      r->stages = stages;
      r->slot = slot;
      if (view != nullptr) {
        r->view = view->view.tex;
        r->gpu_resource_id = static_cast<MtlTextureViewImpl*>(view.ptr())->gpuResourceId;
        r->format = uint32_t(view->key.format);
        r->view_type = uint32_t(view->key.viewType);
      }
      endRecord();
    }
    /* S3 twin (op 35): names the IMAGE by wire id so the server binds the
     * real texture (view key bits carried for server-side view minting). */
    if (m_active)
      emitBindTextureById(stages, slot, view);
    m_inner->bindTexture(stages, slot, std::move(view));
  }

  void MtlWireContext::bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view) {
    m_inner->bindBufferView(stages, slot, std::move(view));
  }

  void MtlWireContext::bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) {
    if (m_active && sampler != nullptr)
      ensureSamplerCreated(sampler);
    if (auto* r = beginRecord<d9mt_wire_rec_bind_sampler>(d9mt_wire_op_bind_sampler)) {
      r->stages = stages;
      r->slot = slot;
      if (sampler != nullptr) {
        r->sampler = sampler->sampler;
        r->gpu_resource_id = static_cast<MtlSamplerStateImpl*>(sampler.ptr())->gpuResourceId;
      }
      endRecord();
    }
    m_inner->bindSampler(stages, slot, std::move(sampler));
  }

  void MtlWireContext::pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) {
    if (auto* r = beginRecord<d9mt_wire_rec_push_data>(d9mt_wire_op_push_data, size)) {
      r->stages = stages;
      r->offset = offset;
      r->size = size;
      std::memcpy(r + 1, data, size);
      endRecord();
    }
    m_inner->pushData(stages, offset, size, data);
  }

  /* --- render targets / barriers --------------------------------------------- */

  void MtlWireContext::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_render_targets>(d9mt_wire_op_bind_render_targets)) {
      for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++)
        fillRtRef(r->color[i], targets.color[i]);
      fillRtRef(r->depth_stencil, targets.depthStencil);
      r->feedback_loop = feedbackLoop;
      endRecord();
    }

    /* S2 twin (op 30): attachments named by IMAGE wire id so the server can
     * resolve them; the client's backbuffer is flagged for IOSurface
     * substitution. Creates emit first (ensureTextureCreated), so the by-id
     * record only ever references known ids. */
    if (m_active) {
      Rc<MtlTexture> bb = currentBackbuffer();
      auto fillIdRef = [&](d9mt_wire_rt_id_ref& dst, const Rc<MtlTextureView>& view) {
        dst = d9mt_wire_rt_id_ref();
        if (view == nullptr || view->image == nullptr)
          return;
        dst.image_wire_id = ensureTextureCreated(view->image);
        if (bb != nullptr && view->image.ptr() == bb.ptr())
          dst.flags |= D9MT_WIRE_RT_FLAG_BACKBUFFER;
        dst.format = uint32_t(view->key.format);
        const MtlImageInfo& info = view->image->info();
        dst.width = info.extent.width;
        dst.height = info.extent.height;
        dst.sample_count = info.sampleCount;
        dst.base_mip = view->key.baseMip;
        dst.base_layer = view->key.baseLayer;
      };
      if (auto* r = beginRecord<d9mt_wire_rec_set_render_targets_by_id>(
            d9mt_wire_op_set_render_targets_by_id)) {
        for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++)
          fillIdRef(r->color[i], targets.color[i]);
        fillIdRef(r->depth_stencil, targets.depthStencil);
        r->feedback_loop = feedbackLoop;
        endRecord();
      }
    }

    m_inner->bindRenderTargets(std::move(targets), feedbackLoop);
  }

  void MtlWireContext::emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                                           MtlStageMask dstStages, MtlBarrierScope dstScope) {
    m_inner->emitGraphicsBarrier(srcStages, srcScope, dstStages, dstScope);
  }

  /* --- draws -------------------------------------------------------------------- */

  void MtlWireContext::draw(uint32_t count, const MtlDrawArgs* args) {
    if (auto* r = beginRecord<d9mt_wire_rec_draw>(d9mt_wire_op_draw,
          count * uint32_t(sizeof(d9mt_wire_draw_args)))) {
      r->count = count;
      auto* out = reinterpret_cast<d9mt_wire_draw_args*>(r + 1);
      for (uint32_t i = 0u; i < count; i++) {
        out[i].vertex_count = args[i].vertexCount;
        out[i].instance_count = args[i].instanceCount;
        out[i].first_vertex = args[i].firstVertex;
        out[i].first_instance = args[i].firstInstance;
      }
      endRecord();
    }
    m_inner->draw(count, args);
  }

  void MtlWireContext::drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) {
    if (auto* r = beginRecord<d9mt_wire_rec_draw_indexed>(d9mt_wire_op_draw_indexed,
          count * uint32_t(sizeof(d9mt_wire_draw_indexed_args)))) {
      r->count = count;
      auto* out = reinterpret_cast<d9mt_wire_draw_indexed_args*>(r + 1);
      for (uint32_t i = 0u; i < count; i++) {
        out[i].index_count = args[i].indexCount;
        out[i].instance_count = args[i].instanceCount;
        out[i].first_index = args[i].firstIndex;
        out[i].vertex_offset = args[i].vertexOffset;
        out[i].first_instance = args[i].firstInstance;
        out[i]._pad0 = 0u;
      }
      endRecord();
    }
    m_inner->drawIndexed(count, args);
  }

  /* --- clears --------------------------------------------------------------------- */

  void MtlWireContext::clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                                         MtlClearValue value, MtlAspectMask discard) {
    if (auto* r = beginRecord<d9mt_wire_rec_clear_render_target>(d9mt_wire_op_clear_render_target)) {
      fillRtRef(r->rt, view);
      r->clear_aspects = clear;
      r->discard_aspects = discard;
      r->value.color[0] = value.color.r;
      r->value.color[1] = value.color.g;
      r->value.color[2] = value.color.b;
      r->value.color[3] = value.color.a;
      r->value.depth = value.depth;
      r->value.stencil = value.stencil;
      endRecord();
    }

    /* S2 twin (op 32): clear named by IMAGE wire id (op 22 carries the view
     * handle, which has no server identity). */
    if (m_active && view != nullptr && view->image != nullptr) {
      uint64_t id = ensureTextureCreated(view->image);
      Rc<MtlTexture> bb = currentBackbuffer();
      if (auto* r = beginRecord<d9mt_wire_rec_clear_render_target_by_id>(
            d9mt_wire_op_clear_render_target_by_id)) {
        r->image_wire_id = id;
        if (bb != nullptr && view->image.ptr() == bb.ptr())
          r->flags |= D9MT_WIRE_RT_FLAG_BACKBUFFER;
        r->clear_aspects = clear;
        r->discard_aspects = discard;
        r->value.color[0] = value.color.r;
        r->value.color[1] = value.color.g;
        r->value.color[2] = value.color.b;
        r->value.color[3] = value.color.a;
        r->value.depth = value.depth;
        r->value.stencil = value.stencil;
        endRecord();
      }
    }

    m_inner->clearRenderTarget(view, clear, value, discard);
  }

  void MtlWireContext::clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                                      MtlAspectMask aspects, MtlClearValue value) {
    m_inner->clearImageView(view, offset, extent, aspects, value);
  }

  /* --- cold paths: forward only ------------------------------------------------------ */

  void MtlWireContext::copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                                   const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    m_inner->copyTexture(dst, dstSub, dstOff, src, srcSub, srcOff, extent);
  }

  void MtlWireContext::copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                  const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    m_inner->copyBuffer(dst, dstOff, src, srcOff, bytes);
  }

  void MtlWireContext::copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                                           MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    m_inner->copyTextureToBuffer(dst, off, rowAlign, sliceAlign, format, src, srcSub, srcOff, extent);
  }

  void MtlWireContext::copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                                           const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format) {
    /* S3: THE texture-content seam (frontend Lock/UpdateTexture staging
     * lands here). Additive: the staging bytes ALSO copy into a heap region
     * and an upload_texture record names the subresource box — the local
     * execution below is untouched. */
    if (m_active && m_heap && dst != nullptr && src != nullptr) {
      const uint64_t texId = ensureTextureCreated(dst);
      const void* bytes = src->mapPtr(off);
      if (texId && bytes) {
        const MtlFormat fmt = format != MtlFormat::Invalid ? format : dst->format;
        MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u, total = 0u;
        wirePitch(fmt, extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage, total);
        d9mt_wire_rec_upload_texture fields = {};
        fields.wire_id = texId;
        fields.client_handle = dst->texture.tex;
        fields.mip = dstSub.mipLevel;
        fields.layer = dstSub.baseLayer;
        fields.x = uint32_t(dstOff.x);
        fields.y = uint32_t(dstOff.y);
        fields.z = uint32_t(dstOff.z);
        fields.width = extent.width;
        fields.height = extent.height;
        fields.depth = extent.depth;
        fields.row_pitch = uint32_t(bytesPerRow);
        fields.slice_pitch = uint32_t(bytesPerImage);
        fields.format = uint32_t(fmt);
        fields.data_size = total;
        /* stashed content for this image must land FIRST (subresource
         * ordering), so a stash forces this upload to queue behind it */
        retryPendingTexUploads(dst.ptr());
        bool stashPending = false;
        for (const auto& p : m_pendingTexUploads) {
          if (p.image.ptr() == dst.ptr()) {
            stashPending = true;
            break;
          }
        }
        uint64_t gen = 0u;
        uint64_t heapOff = UINT64_MAX;
        if (!stashPending)
          heapOff = heapAlloc(bytes, total, &gen);
        if (heapOff != UINT64_MAX) {
          emitUploadTextureRecord(fields, heapOff, gen);
        } else if (m_pendingTexBytes + total <= kMaxPendingUploadBytes) {
          /* S3 v2 lazy re-upload: heap full — keep the bytes (staging
           * recycles them) and retry at the image's next bind / present. */
          PendingTexUpload p;
          p.image = dst;
          p.rec = fields;
          p.bytes.assign(static_cast<const uint8_t*>(bytes),
                         static_cast<const uint8_t*>(bytes) + total);
          m_pendingTexBytes += total;
          m_pendingTexUploads.push_back(std::move(p));
        }
        /* else: stash cap exceeded — content lost for good (heapDrop'd) */
      }
    }
    m_inner->copyBufferToTexture(dst, dstSub, dstOff, extent, src, off, rowAlign, sliceAlign, format);
  }

  void MtlWireContext::resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                                      MtlFormat format, MtlResolveMode color, MtlResolveMode stencil) {
    m_inner->resolveTexture(dst, src, region, format, color, stencil);
  }

  void MtlWireContext::blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                                       const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                                       MtlSamplerFilter filter) {
    m_inner->blitTextureView(dst, dstBox, src, srcBox, filter);
  }

  bool MtlWireContext::presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) {
    return m_inner->presentUpscale(dst, src);
  }

  void MtlWireContext::generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter) {
    m_inner->generateMipmaps(view, filter);
  }

  void MtlWireContext::transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope) {
    m_inner->transformImage(image, range, scope);
  }

  void MtlWireContext::initBuffer(const Rc<MtlBuffer>& buffer) {
    m_inner->initBuffer(buffer);
  }

  void MtlWireContext::initImage(const Rc<MtlTexture>& image) {
    m_inner->initImage(image);
  }

  void MtlWireContext::invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) {
    /* S3: the Discard-rename refresh point. Three wire duties, all additive:
     *  1. create the NEW backing (recycled pooled backings already have ids),
     *  2. best-effort content snapshot — the CS may run this before the app
     *     finishes writing the renamed slice (Lock returns first), so this
     *     can carry PARTIAL bytes; defined-but-stale on the server, honest
     *     flag in HANDOFF-S3,
     *  3. re-emit every binding that referenced the logical buffer (the
     *     frontend never re-binds after a rename). */
    if (m_active && buffer != nullptr) {
      const Rc<MtlAllocation>& fresh = buffer->storage();
      if (fresh != nullptr) {
        const bool wasNew = fresh->wireId == 0u;
        ensureStorageCreated(fresh); /* emits the initial content snapshot */
        if (!wasNew && m_heap && fresh->mapPtr && fresh->size)
          emitUploadBuffer(fresh->wireId, fresh->handle.buf, 0u,
                           fresh->mapPtr, fresh->size);
        rebindAfterInvalidate(buffer);
      }
    }
    m_inner->invalidateBuffer(buffer, std::move(slice));
  }

  void MtlWireContext::uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                    const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) {
    /* S3: buffer-content seam (constant-buffer ring et al). */
    if (m_active && m_heap && dst != nullptr && staging != nullptr && size) {
      const Rc<MtlAllocation>& storage = dst->storage();
      const void* bytes = staging->mapPtr(srcOff);
      if (storage != nullptr && bytes && ensureStorageCreated(storage))
        emitUploadBuffer(storage->wireId, storage->handle.buf, dstOff, bytes, size);
    }
    m_inner->uploadBuffer(dst, dstOff, staging, srcOff, size);
  }

  bool MtlWireContext::ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage) {
    return m_inner->ensureTextureUsage(image, usage);
  }

  void MtlWireContext::beginQuery(const Rc<MtlQuery>& query) {
    m_inner->beginQuery(query);
  }

  void MtlWireContext::endQuery(const Rc<MtlQuery>& query) {
    m_inner->endQuery(query);
  }

  void MtlWireContext::writeTimestamp(const Rc<MtlQuery>& query) {
    m_inner->writeTimestamp(query);
  }

  void MtlWireContext::signal(const Rc<MtlSignal>& sig, uint64_t value) {
    m_inner->signal(sig, value);
  }

  void MtlWireContext::signalGpuEvent(const Rc<MtlEvent>& event) {
    m_inner->signalGpuEvent(event);
  }

  void MtlWireContext::track(Rc<MtlResource>&& resource) {
    m_inner->track(std::move(resource));
  }

  void MtlWireContext::beginDebugLabel(const char* label) {
    m_inner->beginDebugLabel(label);
  }

  void MtlWireContext::endDebugLabel() {
    m_inner->endDebugLabel();
  }

  void MtlWireContext::insertDebugLabel(const char* label) {
    m_inner->insertDebugLabel(label);
  }

}

/* --- S2 cross-file wire surface (mtl_wire_emit.h) --------------------------- */
namespace dxvk::backend::wire {

  void queueDestroy(uint64_t wireId, uint64_t clientHandle) {
    std::lock_guard<std::mutex> lock(g_destroyMutex);
    if (g_pendingDestroys.size() >= kMaxPendingDestroys) {
      /* nothing is draining (transport dead) — cap the queue */
      g_pendingDestroys.clear();
    }
    g_pendingDestroys.emplace_back(wireId, clientHandle);
    g_destroysPending.store(true, std::memory_order_release);
  }

  void noteBackbuffer(const Rc<MtlTexture>& image) {
    std::lock_guard<std::mutex> lock(g_backbufferMutex);
    g_backbuffer = image;
  }

  void presentObserved() {
    const uint64_t local =
      g_localPresents.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (!g_wireAttached.load(std::memory_order_acquire))
      return;
    WMTWireStats s = { };
    WireRing_stats(&s);
    /* S2 pacing gate: server produced frame serial >= (local serial - 1)
     * counts as "kept pace" for this present. */
    if (s.server_frame_serial + 1u >= local)
      g_srvPace.fetch_add(1u, std::memory_order_relaxed);

    /* S3 v4 dead-server latch: the heartbeat ticks ~100 Hz whenever the
     * server lives (the v2 create worker keeps the consumer loop — and thus
     * the heartbeat — running through boot storms). Frozen for >3 s in
     * SERVER mode = dead; the CS producer disables at its next record
     * instead of streaming into a dead ring until it fills. */
    if (g_serverMode.load(std::memory_order_acquire) &&
        !g_serverDead.load(std::memory_order_relaxed)) {
      const int64_t now = monotonicMs();
      if (s.heartbeat != g_lastHb.load(std::memory_order_relaxed)) {
        g_lastHb.store(s.heartbeat, std::memory_order_relaxed);
        g_lastHbChangeMs.store(now, std::memory_order_relaxed);
      } else if (now - g_lastHbChangeMs.load(std::memory_order_relaxed) > kServerDeadMs) {
        g_serverDead.store(true, std::memory_order_release);
      }
    }
  }

}

/* --- PROF-line wire stats (declared in mtl_profiler.h) ---------------------
 * Called once per 256-frame profiler window; pulls the cumulative counters
 * via the WireRing_stats thunk and reports window deltas (decodeErr stays
 * cumulative — it must remain 0). hb is the server heartbeat delta over the
 * window (~100 Hz while the server lives; 0 = no server / dead server);
 * srvErr is the server's cumulative status error code (0 = ok).
 * S2 additions: srvFrames = presentFrame records the server EXECUTED this
 * window (should track the local present rate); srvPace = cumulative
 * presents where the server's frame serial was >= local-1 at present time;
 * srvExecErr = the server's cumulative executor error count. */
namespace dxvk::backend::prof {

  void wireAppendProfStats(std::string& line) {
    WMTWireStats s = { };
    if (g_wireAttached.load(std::memory_order_acquire))
      WireRing_stats(&s);
    static WMTWireStats prev = { };
    /* S3 v3: a re-attach (device churn) restarts the counters — reset the
     * delta baseline instead of printing underflowed windows */
    if (s.records < prev.records)
      prev = WMTWireStats();
    line += str::format(" | wire: rec=", s.records - prev.records,
      " bytes=", s.bytes - prev.bytes,
      " stalls=", s.stalls - prev.stalls,
      " decodeErr=", s.decode_errors,
      " hb=", s.heartbeat - prev.heartbeat,
      " srvErr=", s.server_error,
      " srvFrames=", s.server_frames - prev.server_frames,
      " srvPace=", g_srvPace.load(std::memory_order_relaxed),
      " srvExecErr=", s.server_exec_errors,
      " psoSkip=", s.server_skipped_pso - prev.server_skipped_pso,
      " upl=", s.server_uploads - prev.server_uploads,
      " heapDrop=", s.heap_dropped);
    prev = s;
  }

}
