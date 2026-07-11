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

## Phase C — RESUMED from checkpoint (2026-07-11, solo/sequential)

Resumed per coordinator directive; worked entirely alone (no sub-agents). Ported the
tractable, genuinely-compiling slice of the remaining ADAPT set + authored the
Phase-D neutral stubs + the MtlPresenter seam, and wired them into the build as a
**fatal** set. From-scratch `bash scripts/build-driver.sh` is GREEN: **29 CORE TUs
compile**, `libdriver_core.a` archives (~9.58 MB), **Vulkan gate PASS**.

### Files done this resume (per-file)

| File | Lines | Kind | Notes / sites |
|---|---|---|---|
| d3d9/d3d9_util.h | ~360 | ADAPT retype | Dropped `../dxvk/dxvk_device.h`; all Decode* return types Vk*→Mtl*. `DecodeInputAssemblyState`→`MtlInputAssemblyState`; `Decode{BlendFactor,BlendOp,CompareOp,StencilOp,CullMode}`→Mtl enums; `DecodeFilter/MipFilter`→`MtlSamplerFilter`/`MtlSamplerMipFilter`; `GetImageUsageFlags`/`GetImageFormatFeatures`→`MtlTextureUsage`; `GetPackedDepthStencilFormat`/`PickSRGB`/`GetDepthBufferRValue`→`MtlFormat`; `GetShaderStage`→`MtlStageMask`; `ConvertBox/Rect`→`MtlOrigin/MtlSize/MtlOffset2D/MtlExtent2D`; `IsBlitRegionInvalid`→`MtlOffset3D[2]/MtlSize`. Compiles. |
| d3d9/d3d9_util.cpp | ~430 | ADAPT retype | Switch bodies byte-identical. **`DecodeAddressMode`** rewritten from the Vk-value LUT to an explicit switch (Mtl address-mode values differ; Repeat≠0). **`DecodeFillMode`**: D3DFILL_POINT→Lines (no Metal point-fill). **`DecodeDecltype`**: FLOAT3 + the 4 scaled types (UBYTE4/SHORT2/SHORT4/UDEC3/DEC3N) return `MtlFormat::Invalid` — MtlFormat is a *pixel* enum; PHASE-C needs a dedicated `MtlVertexFormat`. Compiles. |
| d3d9/d3d9_names.cpp | (KEEP) | include fix | Added `#include "../util/util_enum.h"` (ENUM_NAME/ENUM_DEFAULT — severed transitive include). Now compiles as a fatal TU. |
| d3d9/d3d9_format.cpp | (prev) | ADAPT | Promoted from non-fatal D3D9-LEAF to fatal CORE (already compiled at checkpoint). |
| d3d9/d3d9_shader.{h,cpp} | new | PHASE-D stub | Neutral `ID3D9MetalShaderBackend` (doc 03 §5): `stage()`, `masks()`, `getMetallib(linkageCookie, specStateHash)`. **No v2/air, no dxbc_spv/SPIR-V** crosses the boundary. `// PHASE-D:` markers throughout. |
| d3d9/d3d9_fixed_function.{h,cpp} | new | PHASE-D stub | Keys/enums KEPT (`D3D9FF_VertexBlendMode`, `D3D9TextureStageStateFlag(s)`, `TCIOffset/TCIMask`); `D3D9FFShaderModuleSet` holds two neutral `ID3D9MetalShaderBackend` (VS/FS), `GetShader<Stage>()` returns null (lookup stubbed pending build-time MSL uber-shaders). g_ffIsgn accessor deferred (needs shader-analysis/dxbc_spv). |
| mtl/mtl_presenter.h | new | seam (thin) | `MtlPresenter` interface + `MtlAcquiredDrawable`/`MtlPresenterDesc` + `createMtlPresenter()` factory. Methods: `recreate`, `acquireNextImage(frameId)`, `present`, `signalFrameLatency(frameId)` (contract 3e), `getImageExtent`. Retina/backing-scale documented as applied here (doc §6.1). NO CAMetalLayer/winemetal. |
| mtl/mtl_presenter_stub.cpp | new | PHASE-D stub | `MtlPresenterStub` log-and-return; `acquireNextImage` returns `valid=false`. |

### SEAM CHANGES (added to mtl/ this resume)

- **`MtlInputAssemblyState`** (mtl_types.h) — was `DxvkInputAssemblyState{topology,
  primitiveRestart}`; ctor `(MtlPrimitiveTopology, bool)`. Needed by
  `DecodeInputAssemblyState` (d3d9_util). Non-breaking addition.
- **`MtlOffset2D`** (mtl_types.h) — was `VkOffset2D`; needed by the 2D `ConvertRect`
  overload. Non-breaking addition.
- Both are the minimal seam additions the retype required (per the "make the minimal
  seam addition, log it" directive). No existing seam method signatures changed.

### build-driver.sh

- Added to the **fatal CORE** set: `mtl/mtl_presenter_stub.cpp`, and d3d9
  `d3d9_util.cpp d3d9_names.cpp d3d9_format.cpp d3d9_shader.cpp
  d3d9_fixed_function.cpp`. All archived into `libdriver_core.a`.
- Emptied the non-fatal `D3D9_LEAF` list (its two members are now fatal CORE).

### HONEST remaining Phase-C/D work (NOT done — full DoD not met)

The remaining ADAPT set is **one interconnected graph** that cannot reach a green
compile as isolated TUs, for two structural reasons beyond a "retype":

1. **It needs a large `MtlDevice` surface that does not exist.** `d3d9_device.cpp`
   (9.3k lines, 68 EmitCs sites) and `d3d9_common_texture/buffer/adapter/initializer/
   swapchain` call `m_dxvkDevice->createBuffer/createImage/allocateStorage/
   createSampler/importBuffer/...` — the **DxvkDevice** surface, *distinct from the
   MtlContext seam*. The checkpoint's `MtlDevice` is an empty class. Compiling these
   requires designing + adding a resource-factory surface to `MtlDevice`
   (MtlTextureCreateInfo / MtlBufferCreateInfo / allocateStorage ring / sampler
   cache), which is a design task the three docs do not fully specify. This is the
   true blocker, not token-retype effort.
2. **The analysis/shader/constant TUs pull `dxbc_spv` sm3 headers**
   (`third_party/dxbc-spirv/sm3/*`, `util/util_byte_stream.h`). `d3d9_shader_analysis`,
   `d3d9_constant_copy`, and the real `d3d9_shader` need `-Ithird_party/dxbc-spirv` on
   the driver build and careful firewalling so no SPIR-V leaks to the frontend.

Concretely still to port (order per the checkpoint work-plan): d3d9_common_texture,
d3d9_common_buffer, d3d9_subresource/surface/volume/texture/buffer, d3d9_adapter,
d3d9_query, d3d9_swvp_emu, d3d9_shader_analysis, d3d9_constant_copy,
d3d9_format_helpers, d3d9_cursor.cpp (present as KEEP header only); then
d3d9_initializer, d3d9_constant_buffer.cpp, d3d9_annotation; then **d3d9_device.{h,cpp}**
(68 sites) once the MtlDevice factory surface exists; then d3d9_swapchain onto the new
MtlPresenter; then the remaining PHASE-D: real d3d9_shader (air-backed),
d3d9_interface/d3d9_main (need the full device stack — NOT standalone-compilable), and
DROP d3d9_hud + d3d9_on_12 + d3d9_interop/d3d9_interfaces (Vk-interop — dropped, logged
here). `d3d9_main`/`d3d9_interface` were assessed and are **not** the "straightforward"
exports the plan hoped: `d3d9_main.cpp` instantiates `D3D9InterfaceEx`, which transitively
needs the entire device stack, so it can only compile after device.cpp.

**DoD status: PARTIAL.** From-scratch build is GREEN for the 29-TU CORE set (incl. the
5 newly-fatal d3d9 TUs + presenter), archive + Vulkan gate PASS — but the full
"ALL v2/driver TUs compile" bar is **not** met: the device/common-resource/adapter/
swapchain graph above remains, gated on the MtlDevice factory-surface design.

## Phase C-3 — dxbc-spirv firewall + link harness (2026-07-11, solo/sequential)

Resumed solo (no sub-agents). Landed the dxbc-spirv-gated analysis slice + a
from-scratch d3d9.dll link ATTEMPT. From-scratch `bash scripts/build-driver.sh` is
GREEN: **33 CORE TUs compile**, `libdriver_core.a` (~10.2 MB), **Vulkan gate PASS**.

### Environment fix (was the real Step-1 blocker)
- `subprojects/dxbc-spirv` was an **uninitialized submodule** (empty dir) — the sm3
  headers the task assumed present did not exist. `git submodule update --init` in
  `DXVK-3.0-uptsream/` populated it (commit 4c0c8f6). Added symlink
  `v2/third_party/dxbc-spirv` -> that subproject so the build's `-I third_party/dxbc-spirv`
  matches the task spec. The sm3/util headers are third-party `-I` includes, **exempt
  from the driver Vulkan gate** (gate scans `driver/` only) — gate still PASS.

### Files done this resume (per-file)
| File | Lines | Kind | Notes |
|---|---|---|---|
| d3d9/d3d9_constant_copy.{h,cpp} | 205/400 | ADAPT (0 Vk) | Backend-neutral constant-layout/copy. Redirected the `.cpp` include from `d3d9_constant_set.h` (pulls un-ported Vk `d3d9_constant_buffer.h`) to the self header; made the header self-contained (`<unordered_set>`,`<algorithm>`,`util_math.h`,`dxvk_hash.h`,`thread.h`). Scalar (`#else`) path compiles; SSE path needs `-msse2`. |
| d3d9/d3d9_shader_analysis.{h,cpp} | 276/354 | ADAPT (Step-1 firewall) | SM3->IR analysis; `<sm3/*>` + `<util/util_byte_stream.h>` via `-I dxbc-spirv`. Replaced `#include "../dxvk/dxvk_include.h"` -> `util_bit.h`+`util_math.h`; `#include <vulkan/vulkan.h>` -> `mtl_types.h`. `VkImageViewType` -> **`MtlTextureViewType`** (new seam type). `VK_IMAGE_VIEW_TYPE_{2D,3D,CUBE}` -> `MtlTextureViewType::{e2D,e3D,eCube}`. **FORK FIX:** upstream references undefined `MaxFloatConstantsPS` unqualified with no `using` — replaced with `caps::MaxSM3FloatConstantsPS` (224, the correct SM3 PS ceiling) and added `using namespace dxvk::caps;`. |
| util/dxvk_hash.h | 49 | relocate (0 Vk) | `DxvkHash`/`DxvkEq`/`DxvkHashState` moved verbatim from `src/dxvk/dxvk_hash.h` into `util/` (pure FNV hashing; only dep is `util_env.h`) so d3d9 ADAPT files stop depending on the deleted `dxvk/` tree. |

### SEAM CHANGES (mtl/ this resume)
- **`MtlTextureViewType`** (mtl_types.h) — was `VkImageViewType`; the per-sampler-slot
  view dimensionality recorded by shader analysis. Values mirror the Vk enum
  (1D/2D/3D/Cube/1DArray/2DArray/CubeArray). Non-breaking addition. No existing seam
  method signature changed.

### build-driver.sh
- `-msse -msse2 -mfpmath=sse` added to FLAGS (the util SSE fast-paths, e.g. constant
  copy's `_mm_stream_*`, need SSE2 enabled — upstream's i686 target builds with it).
- `-I $V2/third_party/dxbc-spirv` added to FLAGS for the sm3 analysis TUs.
- `util/sha1/*.cpp` added to the CORE glob + a dedicated `gcc` compile of `sha1.c`
  (the C SHA-1 core) folded into the archive — `config.cpp`'s `Sha1Hash` reference was
  an unresolved link input.
- Fatal CORE now includes `d3d9_constant_copy.cpp` + `d3d9_shader_analysis.cpp`.
- **New non-fatal `d3d9.dll` link ATTEMPT step**: builds the winemetal32 import lib via
  `dlltool` from `third_party/winemetal/winemetal32.def`, then
  `i686-w64-mingw32-g++ -shared` over `libdriver_core.a` + import lib + the upstream
  `d3d9.def` (+ `-lgdi32 -luser32 -lole32 -lsetupapi`). Non-fatal; classifies and
  prints the unresolved frontier.

### d3d9.dll link frontier (the honest remaining worklist)
- **20 unexported `d3d9.def` entries** — `Direct3DCreate9{,Ex,On12,On12Ex}`,
  `Direct3DShaderValidatorCreate9`, `PSGP*`, `D3DPERF_*`, `DebugSet*`,
  `Direct3D9{EnableMaximizedWindowedModeShim,ForceHybridEnumeration}`,
  `DXVK_{,Un}RegisterAnnotation`. All gated on porting **d3d9_main / d3d9_interface /
  d3d9_device (+ the common-resource/adapter/swapchain graph)** — none of which are
  ported. This is the true remaining port, not a link-flag gap.
- **4 undefined references (pure build-input gaps, not device logic):**
  - `dxbc_spv::sm3::{Parser::Parser, Parser::parseInstruction, IoMap::findFixedFunctionLocation}`
    — the dxbc-spirv **sm3 parser library** is not yet linked; its `.cpp`s compile
    standalone (verified `sm3_parser.cpp`), so a `libdxbcsm3.a` build + link resolves them.
  - `dxvk::Logger::s_instance` — the `static Logger s_instance;` member (log.h:51) has
    **no out-of-line definition** in this fork snapshot's `log.cpp` (verified byte-identical
    to upstream). Pre-existing latent gap, surfaced only now at first link. Left as-is
    (KEEP file — not edited mid-port); one-line `Logger Logger::s_instance;` fixes it.

### NOT done this resume (steps 2–6 of the C-3 plan)
Confirmed the same structural blocker the checkpoint identified, now with specifics:
`d3d9_common_buffer.h` includes `../dxvk/dxvk_device.h` + `../dxvk/dxvk_cs.h` +
`d3d9_device_child.h`, and its members are `Rc<DxvkBuffer>`/`DxvkBufferSlice`/
`Rc<DxvkResourceAllocation>` with `DxvkCsThread::SynchronizeAll` — i.e. common_buffer/
common_texture/adapter/device/swapchain form **one graph** that only compiles together
and needs a much larger MtlDevice/MtlContext **resource surface than the current factory
stub exposes**: `MtlBufferSlice` value-ops + `GetBufferSlice(offset,length)`,
the `allocateStorage`/Discard **rename ring** returning `Rc<MtlAllocation>`, and the ~89
`DxvkContext` call signatures. The seam factory added at checkpoint (createBuffer/
createImage/createSampler/...) is necessary but **not sufficient** for common_buffer —
the `DxvkBufferSlice`/`allocateStorage` half of the DxvkDevice surface is still missing.
Steps 2–6 (common resources, adapter, resource wrappers, device.{h,cpp}, swapchain,
main/interface) remain.

**DoD status: PARTIAL.** Step 1 (dxbc-spirv firewall: constant_copy + shader_analysis)
COMPLETE + fatal + compiling. Link harness ADDED + attempted; frontier reported above.
Steps 2–6 NOT done — the device/common-resource/adapter/swapchain graph is unchanged.

## Phase C-4 — sm3 parser lib linked + link-frontier bug fix (2026-07-11, solo/sequential)

Resumed solo (no sub-agents). Storage surface (`MtlAllocation` + `MtlBuffer::
allocateStorage/assignStorage/mapPtr/storage`) confirmed present in `mtl_resource.h`/
`mtl_device.h` — it unblocks the header retype of step 1/2, but the resource-graph
`.cpp` bodies remain gated on the full device (below). From-scratch `bash scripts/
build-driver.sh` is GREEN: **33 CORE TUs**, `libdriver_core.a` (~10.2 MB), Vulkan gate
PASS, and the d3d9.dll link frontier is now **20 unexported / 0 undefined references**
(was 20 / 3).

### Done this resume
- **`libdxbcsm3.a` built + linked (task step 6).** All 39 `dxbc-spirv` `sm3/`+`ir/`+
  `util/` `.cpp` (37 archived after mtime-dedup) compile clean under i686 mingw with the
  driver FLAGS; archived to `build/driver/libdxbcsm3.a` and added to the d3d9.dll link
  line (archive semantics — the linker extracts only the referenced objects). This
  **resolves all 3 previously-undefined references**: `dxbc_spv::sm3::Parser::Parser`,
  `Parser::parseInstruction`, `IoMap::findFixedFunctionLocation`. Own archive, no SPIR-V
  crosses into the frontend (the sm3/ir/util TUs are self-contained third-party code).
- **Latent build-script bug fixed.** The link-frontier parser did
  `undef="$(grep -oE 'undefined reference'… )"` under `set -euo pipefail`; once the sm3
  refs were resolved that `grep` matched nothing → pipeline exit 1 → the whole script
  aborted *before* printing the INCOMPLETE summary / `build OK`. Wrapped both the `undef`
  and `noexp` greps in `{ … || true; }` so an empty frontier no longer fails the run.
  (The baseline only passed because undefined refs always happened to exist.)

### build-driver.sh
- New `libdxbcsm3.a` compile+archive step (parallel, mtime-cached, under the existing
  FLAGS incl. `-I dxbc-spirv`), inserted at the head of the non-fatal link ATTEMPT.
- `$DXBCSM3_LIB` added to the `i686-w64-mingw32-g++ -shared` link inputs (after `${OBJS[@]}`,
  before the winemetal import lib).
- `undef`/`noexp` frontier greps hardened against the empty-frontier `set -e` abort.

### Remaining (unchanged structural blocker — the true C-4 body)
The 20 unexported `d3d9.def` entries (`Direct3DCreate9*`, `D3DPERF_*`, `DebugSet*`,
`DXVK_*RegisterAnnotation`, `PSGP*`, shims) still require porting the
device/common-resource/adapter/swapchain/main/interface graph. That is **not a token
swap**: every resource-creation site (`D3D9CommonBuffer::CreateBuffer/CreateStagingBuffer`,
common_texture, adapter, initializer) is written in raw Vulkan
(`DxvkBufferCreateInfo{usage,stages,access}`, `VkBufferUsageFlags`,
`VkMemoryPropertyFlags`, `VK_BUFFER_USAGE_*`, `VK_MEMORY_PROPERTY_*`) and must be
*semantically* rewritten onto `MtlBufferCreateInfo{usage:MtlUsage, cpuAccess:MtlCpuAccess}`
to pass the ZERO-VULKAN gate — and every `.cpp` routes through `D3D9DeviceEx`
(`m_parent->GetOptions()/LockBuffer/GetDXVKDevice()->createBuffer(info,memFlags)`), so
none compile in isolation from the 1718-line `d3d9_device.h` + 9288-line `d3d9_device.cpp`.
This resume advanced the *build-input* half of the link frontier to zero; the *device
export* half is the standing multi-file semantic port.

## Phase C-5 — STAGE 1 device-graph header port (partial) + PHASE-C5-CHECKPOINT (2026-07-11, solo/sequential)

Resumed solo (no sub-agents), per doc 05 (NORMATIVE resource-mapping). STAGE 1 =
retype every header of the device graph onto the Mtl seam; gate = a probe TU
(`driver/d3d9/phasec_probe.cpp`) that includes them compiles fatal. From-scratch
`bash scripts/build-driver.sh` is GREEN: **34 CORE TUs compile** (probe now fatal),
Vulkan gate PASS, 0 undefined refs, 20 unexported d3d9.def entries (unchanged).

### Env / seam additions this resume
- **Vendored `driver/dxvk/dxvk_allocator.h`** (byte-copy of upstream; Vulkan-clean, only
  needs `util/util_env.h`). Unblocks `util/util_unmap.h` (which `#include`s it) so
  `d3d9_common_texture.h`'s `MemoryFileRegion m_data` member compiles on the 32-bit
  (i686) UNMAPPABLE path. `util_unmap.cpp` stays non-CORE. Minimal, non-breaking.

### Files DONE this resume (STAGE-1 headers, per-file) — all compile in the probe
| File | Kind | Notes / substitutions |
|---|---|---|
| d3d9/d3d9_common_buffer.h | NEW (ported) | `Rc<DxvkBuffer>`→`Rc<MtlBuffer>`; `DxvkBufferSlice`→`MtlBufferSlice`; `Rc<DxvkResourceAllocation>`→`Rc<MtlAllocation>`; `VkDeviceSize`→`MtlDeviceSize`; include `../dxvk/dxvk_{device,cs}.h`→`../mtl/mtl_device.h`+`../cs/dxvk_cs.h`. Bodies (CreateBuffer/CreateStagingBuffer) = STAGE 2 per doc 05 §Buffers table. |
| d3d9/d3d9_common_texture.h | NEW (ported) | `Rc<DxvkImage>`→`Rc<MtlTexture>`; `Rc<DxvkImageView>`→`Rc<MtlTextureView>`; `VkImageUsageFlags`→`MtlTextureUsage`; `VkExtent3D`→`MtlExtent3D`; `VkImageType`→`MtlImageType`; `VkImageViewType`→`MtlTextureViewType`; `VkImageSubresource`→`MtlSubresourceLayers`; `VkImageAspectFlags`→`MtlAspectMask`; `D3D9_VK_FORMAT_MAPPING`→`D3D9_MTL_FORMAT_MAPPING`; `DxvkImageCreateInfo`→`MtlImageCreateInfo`. **CreateView drops the `VkImageLayout` arg** (backend owns layout) → new sig `CreateView(Layer,Lod,MtlTextureUsage,Srgb)`. **DROPPED the ID3D9VkInterop path** (`d3d9_interop.h` include, `D3D9VkInteropTexture m_d3d9Interop`, `GetVkInterop()`, `ExportImageInfo()`, `OptimizeLayout()`) — Vulkan interop, doc 02 §DROP. `util::computeMipLevelExtent` (absent Vulkan util) inlined in `GetExtentMip`. |
| d3d9/d3d9_resource.h | (pre-existing KEEP) | Vulkan-clean; unchanged. |
| d3d9/d3d9_subresource.h | NEW (ported) | `Rc<DxvkImageView>`→`Rc<MtlTextureView>`; RTV/DSV creation drops usage+layout Vk args, calls new CreateView sig with `MtlTextureUsageRenderTarget`. |
| d3d9/d3d9_surface.h | NEW (ported) | `VkExtent2D`→`MtlExtent2D` in `GetSurfaceExtent`. |
| d3d9/d3d9_volume.h | NEW (ported) | Pure D3D9 (no Vk); verbatim. |
| d3d9/d3d9_query.h | NEW (ported) | `DxvkContext`→`MtlContext` (Begin/End); `Rc<DxvkQuery>`→`Rc<MtlQuery>`; `Rc<DxvkEvent>`→`Rc<MtlEvent>`; include `../dxvk/dxvk_context.h`→`../mtl/mtl_context.h`. |
| d3d9/d3d9_buffer.h | (pre-existing) | Vulkan-clean already; now compiles transitively via ported common_buffer.h. |
| d3d9/phasec_probe.cpp | NEW | STAGE-1 probe TU; **added to fatal CORE** in build-driver.sh. Currently includes the 8 headers above. Grow it (add device.h/swapchain.h) as they land. |

### PHASE-C5-CHECKPOINT — exact position + resume plan
STOPPED at a clean file boundary **before `d3d9_device.h`** (STAGE 1 NOT complete).
STAGE 2 (bodies) NOT started. Build is GREEN; nothing half-written on disk.

**STAGE-1 headers still to port (dependency order):**
1. `d3d9_constant_buffer.h` — 28 Vk refs, still `#include "../dxvk/dxvk_buffer.h"`. Retype
   to `MtlBufferSlice`/`MtlAllocation`/`MtlDeviceSize`; `VkBufferUsageFlags`/
   `VkShaderStageFlags`/`VkMemoryPropertyFlags`→drop-or-`MtlUsage`/`MtlStageMask`/
   `MtlCpuAccess`. Self-containable (mtl seam only). Remove Vulkan-gate whitelist entry after.
2. `d3d9_swvp_emu.h` — `Rc<DxvkShader>`→`Rc<MtlShaderFunction>`; include
   `../dxvk/dxvk_shader.h`→`../mtl/mtl_context.h`.
3. `d3d9_adapter.h` — **needs NEW seam types** (checkpoint list): `MtlInstance`/`MtlAdapter`
   (was `Rc<DxvkInstance>`/`Rc<DxvkAdapter>`), `MtlFormatInfo` metadata (was
   `DxvkFormatInfo*`, in mtl_format.h), `DxvkDeviceCapabilities`→`MtlDeviceInfo`-ish,
   `D3D9VkFormatTable`→`D3D9MtlFormatTable` (already renamed in d3d9_format.h), `VkFormat`→
   drop the `CheckDeviceVkFormat` path. Add these to mtl_types.h/mtl_device.h first.
4. **`d3d9_device.h` (1719 lines)** — THE linchpin; probe gate requires it. Read in 2 pages
   (offset 1437 for the tail with the private members: m_dxvkDevice, m_csChunkPool,
   m_memoryAllocator, m_consts, m_state, etc.). Retypes needed BEYOND simple tokens — it
   pulls a **DxvkDevice surface the seam lacks**, which MUST be added to `mtl_device.h`
   first (this is the true blocker prior checkpoints named):
     - `GetEnabledShaderStages()` → `m_dxvkDevice->getShaderPipelineStages()` (VkPipelineStageFlags→MtlStageMask; add `getShaderPipelineStages()` to MtlDevice or cut).
     - `WaitForResource(const DxvkPagedResource&, ...)` → needs an `MtlPagedResource`/base handle concept.
     - `const DxvkFormatInfo* UnsupportedFormatInfo(...)`, `CalcImageLockOffset(..., const DxvkFormatInfo*, ...)` → `MtlFormatInfo`.
     - `DxvkStagingBufferStats GetStagingMemoryStatistics()`, `AllocStagingBuffer`/`AllocUPBuffer` (`DxvkStagingBuffer`, `dxvk_staging.h`) → an MtlStaging concept or reuse MtlBuffer ring.
     - `BeginFrame/EndFrame(Rc<DxvkLatencyTracker>,...)` → `Rc<MtlLatencyTracker>` (exists in mtl_context.h).
     - `DxvkShaderOptions GetShaderOptions()`, `m_dxvkShaderOptions` → decide (drop/neutral).
     - `TransformImage(..., const VkImageSubresourceRange*, VkImageLayout Old, VkImageLayout New)` → `MtlSubresourceRange` + drop layouts (doc 05: transitions delete).
     - `UpdateTextureFromBuffer(..., VkOffset3D, VkExtent3D, VkOffset3D)` → `MtlOrigin`/`MtlExtent3D`.
     - `Rc<DxvkDevice> m_dxvkDevice` → `Rc<MtlDevice>`; `DxvkCsChunkPool m_csChunkPool` / `DxvkCsChunkRef` come from `cs/dxvk_cs.h` (kept). `MemoryFilePool m_memoryAllocator` → util_unmap.h (now available via vendored allocator).
     - Includes to retarget: `../dxvk/dxvk_{device,cs,staging}.h`; DROP `d3d9_interop.h`,
       `d3d9_on_12.h` (and stub `friend D3D9VkInteropDevice` / `m_d3d9On12Args`), and decide `d3d9_bridge.h`.
5. `d3d9_texture.h` (`#include "d3d9_device.h"`; otherwise Vulkan-clean — trivial once device.h lands), `d3d9_initializer.h` (`Rc<DxvkDevice>`→`Rc<MtlDevice>`; `DxvkBufferSlice`→`MtlBufferSlice`; `DxvkCsChunkRef` kept), `d3d9_annotation.h` (`../dxvk/dxvk_annotation.h`→vendor or retarget `IDXVKUserDefinedAnnotation`), `d3d9_swapchain.h` (onto MtlPresenter), `d3d9_interface.h`.

**Then extend `phasec_probe.cpp`** to `#include "d3d9_device.h"` + `d3d9_swapchain.h` +
`d3d9_texture.h` etc.; when it compiles fatal, STAGE 1 is done → write **STAGE-1-DONE**
marker here. STAGE 2 (bodies, dependency order, ending with the 9288-line
`d3d9_device.cpp` + swapchain.cpp + interface.cpp + main/exports) follows.

**Net this resume:** 6 new ported headers + 1 vendored allocator + probe wired fatal;
8-header resource sub-graph VERIFIED compiling Vulkan-free; build GREEN. STAGE 1 ~40%
(the resource-object leaf graph); the device/adapter/constant_buffer/swapchain/interface
half + the MtlDevice-surface design remain.

## Phase C-6 — STAGE 1 COMPLETE (device-graph header port) — STAGE-1-DONE (2026-07-11, solo/sequential)

Resumed solo (no sub-agents) from PHASE-C5-CHECKPOINT. Ported ALL remaining STAGE-1
headers onto the Mtl seam; the probe TU (`phasec_probe.cpp`) now `#include`s the full
device graph and compiles **fatal**. From-scratch `bash scripts/build-driver.sh` is
GREEN: **34 CORE TUs**, `libdriver_core.a` (~10.3 MB), **Vulkan gate PASS**, 0 undefined
refs, 20 unexported d3d9.def entries (unchanged — STAGE 2 exports).

### **STAGE-1-DONE** — every device-graph header compiles Vulkan-free on the seam.

### Files DONE this resume (per-file)
| File | Kind | Notes / substitutions |
|---|---|---|
| d3d9/d3d9_shader.h | GRAPH PORT | Extended the neutral stub into the FULL shader class graph the frontend needs (state.h holds Com<D3D9VertexShader>/Com<D3D9PixelShader>; device.h drives D3D9CommonShader/D3D9ShaderModuleSet). Added `D3D9ShaderResourceMapping` (CbvIndex + getTextureSlotInfo VkShaderStageFlags→MtlStageMask), `D3D9ShaderOptions`, `D3D9ShaderCreateInfo` (DxvkIrShaderCreateInfo DROPPED — SPIR-V), `D3D9CommonShader` (`Rc<DxvkShader>`→`Rc<MtlShaderFunction>`, wraps ported D3D9ShaderAnalysis), `D3D9Shader<Base>`/`D3D9VertexShader`/`D3D9PixelShader`/`D3D9ShaderModuleSet`. **`DxvkShaderHash`→new neutral `D3D9ShaderModuleKey`** (Sha1Hash+MtlShaderStage, hash()/eq()). Kept ID3D9MetalShaderBackend (Phase-D). Now pulls d3d9_shader_analysis.h (dxbc via -I, gate-exempt). |
| d3d9/d3d9_constant_buffer.h | ADAPT retype | `VkDeviceSize`→`MtlDeviceSize`; `VkBufferUsageFlags`→`MtlUsage`; `VkShaderStageFlags`→`MtlStageMask`; `VkMemoryPropertyFlags`→`MtlCpuAccess`; `Rc<DxvkBuffer>`→`Rc<MtlBuffer>`; `Rc<DxvkResourceAllocation>`→`Rc<MtlAllocation>`; include dxvk_buffer.h→mtl_device.h. **Removed from Vulkan-gate whitelist (now empty).** |
| d3d9/d3d9_swvp_emu.h | NEW (ported) | `Rc<DxvkShader>`→`Rc<MtlShaderFunction>`; include dxvk_shader.h→mtl_context.h; added util_small_vector.h (was transitive). |
| d3d9/d3d9_adapter.h | NEW (ported) | `Rc<DxvkInstance>`→`Rc<MtlInstance>`; `Rc<DxvkAdapter>`→`Rc<MtlAdapter>`; `DxvkDeviceCapabilities`→`MtlDeviceInfo`; `const DxvkFormatInfo*`→`const MtlFormatInfo*`; `D3D9_VK_FORMAT_MAPPING`→`D3D9_MTL_FORMAT_MAPPING`; `D3D9VkFormatTable`→`D3D9MtlFormatTable`. **DROPPED CheckDeviceVkFormat(VkFormat) path.** D3D9ON12_ARGS kept (d3d9_include.h). |
| **d3d9/d3d9_device.h** | **NEW (ported — THE linchpin, 1719 lines)** | Copied + retyped: `Rc<DxvkDevice>`→`Rc<MtlDevice>` (ctor param + m_dxvkDevice + GetDXVKDevice); `GetEnabledShaderStages()`→`m_dxvkDevice->getShaderStages()` (MtlStageMask); `D3D9_VK_FORMAT_MAPPING`→`D3D9_MTL_FORMAT_MAPPING`; `const DxvkFormatInfo*`→`const MtlFormatInfo*`; `WaitForResource(DxvkPagedResource)`→`WaitForResource(const MtlResource&)`; `CalcImageLockOffset` DxvkFormatInfo→MtlFormatInfo; `UpdateTextureFromBuffer` VkOffset3D/VkExtent3D→MtlOrigin/MtlExtent3D; `BeginFrame/EndFrame(Rc<DxvkLatencyTracker>)`→`Rc<MtlLatencyTracker>`; `TransformImage` VkImageSubresourceRange*→MtlSubresourceRange* + **OldLayout/NewLayout DROPPED**; `D3D9BufferSlice.slice` DxvkBufferSlice→MtlBufferSlice; `AllocUP/StagingBuffer(VkDeviceSize)`→MtlDeviceSize; `DxvkStagingBufferStats`→`MtlStagingBufferStats`; members `Rc<DxvkBuffer> m_upBuffer`→MtlBuffer, `DxvkStagingBuffer m_stagingBuffer`→MtlStagingBuffer, `DxvkSubmitStatus`→MtlSubmitStatus, VkDeviceSize×5→MtlDeviceSize. **DROPPED:** DxvkShaderOptions m_dxvkShaderOptions + GetShaderOptions(); `friend D3D9VkInteropDevice` + `D3D9VkInteropDevice m_d3d9Interop`; `D3D9On12 m_d3d9On12` (D3D9ON12_ARGS m_d3d9On12Args kept); `VkImageLayout m_hazardLayout`; `DxvkDepthBiasRepresentation m_depthBiasRepresentation`; includes d3d9_interop.h/d3d9_on_12.h/dxvk_staging.h. DxvkCs* (chunk pool/thread/ref/datablock) KEPT (cs/). Added `#include ../util/sync/sync_signal.h` (sync::Fence, was via dxvk_device.h). |
| d3d9/d3d9_texture.h | KEEP (verbatim) | Vulkan-clean; copied unchanged (only includes d3d9_device.h). |
| d3d9/d3d9_initializer.h | NEW (ported) | `Rc<DxvkDevice>`→`Rc<MtlDevice>`; `DxvkBufferSlice`→`MtlBufferSlice`; DxvkCsChunkRef KEPT. |
| d3d9/d3d9_annotation.h | KEEP (verbatim) | Vulkan-clean; copied. Vendored `driver/dxvk/dxvk_annotation.h` (IDXVKUserDefinedAnnotation, COM-only over ID3DUserDefinedAnnotation — no Vk). |
| d3d9/d3d9_bridge.h | KEEP (verbatim) | D3D8 interop bridge; Vulkan-clean (holds a `D3D9DeviceEx*` only); copied for device.h's `DxvkD3D8Bridge m_d3d8Bridge`. |
| **d3d9/d3d9_swapchain.h** | **REWRITE (ported onto MtlPresenter)** | `Rc<Presenter>`→`Rc<MtlPresenter>` (in D3D9WindowContext + CreatePresenter); `Rc<DxvkDevice>`→`Rc<MtlDevice>`; `VkExtent2D`→`MtlExtent2D` (member + GetPresentExtent); `Rc<DxvkLatencyTracker>`→`Rc<MtlLatencyTracker>`; `VkSurfaceFormatKHR GetSurfaceFormat()`→`MtlFormat`. **DROPPED:** entire `D3D9VkExtSwapchain` class + `m_swapchainExt` + friend (ID3D9VkExtSwapchain — Vulkan interop); `Rc<DxvkSwapchainBlitter> m_blitter` + CreateBlitter (blit now in presenter); hud items m_apiHud/m_latencyHud; `VkColorSpaceKHR m_colorspace`; `std::optional<VkHdrMetadataEXT> m_hdrMetadata`; includes dxvk/hud/dxvk_hud.h + dxvk_swapchain_blitter.h. |
| d3d9/d3d9_interface.h | NEW (ported) | `Rc<DxvkInstance>`→`Rc<MtlInstance>` (m_instance + GetInstance); `Find9On12Args(Rc<DxvkAdapter>)`→`Rc<MtlAdapter>`; include dxvk_instance.h→mtl_device.h. **DROPPED:** d3d9_interop.h + `D3D9VkInteropInterface m_d3d9Interop` + `D3D9VkExtInterface m_d3d9ExtInterface`. DxvkD3D8InterfaceBridge KEPT (d3d9_bridge.h). |

### SEAM / support changes this resume
- **FIXED a latent double-definition bug:** `MtlFormatInfo` was defined in BOTH
  `mtl_types.h` (canonical, appended in Phase C) AND `mtl_format.h` (older
  blockSize/planeCount copy). It only compiled before because `d3d9_format.o` was
  mtime-cached; the probe pulling d3d9_format.h fresh exposed it. Removed the duplicate
  struct + dead `lookupMtlFormatInfo()` from `mtl_format.h`/`.cpp` (now a thin re-export
  shim); `mtl_types.h`'s `MtlFormatInfo` + `lookupFormatInfo()` is the single source.
  Follow-on: `d3d9_format.cpp`'s `GetUnsupportedFormatInfo` table (14 entries) was on the
  OLD layout (`{elemSize, blockSize{1,1,1}, aspects, planeCount}`) and only compiled due to
  mtime caching — converted to the canonical `{elemSize, blockWidth, blockHeight, flags,
  aspects}`. Caught by a from-scratch (obj-cache-cleared) rebuild; DoD verified from-scratch.
- **`MtlStagingBuffer() = default;`** added (mtl_resource.h) — it is a *value* member
  `D3D9DeviceEx::m_stagingBuffer` (was value-type DxvkStagingBuffer), needs default-ctor.
- **`static constexpr uint32_t MaxNumSpecConstants = 20u;`** added to d3d9_state.h — was
  `DxvkLimits::MaxNumSpecConstants` reached via dxvk_shader.h before the include chain was
  severed; d3d9_state.h references it unqualified.
- **Vulkan-gate whitelist emptied** (d3d9_constant_buffer.h ported).
- **phasec_probe.cpp** now includes the full STAGE-1 set (constant_buffer, swvp_emu,
  adapter, shader, state, device, texture, initializer, annotation, swapchain, interface)
  + the prior 8; compiles fatal in CORE.

### PHASE-C6-CHECKPOINT — exact position + STAGE 2 resume plan
STAGE 1 is **DONE**. STAGE 2 (bodies) **NOT started** — nothing half-written on disk;
build GREEN. Resume STAGE 2 in dependency order, promoting each `.cpp` to fatal CORE:

1. **common_buffer.cpp + common_texture.cpp** — the creation sites EXACTLY per doc 05
   tables (VkBufferUsageFlags/VkMemoryPropertyFlags → MtlBufferCreateInfo{usage,cpuAccess};
   VkImageUsageFlags/layouts → MtlImageCreateInfo). These route through `m_parent->
   GetDXVKDevice()->createBuffer/createImage` — the MtlDevice factory surface EXISTS
   (mtl_device.h). Watch: `MtlStagingBuffer`/`MtlAllocation` ring ops in Lock/Discard.
2. **adapter.cpp** — caps/format probing onto MtlAdapter/MtlDeviceInfo; the
   CheckDeviceVkFormat path was dropped (route through D3D9Format only).
3. The small ones: subresource/surface/volume/texture/buffer/cursor/query/swvp_emu/
   constant_buffer/initializer/annotation `.cpp`.
4. **d3d9_device.cpp (9288 lines, 68 EmitCs sites)** — the bulk. Minimal-token edits of
   each `ctx->…` EmitCs lambda onto MtlContext; the header retypes above fix the member/
   signature types, so most sites are `DxvkContext`→`MtlContext` + the value-type swaps
   already made in the header. Known body-only retypes to expect: DxvkBufferSlice→
   MtlBufferSlice locals, VkOffset3D/VkExtent3D→MtlOrigin/MtlExtent3D, the dropped
   m_dxvkShaderOptions/m_hazardLayout/m_depthBiasRepresentation references (delete their
   uses), TransformImage callers (drop layout args), `D3D9ShaderCreateInfo` construction
   (drop irCreateInfo), `D3D9ShaderModuleKey` construction (was DxvkShaderHash — build
   from Sha1Hash::compute + MtlShaderStage). D3D9CommonShader ctor/D3D9ShaderModuleSet::
   GetShaderModule bodies live in d3d9_shader.cpp (currently the Phase-D stub — the real
   SM3→Air compile is the shader REWRITE track; STAGE 2 may leave GetShaderModule stubbed
   returning a null MtlShaderFunction to keep the link honest).
5. **swapchain.cpp** — the REWRITE onto MtlPresenter (createMtlPresenter, acquire/present,
   frame-latency signal); the hardest body. blitter/hud/colorspace/HDR call sites deleted.
6. **interface.cpp + main.cpp + exports** — resolves the 20 unexported d3d9.def entries
   (Direct3DCreate9*, D3D9on12, D3DPERF_*, DebugSet*, DXVK_*RegisterAnnotation, PSGP*).
   main.cpp instantiates D3D9InterfaceEx which needs the whole device stack — do LAST.

DoD remaining: from-scratch build green with all driver TUs fatal + d3d9.dll LINKS
(20/20 def exports resolved; missing backend symbols → minimal seam stubs, logged).

## Phase C-7 — STAGE 2 (bodies) partial: 12 leaf/mid bodies ported fatal (2026-07-11, solo/sequential)

Resumed STAGE 2 solo from PHASE-C6-CHECKPOINT. Ported the leaf + mid-tier `.cpp`
bodies onto the Mtl seam and promoted **each to fatal CORE the moment it compiled**
(per-file DoD, no batch checkpointing). From-scratch `bash scripts/build-driver.sh`
is GREEN: **46 CORE TUs** (was 34), `libdriver_core.a` ~14.8 MB, **Vulkan gate PASS**
(whitelist stays empty). d3d9.dll link still INCOMPLETE — 20 unexported def entries +
33 undefined refs, ALL from the un-ported device/adapter/interface/main graph
(D3D9DeviceEx::*, D3D9Adapter::*, D3D9Multithread::LockContested, MemoryFilePool::*).

### Files DONE this resume (all compile fatal, in CORE)
| File | Kind | Notes / substitutions |
|---|---|---|
| d3d9_common_buffer.cpp | PORT (doc 05) | CreateBuffer/CreateStagingBuffer rewritten to MtlBufferCreateInfo{usage,cpuAccess}: VB/IB→VertexBuffer/IndexBuffer; DIRECT map→cpuAccess Write (Read if readable); staging→Staging+Write/Read. Vulkan usage/stage/access/memFlags dropped. |
| d3d9_common_texture.cpp | PORT (doc 05) | CreatePrimaryImage/CreateResolveImage→MtlImageCreateInfo (usage from D3D9 case: Sampled/RenderTarget/DepthStencil; cube flag); CreateBuffer→Staging; CheckImageSupport→supportsFormat+deviceInfo extents; CreateView→MtlImageViewKey; GetMipSize/computeMipLevelCount inlined (Vulkan util absent); GetSubresourceFromIndex→MtlSubresourceLayers. DROPPED: m_d3d9Interop, ExportImageInfo, sharedHandle export, OptimizeLayout. PHASE-E: memory-report size approx (m_totalSize), GetPlaneCount=1, view dimensionality, desc.ImageUsage fold, YUV texel-buffer usage. |
| d3d9_buffer.cpp | KEEP (verbatim) | Vulkan-clean; copied unchanged. |
| d3d9_surface.cpp / d3d9_volume.cpp / d3d9_texture.cpp | KEEP−interop | Only change: deleted the `ID3D9VkInteropTexture` QueryInterface block (Vulkan interop, doc 02 §DROP). |
| d3d9_cursor.cpp | KEEP (verbatim) | Already Vulkan-clean; added to CORE. |
| d3d9_swvp_emu.cpp | STUB port | Hash/eq operators copied verbatim. D3D9SWVPShaderGenerator (dxbc_spv IR geometry ubershader) is the SM3→Air REWRITE track (Phase-D) → REMOVED; GetShaderModule stubbed returning null MtlShaderFunction (doc-sanctioned). PHASE-E: real SWVP ProcessVertices module. |
| d3d9_annotation.cpp | PORT (§1k) | 3 EmitCs sites: VkDebugUtilsLabelEXT lambdas → `ctx->beginDebugLabel(name)`/`endDebugLabel()`/`insertDebugLabel(name)` (MtlContext*, color dropped). |
| d3d9_query.cpp | PORT (doc 05 §Queries) | createGpuQuery(VK_QUERY_TYPE_*)→createGpuQuery(MtlQueryType); createGpuEvent; DxvkGpuEventStatus/DxvkQueryData/DxvkGpuQueryStatus→Mtl*; GetTimestampQueryFrequency→device->getTimestampFrequency(); Begin/End(MtlContext*). |
| d3d9_initializer.cpp | PORT (§1h, 3c) | 4 EmitCs: initBuffer/initImage(no layout)/flushCommandList onto MtlContext; host-visible init memsets slice.buffer()->mapPtr(off); InitBuffer host-visible test via mapPtr!=null (was memFlags); packImageData/computeBlockCount inlined; SyncSharedTexture→no-op (shared handle dropped). |
| d3d9_constant_buffer.cpp | PORT (doc 05 row 3, §1d/1h) | 4 EmitCs/EmitCsCmd sites onto MtlContext (invalidateBuffer/bindUniformBuffer(Range)/uploadBuffer). CreateBuffer→MtlBufferCreateInfo{UniformBuffer,Write}; DMA split kept; Determine*() rewritten: usage=UniformBuffer, stages=MtlStageVertex/Fragment, memType=Write/Read, align=max(cacheline,256), DMA=false (UMA). mapPtr()→mapPtr field. PHASE-E: storage-buffer fallback, real const alignment query. |

### SEAM CHANGES this resume (logged per plan)
- **mtl_context.h**: added query/event result surface — `enum MtlQueryType{Occlusion,Timestamp}`,
  `MtlGpuQueryStatus`, `MtlGpuEventStatus`, `union MtlQueryData`; extended `MtlQuery`
  (`type`, `virtual getData(MtlQueryData&)`) and `MtlEvent` (`virtual test()`).
- **mtl_device.h**: added `createGpuQuery(MtlQueryType)`, `createGpuEvent()`,
  `getTimestampFrequency()` factory decls (+ forward decls of MtlQuery/MtlEvent/MtlQueryType).
- **mtl_device_stub.cpp**: stub impls — createGpuQuery/Event return live objects,
  getData→Available+zeros, test→Signaled, getTimestampFrequency→1 GHz (PHASE-E: real
  visibility-buffer readback + MTLSharedEvent serial compare in backend).

### EmitCs sites ported this resume: 11 (annotation 3 + initializer 4 + constant_buffer 4).
### Resource-creation sites ported: common_buffer 2 + common_texture 4 (doc 05 tables).
### PHASE-E markers left (grep `PHASE-E`): common_texture (memory-size approx, GetPlaneCount,
view dim, desc.ImageUsage fold, YUV texel usage, shared-handle export), common_buffer
(SWVP storage usage), swvp_emu (real module), initializer (packImageData), constant_buffer
(storage fallback, const alignment). All are correctness-neutral for the leaf graph or
belong to the shader/backend REWRITE tracks; none block compilation.

### PHASE-C7-CHECKPOINT — exact position + STAGE 2 remaining
STAGE 2 is ~55% by file count / far less by line count — the two largest bodies remain.
Build GREEN, nothing half-written on disk. Remaining, in dependency order:

1. **d3d9_adapter.cpp** (1112 lines, 58 Vk) — caps/format probing onto MtlAdapter/
   MtlDeviceInfo; CheckDeviceVkFormat path dropped (route via D3D9Format only). Resolves
   D3D9Adapter::CheckDeviceMultiSampleType + the interface's adapter enumeration.
2. **d3d9_device.cpp** (9288 lines, 68 EmitCs) — THE linchpin, NOT STARTED. Minimal-token
   per-site DxvkContext→MtlContext + the header value-swaps (DxvkBufferSlice→MtlBufferSlice,
   VkOffset3D/VkExtent3D→MtlOrigin/MtlExtent3D, drop m_dxvkShaderOptions/m_hazardLayout/
   m_depthBiasRepresentation uses, TransformImage drops layout args, D3D9ShaderCreateInfo
   drops irCreateInfo, D3D9ShaderModuleKey from Sha1Hash+MtlShaderStage). Resolves the bulk
   of the 33 undefined refs (all D3D9DeviceEx::*). Budget it as its own multi-session task.
3. **d3d9_swapchain.cpp** — REWRITE onto MtlPresenter (createMtlPresenter/acquire/present/
   frame-latency); blitter/hud/colorspace/HDR call sites deleted.
4. **d3d9_interface.cpp + d3d9_main.cpp + exports** — resolves the 20 unexported def entries
   (Direct3DCreate9*, D3D9on12, D3DPERF_*, DebugSet*, DXVK_*RegisterAnnotation, PSGP*).
5. **d3d9_multithread.cpp** — tiny; blocked on `sync::spin` (util gap, add to util/sync).

DoD remaining: d3d9.dll LINKS (20/20 def exports resolved; missing backend symbols →
minimal seam stubs). Vulkan gate already PASS with whitelist empty; keep it that way.

## Phase C-8 — adapter + multithread + support bodies ported; device.cpp mechanical layer (partial) (2026-07-11, solo/sequential)

Resumed STAGE 2 solo from PHASE-C7-CHECKPOINT. Ported the two small target files
(**d3d9_adapter.cpp**, **d3d9_multithread.cpp**) + the support bodies device.cpp
transitively needs, and promoted each to **fatal CORE**. Began d3d9_device.cpp
(the 9288-line linchpin) — its file-level mechanical layer is applied but it is a
genuine multi-session semantic port (below), so it is **not** yet in driver/ (the
ZERO-Vulkan gate forbids a partially-ported TU on disk). From-scratch
`bash scripts/build-driver.sh` is GREEN: **51 CORE TUs** (was 46),
`libdriver_core.a` ~16.8 MB, **Vulkan gate PASS** (whitelist empty). d3d9.dll link
still INCOMPLETE — 20 unexported def entries + 35 undefined refs, ~all D3D9DeviceEx::*
(device.cpp) + the CORE-excluded com_guid.cpp (logQueryInterfaceError, operator<<(GUID))
+ util_unmap.cpp (MemoryFilePool). `D3D9Adapter::CheckDeviceMultiSampleType`,
`D3D9Multithread::LockContested`, and all d3d9_monitor IsSupported*Format/
GetMonitorFormatBpp are now RESOLVED (moved out of the frontier).

### Files DONE this resume (fatal CORE, compile Vulkan-free)
| File | Kind | Notes / substitutions |
|---|---|---|
| d3d9_adapter.cpp | PORT (caps) | `DxvkGpuVendor`→**`MtlGpuVendor`** (new seam enum); ctor `m_caps(*Instance,Adapter->handle(),nullptr)`→`m_caps(Adapter->deviceInfo())`; `D3D9VkFormatTable(this,Adapter,opts)`→`D3D9MtlFormatTable(this,opts)`; all `mapping.FormatColor/FormatSrgb==VK_FORMAT_UNDEFINED`→`==MtlFormat::Invalid`; `mapping.Aspect!=VK_IMAGE_ASPECT_COLOR_BIT`→`!=MtlAspectColor`. **CheckDeviceVkFormat(VkFormat,…) DELETED** — CheckDeviceFormat now returns support directly from the valid D3D9_MTL_FORMAT_MAPPING (Apple GPUs support the standard usages universally for mapped formats). **CheckDeviceMultiSampleType** Vulkan sample-count probe (framebuffer*SampleCounts + DxvkFormatQuery + getFormatLimits) rewritten to a single `m_caps.maxSampleCount` ceiling bitmask (Metal has one device-wide sample-count cap for color+depth 2D). GetDeviceCaps `limits.pointSizeRange[1]`→`m_caps.maxPointSize`. GetAdapterLUID/CacheIdentifierInfo drop `getProperties().vk11/vk12/core` — identity synthesized from MtlDeviceInfo (vendor/deviceId/name) + ordinal-derived GUID/LUID; `props.vk12.driverID==VK_DRIVER_ID_NVIDIA_PROPRIETARY` collapses to `options.hideNvidiaGpu`. |
| d3d9_multithread.cpp | PORT (trivial) | `#include "../util/sync/sync_spinlock.h"` added (sync::spin present in util/sync — **no vendoring needed**, task's conditional not triggered). Body byte-identical. |
| d3d9_format_helpers.h | STUB (header-only) | D3D9FormatHelper (Vulkan compute YUV/packed converter) neutralized to inline no-op — all conversion formats map to MtlFormat::Invalid so ConvertFormat is dead. `.cpp` removed (Vulkan; unused). PHASE-E: real MSL-compute converter. |
| d3d9_stateblock.cpp | KEEP (verbatim) | Vulkan-clean; added to CORE. |
| d3d9_vertex_declaration.cpp | KEEP (verbatim) | Vulkan-clean; added to CORE. |
| d3d9_monitor.cpp | KEEP (verbatim) | Vulkan-clean; added to CORE (resolves adapter's IsSupported*Format/GetMonitorFormatBpp). |

### SEAM CHANGES this resume (logged per plan)
- **`MtlGpuVendor`** (mtl_types.h) — was `DxvkGpuVendor` (dxvk_adapter.h): `{Amd=0x1002,
  Nvidia=0x10de, Intel=0x8086}`. Pure PCI-vendor identity, no Vulkan. Non-breaking.
- **`MtlDeviceInfo` extended** (mtl_types.h) — added `vendor`, `deviceId`, `maxSampleCount`
  (=8), `maxPointSize` (=256.0f) so the adapter can source its synthetic identity + caps
  from the seam struct instead of VkPhysicalDeviceProperties. Backend fills or defaults.

### d3d9_device.cpp — mechanical layer applied, semantic body is the standing multi-session task
The **file-level mechanical retype is done** and preserved at
`scratchpad/device-wip/d3d9_device.cpp.mechanical-layer` (NOT on disk in driver/,
because a half-ported TU with live `Vk*` fails the ZERO-Vulkan gate — the gate is
all-or-nothing per TU). Applied so far (verified by grep): `DxvkContext`→`MtlContext`
(67), `DxvkBufferSlice`→`MtlBufferSlice` (15), `VkDeviceSize`→`MtlDeviceSize`,
`DxvkImageView`→`MtlTextureView`, `DxvkImage{,ViewKey,CreateInfo}`/`DxvkBufferCreateInfo`/
`DxvkFormatInfo`/`DxvkLatencyTracker`/`DxvkStagingBufferStats`/`DxvkGpuVendor`,
`VkImageSubresource{Layers,Range}`/`VkExtent{2,3}D`/`VkClearValue`, and the 1:1 ctx->
method renames (`bindResourceImageView`→`bindTexture`, `bindResourceSampler`→`bindSampler`,
`bindResourceBufferView`→`bindBufferView`, `setBlendConstants`→`setBlendColor`,
`copyImage{,ToBuffer}`→`copyTexture{,ToBuffer}`, `copyBufferToImage`→`copyBufferToTexture`,
`resolveImage`→`resolveTexture`, `blitImageView`→`blitTextureView`,
`ensureImageCompatibility`→`ensureTextureUsage`), + dead `../dxvk/dxvk_{adapter,instance}.h`
includes retargeted to `../mtl/mtl_device.h`.

**What REMAINS (the real body, ~400+ compiler errors after the mechanical layer):** this
is a semantic rewrite, NOT a token swap, exactly as PHASE-C4/C5 predicted. The interlocking
sites (each needs per-use judgement, seam has different field names / shapes):
- **VK_* constant families** with no 1:1 sed: `VK_IMAGE_ASPECT_{COLOR,DEPTH,STENCIL}_BIT`→
  `MtlAspect{Color,Depth,Stencil}`; `VK_INDEX_TYPE_UINT{16,32}`→`MtlIndexType::Uint{16,32}`;
  `VK_RESOLVE_MODE_*`→`MtlResolveMode::*`; and the **barrier/feedback-loop path**
  (`VK_PIPELINE_STAGE_*`, `VK_ACCESS_*`, `VkImageLayout`, `m_hazardLayout`,
  `feedbackLoopAspects`, `VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT`) which is a
  genuine semantic rewrite onto `emitGraphicsBarrier(MtlStageMask,MtlBarrierScope,…)` +
  the doc 01 contract-3b auto-barrier model — no mechanical mapping.
- **Field-name deltas**: `VkImageSubresourceLayers.aspectMask/baseArrayLayer`→
  `MtlSubresourceLayers.aspects/baseLayer`; `VkOffset3D`→`MtlOrigin` in copies vs
  `MtlOffset3D` in blit corner-boxes (per-use); `DxvkFormatInfo->blockSize`(VkExtent3D)/
  `->flags.test(DxvkFormatFlag::X)`/`->aspectMask`→`MtlFormatInfo.blockWidth/blockHeight/
  flags(bitmask)/aspects`; `DxvkImageUsageInfo`→`MtlTextureUsage`.
- **Signature-changed ctx-> calls** needing arg rewrites (not just names):
  `draw(count, VkDrawIndirectCommand*)`→`draw(count, MtlDrawArgs*)` (fields are
  field-compatible — good), `drawIndexed` (idem, MtlDrawIndexedArgs compatible),
  `setInputAssemblyState(DxvkInputAssemblyState)`→`setPrimitiveTopology(MtlPrimitiveTopology)`,
  `setInputLayout(n,DxvkVertexInput*,…)`→`setVertexLayout(n,MtlVertexAttr*,n,MtlVertexLayout*)`,
  `clearRenderTarget`/`clearImageView`/`resolveTexture`/`blitTextureView`/`ensureTextureUsage`
  arg-lists, `transformImage` (drop OldLayout/NewLayout — 2 layout args deleted).
- **Dropped-member uses to delete**: `m_dxvkShaderOptions` (6), `m_hazardLayout` (3),
  `m_depthBiasRepresentation` (8); `D3D9ShaderCreateInfo` drop irCreateInfo;
  `D3D9ShaderModuleKey` build from Sha1Hash+MtlShaderStage (was `DxvkShaderHash`).
- The **UploadManagedTexture conversion branch** (`d3d9_device.cpp:~5370-5416`) uses
  `formatInfo->blockSize/planes`, `DxvkFormatFlag::MultiPlane`, `util::computeBlockCount`,
  `util::packImageData` — Vulkan-util with no seam equivalent; PHASE-E-neuter (dead: all
  conversion formats Invalid) rather than port.

### PHASE-C8-CHECKPOINT — exact position + resume plan
Build GREEN at 51 CORE TUs, gate PASS, nothing half-written on disk in driver/. The
device.cpp mechanical layer is banked in scratchpad for the successor. Remaining, in
dependency order (unchanged from C-7 items 2–5, now with the mechanical layer pre-done):
1. **d3d9_device.cpp** — resume from the scratchpad mechanical-layer copy; work the ~400
   semantic sites above (barrier/RT path is the hard core), OR per the finale directive
   STUB whole methods with `// PHASE-C8-TODO` compiling bodies to reach LINK, logging the
   cut list. Resolves the bulk of the 35 undefined D3D9DeviceEx::* refs. Its own session.
2. **d3d9_swapchain.cpp** — REWRITE onto MtlPresenter (createMtlPresenter/acquire/present/
   frame-latency; blitter/hud/colorspace/HDR deleted).
3. **d3d9_interface.cpp + d3d9_main.cpp** — the 20 def exports (Direct3DCreate9*, D3D9on12,
   D3DPERF_*, DebugSet*, DXVK_*RegisterAnnotation, PSGP*). main.cpp instantiates
   D3D9InterfaceEx (needs the full device stack) — do LAST.
4. Seam-stub the residual backend/util refs: `D3D9GlobalAnnotationList::s_instance`,
   `logQueryInterfaceError`/`operator<<(GUID)` (CORE-excluded com_guid.cpp — firewall or
   vendor), `MemoryFilePool::*` (util_unmap.cpp — excluded).

**DoD status: PARTIAL.** Two of the four target files (adapter, multithread) + their support
graph (format_helpers stub, stateblock, vertex_declaration, monitor) DONE + fatal + Vk-free.
device.cpp mechanical layer done (banked). d3d9.dll does **NOT** yet LINK — the
device.cpp semantic body + swapchain + interface/main remain (the standing multi-session port).

## Phase C-9 — d3d9_device.cpp SEMANTIC PORT COMPLETE (fatal CORE) (2026-07-11, solo)

Resumed from the banked mechanical-layer. **d3d9_device.cpp (9171 lines) now compiles
FATAL-clean** (`-fsyntax-only` exit 0) and is **promoted to CORE**. From-scratch
`bash scripts/build-driver.sh` is **GREEN: 59 CORE TUs**, libdriver_core.a ~24.9 MB,
**Vulkan gate PASS** (all remaining Vk/vulkan hits in device.cpp are comments). 66 EmitCs
sites preserved; 24 `// PHASE-E` markers logged in-file.

### Error-class fix counts (the ~440 semantic errors, by class)
- **VK_* / Vk* mechanical remainder** (~120): index/aspect/resolve/sample constants →
  Mtl enums; VkOffset3D→MtlOrigin, VkImageResolve→MtlResolveRegion, VkImageSubresource*→
  MtlSubresource*, VkDraw*IndirectCommand→MtlDraw*Args, VkImageBlit→local MtlBlitRegion.
- **Field-name deltas** (~70): `.aspectMask→.aspects`, `.arrayLayer/.baseArrayLayer→.baseLayer`,
  `->blockSize.{w,h}→blockWidth/Height` (+ `util::formatBlockSize`), `.mipIndex→.baseMip`,
  `.layerIndex→.baseLayer`, `.minDepthBounds→.minDepth`, `.allocatedTotal/.allocatedSinceLastReset→
  .allocated`, `->mapPtr()→->mapPtr`, `.depthBias{Constant,Slope,Clamp}→.{constantFactor,slopeFactor,clamp}`.
- **DXVK setter-API state structs** (~40): BindBlend/DepthStencil/Rasterizer/Multisample/Sampler
  rebuilt — enriched the seam Mtl* structs with the fluent `set*`/getter methods so the
  frontend stayed a token-swap (mtl_types.h).
- **Signature-changed ctx-> calls**: setInputLayout→setVertexLayout (DxvkVertexInput split into
  MtlVertexAttr+MtlVertexLayout arrays), setInputAssemblyState→setPrimitiveTopology,
  setSpecConstants→setFunctionConstants(MtlPipeline::Graphics), copyImage→copyTexture,
  transformImage (2 layout args dropped), blitTextureView corners (MtlOffset3D[2]).
- **Barrier/feedback-loop path**: EmitFeedbackLoopBarriers + EnsureImageCompatibility rewritten
  onto `emitGraphicsBarrier(MtlStageMask,MtlBarrierScope,…)` + the RT feedback mask;
  m_hazardLayout / VkImageLayout / VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_* deleted.
- **Dropped-member deletions**: m_dxvkShaderOptions/DxvkShaderCompileFlag (GetShaderOptions),
  m_depthBiasRepresentation + VK_EXT_depth_bias_control block, m_hazardLayout, D3D9ShaderCreateInfo.
  irCreateInfo, the ID3D9VkInteropDevice / IDirect3DDevice9On12 QI cases (m_d3d9Interop/m_d3d9On12).
- **DxvkShaderHash→D3D9ShaderModuleKey**: rebuilt from `Sha1Hash::compute(chunks)` + MtlShaderStage.
- **PHASE-E whole-body neuters** (no Metal analogue): ProcessVertices SWVP geometry-shader
  stream-out (Metal has no GS/transform-feedback → compute-prepass rewrite), the BC block-
  reinterpret clear + planar-YUV conversion branch (dead: conversion formats Invalid),
  EnsureSamplerLimit (no Vulkan sampler-pool cap), 9On12 submit-status handshake, D3DKMT
  shared-resource kmtLocal handles, device features()/properties()/adapter()/perfHints()/
  getSamplerStats()/waitForSubmission() probes.

### Seam enrichments this session (driver/mtl — mine per coordination rule)
- **mtl_resource.h**: MtlTexture gains `MtlImageInfo m_info` + `info()`/`mipLevelExtent()`/
  `formatInfo()`/`createView()` (+ `m_device`); MtlBufferSlice gains `subSlice()`/`mapPtr()`;
  MtlResource gains `isInUse()`; MtlImageViewKey gains `viewType`/`packedSwizzle`/`packSwizzle()`;
  new MtlTexelBufferViewKey.
- **mtl_types.h**: MtlColor4→VkClearColorValue-shaped union (`float32`/`uint32`/`r,g,b,a`);
  MtlExtent3D/MtlSize interop ctors + `==`/`!=`; MtlOrigin/MtlExtent2D `==`; MtlFormatInfo gains
  `planes[]`+MtlFormatFlagMultiPlane; the Mtl{Rasterizer,Multisample,Stencil,DepthStencil,Blend}State
  fluent setter/getter API + MtlRasterizerState.{flatShading,sampleCount} + MtlDepthStencilState.depthTest.
- **mtl_device.h**: MtlInstance gains `config()`/`setConfig()`; MtlTexture::createView +
  MtlStagingBuffer(device,size) out-of-line defs.
- **mtl_context.h**: `using MtlSignal = sync::Signal` (so Rc<sync::Fence> fences feed ctx->signal);
  MtlLatencyTracker::needsAutoMarkers().
- **NEW util/util_image_math.h**: dxvk::util:: block/mip geometry helpers (computeBlockCount/Offset/
  Extent, computeMipLevelExtent/Offset, snapExtent3D, formatBlockSize, packImageData) + PHASE-E
  encodeClearBlockValue. ZERO Vulkan.
- **d3d9_fixed_function.h**: D3D9FFShaderModuleSet::GetShader/m_vs/m_fs retyped
  ID3D9MetalShaderBackend→MtlShaderFunction (align with the bindShader seam + D3D9CommonShader).

### PHASE-C9-CHECKPOINT — remaining to LINK
Build GREEN at 59 CORE TUs, gate PASS. build-driver.sh CORE list adds device.cpp unconditionally +
`[ -f ]`-guards swapchain/interface/main so it stays green until they land. d3d9.dll link frontier
(objdump of the attempt): **20 unexported def entries + 39 undefined refs**, all now concentrated in:
1. **d3d9_swapchain.cpp** — REWRITE onto MtlPresenter (header d3d9_swapchain.h fully ported; ~1500-line
   body remains — a device.cpp-scale port). Resolves all D3D9SwapChainEx::* refs.
2. **d3d9_interface.cpp** — D3D9InterfaceEx (ValidatePresentationParameters{,Ex}, adapter enum, device
   create) + the D3D9Options ctor consumer. Medium.
3. **d3d9_main.cpp** — the 20 d3d9.def exports (Direct3DCreate9{,Ex}, D3D9on12, D3DPERF_*, DebugSet*,
   DXVK_*RegisterAnnotation, PSGP*); instantiates D3D9InterfaceEx — do LAST.
4. **Seam-stub gaps** (checkpoint list, minimal+logged): D3D9GlobalAnnotationList::s_instance,
   logQueryInterfaceError/operator<<(GUID) (com_guid.cpp firewall), MemoryFilePool::* (util_unmap.cpp),
   D3D9State<> ctor/dtor + D3D9ShaderModuleSet::GetShaderModule + DxvkD3D8Bridge (their .cpp into CORE).
   `_imp__MTLDevice_*` winemetal imports come from driver/backend (concurrent agent) — link against
   the mtl_*_stub seam only if absent.
