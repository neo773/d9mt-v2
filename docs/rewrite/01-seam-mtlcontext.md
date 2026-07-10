# 01 — The Frontend→Backend Seam: `DxvkContext` → `MtlContext`

Reference tree (read-only): `v2/DXVK-3.0-uptsream/`. All `file:line` cites are into that tree.

**Thesis.** DXVK's D3D9 frontend never touches Vulkan directly. It records *deferred
lambdas* into a CS chunk (`EmitCs`), and a worker thread replays them against a single
`DxvkContext*`. That `DxvkContext` surface — ~40 methods reachable from `src/d3d9/` — **is
the entire backend contract.** Replace `DxvkContext` with an `MtlContext` of the same
shape and the frontend compiles unchanged. This doc inventories that surface, assesses the
CS plumbing, enumerates the implicit contracts, and sketches the v0 `MtlContext` header.

The seam is literally one type in one place: every recorded lambda takes `DxvkContext* ctx`
(`dxvk_cs.h:54`, `DxvkCsCmd::exec(DxvkContext*)`). Swap that parameter type and the arena,
queue, and worker thread come along for free (see §2).

Metal-4 concepts that shape the `MtlContext` interface are verified in the companion doc
`00-metal4-reference.md`; the load-bearing deltas are threaded through §3–§4 and summarized
in §4.1. Key ones up front: (a) depth/stencil attachment **formats are not part of the
render-PSO descriptor** — exclude them from the PSO key; (b) binding is a `MTL4ArgumentTable`
**snapshot-at-dispatch**, exactly the per-draw ring we already hand-build; (c) resources are
**untracked by default** — hazards are explicit producer/consumer barriers; (d) command
buffers are **explicit begin/end + batched `commit([...])`** with render-pass
**suspend/resume across buffers**; (e) PSOs take **library function descriptors** (our AIR
metallibs feed these directly) and compile **async via `MTL4Compiler`**.

---

## 1. EmitCs call-site inventory (the contract)

97 emit sites across `src/d3d9/` (`d3d9_device.cpp` 68, `d3d9_swapchain.cpp` 7,
`d3d9_constant_buffer.cpp` 4, `d3d9_initializer.cpp` 4, `d3d9_annotation.cpp` 3,
`d3d9_interop.cpp` 1, plus the `EmitCs`/`EmitCsCmd`/`InjectCs` template defs in
`d3d9_device.h` / `d3d9_initializer.h`). Below they are grouped by D3D9 operation. Each
entry lists the **DxvkContext method + as-used signature**, the **frontend-relied semantics**,
and a **proposed Metal-first signature** (Metal-4 concepts, zero `Vk*`).

Three emit *shapes* exist and every Metal equivalent must preserve them:

* `EmitCs([captures](DxvkContext* ctx){...})` — deferred lambda, most common.
* `EmitCsCmd<M>(type, count, lambda)` — lambda **plus** a mutable data block (`M[count]`)
  that the *caller keeps writing into after emit* (used for `DrawIndirectCommand`,
  vertex-element arrays, stream copies). `d3d9_device.h:1247`. The block lives in the chunk;
  the recording thread patches it up to the point the chunk is flushed.
* `InjectCs(lambda)` / `InjectCsChunk` — out-of-band chunk that runs on the CS thread but
  *outside* the ordered main timeline, optionally synchronous (`d3d9_device.h:1196`).

### 1a. Draws

| site | DxvkContext method (as used) | semantics frontend relies on | Metal-first equivalent |
|---|---|---|---|
| `d3d9_device.cpp:3044` DrawPrimitive | `EmitCsCmd<VkDrawIndirectCommand>(Draw,1,…)` → `ctx->draw(count, const VkDrawIndirectCommand*)` (`ctx.h:706`) | draw params written into a *mutable chunk block* after emit; flushes all dirty pipeline/vertex/RT/descriptor state lazily at draw time; ordered vs all prior emits | `draw(uint32_t count, const MtlDrawArgs* args)` — args = `{vertexCount,instanceCount,firstVertex,firstInstance}`. Resolves cached render PSO + argument tables, opens render encoder if none, issues `drawPrimitives`. |
| `d3d9_device.cpp:3095` DrawIndexedPrimitive | `EmitCsCmd<VkDrawIndexedIndirectCommand>` → `ctx->drawIndexed(count, const VkDrawIndexedIndirectCommand*)` (`ctx.h:749`) | as above + bound index buffer/type | `drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args)` → `drawIndexedPrimitives`. |
| `d3d9_device.cpp:3143` DrawPrimitiveUP | `ctx->bindVertexBuffer` ×2 + `ctx->draw` | up-front bind of a *transient* renamed vertex buffer, draw, then rebind previous binding — relies on invalidateBuffer ring (see 3c) | same `draw` + `bindVertexBuffer` on a ring-allocated slice. |
| `d3d9_device.cpp:3204` DrawIndexedPrimitiveUP | `bindVertexBuffer`+`bindIndexBuffer`+`drawIndexed`, then rebind both | transient VB+IB, draw, restore | same. |

The draw path is where **all deferred state resolves**. In DXVK `draw()` internally runs
`commitGraphicsState`: pipeline lookup, descriptor set build, dynamic state, RT (re)bind,
hazard barriers. MtlContext's `draw` must do the Metal-4 analogue: PSO memo lookup, arg-table
commit, viewport/scissor/depth-bias/blend-constant/stencil-ref application, residency-set
requestResidency, encoder (re)open, then the draw. Keep the memoized render-PSO lookup — see
memory note *d9mt-perf-optimizations*.

### 1b. Shaders & pipeline programmable state

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:3309` (SetStreamSourceFreq / swvp emu) `:7609` BindShader `:7624` BindFFUbershader | `ctx->bindShader<Stage>(Rc<DxvkShader>&&)` (`ctx.h:314`, templated on stage) | sets VS/PS (D3D9 has only these + FF ubershader); marks `GpDirtyPipeline*`; shader object lifetime handed to backend via move | `bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn)`. `MtlShaderFunction` wraps an MSL `MTLFunction` + reflection; no SPIR-V. Stage ∈ {Vertex, Fragment}. |
| `:9109` BindSpecConstants | `ctx->setSpecConstants(VK_PIPELINE_BIND_POINT_GRAPHICS, index, count, const void*)` (`ctx.h:1185`) | D3D9 packs render-state bits (alpha-test func, fog mode, …) as spec constants that key the PSO variant | `setFunctionConstants(MtlPipeline::Graphics, uint32_t index, uint32_t count, const void* data)` → MTLFunctionConstantValues; participates in PSO cache key. |
| `:7745` (BindInputLayout body) | `ctx->setInputLayout(attrCount, const DxvkVertexInput* attrs, bindCount, const DxvkVertexInput* binds)` (`ctx.h:1133`) | vertex declaration → attribute/binding arrays; part of PSO state | `setVertexLayout(uint32_t nAttr, const MtlVertexAttr*, uint32_t nLayout, const MtlVertexLayout*)` → MTLVertexDescriptor, folded into render-PSO key. |
| `:8496` ApplyPrimitiveType | `ctx->setInputAssemblyState(const DxvkInputAssemblyState&)` (`ctx.h:1122`) | topology + primitive-restart; PSO + encoder `drawPrimitives` type | `setPrimitiveTopology(MtlPrimitiveTopology)`. Note Metal splits class (PSO) vs type (draw). |
| `d3d9_device.cpp:83` (device ctor) | `ctx->beginRecording(...)` + `ctx->setLogicOpState(const DxvkLogicOpState&)` (`ctx.h:1164`) | one-time initial recording bootstrap + logic-op default | `beginRecording()` + `setLogicOpState(MtlLogicOpState)` (Metal: emulated; D3D9 rarely uses). |

### 1c. Fixed-function / dynamic render state setters

All are pure state, folded into either the render PSO key or Metal dynamic encoder state. Each
sets a dirty flag; nothing touches the GPU until the next `draw`.

| site | method (as used, `ctx.h`) | Metal-first |
|---|---|---|
| `:6881` (viewport) | `setViewports(uint32_t, const DxvkViewport*)` `1066` | `setViewports(uint32_t n, const MtlViewport*)` (MTLViewport+MTLScissorRect; **Retina scale open Q**, see memory *v2 GPU capture*). |
| `:6935` BindMultiSampleState | `setMultisampleState(const DxvkMultisampleState&)` `1150` | `setMultisampleState(const MtlMultisampleState&)` → sampleCount/alphaToCoverage, PSO key. |
| `:6980` BindBlendState | `setBlendMode(uint32_t attachment, const DxvkBlendMode&)` `1173` | `setBlendMode(uint32_t rt, const MtlBlendMode&)` → per-color-attachment blend desc, PSO key. |
| `:7021` BindBlendFactor | `setBlendConstants(DxvkBlendConstants)` `1078` | `setBlendColor(MtlColor4)` — dynamic (`setBlendColorRed:…`). |
| `:7068` BindDepthStencilState | `setDepthStencilState(const DxvkDepthStencilState&)` `1157` | `setDepthStencilState(const MtlDepthStencilState&)` → MTLDepthStencilState object (cached, **not** PSO key). |
| `:7178` BindDepthStencilReference | `setStencilReference(uint32_t)` `1115` | `setStencilReference(uint32_t)` — dynamic encoder state. |
| `:7093` BindRasterizerState | `setRasterizerState(const DxvkRasterizerState&)` `1143` | `setRasterizerState(const MtlRasterizerState&)` → cull/fill/front-face + depth-clip; mostly dynamic. |
| `:7114` BindDepthBias | `setDepthBias(DxvkDepthBias)` `1088` | `setDepthBias(MtlDepthBias)` → `setDepthBias:slopeScale:clamp:` (dynamic). |
| `:112` (ctor) | `setDepthBiasRepresentation(DxvkDepthBiasRepresentation)` `1096` | drop / no-op on Metal (float depth-bias only); mark **open Q**. |
| `:7504` (SetDepthBounds) | `setDepthBounds(DxvkDepthBounds)` `1106` | no native Metal depth-bounds; emulate or ignore. **open Q**. |

### 1d. Resource binding (textures / samplers / constant buffers)

**Binding model.** All binds below target a **slot-based argument table with
snapshot-at-dispatch semantics** — the Metal-4 `MTL4ArgumentTable` captures a zero-overhead
snapshot of every bound slot at each draw (`00-metal4-reference.md` §MTL4ArgumentTable). This is
*exactly* the per-draw ring we already hand-build (the shared-AB-overwrite black-screen class,
memory *v2 blackscreen eliminations*). On the Metal-3 winemetal ABI the impl emulates the
snapshot by copying the live slot table into a per-draw ring argument buffer before each draw;
on the Metal-4 bridge it is native. The `MtlContext` binding API is therefore specified as pure
slot writes (`bind*(stageMask, slot, …)`) with no encoder call implied until `draw`.

| site | method (as used) | semantics | Metal-first (slot/arg-table model) |
|---|---|---|---|
| `:7270` BindTexture, `:7281` UnbindTextures, `:8812` (interop) | `bindResourceImageView(VkShaderStageFlags stages, uint32_t slot, Rc<DxvkImageView>&&)` (`ctx.h:256`) | slot-indexed SRV bind; redundancy-guarded; move-owns the view; marks descriptor set dirty per-stage | `bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&&)` → writes a slot in the stage argument table; residency-set add. |
| (buffer SRV / R2VB path) | `bindResourceBufferView(VkShaderStageFlags, uint32_t, Rc<DxvkBufferView>&&)` (`ctx.h:275`) | typed-buffer SRV in same slot space | `bindBufferView(MtlStageMask, uint32_t slot, Rc<MtlTexelBufferView>&&)`. |
| `:7195` BindSampler (body) | `bindResourceSampler(VkShaderStageFlags, uint32_t, Rc<DxvkSampler>&&)` (`ctx.h:296`) | slot-indexed sampler; deduped | `bindSampler(MtlStageMask, uint32_t slot, Rc<MtlSamplerState>&&)` → MTLSamplerState in sampler arg table. |
| `d3d9_constant_buffer.cpp:40` Alloc, `:56` | `invalidateBuffer(...)` then `bindUniformBufferRange(VkShaderStageFlags, uint32_t slot, VkDeviceSize offset, VkDeviceSize length)` (`ctx.h:239`) | fast re-point of an already-bound UBO to a new sub-range within the *renamed* backing (ring); no rebind of the buffer object | `bindUniformBufferRange(MtlStageMask, uint32_t slot, uint64_t offset, uint64_t length)` — updates offset in arg table only. |
| `d3d9_constant_buffer.cpp:111` | `bindUniformBuffer(VkShaderStageFlags, uint32_t slot, DxvkBufferSlice&&)` (`ctx.h:224`) | full UBO (re)bind, move-owns slice | `bindUniformBuffer(MtlStageMask, uint32_t slot, MtlBufferSlice&&)`. |
| `:6656` UpdatePushDataBlock | `pushData(VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data)` (`ctx.h:958`, inline memcpy into `m_state.pc`) | small hot per-draw constants (push-constant block); memcpy now, uploaded at draw | `pushData(MtlStageMask, uint32_t offset, uint32_t size, const void* data)` → `setBytes`/inline arg-buffer region ≤ 4KB. Keep the bulk-copy optimization (see commit `perf(d3d9fe): bulk-copy push constant blocks`). |
| `:473` (SetIndices/streams reset), `:1863`, `:1870`, `:5799`, `:5819`, `:5833`, `:7762`, `:7773`, `:7789`, `:7805`, `:7812` | `bindVertexBuffer(uint32_t binding, DxvkBufferSlice&&, uint32_t stride)` (`ctx.h:362`), `bindVertexBufferRange(binding, offset, length, stride)` (`ctx.h:380`), `bindIndexBuffer(DxvkBufferSlice&&, VkIndexType)` (`ctx.h:187`) | slot-indexed VB with stride (stride is PSO/vertex-desc input on Metal!), range-only fast path, index buffer + type | `bindVertexBuffer(uint32_t binding, MtlBufferSlice&&, uint32_t stride)`, `bindVertexBufferRange(binding, offset, length, stride)`, `bindIndexBuffer(MtlBufferSlice&&, MtlIndexType)`. **Metal Q:** stride lives in MTLVertexDescriptor, so a stride change must invalidate the render PSO — track it as PSO-dirty, not just a `setVertexBuffer:offset:` update. |
| `d3d9_device.cpp:7648` BindInputLayout | `EmitCsCmd<D3DVERTEXELEMENT9>(...)` builds element array in-chunk then `ctx->setInputLayout` | mutable data block pattern | as 1b setVertexLayout, sourced from a chunk-resident element array. |

### 1e. Render-target / depth setup

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:6811` (SetRenderTarget/DepthStencil apply) | `bindRenderTargets(DxvkRenderTargets&&, VkImageAspectFlags feedbackLoop)` (`ctx.h:146`) | up to 4 color + depth; **only** flags dirty — the actual encoder/load-store is deferred to draw; feedback-loop aspect signals RT-as-texture hazard | `bindRenderTargets(MtlRenderTargets&&, MtlFeedbackLoopMask)`. A *pending* MTLRenderPassDescriptor; encoder opens lazily at next draw/clear. Load/store actions chosen then (see contract 3f). |
| `d3d9_device.cpp:5405` (StretchRect external), `d3d9_swapchain.cpp:880` PresentImage | `beginExternalRendering()` → `Rc<DxvkCommandList>` (`ctx.h:123`) | hands raw command list to HUD/present/interop; invalidates all tracked state | `beginExternalRendering() -> Rc<MtlCommandBuffer>`; must close the current encoder and mark all arg tables dirty. |
| `d3d9_device.cpp:6409` EmitFeedbackLoopBarriers | `emitGraphicsBarrier(srcStages, srcAccess, dstStages, dstAccess)` (`ctx.h:810`) | explicit RAW hazard when a bound RT is also sampled | `emitGraphicsBarrier(MtlStageMask src, MtlBarrierScope srcScope, MtlStageMask dst, MtlBarrierScope dstScope)` → `memoryBarrierWithScope:` inside the render encoder (or a tile barrier). See contract 3b. |

### 1f. Clears

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:1564`, `:1966` (Clear / ColorFill) | `clearRenderTarget(const Rc<DxvkImageView>&, VkImageAspectFlags clearAspects, VkClearValue, VkImageAspectFlags discardAspects)` (`ctx.h:464`) | clears active RT via load-op if possible; `discardAspects` = "don't-care" hint | `clearRenderTarget(const Rc<MtlTextureView>&, MtlAspectMask clear, MtlClearValue, MtlAspectMask discard)` → fold into next render-pass `loadAction=Clear`/`DontCare`; else a clear encoder. |
| `:1518`, `:1564`, `:1976` | `clearImageView(const Rc<DxvkImageView>&, VkOffset3D, VkExtent3D, VkImageAspectFlags, VkClearValue)` (`ctx.h:482`) | clears an arbitrary (non-bound) region; format-converting | `clearImageView(const Rc<MtlTextureView>&, MtlOrigin, MtlSize, MtlAspectMask, MtlClearValue)` → tiny clear render pass or compute clear. |
| `:1518`, `:1564`, `:6436` | `ensureImageCompatibility(const Rc<DxvkImage>&, const DxvkImageUsageInfo&) -> bool` (`ctx.h:945`) | may reallocate backing to add usage (e.g. RT-able) and copy — precedes clears/blits | `ensureTextureUsage(const Rc<MtlTexture>&, MtlTextureUsage) -> bool`. Metal usage is fixed at create; this reallocs+blits. See contract 3g. |

### 1g. Copies / blits / resolves / mips

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:1408`, `:8564` (StretchRect same-size) | `copyImage(dstImage, dstSubLayers, dstOffset, srcImage, srcSubLayers, srcOffset, extent)` (`ctx.h:560`) | exact subresource copy | `copyTexture(...)` → blit `copyFromTexture:...:toTexture:`. |
| `:3371`, `:5597` (ProcessVertices / buffer flush) | `copyBuffer(dst, dstOff, src, srcOff, numBytes)` (`ctx.h:498`) | linear buffer copy | `copyBuffer(...)` → blit `copyFromBuffer:...`. |
| `:1175`, `:5106`, `d3d9_swapchain.cpp:453` (GetRenderTargetData / readback) | `copyImageToBuffer(dstBuf, dstOff, rowAlign, sliceAlign, dstFormat, srcImage, srcSubLayers, srcOffset, srcExtent)` (`ctx.h:599`) | image→linear for CPU readback; row/slice alignment | `copyTextureToBuffer(...)` → blit `copyFromTexture:...:toBuffer:` with bytesPerRow/Image. Readback tool: memory *v2 occlusion/readback*. |
| `:5352` (UpdateSurface upload) | `copyBufferToImage(dstImage, dstSubLayers, dstOffset, dstExtent, srcBuf, srcOff, rowAlign, sliceAlign, srcFormat)` (`ctx.h:538`) | linear→image upload path | `copyBufferToTexture(...)`. |
| `:1398`, `:5083`, `:8576`, `d3d9_swapchain.cpp:313` (StretchRect MSAA / GetFrontBufferData) | `resolveImage(dst, src, const VkImageResolve&, format, VkResolveModeFlagBits mode, stencilMode)` (`ctx.h:985`) | MSAA resolve, color+optional stencil resolve mode | `resolveTexture(dst, src, MtlResolveRegion, MtlPixelFormat, MtlResolveMode color, MtlResolveMode stencil)`. Metal resolve is a store-action on the render pass — may need a dedicated resolve pass. **open Q** on partial-region resolve. |
| `:1448`, `d3d9_swapchain.cpp:431` (StretchRect scaled) | `blitImageView(const Rc<DxvkImageView>& dst, const VkOffset3D* dstOffsets, const Rc<DxvkImageView>& src, const VkOffset3D* srcOffsets, VkFilter)` (`ctx.h:416`) | scaled/filtered blit with per-corner offsets + swizzle | `blitTextureView(dst, const MtlOffset3D dstBox[2], src, const MtlOffset3D srcBox[2], MtlSamplerFilter)`. No native scaled blit in Metal → render-pass draw with a blit shader (MPS or custom). **Historically the biggest gap.** |
| `:5427` EmitGenerateMips | `generateMipmaps(const Rc<DxvkImageView>&, VkFilter)` (`ctx.h:844`) | full mip chain from level 0 | `generateMipmaps(const Rc<MtlTextureView>&, MtlSamplerFilter)` → `MTLBlitCommandEncoder generateMipmapsForTexture:` (linear only; non-linear → shader). |
| `d3d9_device.cpp:8606` TransformImage, `d3d9_interop.cpp:223` | `transformImage(const Rc<DxvkImage>&, const VkImageSubresourceRange&, VkImageLayout src, VkImageLayout dst)` (`ctx.h:1001`) | explicit layout transition (interop/present) | **Likely a no-op on Metal** (no explicit image layouts) except where it maps to a residency/purgeable transition or a barrier. Keep the method as a stub that may enqueue a barrier. See contract 3b. |

### 1h. Uploads / maps / discard (buffer renaming)

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:4790`, `:5526`, `d3d9_constant_buffer.cpp:40`, `:136` | `invalidateBuffer(const Rc<DxvkBuffer>&, Rc<DxvkResourceAllocation>&& slice)` (`ctx.h:891`) | **DISCARD map / renaming**: swap the buffer's backing storage so the CPU can write a fresh slice while the GPU still reads the old one. Core of D3D9 `MAP_DISCARD`. Old backing kept alive by the in-flight command list. | `invalidateBuffer(const Rc<MtlBuffer>&, Rc<MtlAllocation>&& slice)` → point the logical buffer at a new sub-allocation from a per-frame ring/pool; retire old alloc when the frame's command buffer completes. **Contract 3a — deadlock-class if the retirement fence is stubbed.** |
| `d3d9_constant_buffer.cpp:136` SetupStreamCommand | `invalidateBuffer` + `uploadBuffer(dst, dstOff, source, srcOff, size)` (`ctx.h:1033`) | transfer-queue init of a buffer sub-range from staging | `uploadBuffer(dst, dstOff, staging, srcOff, size)` → blit copy (no separate transfer queue on Metal; same queue, ordered). |
| `d3d9_initializer.cpp:73` InitDeviceLocalBuffer | `initBuffer(const Rc<DxvkBuffer>&)` (`ctx.h:855`) | zero-fill a fresh device-local buffer before first use | `initBuffer(const Rc<MtlBuffer>&)` → `fillBuffer:range:value:0` (or rely on private-heap zeroing). Contract 3c. |
| `d3d9_initializer.cpp:101`, `d3d9_swapchain.cpp:1066` | `initImage(const Rc<DxvkImage>&, VkImageLayout)` (`ctx.h:867`) | transition + black-clear a fresh image before first use | `initImage(const Rc<MtlTexture>&)` → clear render/blit pass. Contract 3c. |

### 1i. Queries

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `:7822` Begin → `d3d9_query.cpp:264` | `beginQuery(const Rc<DxvkQuery>&)` (`ctx.h:129`) | occlusion begin; recorded on CS timeline | `beginQuery(const Rc<MtlQuery>&)` → `setVisibilityResultMode:offset:` on encoder. |
| `:7831` End → `d3d9_query.cpp:284` | `endQuery(const Rc<DxvkQuery>&)` (`ctx.h:136`) | occlusion end; result later polled by `GetData` via CS sequence number (`d3d9_query.cpp:129`) | `endQuery(...)` → mode Disabled; result buffer read after command-buffer completion. |
| `d3d9_query.cpp:268/280` (timestamp / event) | `writeTimestamp(const Rc<DxvkQuery>&)` (`ctx.h:1251`) | timestamp + D3DQUERYTYPE_EVENT fence | `writeTimestamp(const Rc<MtlQuery>&)` → `MTLCounterSampleBuffer` sample, or an `MTLSharedEvent` value for EVENT queries. Contract 3d (event queries gate `GetData` — deadlock-class). |

### 1j. Present / frame / sync / latency

| site | method (as used) | semantics | Metal-first |
|---|---|---|---|
| `d3d9_swapchain.cpp:880` PresentImage | reads `ctx->presenter`, `ctx->frameId`; `ensureImageCompatibility`; `beginExternalRendering`; `synchronizeWsi(PresenterSync)` (`ctx.h:112`); `flushCommandList` | acquires WSI image, blits backbuffer, submits, presents; `synchronizeWsi` wires acquire/release semaphores to the submit | present path → `CAMetalLayer nextDrawable` + `presentDrawable:` (via winemetal). `synchronizeWsi(MtlPresenterSync)` sets the drawable wait/signal on the command buffer. Contract 3e (frame-latency signalling — deadlock-class). |
| `:6132` ExecuteFlush | `ctx->signal(const Rc<sync::Signal>&, uint64_t)` ×2 (`ctx.h:1263`); `flushCommandList(const VkDebugUtilsLabelEXT*, DxvkSubmitStatus*)` (`ctx.h:102`) | queues a CPU-visible signal fired after GPU completion; submits current command buffer & starts a new one | `signal(const Rc<MtlSignal>&, uint64_t)` → `MTLSharedEvent` set on completion handler; `flushCommandList(const char* reason, MtlSubmitStatus*)` → commit current MTLCommandBuffer, begin next. |
| `:6188` EndFrame, `d3d9_swapchain.cpp:880` | `endFrame()` (`ctx.h:68`); `signalGpuEvent(const Rc<DxvkEvent>&)` (`ctx.h:1244`) | end-of-frame bookkeeping; GPU event signal | `endFrame()`; `signalGpuEvent(const Rc<MtlEvent>&)`. |
| `:6175` BeginFrame, `:6188`, `d3d9_swapchain.cpp:1113` | `beginLatencyTracking(const Rc<DxvkLatencyTracker>&, uint64_t frameId)` (`ctx.h:80`); `endLatencyTracking(const Rc<DxvkLatencyTracker>&)` (`ctx.h:91`) | Reflex-style latency markers on CS timeline | keep as thin markers; map to `CAMetalDisplayLink`/timestamps or stub. Low risk. |
| `:6132` frameLatencySignal, `deviceResetCounter`, `frameId` | data accessors on context | frame pacing counters | plain `MtlContext` members. |

### 1k. Debug labels

| site | method | Metal-first |
|---|---|---|
| `d3d9_annotation.cpp:131/148/161` | `beginDebugLabel(const VkDebugUtilsLabelEXT&)` / `endDebugLabel()` / `insertDebugLabel(...)` (`ctx.h:1294–1311`) | `pushDebugGroup:` / `popDebugGroup` / `insertDebugSignpost:` on encoder or command buffer. Trivial. |

### 1l. Misc / infra emit points

* `d3d9_device.cpp:5405` and HUD paths call `beginExternalRendering` (1e).
* `d3d9_initializer.cpp:162`, `d3d9_device.cpp:6132`, `d3d9_swapchain.cpp:880` call `flushCommandList` (1j).
* `d3d9_device.h:1197/1230/1248` and `d3d9_initializer.h:83` are the `EmitCs`/`InjectCs`
  template bodies themselves, not backend calls.
* `ctx->track(...)` (2 sites) and `ctx->beginRecording` — lifetime tracking / initial record;
  Metal equivalent `track(Rc<MtlResource>&&)` (residency + keep-alive to command-buffer completion).

**Full DxvkContext surface reachable from src/d3d9/** (unique, from grep): draw, drawIndexed,
bindVertexBuffer(+Range), bindIndexBuffer, bindShader, bindResourceImageView/BufferView/Sampler,
bindResources, bindUniformBuffer(+Range), bindRenderTargets, setInputLayout, setInputAssemblyState,
setViewports, setBlend{Mode,Constants}, setDepthStencilState, setStencilReference,
setRasterizerState, setDepthBias(+Representation), setDepthBounds, setMultisampleState,
setLogicOpState, setSpecConstants, pushData, clearRenderTarget, clearImageView, copyImage,
copyBuffer, copyImageToBuffer, copyBufferToImage, resolveImage, blitImageView, transformImage,
generateMipmaps, ensureImageCompatibility, initImage, initBuffer, invalidateBuffer, uploadBuffer,
beginQuery, endQuery, writeTimestamp, signal, signalGpuEvent, beginExternalRendering,
emitGraphicsBarrier, flushCommandList, synchronizeWsi, beginRecording, endFrame,
begin/endLatencyTracking, begin/end/insertDebugLabel, track. **≈54 methods; ~40 core.**

---

## 2. CS infrastructure assessment (`dxvk_cs.{h,cpp}`)

**How it works (3.0).**
* `DxvkCsChunk` (`dxvk_cs.h:196`) is a 16 KB (`DxvkCsChunkSize`, `:15`) bump-allocated arena of
  type-erased commands. `push<T>` placement-news a `DxvkCsTypedCmd<T>` (a moved lambda) into the
  arena and chains it (`:221`). `pushCmd<M,T>` (`:249`) additionally reserves a *mutable data
  block* of `M[count]` tightly packed after the functor — this is the `EmitCsCmd` draw/upload path.
* `DxvkCsChunkPool` (`:361`) recycles chunks; `DxvkCsChunkRef` (`:404`) is refcounted return-to-pool.
* `DxvkCsThread` (`:513`) runs a worker that pops chunks from two queues — `Ordered` and
  `HighPriority` (`:480`) — and calls `chunk->executeAll(context)` which walks the chain calling
  `cmd->exec(ctx)`. `dispatchChunk` (`:532`) appends to the ordered timeline returning a sequence
  number; `injectChunk` (`:546`) runs out-of-band, optionally synchronous; `synchronize(seq)`
  (`:560`) blocks until the worker has drained up to `seq`. Two atomics + two CVs; no spin.

**Vulkan dependency?** The queue/thread/arena machinery is **Vulkan-clean**. It contains zero
`Vk*` type. Its *only* coupling to the backend is that `DxvkCsCmd::exec` and `executeAll` take a
`DxvkContext*` (`:54`, `:310`), and the header includes `dxvk_device.h` / `dxvk_context.h`
(`:10–11`) purely to name that type and `Rc<DxvkDevice>`/`Rc<DxvkContext>` members on the thread
(`:574–575`, `:519`). The recorded *lambdas* capture Vulkan-ish objects, but the container does not.

**Verdict: lift `dxvk_cs.{h,cpp}` almost verbatim.** Rename `DxvkContext`→`MtlContext`,
`DxvkDevice`→`MtlDevice`, retarget the two includes, keep chunk size / pool / dual-queue /
sequence-number sync exactly. The `pushCmd` mutable-block mechanism is load-bearing for the
`EmitCsCmd` draw path and must be preserved byte-for-byte. This is the single highest-leverage
reuse in the whole rewrite — the entire deferred-recording model is free.

---

## 3. Implicit contracts (hidden behaviors the frontend assumes)

These are *not* in method signatures but the frontend breaks without them. Ordered by blast radius.
⚠ = historically deadlocks when stubbed (a CPU or GPU workload waits on a signal that never fires).

**3a ⚠ Buffer renaming / discard-map ring (`invalidateBuffer`).** D3D9 `MAP_DISCARD` and dynamic
constant buffers assume `invalidateBuffer` instantly gives the CPU a fresh, un-raced backing slice
while the GPU keeps reading the old one; the old allocation is *implicitly kept alive until the
command list that references it completes*, then recycled. **Metal:** a per-frame ring/sub-allocator
whose slices are returned to the pool from the committed command buffer's completion handler
(`addCompletedHandler`). If the completion handler / retirement fence is stubbed, the ring either
grows unbounded (OOM) or recycles live memory (corruption) — and a CPU that spins waiting for a free
slice deadlocks. This is *the* discard path; get it right first.

**3b Automatic inter-draw hazard tracking.** The frontend issues *no* barrier between an ordinary
draw that writes an RT and a later draw that samples it — it assumes `DxvkContext` inserts the
RAW/WAR barrier automatically at `draw`/`bindRenderTargets` time. It only calls `emitGraphicsBarrier`
explicitly for the *feedback-loop* case (RT bound as texture in the same pass, `d3d9_device.cpp:6409`).
**Metal:** MtlContext tracks resource read/write intent per encoder and inserts barriers/splits
the encoder automatically. **Metal 4 is untracked by default** (`00-metal4-reference.md`
§Untracked): every hazard the frontend assumes must become an *explicit* producer→consumer
barrier (`MTL4VisibilityOptions`) at a defined interface point. Map them: RT-write→sample =
barrier at the next `bindRenderTargets`/`draw` consuming a prior RT; copy/blit→sample = barrier
at `bindTexture` of a just-written texture; `uploadBuffer`/`copyBuffer`→draw = barrier at `draw`.
**Metal 3 auto-tracks all of these for free** — on the Metal-3 winemetal ABI the explicit
barrier points (and `emitGraphicsBarrier`) may no-op; on the Metal-4 bridge they must all fire.
Spec the explicit barrier points *now* so the Metal-3 impl no-ops and the Metal-4 impl honors
them with no API change. Missing this on Metal 4 = silent corruption, not deadlock.
`transformImage` becomes mostly-noop but may enqueue such a barrier.

**3c Resource pre-initialization on creation (`initBuffer`/`initImage`).** Freshly created buffers
are zero-filled and images are cleared-to-black + transitioned to a defined layout *before first
frontend use*, via the `D3D9Initializer` batching on a background context (`d3d9_initializer.cpp`).
D3D9 apps rely on cleared-to-zero resources. **Metal:** clear on a private heap at create, or a
batched init blit/clear pass. Not deadlock-class but produces garbage/NaN first frame if skipped
(cf. memory *Metallib cache black screen*).

**3d ⚠ Event-query fence (`writeTimestamp` for `D3DQUERYTYPE_EVENT`).** `Query::GetData` with
`D3DGETDATA_FLUSH` and `IsEvent()`/`IsStalling()` (`d3d9_device.cpp:7835`, `d3d9_query.cpp:129`)
spins the app thread until the GPU passes the event. This maps to an `MTLSharedEvent` value the CPU
waits on. If the signal is stubbed/never advances, the app's GetData loop **deadlocks** the whole
game (this is the classic "waits on a fence that never fires" case flagged in the task).

**3e ⚠ Present / frame-latency signalling (`synchronizeWsi`, `signal`, frame-latency semaphore).**
Present (`d3d9_swapchain.cpp:880`) wires acquire/release semaphores via `synchronizeWsi` and posts a
frame-latency signal; the swapchain throttles the app to N frames in flight by waiting on it.
**Metal:** `CAMetalDrawable` present + an `MTLSharedEvent`/completion-handler-driven frame-latency
counter. Stub the latency signal and the frame-limiter thread deadlocks (or the app free-runs and
starves). Highest-visibility deadlock surface — present must be wired end-to-end before anything
renders (see memory *live draw path*).

**3f Lazy render-pass load/store from bind + clear ordering.** `bindRenderTargets` only sets dirty
flags; the encoder/load-store actions are decided at the *next* draw or clear. A `clearRenderTarget`
issued right after a bind is expected to collapse into the render pass `loadAction=Clear` rather than
a standalone clear; `discardAspects` maps to `DontCare`. MtlContext must keep a *pending*
MTLRenderPassDescriptor and only materialize the encoder lazily, folding pending clears into load
actions. Get this wrong → correct pixels but big perf loss (extra passes) or wrong load-op tiling.
When an implicit mid-frame `flushCommandList` (3i) lands inside an open pass, **Metal 4 render-pass
suspend/resume across command buffers** (`00-metal4-reference.md` §MTL4RenderCommandEncoder) lets the
same logical pass continue in the next command buffer without a store/reload round-trip — spec pass
splitting around suspend/resume rather than always closing the encoder. Metal 3 lacks this, so the
Metal-3 impl closes+reopens (store/load) at a flush; keep the split decision behind one internal
`materializePass()` so only the impl differs.

**3g Backing reallocation copies contents (`ensureImageCompatibility`).** Returns bool and may
allocate a new backing with extra usage and *copy existing contents over*, transparently swapping the
image's storage (`d3d9_device.cpp:1518` before ColorFill/clear). Metal texture usage is immutable at
creation, so `ensureTextureUsage` must alloc-new + blit-copy + retarget, keeping the old texture alive
to command-buffer completion (same retirement discipline as 3a).

**3h Move-ownership & keep-alive of passed refs.** Every `Rc<...>&&` / `DxvkBufferSlice&&` argument is
*moved into* the recorded lambda and thereby kept alive until the chunk executes; `ctx->track()`
extends lifetime further to GPU completion. MtlContext methods must take the same `Rc<>&&` / slice
by-move and register them into the residency set + command-buffer keep-alive list. Drop this and you
get use-after-free the instant the frontend releases its handle post-emit.

**3i State survives across `flushCommandList`; only `beginExternalRendering` invalidates all.**
An implicit mid-frame flush (chunk full → `ConsiderFlush`, `d3d9_device.h:1240`) must **not** lose
bound pipeline/RT/descriptor state — the next draw re-resolves from the frontend's shadow state.
`beginExternalRendering` is the *only* full-invalidate. MtlContext must re-open encoders and
re-commit arg tables after a flush transparently.

---

## 4. Proposed `MtlContext` v0 header sketch

Draft for human review. Metal-4-shaped: PSOs are memoized from folded state, binding is
**slot-based argument tables** per stage, sync is explicit (barriers + `MTLSharedEvent`), lifetime
is residency-set + completion-handler retirement. No `Vk*` anywhere. `Mtl*` value/handle types are
placeholders to be defined against the winemetal ABI. Open questions inline as `// Q:`.

```cpp
// v2/dxvk-mtl/mtl_context.h  (sketch — not final)
namespace d9mt {

  enum class MtlShaderStage : uint32_t { Vertex, Fragment };            // D3D9: VS/PS only
  using MtlStageMask = uint32_t;                                        // bit per stage
  enum class MtlPipeline : uint32_t { Graphics };                      // D3D9 has no compute path
  enum class MtlIndexType : uint32_t { Uint16, Uint32 };
  enum class MtlPrimitiveTopology : uint32_t { /* points..triangles, fans emulated */ };

  class MtlContext : public RcObject {
  public:
    MtlContext(const Rc<MtlDevice>&);

    // --- lifecycle / submission (§1j, contracts 3e,3i) ---
    void beginRecording();
    void endFrame();
    void flushCommandList(const char* reason, MtlSubmitStatus* status);   // commit + new cmdbuf
    Rc<MtlCommandBuffer> beginExternalRendering();                        // full state invalidate
    void synchronizeWsi(MtlPresenterSync sync);                          // ⚠ present acquire/release
    void beginLatencyTracking(const Rc<MtlLatencyTracker>&, uint64_t frameId);
    void endLatencyTracking(const Rc<MtlLatencyTracker>&);

    // --- shaders & pipeline state (folds into memoized render PSO) (§1b,1c) ---
    // MtlShaderFunction wraps an MTL4LibraryFunctionDescriptor over our AIR metallib — no MTLFunction,
    // no SPIR-V. The render PSO is resolved lazily at draw from folded state; its key EXCLUDES
    // depth/stencil attachment formats (not part of MTL4RenderPipelineDescriptor). PSO compile is
    // async (MTL4Compiler): a draw requests the PSO and either uses a ready variant or a flexible/
    // stand-in PSO while MTL4CompilerTask completes — never a blocking compile on the CS thread.
    void bindShader(MtlShaderStage, Rc<MtlShaderFunction>&&);
    void setFunctionConstants(MtlPipeline, uint32_t index, uint32_t count, const void* data);
    void setVertexLayout(uint32_t nAttr, const MtlVertexAttr*, uint32_t nLayout, const MtlVertexLayout*);
    void setPrimitiveTopology(MtlPrimitiveTopology);
    void setViewports(uint32_t n, const MtlViewport*);                  // Q: Retina scale factor?
    void setRasterizerState(const MtlRasterizerState&);
    void setMultisampleState(const MtlMultisampleState&);
    void setDepthStencilState(const MtlDepthStencilState&);             // cached MTLDepthStencilState
    void setBlendMode(uint32_t rt, const MtlBlendMode&);
    void setLogicOpState(const MtlLogicOpState&);                       // Q: emulate on Metal
    // dynamic (no PSO rebuild):
    void setBlendColor(MtlColor4);
    void setStencilReference(uint32_t);
    void setDepthBias(MtlDepthBias);
    void setDepthBounds(MtlDepthBounds);                               // Q: no native; emulate/ignore

    // --- slot-based argument-table binding (§1d, contract 3h) ---
    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&&, uint32_t stride); // Q: stride→PSO dirty
    void bindVertexBufferRange(uint32_t binding, uint64_t off, uint64_t len, uint32_t stride);
    void bindIndexBuffer(MtlBufferSlice&&, MtlIndexType);
    void bindUniformBuffer(MtlStageMask, uint32_t slot, MtlBufferSlice&&);
    void bindUniformBufferRange(MtlStageMask, uint32_t slot, uint64_t off, uint64_t len);
    void bindTexture(MtlStageMask, uint32_t slot, Rc<MtlTextureView>&&);
    void bindBufferView(MtlStageMask, uint32_t slot, Rc<MtlTexelBufferView>&&);
    void bindSampler(MtlStageMask, uint32_t slot, Rc<MtlSamplerState>&&);
    void pushData(MtlStageMask, uint32_t offset, uint32_t size, const void* data); // ≤4KB inline

    // --- render targets (§1e, contract 3f) ---
    void bindRenderTargets(MtlRenderTargets&&, MtlFeedbackLoopMask);    // pending pass, lazy encoder
    void emitGraphicsBarrier(MtlStageMask src, MtlBarrierScope srcScope,
                             MtlStageMask dst, MtlBarrierScope dstScope);

    // --- draws (§1a) — resolve PSO/arg-tables/dynamic state, open encoder, draw ---
    void draw(uint32_t count, const MtlDrawArgs* args);
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args);

    // --- clears (§1f) ---
    void clearRenderTarget(const Rc<MtlTextureView>&, MtlAspectMask clear,
                           MtlClearValue, MtlAspectMask discard);
    void clearImageView(const Rc<MtlTextureView>&, MtlOrigin, MtlSize,
                        MtlAspectMask, MtlClearValue);

    // --- copies / blits / resolves / mips (§1g) ---
    void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers, MtlOrigin srcOff, MtlSize);
    void copyBuffer(const Rc<MtlBuffer>& dst, uint64_t dstOff,
                    const Rc<MtlBuffer>& src, uint64_t srcOff, uint64_t bytes);
    void copyTextureToBuffer(const Rc<MtlBuffer>& dst, uint64_t off, uint64_t rowAlign,
                    uint64_t sliceAlign, MtlPixelFormat, const Rc<MtlTexture>& src,
                    MtlSubresourceLayers, MtlOrigin, MtlSize);
    void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers, MtlOrigin, MtlSize,
                    const Rc<MtlBuffer>& src, uint64_t off, uint64_t rowAlign, uint64_t sliceAlign,
                    MtlPixelFormat);
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion,
                    MtlPixelFormat, MtlResolveMode color, MtlResolveMode stencil); // Q: partial region
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                    const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                    MtlSamplerFilter);                                 // Q: needs blit shader (no native scaled blit)
    void generateMipmaps(const Rc<MtlTextureView>&, MtlSamplerFilter);
    void transformImage(const Rc<MtlTexture>&, MtlSubresourceRange, /*layouts→*/ MtlBarrierScope); // mostly noop

    // --- init / upload / discard (§1h, contracts 3a,3c,3g) ---
    void initBuffer(const Rc<MtlBuffer>&);                             // zero-fill
    void initImage(const Rc<MtlTexture>&);                            // clear-to-black
    void invalidateBuffer(const Rc<MtlBuffer>&, Rc<MtlAllocation>&& slice); // ⚠ discard/rename ring
    void uploadBuffer(const Rc<MtlBuffer>& dst, uint64_t dstOff,
                      const Rc<MtlBuffer>& staging, uint64_t srcOff, uint64_t size);
    bool ensureTextureUsage(const Rc<MtlTexture>&, MtlTextureUsage);   // realloc+blit if needed

    // --- queries (§1i, contract 3d) ---
    void beginQuery(const Rc<MtlQuery>&);
    void endQuery(const Rc<MtlQuery>&);
    void writeTimestamp(const Rc<MtlQuery>&);                          // ⚠ EVENT query → shared event

    // --- signals / events (§1j, contracts 3d,3e) ---
    void signal(const Rc<MtlSignal>&, uint64_t value);                 // ⚠ CPU-visible, on completion
    void signalGpuEvent(const Rc<MtlEvent>&);

    // --- lifetime / residency (contract 3h) ---
    void track(Rc<MtlResource>&&);                                    // residency + keep-alive

    // --- debug labels (§1k) ---
    void beginDebugLabel(const char*);
    void endDebugLabel();
    void insertDebugLabel(const char*);
  };
}
```

**Open design questions carried forward:**
* Vertex-buffer *stride* is a PSO input in Metal (MTLVertexDescriptor) but a dynamic bind in DXVK —
  decide whether stride changes invalidate the render PSO or whether we normalize to a fixed layout.
* Retina/backing-scale for `setViewports` (open in memory *v2 GPU capture*).
* Scaled `blitTextureView` and partial-region `resolveTexture` have no native Metal op — need a blit
  shader / dedicated resolve pass; scope for v0 vs v1.
* `setDepthBounds` / `setDepthBiasRepresentation` / `setLogicOpState` have weak/no Metal analogues.
* Argument-table vs Metal-4 `MTL4ArgumentTable` residency-stamped slots — the DXMT technique
  (memory *DXMT binding techniques*) is portable but architectural; decide the slot model before draw.

### 4.1 Metal 4 alignment checklist (per `00-metal4-reference.md`)

The interface above is deliberately shaped so the Metal-3 winemetal impl and the future Metal-4
bridge (`winemetal4/`) are a *swap, not a redesign*. Concrete commitments baked into the sketch:

1. **PSO key excludes depth/stencil formats.** `MTL4RenderPipelineDescriptor` defers depth/stencil
   attachment formats to runtime, so the render-PSO cache key is {VS,FS function descriptors,
   function constants, vertex layout, color-attachment formats+blend, topology class, sample count}
   — *no depth format*. `setDepthStencilState` produces a separately-cached `MTLDepthStencilState`
   object, never a PSO variant. This shrinks the variant count vs the Vulkan-shaped v1 keying.
2. **Binding = snapshot-at-dispatch slot tables.** `bind*` are pure slot writes; the draw captures
   an `MTL4ArgumentTable` snapshot. Metal-3 impl emulates via a per-draw ring AB copy (already our
   proven fix); Metal-4 impl is native, zero-copy. No API change between the two.
3. **Untracked resources → explicit barrier points.** Every implicit hazard the d3d9 frontend
   assumes (contract 3b) has a named barrier point in the interface (`emitGraphicsBarrier` +
   auto-inserted barriers at `draw`/`bindTexture`/`bindRenderTargets`). Metal-3 auto-tracks and the
   impl no-ops them; Metal-4 requires them via `MTL4VisibilityOptions`. Spec'd now to avoid a churn.
4. **Explicit command model.** `beginRecording`/`flushCommandList`/`endFrame` map to explicit
   `MTL4CommandBuffer` begin/end with batched queue `commit([...])`; per-thread `MTL4CommandAllocator`
   allows later parallel encode (single CS thread first). Pass splitting at an implicit flush uses
   render-pass **suspend/resume** on Metal 4 (no store/reload), plain close/reopen on Metal 3, both
   behind one internal `materializePass()`.
5. **Async PSO compilation.** Render/blit PSOs are built through `MTL4Compiler` on a background QoS
   queue; `draw` requests a PSO and proceeds with a ready/flexible variant rather than blocking the
   CS thread. `MTL4PipelineDataSetSerializer` replaces the v1 metallib disk cache entirely — this
   removes the poisoned-toolchain-cache bug class (memory *Metallib cache black screen*). AIR
   metallibs feed `MTL4LibraryFunctionDescriptor` directly; no runtime SPIR-V→MSL anywhere.

## 6. Open-question resolutions (2026-07-11)

Decided from d9mt v1/v2 production experience; these are final for v0.

1. **Viewport Retina scaling (`setViewports`)** — MtlContext takes viewports in
   render-target pixel coordinates only. Window-scale (Retina ×2 drawable vs
   client rect) is applied exclusively in the swapchain/present layer where the
   drawable size is known. The frontend never sees display scaling. (Root cause
   of the v2 "everything in the bottom-left quarter" bug — scaling leaked into
   the wrong layer.)
2. **`setLogicOpState` — CUT.** D3D9 has no logic ops; the method only existed
   because DxvkContext serves D3D11 too. Not part of MtlContext.
3. **`setDepthBounds` — accept + no-op.** D3D9 exposes depth-bounds only via
   the NVDB FOURCC hack; it is a shading-skip optimization, not a correctness
   feature. Metal has no equivalent; ignoring it renders correctly at slightly
   more fragment cost. Log once when a game enables it.
4. **Vertex stride (`bindVertexBuffer`)** — stride is baked into the Metal
   vertex descriptor, therefore part of the PSO memo key (array of per-binding
   strides). Stride change ⇒ PSO-dirty, binding change alone ⇒ not. (The v2
   "stride = attribute extent" garbled-geometry bug makes this explicit.)
5. **`resolveTexture` partial region** — full-subresource resolve is the fast
   path (render-pass resolve store action). Partial-region resolve: resolve
   full to a scratch view, then copy the requested region; log it (rare in
   D3D9 — StretchRect sub-rects from MSAA surfaces).
6. **`blitTextureView` scaled/filtered blit** — internal meta-blit render
   pipeline (fullscreen triangle + sampler), shipped in the driver's own
   build-time metallib alongside the FF uber-shaders. Same approach DXVK uses
   (meta-blit); no runtime shader generation.
