# HANDOFF — kernel wakeups (os_sync_wait_on_address replaces sleep-polling)

Every cross-process wait on the frame path is now a kernel futex wait
(`os_sync_wait_on_address*`, macOS 14.4+, libSystem) with a kernel wake on
the store side. No spin/sleep polling loop remains on the frame path; every
wait keeps a BOUNDED timeout backstop, so a lost wake degrades to the old
poll latency — never a hang — and all heartbeat/watchdog semantics are
byte-identical.

## The one helper

`driver/mtl/mtl_futex.h` — unix/server sides ONLY (`#error`s under mingw).
`d9mt_futex_wait(addr, expected, timeout_ns)` (0 = woke/changed, -1 =
timeout), `d9mt_futex_wake_one/_all(addr)`. SHARED flag hardcoded (every
address lives in the MAP_SHARED ring/status files — wait and wake match on
file offset, not per-process VA). The canonical discipline (load-check →
wait(addr, observed, slice) → re-check) and the full memory-ordering
contract are documented at the top of the header. If the syscall itself ever
fails, the wait body degrades to a bounded usleep and warns once
(`[d9mt_futex]` on stderr) — the API is unchanged, the swap is one function
body.

## Memory-ordering contract (summary; header is authoritative)

- **head** (producer→consumer): record bytes, THEN `head.store(release)`,
  THEN kick. Consumer `head.load(acquire)` before reading bytes. The kernel
  compares `*addr` under its own lock, so wake-before-wait is never lost.
- **tail** (consumer→producer): `tail.store(release)`, then
  `atomic_thread_fence(seq_cst)`, then load `space_waiters`; the waiter
  registered with `fetch_add(seq_cst)` BEFORE its acquire re-check of tail.
  Either the consumer sees the registration and wakes `&tail`, or the
  waiter's re-check sees the new tail.
- **frame_serial** (server→client): release-store, then `wake_all`
  unconditionally (once per frame; no-waiter wake = cheap ENOENT).

## Sites converted (frame path)

1. **Server consumer idle park** (`server/consumer.m`): `usleep(1000)` →
   `d9mt_futex_wait(&ring->head, head, 1ms)`. The 1 ms backstop equals the
   old poll latency on a lost wake and keeps the unlink/pid checks (~10 Hz)
   on cadence. (V19: the heartbeat no longer lives in this loop at all —
   see HANDOFF-S3.md V19; the executor watchdog owns it on a fixed timer.) `_WireRing_kick` (unixcall 212,
   `bridge/unix/winemetal_unix.c`) is no longer a no-op: it wakes
   `&ring->head`; the producer still kicks ONLY on the empty→nonempty
   transition. Consumer exit wakes `&ring->tail` so a parked producer fails
   fast.
2. **Producer space wait** (`driver/backend/mtl_wire_context.cpp`
   `waitForSpace`): the 2^20 `Sleep(0)` spin per ~1 s window → 1024-iter
   short spin (hot path: space frees in µs, zero thunk cost) + ONE bounded
   kernel wait per window via the new unixcall. Window count, per-window
   stall accounting, frozen-heartbeat dead-server disable, and the
   consecutive-zero-progress permanent disable are all unchanged.
3. **Client publish notice** (`acquireServerFrame`): before reading the
   published frame, park up to `kPublishWaitUs` (3 ms) for
   `status->frame_serial >= localSerial - 1` (this present's own marker is
   emitted after the acquire, so the target is the PREVIOUS frame). Wake
   side: both executor publish paths (`server/executor.mm`, listener block +
   inline) `wake_all(&status->frame_serial)` right after the release-store.
   On timeout — and always at boot (the unix side never waits while
   `frame_serial == 0`) — behavior is exactly the old one: present the
   freshest published frame.
4. **Ring struct** (`driver/mtl/mtl_wire.h`): `space_waiters` (au32) carved
   out of `_pad1` — 192 B control block and all offsets unchanged; both
   sides compile the same header (static_asserts).

## PE thunk ABI (the producer cannot call os_sync itself)

Unixcall **217** (`struct unixcall_wire_wait`, 32 B, static_assert-pinned in
`bridge/winemetal_thunks.h`; appended to BOTH dispatch tables lock-step in
`bridge/unix/winemetal_unix.c`): `{u32 op, u32 timeout_us, u64 a, u64 b} →
u64 out_value`. Ops (`enum WMTWireWaitOp`, `bridge/winemetal.h`):
- `WMTWireWaitSpace` (0): a = producer head, b = bytes needed; parks on
  `ring->tail` (registers in `space_waiters`), also returns on
  `consumer_exited`; returns tail.
- `WMTWireWaitFrameSerial` (1): a = target serial; parks on
  `status->frame_serial`; never waits while serial == 0 (boot); returns the
  serial. Internal re-check slices are ≤100 ms, so even a lost wake inside
  the thunk re-checks well inside any caller budget.
PE wrapper: `WireRing_wait(op, timeout_us, a, b)`
(`bridge/winemetal_thunks.c`).

## NOT converted (off the frame path, polling is fine — documented why)

- Attach epoch handshake (3 s / 1 ms poll) and detach drain (≤500 ms):
  once-per-session, bounded, and run before/after a server exists to wake us.
- `WireRing_quiesce` epoch-bump poll: diagnostic, once per context death.
- Client dead-server heartbeat latch (`presentObserved`): reads stats it
  already fetches per present; no sleep loop exists there.
- Server heartbeat itself: it IS the liveness signal; it must tick on a
  clock, not on wakes. (V19 made this literal: the consumer loop's bump —
  which still coupled the tick to the loop cycling, i.e. to work — moved to
  the executor watchdog's fixed 10 ms timer. Liveness is a property of the
  process, not of work arriving.)
- test_feed `wait_u64`/scenario polls: test-side observation loops.

## Gates (server/test_feed.c, run by scripts/build-server.sh)

- **Wake latency**: 1k kicked records against a parked consumer, x86_64
  (Rosetta) waker → arm64 waiter over MAP_SHARED — the production pair, so a
  PASS also proves the SHARED flag crosses the ISA boundary. Assert p50 <
  200 µs. **Measured: p50 = 12–14 µs, p90 = 30 µs, max = 66–177 µs** (old
  park: ms-class).
- **Publish notice**: present → futex wait on `frame_serial` (arm64 waker →
  x86_64 waiter, reverse direction). Wake observed **429 µs** after the
  present emit (GPU-completion-gated, ms-class expected and fine).
- **Lost wake**: one kick suppressed (`g_skip_next_kick` hook); the 1 ms
  backstop consumed the record in **767–785 µs**, stream continued. Bound
  asserted < 50 ms.
- All pre-existing scenarios green (clean-HEAD baseline, HEAD+wakeups, and
  full-chain runs).

## Rosetta caveat (verified, honest)

`os_sync_*` are libSystem syscall wrappers present in the x86_64 slice;
test_feed exercises BOTH directions across the Rosetta↔native boundary with
SHARED and passed with µs-class latencies — no fallback warning fired. If a
future OS regresses this, the helper logs `[d9mt_futex]` once and degrades
to bounded usleep; swap the one function body if it ever becomes permanent.

## Rollback

Single revert of this commit. The ring ABI change (`space_waiters`) is
inside padding — offsets/size unchanged — and the shm files are recreated
per session, so no cross-version state survives a rollback.
