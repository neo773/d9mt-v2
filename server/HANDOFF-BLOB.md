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
