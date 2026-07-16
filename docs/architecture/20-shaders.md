# 20 — Shaders: DXSO → IR → AIR → metallib → MTL4Compiler

No SPIR-V, no SPIRV-Cross, no MSL, no Apple tooling at runtime. App shaders
compile through our own emitter; only the two offline-generated ubershaders
(FF, meta-blit) ever touched Apple's MSL toolchain, at build time.

## Pipeline (app shaders, at CreateVertexShader/CreatePixelShader time)

```
SM1-3 DXSO bytecode
  → d3d9_shader_analysis (constant layout, masks, signatures)
  → createD3D9MetalShader (d3d9_shader_compiler.cpp):
      SM3→IR conversion + legacy-input lowering + legalization
      (dxbc-spirv ir::Builder; serialized legalized IR cached in the backend obj)
  → getMetallib(linkageCookie, specStateHash)  [lazy, per VS/PS linkage]
      VS↔PS interpolant reconciliation (resolveMismatchedIo/resolveSemanticIo)
      → D9MTAirCompile (bridge thunk 155) crosses PE→unix
          → AirBuilder (air/air_builder.cpp): walks the IR, emits an LLVM 15
            module targeting air64, wraps it in a metallib container
            (air/air_metallib.cpp) MTLDevice_newLibrary accepts
      ← metallib bytes + reflection blob returned INTO CALLER-PROVIDED PE
        buffers (32-bit PE cannot read unix pointers)
  → MTLDevice_newLibrary → library obj_handle_t on MtlShaderFunction
  → PSO: MTL4Compiler_newRenderPipelineState with (library handle, entry name)
```

`MtlShaderFunction` (driver/mtl/mtl_context.h) is the seam object: `library`
handle, `entryName` ("main" for app shaders; `ffVsMain`/`ffFsMain` for FF —
Metal reserves "main" for hand-written MSL), `cbvSlotMap`, `abTableIndex`,
`abWords`.

## Binding models — the load-bearing part

AirBuilder slot assignment (air/air_builder.h header, verified):

**Per-slot (classic) model:**
- `DclInputBuiltIn ConstFloat` (the cF float constants / MVP) → `[[buffer(0)]]`.
- Other legacy builtins (LegacyClipPlanes, LegacyPointArgs, LegacyAlphaTest…)
  → `[[buffer(1..)]]` in declaration order.
- `DclCbv` → `[[buffer(regIndex)]]` when no legacy builtins remain, else
  `[[buffer(8 + regIndex)]]`.
- Vertex streams → `kVertexBufferBaseIndex = 3` + stream (backend constant,
  mtl_context_impl.h).
- Push data → its own buffer at `kPushConstantIndex = 20`.
- Textures/samplers → `[[texture(n)]]`/`[[sampler(n)]]` == the D3D9 register.

**The CbvIndex ≠ Metal slot trap:** the frontend binds constant buffers at raw
D3D9 `CbvIndex` (0..7); the AIR shader reads them at the indices above. The
mapping travels in the **reflection blob**: each `DclCbv` entry carries
`reg_index == CbvIndex` + the actual Metal `index`;
`parseReflectionCbvMap()` (d3d9_shader_compiler.cpp ~1346) builds
`MtlShaderFunction::cbvSlotMap[CbvIndex] → AIR slot` (-1 = shader doesn't
declare it), consumed by the backend's `flushResources`. Binding at raw
CbvIndex was the original "no triangle" bug (SOURCE_OF_TRUTH §4b).

**Argument-buffer (AB) model** (`AirBuilder::Options::useArgumentBuffer`,
default ON; `D9MT_NO_AB=1` reverts): textures, samplers and CBVs become 8-byte
handle words (gpuResourceID / gpuAddress) in ONE struct bound at
`[[buffer(kAbTableIndex = 21)]]` (`AirBuilder::kAbTableIndex` ==
`MtlContextImpl::kAbIndex` == `MtlShaderFunction::abTableIndex` — keep all
three in lockstep). Reflection then reports `abWord` indices instead of slots;
`MtlShaderFunction::abWords[i] = {kind: Cbv|Texture|Sampler, reg}` tells the
backend what to write into word i. The backend CPU-writes the words into an
UploadRing slice and appends ONE `setAddress` per stage per draw instead of
per-slot argument-table calls. Push data keeps its own slot (20). The FF
ubershaders are NOT on the AB path (per-slot); the backend reconciles on every
model switch (`StageBindings::abActive`).

**Reflection outputs the backend consumes** (from
`AirBuilder::getBufferBindings/getTextureBindings/getSamplerBindings/
getSpecBindings/getOutputInfo/getAbInfo`, serialized as
`d9mt_air_reflection_header` + `d9mt_air_reflection_entry` records over the
bridge): buffer index/byteSize/isCbv/regIndex/abWord; texture kind
("2d"|"cube"|"3d"); spec-constant indices; `OutputInfo{renderTargetCount,
writesDepth, writesPointSize}`; `AbInfo{used, tableIndex, sizeBytes}`.

Other AIR facts that bite:
- D3D9 VS always writes `[[point_size]]` → PSO topology class must be
  Unspecified, not Triangle.
- Spec constants → AIR function constants (index = IR specId), one metallib
  serves all specializations; `foldSpecConstants` bakes values instead.
- Helper functions are named by walk order (not SSA id) for byte-determinism
  across `ir_serialize` round trips.
- Push-data blocks: bytes 0..31 = the shared `D3D9SharedPushData` block
  (`kPushSharedBlockSize = 32`), per-stage blocks after. AirBuilder must honor
  DclPushData's stage-mask operand — dropping it aliased the VS block onto the
  shared block (invisible peds/vehicles saga, 90-history.md).

## The FF ubershader story (offline conversion)

D3D9 fixed-function is implemented by **auto-converting DXVK 3.0's FF GLSL**,
not hand-writing Metal. `scripts/build-driver.sh` (FF step, runs only when the
GLSL sources are newer than the header):

```
driver/d3d9/shaders/d3d9_fixed_function_{vert.vert,frag.frag,*.glsl}
  → glslangValidator --target-env vulkan1.3   (GLSL→SPIR-V)
  → spirv-cross --msl --msl-version 30000 --rename-entry-point main ffVsMain/ffFsMain
  → perl pins to v2 slots:  VS: SpecConsts→buffer(0), ShaderData→1,
      ClipPlanes→2, VertexBlendData→5, RenderStates→20(push)
      FS: SpecConsts→0, SharedData→1, RenderStates→20; t2d→texture(0),
      tcube→texture(8), samplers→sampler(0)
  → xcrun metal/metallib → xxd → driver/d3d9/d3d9_ff_metallib.h
      (g_ffVsMetallib / g_ffFsMetallib byte arrays)
```

Wiring: `d3d9_fixed_function.cpp` `D3D9FFShaderModuleSet::Initialize` builds
the `MtlShaderFunction`s (entryName ffVsMain/ffFsMain, cbvSlotMap;
`CbvIndex::SpecData = 7` carries the 20-dword SpecConsts CBV). The FF VS
declares all 18 vertex registers and **Metal requires every declared stage-in
attribute present in the vertex descriptor** (Vulkan tolerated absences) —
hence `RenderPsoKey` attrs/layouts are 20 wide. The FS texture layout was
SHRUNK to t2d[8] + tcube[8] = 16 total: the original 5 type-arrays × 16 (80+
bindings) exceeded the argument table (**do NOT bump max_textures past 16 and
do NOT create a 1×1 cube dummy — both fault at init**; 90-history.md,
FF-binding saga). This recipe is reusable for converting any DXVK GLSL.

The meta-blit pipeline is the same idea smaller:
`driver/backend/shaders/mtl_blit.metal` → `mtl_blit_metallib.h` (blit VS/FS,
solid-fill FS, depth-writing clear VS).

## Debugging a bad shader

1. **AirBuilder rejection at runtime** (unsupported op): logged to
   `/tmp/d9mtair_err.log`. Decode the op number via the `OpCode` enum in
   `third_party/dxbc-spirv/ir/ir.h` (explicit uint16 values). Fix loop:
   edit `air/air_builder.cpp` → `rm build/airlib/obj/air_air_builder.cpp.o` →
   `build-d9mtair.sh` → `build-bridge.sh` → `install-bridge-builtins.sh` → run.
2. **Wrong codegen**: `AirBuilder::getIrText()` returns the LLVM IR text of
   the built module; the differential suite `scripts/run-air-diff.sh` (46
   cases, pixel-identical vs Apple's MSL compiler) is the regression gate —
   add a case there. `DXVK_SHADER_DUMP_PATH` dumps inputs frontend-side.
3. **Wrong result on screen**: GPU-capture (`MTL_CAPTURE_ENABLED=1
   D9MT_GPUCAPTURE=trigger [D9MT_GPUCAPTURE_COMMITS=N]`, `touch
   <bottle>/drive_c/d9mt-test/GPUCAP` → `/tmp/d9mt.gputrace`), then the
   `gpudebug` CLI headlessly: `gpudebug --gputrace /tmp/d9mt.gputrace`,
   commands `list` / `find drawPrimitives` / `go /commands/cb1/re0/draw0` /
   `info pipeline` / `fetch` (sessions persist via `-s N`; absolute paths for
   `go`). Check the draw's bound resources (`setTexture:0` = the unbind tell),
   vertex data at rest, loadActions. Caveat: a serialized capture can hide
   overlapping-execution races that only appear live.
4. **Binding-shaped garbage**: Metal API validation `MTL_DEBUG_LAYER=1
   MTL_DEBUG_LAYER_WARNING_MODE=nslog` (warnings land in wine stderr) —
   found the FF binding overflow.

## To change X, edit Y

- New SM3 op → `air/air_builder.cpp` (+ a run-air-diff case).
- Binding-model change → AirBuilder slot logic + reflection writer,
  `parseReflectionCbvMap` (d3d9_shader_compiler.cpp), backend
  `flushResources`/constants in `mtl_context_impl.h` — all in one change.
- FF shader logic → the GLSL under `driver/d3d9/shaders/`, then rebuild
  (build-driver.sh regenerates the header; needs brew glslang + spirv-cross).
- AIR compile ABI → `bridge/d9mtair_thunks.*` + airlib `d9mt_air_*` structs
  (8-byte/arch-invariant members only).
