#include "mtl_context_impl.h"

#include <algorithm>
#include <cstring>

#include "../util/log/log.h"

/*
 * mtl_context_impl2.cpp — part 2 of MtlContextImpl (see mtl_context_impl.h for
 * the 9 architecture decisions). Sections: PSO resolve, argument-table flush,
 * dynamic state, transfers/blits, init/upload/discard, queries, signals,
 * lifetime, debug labels, and the query/event result subclasses.
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

  // --- resolve helpers -------------------------------------------------------

  obj_handle_t MtlContextImpl::resolveFunction(const Rc<MtlShaderFunction>& fn) {
    if (fn == nullptr || !fn->library)
      return 0u;

    auto it = m_fnCache.find(fn->library);
    if (it != m_fnCache.end())
      return it->second.handle();

    /* AIR metallib entry point is "main" (v2/air air_builder + air_render_test);
     * v1's SPIRV-Cross path used "main0" — this is the AIR path. */
    obj_handle_t f = MTLLibrary_newFunction(fn->library, "main");
    if (!f) {
      Logger::err("MtlContextImpl: shader entry 'main' not found in library");
      return 0u;
    }
    auto res = m_fnCache.emplace(fn->library, WmtFunction::adopt(f));
    return res.first->second.handle();
  }

  uint32_t MtlContextImpl::currentDepthFormat() const {
    const Rc<MtlTextureView>& dv = m_pass.targets.depthStencil;
    return dv != nullptr ? uint32_t(dv->key.format) : 0u;
  }

  bool MtlContextImpl::resolvePso() {
    if (m_vs == nullptr)
      return false;

    obj_handle_t vsFn = resolveFunction(m_vs);
    obj_handle_t fsFn = m_fs != nullptr ? resolveFunction(m_fs) : 0u;
    if (!vsFn)
      return false;

    RenderPsoKey key = {};
    key.vsCookie = uint64_t(m_vs->library);
    key.fsCookie = m_fs != nullptr ? uint64_t(m_fs->library) : 0u;

    uint32_t nAttr = std::min<uint32_t>(uint32_t(m_vertexAttrs.size()), 16u);
    for (uint32_t i = 0u; i < nAttr; i++) {
      const MtlVertexAttr& src = m_vertexAttrs[i];
      PsoVertexAttr& a = key.attrs[i];
      a.format      = uint32_t(attributeFormat(src.format));
      a.offset      = uint16_t(src.offset);
      a.bufferIndex = uint8_t(kVertexBufferBaseIndex + src.binding);
    }
    key.attrCount = uint8_t(nAttr);

    uint32_t nLayout = std::min<uint32_t>(uint32_t(m_vertexLayouts.size()), 16u);
    for (uint32_t i = 0u; i < nLayout; i++) {
      const MtlVertexLayout& src = m_vertexLayouts[i];
      PsoVertexLayout& l = key.layouts[i];
      l.stride       = uint16_t(src.stride);
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
    key.depthFormat     = currentDepthFormat();

    if (m_pass.targets.depthStencil != nullptr) {
      const MtlFormatInfo* fi = lookupFormatInfo(m_pass.targets.depthStencil->key.format);
      if (fi->flags & MtlFormatFlagStencil)
        key.stencilFormat = key.depthFormat;
    }

    obj_handle_t pso = m_psoCache.get(key, vsFn, fsFn);
    if (!pso)
      return false;

    auto* cmd = m_chunk.append<wmtcmd_render_setpso>(WMTRenderCommandSetPSO);
    cmd->pso = pso;
    return true;
  }

  // --- argument-table flush (decision 3) -------------------------------------

  void MtlContextImpl::flushVertexBuffers() {
    for (uint32_t i = 0u; i < m_vertexStreams.size(); i++) {
      VertexStream& s = m_vertexStreams[i];
      if (!s.slice.defined())
        continue;
      auto* buf = static_cast<MtlBufferImpl*>(s.slice.buffer().ptr());
      if (!buf)
        continue;
      makeResident(buf->handle.buf);
      auto* cmd = m_chunk.append<wmtcmd_render_setbuffer>(WMTRenderCommandSetVertexBuffer);
      cmd->buffer = buf->handle.buf;
      cmd->offset = s.slice.offset();
      cmd->index  = uint8_t(kVertexBufferBaseIndex + i);
    }
  }

  void MtlContextImpl::flushResources() {
    for (uint32_t st = 0u; st < 2u; st++) {
      StageBindings& b = m_bindings[st];
      const bool vertex = (st == 0u);

      /* constant buffers at their AIR slots (0..2), with the discard/range
       * offset folded in (bindUniformBufferRange snapshot). */
      for (uint32_t slot = 0u; slot < b.cbvs.size(); slot++) {
        StageBindings::CbvBinding& cbv = b.cbvs[slot];
        if (!cbv.slice.defined())
          continue;
        auto* buf = static_cast<MtlBufferImpl*>(cbv.slice.buffer().ptr());
        if (!buf)
          continue;
        makeResident(buf->handle.buf);
        auto* cmd = m_chunk.append<wmtcmd_render_setbuffer>(
          vertex ? WMTRenderCommandSetVertexBuffer : WMTRenderCommandSetFragmentBuffer);
        cmd->buffer = buf->handle.buf;
        cmd->offset = cbv.slice.offset() + cbv.rangeOffset;
        cmd->index  = uint8_t(slot);
      }

      /* textures: fragment via upstream SetFragmentTexture; vertex via the
       * bridge addition SetVertexTexture (code 45) — D3D9 VTF supported. */
      for (uint32_t slot = 0u; slot < b.textures.size(); slot++) {
        const Rc<MtlTextureView>& view = b.textures[slot];
        if (view == nullptr)
          continue;
        auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
        makeResident(tv->owned.handle());
        auto* cmd = m_chunk.append<wmtcmd_render_settexture>(
          vertex ? WMTRenderCommandSetVertexTexture : WMTRenderCommandSetFragmentTexture);
        cmd->texture = tv->owned.handle();
        cmd->index   = uint8_t(slot);
      }

      /* samplers: per-slot via the bridge additions (codes 44/46). */
      for (uint32_t slot = 0u; slot < b.samplers.size(); slot++) {
        const Rc<MtlSamplerState>& sampler = b.samplers[slot];
        if (sampler == nullptr)
          continue;
        auto* sp = static_cast<MtlSamplerStateImpl*>(sampler.ptr());
        auto* cmd = m_chunk.append<wmtcmd_render_setsamplerstate>(
          vertex ? WMTRenderCommandSetVertexSamplerState
                 : WMTRenderCommandSetFragmentSamplerState);
        cmd->sampler = sp->owned.handle();
        cmd->index   = uint8_t(slot);
      }

      /* push data -> fresh UploadRing slice bound at the provisional push slot
       * (snapshot semantics: encoded commands never reference rewritten mem). */
      if (b.pushDirtyHi > 0u) {
        uint64_t sz = b.pushDirtyHi;
        RingSlice rs = m_device->uploadRing().allocate(sz, 256u);
        if (rs.cpuPtr) {
          std::memcpy(rs.cpuPtr, b.pushData.data(), sz);
          makeResident(rs.buffer);
          auto* cmd = m_chunk.append<wmtcmd_render_setbuffer>(
            vertex ? WMTRenderCommandSetVertexBuffer : WMTRenderCommandSetFragmentBuffer);
          cmd->buffer = rs.buffer;
          cmd->offset = rs.offset;
          cmd->index  = uint8_t(kPushConstantIndex);
        }
        b.pushDirtyLo = b.pushDirtyHi = 0u;
      }
    }
  }

  void MtlContextImpl::flushDynamicState() {
    /* viewport (RT-pixel coords; backing scale handled in the presenter) */
    {
      auto* cmd = m_chunk.append<wmtcmd_render_setviewport>(WMTRenderCommandSetViewport);
      cmd->viewport.originX = m_viewport.originX;
      cmd->viewport.originY = m_viewport.originY;
      cmd->viewport.width   = m_viewport.width;
      cmd->viewport.height  = m_viewport.height;
      cmd->viewport.znear   = m_viewport.znear;
      cmd->viewport.zfar    = m_viewport.zfar;
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
      auto* cmd = m_chunk.append<wmtcmd_render_setscissorrect>(WMTRenderCommandSetScissorRect);
      cmd->scissor_rect.x      = sc.x;
      cmd->scissor_rect.y      = sc.y;
      cmd->scissor_rect.width  = sc.width;
      cmd->scissor_rect.height = sc.height;
    }

    /* depth-stencil state object (cached in the device) + stencil ref */
    {
      obj_handle_t dsso = m_device->getDepthStencilState(m_depthStencil);
      auto* cmd = m_chunk.append<wmtcmd_render_setdsso>(WMTRenderCommandSetDSSO);
      cmd->dsso        = dsso;
      cmd->stencil_ref = uint8_t(m_stencilRef);
    }

    /* blend constant + stencil ref (one combined command) */
    {
      auto* cmd = m_chunk.append<wmtcmd_render_setblendcolor>(WMTRenderCommandSetBlendFactorAndStencilRef);
      cmd->red         = m_blendColor.r;
      cmd->green       = m_blendColor.g;
      cmd->blue        = m_blendColor.b;
      cmd->alpha       = m_blendColor.a;
      cmd->stencil_ref = uint8_t(m_stencilRef);
    }

    /* rasterizer: cull / winding / fill / depth-bias / depth-clip */
    {
      auto* cmd = m_chunk.append<wmtcmd_render_setrasterizerstate>(WMTRenderCommandSetRasterizerState);
      cmd->fill_mode        = WMTTriangleFillMode(m_rasterizer.fillMode);
      cmd->cull_mode        = WMTCullMode(m_rasterizer.cullMode);
      cmd->depth_clip_mode  = m_rasterizer.depthClip ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
      cmd->winding          = WMTWinding(m_rasterizer.frontFace);
      cmd->depth_bias       = m_depthBias.constantFactor;
      cmd->scole_scale      = m_depthBias.slopeFactor;   /* [sic] ABI field name */
      cmd->depth_bias_clamp = m_depthBias.clamp;
    }
  }

  void MtlContextImpl::makeResident(obj_handle_t handle) {
    if (!handle)
      return;
    for (obj_handle_t h : m_residentSet)
      if (h == handle)
        return;
    m_residentSet.push_back(handle);

    auto* cmd = m_chunk.append<wmtcmd_render_useresource>(WMTRenderCommandUseResource);
    cmd->resource = handle;
    cmd->usage    = WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageSample);
    cmd->stages   = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
  }

  // --- transfers / blits (decision 8) ----------------------------------------

  void MtlContextImpl::encodeBlit(const wmtcmd_base* head) {
    if (m_pass.active)
      closePass();
    ensureCommandBuffer();
    obj_handle_t enc = MTLCommandBuffer_blitCommandEncoder(m_cmdbuf.handle());
    MTLBlitCommandEncoder_encodeCommands(enc, head);
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
  }

  void MtlContextImpl::copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                  const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    CmdChunk c;
    auto* cmd = c.append<wmtcmd_blit_copy_from_buffer_to_buffer>(WMTBlitCommandCopyFromBufferToBuffer);
    cmd->src        = s->handle.buf;
    cmd->src_offset = srcOff;
    cmd->dst        = d->handle.buf;
    cmd->dst_offset = dstOff;
    cmd->copy_length = bytes;
    encodeBlit(c.head());
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
    CmdChunk c;
    auto* cmd = c.append<wmtcmd_blit_copy_from_texture_to_texture>(WMTBlitCommandCopyFromTextureToTexture);
    cmd->src        = s->texture.tex;
    cmd->src_slice  = srcSub.baseLayer;
    cmd->src_level  = srcSub.mipLevel;
    cmd->src_origin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    cmd->src_size   = { extent.width, extent.height, extent.depth };
    cmd->dst        = d->texture.tex;
    cmd->dst_slice  = dstSub.baseLayer;
    cmd->dst_level  = dstSub.mipLevel;
    cmd->dst_origin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    encodeBlit(c.head());
  }

  void MtlContextImpl::copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                                           const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    CmdChunk c;
    auto* cmd = c.append<wmtcmd_blit_copy_from_buffer_to_texture>(WMTBlitCommandCopyFromBufferToTexture);
    cmd->src            = s->handle.buf;
    cmd->src_offset     = off;
    cmd->bytes_per_row  = uint32_t(rowAlign);
    cmd->bytes_per_image = uint32_t(sliceAlign);
    cmd->size           = { extent.width, extent.height, extent.depth };
    cmd->dst            = d->texture.tex;
    cmd->slice          = dstSub.baseLayer;
    cmd->level          = dstSub.mipLevel;
    cmd->origin         = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    encodeBlit(c.head());
  }

  void MtlContextImpl::copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                                           MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    CmdChunk c;
    auto* cmd = c.append<wmtcmd_blit_copy_from_texture_to_buffer>(WMTBlitCommandCopyFromTextureToBuffer);
    cmd->src            = s->texture.tex;
    cmd->slice          = srcSub.baseLayer;
    cmd->level          = srcSub.mipLevel;
    cmd->origin         = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    cmd->size           = { extent.width, extent.height, extent.depth };
    cmd->dst            = d->handle.buf;
    cmd->offset         = off;
    cmd->bytes_per_row  = uint32_t(rowAlign);
    cmd->bytes_per_image = uint32_t(sliceAlign);
    encodeBlit(c.head());
  }

  void MtlContextImpl::blitTextureView(const Rc<MtlTextureView>&, const MtlOffset3D[2],
                                       const Rc<MtlTextureView>&, const MtlOffset3D[2], MtlSamplerFilter) {
    logOnce("MtlContextImpl::blitTextureView — meta-blit pipeline unimplemented (LOW priority)");
  }

  void MtlContextImpl::generateMipmaps(const Rc<MtlTextureView>&, MtlSamplerFilter) {
    logOnce("MtlContextImpl::generateMipmaps — unimplemented (LOW priority)");
  }

  // --- init / upload / discard (contracts 3a,3c,3g) --------------------------

  void MtlContextImpl::initBuffer(const Rc<MtlBuffer>& buffer) {
    auto* b = static_cast<MtlBufferImpl*>(buffer.ptr());
    if (!b)
      return;
    CmdChunk c;
    auto* cmd = c.append<wmtcmd_blit_fillbuffer>(WMTBlitCommandFillBuffer);
    cmd->buffer = b->handle.buf;
    cmd->offset = 0u;
    cmd->length = b->size;
    cmd->value  = 0u;
    encodeBlit(c.head());
  }

  void MtlContextImpl::initImage(const Rc<MtlTexture>& image) {
    auto* t = static_cast<MtlTextureImpl*>(image.ptr());
    if (!t)
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

    /* load=Clear + store=Store clears the whole subresource with no draws. */
    obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(m_cmdbuf.handle(), &info);
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

    auto* cmd = m_chunk.append<wmtcmd_render_setvisibilitymode>(WMTRenderCommandSetVisibilityMode);
    cmd->mode   = WMTVisibilityResultModeCounting;
    cmd->offset = q->slotOffset;
  }

  void MtlContextImpl::endQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();

    auto* cmd = m_chunk.append<wmtcmd_render_setvisibilitymode>(WMTRenderCommandSetVisibilityMode);
    cmd->mode   = WMTVisibilityResultModeDisabled;
    cmd->offset = 0u;
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
