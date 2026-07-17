# HANDOFF — task #12 consolidation / thin-recorder migration plan

The engineering-first execution spec for "migrate the whole driver to the
native path in clean high-quality code." VERDICT-GATED: do NOT start until the
cutover is proven on a real game run (menu reached, compare_frames.py near-match
on the local/server pair). Grounded in the current code (native-backend @82bb6c4).

## Current structure (dual execution)
- `MtlDeviceImpl::createContext()` (mtl_device_impl.cpp:453) wraps the real
  `MtlContextImpl` in a `MtlWireContext`.
- `MtlWireContext` (mtl_wire_context.h): HOT calls (binds/state/draws/clears)
  = forward-to-inner + serialize-to-ring; COLD calls (creates/copies/queries/
  lifecycle) = forward-to-inner only. `m_inner` is the local renderer.
- Present (d3d9_swapchain.cpp PresentImage): `acquireServerFrame` → if a valid
  server frame exists, blit the server IOSurface; else blit the local backbuffer.
- So today BOTH renderers run every frame; present chooses the source.

## The migration, phased (each phase independently verifiable, no big-bang)

### C1 — prove the fallback is never needed (measure, don't cut)
Run the cutover for an extended soak with a PROF counter for "presents that fell
back to local while the wire was live and the server was alive" (add
`fallbackWhileHealthy` to the present path). Target: 0 over minutes of play,
across boot / reset / mode-change / heavy scenes. Only a provably-never-needed
fallback is safe to remove. If it fires, fix the cause first — do NOT proceed.

### C2 — route the local-content READERS to the server (the hard part)
These read locally-rendered pixels and MUST get server-rendered data before the
local renderer can be removed:
- `GetRenderTargetData` / `GetFrontBufferData` / the backbuffer dump
  (copyTextureToBuffer paths): the source RT lives on the server now. Add wire
  records to request a readback of a server texture by wire-id → server does the
  copyTextureToBuffer into a shared-heap region → client reads it. Gate on a
  completion serial (the frame_serial mechanism already exists).
- Occlusion queries (visibility.rs equivalent, mtl_backend_query + the visibility
  buffer): the draws counted now execute on the SERVER. The visibility results
  must come from the server's queue. Route query begin/end + result-read across
  the wire; publish results in status or a shared-heap region. THIS is the single
  biggest blocker — GTA IV uses occlusion for LOD/lens-flare; wrong results =
  visible pop/flicker. Verify against a scene with heavy occlusion culling.
- Any StretchRect/GetDC read of a rendered surface: same treatment.
Each reader gets its own sub-gate (dump/query/stretchrect) — verify individually.

### C3 — thin recorder (the fps win, the actual migration)
Once C1 (fallback never needed) + C2 (readers routed) hold:
- `MtlWireContext` stops forwarding HOT calls to `m_inner` — serialize only.
  The inner context no longer executes draws/binds/state → the ~8-13ms of
  Rosetta-translated per-draw C++ leaves the client frame. THIS is where the fps
  arrives.
- `m_inner` shrinks to what cold calls still need locally (resource creation for
  wire-id assignment + info mirroring, present drawable acquire). Audit each cold
  forward: does it need local Metal work, or just bookkeeping? Keep only the
  bookkeeping.
- Present becomes server-source-only (local backbuffer no longer rendered);
  the fallback path is removed ONLY after C1 proved it dead. Keep a
  build-time (not env) escape only if a reviewer insists.
- Measure: PROF CS-thread ms should drop toward the ~4-6ms native target;
  HUD fps is the proof.

### C4 — debt cleanup (after C3 verified)
- Delete the now-dead local hot-loop execution paths in MtlContextImpl that the
  recorder no longer drives (careful: cold paths + the SERVER's copy of the same
  logic in server/executor.mm stay — they're the real renderer now).
- Remove dual-execution scaffolding comments; update docs/architecture/ (30-backend,
  60-present, 70-render-server) to describe server-only as the default path.
- Resolve the flagged leaks: the unix IOSurface wrap cache "never releases evicted
  textures" (mtl_wire_context / winemetal_unix cache) — implement release-when-
  unreferenced now that lifetime is understood; the cross-process GPU write/read
  ordering gap (client reads IOSurface with no barrier vs server writes) — add an
  MTLSharedEvent handshake if C1/soak ever shows tearing.
- Re-run the docs audit for fresh drift.

## Invariants
- Never cut a fallback that C1 hasn't proven dead.
- Each reader (C2) and the recorder (C3) is a separate verified step — no big-bang.
- METAL 4 ONLY; full-clean builds before any perf number; the known-good-control
  discipline (memory: metal-gpu-state-wedge) before bisecting any regression.
