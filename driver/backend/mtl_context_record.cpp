#include "mtl_context_impl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

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
    if (m_pass.active)
      closePass();

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

    if (m_pass.active)
      closePass();
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

  void MtlContextImpl::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
    if (m_pass.active)
      closePass();
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
    materializePass();
    closePass();
  }

  void MtlContextImpl::materializePass() {
    if (m_pass.active)
      return;

    ensureCommandBuffer();

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
    if (stream.stride != stride)
      m_dirty |= DirtyPso;               /* §6.4 stride participates in PSO key */
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
    if (stream.stride != stride)
      m_dirty |= DirtyPso;               /* §6.4 stride participates in PSO key */
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
    if (changed)
      m_dirty |= DirtyResources;
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

  void MtlContextImpl::draw(uint32_t count, const MtlDrawArgs* args) {
    if (!prepareDraw())
      return;
    prof::Scope profScope(prof::EncodeDraw);
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
     * retroactively affect a draw encoded in an earlier one. */
    if (m_cmdChunk.approxBytes() >= kCmdChunkFlushBytes)
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

  // (resolvePso / bindArgumentTables contents / flushVertexBuffers /
  //  flushResources / flushDynamicState / transfers / init / queries / misc
  //  continue in mtl_context_transfer.cpp to keep TU size sane)

}
