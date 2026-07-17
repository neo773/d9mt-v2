# HANDOFF — S3 VISUAL CUTOVER (present the server's IOSurface frames)

Phase S3 final slice. The client now PRESENTS the render server's completed
IOSurface frames (with a bulletproof per-present fallback to the local
backbuffer), instead of always presenting its own locally-rendered backbuffer.
This is STILL dual execution — the client keeps rendering locally as the
fallback source; only what gets *presented* changed. Read
`server/HANDOFF-S3.md` "CUTOVER TODO" (the spec) and MILESTONE (boot survival,
run 9) first. This document is the cutover delta.

## What changed in the present path

`driver/d3d9/d3d9_swapchain.cpp` `PresentImage` — the EmitCs present closure
(CS thread) previously always blitted the local backbuffer into the drawable.
Now, before the drawable copy, it asks the wire producer for a server frame:

```
Rc<MtlTexture>     cServerImage; Rc<MtlTextureView> cServerView;
bool cUseServer = backend::wire::acquireServerFrame(cDevice, cLocalSerial,
                                                    cServerImage, cServerView);
const auto& cPresentImage = cUseServer ? cServerImage : cLocalImage;
const auto& cPresentView  = cUseServer ? cServerView  : cLocalView;
```

`cPresentImage/View` then feed the SAME existing copy/blit/MetalFX branches —
the server texture is just a different source. `cLocalSerial` is captured on
the app thread (`backend::wire::nextPresentSerial()`) so the CS-thread pace
gate compares against the same counter the server echoes as
`status.frame_serial`. This is the ONLY present-source change; `git revert` of
the commit restores dual-execution + unused server frames (revert plan, S3
TODO item 4).

### Cross-ABI plumbing added (the server IOSurface must be wrapped unix-side)

The present blit runs on the arm64 unix Metal side via `MtlContext`. IOSurface
lookup + MTLTexture wrap therefore live behind a new unixcall:

- `bridge/winemetal.h` — `struct WMTServerFrame` + `WireServerFrame_acquire(
  device, local_serial, lag_frames, out)`.
- `bridge/winemetal_thunks.{h,c}` — `struct unixcall_wire_server_frame` + the
  PE thunk (UNIX_CALL **216**).
- `bridge/unix/winemetal_unix.c` — `_WireServerFrame_acquire` (registered 216
  in BOTH dispatch tables, pooled) reads `wire_status` (version, frames,
  frame_serial, present_slot, iosurface_id[slot], iosurface_serial[slot]),
  applies the pace gate, and on success calls `wire_server_lookup_texture`:
  `IOSurfaceLookup(id)` → `newTextureWithDescriptor:iosurface:plane:` on the
  CLIENT's MTLDevice, **cached per {id, generation}** (one +1 ref; a generation
  change releases the old texture and mints a new one). `~4us` lookup +
  `~10us` wrap, paid once per generation. `-weak_framework IOSurface` added to
  `scripts/build-bridge.sh`.
- `driver/backend/mtl_wire_emit.h` / `mtl_wire_context.cpp` —
  `nextPresentSerial()` and `acquireServerFrame()`, which calls the thunk and
  wraps the returned handle as a **borrowed** `MtlTextureImpl` + full-image
  `MtlTextureViewImpl` (identical to the presenter's drawable wrap: image
  carries no owned ref, the view retains +1 for the present's lifetime).

## Exact fallback conditions (per-present, silent, seamless)

`acquireServerFrame` returns false → present the LOCAL backbuffer this frame:
1. Wire not attached (`!g_wireAttached`).
2. Server dead — the existing S3-v4 latch (`g_serverDead`: heartbeat frozen
   >3s in server mode, or a frozen-heartbeat ring stall). Also the permanent
   transport-disable path flips back permanently.
3. `WireServerFrame_acquire` reports unusable:
   - status version < 2, or `frames == 0` (nothing presented yet);
   - **too stale**: `local_serial - frame_serial > lag_frames`
     (`kServerLagFrames = 2`, tunable const in `mtl_wire_context.cpp`);
   - `present_slot >= 8`, or `iosurface_id[slot] == 0` (empty slot);
   - `IOSurfaceLookup` / `newTextureWithDescriptor` failure.

Dual execution guarantees the local frame is always ready, so every fallback
is a correct frame.

## Autonomous correctness harness (replaces human eyes)

Both surfaces are the SAME logical frame rendered two ways (client Metal vs
server Metal), so they must be near-identical modulo known S3 diffs. The
present closure can dump BOTH for the same present serial:

- `DumpTextureToFile()` (refactored from `DumpBackbufferIfRequested`) is the
  reusable copy-to-CPU-buffer + write `{u32 w, u32 h}` + BGRA8 core.
- `CutoverVerifyDumpPair()` dumps the LOCAL backbuffer and the server IOSurface
  for the same present. Gated by **`D9MT_CUTOVER_VERIFY=1`** AND either:
  - one-shot trigger file `C:/d9mt-test/CUTOVER_DUMP` → writes
    `rt_dump_local.bin` + `rt_dump_server.bin`; or
  - auto cadence: every `D9MT_CUTOVER_VERIFY_EVERY` frames (default 600) into a
    3-deep ring `rt_dump_local_{0,1,2}.bin` / `rt_dump_server_{0,1,2}.bin`.
  No-op unless a server frame actually exists that present.
- The legacy `C:/d9mt-test/DUMP` → `rt_dump.bin` verifier is unchanged and
  always dumps the LOCAL backbuffer.

### How the coordinator triggers a dump-pair and runs the comparator

```
# launch GTA IV with D9MT_CUTOVER_VERIFY=1 (and optionally
# D9MT_CUTOVER_VERIFY_EVERY=N). Once in-game with the server alive:
touch "$BOTTLE/drive_c/d9mt-test/CUTOVER_DUMP"     # one-shot pair
# ...or just let the auto cadence write the ring.
python3 scripts/compare_frames.py \
    "$BOTTLE/drive_c/d9mt-test/rt_dump_local.bin" \
    "$BOTTLE/drive_c/d9mt-test/rt_dump_server.bin"
```

`compare_frames.py` loads both, compares RGB (alpha ignored — X8 formats), and
prints `PASS/FAIL: WxH max_delta=.. mean_delta=.. within_tol(8)=..%`. Exit 0 =
PASS, 1 = FAIL, 2 = load/usage error. Flags: `--tol N` (per-channel LSB
tolerance, default 8), `--min-within PCT` (default 99.0), `--json`.

### How to READ the comparator output (PASS/FAIL decision)

- **PASS** (within-tol ≥ 99%, small mean): the cutover is presenting the
  server's pixels and they match the client's render. Ship the flip.
- **FAIL, uniform small bias** (mean ≈ a small constant, max also small):
  gamma/sRGB or MSAA resolve-mode difference — a KNOWN S3 diff, not a cutover
  bug. Re-check with a looser `--tol`; treat as expected.
- **FAIL, localized large deltas** (low within-tol %, high max, modest mean):
  real content divergence — the `srvErr=4` dropped draws or a missing
  server-side resource. This indicts the server RENDER, not the present
  plumbing.
- **"dimension mismatch"**: server IOSurface vs local backbuffer size disagree
  — a pool/resize generation bug (the cache keys on {id, generation}, and
  resize bumps `iosurface_serial`); investigate before trusting pixel stats.

### Expected diff characteristics (document, per S3 stubs)

Near-identical is expected: textures were black in S2 but real in S3 (heap
upload path). Anticipated non-zero sources: (a) gamma/sRGB — the client present
path has no gamma LUT (dropped `DxvkSwapchainBlitter`), the server may differ;
(b) MSAA resolve mode; (c) `srvErr=4` executor-dropped draws (steady-state
render-quality items from the run-9 milestone, not survival) → a few localized
regions differ; (d) best-effort dynamic-buffer content (S3 flag 1) can make a
few draws stale server-side. So: expect a HIGH within-tolerance fraction with a
few localized mismatches, NOT a bit-exact match.

## Build gate (all GREEN)

- `scripts/build-bridge.sh` — PASS (unix `winemetal.so` compiles with the new
  thunk; x86_64 smoke test PASS). Regenerates `winemetal.def` so the driver
  import lib exports `WireServerFrame_acquire`.
- `scripts/build-driver.sh` — exit 0 (full clean: purged `build/driver/obj` for
  the header changes; d3d9.dll links; Vulkan gate PASS).
- `scripts/build-server.sh` (tail-called) — `test_feed` PASS (all S1/S2/S3
  scenarios unchanged; the cutover added no server-side change).
- `scripts/compare_frames.py` — smoke-tested (near-match→PASS, divergent→FAIL,
  dim-mismatch→FAIL).

NOTE build ordering: a NEW `WINEMETAL_API` export requires `build-bridge.sh`
to run BEFORE `build-driver.sh` (it emits the `.def` the import lib consumes).
The first `build-driver` after this change fails the link until the bridge def
is regenerated — expected.

## Run 11 — deterministic present hang: RESIDENCY + extent hardening

The first cutover (d4f3616) DEADLOCKED GTA IV deterministically: two boots, both
hung right after the first FF PSO build with 0 presented frames (DIAG=0), never
reaching the menu (pre-cutover boots to frame 14k fine). Forensic sample: the
dxvk-cs thread parked in a WINE sync primitive (msync wait, WINEMSYNC=1), a
second worker on the same address; NOTHING in winemetal.so, no Metal/present/
IOSurface frames anywhere. That signature = the CS thread is blocked on a
GPU-completion event that never fired (the completion event is a wine sync
object, not a winemetal frame) — i.e. the GPU hung on the server-frame present.

ROOT CAUSE (reconciled against the code, not just the symptom): a texture used
as a copy/blit SOURCE on an MTL4 command queue must be RESIDENT (in the queue's
residency set), or the GPU faults and the present's completion event never
signals. Driver textures get this for free — `MtlDeviceImpl::createImage` calls
`m_residency->allocCreated(handle)` ("resident from creation to retirement",
mtl_device_impl.cpp). The externally-wrapped IOSurface texture minted inside the
unix thunk BYPASSED that entirely, so it was non-resident → `copyTexture` /
`blitTextureView` from it faulted the GPU → `synchronizeWsi`'s completion never
signalled → the CS thread parked forever. Deterministic because the server has
published frames (dual execution) even at the legal screen, so `cUseServer`
became true immediately and every present hit the fault. (The extent, contrary
to the first hypothesis, was already populated — but see the belt-and-suspenders
guard below; both are now closed.)

FIX (commit ON TOP of d4f3616):
1. **Residency** — the real fix. The unix thunk reports `out_minted=1` when it
   freshly wraps an IOSurface; `acquireServerFrame` (PE) then calls
   `dev->residency().allocCreated(handle)`, batched and committed by
   `beforeSubmit` ahead of this present's submit. The unix cache became a fixed
   8-entry `{id,gen}` table that NEVER releases evicted textures (a release
   while still referenced by the residency set would re-introduce the fault) —
   a bounded per-resize leak of small wrapper objects (the 3 steady surfaces
   never evict; the shared IOSurface pixels are owned by the server pool).
2. **Extent, made impossible-by-construction** — the unix side refuses a
   zero-dim IOSurface and returns the wrap's true `[tex width/height]`;
   `acquireServerFrame` refuses `width==0||height==0` (with a warn-once); and
   the swapchain source-switch requires a non-null view+image and NONZERO
   `cServerImage->extent` before `cUseServer` stays true. Any doubt → local
   backbuffer. A GPU-hazardous present copy is now unreachable.
3. **Format/usage verified** — the wrap is `BGRA8Unorm`, `MTLStorageModeShared`,
   `usage = ShaderRead | RenderTarget` (ShaderRead for the scaled-blit render
   pass source; a valid copy source/dest).

Gate limitation (stated plainly): test_feed cannot present to a real
CAMetalLayer, so it cannot reproduce the residency fault or prove the fix.
Builds are green (bridge + driver + server). The REAL proof is the coordinator's
next GTA IV run booting PAST the legal screen to a menu with DIAG advancing and
`compare_frames.py` dumping a local/server pair. If it hangs again with the same
signature, the next suspect is cross-process GPU write/read ordering on the
shared IOSurface (the client reads it with no fence against the server's GPU
writes; frame_serial is completion-gated on the SERVER's queue, but the client's
queue has no dependency edge) — that would need an explicit cross-queue barrier
or a shared-event wait, not covered here.

## What ONLY a real-game run can prove (honest)

- test_feed and the build gates prove the SERVER machinery and that the PE code
  compiles/links; they do NOT exercise the wine PE↔unix boundary or a live
  CAMetalLayer. Whether `WireServerFrame_acquire` resolves a live IOSurface
  in-process, and whether the wrapped texture presents correctly, is proven
  ONLY by the coordinator's GTA IV run + `compare_frames.py`.
- The single-format assumption (`MTLPixelFormatBGRA8Unorm`) matches GTA's
  X8R8G8B8 backbuffer + the server pool; a non-BGRA pool would need format
  negotiation. If the comparator shows channel-swapped output (R↔B), that is
  the tell.
- The pace gate (`kServerLagFrames=2`) is tuned blind; if PROF `srvPace` is
  low or the image visibly lags, adjust the const.

## Thin-recorder TODO (the fps win — a LATER agent, NOT this slice)

The cutover kept dual execution deliberately: the client still runs its full
local hot-loop so the fallback is always ready. The fps win comes from making
the client a THIN RECORDER — stop local EXECUTION, keep only recording:
1. Client stops the local hot-loop execution (draws/state), emitting only wire
   records; the server becomes the sole renderer.
2. Route queries/readbacks/copies/blits/resolves/mips to the server (today
   still client-local — HANDOFF-S2 stub list) — nothing may read a resource the
   client no longer renders.
3. Close the remaining correctness gaps FIRST: dynamic-buffer content (S3 flag
   1), `invalidateBuffer` rename under load, BUFFER upload drops on heap-full
   (still not retried), and a client-death watchdog server-side (EVFILT_PROC).
4. Once thinned, textures become single-copy (removes the dual-execution memory
   doubling that V7 flagged as a create-hang risk).

Do this only after the blit-flip soaks and `compare_frames.py` reads PASS on a
real run.
