# 02 — Type & File Map: extracting the D3D9 state tracker from DXVK 3.0

Source audited (read-only): `v2/DXVK-3.0-uptsream/src/d3d9/` — 81 files, **30,639 lines**.
Goal: keep the D3D9 state machine + resource semantics verbatim; replace every
Vulkan/`dxvk::` dependency with Metal-native equivalents (winemetal ABI: `WMTPixelFormat`, etc.).

Legend:
- **KEEP** — pure D3D9 semantics, zero or cosmetic Vk touch. Copy as-is (maybe one enum retype).
- **ADAPT** — real D3D9 logic wrapped around Vk enums / `dxvk::` handles. Keep the body, retarget the types.
- **REWRITE** — Vulkan-bound backend plumbing; the D3D9 surface is thin, the guts get replaced.

---

## 1. File-by-file keep/adapt/drop table (`src/d3d9/`)

### KEEP verbatim (≈ 4,167 lines)

| File | Lines | Why pure |
|---|---|---|
| d3d9_caps.h | 37 | `caps::` constants (MaxSubresources, MaxSimultaneousTextures…). |
| d3d9_constant_layout.h | 30 | float/int/bool constant counts. |
| d3d9_constant_set.h | 42 | CPU constant arrays (`D3D9ShaderConstantsVSSoftware` etc.). |
| d3d9_names.cpp / .h | 237 / 8 | enum→string debug tables. |
| d3d9_resource.h | 105 | COM refcount base (`ComObject`). |
| d3d9_device_child.h | 74 | device-child COM base. |
| d3d9_stateblock.cpp / .h | 588 / 406 | **State capture/apply — 0 Vk, 0 dxvk.** The heart of the tracker. |
| d3d9_shader_validator.cpp / .h | 189 / 100 | DXSO bytecode validation — 0 Vk, 0 dxvk. |
| d3d9_multithread.cpp / .h | 15 / 101 | CS mutex wrapper (1 `sync::` ref). |
| d3d9_cursor.cpp / .h | 143 / 84 | Win32 cursor. |
| d3d9_monitor.cpp / .h | 81 / 70 | wsi/win32 monitor. |
| d3d9_window.cpp / .h | 184 / 38 | wsi window (1 `DxvkWindowState`). |
| d3d9_vertex_declaration.cpp / .h | 381 / 106 | FVF/decl decode. Body is pure D3D9; only the *output* struct is `Dxvk*Vertex*` — retype output, keep logic. |
| d3d9_options.cpp / .h | 115 / 177 | Config parse. 1 param `Rc<DxvkDevice>` (l.16) → drop/replace with device caps struct. |
| d3d9_include.h | 110 | includes only. |

**Near-pure, listed KEEP with a note:**
- **d3d9_state.h (721)** — the render/sampler/texture-stage state struct: the D3D9 state machine itself. Only Vk leak is 4 `VkShaderStageFlags` constants (l.38/51/65/75) and `setAlphaCompareOp(VkCompareOp)` (l.170). Retype those two enums; everything else copies. **This is the single most important file to preserve.**
- **d3d9_state.cpp (25)** — trivial.

### ADAPT (≈ 21,894 lines — the bulk of the work)

| File | Lines | Vk / dxvk touch → what to retarget |
|---|---|---|
| d3d9_util.h / .cpp | 352 / 358 | **Enum decoders** `Decode{BlendFactor,BlendOp,Filter,MipFilter,AddressMode,CompareOp,StencilOp,CullMode,FillMode,IndexType,Decltype}` (util.h l.164–199) return `Vk*`. Swap each return type to the `MTL*` enum; switch bodies stay 1:1. Also `GetImageUsageFlags`, `GetPackedDepthStencilFormat`, `GetDepthBufferRValue(VkFormat…)`. |
| d3d9_format.h / .cpp | 315 / 656 | `D3D9Format` enum (l.13–129) + all `Is*Format` predicates (l.249–313) are **pure KEEP**. `D3D9_VK_FORMAT_MAPPING` (l.167–182: `VkFormat`×2, `VkImageAspectFlags`, `VkComponentMapping`), `D3D9_CONVERSION_FORMAT_INFO` (l.150), `D3D9VkFormatTable` + `CheckImageFormatSupport(VkFormat, VkFormatFeatureFlags2)` → become `D3D9MtlFormatTable` over `WMTPixelFormat`. |
| d3d9_format_helpers.cpp / .h | 186 / 57 | YUV/palette conversion dispatch; drives `DxvkContext` + `VkFormat`. Retarget to Metal blit/compute. |
| d3d9_common_texture.cpp / .h | 815 / 566 | See §4. Resource anatomy — biggest single ADAPT. |
| d3d9_common_buffer.cpp / .h | 175 / 249 | See §4. `DxvkBufferSlice` Lock/Discard model → Metal ring. |
| d3d9_subresource.h | 152 | RTV/DSV view getters build `Rc<DxvkImageView>` with `VkImageUsageFlags`/`VkImageLayout` (l.79–148) → Metal texture-view handle keyed by (format,usage). |
| d3d9_surface.cpp / .h | 268 / 73 | Thin over common_texture; `GetSurfaceExtent()`→`VkExtent2D` (l.56). |
| d3d9_volume.cpp / .h | 153 / 39 | Thin over common_texture (3D). |
| d3d9_texture.cpp / .h | 308 / 256 | 2D/Cube/Vol texture COM wrappers; hold views. |
| d3d9_buffer.cpp / .h | 123 / 98 | VB/IB COM wrappers over common_buffer. |
| d3d9_constant_buffer.cpp / .h | 267 / 131 | Shader-constant UBO: `Rc<DxvkBuffer>` cpu/gpu, `Rc<DxvkResourceAllocation>` slice, `VkBufferUsageFlags` (buffer.h l.86–120) → Metal buffer + ring. |
| d3d9_constant_copy.cpp / .h | 400 / 350 | SSE constant packing; mostly CPU. 3–4 dxvk refs (context flags) → retarget. |
| d3d9_device.cpp / .h | 9,288 / 1,718 | **The driver call site.** 156 Vk + 193 dxvk refs in .cpp (89× `DxvkContext`). Every draw/clear/copy/lock funnels through `DxvkContext`→ retarget to the Metal backend context. State-tracking half (dirty bits, bindings bookkeeping) is D3D9 and stays. Largest ADAPT surface by far. |
| d3d9_adapter.cpp / .h | 1,112 / 176 | Caps/format enumeration from `Rc<DxvkAdapter>`, `DxvkGpuVendor`, `VkPhysicalDevice` → Metal device caps query. D3D9 caps *values* stay. |
| d3d9_initializer.cpp / .h | 190 / 95 | Initial resource clear/upload via `DxvkContext`/staging → Metal upload. |
| d3d9_interface.cpp / .h | 559 / 194 | `IDirect3D9` factory; holds `Rc<DxvkInstance>` → Metal instance/device factory. |
| d3d9_query.cpp / .h | 330 / 90 | Occlusion/event/timestamp; `DxvkQuery`/`DxvkGpuQueryStatus` → Metal counters (already done in v2). |
| d3d9_swvp_emu.cpp / .h | 461 / 64 | SW vertex-processing compute emu (`DxvkShader`/`DxvkContext`) → Metal compute. |
| d3d9_shader_analysis.cpp / .h | 354 / 276 | DXSO bytecode analysis (sampler/RT masks). Only Vk leak = `VkImageViewType m_imageViewTypes[16]` (analysis.h l.210/264). Body pure; retype the enum. |
| d3d9_main.cpp | 120 | DLL entry; 17 dxvk refs (instance bootstrap) → Metal init. |
| d3d9_bridge.cpp / .h | 126 / 127 | D3D8→D3D9 bridge iface (`DxvkD3D8Bridge`). Retype the bridge interface. |
| d3d9_annotation.cpp / .h | 177 / 90 | D3DPERF debug labels via `DxvkContext` debug markers → Metal `pushDebugGroup` (or DROP). |

### REWRITE / DROP (≈ 4,578 lines)

| File | Lines | Verdict |
|---|---|---|
| d3d9_swapchain.cpp / .h | 1,494 / 258 | **REWRITE.** `Presenter`, `DxvkSwapchainBlitter`, `VkColorSpaceKHR`, `VkHdrMetadataEXT`, `VkSurfaceFormatKHR` (swapchain.h l.36–211) → `CAMetalLayer` + drawable present. Keep only D3D9 present-params/reset bookkeeping. |
| d3d9_shader.cpp / .h | 1,224 / 272 | **REWRITE.** Holds compiled `DxvkShader`/`DxvkSpirvShader`/`DxvkIrShaderConverter`. DXSO→IR translation itself lives in `src/dxso` (outside this tree). This is the backend module wrapper → Metal shader/pipeline object. |
| d3d9_interop.cpp / .h | 399 / 164 | **REWRITE.** `ID3D9VkInterop*` exposes raw `VkImage`/`VkDevice`/`VkInstance` → Metal interop or drop. |
| d3d9_interfaces.h | 304 | **REWRITE.** Public Vk-interop interface decls (`VkImage`, `VkImageLayout`, `VkFormat` in ABI). |
| d3d9_hud.cpp / .h | 94 / 70 | **REWRITE/DROP.** DXVK HUD (`hud::HudItem`). |
| d3d9_fixed_function.cpp / .h | 149 / 81 | **REWRITE.** FF shader-key + SPIR-V gen glue → Metal FF path (v2 generates these). |
| d3d9_on_12.cpp / .h | 40 / 29 | **DROP.** D3D9On12 interop, N/A on Metal. |

---

## 2. Type-leak inventory

### Vk* types reaching `src/d3d9/` (top by frequency, full set in audit)

| Vk type | count | role in d3d9 | Metal-first replacement |
|---|---|---|---|
| `VkExtent3D` / `VkExtent2D` / `VkOffset3D` / `VkOffset2D` | 57/15/53/6 | mip extents, copy/blit boxes, viewport/scissor | plain `struct { uint32_t w,h,d; }` / `MTLSize`, `MTLOrigin`. Own POD — keeps `util::computeMipLevelExtent`. |
| `VkDeviceSize` | 51 | buffer offsets/sizes | `uint64_t` typedef. |
| `VkFormat` | 24 | format mapping tables, view keys | `WMTPixelFormat` (winemetal) via our `D3D9Format→WMTPixelFormat` table. |
| `VkImageLayout` | 20 | RT/DSV/sample layout selection | **delete** — Metal has no explicit image layouts; fold into load/store + usage. |
| `VkImageSubresource(Layers/Range)` | 11/16/5 | subresource addressing for copies | own `{ mip, layer, aspect }` POD; aspect→plane index. |
| `VkImageUsageFlags` / `VkBufferUsageFlags` | 11/6 | resource create-info | `MTLTextureUsage` / `MTLResourceUsage`. |
| `VkComponentMapping` / `VkImageAspectFlags` | 1/6 | swizzle + depth/stencil aspect | `MTLTextureSwizzleChannels` / plane index enum. |
| `VkShaderStageFlags`, `VkCompareOp`, `VkBlend*`, `VkStencilOp`, `VkCullMode`, `VkPolygonMode`, `VkSamplerAddressMode`, `VkFilter`, `VkIndexType` | many | enum decoders in d3d9_util | 1:1 `MTL*` enums (`MTLCompareFunction`, `MTLBlendFactor`, `MTLBlendOperation`, `MTLStencilOperation`, `MTLCullMode`, `MTLTriangleFillMode`, `MTLSamplerAddressMode`, `MTLSamplerMinMagFilter`, `MTLIndexType`). Switch bodies unchanged. |
| `VkColorSpaceKHR`, `VkHdrMetadataEXT`, `VkSurfaceFormatKHR`, `VkSurfaceKHR`, `VkDrawIndirect*`, `VkPhysicalDevice`, `VkInstance`, `VkDevice`, `VkQueue` | 7/4/3/1/13/6/6/3/3 | swapchain, interop, indirect draws, adapter | REWRITE-scoped (swapchain/interop/adapter). |

`#include <vulkan/vulkan.h>` reaches d3d9 transitively via **`dxvk_include.h`** (pulled by `dxvk_device.h`, itself included by common_texture.h, common_buffer.h, util.h, format.h, device.h…). Also directly in **d3d9_shader_analysis.h:13**. Cutting `dxvk_device.h`/`dxvk_format.h`/`dxvk_adapter.h` from the four core headers severs the bulk of the leak.

### dxvk:: types reaching `src/d3d9/`

| dxvk type | count | role | Metal replacement |
|---|---|---|---|
| `DxvkContext` | 89 | the immediate-mode command recorder (draw/clear/copy/bind) | our Metal backend context object. Confined to device.cpp + a few. |
| `DxvkBufferSlice` | 34 | (offset,length) view into a buffer — underpins Lock/Discard | our `MtlBufferSlice { handle, offset, length }`; see §4 ring. |
| `DxvkFormatInfo` | 32 | block size / aspect / component metadata | our `MtlFormatInfo` table keyed by `WMTPixelFormat` (block size, plane count, texel size). |
| `Rc<DxvkImage>` / `DxvkImage` | 29 | GPU texture object | `MtlTextureHandle { id<MTLTexture>, uint32_t generation }`. |
| `Rc<DxvkBuffer>` / `DxvkBuffer` | 21 | GPU buffer object | `MtlBufferHandle { id<MTLBuffer>, uint32_t generation }`. |
| `Rc<DxvkImageView>` / `DxvkImageView` / `DxvkImageViewKey` | 18/14 | keyed texture views (color/srgb, rtv, dsv, sample) | Metal `newTextureViewWithPixelFormat:…`, cached in a view pool keyed by `{format, swizzle, mipRange, layerRange, usage}` (mirrors `DxvkImageViewKey`). |
| `DxvkImageCreateInfo` / `DxvkBufferCreateInfo` | 10/5 | resource creation params | `MTLTextureDescriptor` / `MTLBuffer` alloc params. |
| `Rc<DxvkResourceAllocation>` | 6 | the *renamed* backing slice from Discard (`allocateStorage`) | new suballocation from our per-resource ring; carries generation. |
| `DxvkCsThread` / `DxvkCsChunk*` / `DxvkCommandList` / `DxvkCmdBuffer` | 9/… | deferred command submission thread | our CS thread + Metal command-buffer batching. |
| `DxvkGpuVendor` / `DxvkAdapter` / `DxvkDevice` / `DxvkInstance` | 25/12/16/5 | caps + device/instance | Metal device caps struct + `MTLDevice`. |
| `DxvkShader*` / `DxvkSpirvShader` / `DxvkIrShader*` / `DxvkPipelineLayout` / `DxvkShaderBinding` | 14/… | compiled shader + binding layout | Metal shader/PSO + argument-buffer layout (REWRITE-scoped: shader.cpp). |
| `DxvkContextFlags`/`DxvkContextFlag` | (in device.h/constant_copy) | dirty-state bitset the tracker toggles | keep the *bitset pattern*; rename to our `D3D9ContextFlags`. This is state-tracking, not Vulkan — do not drop, just rename. |
| `DxvkD3D8Bridge` / `DxvkD3D8InterfaceBridge` | 14/13 | D3D8 interop iface | retype interface (bridge.*). |

### Special-attention findings

**Format handling (d3d9_format.\*)** — *highly reusable.* The `D3D9Format` enum (129 lines) and every `Is{Depth,DXT,FourCC,Vendor,…}Format` predicate (l.249–313) are pure D3D9 and copy verbatim. Only the *mapping payload* is Vulkan: `D3D9_VK_FORMAT_MAPPING` holds `VkFormat FormatColor/FormatSrgb`, `VkImageAspectFlags Aspect`, `VkComponentMapping Swizzle`. Replace with `WMTPixelFormat color/srgb`, a plane/aspect enum, and `MTLTextureSwizzleChannels`. The table-*shape* (per-format row, color+srgb pair, conversion-format id) is unchanged — this is a mechanical `VkFormat → WMTPixelFormat` retype of ~30 rows in d3d9_format.cpp, not a redesign. `IsValid()` (l.181) flips from `!= VK_FORMAT_UNDEFINED` to `!= WMTPixelFormatInvalid`.

**Resource-slice / renaming model (`DxvkBufferSlice` underpins Lock/Discard)** — d3d9_common_buffer.h shows the shape: `GetBufferSlice<Type>(offset,length)` returns a `DxvkBufferSlice(buffer, offset, len)` (l.117–134); `Lock` with `D3DLOCK_DISCARD` calls `DiscardMapSlice()` → `GetMapBuffer()->allocateStorage()` returning a fresh `Rc<DxvkResourceAllocation>` (l.136–139) while the old contents keep draining on the GPU. **Metal-native shape:** a per-resource *suballocated ring* — Discard bumps to the next sub-region of a growable `MTLBuffer` (or grabs a fresh block from a shared pool) and stamps a **generation counter**; in-flight draws reference the old (handle,generation), new Locks write the new one. `m_allocation` (buffer.h l.240) becomes `MtlSuballocation { MtlBufferHandle, offset, size, generation }`. Same for texture staging (`CreateBuffer`/`GetBufferSlice(Subresource)` in common_texture).

**Image-view keying** — `DxvkImageViewKey` (14 refs) + `D3D9ColorView{ Color, Srgb }` (common_texture.h l.56–67) + subresource RTV/DSV getters (subresource.h l.79–148) show views are lazily created and cached per (srgb, usage, writable) combo. Keep the exact caching structure; back it with a Metal texture-view pool keyed by `{WMTPixelFormat, swizzle, mip range, layer range, usage}`.

---

## 3. Support-library verdicts

**src/util — KEEP (clean, no Vulkan).** Exact headers pulled by d3d9:
`util_bit.h, util_math.h, util_win32_compat.h, util_vector.h, util_unmap.h, util_small_vector.h, util_string.h, util_matrix.h, util_singleton.h, util_shared_res.h, util_ratio.h, util_misc.h, util_luid.h, util_lru.h, util_likely.h, util_gdi.h, util_flush.h, util_flags.h, util_error.h, util_env.h, util_enum.h, thread.h, sync/sync_signal.h, sync/sync_recursive.h, rc/util_rc.h, rc/util_rc_ptr.h, log/log.h, log/log_debug.h, config/config.h, com/{com_private_data,com_pointer,com_object,com_guid}.h`.
These are pure C++/Win32/COM plumbing (bitsets, matrices, `Rc<>` refcount, logging, config, COM). `Rc<>`/`RcObject` from `rc/` is *the* smart pointer the whole tracker uses — keep it; it is not Vulkan.

**src/wsi — KEEP.** d3d9 pulls only `wsi/wsi_monitor.h` and `wsi/wsi_window.h`. Swapchain/monitor/window pull display-mode enumeration, window geometry, and `DxvkWindowState` (cursor/style save-restore) from it. Platform-abstracted already; the macOS/Wine backend supplies the impl. Swapchain itself is REWRITE but keeps calling wsi.

**Transitive Vulkan entry points to sever:** `../dxvk/dxvk_device.h` (6 includes), `dxvk_adapter.h` (3), `dxvk_instance.h` (3), `dxvk_format.h`, `dxvk_buffer.h`, `dxvk_cs.h` (2), `dxvk_context.h` (2), `dxvk_shader*.h`, `dxvk_swapchain_blitter.h`, `dxvk_staging.h`, `dxvk_annotation.h`, `hud/dxvk_hud*.h`. **No `src/spirv` include appears in d3d9** (SPIR-V lives behind dxso/dxvk_shader). Replacing the four core dxvk headers (device/adapter/format/cs) on common_texture/common_buffer/util/format kills most of the leak.

---

## 4. Resource-object anatomy

### D3D9CommonTexture (common_texture.h/.cpp, 566/815)

**KEEP (D3D9 semantics):** `D3D9_COMMON_TEXTURE_DESC` minus `ImageUsage` (l.36–54); mip/face math `CalcSubresource`/`CountSubresources`/`GetExtentMip`/`ExposedMipLevels` (l.252, 120, 332, 398); pool/usage predicates `IsManaged/IsRenderTarget/IsDepthStencil/IsAutomaticMip/TextureUsesImage` (l.202–303); dirty boxes `AddDirtyBox`/`ClearDirtyBoxes`/`GetDirtyBox` (l.406–442); the bitsets `m_locked`/`m_needsReadback`/`m_needsUpload` + `m_dirtyBoxes[6]` + `m_seqs[]` (l.529–541); shadow/fetch4/upgrade flags; map-mode enum (l.23–28).

**REPLACE (Vulkan plumbing):**
- `Rc<DxvkImage> m_image / m_resolveImage` (l.504–505) → `MtlTextureHandle m_texture / m_resolveTexture`.
- `Rc<DxvkBuffer> m_buffer` staging (l.506) + `CreateBuffer`/`GetBufferSlice`/`GetData` (l.162–166, 493) → Metal staging buffer + suballocation.
- `D3D9_VK_FORMAT_MAPPING m_mapping` (l.517) → `D3D9_MTL_FORMAT_MAPPING`.
- `D3D9ColorView m_sampleView` (Rc<DxvkImageView> pair, l.527) + `CreateView(…, VkImageUsageFlags, VkImageLayout, Srgb)` (l.371–376) → Metal view-pool handles.
- `CreatePrimaryImage`/`CreateResolveImage` (l.545–547) build `DxvkImageCreateInfo` → build `MTLTextureDescriptor`.
- `CheckImageSupport(DxvkImageCreateInfo*, VkImageTiling)` / `OptimizeLayout(VkImageUsageFlags)` (l.553–560) → **delete** (`OptimizeLayout` — no Metal layouts) / retarget to Metal format-support query.
- `GetSubresourceFromIndex(VkImageAspectFlags,…)`→VkImageSubresource / `GetImageType…`/`GetImageViewType…` (l.177, 444–449) → our subresource POD + Metal texture-type enum.

**Replacement handle sketch:** `struct MtlTextureHandle { id<MTLTexture> tex; uint32_t generation; };` — texture object holds the primary + resolve handles, a `D3D9_MTL_FORMAT_MAPPING`, the staging buffer suballocation, and all the KEEP bitsets/boxes/seqs untouched.

### D3D9CommonBuffer (common_buffer.h/.cpp, 249/175)

**KEEP:** `D3D9_BUFFER_DESC` (l.22–29); `D3D9Range` dirty-range (l.40–71); map-mode enum, lock count, map flags, seq, `NeedsUpload`/`DoPerDrawUpload`/`NeedsReadback` logic (l.179–211).
**REPLACE:** `Rc<DxvkBuffer> m_buffer/m_stagingBuffer` (l.237–238) → `MtlBufferHandle`; `Rc<DxvkResourceAllocation> m_allocation` + `DiscardMapSlice`/`GetMappedSlice` (l.136–143, 240) → ring suballocation w/ generation; `GetBufferSlice<Type>()`→`DxvkBufferSlice` (l.116–134) → `MtlBufferSlice`; `BufferSliceAlignment`/`VkDeviceSize` → `uint64_t`.
**Handle sketch:** `struct MtlBufferHandle { id<MTLBuffer> buf; uint32_t generation; };` + `struct MtlSuballocation { MtlBufferHandle backing; uint64_t offset, size; uint32_t generation; };` — Discard bumps generation and re-suballocates; in-flight draws keep the old generation.

### D3D9Surface / D3D9Volume / D3D9Subresource (surface.h 73, volume.h 39, subresource.h 152)

Thin COM wrappers over CommonTexture. **KEEP** the subresource-index/face/mip accessors; **REPLACE** the lazily-cached `Rc<DxvkImageView>` RTV (`GetRenderTargetView`, subresource.h l.79) and DSV (`GetDepthStencilView`, l.94, drops `VkImageLayout`) with Metal view-pool handles. `GetSurfaceExtent()→VkExtent2D` (surface.h l.56) → own extent POD.

### D3D9SwapChainEx (swapchain.h/.cpp, 258/1494) — **REWRITE**

D3D9 semantics to keep: present params, back-buffer array, reset/format-of-backbuffer logic, frame-latency fence bookkeeping. Everything else — `Rc<Presenter>`, `Rc<DxvkSwapchainBlitter>`, `VkColorSpaceKHR m_colorspace`, `VkHdrMetadataEXT`, `VkSurfaceFormatKHR GetSurfaceFormat()` (l.36–211) — is replaced by a `CAMetalLayer` + drawable-present path.

---

## Summary (10 lines)

1. Scope: `src/d3d9/` = 81 files, **30,639 lines**; goal is a zero-Vulkan extraction of the D3D9 state tracker.
2. **KEEP verbatim ≈ 4,167 lines** — stateblock, shader_validator, names, caps, state.h (the state machine), vertex-decl, cursor/monitor/window, options.
3. **ADAPT ≈ 21,894 lines** — device.cpp/.h (11k alone), common_texture/buffer, format, util enum-decoders, adapter, texture/buffer/surface/volume, query, constant buffers, swvp.
4. **REWRITE/DROP ≈ 4,578 lines** — swapchain, shader.cpp, interop/interfaces, fixed_function, hud, on_12. (Sum = 30,639, exact.)
5. Vulkan reaches d3d9 mainly transitively via `dxvk_device.h`→`dxvk_include.h`→`vulkan.h`; cutting 4 core dxvk headers off common_texture/buffer/util/format severs most of it. One direct `#include <vulkan/vulkan.h>` in shader_analysis.h:13.
6. The enum decoders in d3d9_util.h (l.164–199) are a 1:1 `Vk*`→`MTL*` retype — switch bodies unchanged; likewise `D3D9Format`+predicates in format.h are pure KEEP, only the mapping payload retypes `VkFormat→WMTPixelFormat`.
7. Lock/Discard rests on `DxvkBufferSlice` + `DxvkResourceAllocation.allocateStorage()`; Metal shape = per-resource **suballocated ring + generation counter** (`MtlSuballocation`).
8. Resources become `MtlTextureHandle{tex,gen}` / `MtlBufferHandle{buf,gen}`; image views become a Metal view pool keyed like `DxvkImageViewKey`; `VkImageLayout` is deleted outright (no Metal analog).
9. Support libs: **src/util KEEP** (30 headers, incl. the `Rc<>` used everywhere) and **src/wsi KEEP** (monitor+window); neither is Vulkan-bound. No src/spirv reaches d3d9.
10. Practical order: extract KEEP + state.h first, retype format+util enums, then grind common_texture/buffer and device.cpp against the Metal backend; swapchain/shader/interop are separate REWRITE tracks.
