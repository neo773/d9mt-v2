# HANDOFF — S1 client (wire ring -> render server), agent B

Client half of render-server Phase S1 (docs/RENDER_SERVER_SPEC.md). Pairs
with server/HANDOFF-S1-SERVER.md — read that first for the server contract.
The ring moved from heap to a MAP_SHARED file; the in-process consumer thread
is GONE; d9mt-serverd is spawned and consumes cross-process. The PE producer
code is byte-for-byte the same hot path as before.

## Files
- `bridge/unix/winemetal_unix.c` — the whole client lives here (grep
  `_WireRing_`). attach = shm dir + files + MAP_FIXED remap + posix_spawn +
  epoch wait + fallback; detach = drain-wait + SIGTERM + unlink; stats now
  also reads the status block; kick is a NO-OP in S1 (server polls). The old
  wire_consumer_main pthread is deleted (ported earlier to server/consumer.m).
- `bridge/winemetal.h` — WMTWireStats widened (heartbeat, server_error),
  enum WMTWireAttachMode, WireRing_attach now returns the mode,
  WireRing_detach added.
- `bridge/winemetal_thunks.{h,c}` — unixcall_wirering_attach gained
  mode/padding out-fields; new thunk code 214 = WireRing_detach (appended to
  BOTH dispatch tables, lock-step).
- `driver/backend/mtl_wire_context.{h,cpp}` — ring region is now VirtualAlloc
  (page-aligned, required for MAP_FIXED), ctor consumes the attach mode, dtor
  calls WireRing_detach, PROF line gained hb/srvErr, waitForSpace disables
  instantly on ring-full when no consumer exists.
- `driver/mtl/mtl_wire.h` — comments only; ring/record ABI UNCHANGED.
- `scripts/build-driver.sh` — tail-calls scripts/build-server.sh, non-fatal,
  guarded on server/ existing.

## Contracts I defined (client side)

### Address space — THE constraint everyone forgets
d3d9.dll is i686: the PE producer can only address < 4 GiB, so the unix side
canNOT hand back a fresh mmap pointer. Instead the PE side VirtualAllocs the
region (page-aligned) and the unix side maps the `ring` FILE over that exact
VA with mmap(MAP_SHARED|MAP_FIXED). Same pointer before/after; file-backed
after. Never munmap that VA unix-side — restore anon MAP_FIXED on fallback,
and let the PE VirtualFree (after WireRing_detach) drop the last file ref.

### Shm dir layout (client-created, client-unlinked)
`$TMPDIR/d9mt-<unix pid>/` (TMPDIR fallback `/tmp`), mode 0700:
- `ring`   = 192 B d9mt_wire_ring control block + 4 MiB data
  (WireRingCapacity in mtl_wire_context.cpp), header initialized by the
  unix side AFTER mapping, BEFORE spawn.
- `status` = 136 B zeroed (server/d9mt_status.h is authoritative; offsets in
  server/HANDOFF-S1-SERVER.md). Mapped unix-side only — the PE/wine side
  never touches iosurface fields in S1.

### Spawn + path resolution (in order, first executable wins; logged)
1. `$D9MT_SERVERD_PATH` (dev trees)
2. next to winemetal.so (dladdr)
3. `/usr/local/lib/d9mt/d9mt-serverd`
posix_spawn argv = {exe, shm_dir}; server picks default surface size (S1
doesn't pass width/height — S2 must, from the real swapchain).

### Timeouts (all bounded, driver never hangs)
- epoch==1 handshake: 3 s, 1 ms poll, early-out if the child dies first.
- detach: <=500 ms for drain (consumer_exited OR status.progress ==
  ring.head), then SIGTERM + reap within a 950 ms deadline, SIGKILL as last
  resort. Total < 1 s.
- ring-full with a live server: the old 2^20-spin bound stands; ring-full
  with NO server (fallback mode): disables immediately (no spin).

### Fallback (server missing / spawn fails / epoch timeout) — MANDATORY
ONE `[wire] WARN:` line, files unlinked, region restored to anonymous memory,
attach returns WMTWireAttachLocal. The producer keeps writing until the ring
fills once (~4 MiB of records), then stalls++ -> m_active=false -> permanent
forward-only. Driver behavior is then identical to pre-S1.

### Stats ABI (WMTWireStats, PE+unix compiled from one header)
records/bytes/stalls/decode_errors (as before) + `heartbeat` u64 +
`server_error` u64 (widened from the status block's u32). PROF line format:
`| wire: rec=N bytes=N stalls=N decodeErr=N hb=<delta/window> srvErr=<code>`
hb==0 across a window with a server attached means the server is DEAD.

### Teardown ordering (dtor, exact sequence)
1. Detach record (counts into records: expect N+1, see server handoff)
2. ring->shutdown = 1 (release) + kick (no-op, protocol-preserving)
3. WireRing_detach() — the bounded unix-side sequence above + unlink dir
4. VirtualFree of the ring region (last file reference dies here)

## Tested vs not
- Tested by the build gates: full compile both compilers/ABIs;
  build-server.sh test_feed gate (spawn/epoch/progress/heartbeat/detach)
  passes — but that exercises the SERVER with a synthetic client, not this
  client.
- NOT exercised (needs a real game run): the actual in-wine attach
  (MAP_FIXED over VirtualAlloc under wow64+Rosetta — PoC'd P4 in the spec,
  but not with THIS code), fallback-when-binary-missing inside wine, the
  hb/srvErr PROF numbers, detach under a live game, and ring-full behavior
  at real draw rates. First game run: check the log for
  `[wire] render server attached: pid=...` then PROF `hb=~100/s, srvErr=0,
  decodeErr=0`.

## Gotchas for the S2 agent
- WireRing_kick is a no-op; the producer still follows the empty->nonempty
  protocol, so a real kick channel (pipe/kqueue/futex) can slot into
  _WireRing_kick without touching the producer.
- Client crash without detach leaks the shm dir and the server: the server's
  only client-death check is the ring file being unlinked (~10 Hz stat) and
  nobody unlinks it on a crash. S2 should add kqueue EVFILT_PROC on the
  client pid server-side, or an O_EVTONLY watch.
- One ring per process FOREVER: g_wireClaimed never resets, so a game that
  destroys + recreates the device gets no transport the second time (same as
  Phase 1 behavior, now with a real server exit in between).
- S1 never passes swapchain size to the server; the IOSurfaces are the
  default 2992x1934. S2 must plumb width/height (spawn argv or a wire
  record) before the present-blit cutover.
- The 4 MiB capacity assumes the consumer only counts. Once the server
  EXECUTES records it can fall behind; the producer then spins up to 2^20 per
  stall episode and PERMANENTLY disables the transport on timeout — S2
  probably wants back-pressure semantics instead of the kill switch.
- error codes surface as PROF srvErr; nothing in the client reacts to them
  yet (S1 has nothing to fall back FROM — execution is still local).
