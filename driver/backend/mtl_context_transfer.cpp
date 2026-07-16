#include "mtl_context_impl.h"
#include "../util/util_bit.h"
#include "../util/util_string.h"

#include <algorithm>
#include <cstring>

#include "../util/log/log.h"

#include "mtl_profiler.h"

// Build-time-generated: g_blitMetallib[] (backend/shaders/mtl_blit.metal
// compiled to a .metallib by scripts/build-driver.sh) — the meta-blit
// fullscreen-triangle pipeline behind blitTextureView.
#include "mtl_blit_metallib.h"

/*
 * mtl_context_transfer.cpp — part 2 of the METAL 4 MtlContextImpl (transfers/blits/resolve/queries/flush) (see
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
    prof::Scope profScope(prof::ResolvePso);
    if (m_vs == nullptr)
      return false;

    /* MRU short-circuit: snapshot the key INPUTS (~64B) and memcmp against
     * the last 4 resolved snapshots — the A<->B material alternation that
     * dominates DirtyPso draws hits here and skips the 600-byte key build,
     * FNV hash, and map probe entirely. Generation counters stand in for
     * attr/layout/stride/blend contents. D9MT_NO_PSOMRU=1 disables. */
    static const bool mruOff = std::getenv("D9MT_NO_PSOMRU") != nullptr;
    PsoSnapshot snap = {};
    snap.vs = m_vs.ptr();
    snap.fs = m_fs.ptr();
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      const Rc<MtlTextureView>& v = m_pass.targets.color[i];
      snap.colorFmt[i] = v != nullptr ? uint32_t(v->key.format) : 0u;
    }
    snap.layoutGen  = m_layoutGen;
    snap.strideGen  = m_strideGen;
    snap.blendGen   = m_blendGen;
    snap.msaaPacked = (uint32_t(m_msaa.sampleCount) << 1)
                    | (m_msaa.alphaToCoverage ? 1u : 0u);
    snap.topoClass  = uint32_t(topologyClass(m_topology));

    if (!mruOff) {
      for (uint32_t i = 0u; i < 4u; i++) {
        if (m_psoMruPso[i] != 0u
         && std::memcmp(&m_psoMruKey[i], &snap, sizeof(snap)) == 0) {
          obj_handle_t pso = m_psoMruPso[i];
          if (pso == m_lastEncodedPso)
            return true;
          m_lastEncodedPso = pso;
          auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setpso>(
            uint16_t(WMTMtl4RenderCommandSetPSO));
          cmd->pso = pso;
          return true;
        }
      }
    }
    prof::bump(prof::CntPsoKeyBuild);

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

    /* Feed the MRU on the slow path (round-robin eviction). */
    m_psoMruKey[m_psoMruCursor] = snap;
    m_psoMruPso[m_psoMruCursor] = pso;
    m_psoMruCursor = (m_psoMruCursor + 1u) & 3u;

    /* DirtyPso often re-resolves to the PSO already encoded (state rewritten
     * with the same values, or A->B->A shader flips within a pass) — skip
     * the redundant setPSO append. m_lastEncodedPso resets per encoder. */
    if (pso == m_lastEncodedPso)
      return true;
    m_lastEncodedPso = pso;

    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setpso>(
      uint16_t(WMTMtl4RenderCommandSetPSO));
    cmd->pso = pso;
    return true;
  }

  // --- argument-table flush (decision 3) -------------------------------------
  // Every write below is APPENDED to m_cmdChunk, not thunked: the table
  // handle rides in the command, so the replayed stream writes table N's
  // slots strictly before the draw that binds table N — one PE->unix
  // crossing per batch instead of ~90 per draw. Each appender diffs the
  // desired value against the table's shadow (ArgTableShadow, header) and
  // appends only on change: a skipped write leaves the persistent table
  // holding the exact value we would have written, so the draw reads the
  // correct resource either way. GTA-shaped streams (one texture rebound,
  // everything else stable) collapse from ~90 structs/draw to a handful.

  void MtlContextImpl::appendTableAddress(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuAddress, uint32_t index) {
    if (shadow.bufferGpuAddress[index] == gpuAddress)
      return;                     /* table already holds this address (incl. null==null) */
    shadow.bufferGpuAddress[index] = gpuAddress;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setaddress>(
      uint16_t(WMTMtl4RenderCommandTableSetAddress));
    cmd->table       = table;
    cmd->gpu_address = gpuAddress;
    cmd->index       = index;
  }

  void MtlContextImpl::appendTableTexture(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index) {
    if (shadow.textureId[index] == gpuResourceId)
      return;
    shadow.textureId[index] = gpuResourceId;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setresource>(
      uint16_t(WMTMtl4RenderCommandTableSetTexture));
    cmd->table           = table;
    cmd->gpu_resource_id = gpuResourceId;
    cmd->index           = index;
  }

  void MtlContextImpl::appendTableSampler(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index) {
    if (shadow.samplerId[index] == gpuResourceId)
      return;
    shadow.samplerId[index] = gpuResourceId;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setresource>(
      uint16_t(WMTMtl4RenderCommandTableSetSamplerState));
    cmd->table           = table;
    cmd->gpu_resource_id = gpuResourceId;
    cmd->index           = index;
  }

  void MtlContextImpl::flushVertexBuffers() {
    prof::Scope profScope(prof::FlushVertexBuffers);
    ArgTablePair&   pair   = m_argTables[m_currentArgTable];
    obj_handle_t    table  = pair.table[0].handle();
    ArgTableShadow& shadow = pair.shadow[0];
    /* Visit only streams touched since the last flush (m_dirtyVertexStreams,
     * consumed here). Untouched streams already hold their correct address in
     * the bound table: either written earlier this pass (shadow carries
     * forward on the persistent pair) or covered by the rotation full-mask
     * (prepareDraw forces ~0 whenever a different physical pair rotates in). */
    const uint32_t streamMask = m_dirtyVertexStreams
      & ((1u << m_vertexStreams.size()) - 1u);
    m_dirtyVertexStreams = 0u;
    for (uint32_t i : bit::BitMask(streamMask)) {
      VertexStream& s = m_vertexStreams[i];
      /* Record-time snapshot (MtlBufferSlice contract): the slice's captured
       * physical backing, NEVER the live buffer — an app-thread rename must
       * not retarget a draw recorded against the old backing. Renames reach
       * still-bound slices only via invalidateBuffer's refresh, in command
       * order. The gpuAddress is snapshotted into the command at append
       * time; the replay resolves nothing. */
      if (!s.slice.defined() || s.slice.storage() == nullptr) {
        appendTableAddress(shadow, table, 0u, kVertexBufferBaseIndex + i);
        continue;
      }
      /* DIAG: with in-order flow the bound snapshot must equal the buffer's
       * live storage at draw time (renames refresh in command order). A
       * divergence here is the recorded-vs-live retargeting bug class. */
      if (unlikely(s.slice.storage() != s.slice.buffer()->storage()))
        s_diagVbStale++;
      /* GPU-use stamp for the (re)bound stream — this walk visits exactly
       * the streams that changed, so stamping here replaces the former
       * per-draw 16-slot walk in prepareDraw; unchanged-but-bound streams
       * are covered by prepareDraw's serial-change full re-stamp. */
      s.slice.buffer()->setTrackingSerial(m_currentSerial);
      appendTableAddress(shadow, table, s.slice.gpuAddress(), kVertexBufferBaseIndex + i);
    }
  }

  void MtlContextImpl::flushResources() {
    prof::Scope profScope(prof::FlushResources);
    ArgTablePair& pair = m_argTables[m_currentArgTable];
    /* Per-slot dirty-mask walk: visit ONLY slots touched since the last
     * flush (StageBindings::dirty*, consumed below). An untouched slot needs
     * no visit because the bound pair persists across the pass's draws — the
     * slot already holds its correct value, written either earlier this pass
     * or during the rotation full-mask reconcile (prepareDraw forces every
     * mask to ~0 whenever a different physical pair rotates in, since that
     * pair's shadow reflects ITS last contents, not the current bindings).
     *
     * CBVs additionally go wide on a PSO change: the d3d9-slot -> AIR-slot
     * map (fn->cbvSlotMap) is shader-derived, so a new shader can move an
     * UNTOUCHED CBV to a different table index (or newly declare one) — its
     * dirty bit alone would miss that. 8 slots x 2 stages, cheap. */
    const bool psoChanged = (m_dirty & DirtyPso) != 0u;
    for (uint32_t st = 0u; st < 2u; st++) {
      StageBindings&  b      = m_bindings[st];
      obj_handle_t    table  = pair.table[st].handle();
      ArgTableShadow& shadow = pair.shadow[st];

      /* The bound shader for this stage carries the CbvIndex -> AIR Metal
       * buffer-slot map (reflection-derived, SOURCE_OF_TRUTH §4b bug 1). The
       * frontend binds CBVs at raw CbvIndex; we translate to the slot the AIR
       * shader actually declares so CBVs land at 0..2 (or wherever air_builder
       * put them) and never collide with vertex streams (kVertexBufferBaseIndex)
       * or push data (kPushConstantIndex). */
      const Rc<MtlShaderFunction>& fn = (st == 0u) ? m_vs : m_fs;

      /* Binding-model switch (FF per-slot <-> translated AB): both paths
       * consume the same dirty masks, so the incoming model must reconcile
       * everything the outgoing one may have swallowed. */
      const bool abShader = fn != nullptr && !fn->abWords.empty();
      if (abShader != (b.abActive != 0u)) {
        b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = ~0u;
        b.abActive = abShader ? 1u : 0u;
      }

      /* --- argument-buffer path (fn->abWords) ------------------------------
       * The shader reads every texture/sampler/CBV as an 8-byte handle word
       * from ONE buffer at kAbIndex. The words live in a persistent CPU
       * shadow (abShadowWords): a rebuild patches only the words whose
       * (kind,reg) dirty bit is set, then memcpys the shadow into a fresh
       * ring slice (snapshot semantics — ring bytes are never rewritten once
       * bound, and recycled ring memory can never be read back). A shader
       * change (abFnId) is a word-LAYOUT change and recomputes everything;
       * a mere PSO change (blend/vertex-layout) does not touch the words. */
      if (abShader) {
        const uint64_t fnId = uint64_t(fn->library);
        const bool layoutChanged = b.abFnId != fnId;
        const uint32_t dirtyCbvs = layoutChanged ? ~0u : b.dirtyCbvs;
        const uint32_t dirtyTexs = layoutChanged ? ~0u : b.dirtyTextures;
        const uint32_t dirtySmps = layoutChanged ? ~0u : b.dirtySamplers;
        const bool rebuild = layoutChanged || b.abGpuAddress == 0u
          || (dirtyCbvs | dirtyTexs | dirtySmps) != 0u;
        b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = 0u;

        if (rebuild) {
          prof::bump(prof::CntAbRebuild);
          const uint32_t words = uint32_t(fn->abWords.size());
          if (b.abShadowWords.size() != words) {
            b.abShadowWords.assign(words, 0u);
            b.abFnId = 0u;   /* size change: recompute every word below */
          }
          const bool full = layoutChanged || b.abFnId != fnId;
          b.abFnId = fnId;
          uint64_t* sw = b.abShadowWords.data();
          for (uint32_t i = 0u; i < words; i++) {
            const MtlShaderFunction::AbWord& e = fn->abWords[i];
            uint64_t v;
            switch (e.kind) {
              case MtlShaderFunction::AbWordKind::Cbv: {
                if (!full && !(dirtyCbvs & (1u << e.reg)))
                  continue;                     /* shadow already exact */
                v = 0u;
                if (e.reg < b.cbvs.size()) {
                  StageBindings::CbvBinding& cbv = b.cbvs[e.reg];
                  /* Record-time snapshot (MtlBufferSlice contract) with the
                   * discard/range offset folded in. */
                  if (cbv.slice.defined() && cbv.slice.storage() != nullptr
                   && uintptr_t(cbv.slice.storage().ptr()) >= 0x10000u)
                    v = cbv.slice.gpuAddress(cbv.rangeOffset);
                }
              } break;
              case MtlShaderFunction::AbWordKind::Texture: {
                if (!full && !(dirtyTexs & (1u << e.reg)))
                  continue;
                v = 0u;
                if (e.reg < b.textures.size() && b.textures[e.reg] != nullptr)
                  v = static_cast<MtlTextureViewImpl*>(
                    b.textures[e.reg].ptr())->gpuResourceId;
              } break;
              case MtlShaderFunction::AbWordKind::Sampler: {
                if (!full && !(dirtySmps & (1u << e.reg)))
                  continue;
                v = 0u;
                if (e.reg < b.samplers.size() && b.samplers[e.reg] != nullptr)
                  v = static_cast<MtlSamplerStateImpl*>(
                    b.samplers[e.reg].ptr())->gpuResourceId;
              } break;
              default:
                v = 0u;
                break;
            }
            sw[i] = v;
          }
          RingSlice rs = m_device->uploadRing().allocate(
            uint64_t(words) * 8u, 16u);
          if (rs.cpuPtr) {
            std::memcpy(rs.cpuPtr, sw, uint64_t(words) * 8u);
            b.abGpuAddress = rs.gpuAddress;
          }
        }
        appendTableAddress(shadow, table, b.abGpuAddress, kAbIndex);

        /* push data still rides its own slot (below) */
        if (b.pushDirtyHi > 0u) {
          uint64_t sz = b.pushExtent;
          RingSlice prs = m_device->uploadRing().allocate(sz, 256u);
          if (prs.cpuPtr) {
            std::memcpy(prs.cpuPtr, b.pushData.data(), sz);
            b.pushGpuAddress = prs.gpuAddress;
          }
          b.pushDirtyLo = b.pushDirtyHi = 0u;
        }
        appendTableAddress(shadow, table, b.pushGpuAddress, kPushConstantIndex);
        continue;
      }

      const uint32_t cbvMask = (psoChanged ? ~0u : b.dirtyCbvs)
        & ((1u << b.cbvs.size()) - 1u);
      const uint32_t texMask = b.dirtyTextures
        & ((1u << b.textures.size()) - 1u);
      const uint32_t smpMask = b.dirtySamplers
        & ((1u << b.samplers.size()) - 1u);
      b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = 0u;

      /* constant buffers at their AIR slots, with the discard/range offset
       * folded in (bindUniformBufferRange snapshot). */
      for (uint32_t slot : bit::BitMask(cbvMask)) {
        StageBindings::CbvBinding& cbv = b.cbvs[slot];
        /* Only bind CBVs the shader actually declares, at their AIR slot. */
        int32_t airSlot = (fn != nullptr && slot < fn->cbvSlotMap.size())
          ? int32_t(fn->cbvSlotMap[slot]) : -1;
        if (airSlot < 0)
          continue;

        /* Record-time snapshot (MtlBufferSlice contract): resolve through
         * the slice's captured backing. CBV renames (constant-buffer
         * Discard) retarget this binding via invalidateBuffer's refresh. */
        if (!cbv.slice.defined() || cbv.slice.storage() == nullptr
         || uintptr_t(cbv.slice.storage().ptr()) < 0x10000u) {
          appendTableAddress(shadow, table, 0u, uint32_t(airSlot));
          continue;
        }
        if (unlikely(cbv.slice.storage() != cbv.slice.buffer()->storage()))
          s_diagCbvStale++;
        appendTableAddress(shadow, table, cbv.slice.gpuAddress(cbv.rangeOffset),
          uint32_t(airSlot));
      }

      /* textures: setTexture by MTLResourceID into this stage's table. An
       * unbound slot must be CLEARED, not skipped — the argument table persists
       * across draws, so a stale binding (often a recycled dynamic/UP buffer)
       * would leak into a later draw that samples that slot -> flicker/ghosting
       * in multi-draw scenes (single-draw static tests never expose it). The
       * clear is a 0-write diffed like any other: the shadow mirrors the
       * table's actual contents, so a slot that is already null (the common
       * case — most of the 16 slots are never bound) appends nothing, while a
       * newly-unbound slot still gets its clear. */
      for (uint32_t slot : bit::BitMask(texMask)) {
        const Rc<MtlTextureView>& view = b.textures[slot];
        if (view == nullptr) {
          appendTableTexture(shadow, table, 0u, slot);
          continue;
        }
        auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
        appendTableTexture(shadow, table, tv->gpuResourceId, slot);
      }

      /* samplers: setSamplerState by MTLResourceID. */
      for (uint32_t slot : bit::BitMask(smpMask)) {
        const Rc<MtlSamplerState>& sampler = b.samplers[slot];
        if (sampler == nullptr) {
          appendTableSampler(shadow, table, 0u, slot);
          continue;
        }
        auto* sp = static_cast<MtlSamplerStateImpl*>(sampler.ptr());
        appendTableSampler(shadow, table, sp->gpuResourceId, slot);
      }

      /* push data -> fresh UploadRing slice at the push slot (snapshot
       * semantics: the table reads the gpuAddress at the draw snapshot, and
       * this slice's memory is never rewritten under a queued draw).
       * Size by pushExtent, NOT pushDirtyHi: the slice replaces the whole
       * bound block, so it must cover every byte any shader may read, not
       * just the range this update touched (a shared-block-only update
       * would otherwise truncate the per-stage block at byte 32+). */
      if (b.pushDirtyHi > 0u) {
        uint64_t sz = b.pushExtent;
        RingSlice rs = m_device->uploadRing().allocate(sz, 256u);
        if (rs.cpuPtr) {
          std::memcpy(rs.cpuPtr, b.pushData.data(), sz);
          b.pushGpuAddress = rs.gpuAddress;
        }
        b.pushDirtyLo = b.pushDirtyHi = 0u;
      }
      appendTableAddress(shadow, table, b.pushGpuAddress, kPushConstantIndex);
    }
  }

  void MtlContextImpl::flushDynamicState() {
    prof::Scope profScope(prof::FlushDynamicState);
    /* Encoder dynamic state rides the same wmtcmd batch as the table writes
     * and draws — values are snapshotted into the commands here. Each piece
     * is diffed against the encoder-scoped shadow (reset per encoder in
     * materializePass): fresh encoders append everything, later flushes only
     * what changed. */
    const bool first = !m_dynValid;
    m_dynValid = true;

    /* viewport (RT-pixel coords; backing scale handled in the presenter) */
    if (first
     || std::memcmp(&m_lastViewport, &m_viewport, sizeof(MtlViewport)) != 0) {
      m_lastViewport = m_viewport;
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setviewport>(
        uint16_t(WMTMtl4RenderCommandSetViewport));
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
      if (first
       || std::memcmp(&m_lastScissor, &sc, sizeof(MtlRect)) != 0) {
        m_lastScissor = sc;
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setscissorrect>(
          uint16_t(WMTMtl4RenderCommandSetScissorRect));
        cmd->rect.x      = sc.x;
        cmd->rect.y      = sc.y;
        cmd->rect.width  = sc.width;
        cmd->rect.height = sc.height;
      }
    }

    /* depth-stencil state object (cached in the device) + stencil ref */
    {
      obj_handle_t dsso = m_device->getDepthStencilState(m_depthStencil);
      if (first || dsso != m_lastDsso) {
        m_lastDsso = dsso;
        auto* dss = m_cmdChunk.append<wmtcmd_mtl4_setdsso>(
          uint16_t(WMTMtl4RenderCommandSetDepthStencilState));
        dss->dsso = dsso;
      }
      if (first || m_stencilRef != m_lastStencilRef) {
        m_lastStencilRef = m_stencilRef;
        auto* sref = m_cmdChunk.append<wmtcmd_mtl4_setstencilref>(
          uint16_t(WMTMtl4RenderCommandSetStencilReferenceValue));
        sref->reference = m_stencilRef;
      }
    }

    /* blend constant */
    if (first
     || std::memcmp(&m_lastBlendColor, &m_blendColor, sizeof(MtlColor4)) != 0) {
      m_lastBlendColor = m_blendColor;
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setblendcolor>(
        uint16_t(WMTMtl4RenderCommandSetBlendColor));
      cmd->red   = m_blendColor.r;
      cmd->green = m_blendColor.g;
      cmd->blue  = m_blendColor.b;
      cmd->alpha = m_blendColor.a;
    }

    /* rasterizer: cull / winding / depth-bias (fill mode + depth-clip use the
     * Metal defaults — no MTL4 bridge setter, and D3D9 default is Fill/Clip). */
    {
      auto* cull = m_cmdChunk.append<wmtcmd_mtl4_setcullmode>(
        uint16_t(WMTMtl4RenderCommandSetCullMode));
      cull->mode = uint64_t(WMTCullMode(m_rasterizer.cullMode));
      auto* wind = m_cmdChunk.append<wmtcmd_mtl4_setwinding>(
        uint16_t(WMTMtl4RenderCommandSetFrontFacingWinding));
      wind->winding = uint64_t(WMTWinding(m_rasterizer.frontFace));
      auto* bias = m_cmdChunk.append<wmtcmd_mtl4_setdepthbias>(
        uint16_t(WMTMtl4RenderCommandSetDepthBias));
      bias->bias        = m_depthBias.constantFactor;
      bias->slope_scale = m_depthBias.slopeFactor;
      bias->clamp       = m_depthBias.clamp;
    }
  }

  // --- transfers / blits (decision 8: MTL4 compute-encoder copies) ------------

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
     * an immediately following read in a fresh encoder is coherent. The
     * batched wmtcmd stream must replay first: a DIRECT encoder call may not
     * overtake draws still parked in the chunk. */
    if (m_pass.active) {
      if (deferredPassesEnabled()) {
        /* No live encoder — the barrier rides the wmtcmd stream so it stays
         * ordered against the parked draws it separates (replayed by
         * flushFramePasses in exact record order). */
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_barrier>(
          uint16_t(WMTMtl4RenderCommandBarrier));
        cmd->after_stages  = uint64_t(WMTStageFragment);
        cmd->before_stages = uint64_t(WMTStageAll);
        cmd->visibility    = uint64_t(WMTVisibilityOptionDevice);
        return;
      }
      flushCmdChunk();
      emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
    }
  }

  void MtlContextImpl::copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                  const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    const obj_handle_t sbuf = s->handle.buf;
    const obj_handle_t dbuf = d->handle.buf;
    encodeDirect([this, keepD = dst, keepS = src, sbuf, srcOff, dbuf, dstOff, bytes]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromBufferToBuffer(
        enc, sbuf, srcOff, dbuf, dstOff, bytes);
      /* §4c: make this copy's write visible to the later draw/copy that reads it
       * (e.g. a DEFAULT-pool vertex buffer uploaded here then fetched by the VS). */
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
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
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    /* This copy READS src: its earlier passes must keep their Store (also
     * feeds the persistent transfer-read set — the present copy's source is
     * the swapchain backbuffer, the backbuffer exemption anchor). */
    noteTransferRead(s);
    /* Drawable-backed textures (borrowed handle, owned unset) are covered
     * by the CAMetalLayer's own residency set, queue-attached in the
     * presenter; owned textures are resident from creation. */
    WMTOrigin srcOrigin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   srcSize   = { extent.width, extent.height, extent.depth };
    WMTOrigin dstOrigin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([this, keepD = dst, keepS = src, stex, dtex,
                  srcSub, dstSub, srcOrigin, srcSize, dstOrigin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromTextureToTexture(
        enc, stex, srcSub.baseLayer, srcSub.mipLevel, srcOrigin, srcSize,
        dtex, dstSub.baseLayer, dstSub.mipLevel, dstOrigin);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  bool MtlContextImpl::presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) {
    static const bool enabled = std::getenv("D9MT_METALFX") != nullptr;
    if (!enabled || m_fxUnsupported)
      return false;
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return false;
    const MtlSize se = src->extent, de = dst->extent;
    /* Spatial scaler is upscale-only and needs concrete dims on both ends. */
    if (!se.width || !se.height || !de.width || !de.height
     || se.width > de.width || se.height > de.height)
      return false;

    if (!m_fxChecked) {
      m_fxChecked = true;
      if (!MTLDevice_supportsFXSpatialScaler(m_device->wmtDevice())) {
        m_fxUnsupported = true;
        Logger::warn("MtlContextImpl: MetalFX spatial scaler unsupported on this device");
        return false;
      }
    }

    const std::array<uint32_t, 6> key = {
      se.width, se.height, de.width, de.height,
      uint32_t(src->format), uint32_t(dst->format) };
    obj_handle_t scaler = 0u;
    auto it = m_fxScalers.find(key);
    if (it != m_fxScalers.end()) {
      scaler = it->second;
    } else {
      WMTFXSpatialScalerInfo info = {};
      info.color_format  = WMTPixelFormat(uint32_t(src->format));
      info.output_format = WMTPixelFormat(uint32_t(dst->format));
      info.input_width   = se.width;
      info.input_height  = se.height;
      info.output_width  = de.width;
      info.output_height = de.height;
      scaler = MTLDevice_newMTL4SpatialScaler(
        m_device->wmtDevice(), m_device->wmtCompiler(), &info);
      if (!scaler) {
        m_fxUnsupported = true;   /* don't retry (and re-log) per present */
        Logger::warn("MtlContextImpl: MTL4 spatial scaler creation failed — falling back to blit");
        return false;
      }
      m_fxScalers.emplace(key, scaler);
    }

    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    /* The scaler READS the backbuffer: earlier passes must keep their Store
     * (backbuffer exemption anchor, same as the present copy). */
    noteTransferRead(s);
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([keepD = dst, keepS = src, scaler, stex, dtex]
                 (obj_handle_t cmdbuf) {
      MTL4CommandBuffer_encodeSpatialScale(cmdbuf, scaler, stex, dtex);
    });
    return true;
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
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    WMTOrigin origin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : d->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    const obj_handle_t sbuf = s->handle.buf;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([this, keepD = dst, keepS = src, sbuf, dtex, off,
                  bytesPerRow, bytesPerImage, size, dstSub, origin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromBufferToTexture(
        enc, sbuf, off, bytesPerRow, bytesPerImage, size,
        dtex, dstSub.baseLayer, dstSub.mipLevel, origin);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                                           MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    /* Readback READS src — earlier passes on it must keep their Store. The
     * caller's CPU wait rides the submission serial, and submit() replays
     * the frame list before commit, so the copy always lands before the
     * serial can complete. */
    noteTransferRead(s);
    WMTOrigin origin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : s->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dbuf = d->handle.buf;
    encodeDirect([this, keepD = dst, keepS = src, stex, dbuf, off,
                  bytesPerRow, bytesPerImage, size, srcSub, origin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromTextureToBuffer(
        enc, stex, srcSub.baseLayer, srcSub.mipLevel, origin, size,
        dbuf, off, bytesPerRow, bytesPerImage);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src,
                                      MtlResolveRegion region, MtlFormat format,
                                      MtlResolveMode color, MtlResolveMode stencil) {
    /* §6.5 MSAA resolve: a draw-less load=Load / store=MultisampleResolve pass
     * on the MSAA source with the destination as resolve attachment — the
     * TBDR-native resolve, no shader involved. Color aspect only (D3D9 depth
     * resolves never reach this seam; the frontend rejects stretched DS blits
     * and present sources are color). Metal's color resolve is always the
     * sample average, which matches the one mode the frontend requests. */
    (void)color; (void)stencil;
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;

    const MtlFormatInfo* fi = lookupFormatInfo(
      format != MtlFormat::Invalid ? format : s->format);
    if (unlikely(fi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil))) {
      Logger::err("MtlContextImpl::resolveTexture: depth-stencil resolve unsupported");
      return;
    }

    /* Store-action resolve covers the whole subresource; a partial region
     * would need a scratch resolve + region copy. Every current caller
     * (present, StretchRect fast path) resolves the full level. */
    const MtlExtent3D srcExtent = src->mipLevelExtent(region.srcSubresource.mipLevel);
    if (unlikely(region.srcOffset.x || region.srcOffset.y
              || region.dstOffset.x || region.dstOffset.y
              || region.extent.width  < srcExtent.width
              || region.extent.height < srcExtent.height)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn("MtlContextImpl::resolveTexture: partial region resolved as full subresource");
      }
    }

    prepareDirectEncode(uint32_t(prof::CntPassCloseBlit));

    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    /* The resolve READS the MSAA source at pass end. */
    noteTransferRead(s);

    WMTRenderPassInfo info = {};
    info.colors[0].texture         = s->texture.tex;
    info.colors[0].load_action     = WMTLoadActionLoad;
    info.colors[0].store_action    = WMTStoreActionMultisampleResolve;
    info.colors[0].level           = region.srcSubresource.mipLevel;
    info.colors[0].slice           = region.srcSubresource.baseLayer;
    info.colors[0].resolve_texture = d->texture.tex;
    info.colors[0].resolve_level   = region.dstSubresource.mipLevel;
    info.colors[0].resolve_slice   = region.dstSubresource.baseLayer;
    info.render_target_width       = srcExtent.width;
    info.render_target_height      = srcExtent.height;
    encodeDirect([this, keepD = dst, keepS = src, info](obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(cmdbuf, &info);
      /* §4c: the resolve write happens at pass end; order it ahead of the
       * consumer that reads the resolved texture (present copy / sample). */
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  bool MtlContextImpl::ensureBlitPipeline() {
    if (m_blitVs != nullptr && m_blitFs != nullptr)
      return true;
    if (m_blitInitFailed)
      return false;

    /* Both entry points live in the one embedded metallib; each stage wraps
     * its own library handle so the render-PSO cache's vs/fs cookies stay
     * per-stage (matching every other MtlShaderFunction). */
    m_blitVs = m_device->createShaderFunction(
      MtlShaderStage::Vertex, g_blitMetallib, size_t(g_blitMetallibSize));
    if (m_blitVs != nullptr)
      m_blitVs->entryName = "blitVsMain";
    m_blitFs = m_device->createShaderFunction(
      MtlShaderStage::Fragment, g_blitMetallib, size_t(g_blitMetallibSize));
    if (m_blitFs != nullptr)
      m_blitFs->entryName = "blitFsMain";
    m_fillFs = m_device->createShaderFunction(
      MtlShaderStage::Fragment, g_blitMetallib, size_t(g_blitMetallibSize));
    if (m_fillFs != nullptr)
      m_fillFs->entryName = "fillFsMain";
    m_depthClearVs = m_device->createShaderFunction(
      MtlShaderStage::Vertex, g_blitMetallib, size_t(g_blitMetallibSize));
    if (m_depthClearVs != nullptr)
      m_depthClearVs->entryName = "depthClearVsMain";

    /* ClampToEdge: a stretched blit's edge texels must not bleed from the
     * opposite border; no mip filtering (blit views are single-mip). Both
     * ride the device sampler cache, alive for the device lifetime. */
    MtlSamplerKey key = {};
    key.addressU = key.addressV = key.addressW = MtlSamplerAddressMode::ClampToEdge;
    key.mipMode  = MtlSamplerMipMode::None;
    key.minFilter = key.magFilter = MtlSamplerFilter::Nearest;
    m_blitSamplers[0] = m_device->createSampler(key);
    key.minFilter = key.magFilter = MtlSamplerFilter::Linear;
    m_blitSamplers[1] = m_device->createSampler(key);

    if (m_blitVs == nullptr || m_blitFs == nullptr || m_fillFs == nullptr
     || m_depthClearVs == nullptr
     || m_blitSamplers[0] == nullptr || m_blitSamplers[1] == nullptr) {
      Logger::err("MtlContextImpl: meta-blit pipeline init failed");
      m_blitVs = nullptr;
      m_blitFs = nullptr;
      m_fillFs = nullptr;
      m_depthClearVs = nullptr;
      m_blitInitFailed = true;   /* don't retry (and re-log) per blit */
      return false;
    }
    return true;
  }

  void MtlContextImpl::clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset,
                                      MtlSize extent, MtlAspectMask aspects,
                                      MtlClearValue value) {
    /* D3D9 ColorFill and partial-rect Clear. Depth/stencil aspects arrive
     * from Clear() with a viewport/scissor rect smaller than the surface —
     * load-action clears are scissor-blind, so those take a depth-writing
     * draw instead. */
    if (view == nullptr || view->image == nullptr)
      return;
    if (!(aspects & MtlAspectColor)) {
      clearDepthStencilRect(view, offset, extent, aspects, value);
      return;
    }

    const MtlExtent3D mipExtent = view->image->mipLevelExtent(view->key.baseMip);
    const bool fullRect = offset.x == 0u && offset.y == 0u
                       && extent.width  >= mipExtent.width
                       && extent.height >= mipExtent.height;

    if (fullRect) {
      /* Whole subresource: a load=Clear/store=Store mini-pass (free on TBDR,
       * and works for every renderable format) — the standalone-clear shape
       * from clearRenderTarget, targeting the caller's view and value. */
      /* DEFERRED MODE: park the active pass first so the swap below can't
       * hand its chunk to the mini-pass (see clearRenderTarget). */
      if (deferredPassesEnabled() && m_pass.active) {
        prof::bump(prof::CntPassCloseClear);
        closePass();
      }
      PendingPass saved = std::move(m_pass);
      m_pass = PendingPass();
      m_pass.targets.color[0] = view;
      m_pass.clearColor[0]    = true;
      m_pass.clearValues[0]   = value;
      m_pass.pending          = true;
      prof::bump(prof::CntPassCloseClear);
      materializePass();
      closePass();
      m_pass = std::move(saved);
      return;
    }

    /* Partial rect: Metal load-action clears are scissor-blind, so a sub-rect
     * fill must be a scissored solid-color draw (fillFsMain via the meta-blit
     * table/triangle; no texture). */
    if (!ensureBlitPipeline())
      return;

    uint32_t x1 = std::min(offset.x + extent.width,  mipExtent.width);
    uint32_t y1 = std::min(offset.y + extent.height, mipExtent.height);
    if (offset.x >= x1 || offset.y >= y1)
      return;

    RenderPsoKey key = {};
    key.vsCookie              = uint64_t(m_blitVs->library);
    key.fsCookie              = uint64_t(m_fillFs->library);
    key.colors[0].pixelFormat = uint32_t(view->key.format);
    key.colorCount            = 1u;
    key.sampleCount           = uint8_t(std::max(view->image->info().sampleCount, 1u));
    key.topologyClass         = uint8_t(WMTPrimitiveTopologyClassTriangle);
    obj_handle_t pso = m_psoCache.get(
      key, m_blitVs->library, m_fillFs->library,
      m_blitVs->entryName, m_fillFs->entryName);
    if (!pso)
      return;

    prepareDirectEncode(uint32_t(prof::CntPassCloseClear));

    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    view->image->setTrackingSerial(m_currentSerial);
    m_keepAlive.push_back(view);

    WMTRenderPassInfo info = {};
    info.colors[0].texture      = tv->owned.handle();
    info.colors[0].load_action  = WMTLoadActionLoad;
    info.colors[0].store_action = WMTStoreActionStore;
    info.render_target_width    = mipExtent.width;
    info.render_target_height   = mipExtent.height;

    WMTViewport vp = {};
    vp.originX = double(offset.x);
    vp.originY = double(offset.y);
    vp.width   = double(x1 - offset.x);
    vp.height  = double(y1 - offset.y);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = offset.x;
    sc.y      = offset.y;
    sc.width  = x1 - offset.x;
    sc.height = y1 - offset.y;

    /* BlitParams: uvRect unused by fillFsMain; fillColor carries the value.
     * Ring slice + blit-table write happen at RECORD time (snapshot
     * semantics; the table is retire-by-serial so no later blit can rewrite
     * it before the parked encoder replays). */
    float params[8] = {
      0.f, 0.f, 0.f, 0.f,
      value.color.r, value.color.g, value.color.b, value.color.a,
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);

    encodeDirect([this, keepView = view, info, pso, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
    m_dirty = DirtyEverything;
  }

  void MtlContextImpl::clearDepthStencilRect(const Rc<MtlTextureView>& view, MtlOrigin offset,
                                             MtlSize extent, MtlAspectMask aspects,
                                             MtlClearValue value) {
    /* Scissored depth/stencil clear: a rasterization-only draw whose vertex z
     * carries the clear depth (depthClearVsMain, no fragment function) and
     * whose DSS forces write/replace; stencil value rides the reference. The
     * surface's OTHER aspect is attached load=Load/store=Store so it survives
     * untouched (its DSS write stays off). */
    const MtlFormatInfo* fi = lookupFormatInfo(view->key.format);
    const bool hasDepth   = (fi->flags & MtlFormatFlagDepth) != 0u;
    const bool hasStencil = (fi->flags & MtlFormatFlagStencil) != 0u;
    const bool clearDepth   = hasDepth   && (aspects & MtlAspectDepth) != 0u;
    const bool clearStencil = hasStencil && (aspects & MtlAspectStencil) != 0u;
    if (!clearDepth && !clearStencil)
      return;

    if (!ensureBlitPipeline())
      return;

    const MtlExtent3D mipExtent = view->image->mipLevelExtent(view->key.baseMip);
    uint32_t x1 = std::min(offset.x + extent.width,  mipExtent.width);
    uint32_t y1 = std::min(offset.y + extent.height, mipExtent.height);
    if (offset.x >= x1 || offset.y >= y1)
      return;

    /* Vertex-only PSO: colorCount 0, fs absent. depthFormat (a Metal-3-only
     * key field, never in the Metal 4 descriptor) discriminates this key from
     * anything else built on the blit library. */
    RenderPsoKey key = {};
    key.vsCookie      = uint64_t(m_depthClearVs->library);
    key.fsCookie      = 0u;
    key.colorCount    = 0u;
    key.sampleCount   = uint8_t(std::max(view->image->info().sampleCount, 1u));
    key.topologyClass = uint8_t(WMTPrimitiveTopologyClassTriangle);
    key.depthFormat   = uint32_t(view->key.format);
    obj_handle_t pso = m_psoCache.get(
      key, m_depthClearVs->library, 0u, m_depthClearVs->entryName, nullptr);
    if (!pso)
      return;

    MtlDepthStencilState ds = {};
    ds.depthTest      = true;
    ds.depthCompareOp = MtlCompareFunction::Always;
    ds.depthWrite     = clearDepth;
    ds.stencilEnable  = clearStencil;
    if (clearStencil) {
      MtlStencilState s = {};
      s.compareOp = MtlCompareFunction::Always;
      s.failOp = s.depthFailOp = s.passOp = MtlStencilOperation::Replace;
      ds.front = ds.back = s;
    }
    obj_handle_t dsso = m_device->getDepthStencilState(ds);

    prepareDirectEncode(uint32_t(prof::CntPassCloseClear));

    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    view->image->setTrackingSerial(m_currentSerial);
    m_keepAlive.push_back(view);

    WMTRenderPassInfo info = {};
    if (hasDepth) {
      info.depth.texture      = tv->owned.handle();
      info.depth.load_action  = WMTLoadActionLoad;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.level        = view->key.baseMip;
      info.depth.slice        = view->key.baseLayer;
    }
    if (hasStencil) {
      info.stencil.texture      = tv->owned.handle();
      info.stencil.load_action  = WMTLoadActionLoad;
      info.stencil.store_action = WMTStoreActionStore;
      info.stencil.level        = view->key.baseMip;
      info.stencil.slice        = view->key.baseLayer;
    }
    info.render_target_width  = mipExtent.width;
    info.render_target_height = mipExtent.height;

    WMTViewport vp = {};
    vp.originX = double(offset.x);
    vp.originY = double(offset.y);
    vp.width   = double(x1 - offset.x);
    vp.height  = double(y1 - offset.y);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = offset.x;
    sc.y      = offset.y;
    sc.width  = x1 - offset.x;
    sc.height = y1 - offset.y;

    /* BlitParams: fillColor.x = clear depth (vertex z passthrough). Record-
     * time ring slice + blit-table write (same contract as clearImageView). */
    float params[8] = {
      0.f, 0.f, 0.f, 0.f,
      value.depth, 0.f, 0.f, 0.f,
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);

    const uint32_t stencilRef = value.stencil;
    encodeDirect([this, keepView = view, info, pso, dsso, clearStencil,
                  stencilRef, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setDepthStencilState(enc, dsso);
      if (clearStencil)
        MTL4RenderCommandEncoder_setStencilReferenceValue(enc, stencilRef);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
    m_dirty = DirtyEverything;
  }

  obj_handle_t MtlContextImpl::selectBlitArgTable() {
    /* Same retire-by-serial ring discipline as selectArgumentTablesForPass
     * (a table must not be rewritten while an earlier blit that reads it is
     * in flight), but a DEDICATED pool: the blit must never advance the
     * per-draw D3D9 table cursor or rewrite a table a queued draw reads.
     * Bounded by construction: grows only while every table is in flight. */
    const uint64_t completed = m_device->submissions().completedSerial();
    const size_t   n         = m_blitArgTables.size();

    for (size_t scan = 0u; scan < n; ++scan) {
      const size_t idx = (m_blitArgCursor + scan) % n;
      if (m_blitArgTables[idx].serial <= completed) {
        m_blitArgCursor              = uint32_t(idx + 1u);
        m_blitArgTables[idx].serial  = m_currentSerial;
        return m_blitArgTables[idx].table.handle();
      }
    }

    WMTArgTableInfo ati = {};
    ati.max_buffers         = 1u;   /* buffer(0) = BlitParams */
    ati.max_textures        = 1u;   /* texture(0) = source */
    ati.max_samplers        = 1u;   /* sampler(0) */
    ati.initialize_bindings = 1u;
    BlitArgTable entry;
    entry.table  = WmtArgTable::adopt(
      MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    entry.serial = m_currentSerial;
    m_blitArgTables.push_back(std::move(entry));
    m_blitArgCursor = uint32_t(m_blitArgTables.size());
    return m_blitArgTables.back().table.handle();
  }

  void MtlContextImpl::blitTextureView(const Rc<MtlTextureView>& dstView, const MtlOffset3D dstOffsets[2],
                                       const Rc<MtlTextureView>& srcView, const MtlOffset3D srcOffsets[2],
                                       MtlSamplerFilter filter) {
    /* §6.6 meta-blit: sampled fullscreen-triangle draw into the dst rect.
     * load=Load/store=Store — a rect blit preserves the rest of the target. */
    if (dstView == nullptr || srcView == nullptr
     || dstView->image == nullptr || srcView->image == nullptr)
      return;

    /* Depth-format destinations never reach this seam (the frontend rejects
     * stretched DS blits); a color pipeline cannot write one. */
    const MtlFormatInfo* dstFi = lookupFormatInfo(dstView->key.format);
    if (unlikely(dstFi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil))) {
      Logger::err("MtlContextImpl::blitTextureView: depth-stencil destination unsupported");
      return;
    }

    /* The fragment shader samples texture2d; multi-layer sources would need
     * an array variant. The frontend sends layerCount==1 views (e2D) in
     * practice — sample slice 0 of whatever arrives and say so once. */
    if (unlikely(srcView->key.viewType != MtlTextureViewType::e2D)) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl::blitTextureView: non-2D source view, sampling as 2D"); }
    }

    if (!ensureBlitPipeline())
      return;

    /* Normalize the rects: a reversed DST rect is the same blit with both
     * rects flipped on that axis; a reversed SRC rect then mirrors via the
     * UV direction (uv0 > uv1 interpolates backwards). */
    int32_t dx0 = dstOffsets[0].x, dy0 = dstOffsets[0].y;
    int32_t dx1 = dstOffsets[1].x, dy1 = dstOffsets[1].y;
    float   sx0 = float(srcOffsets[0].x), sy0 = float(srcOffsets[0].y);
    float   sx1 = float(srcOffsets[1].x), sy1 = float(srcOffsets[1].y);
    if (dx0 > dx1) { std::swap(dx0, dx1); std::swap(sx0, sx1); }
    if (dy0 > dy1) { std::swap(dy0, dy1); std::swap(sy0, sy1); }
    if (dx0 == dx1 || dy0 == dy1)
      return;                                    /* empty dst rect */

    const MtlExtent3D srcExtent = srcView->image->mipLevelExtent(srcView->key.baseMip);
    const MtlExtent3D dstExtent = dstView->image->mipLevelExtent(dstView->key.baseMip);
    if (!srcExtent.width || !srcExtent.height)
      return;

    /* Frontend-validated to lie within the dst mip; clamp anyway — an
     * out-of-bounds scissor is a Metal validation fault, not a bad pixel. */
    dx0 = std::max(dx0, 0);
    dy0 = std::max(dy0, 0);
    dx1 = std::min(dx1, int32_t(dstExtent.width));
    dy1 = std::min(dy1, int32_t(dstExtent.height));
    if (dx0 >= dx1 || dy0 >= dy1)
      return;

    /* PSO through the shared render-PSO cache: blit library cookies + dst
     * format + sample count; no vertex descriptor, no blend, write all. */
    RenderPsoKey key = {};
    key.vsCookie             = uint64_t(m_blitVs->library);
    key.fsCookie             = uint64_t(m_blitFs->library);
    key.colors[0].pixelFormat = uint32_t(dstView->key.format);
    key.colorCount           = 1u;
    key.sampleCount          = uint8_t(std::max(dstView->image->info().sampleCount, 1u));
    key.topologyClass        = uint8_t(WMTPrimitiveTopologyClassTriangle);
    obj_handle_t pso = m_psoCache.get(
      key, m_blitVs->library, m_blitFs->library,
      m_blitVs->entryName, m_blitFs->entryName);
    if (!pso)
      return;                                    /* build failure already logged */

    /* A blit must observe folded, not-yet-drawn clears (prepareDirectEncode
     * precedent); an open pass flushes its fragment writes via closePass. */
    prepareDirectEncode(uint32_t(prof::CntPassCloseBlit));

    auto* dstTv = static_cast<MtlTextureViewImpl*>(dstView.ptr());
    auto* srcTv = static_cast<MtlTextureViewImpl*>(srcView.ptr());
    dstView->image->setTrackingSerial(m_currentSerial);
    srcView->image->setTrackingSerial(m_currentSerial);
    /* The blit SAMPLES src — earlier passes on it must keep their Store. */
    noteTransferRead(srcView->image.ptr());
    /* The view objects are typically owned only by the recorded CS lambda;
     * their Metal resource IDs must outlive this submission. */
    m_keepAlive.push_back(dstView);
    m_keepAlive.push_back(srcView);

    WMTRenderPassInfo info = {};
    info.colors[0].texture      = dstTv->owned.handle();
    info.colors[0].load_action  = WMTLoadActionLoad;
    info.colors[0].store_action = WMTStoreActionStore;
    info.render_target_width    = dstExtent.width;
    info.render_target_height   = dstExtent.height;

    /* Viewport = dst rect: the fullscreen triangle's t [0,1] spans exactly
     * the rect, so uv interpolates uv0 -> uv1 across it (dst pixel centers
     * land at the D3D9 (x+0.5)/w source positions — box-correct for the
     * downsample chains this path serves). Scissor clips the overhang. */
    WMTViewport vp = {};
    vp.originX = double(dx0);
    vp.originY = double(dy0);
    vp.width   = double(dx1 - dx0);
    vp.height  = double(dy1 - dy0);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = uint32_t(dx0);
    sc.y      = uint32_t(dy0);
    sc.width  = uint32_t(dx1 - dx0);
    sc.height = uint32_t(dy1 - dy0);

    /* BlitParams via a fresh UploadRing slice (snapshot semantics: never
     * rewritten under this queued blit), through the dedicated blit table.
     * All table writes happen at RECORD time; retire-by-serial keeps the
     * table untouched until the parked encoder replays. */
    float params[4] = {
      sx0 / float(srcExtent.width), sy0 / float(srcExtent.height),
      sx1 / float(srcExtent.width), sy1 / float(srcExtent.height),
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);
    MTL4ArgumentTable_setTexture(table, srcTv->gpuResourceId, 0u);
    auto* smp = static_cast<MtlSamplerStateImpl*>(
      m_blitSamplers[filter == MtlSamplerFilter::Linear ? 1u : 0u].ptr());
    MTL4ArgumentTable_setSamplerState(table, smp->gpuResourceId, 0u);

    encodeDirect([this, keepDst = dstView, keepSrc = srcView, info, pso, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      /* §4c: order the blit's fragment write ahead of a later sample/copy of
       * the dst (same contract as closePass/clearImage). */
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });

    /* The next D3D9 draw rebinds its own encoder state from scratch. */
    m_dirty = DirtyEverything;
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
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (view->image != nullptr) {
      view->image->setTrackingSerial(m_currentSerial);
      /* mipgen READS level 0 of the texture. */
      noteTransferRead(view->image.ptr());
    }
    encodeDirect([this, keepView = view, tex](obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_generateMipmaps(enc, tex);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  // --- init / upload / discard (contracts 3a,3c,3g) --------------------------

  void MtlContextImpl::initBuffer(const Rc<MtlBuffer>& buffer) {
    auto* b = static_cast<MtlBufferImpl*>(buffer.ptr());
    if (!b)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    b->setTrackingSerial(m_currentSerial);
    const obj_handle_t buf  = b->handle.buf;
    const uint64_t     size = b->size;
    encodeDirect([this, keep = buffer, buf, size](obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_fillBuffer(enc, buf, 0u, size, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
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

    if (m_pass.active) {
      prof::bump(prof::CntPassCloseBlit);
      closePass();
    }
    ensureCommandBuffer();
    t->setTrackingSerial(m_currentSerial);

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
    encodeDirect([this, keep = image, info](obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = MTL4CommandBuffer_renderCommandEncoder(cmdbuf, &info);
      /* §4c: order the clear's fragment write ahead of a later sample/copy. */
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) {
    /* Contract 3a / decision 6. Runs on the CS thread right after the
     * matching assignStorage, in command order. Two duties:
     *
     * 1. Retarget LIVE bindings (DXVK's invalidateBuffer dirty-tracking):
     *    bound slices are record-time physical snapshots, so a bind-once-
     *    then-Discard pattern (SetStreamSource once, Lock DISCARD per draw;
     *    constant-buffer rings) must re-snapshot here — draws recorded after
     *    this command read the new backing, draws recorded before it keep
     *    their old snapshot untouched.
     *
     * 2. Retire the OLD backing: commands referencing it live in the current
     *    or an older command buffer, so m_currentSerial bounds its last GPU
     *    use. Once that serial completes the backing returns to the buffer's
     *    recycle pool (DXVK's slice cache); renameBuffer hands it out for a
     *    later DISCARD instead of minting a new Metal buffer per Lock. Pool
     *    overflow (or a dead buffer) drops the Rc into deferred destruction. */
    if (slice == nullptr)
      return;
    ensureCommandBuffer();

    /* The retargeted gpuAddress must reach the NEXT draw's table writes: the
     * per-draw binding flush is gated on these dirty bits (prepareDraw), so a
     * silent refresh without them would leave the old backing bound. (The
     * index buffer needs no bit — its address is read live per drawIndexed,
     * not through an argument table.) */
    for (uint32_t i = 0u; i < m_vertexStreams.size(); i++) {
      auto& stream = m_vertexStreams[i];
      if (stream.slice.buffer() == buffer) {
        stream.slice.refreshStorage();
        m_dirtyVertexStreams |= 1u << i;
        m_dirty |= DirtyVertexBuffers;
      }
    }
    if (m_indexBuffer.buffer() == buffer)
      m_indexBuffer.refreshStorage();
    for (uint32_t st = 0u; st < 2u; st++) {
      for (uint32_t slot = 0u; slot < m_bindings[st].cbvs.size(); slot++) {
        auto& cbv = m_bindings[st].cbvs[slot];
        if (cbv.slice.buffer() == buffer) {
          cbv.slice.refreshStorage();
          m_bindings[st].dirtyCbvs |= 1u << slot;
          m_dirty |= DirtyResources;
        }
      }
    }

    Rc<MtlBuffer> owner = buffer;
    Rc<MtlAllocation> keep = std::move(slice);
    m_device->submissions().retireAfter(m_currentSerial, [owner, keep]() mutable {
      if (owner != nullptr)
        static_cast<MtlBufferImpl*>(owner.ptr())->recycleStorage(std::move(keep));
      owner = nullptr;
      keep  = nullptr;
    });
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
    /* Written by the GPU across every pass — resident for the context
     * lifetime; leaves via allocDestroyed in ~MtlContextImpl. */
    m_device->residency().allocCreated(h);
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

    /* Rides the wmtcmd stream so it stays ordered against the batched draws
     * it brackets (a direct call here would overtake them). */
    if (m_pass.active) {
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setvisibilitymode>(
        uint16_t(WMTMtl4RenderCommandSetVisibilityResultMode));
      cmd->mode   = uint64_t(WMTVisibilityResultModeCounting);
      cmd->offset = q->slotOffset;
    }
  }

  void MtlContextImpl::endQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();

    /* Same stream-ordering rule as beginQuery. */
    if (m_pass.active) {
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setvisibilitymode>(
        uint16_t(WMTMtl4RenderCommandSetVisibilityResultMode));
      cmd->mode   = uint64_t(WMTVisibilityResultModeDisabled);
      cmd->offset = 0u;
    }
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
    /* Contract: CPU-visible ON COMPLETION of the recording submission.
     * The frontend's staging-buffer fence rides this — a signal that never
     * fires strands every retired staging backing forever (and deadlocks
     * ThrottleAllocation once the in-flight budget is hit). */
    ensureCommandBuffer();
    Rc<MtlSignal> keep = signal;
    m_device->submissions().retireAfter(m_currentSerial,
      [keep, value]() { keep->signal(value); });
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
