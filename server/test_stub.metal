/*
 * test_stub.metal — shader stub for the S2 test gate (test_feed.c).
 *
 * Compiled to a metallib by scripts/build-server.sh (xcrun metal) and fed to
 * the server through create_shader wire records. No stage-in (the synthetic
 * frame binds no vertex buffers): the VS generates a fullscreen triangle
 * from vertex_id; the FS returns solid red — so the executed draws are
 * verifiable by a one-pixel readback of the presented IOSurface.
 */

#include <metal_stdlib>
using namespace metal;

struct VsOut {
  float4 pos [[position]];
};

vertex VsOut vsMain(uint vid [[vertex_id]]) {
  /* fullscreen triangle: (-1,-1) (3,-1) (-1,3) */
  VsOut o;
  float2 p = float2(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0);
  o.pos = float4(p, 0.0, 1.0);
  return o;
}

fragment float4 fsMain(VsOut in [[stage_in]]) {
  return float4(1.0, 0.0, 0.0, 1.0); /* red */
}

/* S3 texture-content gate: fullscreen triangle with UVs + a nearest-sampled
 * texture at fragment slot 0 (constexpr sampler: no sampler binding needed).
 * The pixel readback asserts the uploaded checkerboard cells. */
struct VsTexOut {
  float4 pos [[position]];
  float2 uv;
};

vertex VsTexOut vsTex(uint vid [[vertex_id]]) {
  VsTexOut o;
  float2 p = float2(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0);
  o.pos = float4(p, 0.0, 1.0);
  o.uv = float2((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5);
  return o;
}

fragment float4 fsTex(VsTexOut in [[stage_in]],
                      texture2d<float> tex [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::nearest, address::clamp_to_edge);
  return tex.sample(s, in.uv);
}
