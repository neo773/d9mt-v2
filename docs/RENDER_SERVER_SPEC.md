# d9mt Render Server — cross-process native backend (the Rosetta escape)

Verified premise (2026-07-17): the whole wine process is x86_64-translated; the
ONLY way to run driver code (and Metal.framework itself) native ARM64 is a
separate helper process. Prior art: Chromium GPU process (shm command buffer +
IOSurface), Mesa Venus (Vulkan serialized over a shm ring across a VM).

## Feasibility: PROVEN (PoC, 2026-07-17)

All four potential blockers verified with running code (scratchpad PoC pair,
arm64 server + x86_64 client, plus a temporary in-wine probe since removed):
- P1 cross-ISA shm atomics: 10k ping/pong translated<->native, 0 ordering
  violations (acquire/release over MAP_SHARED).
- P2 IOSurface cross-process: native Metal rendered pixels, translated client
  IOSurfaceLookup + byte-identical checksum. NOTE: rides kIOSurfaceIsGlobal
  (deprecated; Chromium precedent) — mach-port transfer is the fallback.
- P3 foreign IOSurface wrapped as MTLTexture in the TRANSLATED process's own
  Metal device, GPU blit, checksum match.
- P4 from INSIDE a real wine process running GTA IV: posix_spawn of the
  arm64 helper + full shm handshake + IOSurfaceLookup all succeeded.

## Processes

- **client** = wine process (x86_64/Rosetta): game + d3d9.dll + winemetal.
  Produces wire records (driver/mtl/mtl_wire.h — EXISTING, from the transport
  milestone), writes bulk data into shared regions, presents IOSurfaces.
- **server** = `d9mt-serverd` (arm64 Mach-O, new top-level dir `server/`):
  consumes records, owns ALL Metal objects (device, queues, PSOs, textures,
  buffers, encoders), renders, publishes results.

## Shared memory layout (all mmap MAP_SHARED files under
## $TMPDIR/d9mt-<pid>/, created by the client, unlinked on teardown)

1. `ring` — the EXISTING WireRing control block + payload (mtl_wire.h),
   relocated from heap to this file. Producer=client, consumer=server.
2. `shared-heap` — bump/pool allocator for bulk data (vertex/index/constant
   uploads, texture staging). Server wraps regions via
   newBufferWithBytesNoCopy (page-aligned slices). A region table at the
   head: {offset,u64 size,u64 generation} slots, allocation protocol in
   mtl_wire.h additions.
3. `status` — server→client: progress cursor (consumed ring offset),
   completed submission serial (u64), IOSurfaceID per swapchain slot
   (u32[8]), server heartbeat/epoch, error code. Client waits = poll/futex
   on these counters (NO MTLSharedEvent needed initially — serials are
   already plain counters in this driver).

## Handshake / lifecycle

- Client creates files, spawns server (posix_spawn from the unix side of
  winemetal) with argv = the shm dir path. Server maps, writes epoch=1.
- Client timeout on epoch => fall back to IN-PROCESS execution (the code
  that exists today keeps working — the fallback IS the current driver).
- Server exit/crash: client detects via kqueue on pid / heartbeat staleness,
  logs, falls back in-process for the rest of the session. Never hangs.
- Teardown: Detach record (exists in mtl_wire.h), server acks via status.

## Command surface (phased)

- Phase S1: server consumes + validates + counts (echo semantics of the
  existing transport, relocated cross-process). Client still executes
  everything locally. Proves spawn/shm/ring/status across processes.
- Phase S2: server executes the HOT LOOP natively (backend state machine
  compiled arm64) INTO ITS OWN Metal device, rendering into IOSurface-backed
  render targets mirroring the client's swapchain; client present blits the
  IOSurface instead of its own backbuffer. Cold calls (creates/copies) get
  wire records at this point too — resource identity becomes a server-side
  id allocated by the client (monotonic u64), NOT obj_handle_t (which is
  client-process-local).
- Phase S3: client stops local execution (thin recorder), full cutover.

## IOSurface contract

- Server creates swapchain-sized IOSurfaces (BGRA8), wraps as MTLTextures,
  renders/blits final frames into them, publishes IOSurfaceID + frame serial
  in `status`. Client: IOSurfaceLookup(id) once per generation, wraps via
  newTextureWithDescriptor:iosurface:plane:, present-blits into its drawable
  (existing present path — the blit source changes, nothing else).

## Build

- `server/` builds with clang -arch arm64 -O2, links Metal/QuartzCore/
  IOSurface frameworks directly (NO bridge, NO thunks — direct ObjC).
- Reuse strategy: the Metal-call bodies in bridge/unix/winemetal_unix.c are
  the reference implementations; server-side backend port extracts the
  needed logic into `server/` sources (copy-adapt, do NOT try to compile
  winemetal_unix.c itself — it is wine-entangled).
- Build entry: `scripts/build-server.sh` (new), invoked at the end of
  build-driver.sh (non-fatal if it fails while experimental).

## Invariants

- METAL 4 ONLY on the server side.
- Fixed-width wire ABI, static_asserts both sides (existing discipline).
- Client NEVER blocks forever on the server: every wait has the heartbeat
  fallback. The in-process path remains complete and is the fallback.
- No env flags: branch policy, git revert is the safety net.
