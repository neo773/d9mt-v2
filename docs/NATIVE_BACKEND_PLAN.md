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

- **The seam is the `MtlContext` virtual interface** (~40 calls, POD-friendly).
  `MtlWireContext` implements it PE-side by serializing; the real
  `MtlContextImpl` runs unix-side.
- **Ring**: single-producer/single-consumer, PE-allocated (unix side already
  dereferences PE memory — wmtcmd precedent), acquire/release atomics
  (Rosetta preserves x86 ordering semantics). Fixed-width types ONLY +
  static_asserts on BOTH compilations (mingw x86-64 / clang arm64).
- **Resources**: backend objects live unix-side; PE holds u64 handles via a
  handle table. Frontend-visible metadata (extent/format/info, mapPtr for
  shared-storage buffers) is mirrored into a PE-readable POD block at create
  time. mapPtr works cross-side (same VA) — zero-copy Lock/upload preserved.
- **Releases** ride the ring (ordering free). **Readbacks/waits**: consumer
  publishes a progress counter + completed submission serial to shared
  memory; PE waits on those (same semantics as today's serials).
- **Native thread rules**: never call into PE code or wine internals; pure
  Metal + memory; owns its own autorelease pools; parks on its own condvar.

## Phases (each independently shippable, env-gated)

### Phase 0 — foundations (no behavior change)
- `driver/mtl/mtl_wire.h`: one `#[repr(C)]`-style POD record per MtlContext
  call + opcodes + static_asserts. Shared by driver and bridge.
- Ring header: layout, atomics protocol, kick/park contract, progress counter.
- Handle-table design note (create/destroy records, id recycling by serial).

### Phase 1 — transport proof (echo mode, D9MT_WIRE=1)
- PE: `MtlWireContext` WRAPS the real context: executes locally (unchanged
  behavior) AND serializes every call into the ring.
- Unix: consumer thread decodes, validates (opcode bounds, handle liveness),
  counts, publishes progress; discards. Proves ring/atomics/kick-park under
  a real game at full draw rate with zero risk.
- Gate: GTA plays normally with D9MT_WIRE=1; consumer stats (records/frame,
  ring stalls) in the PROF line.

### Phase 2 — backend compiles native
- `MtlContextImpl` + deps (pso cache, upload ring, residency, submission,
  cmd chunk→direct calls) compile arm64 into winemetal.so. Thunk calls become
  direct ObjC; wmtcmd replay disappears on this side. Keep the PE build too
  (same sources, two targets) until cutover.
- Resource creation moves behind wire records returning handles; PE-side
  MtlTexture/MtlBuffer become handle wrappers carrying mirrored info.
- This is the heavy phase. Sub-gates per resource type.

### Phase 3 — cutover (D9MT_NATIVE=1)
- Echo mode drops the local execution: PE records only, native consumer
  executes. Present handshake, GPU capture hook, backbuffer dump, queries
  all verified native-side. Bisect ladder: D9MT_NATIVE=0 reverts wholesale.

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
- Every phase behind an env flag; main always ships last-verified behavior.
- Struct ABI: fixed-width fields, explicit padding, static_assert(sizeof)
  compiled on both sides.
- No native-thread callbacks into PE; no win32 from the consumer.
