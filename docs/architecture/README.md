# docs/architecture — d9mt v2 driver architecture

Orientation set for a fresh agent: read [00-overview.md](00-overview.md),
then the doc for your subsystem. `docs/SOURCE_OF_TRUTH.md` remains the
authoritative PLAN doc (its §2 Metal-4-only rule overrides everything);
these docs describe the system AS BUILT (branch `native-backend`,
2026-07-17).

| Doc | Covers | Key anchors |
|---|---|---|
| [00-overview.md](00-overview.md) | Process diagram, three layers, repo map, build entry points (+ the no-header-deps gotcha), full env-var inventory, invariants | `build-driver.sh`, `install-bridge-builtins.sh`, `D9MT_*` |
| [10-frontend.md](10-frontend.md) | DXVK 3.0 extraction (kept/ripped, Vulkan gate), EmitCs/CS-thread model, d3d9/ file map, pools/Lock/Reset semantics, wsi, divergences | `EmitCs`, `dxvk_cs.h`, `D3D9DeviceEx`, `EXTRACTION-LOG.md` |
| [20-shaders.md](20-shaders.md) | DXSO→IR→AirBuilder→metallib→MTL4Compiler, per-slot vs argument-buffer binding, cbvSlotMap/reflection, FF ubershader recipe, shader debugging | `AirBuilder`, `kAbTableIndex`, `cbvSlotMap`, `ffVsMain`, `getIrText` |
| [30-backend.md](30-backend.md) | MtlContextImpl: dirty bits, pass lifecycle, CmdChunk batching, argument-table pairs/shadows/rotation, PSO cache + MRU, deferred passes (decision 10), special paths, PROF line decode | `prepareDraw`, `materializePass`, `ArgTableShadow`, `RenderPsoKey`, `D9MT_PROFILE` |
| [40-bridge.md](40-bridge.md) | PE/unix split, thunk recipe (highest index 214), dispatch lockstep, ownership/autorelease contract, execute_on_main hazard, wire ring transport | `UNIX_CALL`, `__wine_unix_call_funcs`, `D9MT_POOLED`, `WireRing_attach` |
| [50-resources.md](50-resources.md) | Creation seams, Rc + handles, renames/invalidateBuffer, the 4 residency invariants, submission serials + pump, upload ring, staging | `MtlResidencyTracker`, `SubmissionTracker`, `UploadRing`, `invalidateBuffer` |
| [60-present.md](60-present.md) | Swapchain/presenter contract, MSAA resolve surface, CS-thread acquire, layer geometry ownership, present handshake, Reset flow | `synchronizeWsi`, `applyProps`, `acquireNextImage`, `m_resolveBuffer` |
| [70-render-server.md](70-render-server.md) | Falsified in-process premise, S1 shipped state + real-game proof, S2/S3 plan; pointers to spec + handoffs | `d9mt-serverd`, `d9mt_status.h`, `mtl_wire.h` |
| [90-history.md](90-history.md) | 15 debugging sagas with root causes and transferable lessons | clear-drop, 2198 freeze, CmdChunk copy, WM_DISPLAYCHANGE, … |

Corrections to stale in-code/in-doc claims are flagged inline where they
matter: deferred gate is `D9MT_DEFERRED` (header text says `D9MT_NO_DEFERRED`),
present DOES waitForDrawable pre-commit (swapchain comment says otherwise),
`mtl_wire.h`'s "x86_64 PE producer" is actually i686, `server/README.md`'s
"not wired into build-driver.sh" is outdated, and SOURCE_OF_TRUTH §4/4b/4c
describe the 2026-07-11 bring-up, not the current backend.
