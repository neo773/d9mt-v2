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
