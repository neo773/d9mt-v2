#include "mtl_context_impl.h"

#include <algorithm>
#include <cstring>

#include "../util/log/log.h"

/*
 * Implementation notes live in mtl_context_impl.h (the 9 architecture
 * decisions). Section order mirrors the header's flow: lifecycle -> pass ->
 * state -> binding -> draw -> transfer -> init -> queries -> misc.
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
    m_psoCache(m_device->wmtDevice()) {
  }

  MtlContextImpl::~MtlContextImpl() {
    if (m_pass.active)
      closePass();
  }

  // --- lifecycle -----------------------------------------------------------

  void MtlContextImpl::beginRecording() {
    m_dirty = DirtyEverything;
    ensureCommandBuffer();
  }

  void MtlContextImpl::ensureCommandBuffer() {
    if (m_cmdbuf)
      return;

    m_cmdbuf = WmtCommandBuffer::retain(
      MTLCommandQueue_commandBuffer(m_device->wmtQueue()));
    m_currentSerial = m_device->submissions().nextSerial();
  }

  void MtlContextImpl::submit(bool waitCompleted) {
    if (m_pass.active)
      closePass();

    if (!m_cmdbuf)
      return;

    uint64_t serial = m_device->submissions().onSubmit(m_cmdbuf.handle());
    MTLCommandBuffer_commit(m_cmdbuf.handle());

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

    if (waitCompleted)
      m_device->submissions().waitFor(serial);

    m_cmdbuf.reset();
    m_residentSet.clear();
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
    /* Contract 3e: the presenter's frame-latency signal must advance every
     * frame or the frame limiter deadlocks. Present path: close everything,
     * hand the command buffer to the presenter for drawable scheduling. */
    if (m_presenter == nullptr) {
      Logger::warn("MtlContextImpl: synchronizeWsi without presenter");
      return;
    }

    if (m_pass.active)
      closePass();
    ensureCommandBuffer();

    /* Acquire the drawable and schedule its present on THIS command buffer
     * (presentDrawable must be recorded before commit). The presenter's
     * present()/signalFrameLatency() then wire the release token + advance
     * the frame-latency signal (contract 3e). */
    MtlAcquiredDrawable image = m_presenter->acquireNextImage(sync.frameId);
    if (image.valid && image.drawable)
      MTLCommandBuffer_presentDrawable(m_cmdbuf.handle(), image.drawable);

    submit(false);

    if (image.valid)
      m_presenter->present(image);
    m_presenter->signalFrameLatency(sync.frameId);
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

    /* residency is per-encoder in Metal — start each pass with a clean dedup
     * set so every shader-read resource re-emits its useResource here. */
    m_residentSet.clear();

    WMTRenderPassInfo info = {};
    /* Occlusion queries write into a per-context visibility buffer bound at
     * pass creation (decision 7); attach it to every pass so beginQuery only
     * has to flip the visibility mode. */
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
      makeResident(impl->owned.handle());
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
      makeResident(impl->owned.handle());
    }

    m_pass.encoder = MTLCommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
    m_pass.active = true;
    m_pass.pending = false;
    m_dirty = DirtyEverything;
  }

  void MtlContextImpl::closePass() {
    if (!m_pass.active)
      return;

    if (!m_chunk.empty()) {
      MTLRenderCommandEncoder_encodeCommands(m_pass.encoder, m_chunk.head());
      m_chunk.reset();
    }
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
      auto* cmd = m_chunk.append<wmtcmd_render_draw>(WMTRenderCommandDraw);
      cmd->primitive_type = wmtPrimitive(m_topology);
      cmd->vertex_start = args[i].firstVertex;
      cmd->vertex_count = args[i].vertexCount;
      cmd->instance_count = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_instance = args[i].firstInstance;
    }
  }

  void MtlContextImpl::drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) {
    if (!prepareDraw())
      return;
    auto* ib = static_cast<MtlBufferImpl*>(m_indexBuffer.buffer().ptr());
    if (!ib)
      return;
    for (uint32_t i = 0; i < count; i++) {
      auto* cmd = m_chunk.append<wmtcmd_render_draw_indexed>(WMTRenderCommandDrawIndexed);
      cmd->primitive_type = wmtPrimitive(m_topology);
      cmd->index_type = m_indexType == MtlIndexType::Uint16 ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;
      cmd->index_buffer = ib->handle.buf;
      cmd->index_buffer_offset = m_indexBuffer.offset()
        + uint64_t(args[i].firstIndex) * (m_indexType == MtlIndexType::Uint16 ? 2u : 4u);
      cmd->index_count = args[i].indexCount;
      cmd->instance_count = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_vertex = args[i].vertexOffset;
      cmd->base_instance = args[i].firstInstance;
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
    /* all folded state is now encoded into the chunk; the next draw only
     * re-flushes what changes (snapshot slices already captured). */
    m_dirty = 0u;
    return true;
  }

  // (resolvePso / flushVertexBuffers / flushResources / flushDynamicState /
  //  transfer section / init / queries / misc — continued in
  //  mtl_context_impl2.cpp to keep TU size sane)

}
