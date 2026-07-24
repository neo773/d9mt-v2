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

### 2. Pass preservation under uploads (server, server/executor.mm)

Staged-upload flushes no longer close render passes on the hot path:

- **Late promotion (buffers)**: a parked slab->buffer blit whose
  destination range is consumed by a draw is first re-tested against
  `completedSerial()` (`promoteParkedXfers`) — with credits bounding the
  lag, the range staged against a PRIOR frame's refs has usually retired
  by consumption time, and the blit becomes a direct memcpy (the V15
  direct path applied late). In-order walk; an entry only promotes when no
  earlier KEPT entry overlaps the same range (that blit encodes later and
  would clobber newer bytes). The pass-closing flush (`uplFlushUse`)
  remains as the counted never-fire backstop — it can still fire for a
  genuine same-frame overwrite (D1 reads old bytes, upload, D2 reads new
  bytes: only an encoder boundary can order that), which is correctness,
  not the disease.
- **Texture parking**: `upload_texture` inside an open render pass no
  longer `ensureXfer`s (which closed the pass). The slab copy is taken
  immediately (heap ack flows, stream-order bytes pinned), the blit parks
  in `deferredTexXfers` and encodes at EVERY xfer-encoder open (a natural
  boundary — ensureXfer drains the queue right after opening, which also
  preserves stream order against the whole copy family with no per-handler
  hooks). A draw sampling a parked texture forces the flush
  (`StageState::texRes` backrefs — the V15 honest flag — scanned only
  while parked blits exist). The stash-drain path (`drainStash`, create-
  race boot transient) keeps the immediate encode: it can run inside draw
  preparation AFTER the consumer-flush check, where parking would let a
  just-promoted bind sample stale content (`allowPark=false`).
- srv-stats: `upl={dir,stg,useFlush,promote,texPark}`; status v5 exports
  `upl_staged/upl_promoted/upl_flush_use` (gate assertions + HUD-ready).

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
- Full pre-existing suite green: v13/v14/v15 flood + mid-pass, v16 wrap
  race 20/20, v17/v18 texture gates, occlusion-query gates a1–c, present
  timeline (512 lockstep), kernel-wake latency p50 14 µs, pid watchdog,
  clean detach.
- Full-clean chain: `rm -rf build` → build-bridge (PASS) → build-driver
  (d3d9.dll linked, 20 exports) → build-server gate (PASS).

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
  `useFlush` ~0 with `promote=` picking up the wrap traffic; `texPark=`
  climbing during streaming with passes unaffected.
- PROF: `credit=` ≈ present rate under GPU-bound scenes (expected, healthy),
  `cBail=` 0, `texDrop=` 0, `hb=` alive through streaming lulls (the V18
  ack-wait storm that triggered the V19 wedge no longer exists).
- srvGpu should collapse from the 281 ms class toward the ~10 ms class:
  the 38 GB/frame tile load/store traffic was O(pass closes).
