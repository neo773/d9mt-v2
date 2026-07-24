# HANDOFF — OCCLUSION-QUERY TRANSPORT (the workload-restoring slice)

Branch `native-backend`, on top of e85d64d. Closes the HANDOFF-ALLIN
"queries + readbacks" debt for QUERIES (readback transport remains open,
see HANDOFF-CONSOLIDATION C2). Predecessors: HANDOFF-ALLIN.md (the stub
being replaced), HANDOFF-S3.md (heap/session/serial machinery this rides).

## Why this is the perf-correctness slice

The ALL-IN occlusion stub answered "all samples passed" instantly — every
occlusion-culled draw the game would have skipped was drawn anyway. GTA IV
street driving measured **4,700 draws/frame server-side** against the
game's native 2,500–4,000 under its own culling; the server drowned
(srvGpu 281 ms avg, consumer seconds behind) while rendering CORRECT
frames, seconds late. Real query results hand the game back its own
culling. Also correctness: sun flares / LOD selection read these counts.

## Mechanism

### Wire (client -> server), stream-ordered
- New opcodes (mtl_wire.h): `begin_query = 43`, `end_query = 44`
  (`d9mt_wire_rec_query`: hdr + u64 query_id, 16 B). They ride the SAME
  ring as the draws they bracket — ordering is free.
- `query_id`: client-assigned monotonic u64 (`g_nextQueryId`),
  **process-lifetime, never reset per session** — result-slot reuse
  detection depends on ids never repeating.
- Client (`MtlWireContext::beginQuery/endQuery`, mtl_wire_context.cpp):
  occlusion brackets emit the records and stamp
  `MtlBackendQuery::{wireQueryId, wireSession}`; nothing forwards to the
  local context (the local visibility buffer would read 0 — the worst
  lie). If emission fails (no consumer / dead wire / no heap) endQuery
  falls back to the documented instant all-passed stub — still counted as
  PROF `qStub=` (must sit ~0 with a live server).

### Server (executor.mm): Metal 4 visibility results
- One 64 KiB shared `visBuf` (8192 x u64 counter slots,
  `HazardTrackingModeUntracked`, residency-added). Every
  `materializePass` sets `MTL4RenderPassDescriptor.visibilityResultBuffer`
  (port of the client's OLD local binding, mtl_context_record.cpp:549).
- `begin_query`: single active bracket (D3D9 occlusion queries cannot
  nest; a nested begin force-ends the previous, counted `qNested`). If a
  pass is live, a slot is armed:
  CPU-zero the slot, `setVisibilityResultMode:Counting offset:slot*8`.
- **Pass splits**: Metal visibility counters are per-encoder, so every
  encoder segment inside a bracket gets a FRESH slot (armed at
  `materializePass`); the CPU sums the segments at publication. This is
  an upgrade over the old local impl (one slot per bracket, splits lost).
- `end_query`: `setVisibilityResultMode:Disabled`, bracket moves to the
  pending queue stamped with the serial covering its LAST slot
  (`g->cmdbuf ? g->serial : g->lastSubmitted`).
- Slot lifecycle: free-running cursor vs `qRetired` (advanced when a
  pending publishes). Pool exhaustion (8192 un-published segments) or a
  missing visBuf marks the bracket `overflow` -> published all-passed
  immediately (counted; never a stale-counter readback, never occluded on
  a transport artifact).

### Results return path: ring inside the heap file
`d9mt_wire_query_ring` (mtl_wire.h) lives at fixed offset
**D9MT_WIRE_QUERY_RING_OFFSET = 512** inside the `heap` shared file — the
reserved gap between the 192 B heap control block and the 16 KiB-aligned
data area. Both sides already map this file; the return path needed ZERO
new transport plumbing and the client poll is two atomic loads (no
unixcall).

- **Capacity: D9MT_WIRE_QUERY_SLOTS = 512** slots x 24 B
  (id/samples/serial) + 64 B header = 12352 B (fits 512..16384 with
  headroom; asserted). Direct-mapped: a result lives ONLY in slot
  `id & 511`.
- Slot write protocol (server, under `queryMu`): id=0 (release) ->
  samples/serial (relaxed) -> id=qid (release). Reader: acquire id, read
  payload, RE-acquire id — a stable matching id brackets a consistent
  payload (overwrites pass through 0).
- Reader classification: `got == id` -> result; `got > id` -> **evicted**
  (>512 newer results landed before the game polled; resolved all-passed,
  PROF `qEvict=` — the capacity knob's tripwire); else pending.
- **No early lies**: publication happens ONLY in `sweepQueries()` when
  `completedSerial()` (the MTLSharedEvent queue-timeline value) covers the
  bracket's serial. Sweep runs on the consumer (begin/end/present) AND on
  the **watchdog 10 ms tick** — a game that stops emitting records while
  it spins on GetData still gets its results (the consumer may be parked
  on an empty ring forever).
- The ring is zeroed once at executor boot, then magic release-stored.
  NEVER zeroed on session boundaries (ids are client-process-monotonic; a
  fresh server process implies a fresh client process).

### Client GetData (async-poll BY DESIGN — never blocks)
`MtlBackendQuery::getData` (mtl_context_transfer.cpp) with a nonzero
`wireQueryId` calls `backend::wire::queryPoll` (mtl_wire_context.cpp):
- pending -> `MtlGpuQueryStatus::Pending` -> D3D9 GetData returns
  **S_FALSE** (the game keeps polling; that is the D3D9 contract).
- available -> the real sample count.
- Every "cannot arrive" path resolves **all-passed with a named counter**
  (never wedge a game):
  - `qSess=`  — wire session bounced under the query, wire died/disabled
    (`g_wireDown` latches at every transport-disable site,
    `g_serverDead` from the heartbeat latch), or heap absent.
  - `qEvict=` — ring eviction (above).
  - A missing ring magic while the wire is HEALTHY reports pending (server
    boot window), bounded by the dead-server latch.
- App-thread safety: `queryPoll` reads only atomics + the write-once heap
  pointer; `g_wireSessionMirror`/`g_wireDown` mirror the CS-owned g_wt
  fields.

## Session semantics
- Client: `wireSession` mismatch -> all-passed (`qSess=`). endQuery never
  emits an end for a begin from a previous session.
- Server: `handleSession` converts an in-flight bracket to a normal
  pending (`qSessionEnd` counted) — publication is harmless (ids never
  reuse), and the slots must still retire on GPU completion. Pendings
  publish/retire normally through the drain.

## Counters
- Client PROF: `qStub=` (non-wire fallbacks, ~0 in production), `qWire=`
  (brackets serialized), `qEvict=`, `qSess=`.
- Server stderr (finish): `queries: begun= published= nested= orphan=
  overflow= sessEnd=`; ring header `published`/`dropped` are visible to
  both sides.

## Gates (server/test_feed.c "OCCLUSION-QUERY TRANSPORT GATES", all green
in the full suite via scripts/build-server.sh)
- (a1) empty bracket -> exactly 0 (a counting query must not stub).
- (a2) draw scissored to 1x1 -> exactly 1 (counter follows raster state;
  a scissored-out draw contributes nothing).
- (a3) fullscreen draw -> exactly 640*480 = 307200 (== pixel area).
- (b) bracket spanning a pass split (RT rebind mid-bracket) -> exactly
  2*307200 — per-encoder segments summed.
- (c) bracket in a never-submitted frame stays PENDING through 300 ms of
  watchdog sweeps; publishes only after the present submits, with a
  serial `completed_serial` reaches. Poll-loop shape == the D3D9 GetData
  loop.
- All existing gates green; full-clean chain (rm -rf build; build-bridge;
  build-driver; build-server) built clean.

## Expected GTA IV signature
- PROF: `qStub=` ~0, `qWire=` climbing at the game's query rate,
  `qEvict=`/`qSess=` ~0 (nonzero qEvict = bump D9MT_WIRE_QUERY_SLOTS).
- Server draws/frame falls from the stubbed 4,700 toward the game's
  native 2,500–4,000 envelope — and in dense streets MUCH lower, wherever
  its own culling decides. srvGpu and consumer lag drop proportionally.
- Correctness: sun flares stop shining through buildings; LOD selection
  follows real visibility.

## Known edges (documented, not blockers)
- Draws skipped server-side (pending PSO, boot storms) do not count into
  a bracket — a bracket made ENTIRELY of skipped draws publishes 0. Boot
  transient only.
- 512-slot ring assumes the game polls within 512 results of issue;
  `qEvict=` names any violation (resolved all-passed, never wedged).
- Readback transport (GetRenderTargetData / GetFrontBufferData) is still
  the open C2 debt — this slice is queries only.
