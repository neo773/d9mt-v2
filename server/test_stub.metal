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
