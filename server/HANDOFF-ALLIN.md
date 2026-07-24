# HANDOFF — THE ALL-IN SLICE (dual execution killed; server-only rendering)

Branch `native-backend`, 4 commits on top of e5ea35d (s3-v13.1). Rollback of
the whole slice = `git revert <first>..<last>` of this range (each commit is
independently revertable — present-source, thin recorder, query/readback
stubs, debt cleanup). The fully-local v2 lives at pre-wire commits; there are
NO runtime fallbacks and NO env toggles — git history is the rollback.

## What this slice is

The client stops rendering. `MtlWireContext` is a THIN RECORDER: hot-loop
calls serialize into the SPSC ring only; the render server is the sole
renderer; the present blits the server's IOSurface, PERIOD.

## Commit 1 — server-only present (`allin(present)`)

- `d3d9_swapchain.cpp PresentImage`: the closure acquires ONLY the server
  frame. No local-backbuffer fallback, no pace-gate source flip — both are
  DELETED, not gated.
- `acquireServerFrame` (mtl_wire_context.cpp): lag budget now UNBOUNDED
  (`WireServerFrame_acquire(..., UINT64_MAX, ...)`): the freshest PUBLISHED
  frame is always accepted — when the server runs behind, the previous frame
  repeats (present-the-last-server-frame semantics). `false` only for: wire
  never attached / server dead (the loud latches, unchanged) / nothing
  published yet (boot) / IOSurface lookup or zero-extent wrap failure.
- SKIP-PRESENT: on `false` the closure does NOT acquire a drawable. Screen
  keeps the last presented frame — black before the first one. That black is
  the honest failure mode; wire death is already loudly logged.
- Frame-marker decoupling (THE subtlety): the server closes frame N only at
  its `present_frame` marker, which used to be emitted inside
  `synchronizeWsi` — unreachable on a skipped present, so the FIRST frame
  could never publish and every present would skip forever. Now the skip and
  occluded paths call `ctx->synchronizeWsi(MtlPresenterSync())` (null
  drawable): `MtlWireContext::synchronizeWsi` emits the marker always and
  forwards the inner Metal 4 handshake only for a real drawable.
- MSAA present resolve is RECORD-ONLY (server executes it); the client never
  reads the resolve surface.

## Commit 2 — thin recorder (`allin(recorder)`)

Hot calls that STOPPED forwarding to `m_inner` (serialize-only): bindShader,
all set* state, all slot binds, pushData, bindRenderTargets, barriers,
draw/drawIndexed, clearRenderTarget/clearImageView. The copy family
(copyTexture / blitTextureView / resolveTexture / generateMipmaps) forwards
ONLY when a side is a pseudo-resource that cannot cross the wire — the
present blit (drawable dst / server-wrap src) is exactly that shape;
eligible↔eligible ops are server-side only.

`m_inner` is now a resource-and-present provider. What it still does and WHY
(the authoritative per-forward list lives in `mtl_wire_context.h`):

| kept forward | why |
|---|---|
| beginRecording / endFrame / flushCommandList / synchronizeWsi / latency | frame lifecycle: CS loop, allocator pacing, submission spine, Metal 4 present handshake |
| beginExternalRendering | external-overlay seam rides the local queue |
| copyTexture / blitTextureView (ineligible sides) + presentUpscale | THE present blit into the drawable from the server IOSurface |
| copyTextureToBuffer | readback executor — the dump harness reads the wrapped server IOSurface through it |
| copyBuffer / copyBufferToTexture / uploadBuffer | upload machinery: staging bytes must land in local backings/mirrors that heap snapshots, shadow re-binds and Lock reads consume |
| initBuffer / initImage / invalidateBuffer / ensureTextureUsage | resource allocation at CS position (wire-id assignment, mapPtr staging, info mirroring hang off the same objects) |
| writeTimestamp / signal / signalGpuEvent / track | completion spine: staging/submission fences + EVENT queries ride LOCAL submission serials; unfired signals deadlock the frontend |
| debug labels | free |

Dropped to documented no-ops (inner impls were base-class stubs anyway):
setFunctionConstants, bindBufferView (warn-once — also untransported),
transformImage, setDepthBounds.

## Commit 3 — queries + readbacks, stubbed HONESTLY (`allin(queries+readbacks)`)

- Occlusion queries: begin/end do not forward. endQuery completes the query
  instantly as "all samples passed" (constant `kQueryStubSamplesPassed`,
  1<<20 — nonzero for every boolean consumer). Counted in PROF `qStub=`.
  Rationale: the counted draws execute on the SERVER; the local visibility
  buffer would only ever read 0 = everything occluded, the worst lie.
- Timestamp/EVENT queries still work (local submission spine lives).
- Readbacks: `copyTextureToBuffer` of a wire-ELIGIBLE RenderTarget-usage
  source returns stale/zero client pixels — counted in PROF `rbStale=`,
  first 8 named `wire readback STALE:`. The dump harness is unaffected (its
  source is the ineligible server wrap = real pixels).
- **DEBT / GTA-gate blocker (the NEXT slice):** occlusion-query transport
  (visibility results from the server queue) and rendered-RT readback
  transport (GetRenderTargetData/GetFrontBufferData). GTA IV uses occlusion
  for LOD/lens-flares — the all-passed stub means possible flare/LOD
  overdraw, never missing geometry. The cube reproducer uses NO queries.

## Commit 4 — debt cleanup (`allin(cleanup)`)

- C1 `fbHealthy` instrument deleted (g_serverEverProduced /
  g_fallbackWhileHealthy + PROF field) — there is no fallback to measure.
- PROF wire suffix now ends `... copySkip= qStub= rbStale=`. `copySkip` is
  re-scoped: ops with a wire-ineligible side that ran LOCALLY instead (the
  present copy is the steady per-frame source); for eligible resources a
  skip is a server-side content gap.
- Dump-pair harness collapsed: `CutoverVerifyDumpPair` →
  `CutoverServerDump` (same `D9MT_CUTOVER_VERIFY=1` gate, same
  `CUTOVER_DUMP` one-shot + every-N cadence, single
  `rt_dump_server{,_N}.bin`). Legacy `C:/d9mt-test/DUMP` → `rt_dump.bin`
  now dumps the PRESENTED server frame; both one-shot triggers are LEFT in
  place until a server frame exists (a too-early touch retries instead of
  eating the trigger).

## Expected cube signature (the reproducer proves this, not the build)

- Log: `render server attached ... (thin recorder: server-only rendering)`,
  then `present source: first SERVER frame accepted (localSerial=...)`.
- Screen: the SERVER-rendered spinning cube — or an honest black window if
  the server pipeline is broken (there is no local image to mask it).
- Wire line: `decodeErr=0 srvErr=0`, `srvFrames` tracking the present rate,
  `qStub=0 rbStale=0` for the cube, `stalls=0`.
- v13.1 checksum bursts (`wire-draw#N` / server twins) still fire — upload
  parity logging is untouched; `ups=` must still grow between bursts.
- First frames before the server publishes: skip-present (no drawable
  churn), then steady presents.

## What only the reproducer proves

The build gates prove serialization/ABI/server execution (test_feed drives
the server directly and is unaffected). They can NOT prove: the skip-present
boot handshake converges (marker-without-drawable → first publish → first
accepted frame), drawable pacing under skip, the unbounded-lag repeat frame
being visually sane, or that nothing in the frontend deadlocks on a signal
that used to come from dropped forwards. That is the coordinator's 40 s cube
run.

## Known honest gaps (carried, all pre-existing or named above)

- clearImageView with an ineligible target: dropped both sides (counted in
  copySkip). Not observed in practice.
- Records lost if the transport dies mid-frame: server-only rendering is
  dead at that point anyway; the dead-wire latches log loudly.
- bindBufferView / setFunctionConstants: untransported (warn-once named);
  base-class stubs locally even before this slice.
- Local texture mirrors still exist (uploads forward) — the single-copy
  memory win (dropping the mirror entirely) is a later slice, after
  readback transport removes the last local readers.
