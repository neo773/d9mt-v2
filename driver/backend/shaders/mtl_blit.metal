/*
 * mtl_blit.metal — the meta-blit pipeline behind MtlContextImpl::blitTextureView
 * (mtl_context.h §6.6). One fullscreen triangle; viewport+scissor confine
 * rasterization to the dst rect, and BlitParams.uvRect maps that rect onto the
 * source rect in normalized source-mip coordinates (uv0 may exceed uv1 on an
 * axis — that is a mirrored blit). Bound through the dedicated blit argument
 * table: buffer(0) = BlitParams, texture(0)/sampler(0) = source. Entry points
 * are blitVsMain/blitFsMain (Metal reserves "main" in hand-written MSL).
 */
#include <metal_stdlib>
using namespace metal;

struct BlitParams {
  float4 uvRect;    /* xy = uv at the dst rect's top-left, zw = at bottom-right */
  float4 fillColor; /* fillFsMain (clearImageView rect fills) — unused by blits */
};

struct BlitVsOut {
  float4 pos [[position]];
  float2 uv;
};

vertex BlitVsOut blitVsMain(uint vid [[vertex_id]],
                            constant BlitParams& params [[buffer(0)]]) {
  /* t = (0,0) (2,0) (0,2): the triangle covers the viewport (NDC [-1,1] maps
   * to t [0,1] on both axes); the overhang past t=1 is clipped/scissored. */
  float2 t = float2(float((vid << 1u) & 2u), float(vid & 2u));
  BlitVsOut out;
  out.pos = float4(t.x * 2.0f - 1.0f, 1.0f - t.y * 2.0f, 0.0f, 1.0f);
  out.uv  = params.uvRect.xy + t * (params.uvRect.zw - params.uvRect.xy);
  return out;
}

fragment float4 blitFsMain(BlitVsOut in [[stage_in]],
                           texture2d<float> src [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
  return src.sample(smp, in.uv);
}

/* Solid rect fill (clearImageView / D3D9 ColorFill with a partial rect):
 * Metal load-action clears are scissor-blind, so a sub-rect fill must be a
 * scissored draw. Same triangle/table as the blit; no texture. */
fragment float4 fillFsMain(BlitVsOut in [[stage_in]],
                           constant BlitParams& params [[buffer(0)]]) {
  return params.fillColor;
}

/* Scissored depth/stencil clear (clearImageView, non-color aspects — D3D9
 * Clear with a partial rect on a depth surface): load-action clears are
 * scissor-blind, so this is a depth-writing draw. No fragment function —
 * rasterization-only; the vertex z IS the clear value (fillColor.x, viewport
 * z [0,1] passthrough). Stencil rides the DSS replace-op + reference value. */
vertex float4 depthClearVsMain(uint vid [[vertex_id]],
                               constant BlitParams& params [[buffer(0)]]) {
  float2 t = float2(float((vid << 1u) & 2u), float(vid & 2u));
  return float4(t.x * 2.0f - 1.0f, 1.0f - t.y * 2.0f, params.fillColor.x, 1.0f);
}
