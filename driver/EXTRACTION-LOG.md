# EXTRACTION-LOG — DXVK 3.0 D3D9 state tracker → Metal-first driver (Phase A+B)

Every deviation from the three rewrite specs (docs 01/02/03). Source tree
(read-only): `v2/DXVK-3.0-uptsream/src/`. Target: `v2/driver/`.

## Global decisions

- **Namespace `dxvk`, not `d9mt`.** The doc 01 §4 sketch used `namespace d9mt` for
  `MtlContext`. The DXVK d3d9 frontend is `namespace dxvk`, and the whole extraction
  thesis is "frontend compiles unchanged when `DxvkContext` → `MtlContext`". Putting
  the `Mtl*` seam types in `namespace dxvk` makes that rename a pure token swap (no
  namespace qualification) and lets the CS lift reference `MtlContext` directly.
  Applies to all of `mtl/`, and the retypes in `cs/` and `d3d9/`.
- **`MtlFormat` defined standalone** (mtl_types.h) mirroring winemetal
  `WMTPixelFormat` numeric values, rather than `#include`-ing the 57 KB
  `third_party/winemetal/winemetal.h`. Keeps the driver compile gate independent of
  the winemetal ABI at this phase (doc 02 sanctioned "define our own enum" fallback).
  A later `using MtlFormat = WMTPixelFormat;` swap is value-compatible.

## cs/ (dxvk_cs.{h,cpp}) — doc 01 §2 lift

- Retargeted the two includes (`dxvk_device.h` / `dxvk_context.h`) to
  `../mtl/mtl_context.h`; added `../util/util_math.h` (align, CACHE_LINE_SIZE) and
  `../util/util_flags.h` (Flags<>), which previously arrived transitively via
  `dxvk_include.h`.
- `DxvkContext` → `MtlContext`, `DxvkDevice` → `MtlDevice` (tokens only). The
  `DxvkCs*` class names, chunk size, dual-queue, pushCmd mutable-block, and
  sequence-number sync are byte-for-byte preserved.
- **Stripped the per-chunk stat counters** (`m_device/m_context->addStatCtr(
  DxvkStatCounter::Cs*)` at 4 sites). Pure profiling, not part of the deferred model;
  reintroduce against MtlDevice/MtlContext profiling in Phase C. Marked `// PHASE-C:`.

## wsi/ — doc 02 §3 KEEP (monitor/window only)

- Removed the Vulkan WSI **surface-creation path** (swapchain is REWRITE): the
  `../vulkan/vulkan_loader.h` include + `createSurface(...)` decl/impl in
  `wsi_window.h`, `wsi_platform.{h,cpp}`, `win32/wsi_platform_win32.h`,
  `win32/wsi_window_win32.cpp`. Marked `// PHASE-C:`.
- `Win32WsiDriver::getInstanceExtensions()` returned `VK_KHR_*surface` extension
  names → now returns `{ }` (Metal present needs none).
- Added `#include <string>` to `wsi_platform.h` (`WsiBootstrap::name`; was transitive).
- Deleted `util/util_version.h` (+ .cpp) — it `#include`s `../vulkan/vulkan_loader.h`
  and is unused by d3d9/wsi.

## mtl/ (new — Phase B foundation)

- `mtl_types.h` (427), `mtl_resource.h` (166), `mtl_context.h` (208),
  `mtl_context_stub.cpp` (124), `mtl_format.h`/`.cpp` (32/17). Authored to doc 01 §4
  sketch + doc 02 §2/§4 replacement map. `MtlContext` interface amended by the
  doc 01 §6 open-question resolutions:
  - §6.2 **`setLogicOpState` CUT** (absent from the surface).
  - §6.3 `setDepthBounds` accepted + no-op.
  - §6.4 vertex stride documented as a render-PSO key participant.
  - §6.1 `setViewports` documented as RT-pixel coords only.
  - `setDepthBiasRepresentation` omitted (doc 01 §1c "drop/no-op on Metal").
- `MtlFormatInfo` lookup (`mtl_format.cpp`) returns nullptr — PHASE-C table.

## d3d9/ — KEEP copies + retypes (Phase A.3) and format ADAPT (Phase B.7)

- **d3d9_state.h**: 4 `VkShaderStageFlags` push-data constants → `MtlStageMask`
  (`MtlStageVertex`/`MtlStageFragment`); `setAlphaCompareOp(VkCompareOp)` →
  `setAlphaCompareOp(MtlCompareFunction)`; added `#include "../mtl/mtl_types.h"`. Now
  0 Vk. Everything else byte-for-byte.
- **d3d9_options.h**: dropped `#include "../dxvk/dxvk_device.h"`; ctor param
  `Rc<DxvkDevice>` → forward-declared `Rc<MtlDevice>` (doc 02 explicitly flags this
  drop); added `util_likely.h` + `util_rc_ptr.h`. `d3d9_options.cpp` body remains an
  ADAPT TU (device-caps read) — not compiled this phase. Marked `// PHASE-C:`.
- **d3d9_format.{h,cpp}**: `D3D9Format` enum + all `Is*Format` predicates KEEP;
  `D3D9_VK_FORMAT_MAPPING` → `D3D9_MTL_FORMAT_MAPPING` (MtlFormat×2 union /
  MtlAspectMask / MtlSwizzleChannels, `IsValid()` → `!= MtlFormat::Invalid`);
  `D3D9VkFormatTable` → `D3D9MtlFormatTable` (ctor drops `Rc<DxvkAdapter>`; caps
  probing stubbed permissive PHASE-C). `ConvertFormatUnfixed` switch retyped: standard
  color/depth/DXT rows mapped; **9 rows left `MtlFormat::Invalid`** (A4L4, UYVY, YUY2,
  R8G8_B8G8, G8R8_G8B8, ATI1/BC4, ATI2/BC5, NV12, YV12) with `// PHASE-C:` notes — the
  current `MtlFormat` subset lacks those. Compiles clean.
- **d3d9_constant_buffer.h** copied as a transitive dep of `d3d9_state.h` (via
  `d3d9_constant_set.h`); it is an ADAPT file still holding `VkDeviceSize` /
  `Rc<DxvkBuffer>` etc. Whitelisted in the Vulkan gate + `// PHASE-C:` banner.
- **d3d9_names.cpp** left pristine; its `ENUM_NAME`/`ENUM_DEFAULT` (util_enum.h) came
  transitively via the old format→dxvk include chain now severed. Trivial Phase-C
  include fix — kept verbatim rather than editing a KEEP file.

## build-driver.sh — CORE compile set + non-fatal exclusions

Excluded from the CORE (must-compile) set because they pull not-yet-ported deps:
`util/util_unmap.cpp` (`../dxvk/dxvk_allocator.h`), `util/com/com_guid.cpp`
(`../../d3d11` + `../../dxgi`, per build-dxvk.sh precedent),
`util/com/com_destruction_notifier.cpp` (broken `../util/thread.h` path in this
layout), `wsi/wsi_edid.cpp` (`libdisplay-info/info.h`). d3d9 KEEP `.cpp` are attempted
in a non-fatal D3D9-LEAF pass (all but format need un-ported ADAPT headers).

## Vulkan-gate whitelist (`build-driver.vulkan-whitelist`)

- `d3d9_constant_buffer.h` — ADAPT transitive dep (above). The `winevulkan.dll`
  module-presence string in `util_env.cpp` is handled by an inline gate rule. All
  other remaining `Vk`/`vulkan` tokens are in comments/doc lines (allowed).

## Phase C — HALTED before any file written (position marker)

Phase C (port the full ADAPT set onto the MtlContext seam) was **started and then
halted by coordinator directive before any target file was created or modified**.
State at halt:

- **Files ported this phase: NONE.** `v2/driver/` is byte-identical to the Phase A+B
  deliverable. The two porter agents launched (core/seam layer; d3d9_device.{h,cpp})
  were stopped before writing to disk — `git status` shows 0 changes under
  `v2/driver/` and the target `d3d9/`+`mtl/` file listings are unchanged.
- **Build gate: PASS** (unchanged) — 23 CORE TUs compile, `libdriver_core.a`
  archives (~9.06 MB), Vulkan gate PASS. `d3d9_format.cpp` leaf compiles;
  `d3d9_names.cpp` still fails the leaf pass on the known severed `util_enum.h`
  include (Phase-C fixup item, see the d3d9_names.cpp note above).
- **Worktree note (not caused by this phase):** `v2/driver/` is entirely *untracked*
  and the old `v2/d3d9`, `v2/d9mt`, `v2/dxvk` trees are pre-existing tracked
  deletions from the v2 rewrite restructuring — present in the worktree before this
  session, unrelated to the extraction work.

### Remaining Phase C work (not started)
1. Retype-only ADAPT: d3d9_util, d3d9_common_texture/buffer, d3d9_adapter,
   d3d9_subresource/surface/volume/texture/buffer/query, d3d9_swvp_emu,
   d3d9_shader_analysis, d3d9_constant_copy, d3d9_format_helpers; fix d3d9_names.cpp
   util_enum.h include.
2. Small EmitCs users: d3d9_initializer (4), d3d9_constant_buffer.cpp (4),
   d3d9_annotation (3).
3. d3d9_device.{h,cpp} — 68 EmitCs sites (the bulk).
4. d3d9_swapchain (7 sites) + new thin MtlPresenter interface + stub.
5. PHASE-D compile-clean stubs: d3d9_shader, d3d9_fixed_function, d3d9_interop/
   d3d9_interfaces, d3d9_main/dll exports. DROP: hud, on_12.

Seam extensions that the port will require (canonical mapping identified, not yet
added to mtl/ headers): MtlAdapter/MtlInstance, MtlTexelBufferView(present),
MtlBufferViewKey, MtlTextureCreateInfo, MtlBufferCreateInfo, MtlTextureUsageInfo,
MtlFormatInfo metadata (mtl_format.h), MtlFormatFlags, MtlInputAssemblyState,
MtlGpuVendor, MtlQueryStatus/MtlEventStatus, MtlPushDataBlock, MtlSharedHandleMode,
MtlAccess, MtlResourceUsage, MtlTextureViewType/MtlTextureType.
