# HANDOFF — S3 slices 1+2 (async PSO + content transport), agent D

Phase S3 of docs/RENDER_SERVER_SPEC.md, slices 1 and 2: the server compiles
PSOs ASYNCHRONOUSLY (ring consumption never blocks on MTL4Compiler), and
texture/buffer CONTENT crosses via the `shared-heap` file — real texture
binds replace the S2 dummy substitution. Frame publication is now
GPU-completion-gated. Read server/HANDOFF-S2.md first; this document covers
only the S3 delta. THE VISUAL CUTOVER IS NOT DONE — the client still
presents its own locally-rendered backbuffer (see the cutover TODO at the
bottom).

## Slice 1 — the GTA IV boot fix (shader storm vs. ring)

Root cause chain (S2): synchronous `newRenderPipelineStateWithDescriptor:`
on the consumer thread → ring consumption stalls during boot shader storms →
ring (4 MiB) fills → client watchdog trips after ONE ~1 s window →
`MtlWireContext: ring stalled >1s, disabling wire transport` (permanent).
Three-part fix:

1. **Async PSO builds** (executor.mm): the PSO cache now maps PsoKey →
   `shared_ptr<PsoEntry>{pso, atomic state}`. A miss kicks
   `newRenderPipelineStateWithDescriptor:compilerTaskOptions:completionHandler:`
   and reports PENDING; the draw is skipped and counted in BOTH
   `draws_skipped` and the new `draws_skipped_pending_pso` (status v3, 200).
   `dirtyPso` stays set so the next draw re-resolves. The completion handler
   (compiler thread) writes `entry->pso` then release-stores the state;
   consumer acquire-loads. Failed builds park at Failed forever (S2 nil-cache
   policy; counted into exec_errors by the handler). `psos_pending` (status
   v3, 208) tracks in-flight tasks.
2. **Ring 16 MiB** (mtl_wire_context.cpp WireRingCapacity): S1 evidence was
   ~665 KB/frame in-game; boot bursts (chunked metallibs + creates) are far
   larger.
3. **Watchdog burst tolerance** (MtlWireContext::waitForSpace): the single
   ~1 s spin window became up to `WireStallWindows` (5) windows with a
   `Logger::warn` per window (PROF near-miss trail: `stalls` now counts one
   per window). A window in which the consumer advanced `tail` at all resets
   the count — only 5 CONSECUTIVE zero-progress windows (a dead/stuck
   consumer) trip the PERMANENT disable, which remains the safety net.

**GPU-completion-gated publication** (S2 flag 3, small but load-bearing for
the cutover): `handlePresent` no longer stores {present_slot, frame_serial,
frames} inline after commit. It registers the frame's serial on an
`MTLSharedEventListener` (serial queue, QOS_USER_INTERACTIVE) and the block
publishes AFTER the GPU signals — status `frame_serial == N` now means "slot
pixels are done". test_feed proves it by asserting the presented pixel on
the FIRST read, no poll.

## Slice 2 — content transport (shared heap)

### The `heap` file (mtl_wire.h `struct d9mt_wire_heap`)

`$TMPDIR/d9mt-<pid>/heap`, created by the client at attach: 192 B header,
data at offset 16384 (D9MT_WIRE_HEAP_DATA_OFFSET), 256 MiB sparse data area
(WireHeapCapacity). PE side VirtualAllocs the region; the unix attach maps
the file MAP_FIXED over it (exact ring recipe). `WireRing_attach` gained
heap_base/heap_bytes/heap_ok (thunk struct extended in lock-step); heap
failure of any kind degrades to a ring-only attach (heap_ok=0, uploads
simply never emitted).

Allocator (client, CS thread only): sequential bump with free-running `head`
cursor; regions are 256-aligned and NEVER straddle the wrap (skip to
boundary). Every region's GENERATION = its free-running END cursor. Server
acks by storing a generation into `heap->consumed` (+ status v3
`heap_consumed` mirror); producer space check is `head - consumed <=
capacity - need`. A FULL heap NEVER stalls the CS thread: the upload is
dropped (`heap->dropped++`, one warn) and the server keeps stale content.

### Records (ops 33..35, ABI-appended)

| op | record | emitted from |
|---:|---|---|
| 33 | upload_buffer {ids, dst_offset, size, heap_offset, generation} | ensureStorageCreated (initial snapshot of host-visible backings), uploadBuffer tap, invalidateBuffer refresh |
| 34 | upload_texture {ids, mip/layer, box, row/slice pitch, format, heap region} | copyBufferToTexture tap (THE texture upload seam; pitch math mirrors computeBufferImagePitch byte-for-byte) |
| 35 | bind_texture_by_id {stages, slot, image wire id, view key bits} | bindTexture (twin of op 3, like the op 10/30 pattern) |

Client emission is additive dual-execution: the local copy still happens;
the wire copy is extra. Emission points:
- **ensureBufferCreated → ensureStorageCreated** (renamed/refactored): after
  the create record, host-visible backings (mapPtr != null) snapshot their
  FULL content — catches static VB/IB written via Lock before first bind.
- **uploadBuffer**: staging bytes → heap region → upload_buffer (constant
  buffer path, d3d9_constant_buffer.cpp:144).
- **invalidateBuffer** (Discard rename): (a) create for the new backing,
  (b) content snapshot, (c) REBIND records for every wire-visible binding
  that referenced the logical buffer — MtlWireContext keeps shadows
  (m_streamShadow/m_uboShadow/m_indexShadow) of what op-level binds carried,
  and re-emits plain bind_vertex/uniform/index records with the refreshed
  slice. This closes HANDOFF-S2 flag 4 (server drawing from retired
  backings) — S3 checklist item 5 done.
- **copyBufferToTexture**: staging bytes (src->mapPtr(off)) → heap →
  upload_texture per subresource box.

### Server side

- Upload records **memcpy the heap region into the upload slab ring**
  (RingSlice gained buf/off) and blit slab→resource inside a shared
  MTL4ComputeCommandEncoder (`ensureXfer`/`closeXfer`) that closes — with
  the §4c producer barrier (Blit|Dispatch → all) — before the next render
  pass materializes and before every submit. Stream position matches the
  client's prepareDirectEncode shape.
- **ZERO-COPY WRAP IS UNSOUND — measured, not assumed**: wrapping the whole
  heap mapping once with newBufferWithBytesNoCopy wires the pages at wrap
  time, when the sparse file still resolves to zero-fill pages; client
  writes that fault in fresh pagecache pages later are invisible to the GPU
  (CPU saw the checkerboard through BOTH mappings, the GPU blit produced
  zeros). If zero-copy matters later: per-region wraps created AFTER the
  bytes exist (16 KiB-aligned page ranges), or pre-materializing the file
  (non-sparse) before the wrap. The slab copy is a native arm64 memcpy and
  lets the ack advance IMMEDIATELY at record execution (no GPU-lifetime
  tracking of heap regions).
- **Real texture binds**: bind_texture_by_id resolves the wire texture; the
  1x1 dummy remains ONLY for unresolved ids and CONTENT-LESS textures
  (private storage with no upload and no server-side render is undefined —
  `Resource::hasContent` gates it; set by uploads, RT use, clears). Non-
  trivial view keys mint `newTextureViewWithPixelFormat:textureType:levels:
  slices:` views, cached per Resource, retired via `retireNoRes` (views
  never joined the residency set — the parent is the resident allocation).
  Both the classic per-slot table writes and the AB handle-word path use the
  resolved texture.

## status v3 / stats / PROF (all additive)

- d9mt_status.h: version=3, +draws_skipped_pending_pso(200),
  psos_pending(208), uploads_executed(216), heap_consumed(224); 232 B total.
- WMTWireStats: +server_skipped_pso, server_uploads, heap_consumed,
  heap_dropped (unix side reads the heap header directly — same process).
- PROF line: `psoSkip=<delta> upl=<delta> heapDrop=<cum>` appended. First
  real-game run: psoSkip spikes at boot then goes quiet; heapDrop stays 0;
  stalls may show near-miss windows but the transport must survive.

## Test evidence (scripts/build-server.sh gate, PASSING end-to-end)

`test_feed` (x86_64, Rosetta boundary) now runs: S1 checks; F1 red frame
under a COLD PSO — presented with 50 pending-PSO skips, consumer never
blocked; F2 after psos_built>=1 — 50 draws executed, presented pixel RED on
the FIRST read (completion gate); F3/F4 — 64x64 checkerboard via
create_texture + heap upload_texture + bind_texture_by_id + textured
fullscreen draw (test_stub.metal vsTex/fsTex, constexpr sampler), pixel
readback asserts alternating checker cells; COMPILE STORM — 64 distinct
shader pairs (same metallib bytes, distinct client handles → 64 libraries →
64 PSO keys), each drawn immediately: frame presents while 64 draws skip on
in-flight compiles, the producer NEVER waited for ring space, then
psos_built +64 / psos_pending 0, and a second wave draws all 64
(draws_executed delta == 64, exec_errors 0, ring stalls 0); detach asserts
heap->consumed == produced heap head. build-bridge.sh smoke PASS; full-clean
build-driver.sh PASS (both mingw targets; header change = obj purge rule).

## Honest flags / gotchas

1. **Dynamic-buffer content is BEST-EFFORT racy**: invalidateBuffer's
   snapshot runs on the CS thread at the Lock's stream position — the app
   may still be writing the renamed slice (Unlock hasn't happened). Bytes
   are defined-but-possibly-stale server-side. The correct refresh point is
   Unlock/draw-time dirty tracking; not built. Same for CPU writes into
   never-renamed host-visible buffers AFTER first bind (no seam sees them).
2. **Texture binds don't self-heal on late content**: a texture bound while
   content-less stays dummy-bound until the game re-binds it (D3D9 games
   rebind constantly; uploads also normally precede binds).
3. **view_type reinterpretation is not modeled** — only format + level/slice
   ranges; exotic reinterpretation falls back to the base texture.
   `resolveTexView` clamps out-of-range keys to the base texture.
4. **create_shader library creation is still synchronous** on the consumer
   (`newLibraryWithData` ~ms). Only PSO builds went async. A metallib storm
   is bounded by ring throughput; not observed as a stall source.
5. **Upload slab growth**: a single upload > 2 MiB mints a slab of that size
   which is kept (rewound, never freed). Initial-content snapshots of very
   large buffers can pin memory server-side; client caps a region at
   heapCap/2 (128 MiB).
6. **heap full ⇒ silent visual staleness** server-side (dropped uploads,
   counted + one warn). By design (never stall CS); watch `heapDrop` in PROF.
7. **256 MiB VirtualAlloc in the (possibly 32-bit) PE process**: reserve+
   commit succeeded in builds, but a fragmented 32-bit VA could fail —
   handled (warn + ring-only attach), only content transport is lost.
8. **Frames counter now lags GPU completion**; anything reading
   status.frames as "records executed" semantics should use it as "frames
   COMPLETED". srvPace in PROF inherits the stricter meaning.
9. **Leaked-server gotcha when a synthetic client dies pre-detach** (S1 flag
   still open): a FAILing test_feed leaves d9mt-serverd alive holding the
   shell pipe — kill `d9mt-serverd` processes whose shm dir is `d9mt-test-*`
   if a gate run ever hangs. EVFILT_PROC watchdog remains unbuilt (S3
   checklist item 6).
10. **Kick channel still 1 ms poll** (S3 checklist item 4, untouched).

## V2 — boot-flood fixes (real-game feedback round)

GTA IV boot verdict on v1: watchdog worked as designed (5 warn windows,
clean disable, game unharmed) but the wire still died — two chokes, both
from v1's own flags list (1 and 4/6). Fixed:

### Create worker (v1 flag 4): newLibraryWithData off the consumer

`executor.mm` gained a dedicated create worker (`createWorkerMain`,
std::thread + mutex/condvar deque, QOS_USER_INTERACTIVE like every server
thread). The consumer's create_shader handler still accumulates chunks
(memcpy) but on the final chunk it registers the Resource IMMEDIATELY in
state `ready=0` (building) — so the very next bind_shader resolves it — and
queues the expensive `newLibraryWithData` + Shader construction to the
worker, which publishes via `Resource::ready` (0 building / 1 ready / 2
failed; payload written before the release-store, consumer acquire-loads).
bind_shader of a building resource parks it in `vsPend/fsPend`;
`promotePendingShaders()` at draw time promotes ready ones (dirtyPso) and
PENDING ones skip the draw, counted in `draws_skipped` AND
`draws_skipped_pending_pso` (meaning broadened: async PSO **or** library
build in flight — d9mt_status.h comment updated). The consumer thread now
only ever decodes + dispatches + memcpys.

Consequence worth knowing: a pipeline needs a DRAW ATTEMPT after the
library lands to even START its PSO compile — nothing precompiles
speculatively. Real games draw every frame so this self-heals in 1-2
frames; test_feed converges with explicit draw waves
(`draw_wave_converge`).

### Heap ack starvation + lazy re-upload (v1 flag 6 hardening)

The observed heapDrop=281 was DOWNSTREAM of the same choke: acks advance at
slab-memcpy time on the consumer, and the consumer was stuck in
newLibraryWithData — fix (1) restores ack flow (verified: 300 MB cycles
through an 8 MiB test heap with 32 producer backpressure waits and zero
timeouts). Independently, dropped TEXTURE content is no longer lost
forever: `MtlWireContext` stashes the bytes (`m_pendingTexUploads`, capped
at 96 MiB `kMaxPendingUploadBytes`) and retries — per-image at its next
bind_texture_by_id (upload lands BEFORE the bind so the server's
hasContent gate is satisfied by draw time) and a full sweep once per
present. FIFO per image is preserved: a new upload for an image with
stashed content queues behind the stash (subresource write order). Retries
probe the heap QUIETLY (no double heapDrop count). `heapAlloc` gained the
`quiet` param.

### Test evidence v2 (scripts/build-server.sh gate, PASSING ~1 s wall)

Existing scenarios restructured to convergence draw-waves (see consequence
above), plus the BOOT FLOOD: 200 shader libraries + 300 MB of texture
uploads (3 x 1 MiB x 100, interleaved with binds/draws/presents) through an
8 MiB heap — producer NEVER waited for ring space, heap acks flowed (32
bounded backpressure waits, no timeout), frames kept presenting, 100
pending-create skips counted, then the drain wave drew 100/100 in 2 waves
with psos_pending=0, exec_errors=0, ring stalls=0; detach acked
315,621,376 heap bytes. Full-clean build-driver.sh PASS.

### V2 flag updates

- v1 flag 4 (sync newLibraryWithData) — FIXED (create worker).
- v1 flag 6 (heap full = silent staleness) — SOFTENED for textures (lazy
  re-upload stash); BUFFER content drops (initial snapshots / uploadBuffer /
  invalidate refresh) are still not retried — dual-execution hides it today,
  must close before the thin-recorder step.
- NEW: create_buffer's zero-fill memset and create_texture/sampler Metal
  object creation still run on the consumer — cheap in practice (memset
  GB/s; object creation µs) but unmeasured under a worst-case boot; the
  worker queue is general enough to take them if a capture shows it.
- NEW: the create worker is ONE thread — a pathological metallib storm
  serializes behind it (but never blocks the consumer). Parallelize only
  with evidence.
- NEW: pending-upload stash keys on the IMAGE (Rc keeps it alive); a game
  that uploads once and never re-binds relies on the per-present sweep.

## V3 — device churn (the throwaway boot device) + orphan sweep

Real-game retest of v2 found a failure EARLIER in the timeline than the v2
fixes: GTA IV creates a throwaway D3D9 device at boot (caps enumeration),
destroys it, then creates its real device. The S1-era design had (a) the
claiming MtlWireContext's dtor run FULL teardown (Detach + SIGTERM +
unlink) and (b) a once-per-process ring claim — so the temp device's death
killed the server cleanly and the real device could never attach (evidence:
server exited, shm dir cleanly unlinked, zero stall warnings, game booted
wireless).

Fix (option a — release + respawn; the server boots <1 s):
- **Claim release** (mtl_wire_context.cpp dtor): after WireRing_detach +
  region frees, the dtor clears cross-epoch state (queued destroys — the
  next server's resource table starts empty; the backbuffer Rc — it pins a
  dead device's image) and release-stores `g_wireClaimed = false`. The next
  MtlWireContext attaches from scratch: fresh VirtualAlloc regions, fresh
  spawn. Resource wireIds are per-device objects and die with the device; a
  theoretical survivor carries a stale id and stays dummy-bound (defined).
- **Attach sequence dirs** (winemetal_unix.c): the shm dir is now
  `$TMPDIR/d9mt-<pid>-<seq>` (per-process attach counter) so churn can never
  collide with a stale dir. The unix attach/detach state machine already
  reset all its globals on detach — re-attach worked unix-side; only the PE
  claim and the dir name needed changing. PROF resets its delta baseline
  when counters regress (new server = counters restart).
- **Boot-time orphan sweep** (wire_sweep_orphans, called per attach): every
  `$TMPDIR/d9mt-<pid>[-seq]` dir whose creator pid is DEAD (kill(pid,0) =>
  ESRCH) gets its ring/status/heap unlinked + rmdir. Skips our own pid,
  non-numeric names (d9mt-test-*), and EPERM-ambiguous pids. This is the
  cheap cleanup for the v1 flag-9 leak, NOT the full client-death watchdog
  (EVFILT_PROC remains open — a client CRASH still leaks the live server
  process until someone attaches again and sweeps, or the ~10 Hz ring-stat
  check fires).

Gate: test_feed gained the churn prelude — attach, 41 records, FULL client
teardown (detach record, drain wait, unlink, rmdir, exit-0 check), then the
ENTIRE existing gate (storm + flood included) runs as the SECOND attach of
the same process. PASSING; build-bridge PASS; full-clean build-driver PASS.

V3 honest flags:
- test_feed spawns servers itself, so the churn gate proves the SERVER's
  second-lifecycle handling; the PE-side claim release + unix re-attach
  sequence is compile-verified and code-reviewed (the unix globals were
  already detach-reset) but its end-to-end proof is the next real-game run.
- Wire ids keep growing across attaches (g_nextWireId never resets) — by
  design, ids are meaningless to a fresh server as long as creates precede
  uses, which the lazy create-at-first-use invariant guarantees.
- The sweep trusts pid liveness: a pid-reused-by-another-process dir would
  survive (kill(pid,0)==0). Bounded leak, same as before.

## V4 — phantom second attach (re-attach verification + dead-ring cutoff)

V3 real-game retest: first attach fully healthy AND the churn release
worked — but the SECOND attach logged "[wire] render server attached" with
NO second "[serverd] ready" anywhere, then hb=0/rec=0 forever: the client
streamed into a dead ring for ~16 MiB until the 5-window stall budget
burned and disabled. The unix attach path DID run the epoch handshake, so
epoch==1 was read without a live server — a stale-mapping / silent-death
class that could not be reproduced outside wine. V4 hardens every candidate
so the failure is either FIXED or NAMES ITSELF in the next log:

1. **Detach anon-restore (the likely actual fix)**: `wire_teardown_shm` at
   detach now restores ANONYMOUS memory over the ring AND heap VAs BEFORE
   the PE VirtualFrees them. Previously the unix side left the MAP_SHARED
   file mappings in place at detach ("PE VirtualFree drops the last ref" —
   the S1 contract): if wine's VM manager recycles a "freed" range without
   a host munmap (reprotect-and-reuse), the next attach's "fresh" PE region
   can still resolve to the DEAD ring/status file. Now wine only ever
   frees/reuses plain anon memory.
2. **Pre-spawn staleness probe**: right after mapping the freshly
   O_TRUNC-created status file, epoch MUST read 0 — nonzero means the
   mapping is stale (the exact phantom mechanism) and the attach fails
   loudly to local fallback instead of handshaking against a dead server's
   last words.
3. **Epoch alone is no longer proof of life**: after epoch==1 the attach
   requires the heartbeat to ADVANCE (1 s budget, ~100 Hz when alive)
   before declaring success. Identical for every attach, first or Nth.
4. **Named diagnostics at every failure point**: `[wire] attach #N: dir=…`
   opens every attempt; child-death-during-handshake is distinguished from
   timeout (wait status logged); frozen-heartbeat and stale-epoch failures
   name themselves. The PE side logs `claim #N` in its attached/local
   lines for correlation. No more silent halves.
5. **Never stream into a dead ring** (the wasted 16 MiB + 5 s): two layers —
   (a) presentObserved latches `g_serverDead` when the server heartbeat is
   frozen >3 s in server mode (one WireRing_stats per present, already
   paid); the CS producer checks the latch per record and disables with a
   named log. (b) waitForSpace: a zero-progress stall window whose
   heartbeat did not tick disables IMMEDIATELY (dead server) — the
   5-window budget remains only for live-but-slow consumers. PROF's delta
   baseline resets on counter regression (new attach epoch).

Gate: the churn prelude now maps BOTH attaches' rings MAP_FIXED at the
SAME VA, deliberately leaving the dead ring's mapping in place after
teardown — attach #2 must fully replace it (asserted: the fresh file reads
magic==0 before init) — and asserts the epoch+heartbeat handshake
explicitly on both lifecycles. PASSING with the full storm+flood as the
second attach; build-bridge PASS; full-clean build-driver PASS.

V4 honest flags:
- The same-VA reuse gate proves the POSIX mechanics; wine's
  VirtualAlloc/VirtualFree behavior is exactly what the harness CANNOT
  reproduce (said plainly: requirement 3's "client attach path twice
  in-process" is not feasible without wine — the diagnostic lines are the
  contract with the next game run). If the phantom recurs, the log now
  pinpoints it: stale-epoch warn = mapping bug persists; DIED-before-ready
  = spawn/env problem; frozen-heartbeat = server hung post-epoch.
- The dead-server latch keys on presents: a client that stops presenting
  (loading screen with no Present calls) delays detection until the next
  present or a ring stall — acceptable, both paths are covered.
- attach #N heartbeat-advance adds up to ~20 ms to a healthy attach (first
  tick), bounded 1 s on a dead one.

## V5 — PROCESS-LIFETIME SERVER (design correction; supersedes V3's
## respawn-per-attach and the interim lazy-attach brief)

Rounds 3–5 kept patching device-lifetime PATTERNS (throwaway device,
overlap, whatever's next) because the wire's lifetime was coupled to
MtlContext lifetime. V5 decouples them BY CONSTRUCTION — the bug class is
eliminated, not enumerated:

1. **The server is process-lifetime.** The FIRST claim in the wine process
   spawns it (V4's verified attach, unchanged, once); it survives every
   context/device destruction and dies only with the client process — its
   own pid watchdog (client pid parsed from the shm dir name
   `d9mt-<pid>-<seq>`; checked ~10 Hz alongside the ring-unlink stat) exits
   AND unlinks the shm files itself, so a crashed or teardown-less client
   leaks nothing. The orphan sweep stays as backstop. WireRing_detach
   remains implemented for explicit final-teardown callers (tests use it);
   the driver no longer calls it.
2. **Context death = QUIESCE, not teardown** (`d9mt_wire_rec_session`
   kind 0, op 36): the server drains the in-flight frame, retires every
   table resource GPU-safely (buffers/textures via the residency retire
   list; samplers/views/PSOs via retireNoRes), CLEARS the PSO cache (its
   keys embed library POINTERS — a new library at a recycled address must
   never alias a stale pipeline; dssoCache is value-keyed and persists),
   resets folded state to D3D9 defaults (resetFoldedState, shared with
   init), acks by BUMPING STATUS EPOCH (+1 per session record; boot-ready
   is 1) and keeps running. Client side sends the record + a bounded
   diagnostic ack wait (WireRing_quiesce, thunk 215) — ring order already
   serializes the reset ahead of any successor's records. No SIGTERM, no
   unlink, no region frees: the ring/heap files, PE regions and producer
   CURSORS (a successor continues where its predecessor stopped) are
   process-lifetime (`WireTransport g_wt`, file-scope in
   mtl_wire_context.cpp).
3. **Context creation = CONNECT** (session-begin record, kind 1, same reset
   + epoch ack): verifies the live server (heartbeat-advance, the V4
   discipline) and marks the session boundary. The v5-brief lazy-claim
   retry is kept exactly as designed: a context born while the claim is
   held (overlapping lifetimes — the round-5 GTA evidence) retries the CAS
   once per present and connects when the holder dies; one bool test in
   steady state. Connecting never spawns; a failed FIRST attach stays
   process-permanent (no per-present respawn stalls).
4. **Single-producer discipline over global transport**: only the claim
   holder may write the ring/heap — `reserveBytes` and `heapAlloc` carry
   the authoritative `m_claimed` gate (every record and region funnels
   through them); tap-site guards use `wired()`. Binding shadows track on
   EVERY context (any of them may claim later) and replay after connect
   (bind-once-then-Discard state the frontend will never re-emit).
   Pre-connect creates need nothing: wire ids are assigned only on
   successful emission, so create-at-first-use never assumed attach
   preceded creation; metallib byte copies are retained on not-yet-claimed
   contexts and dropped only once the process is permanently wireless.

Gate (test_feed, PASSING ~2 s wall): ONE server pid across three sessions —
boot attach + S1 checks; session 1 creates + RED-fills texture wire 777;
QUIESCE (epoch 2, server alive); BEGIN (epoch 3); session 2 runs the ENTIRE
gate (F1/F2 async-PSO, F4 checkerboard, stale-id probe: binding wire 777
now draws the BLACK dummy — red would be aliasing, blue a skipped draw —
proving the table reset; storm; 300 MB flood); QUIESCE #2 (epoch 4) +
BEGIN #3 (epoch 5) + post-reset consumption smoke; explicit detach exits 0.
Then the pid-watchdog scenario: a forked stand-in client is SIGKILLed and
the server exits 0 AND removes its own shm dir within ~1 s. build-bridge
PASS; full-clean build-driver PASS.

V5 honest flags:
- The PE claim/connect logic itself still needs the real game (same harness
  limit as V4 — test_feed exercises the server's session machinery and the
  stream shapes, not wine). Healthy log signature: ONE `[wire] attach #0`,
  ONE `[serverd] ready`, then `session QUIESCE/BEGIN` pairs on device churn
  with the SAME serverd pid, `claim #N` lines PE-side, hb never 0.
- Sessions never shrink the slab pool or argument-table pool (rewound, not
  freed); psoCache resets per session (compiles re-run — boot churn only).
- A quiesce emitted while the ring is nearly full follows normal
  backpressure; the 700 ms ack wait can expire under extreme load (warned,
  harmless — ring order still serializes).
- Straggler destroy records from a previous session are dropped by the
  table reset; a same-handle recreate in the NEW session followed by a
  session-1 straggler destroy (Rc released late, queued after the dtor's
  queue clear) could destroy the new resource — narrow window, unobserved,
  bounded by dummy-substitution semantics.
- V3's "fresh dir per attach" seq suffix remains but now only distinguishes
  processes' FIRST attaches (seq stays 0 in practice); V4's detach
  anon-restore hygiene now runs only on attach-failure fallback and final
  detach.

## V6 — quiesce deadlock hardening + hung-record telemetry

V5 real-game retest: the process-lifetime design held (attach #0 healthy,
pid-watch correct), but the server HUNG — no crash, no session log —
while processing the first real QUIESCE. The heartbeat froze, the client's
fast-disable fired cleanly, game unharmed. HONEST STATUS: a line-by-line
audit of handleSession found NO unbounded wait (submitFrame commits +
signals without waiting; retirement is list pushes; the only create-worker
lock is held for a queue push/pop), and the synthetic gate could not
reproduce the hang even with the exact overlap shape — so the ROOT CAUSE IS
UNCONFIRMED. V6 therefore does three things:

1. **Deadlock-free by construction**: handleSession now ABANDONS the create
   worker's queue first (swap under the queue mutex, destroy outside it) —
   no join, no idle-ack wait, and provably no ordering to invert because
   the worker touches nothing but its own job's Resource + status atomics.
   This also fixes a real cost: a session died mid-boot-storm no longer
   leaves the worker churning hundreds of STALE newLibraryWithData jobs.
   The in-flight job finishes into an orphan. No stage of the handler
   waits on another thread; no lock is held across a Metal call.
2. **Per-stage breadcrumbs**: `[serverd] session QUIESCE/BEGIN:
   worker-abandon... / drain-submit (abandoned N jobs)... / retire
   (serial=S)... / psocache-drop... / state-reset... / state reset,
   epoch=E`. A future hang names its exact stage.
3. **Hung-record telemetry (the bigger net)**: status offset 36 (was _pad0)
   is now `exec_opcode` — wire opcode+1 while the executor is INSIDE a
   handler, 0 between records. Mirrored through WMTWireStats
   (`server_exec_opcode`) into PROF as `execOp=`. THE DECODE RULE for the
   next frozen-heartbeat log: `hb=0` + `execOp=K` → the executor is stuck
   inside opcode K-1 (37 = session, 28 = create_shader, 32 = present, ...);
   `hb=0` + `execOp=0` → the consumer is stuck OUTSIDE handlers (ring walk,
   stat check, or the process is gone). This works for EVERY handler, not
   just quiesce.

Gate: the session-3 churn now happens MID-COMPILE-STORM — 64 fresh
libraries queued, quiesce lands while the worker is mid-queue (log shows
"abandoned 17 jobs" in a passing run), ack within the epoch timeout,
exec_opcode 0 after, and the worker provably still builds afterward (a
fresh pair converges a draw in session 3). PASSING; build-bridge PASS;
full-clean build-driver PASS.

V6 honest flags:
- The GTA quiesce hang is NOT reproduced synthetically and NOT
  conclusively root-caused. If the next run hangs again, execOp + the
  stage breadcrumbs pinpoint it; candidate classes still open: (a) ObjC
  release of the last MTLLibrary reference during table clear colliding
  with the worker deep in MTLCompiler XPC (this project has prior
  MTLCompiler-XPC history — see civ4 memory), (b) an fprintf-to-blocked-
  stderr stall (would now show execOp=37 with the last breadcrumb naming
  the stage BEFORE the hang), (c) the quiesce record racing CS-thread
  emission if the driver ever destroys a context without draining its CS
  thread first (SPSC violation — ring corruption; would show as
  decodeErr>0 or an insane execOp).
- Shader Resources' libraries are NOT retired through the GPU-safe lists
  (libraries are not GPU-referenced objects; PSOs made from them are, and
  ARE retired). If (a) above proves real, the fix is deferring library
  releases to the worker thread itself.

## V7 — create_texture hang: clamps, worker-side creation, reject counter

Run 7 proved the V6 telemetry: `execOp=27` frozen = the consumer stuck
INSIDE create_texture (op 26), from EARLY boot — no session breadcrumbs at
all. Retro-correction: V5/V6's "quiesce hang" framing was a timing
coincidence (device transition == first real texture creates).

**Audit result (honest)**: field-by-field emission vs decode found NO
transposition — MtlImageCreateInfo maps 1:1 onto the record, MtlImageType
values (0/1/2) match the server's switch, cube arrayLayers/6 handling
matches wmtTextureType, and the format marker-bit masking is equivalent
(`& 0xFFFF` vs `& ~WMTPixelFormatCustomSwizzle`; markers are 0x0010_0000..
0x0080_0000, base formats < 0x10000). So the poison — if any — is
value-level, or ENVIRONMENTAL: note that dual-execution DOUBLES texture
memory, and GTA-scale private allocations server-side can stall
newTextureWithDescriptor under memory pressure with exactly this signature
(no crash, no return). The V7 breadcrumb logs a running cumulative-MB
estimate for precisely this theory.

What landed:
1. **Sanity clamps on the consumer** (rejectCreate): create_texture with
   extent > 16384 (depth > 2048), mips > 15, layers > 2048, sampleCount >
   8, or format outside (0, 640] is REJECTED — counted in the new status
   field `creates_rejected` (offset 232; WMTWireStats
   `server_creates_rejected`; PROF `rej=`), first 8 named in the log, and
   the wire id stays unregistered (dummy-resolves forever). create_buffer
   > 1 GiB likewise. A rejected create can never hang the consumer.
2. **Texture + sampler creation moved to the create worker** — the
   consumer's V4 contract (decode, dispatch, memcpy ONLY) is now fully
   honored; a wedged newTextureWithDescriptor freezes the WORKER, the
   heartbeat keeps ticking, and draws referencing the building resource
   skip/dummy-resolve. Machinery: Resource is registered ready=0 before
   dispatch (stream-order lookups always resolve); uploads arriving while
   building STASH their bytes on the Resource (heap region acked at stash);
   the consumer applies residency + stash drains the first time it OBSERVES
   ready==1 (observeReady — MTLResidencySet mutation stays consumer-side),
   with a per-present sweep (sweepAsyncCreates) for never-referenced ones.
   Binds/RT/clears of building textures dummy-resolve / skip until then.
3. **Breadcrumb before the Metal call** (worker): first 64 creates + any
   large one log `wire/fmt/WxHxD/mips/layers/usage/cube (~cum MB)` — the
   next hang prints the poisoned values AND the memory watermark.

Gate: a poisoned create (2^30 extent, 99 mips) is rejected+counted with
the consumer provably alive; a 2992x1934 RT + a full-chain 1024^2 texture
build on the worker, and a mip-4 upload rides the stash-drain path
(uploads_executed >= 2 asserted); the F4 checkerboard now REBINDS after
convergence before the pixel read (async creation means the converge wave
can draw before the stash drains — real games rebind per draw, the gate
now mirrors that). Everything else (sessions, stale probe, storm, flood,
watchdog) unchanged and PASSING; build-bridge PASS; full-clean
build-driver PASS.

V7 honest flags:
- The GTA hang mechanism is still not reproduced; V7 removes the
  consumer's exposure to it entirely and instruments the worker. If the
  next run wedges the WORKER, the log shows the last create_texture
  breadcrumb (its values + cumulative MB) while the wire keeps running
  with draws skipping — degraded but ALIVE, and self-naming.
- A texture bound before its stash drained samples the dummy until the
  game rebinds it (games rebind per draw; the gate had to as well).
- Memory-pressure theory unproven: if breadcrumbs show multi-GB cumulative
  estimates before a wedge, the fix direction is server-side texture
  memory budgeting (or the cutover making textures single-copy).
- The 640 format-ceiling clamp is deliberately loose (real MTLPixelFormat
  max ~600); marker-masked formats pass through as before.

## V8 — wire citizenship at birth + provable consumer/worker decoupling

Run 8: SIX HEALTHY WINDOWS (~1,530 real GTA frames rendered natively,
srvFrames at rate, psoSkip 299→0 heal, uploads flowing, the v7 clamp
caught the ORIGINAL execOp=27 killer: wire=4 "format outside known enum").
Death at f≈1792: breadcrumb "create_texture #15 wire=44 fmt=256 1x0x0
mips=1 layers=0" — ZERO dims (the v7 clamps only checked maxima); the
worker wedged on it AND the heartbeat froze (violating the worker-wedge-
degrades promise). Class-level fixes, per architectural review:

### FIX 1 — wire citizenship stamped AT CREATION (never inferred)

Both killers (wire=4 garbage format, wire=44 zero dims) are the same
class: PSEUDO-RESOURCES — MtlTextureImpl/MtlAllocation objects born
OUTSIDE the canonical device create paths (presenter drawable wraps,
half-initialized temporaries) whose `info` is incomplete BY DESIGN.
Create-at-first-use assumed complete pedigree; that assumption was false.
- `MtlAllocation::wireEligible` / `MtlTexture::wireEligible` (default
  FALSE) are set only by the canonical mints: `MtlDeviceImpl::createBuffer`
  (+ `renameBuffer` — renames stay eligible) and `createImage` after
  `wireEligibleImage()` validates completeness (all extents > 0,
  mips/layers > 0, base format in (0, 640]).
- `ensureStorageCreated`/`ensureTextureCreated` refuse ineligible
  resources before assigning any id: binds carry id 0 and dummy-resolve
  server-side (existing graceful path). Counted per attempt in the new
  PROF field `inel=` (PE-local, no ABI).
- Server clamps extended as the backstop that should now never fire:
  ZERO width/height/depth/mips/layers and zero-size buffers reject
  (v7 only checked maxima — exactly the run-8 gap).

### FIX 2 — consumer provably decoupled from the worker

Audit of every consumer-side touch of a building Resource (documented
here as the contract): `observeReady` = one acquire load; sweep = load +
vector shuffle; stash = memcpy + ack; job dispatch = mutex held for a
push only; the worker holds NO lock during Metal calls. There is NO
our-code wait coupling the threads. The run-8 heartbeat freeze therefore
came from BELOW our code: both threads share ONE MTLDevice, and a wedged
`newTextureWithDescriptor` can hold device-internal locks that the
consumer's next Metal call (encoder/commit) blocks on. That coupling
cannot be removed while sharing a device — but the TRIGGER class is gone
(FIX 1 + zero-clamps mean no malformed descriptor ever reaches Metal),
and the wedge gate proves the healthy-value path stays decoupled.
- WATCHDOG thread (diagnostics only): every second, names (a) a worker
  Metal call that has not returned in >5 s — `WEDGED: create worker stuck
  Ns in newTexture wire=.. fmt=.. WxHxD ...` with the exact values — and
  (b) a consumer stuck inside one wire opcode with frozen progress >5 s.
- TEST HOOK `D9MT_WEDGE_CREATE_MS` (env, read once at executor init, test
  harness only): first texture create sleeps N ms inside the instrumented
  window.

Gates (all PASSING): pseudo-resource records (run-8 zero-dim shape +
garbage format 0x9999 + the v7 absurd-extent) rejected + counted
(creates_rejected == 3 exactly — legal creates unaffected) with the
consumer provably alive; WEDGE SIMULATION — worker blocked 6 s inside its
first create: heartbeat ticks, 100+ records consume, a present completes,
quiesce+begin ack, the >5 s WEDGED breadcrumb prints, and the server
detaches cleanly after the wedge expires. Sessions/stale-probe/storm/
flood/pid-watchdog unchanged; full-clean build-driver PASS (bridge
untouched this round: `inel=` is PE-computed).

V8 honest flags:
- The residual consumer↔worker coupling is Metal-internal (shared
  MTLDevice locks). If a HEALTHY create ever wedges (memory pressure —
  watch the breadcrumb's cumulative-MB watermark), the heartbeat may
  still freeze with it; the WEDGED log + execOp will say so explicitly.
  The definitive fix in that case is texture memory budgeting or the
  cutover (single-copy textures).
- Eligibility is stamped at 3 mint sites today; a future non-canonical
  creation path must either stamp or stay off the wire — grep
  `wireEligible` when adding one.
- The 640 format ceiling accepts nonexistent-but-in-range values (fmt=256
  passes the format check; it now fails the ZERO-dims check that actually
  caught run-8's record — value-level completeness is the client stamp's
  job, and the client no longer emits such records at all).

## V9 — the GPU copy/blit family crosses the wire (the black-server-frame fix)

Milestone context: the visual cutover is LIVE (boot to frame 5600+,
srvFrames=256/256, 99.9% of presents from the server IOSurface) but the
server frame was 100% BLACK on menu/legal screens: MtlWireContext
classified copies/blits/resolves/mipgen as "cold: forward only" — GTA
COMPOSES those screens with texture copies (UpdateTexture + StretchRect),
so 1.5M records/window of binds/draws executed against content that was
assembled by operations the server never received.

What landed (ops 37..42, ABI-appended):
- **copy_texture (37)** — single-subresource box copy, the composition op.
- **copy_buffer (38)** — GPU b2b (CPU-visible content still rides
  upload_buffer; GPU-private staging b2t copies do NOT cross — the
  copyBufferToTexture tap requires a host-visible source; flagged below).
- **blit_texture_view (39)** — the meta-blit. The CLIENT normalizes rects
  exactly like its local path (swap/clamp, uv normalization incl. mirrored
  sources) and ships the final viewport rect + uv4; the server draws a
  fullscreen triangle through its own tiny MSL library (compiled once at
  init via MTL4Compiler), PSOs per {dst pixel format, sampleCount} built
  ASYNC through the PsoEntry machinery.
- **resolve_texture (40)** — draw-less Load/MultisampleResolve pass (the
  local shape; full-subresource, color aspect).
- **generate_mipmaps (41)** — xfer-encoder generateMipmapsForTexture.
- **clear_image_view (42)** — FULL-subresource color executes as the
  mini-pass clear; partial rects / depth-stencil aspects are dropped
  server-side with one named log (they need the fill/depth-clear draw
  pipelines — a later slice).

Client: each tap requires BOTH sides wire-eligible; a pseudo-resource side
skips the record, counted in PROF `copySkip=` (the present copy's drawable
dst lands here by design). Emission precedes the local forward; creates
emit lazily inside.

Server one-shot vs building resources — THE subtlety: these records
execute ONCE (unlike draws, which retry). A record referencing a texture
still BUILDING on the create worker (the UpdateTexture-right-after-create
shape, and the gate's own copy/blit scenarios) is STASHED and retried at
each present (copies, image-clears, mipgens, then blits — cross-family
stream order is approximated at drain time, flagged). Blits additionally
stash while their dst-format PSO compiles. Unresolvable/failed-create
references drop. Execution respects pass boundaries exactly like the
client (xfer encoder / mini-pass closes the render pass; §4c barriers).

Gate: upload checker to A, wire-copy A→B, draw sampling B → checker
verified (wave 3: create-stash-drain-execute); scaled 2x blit A→C
(nearest) → checker verified (wave 4: + async PSO). Everything prior
green; full clean-room build chain (rm -rf build, d9mtair → bridge →
driver+server) PASS.

V9 honest flags:
- Cross-family drain order at present ≠ exact stream order (copy→clear→
  mipgen→blit within one frame's stash). Composition chains that overwrite
  the same texel region with DIFFERENT op families in one frame could
  resolve in the wrong order until the next frame's re-composition.
- Blit dst format uses the DST TEXTURE's base pixel format (view
  reinterpretation not modeled); non-2D blit sources sample as 2D (the
  local path has the same warning).
- copyBufferToTexture with a GPU-PRIVATE source buffer still doesn't cross
  (the upload tap needs mapPtr). Not observed in practice; the record
  shape exists if needed (upload_texture carries CPU bytes only).
- texture→buffer readbacks stay LOCAL by design (C2 slice: readback
  routing).
- A blit executes at the stream position of its drain when stashed — dual
  frames of staleness worst-case, self-healing per frame.
- clear_image_view partial/DS drops are the most likely visible residue if
  a game ColorFills sub-rects (named log will say so).

## V10 — UpdateTexture content path: diagnosis instrumentation + the FF slot-cap fix

V9 retest: still black server frames; PROF showed inel exploding to
~7k (+~2/frame — cumulative, not delta) and NO copySkip field. Audit
findings, in order of certainty:

1. **copySkip was never in the PROF line** (my v9 edit silently no-op'd
   against the merged fbHealthy line — unasserted replace; v10 edits
   assert). FIXED — with it absent, the inel counter absorbed all
   attention. inel's +2/frame growth profile matches the PRESENT COPY
   (drawable + IOSurface wraps, both pseudo-resources BY DESIGN) — likely
   benign noise, now disambiguated: the first 24 ineligible attempts log
   kind/dims/format/usage/ptr client-side (`wire-inel image/buffer: ...`).
2. **The UPDTEX path was audited end-to-end and its content SHOULD cross**:
   UpdateTexture → UpdateTextureFromBuffer (non-converting) packs the
   sysmem bytes into a canonical AllocStagingBuffer slice (wire-eligible,
   mapped) and calls ctx->copyBufferToTexture(dst image, ..., staging,
   rowAlign=0) — exactly the existing upload tap, whose tight-pitch math
   matches packImageData's layout. Design (a) from the brief IS the
   existing mechanism. If it skips, it now COUNTS (copySkip) and NAMES
   itself (`wire b2t upload SKIPPED: dst ineligible | src unmapped ...`,
   first 8). The CONVERTING-format branch (compute conversion) still does
   not cross — flagged, not the A8R8G8B8 menu-art shape.
3. **A real black-composition bug found and fixed: the server's texture/
   sampler STATE arrays capped at 16 slots.** FF-shaped shaders (the
   likely menu path — this project's FF ubershaders bind 80+ registers as
   5 type-arrays x 16 through the AB word path) had every bind at slot >=
   16 silently dropped to the dummy: black FF draws with perfectly healthy
   rec/draw counters. v10: tracking arrays are kMaxTexTrack=256 (the whole
   uint8 AB reg space); the classic argument-TABLE loop stays 16 (its
   Metal capacity); the AB word path reads the full range. If the menu is
   FF-composed, THIS was the black.
4. **No more silent copy family**: first-8 named server logs for
   copy_texture / blit executions and upload_texture (wire/fmt/mip/box) —
   the next run's log will show menu-art uploads landing (or their skip
   reasons client-side).

Gate: the UPDTEX SHAPE — a partial sub-rect upload with an offset (dirty
box, solid red 32x32 at texel 16,16) into the live checker texture:
inside-box points that would alternate under the checker are BOTH red,
outside points keep alternating (offset/pitch path proven). Calling the
MtlContext API directly from the harness is NOT feasible (wine-only);
the wire-shape gate + the named client-side logs are the contract.
Full clean-room chain (rm -rf build; air → bridge → driver+server) PASS.

V10 honest flags:
- The black-frame root cause is a HYPOTHESIS RANKING, not a proof: (a) FF
  slot cap (fixed here, high confidence if menu is FF), (b) an upload-tap
  skip (now named if it happens), (c) something downstream of correct
  uploads (the first-8 upload logs will show wire ids + boxes to compare
  against D9MT-UPDTEX spam). The next run discriminates all three.
- Decode rule for the retest: `copySkip=` small & static + `upload_texture`
  logs showing 1024x1024-ish boxes = content crossing, look at (a);
  `copySkip=` growing + `wire b2t upload SKIPPED` lines = the tap, look at
  the named reason; `inel=` growing at ~2/frame with `wire-inel image:`
  naming drawable/IOSurface wraps = benign present noise.
- kMaxTexTrack widens StageState by ~6 KB per stage (id arrays) — noise.
- Converting-format uploads (compute-conversion textures) still local-only.

## V11 — THE BLACK-FRAME ROOT CAUSE: nested record emission corrupted the
## backbuffer's create (+ observability, RT-routing audit, rotation fixes)

The 40-second cube reproducer (`WAIT=25 D9MT_PROFILE=1 D9MT_SERVERD_PATH=…
scripts/run-test.sh code_cube3d --no-build`) named it: `[serverd] REJECT
create_texture wire=9: zero dimension/mips/layers` — the SWAPCHAIN
BACKBUFFER's create crossed as garbage, the v7/v8 clamp (correctly)
rejected it, the id dummy-resolved forever, and every clear/draw targeting
the backbuffer landed in the dummy while the published IOSurface stayed
black. srvErr=4 (a decode-error CODE, not a count — it was never named)
was the same corruption's other face.

**Root cause (client)**: MtlWireContext::bindRenderTargets called
ensureTextureCreated INSIDE the open set_render_targets_by_id record
(fillIdRef after beginRecord) — a NESTED record emission that wrote the
create record over the pending record's bytes at the same ring position.
The backbuffer is the one image whose FIRST wire use is the RT bind
(everything else creates at texture/buffer binds, outside records), so
precisely its create was corrupted, on every real title, forever. The
comment above the code even said "Creates emit first". Three fixes, class
level:
1. HOIST: bindRenderTargets computes all refs (creates included) into
   locals before beginRecord.
2. STRUCTURAL: reserveBytes refuses reservation while a record is pending
   (named err, first 8) — nested emission is now impossible; the refused
   inner create retries at the next un-nested use.
3. EMISSION GUARD: ensureTextureCreated validates its computed fields
   (nonzero dims/mips/layers/format) BEFORE reserving; incomplete info
   defers the create (named err) instead of shipping garbage. mips/layers
   gained m_info fallbacks.

**Also fixed — the multi-backbuffer rotation gap** (would have been the
NEXT black screen): noteBackbuffer tracks only the present-source image;
with BackBufferCount > 1 the rotation means frame N draws into a different
image than the one noted at present N-1, so the pointer compare never
flags. Now: PresentImage registers ALL swapchain backbuffer images
(wire::noteSwapchainImages; set membership flags RT binds), and the server
ALSO substitutes any RT whose wire id equals the LAST PRESENTED
backbuffer id (the present record teaches the live id — the belt for
flag-less binds).

**And a folded-clear frame-boundary bug caught by the new gate**: a
clear-only frame (no draw materializes the pass) lost its folded clears at
present; submitFrame now runs flushPendingClears first.

Observability (permanent):
- srvErr semantics: it is the status ERROR CODE (4 = decode). The decode
  error site now logs first-8 with opcode/size/tail/head and the dropped
  byte count ("the drop loses every record in [tail,head)" — creates lost
  in that window dummy-resolve forever).
- `[serverd] srv-stats: draws=N skip=N{psoPend,psoFail,noSh,noRt,noIdx}
  passes=N upl=N /window` every 256 presents — the drawing-vs-skipping
  discriminator.
- First-8 `set_rt` logs: color0/ds wire id + flags and WHERE each resolved
  (IOSURFACE / texture / UNRESOLVED), plus lastPresentBB + slot; first-8
  `present` logs (serial, bbWire, slot). Cross-checkable by eye.

**CORRECTION (coordinator's GPU-readback dump pair, mid-slice)**: the
server CLEARS correctly into the published IOSurface — routing/
substitution/publication all work; what's missing server-side is the
DRAW's pixels (cube: perfect clear color, zero geometry). The unified
theory ties both faces to the SAME corruption above: the nested-emission
decode error doesn't just zero one create — the recovery DROPS the whole
[tail, head) window, and at boot that window holds the VB/IB creates,
uploads and constants. Clears survive (flag-based, re-emitted every
frame); the draw INPUTS don't — draws then fetch zeros, degenerate, and
raster nothing over a perfect clear. Fixing the nesting removes the drops
removes the input loss.

Gates: (a) alternating-backbuffer scenario — two backbuffer ids alternate
as RT with distinct clear colors; frame 3 drops the FLAG and relies on
present-id learning; the published surface must hold each frame's color
(single completion-gated reads). Caught the folded-clear bug on its first
run. (b) MINIMAL-CUBE scenario — the reproducer's failure shape: wire
vertex buffer (real fetched bytes through stage_in), wire index buffer,
the WVP matrix through the push block (buffer 20, past the 32-byte shared
reservation), one indexed draw; asserts the draw PRODUCES pixels (centroid
magenta) over the demo's exact clear color with the outside untouched.
PASSES server-side (wave 3) — the server's minimal-draw path is proven
sound, so the reproducer's zero-pixel draw was client-stream corruption
(the nesting bug), not executor logic. Full clean-room chain PASS.
Expected cube-run signature after this fix: NO backbuffer
"REJECT create_texture", decodeErr staying 0, srvErr staying 0, server
dump = clear + cube.

V11 honest flags:
- The nesting guard makes the corruption class impossible but a refused
  inner record is DEFERRED, not emitted — callers that only ever reference
  a resource inside records would never create it (none exist today;
  the guard logs if one appears).
- srv-stats logs are per-256-presents (not per PROF window) — correlate by
  frame counts.
- The rotation fix registers images at every present (a few Rc copies) —
  negligible, and self-healing across Reset.

## V12 — depth-chain audit: DSSO depth_test parity + full-DS clears + a
## depth-tested gate (the clear-but-no-cube investigation, continued)

Context: V11 left the wire CLEAN (decodeErr=0, srvErr=0) and the server
DRAWING (draws=255, passes=256/window, RT routing correct: color0 ->
IOSURFACE, ds id -> texture) — yet the published IOSurface held EXACTLY the
clear color. The handoff's hot lead: `resolveDsso` ignores `depth_test`.

**Hot-lead verdict: the premise was HALF true.** `resolveDsso` does ignore
`depth_test` — but so does the CLIENT (`MtlDeviceImpl::getDepthStencilState`,
driver/backend/mtl_device_impl.cpp, applies `depthCompareOp`/`depthWrite`
unconditionally). There was NO client/server divergence there, so it cannot
be the local-has-cube/server-has-clear delta for a demo that runs
ZENABLE=TRUE anyway (test/code_cube3d.c:173). Both sides were, however,
diverging from D3D9 (ZENABLE=FALSE must disable test AND write), so BOTH
were fixed in lockstep:
- server/executor.mm `resolveDsso`: `depth_test==0` -> compare Always +
  write NO (cache key already spans depth_test — no key change).
- driver/backend/mtl_device_impl.cpp `getDepthStencilState`: identical
  mapping (FNV hash already covers depthTest).
Dual execution stays pixel-identical by construction; ZENABLE=FALSE draws
against a stale-depth attachment (real-game UI class) are now correct on
both sides.

**Full depth-chain audit (client mirrored, divergences found and fixed):**
1. `clear_image_view` (op 42) DROPPED all depth/stencil-aspect clears
   server-side ("a later slice"). The client executes them as a mini-pass
   load-action clear. A game whose D3DCLEAR_ZBUFFER lands on the op-42 path
   (PARTIAL-viewport Clear, i.e. vp/scissor smaller than the surface —
   d3d9_device.cpp Clear -> fullClear=false -> clearImageView) got NO depth
   clear EVER server-side -> first-frame depth garbage -> every
   depth-tested fragment rejected FOREVER over a perfect clear. THE
   cube-signature-shaped hole in the chain. Fixed for FULL-subresource DS
   aspects: executes as the standalone mini-pass (depth/stencil attachment
   + loadAction Clear), mirroring the client's clearRenderTarget mini-pass.
   Partial rects still drop (they need the depth-clear draw pipeline —
   genuinely a later slice) but now COUNT per occurrence and name first-8
   (`clearDsPartialDrop`). NOTE: code_cube3d's own Clear is full-viewport ->
   op 32 (clear_by_id) -> folds into the pass; both paths now covered.
2. `clear_by_id` (op 32) with an unresolvable/still-building target was
   dropped SILENTLY (a still-building ds texture loses its first depth
   clear with no retry). Now counted (`clearDropUnresolved`) + first-8
   named with wire id/aspects/reason.
3. `set_rt` resolving the ds ref to named-but-nil runs the pass WITHOUT a
   depth attachment (client renders WITH) — silent divergence, one frame
   per late create. Now counted (`dsUnresolvedBinds`) + first-8 named.
4. Draws against a ds attachment with the DEFAULT depth state (no
   set_depth_stencil_state record ever seen) now count (`dssoDefaulted`) +
   first-8 named.
5. Verified NOT divergent: pass depth/stencil load-store shape (Clear/Load
   + Store both sides; client's Rule B store=DontCare is perf-only), D24S8
   -> wire format 260 (Depth32Float_Stencil8, client maps BEFORE emission),
   depth texture creation (Private + RenderTarget usage), Metal-4 PSO
   (depth format is NOT in the MTL4 pipeline descriptor — mtl_pso_cache.h
   states it, bridge confirms), stencil-aspect gating by format, viewport
   znear/zfar and scissor defaults, folded clear-then-draw ordering.

srv-stats gained `depth{clrDrop= clrPart= dsUnres= dssoDef=}` (per-window
deltas). All four MUST read 0 on a healthy cube run.

**Gate: v12 DEPTH-TESTED CUBE (server/test_feed.c).** The v11 minimal-cube
hand-crafted state with the reproducer's REAL depth semantics decoded from
code_cube3d + the client's emission shape: auto-DS texture create (2D,
format 260, usage 0x40), ds bound via set_rt, D3DCLEAR_ZBUFFER-shaped
clear_by_id (aspects 0x2, depth 1.0, folds into the draw pass),
set_depth_stencil_state depth_test=1 write=1 compare LESSEQUAL(3) — then
draw A (triangle at z=0) must PRODUCE pixels (garbage depth rejects it);
then compare LESS(1) write=0 + the fullscreen z=0 RED draw must be
REJECTED over the triangle (0<0, proves the test ENGAGES) and ACCEPTED
outside it (0<1.0, proves the clear VALUE landed). Center=magenta +
corner=red or FAIL. Runs inside the existing build-server gate.

Expected cube-run signature after V12: srv-stats unchanged on the happy
path (draws=255, passes=256, skip=1{psoPend=1}) PLUS
`depth{clrDrop=0 clrPart=0 dsUnres=0 dssoDef=0}` — any nonzero bucket names
the exact depth divergence with first-8 logs. If the gate PASSES and the
real reproducer STILL publishes clear-only, the remaining suspects are
outside the depth chain (only the real 40s run can prove: real record
sizes/against-the-wire state deltas the gate can't synthesize — e.g. the
demo's exact viewport/push-block traffic, or a second RT/state record the
gate doesn't emit).

V12 honest flags:
- The depth_test fix changes BOTH sides identically; if a game relied on
  the old always-tested behavior it changes local rendering too (that
  behavior was a D3D9 spec violation — DXVK maps ZENABLE the same way).
- Partial-rect DS clears still drop (counted now). If `clrPart` is nonzero
  on a real title, that title needs the depth-clear draw pipeline slice.
- `clearDropUnresolved` clears are still LOST, not stashed (unlike op-42
  img clears, op 32 has no retry queue) — the counter tells whether that
  matters in practice before we build the stash.
- ONLY the real reproducer can prove the cube renders: the gate proves the
  server's depth chain executes the demo-shaped stream correctly, not that
  the demo's actual wire traffic contains what the gate synthesizes.

## V13 — ZERO-PIXEL-DRAW ROOT CAUSE: host-visible CONSTANT-BUFFER content
## never crossed the wire (+ per-draw input parity checksums)

The V12 state (clear perfect, draws=255 execute, zero pixels) pointed at the
draw's INPUTS. Found by READING, then fixed and gated:

ROOT CAUSE (input class (c) — VS constants): SetVertexShaderConstantF does
NOT ride the push block. It lands in the D3D9 constant-buffer RING
(d3d9_device.cpp UploadConstants → GetConstantBuffer(CbvIndex::
VSDynamicConstants = slot 4).Alloc → copyConstantData). On Apple Silicon
D3D9ConstantBuffer::DetermineDmaUsage returns FALSE (UMA, no staging split)
→ SetupStreamCommand early-outs → `uploadBuffer` is NEVER CALLED for
constants. The frontend writes the matrix straight through mapPtr AFTER the
bind — coherent for the LOCAL GPU (shared storage), invisible to the WIRE.
The server's mirror of the constant buffer kept its create-time
ensureStorageCreated snapshot (zeros) forever → WVP = 0 → every vertex
collapses to the origin → draws execute and rasterize NOTHING. Exactly the
symptom; VB/IB survive only because they are Lock-written BEFORE first bind
(the create-time snapshot catches them).

THE FIX (client, mtl_wire_context.{h,cpp} snapshotDirtyUniforms): UBO
shadows gained a `dirty` bit set by bindUniformBuffer(Range); at draw
emission every dirty UBO range re-ships its CURRENT mapPtr bytes as a
normal upload_buffer (dedup for both-stage binds like SpecData). Draw time
is the SOUND tap: the frontend writes constants (app thread) before
queueing the draw, so the CS thread executing the draw observes the final
bytes; server-side the upload rides the xfer encoder which orders ahead of
the pass the draw materializes (ensureXfer closes an active pass,
materializePass closes xfer first).

OBSERVABILITY (both sides, first 4 draws, identical line format for eyeball
diff): client `wire-draw#N ...` (Logger) vs server `[serverd] wire-draw#N
...` (stderr):
  wire-draw#N vb=<wireid>+<off>/<stride> vbsum=<64B-window> up=<shipped>
      ib=<wireid>+<off> ibsum=.. up=.. cbv=<VS-slot-mask> constsum=.. up=..
      pushsum=<fnv of VS push block> pext=<extent>
plus a draw-#1 `wire-vl` layout dump (attr loc/binding/fmt/off; layout
binding/stride). READ THE SUMS RIGHT: `up=` hashes the exact bytes shipped/
consumed over the heap (client emitUploadBuffer twin of the server's
Resource::uploadSum) — these MUST match immediately and are the transport
discriminator. The window sums (`vbsum`/`ibsum`/`constsum`) hash the first
64 B the draw fetches; the SERVER side reads buffer.contents which lags the
xfer-encoder blit until the GPU executes, so early server window sums
legitimately read the zero-fill hash — do not call that a divergence. The
push mirror (client m_pushShadow) uses the same layout math as
MtlContextImpl::pushData and the executor's push_data decode, so pushsum
divergence = push transport bug.

GATE (test_feed v13 cbv-cube — the REAL mechanism as decoded from client
emission, not the v11 push-block hand-craft): create_buffer + zero snapshot,
bind_uniform_buffer at slot 4, then the per-update pair upload_buffer
(dst_offset 256) + bind_uniform_buffer_range (offset 256) — the matrix at a
NON-ZERO ring offset proves offset plumbing — through cubeCbvVs
(test_stub.metal, float4x4 at AIR buffer 1 via cbv_slot_map[4]=1;
emit_shader grew an emit_shader_map variant). PASSES (wave 3). Full clean
chain (rm -rf build; build-bridge; build-driver → build-server + all gates)
PASSES.

Expected cube-run signature (scripts/run-test.sh code_cube3d): client and
server wire-draw lines with MATCHING `up=` sums on vb/ib/const; server dump
= clear + SPINNING CUBE (matching local), decodeErr=0 rej=0 srvErr=0. If
the cube STILL fails with matching const up-sums, the divergence is inside
PSO/layout decode — the wire-vl dumps discriminate next.

V13 honest flags:
- Constants re-upload per DIRTY BIND, not per byte-change: a title that
  rebinds ranges without changing bytes re-ships them (256 B-ish per draw —
  heap traffic, not ring traffic). Fine for the cube; profile on GTA.
- A mid-pass constant update (multi-draw pass) closes the pass server-side
  (ensureXfer) — pass-count inflation on constant-heavy titles is a KNOWN
  perf follow-up, not a correctness issue.
- Direct-mapped NOOVERWRITE dynamic VB/IB writes (no context call at all)
  are the SAME gap class and remain uncovered — nothing re-snapshots them
  after the create-time snapshot. The cube's static geometry doesn't hit
  it; a streaming title will. Same fix shape applies (draw-time dirty
  snapshot of stream/index shadows) if checksums implicate it.
- The wire-draw client logs go through Logger::warn (wine log), the server
  through stderr — correlate by draw index, not timestamps.

## CUTOVER TODO (next agent — the visual flip; NOT done here)

What presents where TODAY: the client renders locally and presents its OWN
backbuffer via the existing swapchain blit (d3d9_swapchain.cpp PresentImage
→ presentUpscale/blit → drawable). The server renders the same stream into
its IOSurface pool and publishes {present_slot, frame_serial (GPU-done),
iosurface_id[], generation} — nobody consumes those pixels yet.

Flip sequence (client, all pieces now exist):
1. In the present path (MtlPresenterImpl / the swapchain's present blit
   source selection), read status v2/v3: if `frame_serial >=
   localPresentSerial - 1` (pace gate) use the server frame: IOSurfaceLookup
   `iosurface_id[present_slot]` ONCE per {slot, iosurface_serial generation}
   (cache 3 wrapped textures), wrap via
   newTextureWithDescriptor:iosurface:plane:, and blit THAT into the
   drawable instead of the local backbuffer. The completion gate landed in
   S3 means no extra GPU wait is needed — frame_serial already means
   "pixels done".
2. Fallback per-present: server behind (pace gate fails), heartbeat stale,
   version < 3, or lookup failure → present the LOCAL backbuffer for that
   frame (the S2 dual-execution keeps it correct every frame). The
   permanent-disable path (transport death) must also flip back permanently.
3. Only after the blit-flip soaks: thin the client (stop local EXECUTION,
   keep recording) — that is the real S3 "thin recorder" goal and needs the
   remaining gaps closed first: queries/copies/blits/resolves/mips (still
   client-local, HANDOFF-S2 stub list), dynamic-buffer content correctness
   (flag 1 above), invalidateBuffer rename semantics under load, and a
   client-death watchdog server-side.
4. Revert plan: the flip should be ONE commit touching only the present-
   source selection; `git revert` of it restores today's behavior (dual
   execution + unused server frames). No env flags (branch policy).

Sizing note for the pace gate: the IOSurface pool is 3 deep and the resize
path (pool generation bump) already publishes via iosurface_serial[] — the
client cache MUST key on {id, generation}, not id alone (resize leaks old
generations by design, HANDOFF-S2 flag 5).

## MILESTONE — boot survival achieved (run 9, S3-v8 @7b2b570, 2026-07-18)
First full boot survival. GTA IV real run: server + game both alive past frame 2560,
10+ healthy PROF windows (run 8 died at f=1792). Per window: srvFrames=256/256,
hb steady ~350-580, stalls=0, execOp=0, heapDrop=0, inel=0 (citizenship rule holding),
psoSkip 599→0 heal, upl ~71/window. Residual non-fatal (for soak/cutover agents):
decodeErr=1 (one malformed record, tolerated), srvErr=4 (exec errors counted+continued).
These are steady-state RENDER QUALITY items, not survival — the good side of the milestone.
Next: soak (srvExecErr/decodeErr trend over minutes) → visual cutover (IOSurface present +
autonomous pixel-compare vs client backbuffer dump) → thin recorder (fps) → consolidation.
