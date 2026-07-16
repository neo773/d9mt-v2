# 70 — Render server: the Rosetta escape (current state)

Authoritative docs — do not duplicate, read them:
- `docs/RENDER_SERVER_SPEC.md` — the spec (processes, shm layout, phases,
  IOSurface contract, invariants).
- `server/HANDOFF-S1-SERVER.md` — S1 server handoff (status ABI + gotchas).
- `driver/mtl/HANDOFF-S1-CLIENT.md` — S1 client handoff (address-space
  constraint, spawn/timeouts/fallback/teardown contracts).
- `docs/NATIVE_BACKEND_PLAN.md` — the falsified in-process plan (history +
  the surviving wire transport).
- `server/README.md` — build/run/status table. (Its "NOT wired into
  build-driver.sh" note is stale — build-driver.sh tail-calls
  build-server.sh, non-fatal.)

## Premise (verified 2026-07-17)

The ENTIRE wine process is x86_64-translated under Rosetta — wineserver,
ntdll.so, winemetal.so, and Metal.framework code executing in-process. macOS
cannot mix ISAs in one process, so "move work to the unix side" buys nothing
(in-process Phases 2-3 of NATIVE_BACKEND_PLAN: CANCELLED; mtld3d ships
x86_64 unix-side too). The only true native option is a **separate arm64
render-server process**: `d9mt-serverd`, fed by the existing wire ring over
shared memory. Prior art: Chromium GPU process, Mesa Venus. Feasibility PoC
(all four blockers) passed: cross-ISA shm atomics, cross-process IOSurface,
foreign IOSurface as MTLTexture in the translated process, posix_spawn +
handshake from inside a real wine process running GTA IV.

## What exists (S1 — SHIPPED, commits 60f8657..6b0f3ac)

Echo semantics relocated cross-process; the client still executes everything
locally (the in-process path IS the fallback, complete forever).

- **Client** (all in `bridge/unix/winemetal_unix.c`, grep `_WireRing_`, plus
  `backend/mtl_wire_context.*` PE-side): the ring moved from heap to
  `$TMPDIR/d9mt-<pid>/ring` via the MAP_FIXED-over-VirtualAlloc trick
  (40-bridge.md); `WireRing_attach` creates the shm dir + `status` file,
  spawns the server (`$D9MT_SERVERD_PATH` → next-to-winemetal.so →
  /usr/local/lib/d9mt/), waits epoch==1 (3 s bound), falls back to local
  plain-memory mode on any failure (one `[wire] WARN:` line, then behavior
  identical to pre-S1). Detach is bounded < 1 s. The old in-process consumer
  pthread is DELETED.
- **Server** (`server/`): `main.m` maps ring+status, `consumer.m` drains
  (1 ms poll park, ~100 Hz heartbeat, exits on Detach/shutdown/ring-unlink),
  `iosurface_pool.m` boots 3 BGRA8 IOSurfaces wrapped as MTLTextures and
  clear-verified (GPU liveness proof). `d9mt_status.h` is THE 136-byte
  status ABI (epoch/heartbeat/progress/completed_serial/error/
  iosurface_id[8]/iosurface_serial[8]).
- **Gate**: `scripts/build-server.sh` builds arm64 server + x86_64
  `test_feed` (deliberately cross-ISA) and RUNS it: 1000 records + Detach,
  asserts progress/records==1001/decode_errors==0/heartbeat/3 IOSurface IDs.

**Proved under a real game**: GTA runs with the transport attached at full
draw rate — order 16k records/frame with 0 producer stalls and 0 decode
errors on the PROF wire line (`rec=… stalls=0 decodeErr=0 hb=~100/s
srvErr=0`), confirming SPSC ring + cross-ISA atomics + spawn/handshake at
production rates. First-run checklist: log shows
`[wire] render server attached: pid=…`, then those PROF fields.

## Phases

- **S1 (done)**: spawn/shm/ring/status across the process + ISA boundary;
  consume/validate/count only.
- **S2 (IN FLIGHT — check `git log server/` before assuming)**: server
  executes the HOT LOOP natively (backend state machine compiled arm64,
  MTL4-only) into its own Metal device, rendering into IOSurface-backed RTs
  mirroring the client swapchain; client present-blits the IOSurface instead
  of its backbuffer. Cold calls (creates/copies) gain wire records; resource
  identity becomes a client-allocated monotonic u64 (obj_handle_t is
  process-local). Known S2 work items from the handoffs: real kick channel
  (pipe/kqueue/futex) replacing the 1 ms poll, swapchain size plumbing (S1
  IOSurfaces are hardcoded-default sized), back-pressure instead of the
  ring-full kill switch, client-crash reaping (kqueue EVFILT_PROC), mach-port
  IOSurface transfer replacing deprecated kIOSurfaceIsGlobal.
- **S3**: client stops local execution (thin recorder), full cutover. Then
  the deferred-pass rules re-default (submission overlap exists;
  30-backend.md decision 10).

## Invariants

- METAL 4 ONLY server-side (S1's Metal-3 liveness clear in iosurface_pool.m
  is flagged TODO(METAL4) and must not survive S2).
- Fixed-width wire/status ABI, static_asserts on both compilers.
- Client NEVER blocks forever on the server — every wait has a
  heartbeat/timeout fallback; in-process execution remains complete.
- No env gating; git revert is the safety net.
- One ring per process forever (`g_wireClaimed` never resets) — a device
  destroy/recreate gets no transport the second time.
