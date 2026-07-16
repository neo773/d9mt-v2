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

namespace dxvk { class MtlTexture; }

namespace dxvk::backend::wire {

  /* Any thread. No-op when the resource never got a wire identity. */
  void queueDestroy(uint64_t wireId, uint64_t clientHandle);

  /* App thread (PresentImage): the image the present blit reads — D3D9
   * back buffer 0. Held as an Rc; the CS producer compares pointers and
   * assigns/reads the wire id on its own thread. */
  void noteBackbuffer(const Rc<MtlTexture>& image);

  /* App thread (PresentImage, after the present was queued): bumps the
   * local present counter and samples the server's produced-frame serial;
   * feeds the PROF `srvFrames`/`srvPace` fields. Cheap (one unixcall). */
  void presentObserved();

}
