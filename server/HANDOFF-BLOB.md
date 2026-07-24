# HANDOFF — P1 BLOB SUBSTRATE (the Venus port, slice P1)

Branch `native-backend`, on top of dbbe412. Sources of truth:
`docs/VENUS_CONTRACT_DIFF.md` (the contract diff + PORT MAP, P1 section),
the cross-ISA PoC (`poc_blobmodel_server.m` / `poc_blobmodel_writer.c` —
x86_64 writer -> arm64 Metal server, zero copies, offsets reused 3x, ALL
PASS), and mesa `src/virtio/vulkan/{vn_device_memory.c,vn_renderer_virtgpu.c}`.

## What this slice is

CPU-visible D3D9 buffers (dynamic VB/IB, the constant ring, staging-class —
every backing whose mapPtr the frontend writes) no longer SHIP content. Their
backing is carved from the shared heap file's new BLOB region; the client's
Lock pointer, the client-local Metal buffer (bytesNoCopy over the same
pages) and the server's one whole-heap `newBufferWithBytesNoCopy` wrap all
alias the SAME physical pages. The create record carries `{heap_offset,
size}`; the server resource is `{heap wrap buffer, base offset}` — binds add
the base, draws read the client-written bytes directly. Venus's law, ported
whole: *mapped-write content never rides the command stream; flush is a nop;
coherency is the substrate's job* (`vn_renderer_virtgpu.c:427`,
`VK_EXT_command_serialization.txt:126-133`).

## Port table (Venus function -> ours)

| Venus (mesa) | Ours | Notes |
|---|---|---|
| `vn_renderer_shmem`/`vn_renderer_bo` `{res_id, mmap_size, mmap_ptr}` (vn_renderer.h:11-29) | `MtlAllocation{blobOffset, blobReserved}` + `mtl_blob_pool.h` slice | refcount = the existing Rc discipline |
| `virtgpu_bo_create_from_device_memory` (vn_renderer_virtgpu.c:631) | `create_buffer.heap_offset != 0` (mtl_wire.h) | id = wire_id/client_handle as before |
| `virtgpu_ioctl_map` / "renderer … injects the pages into the guest" (vn_device_memory.c:459-463) | already-mapped heap file both sides — the injection step is free | pre-fault at attach replaces lazy fault-in |
| `virtgpu_bo_flush/invalidate` = nop (vn_renderer_virtgpu.c:422-436) | `noteBufferWrite`/`snapshotDirtyUniforms`/DISCARD re-ship SKIP for blob backings | the deletion list below |
| `vn_MapMemory2` app ptr = `mmap_ptr + offset` (vn_device_memory.c:501) | `mapPtr = heapData + blobOffset`; Lock returns it directly | |
| shmem-pool suballocation + "works queued after ongoing works" reuse (vn_ring.h:20-24) | `d9mt_blob_pool` bump + retired-slice freelist gated on `status.frame_serial` | THE CREDIT LAW (below) |
| host allocates nothing extra per object | server `handleCreateBuffer` blob branch: NO alloc, NO memset, NO residency entry | one residency entry = the heap wrap |

## The credit / freelist law (the PoC's own first failure, now load-bearing)

A retired blob slice may be reused ONLY once the server's GPU-COMPLETED
frame serial (`status.frame_serial`, completion-gated publication) covers
the slice's retire stamp:

- retire stamp = `g_markerMirror + 1` — the frame being RECORDED when the
  backing retires is the last frame whose records can reference it
  (conservative; Rc discipline puts retire at/after the last emitted use).
- reuse gate = `g_srvCompletedFrame >= stamp`, sampled at the per-present
  sites that already read `WireRing_stats` (zero extra unixcalls; at most
  one present stale, in the SAFE direction).
- the N=2 frame credits (HANDOFF-FLOWCONTROL.md) bound stamp-vs-completed
  to ~2 frames, so the freelist breathes within ~2-3 presents — the credit
  mechanism IS the second half of the gate, no new wait was invented.
- wire down / server dead => stamp 0 / gate always passes (no reader can
  ever exist); allocation falls back to hostAlloc when the pool misses.

THREE reuse paths all pass through the gate:
1. fresh alloc from the freelist (`d9mt_blob_alloc` serial check);
2. the frontend recycle pool (`MtlBufferImpl::takeRecycledStorage` skips
   blob entries whose stamp is uncovered — LOCAL completion put them there,
   the SERVER may still be behind: exactly the premature-reuse class the
   PoC reproduced);
3. destructor retire (`~MtlAllocationImpl` -> `wire::blobRetire`).

## Pre-fault precondition (S1 finding, unchanged and enforced)

`newBufferWithBytesNoCopy` wires pages at wrap time; pages faulted later
are INVISIBLE to the GPU. Therefore the blob region is made non-sparse
(memset + msync) by its CREATOR **before the server boots**:
`winemetal_unix.c _WireRing_attach` (real client) and `test_feed.c` setup
(synthetic client). The upload region stays sparse (CPU-copied both sides).
The server wraps the whole mapping once at `d9mt_executor_init`, ONLY when
the file carries a blob region (legacy 256 MiB files never get wrapped).

Heap file layout (mtl_wire.h): `[hdr][pad->16K][upload: capacity][blob]`,
blob bytes derived on BOTH sides from file size vs header capacity; blob
offsets are data-area-relative and >= capacity (so 0 still means "no
blob"). Client requests 256 MiB upload + 128 MiB blob
(`D9MT_WIRE_HEAP_BLOB_CAPACITY`); the unix side recognizes the enlarged
file by "total is NOT pow2 but total-128MiB is". Slices are 16 KiB
(host-page) aligned — the same granularity serves the wine-side wrap of a
slice, the server wrap and D3D9 offset rules.

## Deletion inventory (what the aliasing replaced)

Client (`mtl_wire_context.{h,cpp}`), for blob-backed storages:
- `ensureStorageCreated` create-time content snapshot — replaced by page
  aliasing (the snapshot's only job was making Lock-written bytes visible);
- `invalidateBuffer` DISCARD re-ship (+ its partial-bytes honesty flag) —
  a fresh slice is visible the instant Lock writes land;
- `noteBufferWrite` V16 dirty-range re-ship — Venus: flush is a nop;
- `snapshotDirtyUniforms` v13 draw-time UBO re-ship — the constant ring is
  blob-backed; residual non-blob path keeps a plain (merge-only) re-ship;
- `uploadBuffer` upload_buffer emission — replaced by a CPU copy into the
  dst pages at the same stream position;
- BufShadow `lastShip*` dedupe hashing (V15) — deleted WITH the flood it
  bounded.

Server (`executor.mm`):
- `DeferredXfer` struct + `deferredXfers` parking + `flushDeferredXfers`
  (buffer half) + `promoteParkedXfers` + the prepareDraw buffer
  promote/flush + the copy_buffer pre-flush + `Resource::{pendingXfers,
  xferMin,xferMax}` + `uplPromoted` — the entire staged-blit park/promote
  machinery existed to absorb the constant-ring upload flood, which is now
  zero records. Residual non-blob staged uploads (live-range, cold) encode
  their slab blit IMMEDIATELY (in-order, closes an open pass, counted in
  `uplFlushUse`) — the simple correct shape.
- blob creates: server buffer allocation + zero-fill + per-resource
  residency — replaced by `{heapBuf, bindBase}` and one residency entry.

Net: ~200 LOC of mechanism deleted; the upload paths that REMAIN serve
exactly the non-blob residue: textures (explicitly unchanged this slice),
GPU-private buffers, and pool-miss fallbacks (`blobFb=` PROF counter).

## What stays on the upload path

- ALL texture content (upload_texture + slab + deferredTexXfers parking +
  drainStash — untouched; next slice candidate);
- buffers with `cpuAccess != Write` (GPU-private, readback);
- blob-pool misses (region exhausted / wire never attached): hostAlloc
  backing + the legacy create-snapshot/note/re-ship/upload records — the
  whole old path is kept intact as the fallback, selected per-backing.

## Gates (all green; scripts/build-bridge.sh + scripts/build-driver.sh full clean)

- **blob pool** (test_feed GATE 0): the driver's exact `mtl_blob_pool.h`
  logic compiled x86_64 — deliberate premature reuse (retire serial 5,
  completed 4) is BLOCKED; granted at completed 5; serial-0 retire
  immediately reusable.
- **blob rotation** (GATE A — the PoC scenario through the real server):
  blob VB + CB, 12 frames, 4-slice rotation (every offset reused 3x across
  completion), per-frame client writes, draws at offsets, per-frame pixel
  asserts (column baked in the vertex slice, row in the constant slice —
  stale slices paint the wrong cell), **0 upload records**.
- **blob flood** (GATE B): 500 draws with fresh blob slices -> **1 render
  pass**, 500/500 draws, **0 upload records** (v15 asserted "uploads
  executed"; the mechanism beneath swapped, the pass bound held).
- v13/v13.1/v14 (classic + AB + mid-pass), v15 flood + mid-pass, v16
  wrap-race 20/20, FLOW-CONTROL storm/park/overload-pacing, queries,
  present-timeline, watchdog, wedge, idle-liveness: all PASS UNCHANGED.

## POST-PORT RESIDUALS (live-run findings, FIXED on top of a0e24c3)

The first street run (log: /private/tmp/gta4-d9mt-launch.log) proved the blob
path (textures instant, 0 upload flood while the pool held) and exposed the
sizing/reuse residuals. Counters are CUMULATIVE: `blob=` plateaued at 6,837
at scene load and never grew again while `blobFb=` climbed thousands per
window — the pool was PERMANENTLY exhausted and all dynamic-buffer traffic
fell back to the legacy staged path (stg=1.8M/window, useFlush=1.2M pass
closes, srvGpu 910 ms).

Root causes, all in the pool, all fixed + soak-gated:
1. **Region too small**: the street load burst is ~6.8k live slices
   (~107-134 MiB at 16 KiB granularity) — exactly AT the old 128 MiB cap.
   Now 512 MiB blob + 128 MiB upload (rebalance: buffer content left the
   upload ring by design; budget comment at `D9MT_WIRE_HEAP_BLOB_CAPACITY`
   and `WireHeapCapacity`). Total PE reserve 640 MiB + 16 KiB; reserve
   failure still degrades to the legacy pow2 layout, then ring-only.
2. **Freelist cap leaked the region**: mass scene-transition retires
   (6.8k at once) overflowed the 1024-entry freelist; overflow entries were
   DROPPED = region bytes lost for the session. First fix bumped the cap to
   8192 — and the next live street run overflowed THAT too (12.6k
   blobFlDrop/window during transitions, same bug one size up: blobFb=2.5k
   residual fallbacks, useFlush=38k pass closes, srvGpu 19 ms). The cap is
   now GONE: freelist storage is heap-owned and realloc-doubles on demand
   (`D9MT_BLOB_FREE_INITIAL` = 1024 entries, `d9mt_blob_free_reserve`) —
   entry drops are impossible by construction, not just rarer. The OOM last
   resort (realloc failure) merges the slice into an ADJACENT freelist entry
   at max serial: bytes preserved, only the neighbor's reuse delayed (the
   retire-merge hazard, deliberately confined to the unreachable path).
   `freelist_drops` / PROF `blobFlDrop=` can fire only on OOM AND an
   isolated slice — the never-fire tripwire, kept.
3. **Exact-size-only reuse stranded free bytes**: after a scene transition
   the next scene's sizes stop matching retired sizes — every alloc missed
   despite free bytes. Now: exact-size pass, then bump, then SPLIT-FIT of
   the smallest covered larger entry, then (pressure path, once) a
   covered-only coalesce + bump-frontier reclaim and one retry.
   Coalescing must NEVER happen at retire time: a fresh retire is uncovered
   and merging drags old bytes behind max-serial forever (the soak
   reproduced it: a rolling never-covered mega-block burned bump to the cap
   by frame 500).
4. **Mid-frame mirror staleness**: `blobAlloc` misses now refresh
   `g_srvCompletedFrame` once via `WireRing_stats` (unixcall on the MISS
   path only) and retry before falling back; the first 8 fallbacks log the
   requested size + pool state (`blob pool MISS #n size=...`).

Gate: test_feed **BLOB GATE 0b** (production-shaped soak) — 1000 frames at
3x the measured street rate, completion throttled 3 frames behind, two 6.8k
scene bursts with a size shift (16K->32K), per-16KiB-slot shadow asserting
zero misses, zero freelist drops, zero premature/aliased grants, PLUS the
scene-transition STORM: 10 waves each mass-retiring 8500+ slices in a
SINGLE frame (past the old 8192 cap) with a 16K<->32K size-class shift per
wave, re-allocating 4 frames later, churn running throughout. Every storm
frame asserts BYTE CONSERVATION (liveBytes + freelistBytes == bump — one
dropped entry breaks the equality that same frame) and the final frames
assert utilization returns to baseline. Mutation-verified: reintroducing an
8192 drop cap fails the gate at the first mass retire ("LEAK at frame 1001
— a freelist entry was lost"). It FAILS on every one of the four bugs above
individually.

### Street-artifact audit (the 30fps flood build), verified this round
- **Flickering black tile-grid**: NOT a DontCare reopen — `closePass`
  resets all clear flags, so every mid-frame reopen materializes with
  `MTLLoadActionLoad` (store is always Store; DontCare appears nowhere in
  pass setup). A load-action tripwire now guards this by construction in
  `materializePass` (DontCare on any attachment -> exec_errors, asserted 0
  in the flow-control storm gate + the mid-pass pixel gates). The grid is
  pass-shaped fallout of the flood itself: 149 pass closes/frame plus
  create-worker starvation (noRt=2.7k/window, dsUnres late-resolves) put
  never-rendered attachments on screen; collapses with useFlush -> 0.
- **Random geometry stretching**: NOT pool premature reuse — all three
  reuse gates compare in the conservative direction (stale completed
  mirror = LATER grants; retire stamp `markerMirror+1` is emitted by the
  same CS thread that emits uses, and the Rc refcount acq/rel edge makes a
  racing destructor read a stamp >= the last-use frame). NOT the V15
  rangeLive race — the fallback snapshots bytes at emission (`heapAlloc`
  memcpy) into ack-gated staging. The actual mechanism: under the
  fallback flood the 128 MiB staging heap goes FULL and `heapAlloc` DROPS
  V16 dirty-range buffer re-ships (`heap->dropped`, "shared heap full"
  warn) — the server then draws that range from the previous generation's
  bytes = mixed-frame vertices. Traffic on this path exists only while
  blobFb > 0; with the pool leak gone the path is residual-only and the
  heap cannot fill.

### qWire=0 (the "dead bracket emission") — NOT a transport break
Forensics: `dbbe412..a0e24c3` did not touch beginQuery/endQuery/createGpuQuery
(byte-identical), and the live log shows `qStub=0 qWire=0 qEvict=0 qSess=0`
across ALL 38 windows — every occlusion endQuery increments qWire OR qStub,
so zero of both proves the GAME issued zero occlusion brackets this session
(CreateQuery cannot fail: QuerySupported returns D3D_OK for OCCLUSION and the
ctor is a plain new). The game withheld its occlusion system under the
2-3 fps fallback-flood collapse; expect qWire to return at the game's rate
once blobFb=0 restores frame times. Liveness is now named:
"first occlusion bracket emitted (op 43)" logs once at emission, and the
5 server-execution query gates stay green (begun=5 published=5).

### noRt=2,724/window + dsUnres=5 (street artifacts) — same root, documented
Both appeared only in the fully-collapsed windows. The V17 reason log names
dsUnres as "still building (async create; late-resolves at draw)" — the
create worker starved behind the 1.8M staged-upload flood; noRt is the same
starvation on color-RT creates (draws skipped while the RT resource is
pending). No separate mechanism found; re-measure after blobFb=0 and only
then chase whatever residue remains (counters stay in srv-stats).

## Expected GTA signature (next real-game run)

- `upl=` per PROF window collapses ~100x (only texture + residual buffer
  records remain); `blob=` grows with the dynamic working set, `blobFb=`
  ~0 after settle.
- srv-stats: `useFlush=0`-class (residual only), `stg` ~0, passes/frame =
  scene shape (~60-90), srvGpu in the 10-15 ms class (the 38 GB/frame
  load/store traffic of upload-flush pass closes is structurally gone).
- Wrong picture debugging order: (1) `blobFb=` growing => region
  exhausted (bump 128 MiB or fix a leak in the freelist); (2) corruption
  in dynamic geometry => premature reuse — check the recycle-pool gate and
  the marker/completed mirrors before anything else (the PoC's failure
  mode); (3) `rej=` growing => blob create rejected server-side (offset
  out of region / wrap missing — check the attach log line
  "blob substrate: heap wrap ...").
