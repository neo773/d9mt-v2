# 03 — D3D9 Shader Path (DXVK 3.0 fork) → SM3→AIR backend plan

Scope: map the exact shader pipeline in `v2/DXVK-3.0-uptsream` and specify how to
plug our `v2/air` AirBuilder (legalized `dxbc_spv::ir::Builder` → metallib) in the
place currently held by `dxbc_spv::spirv::SpirvBuilder`. All paths are relative to
`v2/DXVK-3.0-uptsream/`.

**Headline architectural facts (this fork ≠ classic DXVK):**
- **No `dxso`.** There is no `src/dxso/` directory. D3D9 programmable shaders go
  `SM3 bytecode → dxbc_spv::sm3::Converter → dxbc_spv::ir::Builder`. The include is
  `sm3/sm3_parser.h` / `sm3/sm3_converter.h` (`d3d9_shader.cpp:7-9`). This is the
  *new dxbc-spirv `sm3`* path, not legacy DXSO and not DXBC.
- **No `D3D9FFShaderKey`.** Fixed-function is **not** runtime-generated. It is a pair
  of precompiled GLSL→SPIR-V **uber-shaders** driven entirely by spec constants
  (`grep D3D9FFShaderKey src/d3d9` → 0 hits; see §3).
- Everything funnels through a common IR shader object `DxvkIrShader`
  (`src/dxvk/dxvk_shader_ir.h:105`), which is where SPIR-V is produced today.

---

## 1. The 3.0 shader flow, end to end

### 1a. Create → converter object
`CreateVertexShader` / `CreatePixelShader` (in `d3d9_device.cpp`) run shader analysis
(`d3d9_shader_analysis.*`) then call
`D3D9ShaderModuleSet::GetShaderModule` → constructs a `D3D9CommonShader`
(`d3d9_shader.cpp:1178`). That ctor builds the base IR converter:

- `D3D9CommonShader::D3D9CommonShader` (`d3d9_shader.cpp:1178-1200`) calls
  `GetDXVKDevice()->createCachedShader(key, irCreateInfo, converter)` where the
  converter is a `D3D9ShaderConverter` (`d3d9_shader.cpp:1093`), a
  `DxvkIrShaderConverter` subclass. `createCachedShader` wraps it in a `DxvkIrShader`.
- Bytecode → IR happens in `D3D9ShaderConverter::convertShader`
  (`d3d9_shader.cpp:1109-1131`): builds `dxbc_spv::sm3::Converter::Options`
  (`name`, `includeDebugNames`, `fastFloatEmulation`, `isSWVP`,
  `forceDynamicTextureType`), runs `sm3::Converter::convertShader(builder)`, then runs
  the D3D9-specific **`D3D9ShaderLowerLegacyInputPass`** (`d3d9_shader.cpp:13-1091`)
  in-place on the IR (see §2).
- `D3D9ShaderConverter::determineResourceIndex` (`d3d9_shader.cpp:1133-1157`) maps
  IR `(stage, type, regSpace, regIndex)` → DXVK binding index. CBVs pass through
  their reg index; SRV/Sampler go through
  `D3D9ShaderResourceMapping::computeTextureBinding` (`d3d9_shader.h:66`).

### 1b. Two-phase compile inside `DxvkIrShader`

**Phase A — `convertShader()` (once per unique shader, cached):**
`src/dxvk/dxvk_shader_ir.cpp:2196-2268`.
1. `m_baseIr->convertShader(builder)` = the SM3→IR + legacy-input lowering above.
2. Optional `LowerIoPass::resolveXfbOutputs` (streamout) / `demoteMultisampledSrv`.
3. **`dxbc_spv::ir::legalizeIr(builder, options)`** (`:2257`) — the big legalization
   (arithmetic lowering, min16, resource/buffer options, scalarize sub-dword,
   sync/ROV, derivative hoist, CSE, descriptor indexing). Options at `:2216-2255`.
4. `DxvkIrLowerBindingModelPass` (`:2260`) produces `DxvkShaderMetadata` (`m_metadata`)
   + `DxvkPipelineLayoutBuilder` (`m_layout`), and appends the spec-data buffer
   (`m_layout.addSpecDataBuffer(...)`, `:2265`).
5. `serializeIr(builder)` (`:2267`) → the **legalized IR is serialized and cached**
   (`m_ir`). **This serialized, legalized `ir::Builder` is exactly what AirBuilder is
   designed to consume.**

**Phase B — `getCode(bindings, linkage)` (per pipeline / per linkage / per spec):**
`src/dxvk/dxvk_shader_ir.cpp:2016-2137`. **This is the EXACT SPIR-V emission point.**
1. `deserializeIr(irBuilder)` (`:2024`) — restore the cached legalized IR.
2. **`dxbc_spv::ir::LowerIoPass ioPass(irBuilder)`** (`:2027`) runs the *link-time* I/O
   fixups that depend on the *other* stage and on pipeline state:
   - `enableFlatInterpolation(m_info.flatShadingInputs)` when FS flat shading (`:2029`).
   - `changeGsInputPrimitiveType` (GS only, `:2033`).
   - dual-source blend output setup `resolveUnusedOutputs` (`:2035-2041`).
   - `swizzleOutputs(...)` — RT component swizzles from `linkage->rtSwizzles` (`:2043-2051`).
   - **VS↔PS interpolant reconciliation** (`:2053-2063`): if
     `DxvkShaderIo::checkStageCompatibility(...)` fails, run
     `resolveSemanticIo(prevStageIoMap)` (when semantic matching) and
     **`resolveMismatchedIo(prevStage, prevStageIoMap)`**. Inputs needed:
     `linkage->prevStage`, `linkage->prevStageOutputs` (an `IoMap`), `matchSemantics`.
   - tess patch-constant location resolve (TCS/TES, `:2065-2069`).
   - **when `linkage == nullptr`**: `ioPass.lowerSpecConstantsToCbv(SpecDataSet, 0u)`
     (`:2071`) — spec constants are lowered to a CBV instead of staying specializable.
     `SpecDataSet == 7` (`dxvk_shader_ir.h:110`).
   - `enableSampleInterpolation()` for sample-rate shading (`:2074-2077`).
3. Build `SpirvBuilder::Options` (`:2083-2128`) — float-control round/denorm modes,
   NV raw access chains, dual-src, `maxCbvSize/Count`.
4. `DxvkShaderResourceMapping mapping(m_metadata.stage, bindings)` (`:2131`) — turns the
   `DxvkShaderBindingMap` into IR-resource→SPIR-V-binding mapping.
5. **`dxbc_spv::spirv::SpirvBuilder spirvBuilder(irBuilder, mapping, options);
   spirvBuilder.buildSpirvBinary();`** (`:2133-2134`) → `SpirvCodeBuffer`. **← replace
   these two lines' output with AirBuilder → metallib.**

### 1c. Who calls `getCode` and builds `linkage`
Graphics: `DxvkGraphicsPipeline::getShaderCode` (`dxvk_graphics.cpp:1537-1538`) →
`shader.getCode(bindingMap, &linkage)`, called per stage in `createPipeline`
(`dxvk_graphics.cpp:1444-1454`). The linkage per stage is built by
`DxvkGraphicsPipelineShaderState::getLinkage` (`dxvk_graphics.cpp:875-912`): for the VS
`prevStageOutputs = DxvkShaderIo::forVertexBindings(attributeMask)`; for later stages
`prevStageOutputs = prevStage->metadata().outputs`. Spec-constant values themselves
travel separately as `scState.scInfo` passed to `stageInfo.addStage(...,&scInfo)`
(Vulkan `VkSpecializationInfo`), i.e. they are **specialized at PSO creation**, not
baked into `getCode`.

**Net for us:** the SPIR-V-specific work in `getCode` that AIR must reproduce is the
step-2 `LowerIoPass` block (link-time I/O). Everything before (Phase A) is already in
the serialized IR. Everything spec-value-related is either specialization (Vulkan spec
info) or `lowerSpecConstantsToCbv`.

---

## 2. What the AIR path must replicate

Two-tier caching mirrors DXVK: **Phase A per unique bytecode** (produces legalized IR —
identical, backend-agnostic, reuse verbatim), **Phase B per (spec-state, linkage)**
(produces the metallib). AirBuilder consumes the *post-Phase-B* IR.

### 2a. Phase-A passes (per shader; already backend-neutral — keep as-is)
| Pass | Where | Purpose |
|---|---|---|
| `sm3::Converter::convertShader` | `d3d9_shader.cpp:1124-1127` | SM3 bytecode → IR |
| `D3D9ShaderLowerLegacyInputPass::run` | `d3d9_shader.cpp:23-73` | Replace D3D9 "legacy" built-in inputs (`eLegacyConstFloat/Int/Bool`, `eLegacyAlphaTest/Fog/ClipPlanes/PointArgs/SamplerState/TextureStage`) with loads from CBVs / push-data / **spec-constant helper functions**. Emits the CBV declarations (VSClipPlanes, VSFixedFunction, VSVertexBlend, VS/PS Static/Dynamic constants, PSShared) — see `D3D9ShaderResourceMapping::CbvIndex` (`d3d9_shader.h:54-63`). |
| `legalizeIr` | `dxvk_shader_ir.cpp:2257` | arithmetic/resource/scalarize/sync legalization AirBuilder already expects |
| `LowerBindingModelPass` | `dxvk_shader_ir.cpp:2260` | metadata + layout |

Note the legacy pass is where **sampler-type/mode specialization** originates
(`loadSamplerStateArgs`, `d3d9_shader.cpp:909-997`): texture type (2d/cube/3d/null),
`useDepthCompare` (depth vs color — `samplerMode >= Dref`), projection, gather (Fetch4),
dref clamp/scale — **all sourced from spec constants** (`emitSpecConstantLoad`,
byte offsets into `D3D9SpecData`). Alpha-test (`loadAlphaTestArgs`, `:740`) and fog
(`loadFogArgs`, `:769`) likewise pull compare-op / mode / enable from spec constants and
color/ref/scale from push data. **These are already lowered in Phase A**; the values
they read are the spec-data bytes resolved in Phase B.

### 2b. Phase-B passes AIR MUST run before AirBuilder (per VS/PS pair or per spec)
| Pass | Trigger | Frequency | Metal relevance |
|---|---|---|---|
| `resolveMismatchedIo(prevStage, prevStageIoMap)` | FS/GS/TES input vs prev-stage output mismatch (`dxvk_shader_ir.cpp:2062`) | **per VS/PS pair** | **CRITICAL.** D3D9 lets a PS read interpolants the VS doesn't write (and vice-versa); Metal render PSOs reject a `[[stage_in]]`/vertex-output mismatch. This pass rewrites the FS to inject/drop varyings so the two agree. This is the production blackscreen/PSO-reject class we hit. |
| `resolveSemanticIo(prevStageIoMap)` | when `matchSemantics` (`:2060`) | per pair | semantic-based varying matching (D3D9 uses semantics, not pure locations) |
| `enableFlatInterpolation(flatShadingInputs)` | FS flat shading (`:2029`) | per pair (depends on `linkage->fsFlatShading`) | Metal `[[flat]]` on the interpolant |
| `swizzleOutputs(rtSwizzles)` | FS (`:2050`) | per RT-format state | RT component swizzle |
| `resolveUnusedOutputs` (dual-src) | FS dual-src blend (`:2040`) | per blend state | Metal `[[color(0),index(1)]]` |
| `enableSampleInterpolation` | sample-rate shading (`:2077`) | per state | Metal `[[sample_perspective]]` |
| `lowerSpecConstantsToCbv(7,0)` OR keep as spec | `linkage==nullptr` path (`:2071`) | per spec-data | **Decision point (see 2c).** |

`resolveMismatchedIo` needs, from the *other* stage: `prevStage` id and
`prevStageOutputs` (`DxvkShaderIo` → `convertIoMap`, `dxvk_shader_ir.h:217`), plus
`matchSemantics = linkage->semanticIo && metadata.flags(SemanticIo)`.

### 2c. Spec constants → Metal function constants
DXVK has two modes: (a) real Vulkan spec constants specialized at PSO build (default,
graphics path passes `&scInfo`), or (b) `lowerSpecConstantsToCbv` when no linkage.
For Metal the clean analog of (a) is **`MTLFunctionConstantValues`** — declare each
`DclSpecConstant` (specIds 0..6, names at `d3d9_shader.cpp:633-641`;
`vsBoolConstants`/`psBoolConstants` also spec) as a Metal function constant and
specialize at PSO creation. AirBuilder today folds `DclSpecConstant` to its default
(header line 22, "TODO: driver specialization") — **this is the gap to close**: emit
function-constant references instead of constants. The `D3D9SpecData` struct
(`d3d9_state.h:105`) is the byte layout the spec ids index into; keep it as the CPU-side
source and translate the 7 dwords (+bool dwords) into function-constant values.

### 2d. Proposed AIR-shader-cache key
Mirror DXVK's split so Phase A is shared:

```
PhaseA cache (legalized IR):  key = DxvkShaderHash (SHA-1 of SM3 bytecode)
                                    + D3D9ShaderOptions (isSWVP, floatEmu,
                                      forceSamplerTypeSpecConstants)  // 3 bytes
PhaseB cache (metallib):      key = { irHash,                        // ptr/hash of Phase-A IR
                                      linkageCookie,                 // hash of prevStage + prevStageOutputs IoMap
                                                                     //   + fsFlatShading + rtSwizzles + dualSrc + sampleLoc
                                      specStateHash }                // hash of the D3D9SpecData bytes actually referenced
```

`linkageCookie` = reuse `DxvkShaderLinkage::hash()` (`dxvk_shader.cpp:41-49`, already
hashes `prevStageOutputs` vars). If spec values become Metal **function constants**
(2c), then `specStateHash` drops out of the *metallib* key and moves into the *PSO*
key instead (one metallib, many specialized PSOs) — strongly preferred, matches how the
Vulkan path uses `scInfo`.

---

## 3. Fixed-function pipeline in 3.0

**FF is precompiled uber-shaders, not runtime IR, and there is no `D3D9FFShaderKey`.**
`D3D9FFShaderModuleSet` (`d3d9_fixed_function.h:41`, impl `d3d9_fixed_function.cpp`)
builds exactly **two** `DxvkSpirvShader`s at device init from GLSL-compiled SPIR-V
headers:
- VS: `buildVs()` → `new DxvkSpirvShader(info, d3d9_fixed_function_vert)`
  (`d3d9_fixed_function.cpp:26-65`), source `shaders/d3d9_fixed_function_vert.vert`.
- FS: `buildFs()` → `d3d9_fixed_function_frag` or `..._frag_sample` if
  `forceSampleRateShading` (`:68-124`), source `shaders/d3d9_fixed_function_frag.frag`.

These are `DxvkSpirvShader` (raw SPIR-V), **bypassing the IR entirely** — so they are
specialized purely by **spec constants + push data + CBVs**, not by regenerating code.

**Full FF "state key" = the spec-constant/CBV set, not a struct key.** Driven by:
- `D3D9SpecData` (`d3d9_state.h:105-...`): spec id0 common (alphaTest op+precision,
  enablePointSprite, clipPlaneCount, drefScale); id1 fog (enable, vertex/pixel mode,
  useZ); id2 FF args (samplerProjMask, enableGlobalSpecular, enablePointScale,
  psIsShaderModel3); id4/5 ps sampler types/modes (lower 16 bits used by FF);
  **id8..11 `stageOps[8]`** (per-stage color op / alpha op / temp bit) and
  **id12..19 `stageArgs[8]`** (per-stage color/alpha arg0..2). Decoded in
  `d3d9_fixed_function_common.glsl:61-107` (`SpecConsts` block, `getFogState`,
  `isSpecularEnabled`).
- CBVs: `CBV_VS_FIXED_FUNCTION` (matrices WorldView/Projection/WorldViewProj,
  `TexcoordMatrices[8]`, `Lights[8]`, material, `WorldViewArray[]` for vertex blend),
  `CBV_VS_CLIP_PLANES`, `CBV_VS_VERTEX_BLEND`, `CBV_PS_SHARED` (texture-stage bump
  matrices / luminance / constant color).
- Push data: `D3D9FfvsPushData` (pointScaleA/B/C), `D3D9FfpsPushData` (textureFactor),
  `D3D9SharedPushData` (fog color/ref/scale/end/density), per-sampler push (sampler
  heap indices).

**What the VS uber-shader covers** (`d3d9_fixed_function_vert.vert`): world/view/proj
transform incl. pre-multiplied WVP; **vertex blend** (`WorldViewArray[]` + blend
weights/indices, `:411-446`); **TCI flags** (`DXVK_TSS_TCI_*`: PASSTHRU,
CAMERASPACENORMAL/POSITION/REFLECTIONVECTOR, SPHEREMAP; `:124-128`, applied `:496-603`)
with `TexcoordMatrices`; **lighting** (`computeLighting`, `:603`; point/spot/directional,
diffuse/specular/ambient, `D3DMATERIAL9`, up to 8 lights); fog output; point size +
point scale. Fog color/spec handled in `d3d9_fixed_function_frag.frag` +
`_common.glsl`. The FS uber-shader covers the 8 texture stages (op/arg decode from spec
constants), alpha test, fog application, texture-factor/constant blending.

**Metal-first FF plan:** author **two MSL uber-shaders** (VS + FS) — one metallib pair
compiled at **build time** — that reproduce the GLSL above, with **Metal function
constants replacing the SPIR-V spec constants**. Enumerate the function constants from
`D3D9SpecData` + `_common.glsl`:
- alpha: `alphaCompareOp` (4b), `alphaPrecision` (4b)
- fog: `fogEnable`, `fogVertexMode`(2b), `fogPixelMode`(2b), `fogUseZ`
- FF flags: `samplerProjMask`(8b), `enableGlobalSpecular`, `enablePointScale`,
  `psIsShaderModel3`, `clipPlaneCount`(3b), `enablePointSprite`
- per-sampler (16 samplers ×2b): `psSamplerTypes`, `psSamplerModes`
- **per-stage (×8): colorOp, alphaOp, resultIsTemp (`stageOps`); colorArg0..2,
  alphaArg0..2 (`stageArgs`)** — the bulk of the FF FS combinatorics
- per-texcoord (×8): TCI flags (3b) + projection index (VS)
- vertex-blend mode (Disabled/Normal/Tween — `D3D9FF_VertexBlendMode`,
  `d3d9_fixed_function.h:22`) + blend count; lighting enable + light count.

CBV/push layouts must match the tables above so the existing D3D9 state-tracker code
(which fills `D3D9SpecData`, the FF CBVs, and push data) is reused unchanged; only the
GLSL→SPIR-V uber-shaders are swapped for MSL→metallib uber-shaders, and
`setSpecialization`/spec-info becomes `MTLFunctionConstantValues`.

---

## 4. SWVP + misc (shader-adjacent state-tracker expectations)

- **SWVP constant limits.** `D3D9ShaderOptions.isSWVP` (`d3d9_shader.h:25`) →
  `sm3 options.isSWVP` (VS only, `d3d9_shader.cpp:1119`). SWVP uses vastly larger
  float-constant counts, so `setupStaticCbv` (`d3d9_shader.cpp:266-326`) has a fallback:
  if the constant struct exceeds `ir::Type::MaxStructMembers` or has bools, it emits a
  **dword array CBV that is dynamically indexed** (`:311-325`) instead of a member
  struct. Dynamic float constants use a separate `VSDynamicConstants` CBV
  (`setupDynamicCbv`, `:328-344`), over-declared by one vec4 to allow reading zero past
  the end, and dynamic indices are **clamped** to a runtime count read from
  `vsPushData[0]` (`cFSize`, `:381-388`). AIR must honor: dynamic-indexed CBV, the +1
  over-allocation, and the clamp against push-data count.
- **Bool constants**: HWVP packs 16 bools into spec data (`vsBoolConstants` /
  `psBoolConstants`); SWVP loads them from the static CBV via a generated helper
  (`lowerLegacyConstBoolInput`, `:426-519`). Both branches emit real IR — no backend
  work beyond supporting `FunctionCall`.
- **Point size emulation.** `vsPushData` carries packed pointSize/min/max
  (`setupVsPushData`, `:247-264`); decode = `u16/8.0` (`emitDecodePointSize`, `:1058`).
  `loadPointArgs` (`:873-907`) reads `enablePointSprite` (spec), sizes (push). The IR
  writes `DclOutputBuiltIn PointSize` — **AirBuilder currently DROPS PointSize stores**
  (air_builder.h:21, "stores DROPPED (TODO)"). Metal needs `[[point_size]]` from the VS;
  this must be implemented for point-sprite draws.
- **Clip planes.** `VSClipPlanes` CBV + `clipPlaneCount` spec (`loadClippingArgs`,
  `:832-871`); Metal has no fixed clip-distance array on all GPUs — may need
  `[[clip_distance]]` or manual discard; AirBuilder header notes ClipDistance stores are
  also currently dropped.
- **Shared push data** (`setupSharedPushData`, `:226-245`): fog color/alphaRef/scale/
  end/density — shared VS+FS; ensure Metal push (argument buffer / setBytes) layout
  matches `D3D9SharedPushData`.

---

## 5. Interface spec — minimal API our shader module must expose to the d3d9 frontend

The frontend needs a `DxvkIrShader`-shaped object whose `getCode`-equivalent returns a
**metallib** instead of SPIR-V. Minimal surface:

```cpp
// Phase A — create from SM3 bytecode (once; cached by bytecode hash + options)
AirShader::AirShader(DxvkShaderHash key,
                     D3D9ShaderOptions opts,
                     const void* sm3Bytecode, size_t len,
                     const D3D9ShaderAnalysis& analysis);
//   internally: sm3::Converter -> D3D9ShaderLowerLegacyInputPass -> legalizeIr
//               -> LowerBindingModelPass -> serialize legalized IR.

// Reflection the binder needs (available after Phase A; from metadata + AirBuilder):
DxvkShaderMetadata      metadata() const;      // stage, inputs, outputs (semantic IoMap)
DxvkPipelineLayoutBuilder layout() const;      // CBV/SRV/sampler bindings + spec-data buffer
const std::vector<AirBuilder::BufferBinding>& bufferBindings() const; // [[buffer(n)]] + sizes
D3D9ShaderMasks         masks() const;         // sampler mask + RT mask (D3D9CommonShader::GetShaderMask)
const D3D9InputSignature& inputSignature() const; // semantics for vertex-decl matching (see g_ffIsgn)

// Phase B — get a metallib for a concrete (spec-state, linkage) pair (cached per key in 2d)
std::vector<uint8_t> getMetallib(const DxvkShaderBindingMap* bindings,
                                 const DxvkShaderLinkage*     linkage);
//   internally: deserialize legalized IR
//               -> LowerIoPass link-time fixups (resolveMismatchedIo / resolveSemanticIo /
//                  enableFlatInterpolation / swizzleOutputs / dual-src / sample-rate)
//               -> [spec: emit as MTLFunctionConstant refs, do NOT fold]
//               -> AirBuilder(irBuilder, {entryName}).buildMetallib();
//   plus: getVertexAttributes() for the metallib VATT/VATY (VS stage-in).

// Function-constant descriptor the PSO builder consumes (replaces VkSpecializationInfo):
struct AirSpecLayout { /* map D3D9SpecData bytes -> MTLFunctionConstant indices */ };
AirSpecLayout specLayout() const;
```

Reflection required by the binder, all obtainable today:
- **constant slots used** — `D3D9CommonShader::GetConstantsInfo` /
  `GetConstantLayout` (`d3d9_shader.h:114-119`), plus `AirBuilder::getBufferBindings`.
- **samplers used** — `GetShaderMask().samplerMask`
  (`d3d9_shader.h:121`); type/mode come from spec data at Phase B.
- **input semantics** — `GetInputSignature()` (`d3d9_shader.h:110`) for vertex-decl →
  attribute matching; `AirBuilder::getVertexAttributes()` gives the metallib stage-in
  attribute list.

---

## Summary (10 lines)

1. This fork has **no DXSO and no DXBC** for D3D9: SM3 → `dxbc_spv::sm3::Converter` → IR,
   then `DxvkIrShader` — the single choke point where SPIR-V is emitted.
2. The exact SPIR-V point is `DxvkIrShader::getCode`,
   `dxvk_shader_ir.cpp:2133-2134` (`SpirvBuilder(...).buildSpirvBinary()`) — swap for
   AirBuilder→metallib.
3. Compile is **two-phase**: Phase A (`convertShader`, cached, ends in `legalizeIr`+
   serialize) produces the legalized IR AirBuilder wants; Phase B (`getCode`) runs
   link-time `LowerIoPass` fixups then the backend.
4. Phase A is backend-neutral — reuse verbatim, including
   `D3D9ShaderLowerLegacyInputPass` (legacy inputs → CBV/push/spec functions).
5. Phase B I/O fixups AIR **must** replicate: `resolveMismatchedIo`/`resolveSemanticIo`
   (VS↔PS varying reconciliation — our production Metal-PSO-reject bug),
   flat interp, output swizzle, dual-src, sample-rate.
6. Spec constants (7 dwords + bool dwords, `D3D9SpecData`) should become
   **`MTLFunctionConstantValues`**; AirBuilder currently folds them to defaults — close
   that gap so one metallib serves many specialized PSOs.
7. Cache key: Phase A = bytecode SHA-1 + 3-byte options; Phase B metallib = irHash +
   linkageCookie (reuse `DxvkShaderLinkage::hash`) + (spec moves to PSO key).
8. **Fixed-function has no `D3D9FFShaderKey`** — it is two precompiled GLSL→SPIR-V
   uber-shaders (`DxvkSpirvShader`) driven only by spec constants + CBVs + push data.
9. Metal-first FF = author two **MSL uber-shaders** (VS/FS) → build-time metallib, with
   function constants for the full state (alpha/fog/proj/per-stage ops+args ×8/TCI ×8/
   vertex blend/lighting/point) — CBV & push layouts kept identical so the state tracker
   is reused.
10. Known AIR gaps for D3D9: **PointSize and ClipDistance stores are dropped** (needed
    for point sprites / clip planes), and spec-constant specialization is unimplemented.

## Single hardest problem foreseen

**VS↔PS interpolant reconciliation under Metal's strict PSO linkage
(`resolveMismatchedIo`).** D3D9 legally lets a pixel shader read varyings the vertex
shader never writes (and vice-versa) and matches by *semantic*, not location — DXVK
papers over this per VS/PS pair inside `getCode` via `LowerIoPass::resolveMismatchedIo`
/ `resolveSemanticIo`, using the *other* stage's `IoMap` (`dxvk_shader_ir.cpp:2053-2063`).
Metal render pipelines hard-reject any mismatch between the vertex-function output struct
and the fragment `[[stage_in]]`, so AIR must run the same reconciliation and, crucially,
key the metallib cache on the *pair* (linkage cookie) rather than the single shader —
turning what was one SPIR-V blob into an N×M matrix of vertex/fragment combinations,
each needing consistent, deterministic varying slot assignment on both sides. This is the
class of bug behind our production blackscreens, and it is structurally harder on Metal
because there is no post-hoc I/O relaxation and no semantic linking at PSO time — the
agreement must be baked into the emitted AIR of both stages before compilation.
