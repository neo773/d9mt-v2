#include "mtl_context_impl.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

#include "../util/log/log.h"
#include "../util/util_string.h"

#include "mtl_profiler.h"

/*
 * METAL 4 backend (SOURCE_OF_TRUTH §2). Implementation notes live in
 * mtl_context_impl.h (the 9 architecture decisions). Section order mirrors the
 * header's flow: lifecycle -> pass -> state -> binding -> draw. Part 2
 * (resolve/arg-table flush/dynamic state/transfers/queries) is in impl2.cpp.
 */

namespace dxvk::backend {

  namespace {
    WMTPrimitiveType wmtPrimitive(MtlPrimitiveTopology t) {
      switch (t) {
        case MtlPrimitiveTopology::PointList:     return WMTPrimitiveTypePoint;
        case MtlPrimitiveTopology::LineList:      return WMTPrimitiveTypeLine;
        case MtlPrimitiveTopology::LineStrip:     return WMTPrimitiveTypeLineStrip;
        case MtlPrimitiveTopology::TriangleStrip: return WMTPrimitiveTypeTriangleStrip;
        default:
        case MtlPrimitiveTopology::TriangleList:  return WMTPrimitiveTypeTriangle;
      }
    }
  }

  MtlContextImpl::MtlContextImpl(Rc<MtlDeviceImpl> device)
  : MtlContext(device), m_device(std::move(device)),
    m_psoCache(m_device->wmtCompiler()) {
    obj_handle_t dev = m_device->wmtDevice();

    /* Per-stage argument tables (decision 3). max_buffers >= 21 (push data at
     * [[buffer(20)]]); textures/samplers 16 = D3D9 sampler slot count.
     * Metal 4 validation: "max buffer bind count must not be more than 31". */
    WMTArgTableInfo ati = {};
    ati.max_buffers                = 31u;
    ati.max_textures               = 16u;
    ati.max_samplers               = 16u;
    ati.initialize_bindings        = 1u;
    ati.support_attribute_strides  = 1u;
    ArgTablePair pair;
    pair.table[0] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));
    pair.table[1] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));
    m_argTables.push_back(std::move(pair));

    /* Residency: owned entirely by the device's MtlResidencyTracker
     * (queue-attached set; nothing to bind per cmdbuf). The context only
     * drives beforeSubmit()/serialRetired() from submit()/endFrame(). */

    /* Allocator ring seed: slot 0 exists so pre-frame resource-init cmdbufs
     * (before the first beginRecording) have backing memory. beginRecording
     * rotates to slot 1 next. */
    m_allocators[0] = WmtAllocator::adopt(MTLDevice_newCommandAllocator(dev));
    m_allocator     = m_allocators[0].handle();
    m_frameIndex    = 1u;
  }

  MtlContextImpl::~MtlContextImpl() {
    if (m_pass.active)
      closePass();
    /* Context-lifetime visibility buffer: registered at creation, leaves
     * the residency set through the same deferred-destruction path as
     * every other allocation. */
    if (m_visBuffer) {
      m_device->residency().allocDestroyed(m_visBuffer.detach(), nullptr,
        m_device->submissions().nextSerial());
    }
  }

  // --- lifecycle -----------------------------------------------------------

  void MtlContextImpl::beginRecording() {
    /* PROF: allocator rotation + the waitFor on the slot's last serial — a
     * large bucket here means the CPU is waiting on the GPU (pacing), not
     * that this function does work. */
    prof::Scope profScope(prof::BeginRecording);
    m_dirty = DirtyEverything;
    /* NOTE: do NOT reset m_argTableCursor here. The argument-table pool is a
     * cross-frame ring (selectArgumentTablesForPass); resetting the cursor per
     * recording broke reuse and ratcheted the pool. It wraps mod size itself. */

    /* Rotate to a >= ring-depth-old allocator and reset it. MTL4CommandAllocator
     * contract: every command buffer whose memory came from this allocator must
     * have COMPLETED before reset — with >ring-depth frames in flight (shader
     * compile stalls) an ungated reset recycles command memory the GPU is still
     * reading (command-stream corruption). Wait for the slot's last serial. */
    uint32_t slot = m_frameIndex % uint32_t(m_allocators.size());
    m_frameIndex++;
    if (!m_allocators[slot]) {
      m_allocators[slot] = WmtAllocator::adopt(
        MTLDevice_newCommandAllocator(m_device->wmtDevice()));
    } else {
      /* MTL4CommandAllocator contract: the slot's prior command buffers must
       * have COMPLETED before reset. Block on the slot's last serial (a real
       * wait, not a log) or reset corrupts command memory the GPU is reading. */
      if (m_device->submissions().completedSerial() < m_allocatorSerial[slot])
        m_device->submissions().waitFor(m_allocatorSerial[slot]);
      MTL4CommandAllocator_reset(m_allocators[slot].handle());
    }
    m_allocator = m_allocators[slot].handle();
    m_allocatorSlot = slot;

    /* NO residency work here. Allocations are resident from creation until
     * their deferred destruction retires (MtlResidencyTracker) — any
     * per-frame set rebuild drops bound-but-not-re-touched resources and
     * faults the GPU ("BIF0 page fault"). */

    ensureCommandBuffer();
  }

  void MtlContextImpl::ensureCommandBuffer() {
    if (m_cmdbuf)
      return;

    m_cmdbuf = WmtCommandBuffer::adopt(
      MTLDevice_newMTL4CommandBuffer(m_device->wmtDevice()));
    MTL4CommandBuffer_beginCommandBuffer(m_cmdbuf.handle(), m_allocator);
    /* Residency sets are queue-attached (MtlResidencyTracker) — no
     * per-cmdbuf useResidencySet. */
    m_currentSerial = m_device->submissions().nextSerial();
  }

  /* D9MT_GPUCAPTURE (with MTL_CAPTURE_ENABLED=1): programmatic
   * MTLCaptureManager GPU trace to /tmp/d9mt.gputrace. Diagnostic only —
   * wine processes don't register with gputoolsserviced, so the gpucapture
   * CLI can't attach to them. Two modes:
   *   D9MT_GPUCAPTURE=N        capture the first N queue commits
   *   D9MT_GPUCAPTURE=trigger  armed: touching C:\d9mt-test\GPUCAP starts a
   *                            capture of the next 3 commits (repeatable) —
   *                            lets us grab a mid-game frame on demand. */
  static int  s_gpuCaptureRemaining = -2;  /* -2 env unread, -1 off, 0 armed */
  static bool s_gpuCaptureTrigger   = false;
  static int  s_gpuCaptureCommits   = 3;

  static void gpuCaptureStart(obj_handle_t device, int commits) {
    obj_handle_t mgr = MTLCaptureManager_sharedCaptureManager();
    WMTCaptureInfo ci = {};
    ci.capture_object = device;
    ci.destination = WMTCaptureDestinationGPUTraceDocument;
    ci.output_url.set("/tmp/d9mt.gputrace");
    bool ok = MTLCaptureManager_startCapture(mgr, &ci);
    s_gpuCaptureRemaining = commits;
    Logger::info(str::format("D9MT-GPUCAPTURE start ok=", ok,
      " commits=", commits, " -> /tmp/d9mt.gputrace"));
  }

  static void gpuCaptureBeforeCommit(obj_handle_t device) {
    if (s_gpuCaptureRemaining == -2) {
      const char* v = std::getenv("D9MT_GPUCAPTURE");
      if (v && std::strcmp(v, "trigger") == 0) {
        s_gpuCaptureTrigger = true;
        s_gpuCaptureRemaining = 0;
        const char* n = std::getenv("D9MT_GPUCAPTURE_COMMITS");
        if (n && *n && std::atoi(n) > 0)
          s_gpuCaptureCommits = std::atoi(n);
        Logger::info(str::format("D9MT-GPUCAPTURE armed (touch C:\\d9mt-test\\GPUCAP, ",
          s_gpuCaptureCommits, " commits)"));
      } else {
        s_gpuCaptureRemaining = (v && *v) ? std::atoi(v) : -1;
        if (s_gpuCaptureRemaining > 0)
          gpuCaptureStart(device, s_gpuCaptureRemaining);
      }
      return;
    }
    if (s_gpuCaptureTrigger && s_gpuCaptureRemaining == 0) {
      std::FILE* f = std::fopen("C:\\d9mt-test\\GPUCAP", "rb");
      if (f) {
        std::fclose(f);
        std::remove("C:\\d9mt-test\\GPUCAP");
        Logger::info("D9MT-GPUCAPTURE trigger hit — starting capture");
        gpuCaptureStart(device, s_gpuCaptureCommits);
        Logger::info("D9MT-GPUCAPTURE startCapture returned");
      }
    }
  }

  static void gpuCaptureAfterCommit() {
    if (s_gpuCaptureRemaining > 0 && --s_gpuCaptureRemaining == 0) {
      MTLCaptureManager_stopCapture(MTLCaptureManager_sharedCaptureManager());
      Logger::info("D9MT-GPUCAPTURE stopped");
    }
  }

  void MtlContextImpl::submit(bool waitCompleted) {
    prof::Scope profScope(prof::Submit);
    if (m_pass.active) {
      prof::bump(prof::CntPassCloseSubmit);
      closePass();
    }

    /* Decision 10: replay the parked frame list into this command buffer
     * BEFORE endCommandBuffer/commit. Every consumer of GPU-ordered results
     * (readback waits, query polls, staging fences, Reset, present) funnels
     * through this submit, so nothing can observe un-replayed work. No-op in
     * immediate mode / when nothing was parked. */
    flushFramePasses();

    if (!m_cmdbuf)
      return;

    MTL4CommandBuffer_endCommandBuffer(m_cmdbuf.handle());

    /* Realize pending residency adds before the queue sees this cmdbuf
     * (batched in the tracker; commits only when dirty). */
    m_device->residency().beforeSubmit();

    /* One commit per command buffer, DELIBERATELY unbatched (the bridge
     * signature takes an array). Every intra-frame submit is driven by
     * D3D9DeviceEx::ExecuteFlush, which signals the submission and
     * staging-buffer fences under this cmdbuf's serial immediately before
     * flushing — app threads block on those fences (flush throttle, staging
     * budget) and on query polls (MtlBackendQuery::getData) keyed to this
     * serial. Deferring the commit to a later flush point would turn each of
     * those waits into a stall bounded only by whoever eventually flushes
     * (the retirement pump has no commit hook), a mid-frame latency
     * regression that dwarfs the few saved queue crossings per frame. */
    obj_handle_t cb = m_cmdbuf.handle();
    gpuCaptureBeforeCommit(m_device->wmtDevice());
    MTL4CommandQueue_commit(m_device->wmtQueue(), &cb, 1u);
    gpuCaptureAfterCommit();

    /* MTL4: the completion signal is a queue-timeline op AFTER commit. */
    uint64_t serial = m_device->submissions().onSubmit(m_device->wmtQueue());
    /* Every submission recorded from the current allocator slot pins that
     * slot until this serial retires (see beginRecording reset gate). */
    m_allocatorSerial[m_allocatorSlot] = serial;

    m_device->uploadRing().onSubmit(serial);

    /* keep-alive set rides the retirement spine (decision 5) */
    if (!m_keepAlive.empty()) {
      auto keepAlive = std::make_shared<std::vector<Rc<MtlResource>>>(
        std::move(m_keepAlive));
      for (auto& res : *keepAlive)
        res->setTrackingSerial(serial);
      m_device->submissions().retireAfter(serial,
        [keepAlive]() mutable { keepAlive->clear(); });
      m_keepAlive.clear();
    }

    /* The committed MTL4CommandBuffer is released only after it retires (the
     * queue holds its own reference for the duration of GPU execution). */
    auto doneCmd = std::make_shared<WmtCommandBuffer>(std::move(m_cmdbuf));
    m_device->submissions().retireAfter(serial,
      [doneCmd]() mutable { doneCmd->reset(); });

    if (waitCompleted)
      m_device->submissions().waitFor(serial);

    m_cmdbuf.reset();
    m_dirty = DirtyEverything;

    /* Reclaim completed work on every submission, not only at endFrame:
     * load phases submit without presenting for long stretches, and memory
     * (retired backings, ring chunks, residency entries) must retire on the
     * submission cadence or it piles up until present resumes. */
    const uint64_t completed = m_device->submissions().completedSerial();
    m_device->submissions().processRetirements();
    m_device->residency().serialRetired(completed);
    m_device->uploadRing().onComplete(completed);
  }

  void MtlContextImpl::flushCommandList(const char* reason, MtlSubmitStatus* status) {
    submit(false);
    /* beginRecording (NOT ensureCommandBuffer) rotates the 3-deep command-
     * allocator ring: it waits for the slot's prior GPU work to retire before
     * reusing it (CS<->GPU pacing, caps frames in flight) and resets the
     * allocator's command memory. Calling ensureCommandBuffer here instead left
     * beginRecording running only once at device creation, so the CS thread
     * submitted unbounded frames ahead of the GPU and allocator/retirement
     * memory piled up until it froze/OOM'd after ~30s. */
    beginRecording();
    if (status) {
      status->submissionId = m_currentSerial;
      status->completed    = false;
    }
  }

  void MtlContextImpl::endFrame() {
    /* Frame-boundary marker for Rule B (decision 10): the frontend emits
     * endFrame() from PresentImage immediately BEFORE the present flush
     * (EndFrame -> Flush -> present blit -> synchronizeWsi, all in CS
     * order), so the very next flushFramePasses carries the batch that ends
     * at the Present boundary — the one place D3D9 declares depth/stencil
     * contents undefined and their final store may be discarded. */
    m_frameBoundaryFlush = true;

    uint64_t completed = m_device->submissions().completedSerial();
    {
      prof::Scope profScope(prof::EndFrame);
      /* Retirements drop the last Rcs (allocations enqueue their deferred
       * removal), then the tracker flushes removals whose serial completed. */
      m_device->submissions().processRetirements();
      m_device->residency().serialRetired(completed);
      m_device->uploadRing().onComplete(completed);
    }
    prof::onFrameEnd();

    /* DIAG: per-frame growth watch (leak instrumentation; observers only).
     * bufNew/bufRename/bufRecycle/imgNew attribute residency churn to its
     * creation source (cumulative device counters). */
    static uint64_t s_diagFrame = 0u;
    if ((++s_diagFrame & 0xFFu) == 0u) {
      Logger::info(str::format("DIAG frame=", s_diagFrame,
        " residencyNet=", m_device->residency().netResident(),
        " adds=", m_device->residency().totalAdds(),
        " removes=", m_device->residency().totalRemoves(),
        " pending=", m_device->residency().pendingRemovals(),
        " argTables=", m_argTables.size(),
        " retirements=", m_device->submissions().retirementCount(),
        " serial=", m_device->submissions().lastSubmittedSerial(),
        " completed=", completed,
        " bufNew=", m_device->diagBuffersCreated(),
        " bufRename=", m_device->diagBuffersRenamed(),
        " bufRecycle=", m_device->diagBuffersRecycled(),
        " imgNew=", m_device->diagImagesCreated(),
        " vbStale=", s_diagVbStale,
        " cbvStale=", s_diagCbvStale));
    }
  }

  void MtlContextImpl::synchronizeWsi(MtlPresenterSync sync) {
    /* PROF: contains one Submit (the present commit) — read Submit as a
     * sub-bucket of SynchronizeWsi on present frames. */
    prof::Scope profScope(prof::SynchronizeWsi);
    /* Contract 3e + METAL 4 present handshake (decision 5): waitForDrawable
     * BEFORE commit, signalDrawable + drawable present AFTER commit. The
     * swapchain acquires the drawable, records the backbuffer blit into it,
     * then hands it here via sync.drawable — this is the ONLY place the
     * drawable is actually presented to the CAMetalLayer. */
    if (sync.drawable == MtlNullHandle) {
      Logger::warn("MtlContextImpl: synchronizeWsi without drawable");
      return;
    }

    if (m_pass.active) {
      prof::bump(prof::CntPassCloseSubmit);
      closePass();
    }
    ensureCommandBuffer();

    /* METAL 4 present handshake, all four steps: queue waitForDrawable BEFORE
     * the commit that writes the drawable (GPU-side wait for the display to
     * release the texture — without it the present copy scribbles into a
     * drawable mid-scanout: visible tearing), then commit, then
     * signalDrawable + present. The old forever-block fear dated from the
     * drawable-leak era (drawables pinned in the residency set were never
     * released by CA, so the wait could never satisfy); drawable lifecycle
     * is sound now — acquire/copy/present/release on one thread, layer-owned
     * residency set. */
    MTL4CommandQueue_waitForDrawable(m_device->wmtQueue(), sync.drawable);
    submit(false);

    MTL4CommandQueue_signalDrawable(m_device->wmtQueue(), sync.drawable);
    MetalDrawable_present(sync.drawable);
    /* Balance the retain taken in the bridge's nextDrawable — after present
     * nothing touches the drawable again. Acquire/present/release now all run
     * on the CS thread (see D3D9SwapChainEx::PresentImage), so no drawable is
     * ever held across a thread boundary. */
    NSObject_release(sync.drawable);
  }

  // --- render pass (decision 1) ---------------------------------------------

  static bool sameAttachment(const Rc<MtlTextureView>& a,
                             const Rc<MtlTextureView>& b);

  void MtlContextImpl::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
    /* Redundant framebuffer rebind: the frontend re-binds on ANY FB-dirty
     * flag (present, state restores, SetRenderTarget with the same target),
     * and most of those name the SAME attachments as the open pass. Closing
     * and reopening an identical pass is a full TBDR tile store+load for
     * nothing — measured 52 of ~83 pass closes/frame. Keep the encoder.
     * Identity = same subresource AND same view format (an sRGB re-view of
     * the same texture is a different attachment to the PSO). */
    /* EXPERIMENTAL, default OFF (D9MT_RTSKIP=1 enables): keeping the pass
     * open across a same-target rebind cut passes 83 -> 57/frame, but a
     * display-mode Reset with the skip active leaves the game wedged after
     * the switch (bisect-confirmed; the held pass straddles backbuffer
     * destruction). Re-enable once the Reset path force-closes the pass. */
    static const bool rtSkipOn = std::getenv("D9MT_RTSKIP") != nullptr;
    if (m_pass.active) {
      if (rtSkipOn) {
        auto sameRt = [](const Rc<MtlTextureView>& a, const Rc<MtlTextureView>& b) {
          if (!sameAttachment(a, b))
            return false;
          return a == nullptr || b == nullptr || a->key.format == b->key.format;
        };
        bool same = sameRt(m_pass.targets.depthStencil, targets.depthStencil);
        for (uint32_t i = 0u; same && i < MtlRenderTargets::MaxColorTargets; i++)
          same = sameRt(m_pass.targets.color[i], targets.color[i]);
        if (same)
          return;
      }
      prof::bump(prof::CntPassCloseRt);
      closePass();
    }
    else
      flushPendingClears();
    /* A folded, not-yet-drawn clear must EXECUTE before the pending pass is
     * replaced — silently discarding it turned "Clear; (framebuffer
     * re-dirtied); draw" into load=Load on stale content: every frame's draws
     * accumulated on the previous frame's (the RT-feedback clear-drop bug,
     * formerly papered over by unbinding sampled RTs in SetRenderTarget,
     * which broke D3D9 sampler-state persistence instead). */

    m_pass = PendingPass();
    m_pass.targets = std::move(targets);
    m_pass.pending = true;
    m_dirty |= DirtyPso | DirtyViewport;
  }

  /* Two MtlTextureViews name the SAME render-pass attachment when they view the
   * same subresource of the same texture — NOT when they are the same object.
   * createImageView() interns nothing (a fresh view per call), so comparing the
   * Rc pointers here only "worked" by heap-address reuse and silently degraded
   * to loadAction=Load on frames where the SetRenderTarget view and the Clear
   * view landed at different addresses — dropping the clear (RT accumulation /
   * feedback garbage; GTA-shaped). Compare the backing texture + subresource. */
  static bool sameAttachment(const Rc<MtlTextureView>& a,
                             const Rc<MtlTextureView>& b) {
    if (a == b)                     return true;
    if (a == nullptr || b == nullptr) return false;
    return a->image.ptr()  == b->image.ptr()
        && a->key.baseMip  == b->key.baseMip
        && a->key.baseLayer == b->key.baseLayer;
  }

  void MtlContextImpl::clearRenderTarget(
    const Rc<MtlTextureView>& view, MtlAspectMask clear,
    MtlClearValue value, MtlAspectMask discard) {
    if (d9mtTrace() && (clear & MtlAspectColor))
      Logger::info(str::format("CLR color img=",
        (void*)(view ? view->image.ptr() : nullptr),
        " rgba=", value.color.r, ",", value.color.g, ",", value.color.b));
    /* DIAGNOSTIC: D9MT_NOCOLORCLEAR drops COLOR clears (keeps depth/stencil) so
     * the backbuffer is preserved (loadAction=Load). Tests whether GTA's 2D is
     * accumulated across frames (would compose the full loading image) vs
     * redrawn per frame (shader/transform bug). */
    if (std::getenv("D9MT_NOCOLORCLEAR") && (clear & MtlAspectColor)) {
      clear = MtlAspectMask(clear & ~MtlAspectColor);
      if (!clear) return;
    }
    /* Fold into the pending pass when the view is one of its attachments
     * and the encoder hasn't opened; else flush as a mini-pass. */
    if (!m_pass.active && m_pass.pending) {
      for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
        if (sameAttachment(m_pass.targets.color[i], view) && (clear & MtlAspectColor)) {
          m_pass.clearColor[i] = true;
          m_pass.clearValues[i] = value;
          return;
        }
      }
      if (sameAttachment(m_pass.targets.depthStencil, view)) {
        if (clear & MtlAspectDepth)   { m_pass.clearDepth = true;   m_pass.depthClear = value.depth; }
        if (clear & MtlAspectStencil) { m_pass.clearStencil = true; m_pass.stencilClear = value.stencil; }
        return;
      }
    }

    /* standalone clear: dedicated mini-pass with load=clear, store=store */
    /* DEFERRED MODE: an ACTIVE recording pass owns m_cmdChunk — the swap
     * dance below would otherwise let the mini-pass's recordPassEntry steal
     * the active pass's parked draws. Close (park) the active pass first;
     * the clear entry then records in true D3D9 order (prior draws, then
     * clear, then later draws in a fresh pass). */
    if (deferredPassesEnabled() && m_pass.active) {
      prof::bump(prof::CntPassCloseClear);
      closePass();
    }
    PendingPass saved = std::move(m_pass);
    m_pass = PendingPass();
    if (clear & MtlAspectColor) {
      m_pass.targets.color[0] = view;
      m_pass.clearColor[0] = true;
      m_pass.clearValues[0] = value;
    } else {
      m_pass.targets.depthStencil = view;
      if (clear & MtlAspectDepth)   { m_pass.clearDepth = true;   m_pass.depthClear = value.depth; }
      if (clear & MtlAspectStencil) { m_pass.clearStencil = true; m_pass.stencilClear = value.stencil; }
    }
    m_pass.pending = true;
    prof::bump(prof::CntPassCloseClear);
    materializePass();
    closePass();
    m_pass = std::move(saved);
  }

  void MtlContextImpl::flushPendingClears() {
    /* Fold-in clears (clearRenderTarget on a bound-but-unopened pass) only
     * execute when the pass materializes at a draw. A copy/readback (or the
     * present blit) that arrives with no intervening draw must still observe
     * the clear — materialize an empty load=Clear/store=Store pass now, else
     * the clear is silently dropped (clear-only frames read back stale/black,
     * wine test_sanity). */
    if (m_pass.active || !m_pass.pending)
      return;
    bool any = m_pass.clearDepth || m_pass.clearStencil;
    for (bool c : m_pass.clearColor)
      any = any || c;
    if (!any)
      return;
    prof::bump(prof::CntPassCloseClear);
    materializePass();
    closePass();
  }

  void MtlContextImpl::materializePass() {
    if (m_pass.active)
      return;

    ensureCommandBuffer();

    /* Decision 10 (deferred): open a RECORDING pass, not an encoder. All
     * per-pass encoder-scoped state behaves identically because every write
     * rides the CmdChunk, not the encoder: the chunk replays into the real
     * encoder at flushFramePasses. Folded clears stay on m_pass and are
     * captured by recordPassEntry. */
    if (deferredPassesEnabled()) {
      m_pass.encoder = 0;
      m_pass.active  = true;
      m_pass.pending = false;
      m_pass.encodedDraws = 0u;
      m_dirty = DirtyEverything;
      m_argTablesBound = false;
      m_lastEncodedPso = 0u;
      m_dynValid       = false;
      return;
    }

    WMTRenderPassInfo info = {};
    /* Occlusion queries write into a per-context visibility buffer bound at
     * pass creation (decision 7). */
    info.visibility_buffer = m_visBuffer.handle();
    for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
      const auto& view = m_pass.targets.color[i];
      if (view == nullptr)
        continue;
      auto* impl = static_cast<MtlTextureViewImpl*>(view.ptr());
      info.colors[i].texture = impl->owned.handle();
      info.colors[i].load_action = m_pass.clearColor[i]
        ? WMTLoadActionClear : WMTLoadActionLoad;
      info.colors[i].store_action = WMTStoreActionStore;
      if (d9mtTrace() && i == 0)
        Logger::info(str::format("MAT color img=", (void*)impl->image.ptr(),
          " load=", m_pass.clearColor[i] ? "CLEAR" : "LOAD"));
      if (m_pass.clearColor[i]) {
        const auto& v = m_pass.clearValues[i];
        info.colors[i].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
      }
    }

    if (m_pass.targets.depthStencil != nullptr) {
      auto* impl = static_cast<MtlTextureViewImpl*>(m_pass.targets.depthStencil.ptr());
      info.depth.texture = impl->owned.handle();
      info.depth.load_action = m_pass.clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.clear_depth = m_pass.depthClear;
      const auto* fmtInfo = lookupFormatInfo(impl->key.format);
      if (fmtInfo->flags & MtlFormatFlagStencil) {
        info.stencil.texture = impl->owned.handle();
        info.stencil.load_action = m_pass.clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
        info.stencil.store_action = WMTStoreActionStore;
        info.stencil.clear_stencil = m_pass.stencilClear;
      }
    }

    m_pass.encoder = MTL4CommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
    m_pass.active = true;
    m_pass.pending = false;
    m_dirty = DirtyEverything;
    /* Fresh encoder: encoder-level state (incl. setArgumentTable) is gone —
     * the first draw of this pass must rotate in a retired pair and re-bind
     * it (prepareDraw). Encoder-scoped trim shadows reset with it. */
    m_argTablesBound = false;
    m_lastEncodedPso = 0u;
    m_dynValid       = false;

  }

  MtlContextImpl::ArgTablePair MtlContextImpl::createArgumentTablePair() const {
    WMTArgTableInfo ati = {};
    ati.max_buffers                = 31u;
    ati.max_textures               = 16u;
    ati.max_samplers               = 16u;
    ati.initialize_bindings        = 1u;
    ati.support_attribute_strides  = 1u;

    ArgTablePair pair;
    pair.table[0] = WmtArgTable::adopt(MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    pair.table[1] = WmtArgTable::adopt(MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    return pair;
  }

  void MtlContextImpl::selectArgumentTablesForPass() {
    /* Once per materialized pass, NOT per draw: Metal snapshots an argument
     * table's contents at each encoded draw (MTL4RenderCommandEncoder.h,
     * setArgumentTable:atStages: — "Metal takes a snapshot of the resources
     * in the argument table when you encode a draw"), so mutate-draw-mutate-
     * draw on the ONE selected pair is well-defined for the whole encoder.
     * The old per-draw rotation neutered the ArgTableShadow diff: every draw
     * rotated onto a cold pair and re-wrote all ~48x2 slots (argRotate ==
     * draws, prepareDraw ~11ms/frame in GTA). Keeping the pair for the pass
     * makes the diff write only the slots that actually change per draw.
     *
     * Rotation still matters at the pass boundary: round-robin the pool,
     * reusing a slot only once its last-use submission serial has RETIRED
     * (the same retire-after-serial discipline as the command-allocator ring
     * / upload ring) so a pair is never rewritten while an earlier
     * submission's GPU work is still in flight against it. Within one cmdbuf
     * two passes can never share a pair either: the pass-start stamp below
     * uses the UNSUBMITTED m_currentSerial, which cannot compare <= completed.
     * If every slot is still in flight, the pool is too small for the current
     * in-flight pass depth, so grow it right here. This self-limits: once the
     * pool covers the peak concurrent in-flight passes it stops growing —
     * bounded by construction. */
    prof::bump(prof::CntArgRotate);
    const uint64_t completed = m_device->submissions().completedSerial();
    const size_t   n         = m_argTables.size();

    /* Round-robin from the cursor and reuse the FIRST table whose last-use
     * serial has retired. Only when every table is still in flight (pool too
     * small for the current in-flight draw depth) do we grow — by push_back at
     * the end, NEVER insert-at-cursor. The prior insert-at-cursor scrambled the
     * per-slot serials so the round-robin never landed on a retired slot: the
     * pool ratcheted up ~4 tables/frame forever (never shrank) and exhausted
     * memory after ~2200 frames. A stable ring self-limits to the peak
     * concurrent in-flight draw count and then stops growing. */
    for (size_t scan = 0u; scan < n; ++scan) {
      const size_t idx = (m_argTableCursor + scan) % n;
      if (m_argTables[idx].serial <= completed) {
        m_currentArgTable       = idx;
        m_argTableCursor        = idx + 1u;
        m_argTables[idx].serial = m_currentSerial;
        return;
      }
    }

    m_argTables.push_back(createArgumentTablePair());
    m_currentArgTable       = m_argTables.size() - 1u;
    m_argTableCursor        = m_currentArgTable + 1u;
    m_argTables[m_currentArgTable].serial = m_currentSerial;
  }

  void MtlContextImpl::bindArgumentTables() {
    ArgTablePair& pair = m_argTables[m_currentArgTable];
    auto* vs = m_cmdChunk.append<wmtcmd_mtl4_setargtable>(
      uint16_t(WMTMtl4RenderCommandSetArgumentTable));
    vs->table  = pair.table[0].handle();
    vs->stages = uint64_t(WMTRenderStageVertex);
    auto* fs = m_cmdChunk.append<wmtcmd_mtl4_setargtable>(
      uint16_t(WMTMtl4RenderCommandSetArgumentTable));
    fs->table  = pair.table[1].handle();
    fs->stages = uint64_t(WMTRenderStageFragment);
  }

  void MtlContextImpl::flushCmdChunk() {
    if (m_cmdChunk.empty())
      return;
    /* Deferred recording has no live encoder — the chunk is consumed by
     * recordPassEntry and replayed at flushFramePasses instead. Nothing
     * should call this without an encoder; guard rather than crash. */
    if (!m_pass.encoder)
      return;
    prof::Scope profScope(prof::FlushCmdChunk);
    prof::bump(prof::CntChunkFlush);
    /* ONE crossing replays the whole batch natively (mtl_cmd_chunk.h /
     * winemetal.h code 208). The unix side consumes the structs
     * synchronously, so the arena recycles immediately — no GPU-lifetime
     * tracking (unlike UploadRing memory, which the GPU reads later). */
    MTL4RenderCommandEncoder_encodeCommands(m_pass.encoder, m_cmdChunk.head());
    m_cmdChunk.reset();
  }

  void MtlContextImpl::closePass() {
    if (!m_pass.active)
      return;

    /* Decision 10 (deferred): no encoder exists yet — park the pass. */
    if (deferredPassesEnabled()) {
      recordPassEntry();
      return;
    }

    /* Replay every batched wmtcmd for this pass before the encoder is
     * barriered/ended — the chunk invariant (non-empty only while a pass is
     * open) holds because appends happen only after materializePass. */
    flushCmdChunk();

    /* §4c: order this pass's fragment (color/depth) writes ahead of every
     * later queue stage — a subsequent encoder that samples this RT as a
     * texture, or copies it (present blit / gated readback), must observe the
     * finished pixels. Metal 4 does no implicit render-pass transition. */
    emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
    MTLCommandEncoder_endEncoding(m_pass.encoder);
    NSObject_release(m_pass.encoder);
    m_pass.encoder = 0;
    m_pass.active = false;
    m_pass.pending = true;   /* same targets stay bound until rebound */
    m_argTablesBound = false; /* encoder gone; next pass rotates + re-binds */
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }

  // --- state setters (fold + dirty) ------------------------------------------

  /* Redundant-state guards (perf): D3D9 titles re-set identical state
   * constantly; an unguarded setter turns each into a dirty bit and a
   * per-draw resolve/flush cascade downstream (resolvePso key rebuilds were
   * ~per draw in GTA). Skipping the no-change case here kills the whole
   * chain at the cheapest point. */

  void MtlContextImpl::bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) {
    Rc<MtlShaderFunction>& slot = (stage == MtlShaderStage::Vertex ? m_vs : m_fs);
    if (slot == fn)
      return;
    slot = std::move(fn);
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setVertexLayout(
    uint32_t nAttr, const MtlVertexAttr* attrs,
    uint32_t nLayout, const MtlVertexLayout* layouts) {
    if (m_vertexAttrs.size() == nAttr && m_vertexLayouts.size() == nLayout
     && std::memcmp(m_vertexAttrs.data(), attrs, nAttr * sizeof(MtlVertexAttr)) == 0
     && std::memcmp(m_vertexLayouts.data(), layouts, nLayout * sizeof(MtlVertexLayout)) == 0)
      return;
    m_vertexAttrs.assign(attrs, attrs + nAttr);
    m_vertexLayouts.assign(layouts, layouts + nLayout);
    m_layoutGen++;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setPrimitiveTopology(MtlPrimitiveTopology topology) {
    if (m_topology == topology)
      return;
    m_topology = topology;   /* PSO topology CLASS only changes across point/line/tri */
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setViewports(uint32_t n, const MtlViewport* viewports) {
    if (!n)
      return;
    if (std::memcmp(&m_viewport, &viewports[0], sizeof(MtlViewport)) == 0)
      return;
    m_viewport = viewports[0];
    m_dirty |= DirtyViewport;
  }

  void MtlContextImpl::setRasterizerState(const MtlRasterizerState& rs) {
    if (std::memcmp(&m_rasterizer, &rs, sizeof(rs)) == 0)
      return;
    m_rasterizer = rs;
    m_dirty |= DirtyRasterizer;
  }

  void MtlContextImpl::setMultisampleState(const MtlMultisampleState& ms) {
    if (std::memcmp(&m_msaa, &ms, sizeof(ms)) == 0)
      return;
    m_msaa = ms;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setDepthStencilState(const MtlDepthStencilState& ds) {
    if (std::memcmp(&m_depthStencil, &ds, sizeof(ds)) == 0)
      return;
    m_depthStencil = ds;
    m_dirty |= DirtyDepthStencil;
  }

  void MtlContextImpl::setBlendMode(uint32_t rt, const MtlBlendMode& blend) {
    if (rt >= 4)
      return;
    if (std::memcmp(&m_blend[rt], &blend, sizeof(blend)) == 0)
      return;
    m_blend[rt] = blend;
    m_blendGen++;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setBlendColor(MtlColor4 color) {
    if (std::memcmp(&m_blendColor, &color, sizeof(color)) == 0)
      return;
    m_blendColor = color;
    m_dirty |= DirtyBlendColor;
  }

  void MtlContextImpl::setStencilReference(uint32_t reference) {
    if (m_stencilRef == reference)
      return;
    m_stencilRef = reference;
    m_dirty |= DirtyStencilRef;
  }

  void MtlContextImpl::setDepthBias(MtlDepthBias bias) {
    if (std::memcmp(&m_depthBias, &bias, sizeof(bias)) == 0)
      return;
    m_depthBias = bias;
    m_dirty |= DirtyDepthBias;
  }

  void MtlContextImpl::setDepthBounds(MtlDepthBounds bounds) {
    /* §6.3: accepted + no-op */
  }

  // --- binding ---------------------------------------------------------------

  void MtlContextImpl::bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) {
    auto& stream = m_vertexStreams[binding];
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;               /* §6.4 stride participates in PSO key */
    }
    stream.slice = std::move(slice);
    stream.stride = stride;
    m_dirtyVertexStreams |= 1u << binding;
    m_dirty |= DirtyVertexBuffers;
  }

  void MtlContextImpl::bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) {
    /* SetStreamSource fast path: same logical buffer, new offset/stride
     * (D3D9 dynamic-VB append pattern — one bind, per-element offsets).
     * Rebase within the slice's captured backing; a Discard rename between
     * range updates retargets the binding through invalidateBuffer's
     * CS-ordered refresh, same as the full-bind path. */
    auto& stream = m_vertexStreams[binding];
    if (!stream.slice.defined())
      return;                            /* range update with nothing bound */
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;               /* §6.4 stride participates in PSO key */
    }
    stream.slice = MtlBufferSlice(stream.slice.buffer(), stream.slice.storage(),
                                  offset, length);
    stream.stride = stride;
    m_dirtyVertexStreams |= 1u << binding;
    m_dirty |= DirtyVertexBuffers;
  }

  void MtlContextImpl::bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) {
    m_indexBuffer = std::move(slice);
    m_indexType = type;
    m_dirty |= DirtyIndexBuffer;
  }

  void MtlContextImpl::bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& cbv = m_bindings[s].cbvs[slot];
        cbv.slice = slice;
        cbv.rangeOffset = 0;
        cbv.rangeLength = slice.length();
        m_bindings[s].dirtyCbvs |= 1u << slot;
      }
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindUniformBufferRange(MtlStageMask stages, uint32_t slot,
                                              MtlDeviceSize offset, MtlDeviceSize length) {
    /* THE historical bug-1: this must offset the previously-bound slice. */
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& cbv = m_bindings[s].cbvs[slot];
        cbv.rangeOffset = offset;
        cbv.rangeLength = length;
        m_bindings[s].dirtyCbvs |= 1u << slot;
      }
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) {
    /* Stage tables have 16 slots. Internal slot 16 (the FF DMAP sampler) has
     * no Metal table slot — drop it rather than write past the array. */
    if (unlikely(slot >= m_bindings[0].textures.size())) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl: dropped texture bind beyond stage table (DMAP?)"); }
      return;
    }
    /* No-change guard: D3D9 titles re-SetTexture the bound texture per draw;
     * an accepted redundant bind dirties the slot and (AB path) rebuilds the
     * stage's whole handle slice downstream. */
    bool changed = false;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        if (m_bindings[s].textures[slot] == view)
          continue;
        m_bindings[s].textures[slot] = view;
        m_bindings[s].dirtyTextures |= 1u << slot;
        changed = true;
      }
    }
    if (changed) {
      m_dirty |= DirtyResources;
      /* Decision 10: shader-read bookkeeping for Rules B/C — an attachment
       * sampled anywhere this batch must keep its Store. Insert only on
       * CHANGE (this branch), so the per-draw redundant-bind storm never
       * touches the set. Conservative: a bind without a draw still inserts
       * (only ever blocks an optimization). */
      if (deferredPassesEnabled() && view != nullptr && view->image != nullptr)
        m_frameSampled.insert(view->image.ptr());
    }
  }

  void MtlContextImpl::bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) {
    if (unlikely(slot >= m_bindings[0].samplers.size())) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl: dropped sampler bind beyond stage table (DMAP?)"); }
      return;
    }
    bool changed = false;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        if (m_bindings[s].samplers[slot] == sampler)
          continue;
        m_bindings[s].samplers[slot] = sampler;
        m_bindings[s].dirtySamplers |= 1u << slot;
        changed = true;
      }
    }
    if (changed)
      m_dirty |= DirtyResources;
  }

  void MtlContextImpl::pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) {
    // Each stage owns a private push buffer (bound at kPushConstantIndex). The FF
    // ubershaders (auto-converted from DXVK's GLSL) declare a RenderStates push
    // block laid out as: D3D9SharedPushData at byte 0 (reserving MaxSharedPushDataSize
    // = 32 bytes), then the per-stage block (D3D9Vs/Ffvs/FfpsPushData) at byte 32.
    // The D3D9*PushData::Offset fields are relative to their block base, so a
    // single-stage block must be shifted past the shared reservation (this mirrors
    // DxvkPushDataBlock::computeBlockOffsetForStage: shared -> 0, per-stage -> 32).
    const bool  isShared   = (uint32_t(stages) & (uint32_t(stages) - 1u)) != 0u;
    const uint32_t baseOff = isShared ? 0u : kPushSharedBlockSize;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& b = m_bindings[s];
        std::memcpy(b.pushData.data() + baseOff + offset, data, size);
        b.pushDirtyLo = b.pushDirtyLo ? std::min(b.pushDirtyLo, baseOff + offset) : baseOff + offset;
        b.pushDirtyHi = std::max(b.pushDirtyHi, baseOff + offset + size);
        b.pushExtent  = std::max(b.pushExtent, baseOff + offset + size);
      }
    }
    m_dirty |= DirtyPush;
  }

  // --- draw path (decision 2+3) ----------------------------------------------

  bool MtlContextImpl::ensureFanIndices() {
    if (m_fanIndices != nullptr)
      return true;
    MtlBufferCreateInfo bi = {};
    bi.size      = uint64_t(3u * (kFanMaxVerts - 2u)) * 4u;
    bi.usage     = MtlUsage::IndexBuffer;
    bi.cpuAccess = MtlCpuAccess::Write;
    Rc<MtlBuffer> buf = m_device->createBuffer(bi);
    uint32_t* p = buf != nullptr ? static_cast<uint32_t*>(buf->mapPtr(0u)) : nullptr;
    if (p == nullptr) {
      Logger::err("MtlContextImpl: fan index pattern buffer creation failed");
      return false;
    }
    for (uint32_t t = 0u; t + 2u < kFanMaxVerts; t++) {
      *p++ = 0u;
      *p++ = t + 1u;
      *p++ = t + 2u;
    }
    m_fanIndices = std::move(buf);
    return true;
  }

  void MtlContextImpl::draw(uint32_t count, const MtlDrawArgs* args) {
    if (!prepareDraw())
      return;
    prof::Scope profScope(prof::EncodeDraw);
    if (unlikely(m_topology == MtlPrimitiveTopology::TriangleFan)) {
      /* Fan emulation: without this, fans fall into the triangle-list default
       * and rasterize as disconnected wedges (every 3rd triangle) — 2D vector
       * content (minimaps, HUD polygons) is the classic victim. */
      if (!ensureFanIndices())
        return;
      if (m_fanIndices->trackingSerial() != m_currentSerial)
        m_fanIndices->setTrackingSerial(m_currentSerial);
      for (uint32_t i = 0; i < count; i++) {
        if (args[i].vertexCount < 3u)
          continue;
        const uint32_t verts = std::min(args[i].vertexCount, kFanMaxVerts);
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw_indexed>(
          uint16_t(WMTMtl4RenderCommandDrawIndexed));
        cmd->primitive           = uint64_t(WMTPrimitiveTypeTriangle);
        cmd->index_count         = 3u * (verts - 2u);
        cmd->index_type          = uint64_t(WMTIndexTypeUInt32);
        cmd->index_buffer        = m_fanIndices->gpuAddress(0u);
        cmd->index_buffer_length = m_fanIndices->size;
        cmd->instance_count      = args[i].instanceCount ? args[i].instanceCount : 1u;
        cmd->base_vertex         = int64_t(args[i].firstVertex);
        cmd->base_instance       = args[i].firstInstance;
      }
      return;
    }
    for (uint32_t i = 0; i < count; i++) {
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw>(
        uint16_t(WMTMtl4RenderCommandDraw));
      cmd->primitive      = uint64_t(wmtPrimitive(m_topology));
      cmd->vertex_start   = args[i].firstVertex;
      cmd->vertex_count   = args[i].vertexCount;
      cmd->instance_count = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_instance  = args[i].firstInstance;
    }
  }

  void MtlContextImpl::drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) {
    if (!prepareDraw())
      return;
    if (unlikely(m_topology == MtlPrimitiveTopology::TriangleFan)) {
      /* Indexed fans need a CPU remap of the source indices (GPU-side buffer,
       * unreadable at record time) — unimplemented; drawn as a list. Warn so
       * a title that actually uses them is identifiable. */
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn("MtlContextImpl: indexed triangle fan drawn as list (index remap unimplemented)");
      }
    }
    /* Record-time snapshot (MtlBufferSlice contract): index fetch resolves
     * through the slice's captured backing, never the live buffer. */
    if (m_indexBuffer.storage() == nullptr)
      return;
    prof::Scope profScope(prof::EncodeDraw);
    /* GPU-use stamp, compare-guarded (atomic store only when stale) —
     * replaces the per-draw stamp walk in prepareDraw for the index buffer. */
    if (m_indexBuffer.buffer() != nullptr
     && m_indexBuffer.buffer()->trackingSerial() != m_currentSerial)
      m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
    const uint64_t idxSize = (m_indexType == MtlIndexType::Uint16) ? 2u : 4u;
    const WMTIndexType idxType = (m_indexType == MtlIndexType::Uint16)
      ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;

    for (uint32_t i = 0; i < count; i++) {
      uint64_t indexOffset = uint64_t(args[i].firstIndex) * idxSize;
      uint64_t gpuAddr     = m_indexBuffer.gpuAddress(indexOffset);
      /* MTL4 rule: indexBufferLength is the accessible byte range from the
       * given GPU address (to the end of the backing allocation). */
      uint64_t accessible  = m_indexBuffer.addressableLength(indexOffset);
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw_indexed>(
        uint16_t(WMTMtl4RenderCommandDrawIndexed));
      cmd->primitive           = uint64_t(wmtPrimitive(m_topology));
      cmd->index_count         = args[i].indexCount;
      cmd->index_type          = uint64_t(idxType);
      cmd->index_buffer        = gpuAddr;
      cmd->index_buffer_length = accessible;
      cmd->instance_count      = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_vertex         = int64_t(args[i].vertexOffset);
      cmd->base_instance       = args[i].firstInstance;
    }
  }

  bool MtlContextImpl::prepareDraw() {
    if (!m_pass.pending && !m_pass.active)
      return false;                       /* no render targets bound */
    /* PROF: parent bucket for the whole per-draw resolve; ResolvePso/
     * FlushVertexBuffers/FlushResources/FlushDynamicState (and an arena-cap
     * FlushCmdChunk) nest inside it. */
    prof::Scope profScope(prof::PrepareDraw);
    /* Inline the common case: the pass is already materialized for every
     * draw but the first — skip the call (x86-emulated prologue per draw). */
    if (unlikely(!m_pass.active))
      materializePass();

    /* Arena cap: replay-and-recycle mid-pass once the batch crosses the cap.
     * Any command boundary is a valid flush point — the stream replays in
     * append order across flushes, and Metal snapshots the argument table at
     * each encoded draw, so table writes replayed in a later flush can never
     * retroactively affect a draw encoded in an earlier one.
     * DEFERRED MODE: no live encoder to flush into — the chunk parks with
     * the pass and its memory pools across frames (m_chunkPool), so the cap
     * (a PE-side arena memory knob, not a correctness one) does not apply. */
    if (!deferredPassesEnabled()
     && m_cmdChunk.approxBytes() >= kCmdChunkFlushBytes)
      flushCmdChunk();

    if ((m_dirty & DirtyPso) && !resolvePso())
      return false;

    /* Argument tables rotate once per PASS, not per draw (see
     * selectArgumentTablesForPass): the first draw after materializePass
     * rotates in a retired pair, binds it, and stamps its serial for this
     * cmdbuf; every later draw of the pass mutates that same pair, which is
     * well-defined because the encoder snapshots table contents at each
     * encoded draw. materializePass sets DirtyEverything, so the rotation
     * draw always performs the full-walk flush that reconciles the rotated-in
     * pair's persistent contents (ArgTableShadow diff) with the current
     * logical bindings; the OR below is belt-and-braces for any future path
     * that clears m_argTablesBound without dirtying bindings. */
    if (!m_argTablesBound) {
      selectArgumentTablesForPass();
      bindArgumentTables();
      m_argTablesBound = true;
      m_dirty |= DirtyVertexBuffers | DirtyResources | DirtyPush;
      /* ROTATION = FULL REWRITE. The rotated-in pair is a DIFFERENT physical
       * table whose shadow mirrors ITS OWN last contents, not the current
       * logical bindings — the per-slot dirty masks (which only say "touched
       * since the last flush") are meaningless against it. Force every mask
       * wide so the flush below visits ALL slots and reconciles the fresh
       * pair via the shadow diff (slots that happen to match still skip the
       * Metal write). Missing this would leave a rotated-in table with
       * stale/missing slots = silent wrong-texture corruption. This is the
       * ONLY place m_currentArgTable changes, so it is the only point where
       * mask-vs-shadow coherence can break. */
      m_dirtyVertexStreams = ~0u;
      for (auto& b : m_bindings) {
        b.dirtyTextures = ~0u;
        b.dirtySamplers = ~0u;
        b.dirtyCbvs     = ~0u;
      }
    }

    /* Binding flush, gated per draw: when NO binding-shaped state changed
     * since the previous draw (every mutation funnels into these bits —
     * the bind and pushData setters set them, invalidateBuffer's retarget
     * sets them, and pass/cmdbuf/blit boundaries set DirtyEverything), the
     * bound tables already hold exactly the current logical bindings: skip
     * the walk entirely. On the dirty path the per-slot ArgTableShadow diff
     * appends only slots whose value changed. Vertex streams live at fixed
     * indices (kVertexBufferBaseIndex+) independent of the bound shader, so
     * only DirtyVertexBuffers gates them; DirtyPso participates in the
     * resource walk because the CBV AIR-slot map is shader-derived
     * (flushResources translates through fn->cbvSlotMap). */
    if (m_dirty & DirtyVertexBuffers)
      flushVertexBuffers();
    if (m_dirty & (DirtyPso | DirtyResources | DirtyPush))
      flushResources();
    if (m_dirty & (DirtyViewport | DirtyDepthStencil | DirtyBlendColor
                 | DirtyStencilRef | DirtyDepthBias | DirtyRasterizer))
      flushDynamicState();
    m_pass.encodedDraws++;

    /* GPU-use stamp (MtlResource contract): every buffer this draw fetches
     * is busy until the recording command buffer's serial completes. This
     * is what makes D3D9DeviceEx::WaitForResource a serial compare instead
     * of a full GPU drain. Stamped per draw, not per bind — bindings
     * persist across submissions, so a bind-time stamp would go stale and
     * let a Lock overwrite vertex memory a later cmdbuf still reads.
     * TRIM: (re)bound streams are stamped by flushVertexBuffers' dirty-mask
     * walk (it visits exactly the changed slots), and the index buffer by
     * drawIndexed's compare-guarded stamp. The full walk below only runs
     * when the SERIAL moves (new cmdbuf): that is the one case where an
     * unchanged-but-still-bound binding carries a stale stamp. */
    if (unlikely(m_currentSerial != m_lastStampSerial)) {
      for (auto& stream : m_vertexStreams) {
        if (stream.slice.defined() && stream.slice.buffer() != nullptr)
          stream.slice.buffer()->setTrackingSerial(m_currentSerial);
      }
      if (m_indexBuffer.defined() && m_indexBuffer.buffer() != nullptr)
        m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
      m_lastStampSerial = m_currentSerial;
    }

    /* all folded state is now bound on the encoder / argument tables; the
     * next draw only re-flushes what changes (ring slices already snapshot). */
    m_dirty = 0u;
    prof::bump(prof::CntDraws);
    return true;
  }

  // --- frame-deferred pass list (decision 10) --------------------------------

  CmdChunk MtlContextImpl::takeChunk() {
    if (m_chunkPool.empty())
      return CmdChunk();
    CmdChunk c = std::move(m_chunkPool.back());
    m_chunkPool.pop_back();
    return c;
  }

  void MtlContextImpl::recordPassEntry() {
    FrameEntry e;
    e.kind         = FrameEntry::Kind::Render;
    e.targets      = m_pass.targets;          /* copy — m_pass keeps them pending */
    e.clearColor   = m_pass.clearColor;
    e.clearValues  = m_pass.clearValues;
    e.clearDepth   = m_pass.clearDepth;
    e.clearStencil = m_pass.clearStencil;
    e.depthClear   = m_pass.depthClear;
    e.stencilClear = m_pass.stencilClear;
    e.draws        = m_pass.encodedDraws;
    e.chunks.push_back(std::move(m_cmdChunk));
    m_cmdChunk = takeChunk();
    m_frameEntries.push_back(std::move(e));

    /* Mirror the immediate closePass post-state exactly. */
    m_pass.encoder = 0;
    m_pass.active  = false;
    m_pass.pending = true;    /* same targets stay bound until rebound */
    m_pass.encodedDraws = 0u;
    m_argTablesBound = false; /* next pass rotates + re-binds */
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }

  void MtlContextImpl::prepareDirectEncode(uint32_t passCloseCounter) {
    if (m_pass.active) {
      prof::bump(prof::Counter(passCloseCounter));
      closePass();
    }
    else
      flushPendingClears();   /* a copy must observe folded, not-yet-drawn clears */
    ensureCommandBuffer();
  }

  void MtlContextImpl::encodeDirect(std::function<void(obj_handle_t)>&& fn) {
    if (!deferredPassesEnabled()) {
      fn(m_cmdbuf.handle());
      return;
    }
    FrameEntry e;
    e.kind   = FrameEntry::Kind::Direct;
    e.encode = std::move(fn);
    m_frameEntries.push_back(std::move(e));
  }

  void MtlContextImpl::noteTransferRead(const MtlTexture* tex) {
    if (!deferredPassesEnabled() || tex == nullptr)
      return;
    m_frameSampled.insert(tex);
    m_everTransferRead.insert(tex);
  }

  void MtlContextImpl::flushFramePasses() {
    /* Rule B applies only to the batch that ends at the Present boundary
     * (armed by endFrame); consume the flag on EVERY flush so a mid-frame
     * flush can never inherit it. */
    const bool frameBoundary = m_frameBoundaryFlush;
    m_frameBoundaryFlush = false;

    if (m_frameEntries.empty()) {
      m_frameSampled.clear();
      return;
    }

    if (!m_cmdbuf) {
      /* Cannot happen by construction (every record path runs
       * ensureCommandBuffer first) — drop loudly rather than crash. */
      Logger::err("MtlContextImpl::flushFramePasses: entries without cmdbuf, dropped");
      m_frameEntries.clear();
      m_frameSampled.clear();
      return;
    }

    /* Independent runtime gates, ON by default (mtld3d's const-bool gates,
     * exposed as env kill-switches for in-game bisects). */
    static const bool ruleB   = std::getenv("D9MT_NO_RULE_B") == nullptr;
    static const bool ruleC   = std::getenv("D9MT_NO_RULE_C") == nullptr;
    static const bool ruleE   = std::getenv("D9MT_NO_RULE_E") == nullptr;
    static const bool ruleF   = std::getenv("D9MT_NO_RULE_F") == nullptr;
    static const bool mergeOn = std::getenv("D9MT_NO_PASSMERGE") == nullptr;

    auto& entries = m_frameEntries;

    /* Attachment identity for the rules: texture + mip + layer (the
     * sameAttachment contract) — Rc-held by the entries, so no pointer can
     * be recycled while parked. */
    using AttKey = std::tuple<const void*, uint32_t, uint32_t>;
    auto attKeyOf = [](const Rc<MtlTextureView>& v) {
      return AttKey((const void*)v->image.ptr(), v->key.baseMip, v->key.baseLayer);
    };
    auto sameRt = [](const Rc<MtlTextureView>& a, const Rc<MtlTextureView>& b) {
      if (!sameAttachment(a, b))
        return false;
      return a == nullptr || b == nullptr || a->key.format == b->key.format;
    };
    auto isSampled = [&](const Rc<MtlTextureView>& v) {
      return v != nullptr && v->image != nullptr
          && m_frameSampled.find(v->image.ptr()) != m_frameSampled.end();
    };
    auto anyClear = [](const FrameEntry& e) {
      bool c = e.clearDepth || e.clearStencil;
      for (bool b : e.clearColor) c = c || b;
      return c;
    };
    auto chunksEmpty = [](const FrameEntry& e) {
      for (const auto& c : e.chunks)
        if (!c.empty()) return false;
      return true;
    };

    /* --- Rule E: fold clear-only passes into the next pass on the same
     * targets as its load actions. Clear-only = Render entry, zero draws,
     * empty chunk (a non-empty chunk may carry visibility-mode or barrier
     * commands that must materialize in place). Merge target: the next
     * Render entry attaching EVERY cleared attachment at the SAME slot with
     * load=Load. Bail on any intervening Direct entry (its reads/writes are
     * positional and opaque), on any intervening pass touching a cleared
     * attachment, and on sampled targets — conservative, correctness first. */
    if (ruleE) {
      for (size_t i = 0u; i < entries.size(); ) {
        FrameEntry& p = entries[i];
        if (p.kind != FrameEntry::Kind::Render || p.draws != 0u
         || !chunksEmpty(p) || !anyClear(p)) {
          i++;
          continue;
        }
        bool blocked = false;
        for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets && !blocked; s++)
          blocked = p.clearColor[s] && isSampled(p.targets.color[s]);
        if ((p.clearDepth || p.clearStencil) && isSampled(p.targets.depthStencil))
          blocked = true;
        size_t target = SIZE_MAX;
        for (size_t j = i + 1u; j < entries.size() && !blocked && target == SIZE_MAX; j++) {
          const FrameEntry& c = entries[j];
          if (c.kind != FrameEntry::Kind::Render) {
            blocked = true;   /* opaque transfer/blit between clear and target */
            break;
          }
          bool touches = false, matches = true;
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
            if (!p.clearColor[s])
              continue;
            /* the cleared attachment appearing at ANY slot of c counts as a
             * touch; a fold is only legal at the SAME slot with load=Load */
            bool touchedHere = false;
            for (uint32_t cs = 0u; cs < MtlRenderTargets::MaxColorTargets; cs++)
              touchedHere = touchedHere
                || (c.targets.color[cs] != nullptr
                 && sameAttachment(c.targets.color[cs], p.targets.color[s]));
            touches = touches || touchedHere;
            matches = matches && sameRt(c.targets.color[s], p.targets.color[s])
                   && !c.clearColor[s];
          }
          if (p.clearDepth || p.clearStencil) {
            bool touchedHere = c.targets.depthStencil != nullptr
              && sameAttachment(c.targets.depthStencil, p.targets.depthStencil);
            touches = touches || touchedHere;
            matches = matches && sameRt(c.targets.depthStencil, p.targets.depthStencil)
                   && !(p.clearDepth   && c.clearDepth)
                   && !(p.clearStencil && c.clearStencil)
                   && !c.clearDepth && !c.clearStencil;
          }
          if (!touches)
            continue;         /* unrelated pass; keep scanning */
          if (matches)
            target = j;
          else
            blocked = true;   /* partial touch — materialize in place */
        }
        if (target != SIZE_MAX) {
          FrameEntry& t = entries[target];
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
            if (p.clearColor[s]) {
              t.clearColor[s]  = true;
              t.clearValues[s] = p.clearValues[s];
            }
          }
          if (p.clearDepth)   { t.clearDepth = true;   t.depthClear = p.depthClear; }
          if (p.clearStencil) { t.clearStencil = true; t.stencilClear = p.stencilClear; }
          for (auto& ch : entries[i].chunks) {
            ch.reset();
            m_chunkPool.push_back(std::move(ch));
          }
          entries.erase(entries.begin() + i);
          /* don't advance — what was at i+1 is now at i */
        } else {
          i++;
        }
      }
    }

    /* --- Pass merging: adjacent Render entries with IDENTICAL attachments
     * (texture+mip+layer+format on every slot) replay into one encoder,
     * killing a full tile store+load per merge on TBDR. The follower must
     * have no clears (a load-action clear would wipe the leader's draws).
     * This is the flush-time replacement for the reverted D9MT_RTSKIP hack:
     * a Reset can never interleave here, and entries hold Rc keepalives. */
    if (mergeOn) {
      for (size_t i = 0u; i + 1u < entries.size(); ) {
        FrameEntry& a = entries[i];
        FrameEntry& b = entries[i + 1u];
        bool merge = a.kind == FrameEntry::Kind::Render
                  && b.kind == FrameEntry::Kind::Render
                  && !anyClear(b)
                  && sameRt(a.targets.depthStencil, b.targets.depthStencil);
        for (uint32_t s = 0u; merge && s < MtlRenderTargets::MaxColorTargets; s++)
          merge = sameRt(a.targets.color[s], b.targets.color[s]);
        if (merge) {
          for (auto& ch : b.chunks)
            a.chunks.push_back(std::move(ch));
          a.draws += b.draws;
          entries.erase(entries.begin() + i + 1u);
        } else {
          i++;
        }
      }
    }

    /* --- default store actions */
    for (auto& e : entries) {
      if (e.kind != FrameEntry::Kind::Render)
        continue;
      e.colorStore.fill(uint8_t(WMTStoreActionStore));
      e.depthStore   = uint8_t(WMTStoreActionStore);
      e.stencilStore = uint8_t(WMTStoreActionStore);
    }

    /* --- Rule B: depth/stencil store=DontCare on each depth attachment's
     * LAST pass of the batch — only at the Present boundary (D3D9: depth is
     * undefined across Present), and only when the texture can never be
     * shader-read: structurally (no ShaderRead usage — a D3D9 INTZ-style
     * depth texture keeps Store forever, mtld3d's depth_is_sampleable) and
     * observationally (not read this batch). */
    if (ruleB && frameBoundary) {
      std::unordered_set<const void*> handled;
      for (size_t ri = entries.size(); ri-- > 0u; ) {
        FrameEntry& e = entries[ri];
        if (e.kind != FrameEntry::Kind::Render || e.targets.depthStencil == nullptr)
          continue;
        const Rc<MtlTextureView>& dsv = e.targets.depthStencil;
        if (dsv->image == nullptr || !handled.insert(dsv->image.ptr()).second)
          continue;
        const MtlTexture* img = dsv->image.ptr();
        if ((img->info().usage & MtlTextureUsageShaderRead) != 0u)
          continue;
        if (m_frameSampled.find(img) != m_frameSampled.end())
          continue;
        e.depthStore   = uint8_t(WMTStoreActionDontCare);
        e.stencilStore = uint8_t(WMTStoreActionDontCare);
        prof::bump(prof::CntStoreDontCare);
      }
    }

    /* --- Rule C: color store=DontCare when the NEXT pass on the same
     * attachment starts with a full clear. Skipped for anything sampled or
     * transfer-read this batch (positional reads collapse conservatively
     * into the global set) and for any texture EVER read by a transfer —
     * the present copy reads the backbuffer every frame, which makes this
     * the backbuffer exemption (its store never goes DontCare). */
    if (ruleC) {
      std::map<AttKey, std::pair<size_t, uint32_t>> nextUse;
      for (size_t ri = entries.size(); ri-- > 0u; ) {
        FrameEntry& e = entries[ri];
        if (e.kind != FrameEntry::Kind::Render)
          continue;
        for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
          const Rc<MtlTextureView>& v = e.targets.color[s];
          if (v == nullptr || v->image == nullptr)
            continue;
          AttKey key = attKeyOf(v);
          auto it = nextUse.find(key);
          if (it != nextUse.end()) {
            const auto [nj, ns] = it->second;
            if (entries[nj].clearColor[ns]
             && m_frameSampled.find(v->image.ptr()) == m_frameSampled.end()
             && m_everTransferRead.find(v->image.ptr()) == m_everTransferRead.end()) {
              e.colorStore[s] = uint8_t(WMTStoreActionDontCare);
              prof::bump(prof::CntStoreDontCare);
            }
          }
          nextUse[key] = { ri, s };
        }
      }
    }

    /* --- Rule F: cull passes with no draws, no commands and no surviving
     * Store — nothing they do reaches device memory. */
    if (ruleF) {
      for (size_t i = 0u; i < entries.size(); ) {
        FrameEntry& e = entries[i];
        bool cull = e.kind == FrameEntry::Kind::Render
                 && e.draws == 0u && chunksEmpty(e);
        if (cull) {
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++)
            cull = cull && !(e.targets.color[s] != nullptr
                          && e.colorStore[s] == uint8_t(WMTStoreActionStore));
          if (e.targets.depthStencil != nullptr
           && (e.depthStore   == uint8_t(WMTStoreActionStore)
            || e.stencilStore == uint8_t(WMTStoreActionStore)))
            cull = false;
        }
        if (cull) {
          for (auto& ch : e.chunks) {
            ch.reset();
            m_chunkPool.push_back(std::move(ch));
          }
          entries.erase(entries.begin() + i);
        } else {
          i++;
        }
      }
    }

    /* --- replay, in exact record order ------------------------------------ */
    uint32_t encoders = 0u;
    for (auto& e : entries) {
      if (e.kind == FrameEntry::Kind::Direct) {
        e.encode(m_cmdbuf.handle());
        e.encode = nullptr;
        continue;
      }

      WMTRenderPassInfo info = {};
      info.visibility_buffer = m_visBuffer.handle();
      for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
        const auto& view = e.targets.color[s];
        if (view == nullptr)
          continue;
        auto* impl = static_cast<MtlTextureViewImpl*>(view.ptr());
        info.colors[s].texture      = impl->owned.handle();
        info.colors[s].load_action  = e.clearColor[s]
          ? WMTLoadActionClear : WMTLoadActionLoad;
        info.colors[s].store_action = WMTStoreAction(e.colorStore[s]);
        if (e.clearColor[s]) {
          const auto& v = e.clearValues[s];
          info.colors[s].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
        }
        if (d9mtTrace() && s == 0u)
          Logger::info(str::format("MAT(replay) color img=", (void*)impl->image.ptr(),
            " load=", e.clearColor[s] ? "CLEAR" : "LOAD",
            " store=", e.colorStore[s] == uint8_t(WMTStoreActionStore) ? "STORE" : "DONTCARE"));
      }
      if (e.targets.depthStencil != nullptr) {
        auto* impl = static_cast<MtlTextureViewImpl*>(e.targets.depthStencil.ptr());
        info.depth.texture      = impl->owned.handle();
        info.depth.load_action  = e.clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
        info.depth.store_action = WMTStoreAction(e.depthStore);
        info.depth.clear_depth  = e.depthClear;
        const auto* fmtInfo = lookupFormatInfo(impl->key.format);
        if (fmtInfo->flags & MtlFormatFlagStencil) {
          info.stencil.texture       = impl->owned.handle();
          info.stencil.load_action   = e.clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
          info.stencil.store_action  = WMTStoreAction(e.stencilStore);
          info.stencil.clear_stencil = e.stencilClear;
        }
      }

      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
      for (auto& ch : e.chunks) {
        if (!ch.empty())
          MTL4RenderCommandEncoder_encodeCommands(enc, ch.head());
        ch.reset();
        m_chunkPool.push_back(std::move(ch));
      }
      e.chunks.clear();
      /* §4c: identical producer-barrier structure to the immediate path —
       * every encoder's fragment writes are ordered ahead of all later
       * queue stages before endEncoding. */
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
      encoders++;

      /* GPU-lifetime keepalive for the attachment views (retire-by-serial
       * at submit, which is the caller of this function). */
      for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
        if (e.targets.color[s] != nullptr)
          m_keepAlive.push_back(e.targets.color[s]);
      }
      if (e.targets.depthStencil != nullptr)
        m_keepAlive.push_back(e.targets.depthStencil);
    }
    prof::bump(prof::CntFramePasses, encoders);

    /* No FrameEntry outlives the frame flush — the Reset-vs-parked-pass bug
     * class (the D9MT_RTSKIP killer) is structurally impossible. */
    entries.clear();
    m_frameSampled.clear();
  }

  // (resolvePso / bindArgumentTables contents / flushVertexBuffers /
  //  flushResources / flushDynamicState / transfers / init / queries / misc
  //  continue in mtl_context_transfer.cpp to keep TU size sane)

}
