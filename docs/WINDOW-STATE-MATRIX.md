# Window state matrix — DXVK vs DXMT vs macOS/winemac

What each layer does per window state. "macOS" = what winemac/CoreAnimation
actually does underneath, measured on this machine (M1 Max, Retina, bottle
`RetinaMode=Y`, contentsScale 2, panel 3456x2234 px = 1728x1117 pt, ProMotion).

Legend: ✅ works as intended · ⚠️ works but with a caveat · ❌ broken on macOS

---

## 1. Fullscreen entry

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Mechanism | Real display-mode switch: `ChangeDisplaySettingsEx` (`CDS_FULLSCREEN`) then size window to desktop rect | **Borderless only.** `enterFullscreenWindow` restyles to `WS_POPUP`, sizes to monitor rect. *"the display mode is never touched"* (`dxmt/src/d3d9/d3d9_device.cpp:2110`) | ❌ Mode switch is **asynchronous**. `getDesktopCoordinates()` right after it returns the PRE-switch desktop → window sized to the old rect → stranded top-left |
| Window size source | `getDesktopCoordinates()` (desktop rect) | Monitor rect + backbuffer extent | Monitor rect is the only safe source; backbuffer may be smaller than the mode |
| Z-order | `HWND_TOPMOST` | `HWND_TOPMOST` | ⚠️ Topmost makes **cmd-tab impossible**. Must use `HWND_TOP` |
| Refresh rate | Comes free with the mode switch | Not controlled here | ❌ Borderless leaves panel at native rate (120Hz ProMotion). Frame-rate-coupled games (SA2) run double speed |

**Ours:** DXMT's model — borderless, `HWND_TOP`, `ChangeDisplayMode` has zero callers.

---

## 2. Fullscreen exit

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Action | `RestoreDisplayMode` + `wsi::leaveFullscreenMode` + restore saved rect/style | Restore saved style; preserve live `WS_VISIBLE` / `WS_EX_TOPMOST` bits (wined3d behaviour) | No mode to restore if none was set. Clobbering visibility bits can hide a window the app is driving |

---

## 3. Windowed

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Geometry | App owns it; DXVK follows via wndproc | App owns it; **re-probed every Present** via `GetClientRect` (`dxmt/src/d3d9/d3d9_swapchain.cpp:511-529`) | ✅ NSView resizes **asynchronously**, so anything latched once goes stale. Per-Present re-probe is the only self-healing option |
| Layer size | n/a (Vulkan swapchain recreate) | `drawableSize` = client size | ✅ wine's client rect is already in **backing pixels** — do NOT multiply by `contentsScale` (that doubles it) |

---

## 4. Focus loss (alt-tab / cmd-tab / click away)

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Window action | `WM_ACTIVATEAPP` deactivate → `ShowWindow(SW_MINIMIZE)` | **Never calls ShowWindow at all** (no such call anywhere in its tree) | ❌ DXVK minimizes, and its activate path only does `SetWindowPos`, which does **not** un-minimize. No macOS WM undoes it → permanently minimized, render loop still burning ~75% CPU on black frames |
| Activate action | Resize window to **`BackBufferWidth/Height`** at desktop origin | Nothing | ❌ If backbuffer < desktop, the first mouse click shrinks fullscreen to the backbuffer at (0,0) — the top-left-corner bug |
| Device state | Optional `deviceLossOnFocusLoss` (default **false**; per-game True list) | d3d11: occlusion only. d3d9 fork: returns occluded/`D3D_OK`, *refuses* to synthesize DEVICELOST | With the default false, `NotifyWindowActivated` early-outs → nothing logged, nothing recovers |

**Ours:** no wndproc hook, so neither branch exists.

---

## 5. Minimized

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Present | Keeps presenting | **Skips present entirely**; `nextDrawable` never called | ❌ Presenting while minimized leaves a second surface composited under the window (the "duplicate black screen") |
| Detection | `wsi::isMinimized` = `IsIconic` | `IsIconic` | ✅ reliable |

---

## 6. Occluded (other Space, covered by another window)

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Detection | `wsi::isOccluded()` exists — **zero callers**, dead code | d3d11 returns `DXGI_STATUS_OCCLUDED` and skips present | Not the same as minimized: a Space switch does **not** set `IsIconic` |
| Present | Keeps presenting | Skipped | ⚠️ CoreAnimation throttles drawable vending while off-screen; continuing to present builds a backlog that keeps costing frames after returning (suspected cause of the post-Space-switch slideshow — **not yet verified**) |

---

## 7. Reset / resolution change

| | DXVK | DXMT | macOS / winemac |
|---|---|---|---|
| Path | `changeFullscreen ? EnterFullscreenMode : ChangeDisplayMode + updateFullscreenWindow` | `ResizeBuffers` → `ApplyLayerProps` | The `changeFullscreen == true` branch never re-snapped the window — that fix-up existed only on the other branch |
| Layer | swapchain recreate | `drawableSize` re-applied on resize only | Per-Present re-probe makes this mostly moot |

---

## 8. Layer geometry ownership (the recurring trap)

| Property | Owner | Unit | Notes |
|---|---|---|---|
| `layer.bounds` | CoreAnimation (driven by the wine window) | **points** | measured 1496x967 for a 2992x1934 px client |
| `layer.contentsScale` | winemac (backing scale) | ratio | 2.0 under `RetinaMode=Y`. DXMT *re-asserts* the live value; we leave it (bridge treats 0 as "don't touch" — **fork-specific**, upstream dxmt assigns unconditionally) |
| `layer.drawableSize` | us | **pixels** | = `bounds x contentsScale` = wine `GetClientRect` directly |
| present blit dst | live drawable texture size | pixels | Must read `MTLTexture_width/height` of the acquired drawable, not the cached/backbuffer extent |

---

## 9. Frame pacing

| | DXVK | DXMT | macOS |
|---|---|---|---|
| Mechanism | `Presenter::setFrameRateLimit`, `setSyncInterval` | `presentDrawableAfterMinimumDuration(drawable, after)` where `after = SyncInterval / refresh` | ✅ Display-pipeline pacing; lets ProMotion settle. A CPU `sleep_until` limiter blocks the CS thread and fights vsync |
| Guard | — | — | ⚠️ Sanity-check the target: an implausible refresh yields a ~1s hold per frame → `nextDrawable` starvation → looks like a hang |
