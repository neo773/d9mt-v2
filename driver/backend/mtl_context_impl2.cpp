#include "mtl_context_impl.h"
#include "../util/util_string.h"

#include <algorithm>
#include <cstring>

#include "../util/log/log.h"

/*
 * mtl_context_impl2.cpp — part 2 of the METAL 4 MtlContextImpl (see
 * mtl_context_impl.h for the 9 architecture decisions). Sections: PSO resolve,
 * argument-table flush, dynamic state, transfers (compute-encoder copies),
 * init/upload/discard, queries, signals, lifetime, debug labels, and the
 * query/event result subclasses.
 */

namespace dxvk::backend {

  namespace {

    WMTPrimitiveTopologyClass topologyClass(MtlPrimitiveTopology t) {
      switch (t) {
        case MtlPrimitiveTopology::PointList:
          return WMTPrimitiveTopologyClassPoint;
        case MtlPrimitiveTopology::LineList:
        case MtlPrimitiveTopology::LineStrip:
          return WMTPrimitiveTopologyClassLine;
        default:
          return WMTPrimitiveTopologyClassTriangle;
      }
    }

    /* MtlVertexAttr carries a pixel-format enum; the Metal vertex descriptor
     * wants an MTLVertexFormat (WMTAttributeFormat). Map the D3D9 vertex decl
     * formats that reach this seam; default to Float4 (loud-but-safe). */
    WMTAttributeFormat attributeFormat(MtlFormat f) {
      switch (f) {
        case MtlFormat::R32Float:     return WMTAttributeFormatFloat;
        case MtlFormat::RG32Float:    return WMTAttributeFormatFloat2;
        case MtlFormat::RGB32Float:   return WMTAttributeFormatFloat3;
        case MtlFormat::RGBA32Float:  return WMTAttributeFormatFloat4;
        case MtlFormat::RGBA8Unorm:   return WMTAttributeFormatUChar4Normalized;
        case MtlFormat::BGRA8Unorm:   return WMTAttributeFormatUChar4Normalized_BGRA;
        case MtlFormat::RGBA8Snorm:   return WMTAttributeFormatChar4Normalized;
        case MtlFormat::RG16Float:    return WMTAttributeFormatHalf2;
        case MtlFormat::RGBA16Float:  return WMTAttributeFormatHalf4;
        case MtlFormat::R16Float:     return WMTAttributeFormatHalf;
        case MtlFormat::RGBA16Snorm:  return WMTAttributeFormatShort4Normalized;
        case MtlFormat::RGBA16Unorm:  return WMTAttributeFormatUShort4Normalized;
        case MtlFormat::RG16Snorm:    return WMTAttributeFormatShort2Normalized;
        case MtlFormat::RG16Unorm:    return WMTAttributeFormatUShort2Normalized;
        case MtlFormat::RGB10A2Unorm: return WMTAttributeFormatUInt1010102Normalized;
        default:                      return WMTAttributeFormatFloat4;
      }
    }

    void logOnce(const char* what) {
      static bool s_logged = false;
      if (!s_logged) { s_logged = true; Logger::warn(what); }
    }

  }

  // --- query / event result surface (subclasses declared in the header) ------

  MtlGpuQueryStatus MtlBackendQuery::getData(MtlQueryData& out) {
    if (tracker && serial && !tracker->isComplete(serial))
      return MtlGpuQueryStatus::Pending;

    out = MtlQueryData{};
    if (type == MtlQueryType::Occlusion && visCpu)
      out.occlusion.samplesPassed =
        *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(visCpu) + slotOffset);
    else
      out.timestamp.time = serial;
    return MtlGpuQueryStatus::Available;
  }

  MtlGpuEventStatus MtlBackendEvent::test() {
    if (tracker && serial && !tracker->isComplete(serial))
      return MtlGpuEventStatus::Pending;
    return MtlGpuEventStatus::Signaled;
  }

  // --- PSO resolve (decision 9) ----------------------------------------------

  bool MtlContextImpl::resolvePso() {
    if (m_vs == nullptr)
      return false;

    obj_handle_t vsLibrary = m_vs->library;
    obj_handle_t fsLibrary = m_fs != nullptr ? obj_handle_t(m_fs->library) : 0u;
    if (!vsLibrary)
      return false;

    RenderPsoKey key = {};
    key.vsCookie = uint64_t(m_vs->library);
    key.fsCookie = m_fs != nullptr ? uint64_t(m_fs->library) : 0u;

    uint32_t nAttr = std::min<uint32_t>(uint32_t(m_vertexAttrs.size()), 20u);
    for (uint32_t i = 0u; i < nAttr; i++) {
      const MtlVertexAttr& src = m_vertexAttrs[i];
      PsoVertexAttr& a = key.attrs[i];
      a.format      = uint32_t(attributeFormat(src.format));
      a.offset      = uint16_t(src.offset);
      a.bufferIndex = uint8_t(kVertexBufferBaseIndex + src.binding);
    }
    key.attrCount = uint8_t(nAttr);

    uint32_t nLayout = std::min<uint32_t>(uint32_t(m_vertexLayouts.size()), 20u);
    for (uint32_t i = 0u; i < nLayout; i++) {
      const MtlVertexLayout& src = m_vertexLayouts[i];
      PsoVertexLayout& l = key.layouts[i];
      /* D3D9's authoritative per-stream stride comes from SetStreamSource, kept
       * live in m_vertexStreams. The frontend-supplied layout stride can be 0
       * (declaration bound before the stream, or not carried through), and a 0
       * stride makes Metal assert "buffer at index N has no stride" — GTA IV
       * world geometry hits this and the device dies. Prefer the live stream
       * stride whenever we have one. */
      uint32_t strideVal = src.stride;
      if (src.binding < m_vertexStreams.size()
       && m_vertexStreams[src.binding].stride != 0u)
        strideVal = m_vertexStreams[src.binding].stride;
      l.stride       = uint16_t(strideVal);
      l.stepFunction = src.perInstance ? 2u : 1u;
      l.pad          = uint8_t(kVertexBufferBaseIndex + src.binding);  /* buffer idx */
    }
    key.layoutCount = uint8_t(nLayout);

    uint8_t colorCount = 0u;
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      const Rc<MtlTextureView>& view = m_pass.targets.color[i];
      if (view == nullptr)
        continue;
      PsoColorAttachment& c = key.colors[i];
      const MtlBlendMode& b = m_blend[i];
      c.pixelFormat    = uint32_t(view->key.format);
      c.blendEnabled   = b.enable ? 1u : 0u;
      c.writeMask      = uint8_t(b.writeMask & 0xfu);
      c.rgbSrcFactor   = uint8_t(b.colorSrc);
      c.rgbDstFactor   = uint8_t(b.colorDst);
      c.rgbBlendOp     = uint8_t(b.colorOp);
      c.alphaSrcFactor = uint8_t(b.alphaSrc);
      c.alphaDstFactor = uint8_t(b.alphaDst);
      c.alphaBlendOp   = uint8_t(b.alphaOp);
      colorCount = uint8_t(i + 1u);
    }
    key.colorCount = colorCount;

    key.sampleCount     = uint8_t(m_msaa.sampleCount ? m_msaa.sampleCount : 1u);
    key.topologyClass   = uint8_t(topologyClass(m_topology));
    key.alphaToCoverage = m_msaa.alphaToCoverage ? 1u : 0u;
    /* depthFormat/stencilFormat stay 0 — Metal 4 omits them from the pipeline
     * descriptor, so they must not add spurious PSO variants (decision 9). */

    const char* vsEntry = m_vs->entryName ? m_vs->entryName : "main";
    const char* fsEntry = (m_fs != nullptr && m_fs->entryName) ? m_fs->entryName : "main";
    obj_handle_t pso = m_psoCache.get(key, vsLibrary, fsLibrary, vsEntry, fsEntry);
    if (!pso)
      return false;

    MTL4RenderCommandEncoder_setRenderPipelineState(m_pass.encoder, pso);
    return true;
  }

  // --- argument-table flush (decision 3) -------------------------------------

  void MtlContextImpl::flushVertexBuffers() {
    obj_handle_t table = m_argTables[m_currentArgTable].table[0].handle();
    for (uint32_t i = 0u; i < m_vertexStreams.size(); i++) {
      VertexStream& s = m_vertexStreams[i];
      if (!s.slice.defined()) {
        MTL4ArgumentTable_setAddress(table, 0u, kVertexBufferBaseIndex + i);
        continue;
      }
      const Rc<MtlBuffer>& buf = s.slice.buffer();
      if (buf == nullptr) {
        MTL4ArgumentTable_setAddress(table, 0u, kVertexBufferBaseIndex + i);
        continue;
      }
      makeResident(buf->handle.buf);
      MTL4ArgumentTable_setAddress(
        table, buf->gpuAddress(s.slice.offset()), kVertexBufferBaseIndex + i);
    }
  }

  void MtlContextImpl::flushResources() {
    for (uint32_t st = 0u; st < 2u; st++) {
      StageBindings& b = m_bindings[st];
      obj_handle_t table = m_argTables[m_currentArgTable].table[st].handle();

      /* The bound shader for this stage carries the CbvIndex -> AIR Metal
       * buffer-slot map (reflection-derived, SOURCE_OF_TRUTH §4b bug 1). The
       * frontend binds CBVs at raw CbvIndex; we translate to the slot the AIR
       * shader actually declares so CBVs land at 0..2 (or wherever air_builder
       * put them) and never collide with vertex streams (kVertexBufferBaseIndex)
       * or push data (kPushConstantIndex). */
      const Rc<MtlShaderFunction>& fn = (st == 0u) ? m_vs : m_fs;

      /* constant buffers at their AIR slots, with the discard/range offset
       * folded in (bindUniformBufferRange snapshot). */
      for (uint32_t slot = 0u; slot < b.cbvs.size(); slot++) {
        StageBindings::CbvBinding& cbv = b.cbvs[slot];
        /* Only bind CBVs the shader actually declares, at their AIR slot. */
        int32_t airSlot = (fn != nullptr && slot < fn->cbvSlotMap.size())
          ? int32_t(fn->cbvSlotMap[slot]) : -1;
        if (airSlot < 0)
          continue;

        if (!cbv.slice.defined()) {
          MTL4ArgumentTable_setAddress(table, 0u, uint32_t(airSlot));
          continue;
        }

        const Rc<MtlBuffer>& buf = cbv.slice.buffer();
        if (buf == nullptr) {
          MTL4ArgumentTable_setAddress(table, 0u, uint32_t(airSlot));
          continue;
        }
        /* Defense-in-depth: a CBV whose MtlBuffer has null/garbage storage
         * would fault on gpuAddress. With the reflection-driven mapping this
         * should never trigger (we only bind declared CBVs). */
        if (buf->storage() == nullptr
         || uintptr_t(buf->storage().ptr()) < 0x10000u) {
          logOnce("MtlContextImpl: skipped CBV with no backing storage "
                  "(slot-mapping/lifetime bug — see SOURCE_OF_TRUTH)");
          MTL4ArgumentTable_setAddress(table, 0u, uint32_t(airSlot));
          continue;
        }
        makeResident(buf->handle.buf);
        MTL4ArgumentTable_setAddress(
          table, buf->gpuAddress(cbv.slice.offset() + cbv.rangeOffset),
          uint32_t(airSlot));
      }

      /* textures: setTexture by MTLResourceID into this stage's table. An
       * unbound slot must be CLEARED, not skipped — the argument table persists
       * across draws, so a stale binding (often a recycled dynamic/UP buffer)
       * would leak into a later draw that samples that slot -> flicker/ghosting
       * in multi-draw scenes (single-draw static tests never expose it). */
      for (uint32_t slot = 0u; slot < b.textures.size(); slot++) {
        const Rc<MtlTextureView>& view = b.textures[slot];
        if (view == nullptr) {
          MTL4ArgumentTable_setTexture(table, 0u, slot);
          continue;
        }
        auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
        residentTextureView(view);
        MTL4ArgumentTable_setTexture(table, tv->gpuResourceId, slot);
      }

      /* samplers: setSamplerState by MTLResourceID. */
      for (uint32_t slot = 0u; slot < b.samplers.size(); slot++) {
        const Rc<MtlSamplerState>& sampler = b.samplers[slot];
        if (sampler == nullptr) {
          MTL4ArgumentTable_setSamplerState(table, 0u, slot);
          continue;
        }
        auto* sp = static_cast<MtlSamplerStateImpl*>(sampler.ptr());
        MTL4ArgumentTable_setSamplerState(table, sp->gpuResourceId, slot);
      }

      /* push data -> fresh UploadRing slice at the push slot (snapshot
       * semantics: the table reads the gpuAddress at the draw snapshot, and
       * this slice's memory is never rewritten under a queued draw). */
      if (b.pushDirtyHi > 0u) {
        uint64_t sz = b.pushDirtyHi;
        RingSlice rs = m_device->uploadRing().allocate(sz, 256u);
        if (rs.cpuPtr) {
          std::memcpy(rs.cpuPtr, b.pushData.data(), sz);
          makeResident(rs.buffer);
          b.pushGpuAddress = rs.gpuAddress;
        }
        b.pushDirtyLo = b.pushDirtyHi = 0u;
      }
      MTL4ArgumentTable_setAddress(table, b.pushGpuAddress, kPushConstantIndex);
    }
  }

  void MtlContextImpl::flushDynamicState() {
    obj_handle_t enc = m_pass.encoder;

    /* viewport (RT-pixel coords; backing scale handled in the presenter) */
    {
      WMTViewport vp = {};
      vp.originX = m_viewport.originX;
      vp.originY = m_viewport.originY;
      vp.width   = m_viewport.width;
      vp.height  = m_viewport.height;
      vp.znear   = m_viewport.znear;
      vp.zfar    = m_viewport.zfar;
      MTL4RenderCommandEncoder_setViewport(enc, vp);
    }

    /* scissor — full render target when none is set */
    {
      MtlRect sc = m_viewport.scissor;
      if (sc.width == 0u || sc.height == 0u) {
        uint32_t w = 0u, h = 0u;
        for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets && !w; i++) {
          const Rc<MtlTextureView>& v = m_pass.targets.color[i];
          if (v != nullptr && v->image != nullptr) { w = v->image->extent.width; h = v->image->extent.height; }
        }
        if (!w && m_pass.targets.depthStencil != nullptr && m_pass.targets.depthStencil->image != nullptr) {
          w = m_pass.targets.depthStencil->image->extent.width;
          h = m_pass.targets.depthStencil->image->extent.height;
        }
        if (!w) { w = uint32_t(m_viewport.width); h = uint32_t(m_viewport.height); }
        sc = { 0u, 0u, w, h };
      }
      WMTScissorRect rect = {};
      rect.x      = sc.x;
      rect.y      = sc.y;
      rect.width  = sc.width;
      rect.height = sc.height;
      MTL4RenderCommandEncoder_setScissorRect(enc, rect);
    }

    /* depth-stencil state object (cached in the device) + stencil ref */
    {
      obj_handle_t dsso = m_device->getDepthStencilState(m_depthStencil);
      MTL4RenderCommandEncoder_setDepthStencilState(enc, dsso);
      MTL4RenderCommandEncoder_setStencilReferenceValue(enc, m_stencilRef);
    }

    /* blend constant */
    MTL4RenderCommandEncoder_setBlendColor(
      enc, m_blendColor.r, m_blendColor.g, m_blendColor.b, m_blendColor.a);

    /* rasterizer: cull / winding / depth-bias (fill mode + depth-clip use the
     * Metal defaults — no MTL4 bridge setter, and D3D9 default is Fill/Clip). */
    MTL4RenderCommandEncoder_setCullMode(enc, WMTCullMode(m_rasterizer.cullMode));
    MTL4RenderCommandEncoder_setFrontFacingWinding(enc, WMTWinding(m_rasterizer.frontFace));
    MTL4RenderCommandEncoder_setDepthBias(
      enc, m_depthBias.constantFactor, m_depthBias.slopeFactor, m_depthBias.clamp);
  }

  void MtlContextImpl::makeResident(obj_handle_t handle) {
    /* Most resources are already resident from creation (device add-at-alloc).
     * This covers the exceptions — borrowed handles the device never created:
     * the CAMetalLayer drawable texture and the visibility buffer. The device
     * residency set is a set (addAllocation idempotent), so re-adding an
     * already-resident created resource is a harmless no-op; the local dedup
     * just avoids the unix round-trip. Never cleared → never wiped. */
    if (!handle)
      return;
    for (obj_handle_t h : m_residentSet)
      if (h == handle)
        return;
    m_residentSet.push_back(handle);
    m_device->residencyAdd(handle);
  }

  void MtlContextImpl::residentTextureView(const Rc<MtlTextureView>& view) {
    if (view == nullptr)
      return;
    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    /* Track the underlying image allocation (the view shares its storage);
     * fall back to the view handle for drawable-backed attachments whose base
     * image carries no owned allocation. */
    if (view->image != nullptr) {
      auto* base = static_cast<MtlTextureImpl*>(view->image.ptr());
      makeResident(base->owned.handle() ? base->owned.handle() : base->texture.tex);
    }
    makeResident(tv->owned.handle());
  }

  // --- transfers / blits (decision 8: MTL4 compute-encoder copies) ------------

  obj_handle_t MtlContextImpl::computeEncoder() {
    if (m_pass.active)
      closePass();
    else
      flushPendingClears();   /* a copy must observe folded, not-yet-drawn clears */
    ensureCommandBuffer();
    return MTL4CommandBuffer_computeCommandEncoder(m_cmdbuf.handle());
  }

  void MtlContextImpl::emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages) {
    if (!encoder)
      return;
    /* Producer queue barrier: order `afterStages` on this (outgoing) encoder
     * ahead of ALL later queue stages, flushing writes to device memory so a
     * later encoder's read is coherent (SOURCE_OF_TRUTH §4c). */
    MTL4CommandEncoder_barrier(encoder, afterStages, uint64_t(WMTStageAll),
                               uint64_t(WMTVisibilityOptionDevice));
  }

  void MtlContextImpl::emitGraphicsBarrier(MtlStageMask, MtlBarrierScope,
                                           MtlStageMask, MtlBarrierScope) {
    /* Frontend hazard hook (d3d9_device feedback-loop path). Our coarse
     * per-encoder producer barriers (closePass + each transfer) already order
     * render/compute writes ahead of later reads across encoders; a same-
     * encoder RT->texture feedback loop is handled by re-materializing the
     * pass. If a pass is open, flush its fragment writes to device memory so
     * an immediately following read in a fresh encoder is coherent. */
    if (m_pass.active)
      emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
  }

  void MtlContextImpl::copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                  const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    obj_handle_t enc = computeEncoder();
    makeResident(s->handle.buf);
    makeResident(d->handle.buf);
    MTL4ComputeCommandEncoder_copyFromBufferToBuffer(
      enc, s->handle.buf, srcOff, d->handle.buf, dstOff, bytes);
    /* §4c: make this copy's write visible to the later draw/copy that reads it
     * (e.g. a DEFAULT-pool vertex buffer uploaded here then fetched by the VS). */
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                    const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) {
    copyBuffer(dst, dstOff, staging, srcOff, size);
  }

  void MtlContextImpl::copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                                   const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    obj_handle_t enc = computeEncoder();
    /* Drawable-backed textures carry only a borrowed handle (owned unset) —
     * they still MUST be resident for the GPU write (CAMetalLayer drawables
     * are not implicitly resident on an MTL4 queue). */
    makeResident(s->owned.handle() ? s->owned.handle() : s->texture.tex);
    makeResident(d->owned.handle() ? d->owned.handle() : d->texture.tex);
    WMTOrigin srcOrigin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   srcSize   = { extent.width, extent.height, extent.depth };
    WMTOrigin dstOrigin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    MTL4ComputeCommandEncoder_copyFromTextureToTexture(
      enc, s->texture.tex, srcSub.baseLayer, srcSub.mipLevel, srcOrigin, srcSize,
      d->texture.tex, dstSub.baseLayer, dstSub.mipLevel, dstOrigin);
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  namespace {
    /* rowAlign/sliceAlign are ALIGNMENTS (DXVK semantics), not pitches. Metal
     * wants bytesPerRow/bytesPerImage — compute the tight pitch from the
     * format's block geometry, then round up. Callers that pass a full pitch
     * as the alignment still get that pitch back (align(tight, pitch) = pitch
     * whenever tight <= pitch). */
    void computeBufferImagePitch(
        MtlFormat format, MtlSize extent,
        MtlDeviceSize rowAlign, MtlDeviceSize sliceAlign,
        MtlDeviceSize& bytesPerRow, MtlDeviceSize& bytesPerImage) {
      const MtlFormatInfo* fi = lookupFormatInfo(format);
      const uint32_t elementSize = fi->elementSize ? fi->elementSize : 4u;
      const uint32_t bw = fi->blockWidth  ? fi->blockWidth  : 1u;
      const uint32_t bh = fi->blockHeight ? fi->blockHeight : 1u;
      auto alignUp = [](MtlDeviceSize v, MtlDeviceSize a) {
        return a > 1u ? (v + a - 1u) / a * a : v;
      };
      bytesPerRow   = alignUp(MtlDeviceSize((extent.width + bw - 1u) / bw) * elementSize, rowAlign);
      bytesPerImage = extent.depth > 1u
        ? alignUp(bytesPerRow * ((extent.height + bh - 1u) / bh), sliceAlign)
        : 0u;
    }
  }

  void MtlContextImpl::copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                                           const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    obj_handle_t enc = computeEncoder();
    makeResident(s->handle.buf);
    makeResident(d->owned.handle());
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    WMTOrigin origin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : d->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    MTL4ComputeCommandEncoder_copyFromBufferToTexture(
      enc, s->handle.buf, off, bytesPerRow, bytesPerImage, size,
      d->texture.tex, dstSub.baseLayer, dstSub.mipLevel, origin);
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                                           MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    obj_handle_t enc = computeEncoder();
    makeResident(s->owned.handle());
    makeResident(d->handle.buf);
    WMTOrigin origin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : s->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    MTL4ComputeCommandEncoder_copyFromTextureToBuffer(
      enc, s->texture.tex, srcSub.baseLayer, srcSub.mipLevel, origin, size,
      d->handle.buf, off, bytesPerRow, bytesPerImage);
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::blitTextureView(const Rc<MtlTextureView>&, const MtlOffset3D[2],
                                       const Rc<MtlTextureView>&, const MtlOffset3D[2], MtlSamplerFilter) {
    logOnce("MtlContextImpl::blitTextureView — meta-blit pipeline unimplemented (LOW priority)");
  }

  void MtlContextImpl::generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter) {
    if (view == nullptr)
      return;
    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    obj_handle_t tex = view->image != nullptr
      ? static_cast<MtlTextureImpl*>(view->image.ptr())->owned.handle()
      : tv->owned.handle();
    if (!tex)
      return;
    obj_handle_t enc = computeEncoder();
    makeResident(tex);
    MTL4ComputeCommandEncoder_generateMipmaps(enc, tex);
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  // --- init / upload / discard (contracts 3a,3c,3g) --------------------------

  void MtlContextImpl::initBuffer(const Rc<MtlBuffer>& buffer) {
    auto* b = static_cast<MtlBufferImpl*>(buffer.ptr());
    if (!b)
      return;
    obj_handle_t enc = computeEncoder();
    makeResident(b->handle.buf);
    MTL4ComputeCommandEncoder_fillBuffer(enc, b->handle.buf, 0u, b->size, 0u);
    emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::initImage(const Rc<MtlTexture>& image) {
    auto* t = static_cast<MtlTextureImpl*>(image.ptr());
    if (!t)
      return;

    /* initImage clears via a load=Clear render pass — that requires the
     * texture to be a valid attachment. Sampled-only textures (no
     * RenderTarget usage) and compressed formats (BC1-3 are never
     * color-renderable) cannot be attachments; a pass built on them is an
     * invalid descriptor that FAULTS the GPU and poisons the queue (every
     * subsequent upload/draw silently drops → black textures). Such textures
     * get their defined content from uploads — skip the clear. */
    const bool renderable = (uint32_t(t->info.usage)
      & (uint32_t(MtlUsage::RenderTarget) | uint32_t(MtlUsage::DepthStencil))) != 0u;
    if (!renderable)
      return;

    if (m_pass.active)
      closePass();
    ensureCommandBuffer();

    const MtlFormatInfo* fi = lookupFormatInfo(t->format);
    const bool depthStencil = (fi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil)) != 0u;

    WMTRenderPassInfo info = {};
    info.render_target_width  = t->extent.width;
    info.render_target_height = t->extent.height;

    if (depthStencil) {
      info.depth.texture      = t->owned.handle();
      info.depth.load_action  = WMTLoadActionClear;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.clear_depth  = 1.0f;
      if (fi->flags & MtlFormatFlagStencil) {
        info.stencil.texture      = t->owned.handle();
        info.stencil.load_action  = WMTLoadActionClear;
        info.stencil.store_action = WMTStoreActionStore;
        info.stencil.clear_stencil = 0u;
      }
    } else {
      info.colors[0].texture      = t->owned.handle();
      info.colors[0].load_action  = WMTLoadActionClear;
      info.colors[0].store_action = WMTStoreActionStore;
      info.colors[0].clear_color  = { 0.0, 0.0, 0.0, 0.0 };
    }

    makeResident(t->owned.handle());

    /* load=Clear + store=Store clears the whole subresource with no draws. */
    obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
    /* §4c: order the clear's fragment write ahead of a later sample/copy. */
    emitProducerBarrier(enc, uint64_t(WMTStageFragment));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::invalidateBuffer(const Rc<MtlBuffer>&, Rc<MtlAllocation>&& slice) {
    /* Contract 3a / decision 6: the OLD backing rides the retirement spine and
     * releases only once the last submission referencing it completes. */
    if (slice == nullptr)
      return;
    ensureCommandBuffer();
    Rc<MtlAllocation> keep = std::move(slice);
    m_device->submissions().retireAfter(m_currentSerial, [keep]() mutable { keep = nullptr; });
  }

  bool MtlContextImpl::ensureTextureUsage(const Rc<MtlTexture>&, MtlTextureUsage) {
    /* Usage is set liberally at image creation (mtl_device_impl textureUsage). */
    return true;
  }

  // --- queries (contract 3d) -------------------------------------------------

  void MtlContextImpl::ensureVisBuffer() {
    if (m_visBuffer)
      return;
    WMTBufferInfo info = {};
    info.length  = 8192u * 8u;   /* 8192 visibility counters */
    info.options = WMTResourceOptions(WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
    info.memory.set(nullptr);
    obj_handle_t h = MTLDevice_newBuffer(m_device->wmtDevice(), &info);
    m_visBuffer = WmtBuffer::adopt(h);
    m_visCpu    = info.memory.get();
    /* The visibility buffer is read by the GPU across every pass — keep it
     * resident for the context lifetime. */
    makeResident(h);
  }

  void MtlContextImpl::beginQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q || q->type != MtlQueryType::Occlusion)
      return;
    ensureVisBuffer();
    q->visBuffer  = m_visBuffer.handle();
    q->visCpu     = m_visCpu;
    q->slotOffset = (m_visSlot % 8192u) * 8u;
    q->tracker    = &m_device->submissions();
    m_visSlot++;
    if (m_visCpu)
      *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(m_visCpu) + q->slotOffset) = 0u;

    if (m_pass.active)
      MTL4RenderCommandEncoder_setVisibilityResultMode(
        m_pass.encoder, uint32_t(WMTVisibilityResultModeCounting), q->slotOffset);
  }

  void MtlContextImpl::endQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();

    if (m_pass.active)
      MTL4RenderCommandEncoder_setVisibilityResultMode(
        m_pass.encoder, uint32_t(WMTVisibilityResultModeDisabled), 0u);
  }

  void MtlContextImpl::writeTimestamp(const Rc<MtlQuery>& query) {
    /* EVENT/timestamp query: answer = has the recording submission completed. */
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();
  }

  // --- signals / events (contracts 3d,3e) ------------------------------------

  void MtlContextImpl::signal(const Rc<MtlSignal>& signal, uint64_t value) {
    (void)value;
    ensureCommandBuffer();
    Rc<MtlSignal> keep = signal;
    m_device->submissions().retireAfter(m_currentSerial, [keep]() { (void)keep; });
  }

  void MtlContextImpl::signalGpuEvent(const Rc<MtlEvent>& event) {
    auto* e = static_cast<MtlBackendEvent*>(event.ptr());
    if (!e)
      return;
    ensureCommandBuffer();
    e->serial  = m_currentSerial;
    e->tracker = &m_device->submissions();
  }

  // --- lifetime / residency (contract 3h) ------------------------------------

  void MtlContextImpl::track(Rc<MtlResource>&& resource) {
    if (resource != nullptr)
      m_keepAlive.push_back(std::move(resource));
  }

  // --- debug labels ----------------------------------------------------------

  void MtlContextImpl::beginDebugLabel(const char*)  { }
  void MtlContextImpl::endDebugLabel()               { }
  void MtlContextImpl::insertDebugLabel(const char*) { }

}
