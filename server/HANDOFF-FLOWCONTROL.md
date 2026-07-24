# HANDOFF — FRAME-CREDIT FLOW CONTROL (the designed producer/consumer coupling)

Branch `native-backend`, on top of 8db7c27. Predecessors: HANDOFF-S3.md
(V15 upload staging, V18 stash/ack-wait, V19 liveness), HANDOFF-WAKEUPS.md
(futex machinery this rides), HANDOFF-TIMELINE.md (the lockstep pacing
model), HANDOFF-QUERIES.md (the workload restore that made server rate
sane).

## Why (the architecture verdict this implements)

The system had NO designed producer/consumer coupling. Every overload
symptom was an ad-hoc defense at a different queue: stash caps + bounded
ack-waits (V18), heartbeat/liveness patches for the stalls those waits
caused (V19), useFlush pass closes (V15), ring stall windows, multi-second
consumer lag. Measured failure (GTA street): consumer 2.6 s behind,
~1,170 render passes/frame server-side of which ~1,085 were staged-upload
flush closes (useFlush = 292k/window) — on TBDR ~38 GB/frame of load/store
traffic (srvGpu 281 ms). The minimized-game experiment (server caught up →
PERFECT textured frame) proved rendering correct and LATENESS the whole
disease. This slice replaces the zoo with ONE mechanism.

## The mechanism

### 1. Frame credits (client, driver/backend/mtl_wire_context.cpp)

The client may run at most **N frames (default 2, `D9MT_FRAME_CREDITS`,
0 = disable)** ahead of the server's GPU-COMPLETED frame serial
(`status->frame_serial` — publication is completion-gated by the
MTLSharedEvent listener, so "published" == "pixels done"). Enforced at ONE
choke point: `acquireServerFrame`, which every present passes BEFORE its
marker is emitted (`synchronizeWsi` runs later in the same CS closure).

- Target domain is the **MARKER counter** (`g_wt.presentSerial + 1 - N`),
  not the local present counter — the two can drift (non-claimant
  contexts, wire-down presents) and a drifted target would be unreachable.
- Wait shape: zero-timeout probe, then sliced kernel parks (100 ms slices
  on the existing `WMTWireWaitFrameSerial` futex; the executor wakes the
  address at every publication). MANDATORY but liveness-safe:
  - `frame_serial == 0` (boot) never gates — same exemption the unix wait
    already had; backlog during the boot window is bounded by server
    startup (one frame once running).
  - dead-server latch (`g_serverDead`) breaks out — the disable path
    stands untouched;
  - **progress bail**: if NO publication lands for 3 s while parked
    (dead process the latch hasn't seen yet, or a wedged-GPU-but-
    heartbeating pipeline) the wait falls through to the pre-credit
    behavior, counted (`cBail=`, must stay ~0). A merely-slow server
    publishes every frame and simply paces us — it can NEVER trip this.
- This is D3D9's own frame-latency contract applied across the wire:
  `SyncFrameLatency` paces the app thread against CS presents (frontend,
  unchanged); credits pace CS presents against server completions. The two
  compose: app ≤ latency behind CS, CS ≤ N ahead of the server GPU.
- Result by construction: ring/heap backlog ≤ N frames of records; worst
  case fps = min(client rate, server rate).
- PROF: `credit=` (window presents that parked), `cwMs=` (window ms spent
  parked — under overload this approaching the window wall-clock is the
  mechanism WORKING), `cBail=` (cumulative, ~0).

### 2. Pass preservation under uploads (server, server/executor.mm) — P2 shape

History: this section's original mechanism (park + promote for buffers,
park + consumer-FLUSH for textures) was partially DELETED by P1 on the
assumption that blob-backing zeroed the upload flood. Live GTA streaming
falsified that: the residual non-blob class (blobFb fallbacks, GPU-private,
pool misses) still staged ~69k uploads/window with useFlush=66k — ~260 pass
closes/frame, srvGpu ~19 ms, and a flickering black tile grid during
streaming windows (pass-shaped fallout of the flood; load/store actions
verified Load/Store — see the tripwires). texPark read 25/window NOT
because texture parking was too narrow but because the BUFFER staged path
shattered the pass first: upload_texture then usually arrived with no pass
open and encoded plainly. P2 kills both closers by construction:

- **Buffer parking + late promotion (RESTORED, redesigned)**: a staged
  upload inside an open pass parks in `deferredBufXfers` (slab copy taken,
  heap acked; `blockSerial` = the resource's ref watermark at staging = the
  GPU readers that forced staging). A consuming draw
  (`promoteOrFlushParkedBufXfers`, behind the O(bindings)
  `boundBufHasPendingXfer` pre-check) promotes the blit to a direct memcpy
  when `completedSerial() >= blockSerial` AND no earlier kept entry
  overlaps AND the destination is shared-storage — else the counted
  pass-closing flush backstop (genuine same-frame overwrite / GPU-private
  dst; correctness, not the disease). CRITICAL DESIGN POINT: the park does
  NOT stamp `markBufRef` (that poisons blockSerial of every LATER parked
  upload on the same resource into the flush backstop — the mid-pass gate
  caught this); parked-range liveness is a separate clause in `rangeLive`
  (`Resource::pendingBufXfers + bufXferMin/Max`, reset when the count
  returns to zero). The promoted memcpy DOES markBufRef (its bytes are now
  observable by encoded work). Parked entries never outlive their frame —
  submitFrame drains.
- **Texture parking, flushless consumption**: `upload_texture` inside an
  open pass ALWAYS parks (the old `allowPark=false` stash-drain exception
  is gone). A texture delivery NEVER closes a render pass; same-pass
  consumption resolves by semantics instead:
  - texture HAD content before the first parked blit (`Resource::
    texStaleOk`, decided at the first park of a run): binds real-resolve
    and draws sample the PREVIOUS texels — the blit encodes after the pass
    in the same cmdbuf, so this is deterministic stale-by-one-pass
    (upl_tex_late, counted once per draw). D3D9 apps updating+sampling in
    one frame see their new texels one pass late — visually benign for
    streaming.
  - texture had NO content (FIRST upload still parked — private storage
    undefined until the blit runs): the bind gate (handleBindTextureById /
    promotePendingTexBinds) keeps the slot dummy+pend for this pass
    (upl_tex_dummy) and the V18 promote re-resolves it real on the first
    draw after the boundary drain. `noteParkedTexSampling` also carries a
    defensive dummy-for-this-pass downgrade should a real-bound slot ever
    hold parked first content.
- **Boundary drains**: both queues drain at EVERY xfer-encoder open
  (ensureXfer — covers the whole copy family with no per-handler hooks),
  at every pass materialization (BEFORE the render encoder — a parked
  upload must land ahead of a pass that samples or attaches the resource),
  at the frame's submit, and explicitly in the custom-pass handlers
  (resolve / meta-blit / partial clear) which build their own encoders.
- **TBDR tripwires** (materializePass, counted into exec_errors, asserted
  0 by the gates): no attachment ever materializes with DontCare LOAD
  (pre-existing) and — P2 — none with a non-Store STORE action: any pass
  split mid-frame must Load and Store ALL attachments.
- srv-stats: `upl={dir,stg,useFlush,promote,texPark,texLate,texDummy}`;
  status v6 adds `upl_tex_parked/upl_tex_late/upl_tex_dummy` (776/784/792,
  additive; size 800) to v5's `upl_staged/upl_promoted/upl_flush_use`.

### 3. Deleted/simplified ad-hoc defenses

- **heapAllocWait** (V18 bounded 50 ms ack-wait storms) — REMOVED: credits
  bound the consumer's lag to N presents, so heap acks (published at
  record execution, V19 audit) arrive within N presents and the stash
  drains at the next bind/present sweep. This was also the V19 wedge
  trigger (producer stalls burning the CS thread into the lull).
- **Stash soft cap** (`kMaxPendingUploadBytes`, 96 MiB) — REMOVED: its only
  role was engaging the ack-wait. The 768 MiB HARD cap stays as the
  never-fire backstop with counters (`texDrop=` + first-8 named); the
  invariant "content is never lost below the hard cap" is unchanged.
- PROF `texWait=`/`hWaitMs=` fields retired with the mechanism.
- KEPT as never-fire backstops (all counted): `uplFlushUse` pass closes,
  ring `waitForSpace` stall windows + dead-server disable, heap
  drop-on-full for re-shipped-every-frame buffer content, stash hard cap.

## Gates (server/test_feed.c, run by scripts/build-server.sh — ALL green)

- **(a) sustained overload**: feeder mirrors the client credit rule
  against a server throttled to 4 ms/frame (`D9MT_TEST_FRAME_DELAY_US`
  test hook in handlePresent; feeder is unpaced ~µs/frame = far past 5x
  overload). 150 frames: **emission loop 721–777 ms (== consumer rate;
  unpaced would be ms-total), 147 credit parks, max backlog 2 == N, no
  stall (every park resolved ≪ 5 s), all frames published, final frame's
  pixels correct**.
- **(b) upload storm / pass preservation**: 10 iterations of
  frame A (16 draws referencing constant slices) → frame B (overwrite the
  SAME slices while A races completion — the staged path — then consume
  after A publishes): **20 passes for 20 frames, useFlush = 0,
  staged = 157–158 and promoted = 157–158 (every staged blit promoted to
  memcpy), 320/320 draws + 160/160 uploads executed, final bytes land
  where the consuming draw reads** (readback magenta/clear exact).
- **(c) V19 liveness unchanged and green**: idle-liveness (10 s silence,
  hb every second), load-lull-load (second storm flows), wedge sim —
  credits never trip dead-server detection on a merely-slow server (the
  progress-bail requires a 3 s publication gap; the throttled-consumer
  gate parks 147 times without a single bail).
- **P2 (a) mid-pass buffer storm**: pass OPENED first (helper-cbv draw so
  the watermark stays honest), then 16 uploads to a PRIOR frame's
  referenced slices MID-PASS, then same-pass consuming draws: **6
  iterations -> 12 passes for 12 frames, useFlush=0, staged=96
  promoted=96 (every parked blit promoted at its consuming draw), final
  bytes land (magenta/clear exact)**.
- **P2 (b) texture streaming storm**: 8 frames x (draw + 8x[upload_texture
  mid-pass + bind + draw]) on one texture: **9 passes for 9 frames,
  useFlush=0, texPark=64, texLate=64, per-frame pixel assert that every
  same-pass draw sampled the PREVIOUS content (late-by-one), landing frame
  shows the new content, exec_errors=0 (load+store tripwires)**.
- **P2 (c) first-upload-then-sample-same-pass**: bind+draw (dummy), FIRST
  upload mid-pass, rebind+draw: **screen stays dummy-black for that pass
  (never undefined storage, never a pass close), WHITE lands the next
  pass, texDummy counted, useFlush=0**.
- Full pre-existing suite green: v13/v14/v15 flood + mid-pass, v16 wrap
  race 20/20, v17/v18 texture gates, occlusion-query gates a1–c, present
  timeline (512 lockstep), kernel-wake latency p50 14 µs, pid watchdog,
  clean detach.
- Full-clean chain: `rm -rf build` → build-bridge (PASS) → build-driver
  (d3d9.dll linked, 20 exports) → build-server gate (PASS).

## P3 — GPU-written frame feedback (the notification dead-time kill)

Safehouse evidence: frame 30 ms with client work 0.7 ms and real GPU ~1 ms;
`notice=` 14–19 ms of published-frame-sitting-idle — the
MTLSharedEventListener publication is a GCD dispatch (documented
semaphore-hop, ms-class) and the lockstep loop compounds it every frame.
Venus (`vn_feedback.c`) solves this class by having the GPU ITSELF write
completion values into shared memory in submission order.

Landed (this slice):
- **The word** (`mtl_wire.h D9MT_WIRE_HEAP_FEEDBACK_OFFSET` = 8192, inside
  the heap header pad the whole-heap wrap already covers): packed
  `(frame_serial << 2 | present_slot)`. `encodeFrameFeedback`
  (executor.mm, called by submitFrame after the final closeXfer) encodes a
  fresh compute encoder: `barrierAfterQueueStages:kStageAll` then an
  8-byte `copyFromBuffer` into the heapBuf — the LAST work of every
  present's cmdbuf, so an observer of the word knows the frame's pixels
  (publish copy included) are done. Empty/completed frames CPU-store the
  word inline; session boundaries zero it (same staleness semantics as
  `frame_serial` across sessions); legacy heap layout (no wrap) leaves it
  0 = clients degrade to the listener path.
- **The readers** (winemetal_unix.c — NO PE-side driver change needed):
  `wire_feedback_word()` double-loads (blit copies have no formal
  atomicity; torn read = absent this poll, monotonicity recovers).
  `WMTWireWaitFrameSerial` (credit gate + publish notice) takes
  `max(status->frame_serial, word>>2)` with a ~400 µs / 20 µs poll phase
  then 1 ms-sliced futex parks (the GPU write has no waker; the listener
  wake still short-circuits). `WireServerFrame_acquire` acquires the
  fb-led `{serial, slot}` — without this the early wake would still
  present the listener's stale slot. tl_* segments for fb-led frames read
  0 (tl_serial mismatch) and are skipped — client `notice=` may go
  negative-ish/small; trust the fps and `srvGpu`.
- **Blocking waits**: server-side GPU blocks already use
  `MTLSharedEvent waitUntilSignaledValue:timeoutMS:` (waitSerial) — direct
  kernel wait, no listener. The listener now serves ONLY bookkeeping
  (status mirrors, tl instrumentation, futex wake backstop): nothing on
  the frame path blocks on it.
- **Gate** (test_feed "P3 feedback"): 64 present frames — the word arrives
  for every frame, never lags the listener beyond scheduler jitter
  (>100 µs lag on >3/64 fails), and its packed slot matches the
  listener-published slot per serial.
- Cross-process `MTLSharedEventHandle` (client-side kernel wait on the
  server's event) is NOT needed for the frame loop anymore; if a future
  slice wants it for the long credit parks, it requires a mach-port
  channel (XPC/spawn-inherited) — the shm files cannot carry it.

DEFERRED (designed, not landed — next slice):
- **Publish-copy elision**: frames whose first backbuffer pass begins with
  a folded full clear (every 3D frame) could render DIRECTLY into the
  pool's IOSurface slot, keeping persistentBB+copy only for carry-over
  composition (menus/CRT-scan). Touches set_rt resolution + the
  IOSurface-as-RT usage flags + the quadrant gate contract; the copy is
  GPU-cheap (~0.5 ms class), so this is second-order vs the listener win.
- **Present zero-copy** (client): CALayer.contents = IOSurface directly
  instead of the CAMetalLayer drawable blit (kills drawable acquire +
  fullscreen blit). Needs CATransaction/scale handling against the
  winemac layer hierarchy; keep the blit path behind a build-time
  constant as fallback. Verify with the coordinator-run demos.

Expected live signature: safehouse frame 30 ms -> single-digit
(0.7 ms client + real GPU + µs-class notice); `wait`/`notice` collapse;
street unlocked in proportion.

## Tuning

- `D9MT_FRAME_CREDITS` (client env): N, default 2. 1 = strict lockstep
  (lowest latency, least overlap); larger N = more pipelining headroom and
  more staged->promoted traffic; 0 = disable (bisection kill-switch —
  restores the pre-credit uncoupled behavior).
- `kCreditStallMs` (3000, compile-time): the progress bail. Matches
  `kServerDeadMs`.

## Expected GTA signature

- Consumer lag bounded: `wait`/`cwMs` absorb what used to be seconds of
  backlog; presents show the server's CURRENT frame (no more multi-second
  stale streets); fps = min(client rate, server rate) — the game may read
  lower fps than the old free-running client counter while LOOKING vastly
  better, because the old number was frames into a queue, not frames on
  screen.
- srv-stats: passes/frame back to scene shape (~60–80, not ~1,170);
  `useFlush` ~0 IN ALL WINDOWS INCLUDING STREAMING with `promote=` picking
  up the residual staged traffic and `texPark=`/`texLate=` climbing during
  streaming with passes unaffected; `texDummy=` small (first-sight
  textures only). The streaming-window signature specifically: the old
  stg=69k -> useFlush=66k (~260 closes/frame, srvGpu ~19 ms, flickering
  black tile grid) becomes stg~69k -> promote~stg, useFlush~0, passes
  scene-shaped, grid gone, srvGpu 12–15 ms.
- PROF: `credit=` ≈ present rate under GPU-bound scenes (expected, healthy),
  `cBail=` 0, `texDrop=` 0, `hb=` alive through streaming lulls (the V18
  ack-wait storm that triggered the V19 wedge no longer exists).
- srvGpu should collapse from the 281 ms class toward the ~10 ms class:
  the 38 GB/frame tile load/store traffic was O(pass closes).
