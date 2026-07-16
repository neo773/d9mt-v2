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
