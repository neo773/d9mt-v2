# Windowing / resolution / present audit — Metal seam vs what DXVK expects

Audit date: 2026-07-27. Scope: window sizing, display mode, fullscreen, focus
loss, present/vsync. Question asked: *is the D3D9 windowing logic wrong, or is
our Metal seam failing the contract it expects?*

## Headline

**The D3D9/WSI layer is not the problem — the Metal seam is.**

`driver/wsi/win32/` is stock DXVK:

| file | diff vs `DXVK-3.0-uptsream/src/wsi/win32/` |
|---|---|
| `wsi_monitor_win32.cpp` | **0 lines** (byte-identical) |
| `wsi_platform_win32.cpp` | 4 lines |
| `wsi_platform_win32.h` | 6 lines |
| `wsi_window_win32.cpp` | 21 lines (incl. the 2026-07-27 fixes below) |

So the Win32 windowing logic is DXVK's, with its accumulated D3D9 edge cases.
Every bug found so far sits either (a) where DXVK assumes Windows/X11 window-
manager behaviour that winemac does not provide, or (b) in our own Metal
present seam, where D3D9 state is computed correctly and then dropped.

## Confirmed defects

### 1. `desc.vsync` is hardcoded `true` — D3DPRESENT_INTERVAL is ignored

`d3d9_swapchain.cpp:825` (CreatePresenter) and `:1229`
(UpdatePresentParameters) both set:

```cpp
desc.vsync = true;
```

Meanwhile the D3D9 layer computes the interval correctly at `:213-224`:
`D3DPRESENT_INTERVAL_IMMEDIATE` / `D3DPRESENT_FORCEIMMEDIATE` -> `0`, the
`d3d9.presentInterval` config override is honoured, and the result is passed to
`PresentImage(presentInterval)`.

That value never reaches the presenter. `applyProps()` sets
`props.display_sync_enabled = m_desc.vsync`, i.e. **always on**. A game asking
for `D3DPRESENT_INTERVAL_IMMEDIATE` still gets a vsynced `CAMetalLayer`.

Consequence: uncapped/benchmark modes are impossible, and any frame-rate
reasoning that assumes vsync-off is invalid. Note this is separate from the
frame limiter, which is *also* unimplemented (`d3d9_swapchain.cpp:975-978`:
`m_targetFrameRate` only feeds GetRasterStatus math).

**Fix:** thread `presentInterval` into `MtlPresenterDesc::vsync`, and make
`UpdatePresentParameters()` recreate on a vsync change, not just an extent
change (it currently compares extent only).

### 2. `isOccluded()` is dead code — occluded frames are still rendered

`wsi::isOccluded()` exists (`wsi_platform.cpp:136`, implemented
`wsi_window_win32.cpp:303`) and has **zero callers** in the D3D9 code.

Consequence: minimized/occluded, the game keeps rendering and presenting at
full tilt. Measured 2026-07-27 with SA2 minimized: **74.8% CPU**, log filling
with black `D3D9::Clear` + `CLR` lines.

D3D9 expects presents on an occluded swapchain to be cheap/skipped
(`S_PRESENT_OCCLUDED`), and combined with defect 3 there is no path that
throttles or stops the loop.

**Fix:** consult the D3D9 `Present` return-code spec, then check occlusion in
the present path and skip/throttle accordingly.

### 3. Focus loss does nothing for most games (`deviceLossOnFocusLoss`)

`NotifyWindowActivated()` (`d3d9_device.cpp:9077`) early-outs unless
`m_d3d9Options.deviceLossOnFocusLoss` is set. That option defaults to `false`
(`d3d9_options.cpp:77`) and is only `True` for specific titles in
`config.cpp`. SA2 is not one of them.

So for SA2 there is no lost/reset cycle at all on focus loss: no `Device lost`,
no `Device not reset`, nothing logged — which is exactly what the hang looked
like from the outside.

Whether SA2 *should* get device loss is a spec question (non-Ex D3D9 devices do
lose the device on fullscreen focus loss on real Windows). Needs checking
against Microsoft's docs before changing the default.

### 4. Present absorbs size mismatch instead of honouring the contract

`mtl_presenter_impl.cpp:86-108`: each acquire reads the drawable's ACTUAL
width/height and clamps to it, and the present path uses a **scaling blit**
(`blitTextureView`) when backbuffer != drawable size, because "CoreAnim window
layout can re-size drawables to the window behind our pinned drawableSize".

This makes mismatches invisible rather than resolving them: the swapchain
believes it is presenting at the backbuffer size while the layer silently
rescales. DXVK's model is that a size change triggers swapchain recreation, not
per-frame rescaling.

`recreate()` is reached only via `UpdatePresentParameters()` from two sites
(`:240`, `:527`), gated purely on extent difference.

### 5. Dead stub backend still in tree

`driver/mtl/mtl_presenter_stub.cpp`, `mtl_context_stub.cpp`,
`mtl_device_stub.cpp` are PHASE-D "log-and-return" stubs. The real backend is
`driver/backend/mtl_presenter_impl.cpp` etc. The stubs are misleading when
reading the seam and should be deleted or clearly quarantined.

## Fixed 2026-07-27 (same class: macOS != Windows WM)

- **Fullscreen window sized before the async mode switch landed.**
  `wsi::enterFullscreenMode` sizes from `getDesktopCoordinates()`; under
  wine/macOS `ChangeDisplayMode` is asynchronous, so that returned the
  pre-switch desktop -> window stranded at the old size in a corner. The
  re-snap (`wsi::updateFullscreenWindow`) existed but was only wired to the
  `changeFullscreen == false` branch. Added to the transition path
  (`d3d9_swapchain.cpp`, marker `D9MT-FS:`).
- **`SW_MINIMIZE` with no `SW_RESTORE`.** `d3d9_window.cpp` minimizes on
  deactivate; on activate it only called `SetWindowPos`, which does not
  un-minimize an iconic window. Added `if (IsIconic(window)) ShowWindow(window,
  SW_RESTORE);`.

## Prior art in the code (same symptom, already burned once)

`mtl_presenter_impl.cpp:158-170` documents two earlier regressions worth not
repeating:

- Writing `contentsScale` from the presenter "stranded a stale 1.0 after
  minimize/restore: pixel-sized drawable composited in POINTS = top-left
  quarter magnified" — i.e. the *same* corner-window symptom, via backing
  scale. `contents_scale = 0.0` now leaves it to winemac.
- Per-frame `drawableSize` re-pinning ("the old acquire-time drift loop")
  "fought the layout and broke minimize/restore".

Both say the same thing: **points vs pixels and who owns layer geometry is the
unstable seam.** This bottle runs `RetinaMode=Y`, so backing scale is 2.

## Recommended order

1. Thread `presentInterval` -> `desc.vsync` (defect 1). Small, unblocks all
   frame-rate work.
2. Wire occlusion into the present path (defect 2). Stops the CPU burn.
3. Write down the ownership rule for layer geometry — who sets `drawableSize`,
   who sets `contentsScale`, in points or pixels, and when — then make the
   presenter obey it and delete the compensating blit (defect 4).
4. Settle device-loss-on-focus-loss against the D3D9 spec (defect 3).
5. Delete the stub backend (defect 5).

## Still to verify against primary sources

Not yet checked against Microsoft/Apple documentation — do this before
changing behaviour, rather than inferring from DXVK:

- `IDirect3DDevice9::Present` return codes: `S_PRESENT_OCCLUDED`,
  `S_PRESENT_MODE_CHANGED`, and when `D3DERR_DEVICELOST` is required.
- Exact focus-loss/device-loss rules for non-Ex vs Ex devices, fullscreen vs
  windowed.
- `CAMetalLayer` contract: `drawableSize` vs `bounds` vs `contentsScale`, who
  is allowed to write each, and behaviour across minimize/restore and display
  mode changes.
