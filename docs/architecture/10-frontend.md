# 10 — Frontend: the DXVK-derived D3D9 state tracker

`driver/d3d9/` + `driver/cs/` are DXVK 3.0's D3D9 frontend, extracted and
retargeted from `DxvkContext`/Vulkan onto the `MtlContext` seam
(`driver/mtl/mtl_context.h`). The extraction thesis: **the frontend compiles
unchanged when `DxvkContext` → `MtlContext`** — same namespace `dxvk`, same
class shapes, token-level retypes only. Every deviation is logged in
`driver/EXTRACTION-LOG.md`; `scripts/audit-port-diff.sh` gates that ported
files stay diffable against `DXVK-3.0-uptsream/src/` (read-only reference
tree, gitignored).

## What was kept vs ripped

**KEPT (byte-for-byte where possible):** the whole D3D9 API surface —
`d3d9_device.cpp` (~9k lines, ~68 EmitCs sites), resource pools and Lock/Unlock
(`d3d9_common_texture/buffer`), state blocks, fixed-function state keys,
queries, multithread guard, format tables (retyped `D3D9_VK_FORMAT_MAPPING` →
`D3D9_MTL_FORMAT_MAPPING`), the CS machinery (`cs/dxvk_cs.{h,cpp}`: chunk
size, dual-queue, pushCmd, sequence-number sync all preserved), the DXVK
allocator (`dxvk/dxvk_allocator.*`), util.

**RIPPED:** everything Vulkan. The build enforces it: `build-driver.sh` greps
the objects for Vulkan symbols against `scripts/build-driver.vulkan-whitelist`
and fails the build on a hit (the "Vulkan gate"). Also gone: DXVK's
`DxvkSwapchainBlitter` (present blit lives in the presenter/backend), the HUD,
`util_version.h`, the WSI Vulkan-surface path, per-chunk CS stat counters.

## The EmitCs / CS-thread model

Identical to DXVK. App threads never touch Metal:

1. A `D3D9DeviceEx` method validates, updates CPU-side state, then
   `EmitCs([captures](MtlContext* ctx) { ctx->...; })` appends a closure to the
   current `DxvkCsChunk`.
2. `FlushCsChunk()` queues the chunk; the single **`dxvk-cs` thread** replays
   closures in order against the seam — which is `MtlWireContext` (echo
   wrapper, see 70-render-server.md) wrapping `MtlContextImpl`.
3. Synchronization back to app threads is by CS sequence numbers and
   submission serials (`WaitForResource` compares serials — never a stub spin;
   see 50-resources.md).

**Invariant:** `MtlContextImpl` is single-threaded — only the CS thread calls
it. Resource *creation* runs on app threads via `MtlDevice` (the factory half
of the seam, `driver/mtl/mtl_device.h` / `backend/mtl_device_impl.*`), which is
why the residency tracker takes a mutex (see 50-resources.md).

The CS thread is also the frame ceiling: all D3D9→Metal translation is x86
code under Rosetta on this one thread (~38% of process time; other threads
~45% blocked on it). That measurement is what drives the render-server track.

## d3d9/ file map (one-liners; the interesting ones)

| File | Purpose |
|---|---|
| `d3d9_device.{h,cpp}` | `D3D9DeviceEx` — the API core: draws, SetRenderTarget, Lock/Unlock orchestration, staging, EmitCs sites. |
| `d3d9_swapchain.{h,cpp}` | `D3D9SwapChainEx` REWRITE — present lambda, MSAA resolve surface, Reset. See 60-present.md. |
| `d3d9_common_texture.{h,cpp}` / `d3d9_common_buffer.{h,cpp}` | Pool semantics (DEFAULT/MANAGED/SYSTEMMEM), mapping modes, backing storage, rename/Discard. |
| `d3d9_buffer.*`, `d3d9_surface.*` (via texture), `d3d9_texture.*`, `d3d9_volume*` | COM resource wrappers. |
| `d3d9_shader.{h,cpp}` | `ID3D9MetalShaderBackend` seam: `getMetallib(linkageCookie, specStateHash)`, masks, stage-relative sampler slot mapping (`getTextureSlotInfo`). |
| `d3d9_shader_compiler.{h,cpp}` | SM3 DXSO → dxbc-spirv IR → d9mtair bridge compile; reflection parsing → `cbvSlotMap`/`abWords`; VS↔PS interpolant reconciliation (`resolveMismatchedIo`/`resolveSemanticIo` — the historical black-screen class). |
| `d3d9_shader_analysis.*`, `d3d9_shader_validator.*` | SM3 bytecode analysis (constant layout, masks, signatures). |
| `d3d9_fixed_function.{h,cpp}` + `d3d9_ff_metallib.h` + `shaders/` | FF pipeline: state keys + the generated ubershader metallibs (ffVsMain/ffFsMain). See 20-shaders.md. |
| `d3d9_constant_set.h`, `d3d9_constant_buffer.*`, `d3d9_constant_copy.*` | Shader-constant upload (`CbvIndex` namespace: VSClipPlanes=0 … VSStaticConstants=3 … PSShared=5, SpecData=7). |
| `d3d9_stateblock.*`, `d3d9_state.*` | State capture/apply; push-data stage masks retyped to `MtlStageMask`. |
| `d3d9_initializer.*` | New-resource init; GPU-only textures zero-init (WDDM parity — uninitialized garbage broke GTA's composite). |
| `d3d9_format.*`, `d3d9_util.*` | Format mapping + Vk→Mtl decode helpers (`DecodeAddressMode` rewritten — Metal enum values differ). |
| `d3d9_adapter.*`, `d3d9_interface.*`, `d3d9_monitor.*`, `d3d9_caps.h` | Enumeration/caps. |
| `d3d9_query.*` | Occlusion/timestamp/EVENT queries → seam query objects (serial-based; decision 7). |
| `d3d9_multithread.*` | D3DCREATE_MULTITHREADED lock. |
| `d3d9_main.cpp`, `d3d9_bridge.*`, `d3d9_annotation.*`, `d3d9_names.*` | DLL entry/exports, debug. |

## Where D3D9 semantics live

- **Pools/Lock/Unlock:** `d3d9_common_texture/buffer` decide mapping mode and
  backing; `d3d9_device.cpp` implements Lock paths, `AllocStagingBuffer`
  (single linear `MtlStagingBuffer`, `reinit()` on exhaustion — see
  90-history.md VA saga), `FlushBuffer`/UpdateTexture upload via
  `ctx->copyBuffer`/`copyBufferToTexture`, and `WaitForResource`.
- **Discard rename:** app thread swaps the buffer's storage
  (`D3D9CommonBuffer` rename), then EmitCs `ctx->invalidateBuffer(buffer,
  oldSlice)` — the CS-time commit point that retargets still-bound slices in
  command order (bind slices are PHYSICAL snapshots; a rename never retargets
  already-recorded draws).
- **Reset/mode change:** `d3d9_swapchain.cpp` Reset → presenter `recreate()`
  (re-pins drawableSize) + backbuffer recreation; `D9MT-RESET` step logs trace
  it. The device lock is held — anything on this path must never
  `dispatch_sync` to the wine main thread (see 40-bridge.md deadlock).
- **SetRenderTarget hazard rule:** the DXVK feedback-loop
  `ensureTextureUsage` path is kept, but the historical "auto-unbind sampled
  RT" band-aid is DELETED; the root fix is backend-side (bindRenderTargets
  flushes folded clears — 90-history.md clear-drop saga). D3D9 sampler
  bindings persist across SetRenderTarget, by spec.

## wsi/

Win32-only monitor/window helpers (`wsi_monitor.h`, `wsi_window.h`,
`wsi_edid.*`, `win32/`). The Vulkan surface-creation path was removed —
swapchain presentation is a full REWRITE through `MtlPresenter`, not WSI.
`getInstanceExtensions()` returns `{}`.

## Known divergences from DXVK / D3D9 quirks handled elsewhere

- `setLogicOpState` — CUT from the seam (D3D9 has no logic ops).
- `setDepthBounds` — accepted + no-op.
- **Vertex stride is a PSO-key participant** (Metal bakes stride into the
  descriptor); the backend prefers the LIVE `SetStreamSource` stride over the
  declaration stride (stride-0 crash saga).
- **Triangle fans pass through the frontend** (Vulkan had them native); the
  backend emulates via a shared index-pattern buffer.
- Vertex texture samplers are **stage-relative** slots in `d3d9_shader.h`
  (flat D3D9 indices 17-20 once overflowed the 16-slot backend arrays).
- 9 exotic formats still `MtlFormat::Invalid` (A4L4, UYVY, YUY2, R8G8_B8G8,
  G8R8_G8B8, ATI1/BC4, ATI2/BC5, NV12, YV12) — see EXTRACTION-LOG.
- `DecodeFillMode`: D3DFILL_POINT → Lines (Metal has no point fill).
- Y-flip: the inherited Vulkan viewport Y-flip was dropped (d3986ae) —
  Metal's coordinate convention differs from Vulkan's.

## To change X, edit Y

- New D3D9 behavior/quirk → `d3d9_device.cpp` (find the EmitCs site), keep the
  DXVK shape so `audit-port-diff.sh` stays reviewable.
- New seam capability → add the virtual to `driver/mtl/mtl_context.h`, a
  log-stub default in `mtl_context_stub.cpp`, the real override in
  `backend/mtl_context_impl.h` + record/transfer cpp, AND forward it in
  `backend/mtl_wire_context.{h,cpp}` (the always-installed wrapper).
- Format support → `d3d9_format.cpp` mapping row + `driver/mtl/mtl_format.*`
  + backend format table (`backend` uses `WMTPixelFormat` numerics).
- **Any .h edit → `rm -rf build/driver/obj` before rebuilding.**
