/*
 * RenderPsoCache — synchronous render-PSO build through the winemetal BRIDGE
 * vertex-descriptor entry (decision 9). MtlDevice_newRenderPipelineStateWith-
 * VertexDescriptor (bridge/winemetal.h, code 153) is a d9mt addition absent
 * from third_party/winemetal; we include the bridge header FIRST so its
 * superset definitions win (shared _WINEMETAL_H guard makes the subsequent
 * third_party include via wmt_handle.h a no-op).
 */
#include "../../bridge/winemetal.h"

#include "mtl_pso_cache.h"

#include <algorithm>

#include "../util/log/log.h"

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

  RenderPsoCache::RenderPsoCache(obj_handle_t device)
  : m_device(device) { }

  obj_handle_t RenderPsoCache::get(
    const RenderPsoKey& key, obj_handle_t vsFunction, obj_handle_t fsFunction) {
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

    obj_handle_t pso = build(key, vsFunction, fsFunction);
    if (!pso)
      return 0u;

    m_cache.emplace(key, WmtRenderPso::adopt(pso));
    m_hasLast = true;
    m_lastKey = key;
    m_lastPso = pso;
    return pso;
  }

  obj_handle_t RenderPsoCache::build(
    const RenderPsoKey& key, obj_handle_t vsFunction, obj_handle_t fsFunction) {

    WMTRenderPipelineInfo info = {};
    info.vertex_function            = vsFunction;
    info.fragment_function          = fsFunction;
    info.rasterization_enabled      = true;
    info.raster_sample_count        = key.sampleCount ? key.sampleCount : 1u;
    info.alpha_to_coverage_enabled  = key.alphaToCoverage != 0u;
    info.input_primitive_topology   = WMTPrimitiveTopologyClass(key.topologyClass);
    info.depth_pixel_format         = WMTPixelFormat(key.depthFormat);
    info.stencil_pixel_format       = WMTPixelFormat(key.stencilFormat);
    /* Metal validation requires max_tessellation_factor >= 1 even for the
     * non-tessellated path (decision 9). */
    info.max_tessellation_factor            = 64u;
    info.tessellation_partition_mode        = WMTTessellationPartitionModePow2;
    info.tessellation_output_winding_order  = WMTWindingClockwise;
    info.tessellation_factor_step           = WMTTessellationFactorStepFunctionConstant;

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

    WMTRenderPipelineVertexInfo vinfo = {};
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

    obj_handle_t err = 0u;
    obj_handle_t pso = MTLDevice_newRenderPipelineStateWithVertexDescriptor(
      m_device, &info, &vinfo, &err);

    if (!pso) {
      Logger::err("RenderPsoCache: pipeline build failed");
      if (err)
        NSObject_release(err);
    }
    return pso;
  }

}
