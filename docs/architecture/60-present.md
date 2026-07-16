# 60 — Present: swapchain / presenter contract

Split of responsibilities:
- `driver/d3d9/d3d9_swapchain.cpp` (`D3D9SwapChainEx`, a REWRITE keeping all
  D3D9 bookkeeping) — present params, backbuffer array, Reset/format logic,
  frame-latency counter, the present EmitCs lambda.
- `driver/mtl/mtl_presenter.h` (seam) → `backend/mtl_presenter_impl.cpp`
  (real impl): owns the CAMetalLayer (via `CreateMetalViewFromHWND`),
  drawable acquisition, layer props. The stub factory `createMtlPresenter`
  is still the seam default; the swapchain selects
  `createMtlPresenterImpl`/`backendCreatePresenter` once it has a HWND.
- `MtlContextImpl::synchronizeWsi` (backend) drives the actual Metal 4
  scan-out handshake.

DXVK's `DxvkSwapchainBlitter` is GONE — the present copy/blit is plain
context work inside the present lambda.

## Backbuffers and the MSAA resolve surface

- Backbuffers are ordinary `D3D9Surface` textures (the D3D9 rotation
  semantics live in the swapchain). The present model is BLIT/COPY, windowed
  AND fullscreen: backbuffer 0 → the freshly acquired drawable each frame.
- **MSAA swapchain** (D3D9 allows a multisampled backbuffer): a raw
  MSAA→drawable copy is invalid in Metal — it displayed as scrambled sample
  blocks (whole-screen tiled/rotated corruption; 90-history.md). On
  multisampled present params the swapchain creates `m_resolveBuffer`, a
  single-sample twin; `PresentImage` first `ctx->resolveTexture`s backbuffer
  0 into it (draw-less StoreActionMultisampleResolve pass) and everything
  downstream (dump, copy, blit) consumes the resolved image.

## The present lambda — everything on the CS thread

`PresentImage` emits ONE CS closure (d3d9_swapchain.cpp ~697) that does the
ENTIRE present: acquire → copy/blit → synchronizeWsi → present → latency
signal. **Drawable acquire happens ON THE CS THREAD, late** (dxmt does the
same): acquiring on the producer thread and releasing on the CS thread lets
more than maximumDrawableCount(3) drawables be in flight and wedges
`nextDrawable` after exactly 3 frames. One thread owns the whole
acquire/present/release cycle.

Inside the lambda:
1. MSAA resolve (if `m_resolveBuffer`).
2. `presenter->acquireNextImage(frameId)` → `MetalLayer_nextDrawable`.
   Invalid (occluded/no drawable) still advances the frame fences —
   `signalFrameLatency` + the CS signal — or the limiter stalls. Acquire is
   timed; >500 ms logs a warning (nextDrawable has no timeout — a silent
   starvation otherwise). The acquired texture handle is BORROWED from the
   drawable; the impl wraps it (+1 retained full-image view) and records the
   ACTUAL drawable dimensions — CoreAnimation may size drawables to the
   window, and an oversized copy is an OOB GPU fault.
3. Copy backbuffer → drawable:
   - sizes equal → 1:1 native texel `copyTexture` (compute encoder).
   - sizes differ → `presentUpscale` (MetalFX spatial, only when
     `D9MT_METALFX=1`; render low, present near-native) else the scaled
     meta-blit `blitTextureView` fills the drawable whatever its size.
   - no drawable view (stub presenter) → clamped copy.
4. `ctx->synchronizeWsi(sync)` — the **Metal 4 present handshake**
   (mtl_context_record.cpp:337): `MTL4CommandQueue_waitForDrawable` BEFORE
   commit → `submit()` (commit) → `MTL4CommandQueue_signalDrawable` →
   `MetalDrawable_present`. All four steps, in that order.
   **Stale comment warning:** the swapchain comment near the call still says
   "no pre-commit waitForDrawable" — wrong; the wait was restored (b610c22,
   fixed tearing/jitter) after the drawable-lifecycle fixes removed the
   forever-block fear.
5. `presenter->present()` (frame tracking only) + `signalFrameLatency` —
   contract 3e, deadlock-class if ever stubbed: the swapchain frame limiter
   waits on this monotonic counter.

## Layer geometry ownership — the iron rule

**The window system (winemac driver) owns CAMetalLayer geometry.**
`MtlPresenterImpl::applyProps` (mtl_presenter_impl.cpp ~155):

- `contentsScale` is **NEVER written** by the driver (props field 0.0 =
  leave untouched; the bridge `_MetalLayer_setProps` skips zero geometry
  fields). History (a7ae922): the presenter wrote contentsScale=1.0 + a
  pinned drawableSize on every applyProps, racing the winemac view layout
  which recomputes both from view bounds and backing scale — last writer
  won. After minimize/restore (cmd-tab) the stale values stranded the layer
  with a pixel-sized drawable composited in POINTS: top-left quarter of the
  image magnified across the window — or an oversized drawable (4× present
  cost).
- `drawableSize` IS written, but **only at construction and `recreate()`**
  (i.e. swapchain create and D3D9 Reset): a display-mode Reset must re-pin
  drawables to the new backbuffer size or the layer keeps serving
  stale-sized drawables (acc2270). NO per-frame re-pinning — the old
  acquire-time drift loop is what fought the layout. Between recreates the
  scaled present blit absorbs whatever size the layout imposes.
- `framebufferOnly = NO`, always: the present path copies INTO the drawable
  with a compute encoder (MTL4 has no blit encoder) and Metal rejects copies
  into framebufferOnly drawables.
- `displaySyncEnabled` = the vsync flag from `MtlPresenterDesc`.
- The layer's own residency set is attached at queue level
  (`attachLayerResidency`, retried each acquire until the async layer-device
  init completes) — drawables never enter the driver residency set
  (50-resources.md rule 4).
- `MetalLayer_setProps` applies asynchronously on the main thread (value
  copy + retain) — NEVER dispatch_sync; see the WM_DISPLAYCHANGE deadlock
  (40-bridge.md).

## Mode change / Reset flow

D3D9 `Reset` (device lock held, app thread):
`D3D9SwapChainEx` recreates backbuffers (+ resolve buffer if MSAA) →
`presenter->recreate(desc)` re-applies props (new drawableSize) → CS-side
the next present acquires from the resized layer. `D9MT-RESET` step logs
trace the path. Hazards encoded in history:
- No pass may straddle backbuffer destruction (why `D9MT_RTSKIP`
  pass-keep-open is default OFF and merge happens at deferred flush time).
- No bridge call on this path may sync-hop to the wine main thread
  (WM_DISPLAYCHANGE deadlock).
- Fullscreen behavior needs `CX_GRAPHICS_BACKEND = "dxmt"` in the bottle
  conf, or GTA IV fullscreen traps in a borderless window.

## Frame pacing

`SyncFrameLatency()` (swapchain) waits on the presenter's
`signalFrameLatency` counter to cap frames in flight (imageCount=3 default,
triple buffer). The occluded path and every early-out must still signal.
GPU-side pacing appears in PROF as high `beginRecording` (allocator-slot
serial wait).

## To change X, edit Y

- Present copy/scale policy → the present lambda, d3d9_swapchain.cpp ~740.
- Layer configuration → `applyProps` (presenter) + `_MetalLayer_setProps`
  (bridge); respect the zero-means-untouched geometry contract.
- Handshake order → `MtlContextImpl::synchronizeWsi` — do not reorder the
  four steps.
- Swapchain sizing/Reset → `d3d9_swapchain.cpp` (CreateBackBuffers /
  Reset paths) + `MtlPresenterDesc`.
