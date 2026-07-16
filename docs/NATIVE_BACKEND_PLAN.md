# Native Backend — moving the CS-thread's work to ARM64

## Why (measured, 2026-07-17)

The frame ceiling is ONE `dxvk-cs` thread doing all D3D9→Metal translation as
x86 code under Rosetta (~2× per-instruction tax; ~38% of process time is
translation, ~45% is other threads blocked on the CS thread). The PE↔unix
crossings are NOT the tax (~85 batched crossings/frame ≈ 1ms, bridge <1%).
Wine's PE side is condemned to Rosetta (the game is x86); the unix side of
this process is native ARM64. We own both sides. Move the translation there.

Precedent: Apple's D3DMetal batches command lists across the wine boundary
and executes them native-side (see d3dmetal4-audit). mtld3d's encoder thread
is PE-side (pays Rosetta) — this design goes one better.

## Target architecture

```
game thread (x86/Rosetta)          native consumer thread (ARM64, in .so)
  D3D9 call                          parked on condvar
   └─ dxvk-cs closure (existing)      │
        └─ MtlWireContext:            │
           POD record → SPSC ring ────┴─ decode record
           (shared VA, zero-copy)        └─ MtlContextImpl (compiled arm64,
           empty→nonempty: one              DIRECT ObjC Metal calls — the
           unix_call kick                   whole wmtcmd/thunk layer vanishes
                                            on this side)
```

- **The seam is the HOT LOOP of the `MtlContext` virtual interface** — the
  per-draw state machine (binds, state setters, clears, draws; ~23 calls).
  `MtlWireContext` serializes those PE-side; cold calls (creates, copies,
  blits, queries, init/upload, frame lifecycle) keep their existing direct
  paths.
- **Ring**: single-producer/single-consumer, PE-allocated (unix side already
  dereferences PE memory — wmtcmd precedent), acquire/release atomics
  (Rosetta preserves x86 ordering semantics). Fixed-width types ONLY +
  static_asserts on BOTH compilations (mingw x86-64 / clang arm64).
- **Resources**: the Metal objects ALREADY live unix-side behind
  `obj_handle_t` — that is the handle system; no handle table, no ownership
  migration. Records carry the obj_handle_t plus the fields the hot loop
  consumes downstream (gpuResourceID/gpuAddress mirrors the PE wrappers
  already hold, and the POD key bits: format, extent, stride, offsets).
  No pointer-valued fields on the wire.
- **Readbacks/waits**: consumer publishes a progress counter (its tail
  cursor) + completed submission serial to shared memory; PE waits on those
  (same semantics as today's serials).
- **Native thread rules**: never call into PE code or wine internals; pure
  Metal + memory; owns its own autorelease pools; parks on its own condvar.

## Phases (each independently shippable; no env gating — the transport is
## default behavior of the branch, a broken phase is reverted via git)

### Phase 0 — foundations (no behavior change)
- `driver/mtl/mtl_wire.h`: one `#[repr(C)]`-style POD record per HOT-LOOP
  MtlContext call + opcodes + static_asserts. Shared by driver and bridge.
- Ring header: layout, atomics protocol, kick/park contract, progress counter.

### Phase 1 — transport proof (echo mode, always on)
- PE: `MtlWireContext` WRAPS the real context: executes locally (unchanged
  behavior) AND serializes every hot-loop call into the ring.
- Unix: consumer thread decodes, validates (opcode bounds, record framing),
  counts, publishes progress; discards. Proves ring/atomics/kick-park under
  a real game at full draw rate with zero risk.
- Gate: GTA plays normally; consumer stats (records/frame, ring stalls) in
  the PROF line (D9MT_PROFILE — the existing profiler gate, not a new flag).

### Phase 2 — hot loop compiles native
- Recompile the hot-loop state machine (prepareDraw / flushResources /
  resolvePso + their state) into winemetal.so, consuming these records
  natively. The Metal objects are already native behind obj_handle_t — no
  resource-ownership migration needed; thunk calls become direct ObjC on
  that side and the wmtcmd replay disappears for the hot loop. Keep the PE
  build too (same sources, two targets) until cutover.
- Sub-gates per record family (state, binds, draws).

### Phase 3 — cutover
- Echo mode drops the local execution for the hot loop: PE records only,
  native consumer executes. Present handshake, GPU capture hook, backbuffer
  dump, queries all verified native-side. Revert = git, per branch policy.

### Phase 4 — what native unlocks
- Submission overlap: consumer submits pass batches while later records
  stream in → restores CPU/GPU overlap AND enables re-defaulting the
  deferred-pass rules (see decision 10 note in mtl_context_impl.h).
- Multithreaded native encode (Metal 4 parallel encoding).
- Expected end state: CS translation 8-13ms Rosetta → ~4-6ms native,
  overlapped with the game thread. The 100-120fps path.

## Invariants that must hold at every phase
- METAL 4 ONLY (SOURCE_OF_TRUTH §2).
- Full-clean builds (`rm -rf build/driver/obj`) before any perf number.
- No env flags for the transport: each phase lands as default branch
  behavior; a broken phase is reverted via git. (The PROF wire stats stay
  behind D9MT_PROFILE — the profiler's own pre-existing gate.)
- Struct ABI: fixed-width fields, explicit padding, static_assert(sizeof)
  compiled on both sides.
- No native-thread callbacks into PE; no win32 from the consumer.
