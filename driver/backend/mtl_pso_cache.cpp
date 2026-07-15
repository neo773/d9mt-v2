/*
 * RenderPsoCache — synchronous render-PSO build through MTL4Compiler
 * (decision 9, METAL 4 ONLY). MTL4Compiler_newRenderPipelineState takes a
 * WMTMtl4RenderPipelineInfo carrying the VS/PS metallib handles + entry
 * names, colour formats+blend, sample count and the vertex descriptor — no
 * depth/stencil format, no tessellation. We include the bridge header FIRST
 * so its superset definitions win (shared _WINEMETAL_H guard).
 */
#include "../../bridge/winemetal.h"

#include "mtl_pso_cache.h"

#include <algorithm>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  namespace {
    /* D3D9/Vk colour write mask (R=1,G=2,B=4,A=8) -> WMTColorWriteMask
     * (R=8,G=4,B=2,A=1). */
    uint8_t wmtWriteMask(uint8_t v) {
      uint8_t m = 0u;
      if (v & 0x1u) m |= WMTColorWriteMaskRed;
      if (v & 0x2u) m |= WMTColorWriteMaskGreen;
      if (v & 0x4u) m |= WMTColorWriteMaskBlue;
      if (v & 0x8u) m |= WMTColorWriteMaskAlpha;
      return m;
    }
  }

  RenderPsoCache::RenderPsoCache(obj_handle_t compiler)
  : m_compiler(compiler) { }

  obj_handle_t RenderPsoCache::get(
    const RenderPsoKey& key, obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry) {
    /* DIAGNOSTIC: D9MT_NOPSOCACHE forces a fresh build with the LIVE shader
     * libraries every draw (drops the memo + any cached entry for this key).
     * Tests whether a stale/collided cached PSO — e.g. a reused vs/fs library
     * handle mapping to the same key — is rendering wrong (block artwork). Slow;
     * for diagnosis only. */
    static const bool s_noPsoCache = std::getenv("D9MT_NOPSOCACHE") != nullptr;
    if (s_noPsoCache) {
      m_hasLast = false;
      m_cache.erase(key);
    }

    /* one-entry memo: same-material draw runs skip the map entirely */
    if (m_hasLast && m_lastKey == key)
      return m_lastPso;

    auto entry = m_cache.find(key);
    if (entry != m_cache.end()) {
      m_hasLast = true;
      m_lastKey = key;
      m_lastPso = entry->second.handle();
      return m_lastPso;
    }

    obj_handle_t pso = build(key, vsLibrary, fsLibrary, vsEntry, fsEntry);
    if (!pso)
      return 0u;

    m_cache.emplace(key, WmtRenderPso::adopt(pso));
    m_hasLast = true;
    m_lastKey = key;
    m_lastPso = pso;
    return pso;
  }

  obj_handle_t RenderPsoCache::build(
    const RenderPsoKey& key, obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry) {

    WMTMtl4RenderPipelineInfo info = {};
    info.vertex_library            = vsLibrary;
    info.fragment_library          = fsLibrary;
    /* AIR entry point is "main" for MSC-compiled app shaders (air_builder +
     * air_render_test); the hand-authored fixed-function ubershaders carry
     * their own entry names ("ffVsMain"/"ffFsMain") since Metal reserves
     * "main". Fragment is optional (depth-only passes); leave its name null
     * when no PS. */
    info.vertex_entry_name.set(vsEntry ? vsEntry : "main");
    info.fragment_entry_name.set(fsLibrary ? (fsEntry ? fsEntry : "main") : nullptr);
    if (vsEntry && vsEntry[0] == 'f' && vsEntry[1] == 'f') {
      static bool s_ffOnce = false;
      if (!s_ffOnce) { s_ffOnce = true;
        Logger::warn(str::format("D9MT-FF: building FF render PSO vsEntry=", vsEntry,
          " fsEntry=", (fsLibrary && fsEntry) ? fsEntry : "(none)",
          " attrs=", uint32_t(key.attrCount), " colors=", uint32_t(key.colorCount))); }
    }
    info.rasterization_enabled     = 1u;
    info.raster_sample_count       = key.sampleCount ? key.sampleCount : 1u;
    info.alpha_to_coverage_enabled = key.alphaToCoverage != 0u ? 1u : 0u;
    info.input_primitive_topology  = uint32_t(WMTPrimitiveTopologyClass(key.topologyClass));

    for (uint8_t i = 0u; i < key.colorCount && i < 8u; i++) {
      const PsoColorAttachment& k = key.colors[i];
      WMTColorAttachmentBlendInfo& c = info.colors[i];
      c.pixel_format          = WMTPixelFormat(k.pixelFormat);
      c.blending_enabled      = k.blendEnabled != 0u;
      c.write_mask            = wmtWriteMask(k.writeMask);
      c.rgb_blend_operation   = WMTBlendOperation(k.rgbBlendOp);
      c.alpha_blend_operation = WMTBlendOperation(k.alphaBlendOp);
      c.src_rgb_blend_factor  = WMTBlendFactor(k.rgbSrcFactor);
      c.dst_rgb_blend_factor  = WMTBlendFactor(k.rgbDstFactor);
      c.src_alpha_blend_factor = WMTBlendFactor(k.alphaSrcFactor);
      c.dst_alpha_blend_factor = WMTBlendFactor(k.alphaDstFactor);
    }

    WMTRenderPipelineVertexInfo& vinfo = info.vertex;
    vinfo.num_attributes = key.attrCount;
    for (uint8_t i = 0u; i < key.attrCount && i < 31u; i++) {
      const PsoVertexAttr& k = key.attrs[i];
      WMTVertexAttributeInfo& a = vinfo.attributes[i];
      a.format       = WMTAttributeFormat(k.format);
      a.offset       = k.offset;
      a.buffer_index = k.bufferIndex;
    }

    uint32_t numLayouts = 0u;
    for (uint8_t i = 0u; i < key.layoutCount && i < 31u; i++) {
      const PsoVertexLayout& k = key.layouts[i];
      uint32_t idx = k.pad;                 /* pad repurposed as buffer index */
      if (idx >= 31u)
        continue;
      WMTVertexBufferLayoutInfo& l = vinfo.layouts[idx];
      l.stride        = k.stride;
      l.step_function = k.stepFunction == 2u
        ? WMTVertexStepFunctionPerInstance
        : WMTVertexStepFunctionPerVertex;
      l.step_rate     = 1u;
      numLayouts = std::max(numLayouts, idx + 1u);
    }
    vinfo.num_layouts = numLayouts;

    /* D3D9 sm3 vertex shaders always declare a point-size output; Metal
     * rejects that against an explicit primitive-topology class, so the
     * pipeline's topology class stays Unspecified (matches the classic
     * vertex-descriptor path). Per-draw primitive type is set on the
     * encoder, not baked into the PSO. */
    info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;

    obj_handle_t err = 0u;
    obj_handle_t pso = MTL4Compiler_newRenderPipelineState(m_compiler, &info, &err);

    if (!pso) {
      if (err) {
        char buf[512] = {};
        obj_handle_t desc = NSObject_description(err);
        if (desc) {
          NSString_getCString(desc, buf, sizeof(buf) - 1u, WMTUTF8StringEncoding);
          NSObject_release(desc);   /* description crosses the bridge as +1 */
        }
        Logger::err(str::format("RenderPsoCache: pipeline build failed: ", buf));
        NSObject_release(err);
      } else {
        Logger::err("RenderPsoCache: pipeline build failed (no error object)");
      }
    }
    return pso;
  }

}
