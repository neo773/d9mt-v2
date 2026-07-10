#pragma once

#include <cstdint>

#include "mtl_types.h"
#include "mtl_resource.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

// ---------------------------------------------------------------------------
// mtl_presenter.h — thin present seam for the D3D9 swapchain REWRITE track
// (doc 01 §1j, contract 3e; doc 02 §4 "D3D9SwapChainEx REWRITE").
//
// This is the Metal-first replacement for DXVK's `Rc<Presenter>` +
// `DxvkSwapchainBlitter`. The D3D9 swapchain keeps all of its D3D9 bookkeeping
// (present params, back-buffer array, reset/format logic, frame-latency counter)
// and drives the actual scan-out through this interface, whose real impl is a
// CAMetalLayer + drawable present via winemetal.
//
// PHASE-D: no CAMetalLayer / winemetal plumbing here — this header defines the
// contract, and mtl_presenter_stub.cpp is a log-and-return implementation so the
// swapchain compiles and links against the seam. Contract 3e (frame-latency
// signalling) is deadlock-class: the real impl must drive the latency signal from
// the drawable-present completion handler, never stub it live.
// ---------------------------------------------------------------------------

namespace dxvk {

  class MtlDevice;   // defined in mtl_context.h; the impl (stub) includes it.

  // Acquired scan-out target for a frame: the drawable's backing texture plus the
  // acquire/release sync tokens the command buffer must wait/signal on (contract 3e).
  struct MtlAcquiredDrawable {
    Rc<MtlTexture>  texture = nullptr;      // CAMetalDrawable.texture
    MtlObjectHandle drawable = MtlNullHandle;
    MtlPresenterSync sync   = { };          // acquire (wait) / release (signal) / frameId
    uint32_t        imageIndex = 0u;
    bool            valid   = false;        // false => acquire failed (e.g. occluded)
  };

  // Present configuration the swapchain hands the presenter on (re)create/reset.
  struct MtlPresenterDesc {
    MtlExtent2D imageExtent  = { };         // drawable size in RT pixels
    MtlFormat   imageFormat  = MtlFormat::BGRA8Unorm;
    uint32_t    imageCount   = 3u;          // triple-buffer default
    bool        vsync        = true;        // displaySyncEnabled
  };

  /**
   * \brief Metal-first present interface (was Rc<Presenter>)
   *
   * The swapchain REWRITE calls only these; the impl owns the CAMetalLayer, the
   * drawable pool, and the frame-latency shared event. Retina/backing-scale is
   * applied HERE (doc 01 §6.1) — the frontend viewports stay in RT-pixel coords.
   */
  class MtlPresenter : public RcObject {

  public:

    virtual ~MtlPresenter() { }

    // (Re)configure the layer for a new back-buffer size/format/mode. Called on
    // swapchain create and on D3D9 Reset.
    virtual void recreate(const MtlPresenterDesc& desc) = 0;

    // Acquire the next drawable (CAMetalLayer nextDrawable). May block on the
    // frame-latency semaphore to throttle to imageCount frames in flight.
    virtual MtlAcquiredDrawable acquireNextImage(uint64_t frameId) = 0;

    // Present the acquired drawable (presentDrawable:), wiring the release token as
    // the drawable-signal on the submitted command buffer.
    virtual void present(const MtlAcquiredDrawable& image) = 0;

    // Frame-latency signal (contract 3e) — posted from the present completion
    // handler; the swapchain's frame-limiter waits on it. Deadlock-class if stubbed.
    virtual void signalFrameLatency(uint64_t frameId) = 0;

    // Current drawable size (post backing-scale) so the swapchain can size its
    // back buffers / blit target.
    virtual MtlExtent2D getImageExtent() const = 0;
  };

  // Factory — PHASE-D returns the logging stub (mtl_presenter_stub.cpp). The real
  // impl takes a HWND / CAMetalLayer + MtlDevice.
  Rc<MtlPresenter> createMtlPresenter(const Rc<MtlDevice>& device, const MtlPresenterDesc& desc);

}
