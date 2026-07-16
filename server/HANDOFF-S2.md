# HANDOFF — S2 (render-server executes), agent C

Phase S2 of docs/RENDER_SERVER_SPEC.md: the native arm64 server stops
counting and EXECUTES the wire stream — real frames render into its
IOSurfaces via METAL 4 ONLY, while the client still renders everything
locally (dual-execution validation). Read server/HANDOFF-S1-SERVER.md and
driver/mtl/HANDOFF-S1-CLIENT.md first; this document only covers the S2
delta.

## What EXECUTES server-side (executor.mm — the port)

A direct-ObjC++ Metal 4 port of the backend hot-loop state machine
(mtl_context_impl.h decisions 1-3, 5, 9; bodies ported from
mtl_context_record.cpp / mtl_context_transfer.cpp; ObjC translations of the
WMT calls taken from bridge/unix/winemetal_unix.c):

- **State folding**: all hot-loop setters (vertex layout, topology, blend,
  raster, depth-stencil, viewport/scissor, blend color, stencil ref, depth
  bias, push data) fold into a mirror of the client's state. Defaults are
  seeded to the client's (writeMask=0xF etc. — zero-init silently kills all
  color writes; that was the first executor bug).
- **Pass management** (immediate-mode shape): `setRenderTargetsById` closes
  the pass; clears fold into loadActions when the target matches a pending
  attachment, else run as a mini-pass; the encoder
  (MTL4RenderCommandEncoder) materializes lazily at the first draw. Producer
  barrier (fragment→all, device visibility) before every endEncoding (§4c).
- **PSO cache**: synchronous `MTL4Compiler
  newRenderPipelineStateWithDescriptor:` builds keyed on a memcmp PsoKey
  (libraries+entries, vertex attrs/layouts with LIVE stream strides, color
  formats+blend, sampleCount, a2c). Topology class stays Unspecified
  (point-size rule). Failed builds are cached as nil (no retry storm).
- **Argument tables**: per-pass rotated pair (vertex/fragment) with
  retire-by-serial (MTLSharedEvent timeline), pool grows only when every
  pair is in flight. CBVs at shader-derived slots (cbvSlotMap from the wire),
  vertex streams at 3+i, push data at 20, AB handle-word slices at the
  shader's abTableIndex — the FULL argument-buffer model works (abWords ride
  create_shader records).
- **Uploads**: push-constant/AB-word content rides the wire (push_data
  payload) and lands in a server-side slab ring (2 MiB slabs, rewind on
  serial retirement).
- **Draws**: draw/drawIndexed incl. instancing, u16/u32 indices with the
  MTL4 accessible-length rule, and triangle-fan emulation (the shared fan
  index pattern buffer, port of ensureFanIndices).
- **Submission spine**: 3-deep MTL4CommandAllocator ring gated on the shared
  event; ONE command buffer per frame, committed at presentFrame;
  `queue signalEvent:serial` after commit; deferred destruction of destroyed
  resources by serial; one queue-attached MTLResidencySet (add at create,
  commit-if-dirty before each queue commit).
- **Present**: presentFrame closes+commits the frame, publishes
  {present_slot, frame_serial, frames} in status v2, rotates the IOSurface
  slot. Backbuffer-flagged attachments substitute the current slot's
  IOSurface texture. First flagged RT whose size disagrees with the pool
  triggers a GPU drain + pool RESIZE (new IOSurfaces, generation bump in
  iosurface_serial[]).
- **QoS**: the consumer/executor thread declares
  `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)` at boot
  (main.m) — a default-QoS helper process gets E-core-scheduled like a
  daemon. EVERY future frame-critical server thread (S3 kick listener,
  encode workers) must repeat this line.

## What is STUBBED / dropped (deliberate S2 subset)

- **Texture CONTENT does not cross.** Lock/staging uploads never reach the
  server: wire-created textures are empty, wire-created buffers are
  zero-filled shared allocations (garbage-free vertex/index fetch — private
  buffers would feed undefined indices into vertex fetch, a GPU-fault
  class). Server rendering is therefore geometrically correct but textures
  are black and vertex data is all-zero (degenerate geometry). The S2 gate
  is frame production RATE + no crashes, not pixels.
- **Texture BINDS resolve to a 1x1 dummy.** bind_texture (op 3) carries the
  client VIEW handle; views have no wire identity in S2. Every texture slot
  is written with the dummy (defined sampling); samplers resolve for real
  (create_sampler records).
- **bind_render_targets (op 10) and clearRenderTarget (op 22) are counted,
  not executed** — their by-id twins (ops 30/32, emitted by the client from
  the same calls) are the executed ones.
- **invalidateBuffer retargets are invisible.** A Discard rename refreshes
  still-bound client slices with NO wire record; the server keeps drawing
  from the previously-bound backing (which still exists — destroys are
  deferred). Harmless while content doesn't cross; S3 must emit rebinds (or
  make renames explicit on the wire).
- Queries, copies/blits/resolves/mips, MSAA resolve textures, texel-buffer
  views, setFunctionConstants: not on the wire (cold, still client-local).
- depth-bounds, feedback-loop mask: carried but unused server-side.

## Wire additions (mtl_wire.h — appended opcodes, ABI additive)

| op | record | notes |
|---:|---|---|
| 25 | create_buffer | wire_id + client_handle (the BACKING id<MTLBuffer> handle buffer_refs carry; one per rename) + size; heap_offset reserved for S3 |
| 26 | create_texture | wire_id + client_handle + full MtlImageCreateInfo POD |
| 27 | create_shader | CHUNKED (uint16 record size vs metallib bytes): total/chunk_offset/chunk_size; chunk 0 carries abWords (2 B each) before the payload; + stage, ab flag, cbv_slot_map[8], abTableIndex, entry[32] |
| 28 | create_sampler | wire_id + client_handle + MtlSamplerKey POD |
| 29 | destroy_resource | wire_id + client_handle |
| 30 | set_render_targets_by_id | 4 color + ds refs: image WIRE id + view format/mip/layer/size + FLAG_BACKBUFFER |
| 31 | present_frame | client present serial + backbuffer wire id |
| 32 | clear_render_target_by_id | image wire id + aspects + clear value |

Client emission (mtl_wire_context.cpp) is **lazy create-at-first-wire-use on
the CS thread** (bind paths / RT binds), so creates always precede
references without a cross-thread ring producer. The ONE cross-thread path
is destroy: resource dtors (any thread) queue into a mutex vector
(wire::queueDestroy, mtl_wire_emit.h) drained at the top of reserveBytes —
destroy is queued after last use and BEFORE the Metal handle can be
released, so use→destroy→(handle-reuse)create order holds on the wire.
Shader metallib bytes are kept client-side on MtlShaderFunction
(wireMetallib) until first bindShader, because cbvSlotMap/abWords/entryName
are applied only AFTER createShaderFunction returns.

## status v2 (d9mt_status.h — additive, first 136 bytes unchanged)

New fields at 136+: `version` (=2, stored before epoch), `present_slot`,
`frame_serial`, `frames`, `encoders`, `draws_executed`, `draws_skipped`,
`psos_built`, `exec_errors`. Total 200 B. iosurface_serial[] now means POOL
GENERATION (bumped on resize), not frame serial — frames publish through
the v2 fields. WMTWireStats gained server_frames / server_frame_serial /
server_exec_errors (read only when version >= 2).

## Client present integration

- d3d9_swapchain.cpp PresentImage: `wire::noteBackbuffer(swapImage)` before
  the present EmitCs; `wire::presentObserved()` after SyncFrameLatency.
- MtlWireContext::synchronizeWsi emits present_frame (op 31) before
  forwarding — the CS-order present point.
- PROF line gained `srvFrames=<delta>` (server-executed presents per
  window), `srvPace=<cum>` (presents where server serial >= local-1),
  `srvExecErr=<cum>`. Gate for a real game: srvFrames tracks the local
  frame rate, srvExecErr stays low, decodeErr stays 0.

## Test evidence (scripts/build-server.sh gate, PASSING)

test_feed extended (S1 checks unchanged): feeds create_shader (vs/fs from a
build-time metallib, xcrun metal on server/test_stub.metal) + backbuffer RT
+ clear blue + 50 fullscreen-triangle draws + presentFrame; asserts
version=2, frames=1, frame_serial=1, draws_executed=50, psos_built>=1,
encoders>=1, exec_errors=0, present_slot=0, AND reads the presented
IOSurface via IOSurfaceLookup: pixel(0,0) is RED — the draws demonstrably
landed cross-process. S1's Metal-3 liveness clear moved into the executor
as an MTL4 clear+readback-verify (iosurface_pool.m no longer owns a queue —
METAL 4 ONLY holds server-wide).

## Honest flags / gotchas for S3

1. **Ring backpressure**: 4 MiB assumed a counting consumer. The executor
   is slower (PSO compiles!); a full ring spins the producer up to 2^20 and
   then PERMANENTLY disables the transport. A synchronous PSO compile storm
   at level load is the likely first trip. S3 wants real backpressure (or
   async PSO builds server-side).
2. **Heartbeat stalls during PSO compiles** (single-threaded consumer=
   executor): the client only logs hb=0 windows today, but don't tighten
   heartbeat-based death detection without moving the heartbeat to its own
   thread.
3. **Server frame publication precedes GPU completion** (commit+signal,
   no wait): frame_serial says "committed", not "scanned out". Visual
   cutover (S3) must gate the client's IOSurface consumption on the shared
   event or a completed-serial field.
4. **Backbuffer slot texture goes nil after present** until the next
   flagged setRenderTargetsById (the client re-binds every frame; draws in
   between would be skipped). If a game never re-binds after Present, S3
   must re-resolve the flag lazily.
5. **Pool resize leaks the previous IOSurface generation** by design
   (client lookups may straddle); bounded by display-mode changes.
6. **PSO parity is entry-name + library identity**, not linkage-variant
   aware beyond that (each linkage variant IS a distinct library, so this
   matches the client's vsCookie/fsCookie folding).
7. **wow64**: mtl_wire_emit.h/std::mutex paths compile in the PE driver —
   both mingw targets built clean; no unix types cross.
8. **Stale destroys after transport death** are dropped mid-drain (server
   exiting anyway); queue caps at 65536 and self-clears.

## Exactly what S3 must add

1. **Texture/buffer CONTENT transport via the shared heap** (spec region
   #2): client Lock/staging writes land in the shared-heap file; server
   wraps regions via newBufferWithBytesNoCopy; copy records
   (copyBufferToTexture et al) get wire records; create_buffer.heap_offset
   is already reserved. Constants then also stop riding record payloads
   (bindUniformBuffer content becomes heap slices).
2. **View identity**: create_view records (or view keys folded into
   bind_texture) so texture binds resolve to REAL textures — kills the
   dummy-texture substitution.
3. **Visual cutover**: client present blits the server's IOSurface (status
   v2 {present_slot, frame_serial}; IOSurfaceLookup once per generation,
   newTextureWithDescriptor:iosurface:plane:) instead of its own backbuffer;
   gate on completion (flag 3 above). Client stops local execution (thin
   recorder).
4. **Kick channel replacing the 1 ms poll**: pipe/kqueue EVFILT_USER or
   os_sync futex; the producer's empty→nonempty kick protocol is already in
   place (WireRing_kick no-op slot), the consumer park loop is the only
   change. Keep QOS_CLASS_USER_INTERACTIVE on every new thread.
5. **invalidateBuffer/rename rebind records** (flag above) — required the
   moment content matters.
6. **Client-death watchdog**: kqueue EVFILT_PROC on the client pid (S1
   handoff gotcha; still open — a client crash leaks the server until the
   ring file unlink check happens to fire, and nobody unlinks on a crash).
