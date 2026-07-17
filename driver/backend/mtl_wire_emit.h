#pragma once
/*
 * mtl_wire_emit.h — cross-file surface of the S2 wire producer
 * (implemented in mtl_wire_context.cpp). Three seams outside the wire
 * context need to talk to the transport:
 *
 *  - resource destructors (mtl_device_impl.{h,cpp}) queue destroy_resource
 *    records from ARBITRARY threads; the CS-thread producer drains the
 *    queue into the ring before its next record (so a destroy can never
 *    overtake the ring position of the resource's last use).
 *  - the swapchain (d3d9_swapchain.cpp) names its current backbuffer image
 *    (app thread) so the CS producer can flag backbuffer render targets and
 *    stamp presentFrame records.
 *  - the swapchain's present path reports each local present so the PROF
 *    line can compare the server's produced-frame serial against the local
 *    one (S2 gate: the server produces frames AT RATE).
 *
 * Header is PE-safe (mingw x86_64 C++17): no unix/ObjC types.
 */

#include <cstdint>

#include "../util/rc/util_rc_ptr.h"

namespace dxvk { class MtlTexture; class MtlTextureView; class MtlDevice; }

namespace dxvk::backend::wire {

  /* App thread (PresentImage): the local present serial this frame will use
   * (matches the counter the server echoes as status.frame_serial). Captured
   * into the present closure so the CS-thread server-frame gate compares like
   * for like. */
  uint64_t nextPresentSerial();

  /* CS thread (inside the present closure): the S3 VISUAL CUTOVER source
   * switch. When the wire is live, the server is not dead, and it has published
   * a completed frame within the lag budget for `localSerial` in a valid slot
   * with a nonzero IOSurface id, wraps that IOSurface (unix-cached per
   * {id,generation}) as a borrowed MtlTexture + full-image view and returns
   * true — the caller blits THOSE into the drawable instead of the local
   * backbuffer. Returns false for a per-present fallback to the local
   * backbuffer (wire disabled, server dead, frame too stale, or lookup
   * failure); dual execution keeps the local frame always ready. */
  bool acquireServerFrame(const Rc<MtlDevice>& device, uint64_t localSerial,
                          Rc<MtlTexture>& outImage, Rc<MtlTextureView>& outView);

  /* Any thread. No-op when the resource never got a wire identity. */
  void queueDestroy(uint64_t wireId, uint64_t clientHandle);

  /* App thread (PresentImage): the image the present blit reads — D3D9
   * back buffer 0. Held as an Rc; the CS producer compares pointers and
   * assigns/reads the wire id on its own thread. */
  void noteBackbuffer(const Rc<MtlTexture>& image);

  /* App thread (CreateBackBuffers, init + Reset): register EVERY swapchain
   * backbuffer image. S3 v11: with BackBufferCount > 1 the rotation means
   * frame N draws into a different image than the one noteBackbuffer saw at
   * present N-1 — a single-pointer compare never matches, no RT bind ever
   * carries the BACKBUFFER flag, and the server renders into plain textures
   * while its published IOSurface stays black. Membership in THIS set is
   * what flags render targets. Replaces the previous set (Reset). */
  void noteSwapchainImages(const Rc<MtlTexture>* images, uint32_t count);

  /* App thread (PresentImage, after the present was queued): bumps the
   * local present counter and samples the server's produced-frame serial;
   * feeds the PROF `srvFrames`/`srvPace` fields. Cheap (one unixcall). */
  void presentObserved();

}
