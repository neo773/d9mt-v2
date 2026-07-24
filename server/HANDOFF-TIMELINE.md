# HANDOFF — present-pipeline frame timeline (measure-only instrumentation)

Instrumentation ONLY: nothing on any hot path changed behavior. One decoded
timeline line per 256 frames on each side; the goal is to name the term that
paces the ~5.9 ms uncapped frame (~170 fps) that survived the kernel-wakeup
work (p50 wake 14 µs — wake latency already exonerated).

## The pacing model AS FOUND (by reading, before measuring)

The present path is **LOCKSTEP, depth exactly 1** — not pipelined:

1. Present N enters the CS-thread closure (`d3d9_swapchain.cpp` ~808).
2. `acquireServerFrame` (mtl_wire_context.cpp) **parks up to 3 ms
   (`kPublishWaitUs`) for `status->frame_serial >= N-1`** — the PREVIOUS
   present's publication (unixcall 217, `WMTWireWaitFrameSerial`). On
   timeout it falls through and presents the freshest published frame (a
   repeat); the wait is bounded, so the client can NEVER outrun the server
   by more than one frame, and in steady state it does not time out.
3. It then wraps the freshest PUBLISHED IOSurface (serial N-1), acquires the
   drawable, encodes the present copy, and only THEN — inside
   `synchronizeWsi` — emits the frame-N marker onto the wire (the marker is
   the LAST thing the client says about frame N).
4. The server closes frame N at the marker (`handlePresent`): final
   persistent-BB → pool-slot copy, `submitFrame()` (commit), then
   publication is **GPU-completion-gated**: `frame_serial = N` is stored
   from an `MTLSharedEventListener` block when the frame's shared-event
   serial signals, then `d9mt_futex_wake_all`.
5. Present N+1 parks for that store (step 2). Loop closed.

So the frame period is the SERIAL sum of one client lap and one server lap
— nothing overlaps except the app's record emission for frame N+1 riding on
top of the server's *consumption* of frame N's draw records (the server GPU
lap and the client present lap for the same serial NEVER overlap):

    period ≈ wait(N-1 publish) + drawable acquire + copy encode + marker
             + [server: marker consume + encode tail + commit + GPU +
                listener dispatch + publish] + wake

The server runs **at most one frame behind** by construction: the marker
for N+1 cannot be emitted before N published (modulo the 3 ms timeout
fallback, which presents repeats and shows up as `skip`/repeat accepts).

## The instrumented hops

Per frame serial N (names in the output; all µs, avg/p95 per 256 frames):

| hop | where recorded | segment |
|-----|----------------|---------|
| t0 present-closure entry | swapchain lambda, CS thread | |
| t5 server frame observed/accepted | `acquireServerFrame` after the futex park | `wait` = t5−t0 (the lockstep stall) |
| t6 drawable acquired | after `acquireNextImage` | `acquire` = t6−t5 |
| t1 present records emitted | `synchronizeWsi`, marker + kick on the wire | `emit` = t1−t6 (copy encode + marker) |
| t7 blit committed + Metal4 present handshake | after `ctx->synchronizeWsi` | `commit` = t7−t1 |
| t8 present() returned | after `presenter->present` | `present` = t8−t7 |
| t2 server consumed the marker | `handlePresent` entry | `consume` = t2−t1 |
| t3 server GPU completion observed | listener block entry (includes listener dispatch) | `srvGpu` = t3−t2 |
| t4 published | just before the `frame_serial` release-store | `publish` = t4−t3 |
| t5′ of present N+1 | the present that ACCEPTS N | `notice` = t5′−t4 (publish→observed wake) |
| | | `total` = t8−t0, `frame` = t0[N]−t0[N-1] |

Chronological order per serial: t0 → t5 → t6 → t1 → t7 → t8 → t2… wait, no
— t2 (consume) races t7/t8 (the marker crosses while the client commits);
t3/t4 always follow, and t5′ lands inside present N+1. `wait` at N+1 ≈
however much of the server lap was NOT hidden under the client lap.

**Reading the line: the pacing term is whichever of
`wait`(client-visible stall) ↔ `srvGpu`(server lap) dominates; if `frame` ≫
`total`+`consume`+`srvGpu`+`publish`, something OUTSIDE these hops paces
(e.g. app-side/CS-queue gaps between presents).**

## Clock domain (cross-process, cross-ISA)

- Server hops: `mach_absolute_time * numer / denom` → ns. mach_absolute_time
  is machine-global (same hardware counter for the Rosetta x86_64 client and
  the native arm64 server).
- Client marks: QPC (Wine implements it on the same mach timebase, 10 MHz).
  Converted into the mach-ns domain with ONE offset, measured by bracketing
  the new `WMTWireWaitTimeTap` unixcall (217 op 2 — returns mach ns, no
  wait) between two QPC reads; error ≤ half a thunk round trip (µs-class),
  recalibrated once per 256-frame window.
- Transport: server hops travel in the status page (`d9mt_status.h` v4
  `tl_*[16]` ring, serial-indexed; `tl_serial` release-stored BEFORE
  `frame_serial` so an acquire that sees serial N finds finished hops) and
  are shipped to the PE side inside `WMTServerFrame` (extended unixcall 216,
  96 B, static_assert-pinned both sides).

## Where the lines appear

- **Client** (`D9MT_PROFILE=1`, same 256-frame cadence as PROF):
  `PROF timeline win=256 skip=0 us(avg/p95): wait=… acquire=… emit=…
  commit=… present=… | srv: consume=… srvGpu=… publish=… notice=… |
  frame=… total=…`
  `skip` counts presents that never reached t8 (skip-present/no drawable).
  Server segments print `(n=…)` when fewer samples than the window arrived
  (pre-v4 server / recycled tl slot / repeats).
- **Server** (always, on the srv-stats cadence):
  `[serverd] srv-timeline: markGap=a/p srvGpu=a/p publish=a/p us (avg/p95,
  win=256)` — markGap is the frame-marker arrival period (the pacing as the
  server sees it).
- **test_feed gate** ("PRESENT-TIMELINE GATE"): 512 lockstep presents
  (present → futex-park for THIS serial), decodes the tl ring into
  `[test_feed] timeline win=256 us(avg/p95): emit=… consume=… srvGpu=…
  publish=… notice=…`. Asserts publication per frame, tl-slot serial match,
  hop monotonicity. One process, mach clock only, no wine — the wine-free
  floor of the pipeline.

## Gate numbers (this machine, headless clear-only frames)

Two 256-frame windows over 512 lockstep presents (x86_64 Rosetta feeder →
native arm64 server, MAP_SHARED — the production pair):

    [test_feed] timeline win=256 us(avg/p95): emit=2/6 consume=3/7 srvGpu=281/351 publish=0/0 notice=9/22
    [test_feed] timeline win=256 us(avg/p95): emit=3/6 consume=4/7 srvGpu=267/324 publish=0/0 notice=9/20
    [serverd]   srv-timeline: markGap=291/342 srvGpu=273/323 publish=0/0 us (avg/p95, win=256)
    [test_feed] present-timeline gate: 512 lockstep presents, all tl slots matched, hops monotonic

Read: the wine-free lockstep floor is ~290 µs/frame (≈3.4 k presents/s) and
it is ENTIRELY `srvGpu` (~270-280 µs = clear-encode + commit + GPU + listener
dispatch for an empty frame — largely the fixed commit+completion+listener
cost). Transport is invisible: marker crossing (`consume`) 3-4 µs, publish
store 0, publish→observe wake (`notice`) 9 µs avg / 22 µs p95. None of the
IPC hops can explain 5.9 ms; whatever paces the live run lives in `srvGpu`
(real GPU work + completion chain), `wait`/`acquire`/`emit`/`commit`
(client-side, wine + drawable), or between-present gaps (`frame` vs `total`).

## What the live cube run disambiguates

The gate has no wine emit path, no drawable, and near-zero GPU work, so it
can only bound `consume`/`publish`/`notice`. The live run separates:
(a) lockstep publish-wait — `wait` dominates and ≈ `frame` − client lap;
(b) server GPU/completion chain — `srvGpu` dominates (then split encode tail
vs listener dispatch next);
(c) drawable pacing — `acquire` dominates (CAMetalLayer pool);
(d) wine present overhead — `emit`+`commit`+`present` dominate.
