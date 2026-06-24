# d9mt v2 — Metal-first Roadmap

Goal: a D3D9→Metal layer that runs real games, built on the **unmodified DXVK D3D9
frontend** but reshaped so the backend is **Metal-resident by design**, not
Vulkan-shaped. Beat v1 on wastage, performance, and maintainability — *provably*,
not by assertion.

**Guiding principle:** never do frontend surgery blind. Get a real workload →
profile → rip Vulkan waste one validated cut at a time, each backed by a measured
delta and a regression target.

---

## Status

- [x] **Triangle milestone** — RGB triangle through the real DXVK frontend +
      SPIR-V→MSL conversion + argument-buffer binding. (commit `0ff82a3`)
- [x] **Stage A** — frame-scoped present: one drawable/encoder per frame,
      present once at `Present()`, per-draw GPU stall gone. (`2aa7fe1`)
- [x] **Stage B** — zero per-draw heap allocations (reused scratch). (`b056014`)
- [x] **Phase 1.1 — programmable SM3 shaders** — ported the real DXVK
      `DxvkIrShader` (dxbc-spirv IR→SPIR-V + binding-model lowering) over the
      stub; `code_shader.exe` renders the RGB triangle. (`2f414cd`)
- [x] **Phase 1.3 — index buffers + drawIndexed** — `code_index.exe`
      renders via `DrawIndexedPrimitive`. (`712c11f`)

### Phase 1.2 status — infrastructure DONE, color blocked on depth2d

Texture binding works end-to-end (verified: `code_texture.exe` renders the
8×8 checkerboard, sharp point-filtered, correct UVs). Implemented: real
MTLTexture creation + `replaceRegion` upload (`copyBufferToImage`), sampler
creation + bindless sampler heap, texture argument-buffer descriptors,
residency, and `getSamplerHeapSet()` so the converter finds the heap.

**Remaining blocker — `depth2d` returns one channel.** The uber-PS contains a
`sample_compare` (shadow) path guarded by a *runtime* `samplerMode` (spec
constants are lowered to a CBV in `DxvkIrShader::getCode` via
`lowerSpecConstantsToCbv` when linkage is null). SPIRV-Cross therefore sees a
Dref use and emits `depth2d<float>`, whose `.sample()` returns only the red
channel (red/blue checker shows as black/white). Fix: prune the shadow path so
SPIRV-Cross emits `texture2d`. The clean approach is an IR-level permutation —
in the ported `dxvk_shader_ir.cpp`, replace the sampler-mode/type spec-constant
SSA values with literals for the bound sampler state (so `legalizeIr` DCE kills
the dead Dref branch), keyed into the shader cache by those values. Do NOT
touch the SPIR-V→MSL convert module; the fix belongs in the IR getCode path.

### Phase 1.2 texture binding contract (decoded from generated MSL, for the loop)

The convert module already reflects textures/samplers/heaps; the gap is the
backend (app textures are stubbed — no MTLTexture). The PS MSL expects:
- `buffer(0)` = sampler heap: device AB of `spvDescriptor<sampler>` (sampler
  `gpu_resource_id`s), indexed by a 16-bit field in push data (`s0_s0_sPad`).
- `buffer(2)` id(0) = the texture descriptor (texture `gpu_resource_id` in the
  set-2 AB), accessed directly (not heap-indexed).
- `buffer(7)` specData `psSamplerTypes`/`psSamplerModes` select sample mode;
  `samplerType==3` returns black.
- winemetal: `MTLDevice_newTexture`/`newSamplerState` (out `gpu_resource_id`),
  `MTLTexture_replaceRegion` (upload), `WMTRenderCommandUseResource` (residency).
- WATCH: SPIRV-Cross emitted `depth2d<float>` for the color sampler — the D3D9
  sampler-type (shadow vs color) spec/permutation likely needs to flip it to
  `texture2d`. Verify with `code_texture.exe` (checkerboard) and chase if the
  quad is grayscale/black.

What A+B are NOT: the Vulkan-ectomy. They were the prerequisite + hygiene. The
real Vulkan removal is Phases 3–4 below, deliberately held until there's a real
frame to profile and regression-test against.

---

## Phase 1 — Capability to a real frame *(the gate)*

Can't profile or prove the rip on a 1-draw triangle. Get a real game frame first.
This is the most work but everything downstream depends on it, and it's what makes
"better than v1" provable.

1. **Programmable shaders (sm3)** — wire `d3d9_shader.cpp` vs/ps through the same
   translate→PSO→bind path FF already uses (infra mostly exists; needs bind
   wiring + a programmable-shader test). *Risk: low-med.*
2. **Textures + samplers** — `bindResourceImageView` / `bindResourceSampler` +
   the bindless sampler heap; image AB slots; `useResource` for textures.
   *Risk: med — new resource kind.*
3. **Index buffers + `drawIndexed`** — currently no-op stubs. *Risk: low.*
4. **Dynamic state into the PSO key** — blend, RT format, topology, depth/stencil
   (today the PSO cache keys only on the shader pair). *Risk: med — correctness.*

**Exit:** a real game (L4D2 / GTA IV) renders a frame. Unlocks Phases 2–4.

---

## Phase 2 — Instrument *(cheap, mandatory before cutting)*

5. **Per-frame CPU profiler** — port v1's `D9MT_TRACE` zone concept; capture a
   real frame's draw-path CPU breakdown. Without numbers, the Vulkan rip is
   guesswork. *Risk: low (additive).*

---

## Phase 3 — The Vulkan rip *(X′; profile → cut → re-profile → commit)*

Order by measured cost. Stop a cut that doesn't move the needle.

6. **C′ — residency once per frame** — winemetal has no `MTLResidencySet`, so
   dedupe `useResource` manually: issue per resource per *frame*, not per draw.
   *Risk: low, backend-only.*
7. **D — dirty-gate AB/push + cache vertex layout** — skip rebuilds when bound
   state is unchanged since the last draw (v1 had this gate; v2 dropped it).
   Validatable only against multi-state frames. *Risk: med.*
8. **PSO/state cache hardening** — proper key; stop per-draw PSO churn. *Low-med.*
9. **E — devirtualize the command path** *(big structural win)* — replace the
   `CsThread`/`CsChunk` type-erased lambda recording with POD `wmtcmd` packets +
   direct calls. **Edits the frontend `EmitCs` templates** → highest blast
   radius; behind a flag; last in this phase. *Risk: high, highest leverage.*
10. **Collapse descriptor-set / pipeline-layout indirection → Metal-native bind**
    — kill the Vulkan binding-map dance between frontend slots and Metal buffer
    indices; cut per-draw AB assembly + `Rc<>` atomic churn. *Risk: high.*

---

## Phase 4 — Frontend de-plumb in place *(V2_ARCHITECTURE §7)*

11. Rip dead Vulkan paths out of the frontend's **hot** functions (the ones that
    actually run per-draw — NOT the gc-dropped no-ops), reshape Metal-first.
    Profile-guided, surgical, backed by a working game to regression-test.
    *Risk: high — but now numbers + regression target exist.*

---

## Phase 5 — Maintainability *(V2_ARCHITECTURE §6 / Stage F)*

12. File split, retire the no-op Vulkan stubs (barriers / layout transitions /
    sync — currently empty inlines, gc-dropped, ~0 runtime), rename to the §6
    module layout. Pure cleanliness; do last.

---

## Critical path

`1 → 2 → 3 → 4 → 5`. Phase 1 is the gate. Phases 3–4 are the actual Vulkan
removal and are deliberately *after* instrumentation (so every cut is justified by
a measured delta) and *after* a working game (so there's a regression target).

The compiled-but-dead Vulkan stubs stay untouched until Phase 5 — ripping them
earlier looks productive but buys ~0 runtime and risks link breakage.

---

## Working notes (for the loop)

- Build: `bash v2/scripts/build-dxvk.sh` (parallel + sccache; ~3s warm). Deploy +
  launch: `bash v2/scripts/run.sh`. Self-verify by screenshot (`screencapture -x`)
  + Read the PNG — the triangle window is titled "d9mt triangle".
- Diagnostics land in the bottle: `…/Rockstar Games Launcher/drive_c/d9mt-test/v2.log`.
- Keep the triangle rendering at every step; commit per validated step; push to
  `origin/main` (private repo `neo773/d9mt-v2`).
- Divide work with sub-agents on **disjoint files**; define the header/API seam
  first, then fan out (the Stage A pattern worked well).
- Do NOT touch the SPIR-V→MSL shader conversion (cached after warmup, low
  priority, swappable later).
