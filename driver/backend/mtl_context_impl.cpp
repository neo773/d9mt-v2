#include "mtl_context_impl.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "../util/log/log.h"

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
     * [[buffer(20)]]); textures/samplers 16 = D3D9 sampler slot count. */
    WMTArgTableInfo ati = {};
    ati.max_buffers                = 32u;
    ati.max_textures               = 16u;
    ati.max_samplers               = 16u;
    ati.initialize_bindings        = 1u;
    ati.support_attribute_strides  = 1u;
    m_argTable[0] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));
    m_argTable[1] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));

    /* ONE residency set per context (decision 4). */
    WMTResidencySetInfo rsi = {};
    rsi.initial_capacity = 4096u;
    m_residencySet = WmtResidencySet::adopt(MTLDevice_newResidencySet(dev, &rsi));

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
  }

  // --- lifecycle -----------------------------------------------------------

  void MtlContextImpl::beginRecording() {
    m_dirty = DirtyEverything;

    /* Rotate to a >= ring-depth-old allocator and reset it (GPU-idle). */
    uint32_t slot = m_frameIndex % uint32_t(m_allocators.size());
    m_frameIndex++;
    if (!m_allocators[slot])
      m_allocators[slot] = WmtAllocator::adopt(
        MTLDevice_newCommandAllocator(m_device->wmtDevice()));
    else
      MTL4CommandAllocator_reset(m_allocators[slot].handle());
    m_allocator = m_allocators[slot].handle();

    /* Rebuild the residency set only once the epoch that last used it has
     * retired — never evict an allocation an in-flight cmdbuf still reads. */
    if (m_device->submissions().completedSerial() >= m_residencyEpochSerial) {
      MTLResidencySet_removeAllAllocations(m_residencySet.handle());
      MTLResidencySet_commit(m_residencySet.handle());
      m_residentSet.clear();
    }

    ensureCommandBuffer();
  }

  void MtlContextImpl::ensureCommandBuffer() {
    if (m_cmdbuf)
      return;

    m_cmdbuf = WmtCommandBuffer::adopt(
      MTLDevice_newMTL4CommandBuffer(m_device->wmtDevice()));
    MTL4CommandBuffer_beginCommandBuffer(m_cmdbuf.handle(), m_allocator);
    MTL4CommandBuffer_useResidencySet(m_cmdbuf.handle(), m_residencySet.handle());
    m_currentSerial = m_device->submissions().nextSerial();
  }

  void MtlContextImpl::submit(bool waitCompleted) {
    if (m_pass.active)
      closePass();

    if (!m_cmdbuf)
      return;

    MTL4CommandBuffer_endCommandBuffer(m_cmdbuf.handle());

    /* Realize residency before the GPU runs this cmdbuf (decision 4). */
    MTLResidencySet_commit(m_residencySet.handle());
    MTLResidencySet_requestResidency(m_residencySet.handle());

    obj_handle_t cb = m_cmdbuf.handle();
    MTL4CommandQueue_commit(m_device->wmtQueue(), &cb, 1u);

    /* MTL4: the completion signal is a queue-timeline op AFTER commit. */
    uint64_t serial = m_device->submissions().onSubmit(m_device->wmtQueue());
    m_residencyEpochSerial = serial;

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
  }

  void MtlContextImpl::flushCommandList(const char* reason, MtlSubmitStatus* status) {
    submit(false);
    ensureCommandBuffer();
    if (status) {
      status->submissionId = m_currentSerial;
      status->completed    = false;
    }
  }

  void MtlContextImpl::endFrame() {
    uint64_t completed = m_device->submissions().completedSerial();
    m_device->submissions().processRetirements();
    m_device->uploadRing().onComplete(completed);
  }

  void MtlContextImpl::synchronizeWsi(MtlPresenterSync sync) {
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

    /* GPU-side wait: the drawable's backing texture must be released by the
     * display before this cmdbuf writes it. Enqueued before commit. */
    MTL4CommandQueue_waitForDrawable(m_device->wmtQueue(), sync.drawable);

    submit(false);

    MTL4CommandQueue_signalDrawable(m_device->wmtQueue(), sync.drawable);
    MetalDrawable_present(sync.drawable);
  }

  // --- render pass (decision 1) ---------------------------------------------

  void MtlContextImpl::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
    if (m_pass.active)
      closePass();

    m_pass = PendingPass();
    m_pass.targets = std::move(targets);
    m_pass.pending = true;
    m_dirty |= DirtyPso | DirtyViewport;
  }

  void MtlContextImpl::clearRenderTarget(
    const Rc<MtlTextureView>& view, MtlAspectMask clear,
    MtlClearValue value, MtlAspectMask discard) {
    /* Fold into the pending pass when the view is one of its attachments
     * and the encoder hasn't opened; else flush as a mini-pass. */
    if (!m_pass.active && m_pass.pending) {
      for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
        if (m_pass.targets.color[i] == view && (clear & MtlAspectColor)) {
          m_pass.clearColor[i] = true;
          m_pass.clearValues[i] = value;
          return;
        }
      }
      if (m_pass.targets.depthStencil == view) {
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
      if (m_pass.clearColor[i]) {
        const auto& v = m_pass.clearValues[i];
        info.colors[i].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
      }
      residentTextureView(view);
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
      residentTextureView(m_pass.targets.depthStencil);
    }

    m_pass.encoder = MTL4CommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
    m_pass.active = true;
    m_pass.pending = false;
    m_dirty = DirtyEverything;

    /* Bind the per-stage argument tables once for the encoder's lifetime;
     * their CONTENTS change per draw but the table->stage binding is fixed. */
    bindArgumentTables();
  }

  void MtlContextImpl::bindArgumentTables() {
    MTL4RenderCommandEncoder_setArgumentTable(
      m_pass.encoder, m_argTable[0].handle(), uint64_t(WMTRenderStageVertex));
    MTL4RenderCommandEncoder_setArgumentTable(
      m_pass.encoder, m_argTable[1].handle(), uint64_t(WMTRenderStageFragment));
  }

  void MtlContextImpl::closePass() {
    if (!m_pass.active)
      return;

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
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }

  // --- state setters (fold + dirty) ------------------------------------------

  void MtlContextImpl::bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) {
    (stage == MtlShaderStage::Vertex ? m_vs : m_fs) = std::move(fn);
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setVertexLayout(
    uint32_t nAttr, const MtlVertexAttr* attrs,
    uint32_t nLayout, const MtlVertexLayout* layouts) {
    m_vertexAttrs.assign(attrs, attrs + nAttr);
    m_vertexLayouts.assign(layouts, layouts + nLayout);
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setPrimitiveTopology(MtlPrimitiveTopology topology) {
    m_topology = topology;   /* PSO topology CLASS only changes across point/line/tri */
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setViewports(uint32_t n, const MtlViewport* viewports) {
    if (n) m_viewport = viewports[0];
    m_dirty |= DirtyViewport;
  }

  void MtlContextImpl::setRasterizerState(const MtlRasterizerState& rs) {
    m_rasterizer = rs;
    m_dirty |= DirtyRasterizer;
  }

  void MtlContextImpl::setMultisampleState(const MtlMultisampleState& ms) {
    m_msaa = ms;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setDepthStencilState(const MtlDepthStencilState& ds) {
    m_depthStencil = ds;
    m_dirty |= DirtyDepthStencil;
  }

  void MtlContextImpl::setBlendMode(uint32_t rt, const MtlBlendMode& blend) {
    if (rt < 4) m_blend[rt] = blend;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setBlendColor(MtlColor4 color) {
    m_blendColor = color;
    m_dirty |= DirtyBlendColor;
  }

  void MtlContextImpl::setStencilReference(uint32_t reference) {
    m_stencilRef = reference;
    m_dirty |= DirtyStencilRef;
  }

  void MtlContextImpl::setDepthBias(MtlDepthBias bias) {
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
      }
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s))
        m_bindings[s].textures[slot] = view;
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s))
        m_bindings[s].samplers[slot] = sampler;
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& b = m_bindings[s];
        std::memcpy(b.pushData.data() + offset, data, size);
        b.pushDirtyLo = b.pushDirtyLo ? std::min(b.pushDirtyLo, offset) : offset;
        b.pushDirtyHi = std::max(b.pushDirtyHi, offset + size);
      }
    }
    m_dirty |= DirtyPush;
  }

  // --- draw path (decision 2+3) ----------------------------------------------

  void MtlContextImpl::draw(uint32_t count, const MtlDrawArgs* args) {
    if (!prepareDraw())
      return;
    for (uint32_t i = 0; i < count; i++) {
      MTL4RenderCommandEncoder_drawPrimitives(
        m_pass.encoder,
        wmtPrimitive(m_topology),
        args[i].firstVertex,
        args[i].vertexCount,
        args[i].instanceCount ? args[i].instanceCount : 1u,
        args[i].firstInstance);
    }
  }

  void MtlContextImpl::drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) {
    if (!prepareDraw())
      return;
    const Rc<MtlBuffer>& ib = m_indexBuffer.buffer();
    if (ib == nullptr)
      return;
    const uint64_t idxSize = (m_indexType == MtlIndexType::Uint16) ? 2u : 4u;
    const WMTIndexType idxType = (m_indexType == MtlIndexType::Uint16)
      ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;

    for (uint32_t i = 0; i < count; i++) {
      uint64_t byteOffset = m_indexBuffer.offset() + uint64_t(args[i].firstIndex) * idxSize;
      uint64_t gpuAddr    = ib->gpuAddress(byteOffset);
      /* MTL4 rule: indexBufferLength is the accessible byte range from the
       * given GPU address (to the end of the backing allocation). */
      uint64_t accessible = ib->size > byteOffset ? ib->size - byteOffset : 0u;
      MTL4RenderCommandEncoder_drawIndexedPrimitives(
        m_pass.encoder,
        wmtPrimitive(m_topology),
        args[i].indexCount,
        idxType,
        gpuAddr,
        accessible,
        args[i].instanceCount ? args[i].instanceCount : 1u,
        int64_t(args[i].vertexOffset),
        args[i].firstInstance);
    }
  }

  bool MtlContextImpl::prepareDraw() {
    if (!m_pass.pending && !m_pass.active)
      return false;                       /* no render targets bound */
    materializePass();

    if ((m_dirty & DirtyPso) && !resolvePso())
      return false;
    if (m_dirty & DirtyVertexBuffers)
      flushVertexBuffers();
    if (m_dirty & (DirtyResources | DirtyPush))
      flushResources();
    if (m_dirty & (DirtyViewport | DirtyDepthStencil | DirtyBlendColor
                 | DirtyStencilRef | DirtyDepthBias | DirtyRasterizer))
      flushDynamicState();
    m_pass.encodedDraws++;
    /* all folded state is now bound on the encoder / argument tables; the
     * next draw only re-flushes what changes (ring slices already snapshot). */
    m_dirty = 0u;
    return true;
  }

  // (resolvePso / bindArgumentTables contents / flushVertexBuffers /
  //  flushResources / flushDynamicState / transfers / init / queries / misc
  //  continue in mtl_context_impl2.cpp to keep TU size sane)

}
