/*
 * MtlWireContext — PE-side producer of the native-backend wire transport
 * (Phase 1, echo mode). See mtl_wire_context.h and driver/mtl/mtl_wire.h
 * for the design and the ring/kick/park contracts.
 */

#include "mtl_wire_context.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <new>

#include "mtl_device_impl.h"

#include "../../bridge/winemetal.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  namespace {
    /* 4 MiB of record space: ~64 B/record * ~10k hot records/frame stays
     * well under one ring turn per frame; the consumer only counts. */
    constexpr uint64_t WireRingCapacity = 4ull << 20;

    /* Only one ring per process; later contexts forward without recording. */
    std::atomic<bool> g_wireClaimed = { false };

    /* True once WireRing_attach has been issued (profiler stats source). */
    std::atomic<bool> g_wireAttached = { false };
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

    const uint32_t mode = WireRing_attach(m_ringMem, total);
    if (mode == WMTWireAttachFailed) {
      ::VirtualFree(m_ringMem, 0, MEM_RELEASE);
      m_ringMem = nullptr;
      Logger::err("MtlWireContext: wire attach failed, forwarding only");
      return;
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

    g_wireAttached.store(true, std::memory_order_release);
    if (m_hasConsumer) {
      Logger::info(str::format("MtlWireContext: render server attached, shm ring=",
        WireRingCapacity >> 20, " MiB (echo mode)"));
    } else {
      Logger::warn(str::format("MtlWireContext: no render server, local ring=",
        WireRingCapacity >> 20, " MiB with no consumer (disables on first ring-full)"));
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
    m_active = false;
  }

  /* --- ring producer ------------------------------------------------------ */

  bool MtlWireContext::waitForSpace(uint64_t need) {
    uint64_t tail = m_ring->tail.load(std::memory_order_acquire);
    if (m_head - tail + need <= m_cap)
      return true;

    /* Ring full: the consumer is starved (it only counts) or dead. Count
     * one stall episode, kick defensively, then spin. Give up after a
     * bounded wait so a dead consumer degrades to forward-only instead of
     * hanging the CS thread. */
    m_ring->stalls.fetch_add(1u, std::memory_order_relaxed);

    /* No server attached (WMTWireAttachLocal): nothing will EVER consume, so
     * a full ring is final — degrade to forward-only immediately instead of
     * spinning out the bound. */
    if (!m_hasConsumer) {
      Logger::warn("MtlWireContext: ring full with no consumer, disabling wire transport");
      m_active = false;
      return false;
    }
    WireRing_kick();

    for (uint32_t spin = 0u; spin < (1u << 20); spin++) {
      tail = m_ring->tail.load(std::memory_order_acquire);
      if (m_head - tail + need <= m_cap)
        return true;
      if ((spin & 0x3Fu) == 0x3Fu)
        ::Sleep(0);
    }

    Logger::err("MtlWireContext: ring stalled >1s, disabling wire transport");
    m_active = false;
    return false;
  }

  void* MtlWireContext::reserveBytes(uint16_t op, uint32_t bytes) {
    if (!m_active)
      return nullptr;

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
    if (auto* r = beginRecord<d9mt_wire_rec_bind_vertex_buffer>(d9mt_wire_op_bind_vertex_buffer)) {
      r->binding = binding;
      r->stride = stride;
      fillBufferRef(r->buf, slice);
      endRecord();
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
    m_inner->bindVertexBufferRange(binding, offset, length, stride);
  }

  void MtlWireContext::bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_index_buffer>(d9mt_wire_op_bind_index_buffer)) {
      fillBufferRef(r->buf, slice);
      r->index_type = uint32_t(type);
      endRecord();
    }
    m_inner->bindIndexBuffer(std::move(slice), type);
  }

  void MtlWireContext::bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) {
    if (auto* r = beginRecord<d9mt_wire_rec_bind_uniform_buffer>(d9mt_wire_op_bind_uniform_buffer)) {
      r->stages = stages;
      r->slot = slot;
      fillBufferRef(r->buf, slice);
      endRecord();
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
    m_inner->bindTexture(stages, slot, std::move(view));
  }

  void MtlWireContext::bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view) {
    m_inner->bindBufferView(stages, slot, std::move(view));
  }

  void MtlWireContext::bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) {
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
    m_inner->invalidateBuffer(buffer, std::move(slice));
  }

  void MtlWireContext::uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                    const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) {
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

/* --- PROF-line wire stats (declared in mtl_profiler.h) ---------------------
 * Called once per 256-frame profiler window; pulls the cumulative counters
 * via the WireRing_stats thunk and reports window deltas (decodeErr stays
 * cumulative — it must remain 0). hb is the server heartbeat delta over the
 * window (~100 Hz while the server lives; 0 = no server / dead server);
 * srvErr is the server's cumulative status error code (0 = ok). */
namespace dxvk::backend::prof {

  void wireAppendProfStats(std::string& line) {
    WMTWireStats s = { };
    if (g_wireAttached.load(std::memory_order_acquire))
      WireRing_stats(&s);
    static WMTWireStats prev = { };
    line += str::format(" | wire: rec=", s.records - prev.records,
      " bytes=", s.bytes - prev.bytes,
      " stalls=", s.stalls - prev.stalls,
      " decodeErr=", s.decode_errors,
      " hb=", s.heartbeat - prev.heartbeat,
      " srvErr=", s.server_error);
    prev = s;
  }

}
