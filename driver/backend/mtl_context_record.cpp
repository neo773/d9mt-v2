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

    m_allocators[0] = WmtAllocator::adopt(MTLDevice_newCommandAllocator(dev));
    m_allocator     = m_allocators[0].handle();
    m_frameIndex    = 1u;
  }

  MtlContextImpl::~MtlContextImpl() {
    if (m_pass.active)
      closePass();
    for (auto& kv : m_fxScalers)
      NSObject_release(kv.second);
    if (m_visBuffer) {
      m_device->residency().allocDestroyed(m_visBuffer.detach(), nullptr,
        m_device->submissions().nextSerial());
    }
  }


  void MtlContextImpl::beginRecording() {
    prof::Scope profScope(prof::BeginRecording);
    m_dirty = DirtyEverything;

    uint32_t slot = m_frameIndex % uint32_t(m_allocators.size());
    m_frameIndex++;
    if (!m_allocators[slot]) {
      m_allocators[slot] = WmtAllocator::adopt(
        MTLDevice_newCommandAllocator(m_device->wmtDevice()));
    } else {
      /* Reset requires the slot's prior command buffers to have COMPLETED
       * (MTL4CommandAllocator contract) — an ungated reset recycles command
       * memory the GPU may still be reading. */
      if (m_device->submissions().completedSerial() < m_allocatorSerial[slot])
        m_device->submissions().waitFor(m_allocatorSerial[slot]);
      MTL4CommandAllocator_reset(m_allocators[slot].handle());
    }
    m_allocator = m_allocators[slot].handle();
    m_allocatorSlot = slot;

    ensureCommandBuffer();
  }

  void MtlContextImpl::ensureCommandBuffer() {
    if (m_cmdbuf)
      return;

    m_cmdbuf = WmtCommandBuffer::adopt(
      MTLDevice_newMTL4CommandBuffer(m_device->wmtDevice()));
    MTL4CommandBuffer_beginCommandBuffer(m_cmdbuf.handle(), m_allocator);
    m_currentSerial = m_device->submissions().nextSerial();
  }

  static int  s_gpuCaptureRemaining = -2;
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

    flushFramePasses();

    if (!m_cmdbuf)
      return;

    MTL4CommandBuffer_endCommandBuffer(m_cmdbuf.handle());

    m_device->residency().beforeSubmit();

    obj_handle_t cb = m_cmdbuf.handle();
    gpuCaptureBeforeCommit(m_device->wmtDevice());
    MTL4CommandQueue_commit(m_device->wmtQueue(), &cb, 1u);
    gpuCaptureAfterCommit();

    uint64_t serial = m_device->submissions().onSubmit(m_device->wmtQueue());
    m_allocatorSerial[m_allocatorSlot] = serial;

    m_device->uploadRing().onSubmit(serial);

    if (!m_keepAlive.empty()) {
      auto keepAlive = std::make_shared<std::vector<Rc<MtlResource>>>(
        std::move(m_keepAlive));
      for (auto& res : *keepAlive)
        res->setTrackingSerial(serial);
      m_device->submissions().retireAfter(serial,
        [keepAlive]() mutable { keepAlive->clear(); });
      m_keepAlive.clear();
    }

    auto doneCmd = std::make_shared<WmtCommandBuffer>(std::move(m_cmdbuf));
    m_device->submissions().retireAfter(serial,
      [doneCmd]() mutable { doneCmd->reset(); });

    if (waitCompleted)
      m_device->submissions().waitFor(serial);

    m_cmdbuf.reset();
    m_dirty = DirtyEverything;

    const uint64_t completed = m_device->submissions().completedSerial();
    m_device->submissions().processRetirements();
    m_device->residency().serialRetired(completed);
    m_device->uploadRing().onComplete(completed);
  }

  void MtlContextImpl::flushCommandList(const char* reason, MtlSubmitStatus* status) {
    submit(false);
    beginRecording();
    if (status) {
      status->submissionId = m_currentSerial;
      status->completed    = false;
    }
  }

  void MtlContextImpl::endFrame() {
    m_frameBoundaryFlush = true;

    uint64_t completed = m_device->submissions().completedSerial();
    {
      prof::Scope profScope(prof::EndFrame);
      m_device->submissions().processRetirements();
      m_device->residency().serialRetired(completed);
      m_device->uploadRing().onComplete(completed);
    }
    prof::onFrameEnd();

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
    prof::Scope profScope(prof::SynchronizeWsi);
    if (sync.drawable == MtlNullHandle) {
      Logger::warn("MtlContextImpl: synchronizeWsi without drawable");
      return;
    }

    if (m_pass.active) {
      prof::bump(prof::CntPassCloseSubmit);
      closePass();
    }
    ensureCommandBuffer();

    MTL4CommandQueue_waitForDrawable(m_device->wmtQueue(), sync.drawable);
    submit(false);

    MTL4CommandQueue_signalDrawable(m_device->wmtQueue(), sync.drawable);
    if (sync.minDuration > 0.0)
      MetalDrawable_presentAfterMinimumDuration(sync.drawable, sync.minDuration);
    else
      MetalDrawable_present(sync.drawable);
    NSObject_release(sync.drawable);
  }


  static bool sameAttachment(const Rc<MtlTextureView>& a,
                             const Rc<MtlTextureView>& b);

  void MtlContextImpl::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
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

    m_pass = PendingPass();
    m_pass.targets = std::move(targets);
    m_pass.pending = true;
    m_dirty |= DirtyPso | DirtyViewport;
  }

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
    if (std::getenv("D9MT_NOCOLORCLEAR") && (clear & MtlAspectColor)) {
      clear = MtlAspectMask(clear & ~MtlAspectColor);
      if (!clear) return;
    }
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
    /* Metal snapshots an argument table's contents at each encoded draw, so
     * one pair can be mutated and reused for every draw in a pass; rotation
     * only needs to happen at the pass boundary. Growth is always by
     * push_back at the end, never insert-at-cursor — inserting scrambles the
     * per-slot serials so the round-robin never lands on a retired slot and
     * the pool grows unbounded. */
    prof::bump(prof::CntArgRotate);
    const uint64_t completed = m_device->submissions().completedSerial();
    const size_t   n         = m_argTables.size();

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
    if (!m_pass.encoder)
      return;
    prof::Scope profScope(prof::FlushCmdChunk);
    prof::bump(prof::CntChunkFlush);
    MTL4RenderCommandEncoder_encodeCommands(m_pass.encoder, m_cmdChunk.head());
    m_cmdChunk.reset();
  }

  void MtlContextImpl::closePass() {
    if (!m_pass.active)
      return;

    if (deferredPassesEnabled()) {
      recordPassEntry();
      return;
    }

    flushCmdChunk();

    /* Metal 4 does no implicit render-pass transition: a later encoder that
     * samples or copies this attachment must be ordered after these writes. */
    emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
    MTLCommandEncoder_endEncoding(m_pass.encoder);
    NSObject_release(m_pass.encoder);
    m_pass.encoder = 0;
    m_pass.active = false;
    m_pass.pending = true;
    m_argTablesBound = false;
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }


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
    m_topology = topology;
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
    /* accepted but intentionally a no-op: Metal has no depth-bounds test */
  }

  void MtlContextImpl::bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) {
    auto& stream = m_vertexStreams[binding];
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;
    }
    stream.slice = std::move(slice);
    stream.stride = stride;
    m_dirtyVertexStreams |= 1u << binding;
    m_dirty |= DirtyVertexBuffers;
  }

  void MtlContextImpl::bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) {
    auto& stream = m_vertexStreams[binding];
    if (!stream.slice.defined())
      return;
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;
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
    if (unlikely(slot >= m_bindings[0].textures.size())) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl: dropped texture bind beyond stage table (DMAP?)"); }
      return;
    }
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
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn("MtlContextImpl: indexed triangle fan drawn as list (index remap unimplemented)");
      }
    }
    if (m_indexBuffer.storage() == nullptr)
      return;
    prof::Scope profScope(prof::EncodeDraw);
    if (m_indexBuffer.buffer() != nullptr
     && m_indexBuffer.buffer()->trackingSerial() != m_currentSerial)
      m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
    const uint64_t idxSize = (m_indexType == MtlIndexType::Uint16) ? 2u : 4u;
    const WMTIndexType idxType = (m_indexType == MtlIndexType::Uint16)
      ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;

    for (uint32_t i = 0; i < count; i++) {
      uint64_t indexOffset = uint64_t(args[i].firstIndex) * idxSize;
      uint64_t gpuAddr     = m_indexBuffer.gpuAddress(indexOffset);
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
      return false;
    prof::Scope profScope(prof::PrepareDraw);
    if (unlikely(!m_pass.active))
      materializePass();

    if (!deferredPassesEnabled()
     && m_cmdChunk.approxBytes() >= kCmdChunkFlushBytes)
      flushCmdChunk();

    if ((m_dirty & DirtyPso) && !resolvePso())
      return false;

    if (!m_argTablesBound) {
      selectArgumentTablesForPass();
      bindArgumentTables();
      m_argTablesBound = true;
      m_dirty |= DirtyVertexBuffers | DirtyResources | DirtyPush;
      m_dirtyVertexStreams = ~0u;
      for (auto& b : m_bindings) {
        b.dirtyTextures = ~0u;
        b.dirtySamplers = ~0u;
        b.dirtyCbvs     = ~0u;
      }
    }

    if (m_dirty & DirtyVertexBuffers)
      flushVertexBuffers();
    if (m_dirty & (DirtyPso | DirtyResources | DirtyPush))
      flushResources();
    if (m_dirty & (DirtyViewport | DirtyDepthStencil | DirtyBlendColor
                 | DirtyStencilRef | DirtyDepthBias | DirtyRasterizer))
      flushDynamicState();
    m_pass.encodedDraws++;

    if (unlikely(m_currentSerial != m_lastStampSerial)) {
      for (auto& stream : m_vertexStreams) {
        if (stream.slice.defined() && stream.slice.buffer() != nullptr)
          stream.slice.buffer()->setTrackingSerial(m_currentSerial);
      }
      if (m_indexBuffer.defined() && m_indexBuffer.buffer() != nullptr)
        m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
      m_lastStampSerial = m_currentSerial;
    }

    m_dirty = 0u;
    prof::bump(prof::CntDraws);
    return true;
  }


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
    e.targets      = m_pass.targets;
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

    m_pass.encoder = 0;
    m_pass.active  = false;
    m_pass.pending = true;
    m_pass.encodedDraws = 0u;
    m_argTablesBound = false;
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }

  void MtlContextImpl::prepareDirectEncode(uint32_t passCloseCounter) {
    if (m_pass.active) {
      prof::bump(prof::Counter(passCloseCounter));
      closePass();
    }
    else
      flushPendingClears();
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
    const bool frameBoundary = m_frameBoundaryFlush;
    m_frameBoundaryFlush = false;

    if (m_frameEntries.empty()) {
      m_frameSampled.clear();
      return;
    }

    if (!m_cmdbuf) {
      Logger::err("MtlContextImpl::flushFramePasses: entries without cmdbuf, dropped");
      m_frameEntries.clear();
      m_frameSampled.clear();
      return;
    }

    static const bool ruleB   = std::getenv("D9MT_NO_RULE_B") == nullptr;
    static const bool ruleC   = std::getenv("D9MT_NO_RULE_C") == nullptr;
    static const bool ruleE   = std::getenv("D9MT_NO_RULE_E") == nullptr;
    static const bool ruleF   = std::getenv("D9MT_NO_RULE_F") == nullptr;
    static const bool mergeOn = std::getenv("D9MT_NO_PASSMERGE") == nullptr;

    auto& entries = m_frameEntries;

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
            blocked = true;
            break;
          }
          bool touches = false, matches = true;
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
            if (!p.clearColor[s])
              continue;
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
            continue;
          if (matches)
            target = j;
          else
            blocked = true;
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
        } else {
          i++;
        }
      }
    }

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

    for (auto& e : entries) {
      if (e.kind != FrameEntry::Kind::Render)
        continue;
      e.colorStore.fill(uint8_t(WMTStoreActionStore));
      e.depthStore   = uint8_t(WMTStoreActionStore);
      e.stencilStore = uint8_t(WMTStoreActionStore);
    }

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
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
      encoders++;

      for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
        if (e.targets.color[s] != nullptr)
          m_keepAlive.push_back(e.targets.color[s]);
      }
      if (e.targets.depthStencil != nullptr)
        m_keepAlive.push_back(e.targets.depthStencil);
    }
    prof::bump(prof::CntFramePasses, encoders);

    entries.clear();
    m_frameSampled.clear();
  }

}
