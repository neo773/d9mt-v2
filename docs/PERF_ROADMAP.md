# d9mt v2 — Performance Roadmap (toward 90fps stable)

Living plan for closing the GTA IV daylight gap. Predecessor driver (v1, Metal 3)
hit 80-90fps; v2 (Metal 4) started at ~20fps daylight and is climbing. This tracks
what's done, what's next, and — critically — *why* each lever matters, grounded in
measured profiler data (not guesses).

## Where we are

| milestone | commit | effect |
|---|---|---|
| command-stream batching | d554c2b | per-draw encoder ops → 1 encodeCommands/pass. 20→50-60fps |
| arg-table dirty-tracking | 8744a88 | skip redundant slot *writes* (output side) |
| per-function profiler | 452ce2d | `PROFILE_INSTRUMENT=1` — found the real hotspots |
| per-pass rotation + per-slot bitmasks | 353ebf9 | stop *scanning* unchanged slots. appendTable 97k→~2k/frame (39-74x); steadier 60, instrumented driving 6→30fps |

**Current bottleneck (measured):** everything runs on ONE `dxvk-cs` thread doing
~20ms/frame of work. Process sample: **~45% of the process is other threads BLOCKED
on that CS thread** (msync spin), **~38% Rosetta translation tax**, Metal bridge <1%.
So 90fps = getting that single thread from ~20ms → ~11ms/frame. The cost is our
Rosetta-translated per-draw C++ — NOT Metal, NOT the GPU.

## Post-bitmask hotspots (per-function profiler, per frame)

After 353ebf9 the append cluster collapsed. Remaining top self-time:
- `flushResources` — still #1 (partly instrumentation artifact: the BitMask iterators
  inline to nothing in the normal build; real residual is the per-draw writes below)
- `appendTableAddress` ~6,662/f + `WMTMemoryPointer::set` ~17,751/f — the per-draw
  **CBV / push-constant** address updates (genuinely change most draws → harder to skip)
- `flushVertexBuffers`, `materializePass` (~1 call/draw each), `flushCmdChunk` (the
  PE→unix crossing, ~43/frame @ 17µs)

## Ranked next levers

### 1. CBV / push-constant dirty-tracking (cheap, ~next few fps)
The bitmask skipped unchanged texture/sampler slots; do the same one level over for
**constant buffers**. `appendTableAddress` (6,662/f) + `WMTMemoryPointer::set` (17,751/f)
are the per-draw CBV/push address writes. Dirty-track CBV bindings so a draw whose
constant-buffer slice+offset is unchanged skips the re-bind. Also investigate
`WMTMemoryPointer::set` — 17,751 calls/frame is high; if it's a per-append wrapper, see
if the wmtcmd structs can be filled without it. Profiler-guided, same pattern as bitmask.

### 2. Read v1's per-draw binding path — the "why was v1 faster" question (HIGH INFO)
v1 hit 80-90 on *Metal 3* using tier-2 **argument BUFFERS**: it wrote resource IDs into
a shared buffer as **plain CPU memory writes (memcpy)** — *zero* per-draw API calls.
Metal 4 argument *tables* require per-slot API calls (setTexture/setAddress), even
batched through our command stream. This is likely the fundamental per-draw cost
difference. ACTION: read `d9mt/src/d3d9fe/d9mt_context.cpp` + `d9mt_backend.h` +
`d9mt_draw.h` — find v1's exact per-draw bind path. If a leaner Metal 4 technique exists
(e.g. writing argument-buffer memory directly vs per-slot API calls), that's a
*structural* win, not a trim. This decides whether 90 is a trim-fest or a technique-swap.

### 3. Reduce per-draw overhead in flushResources/materializePass
`materializePass` ~1 call/draw and `flushVertexBuffers` ~1/draw — check what runs per
draw that should run per pass. Hoist `pair.table[st].handle()` / WmtRef resolution out
of slot loops (profiler flagged). Micro-opts under the Rosetta tax — each instruction
cut is doubly valuable (x86-emulated).

### 4. Multi-threaded encoding — LOWER priority (may not be feasible for D3D9)
Research Finding 2 (parallel encoding w/ per-thread MTL4CommandAllocator) works for
D3D12 (independent command lists) but D3D9 is **immediate-mode sequential** — one ordered
command stream, hard to split across threads without breaking submission order. NOT the
easy win it is for D3D12. Scope only if 1-3 stall out. Would require splitting by render
pass or independent work the D3D9 model doesn't cleanly expose.

### 5. Free wins that follow
- The 45% msync spin is the app/render thread waiting on the CS thread — as the CS thread
  gets faster (levers 1-3), the spin drops automatically. They're coupled; no separate work.
- Async PSO / binary archives (METAL4_PERF_RESEARCH.md #4/#5): psoMiss ≈ 0 in GTA, so
  IRRELEVANT for this title. Keep for shader-heavy titles that hitch on first-encounter.

## Verification debt (do before/with next perf work)
The per-pass rotation + bitmask stack (353ebf9) is verified **by gameplay only** (busy
multi-draw scenes render with correct per-object textures — user-confirmed). A bounded
GPU trace confirming several consecutive gbuffer draws have correct DISTINCT bound
textures is the outstanding formal gate. Fold into the next agent as cheap insurance.

## Method notes
- Profiler: `PROFILE_INSTRUMENT=1 ./scripts/build-driver.sh` + env `D9MT_PROFILE_INSTRUMENT=1`;
  dump is raw `D9MTINST F <offset> <calls> <self_ns> <incl_ns>`; symbolicate offline with
  `i686-w64-mingw32-nm build/driver/d3d9.dll` + the report script (scratchpad
  `d9mtinst_report.py <log> <syms> <ImageBase-hex>`). ImageBase via PE header (~0x6b2c0000).
  REGENERATE syms from the CURRENT dll each run — offsets are build-specific.
- Instrumentation inflates absolute times (esp. inlined iterators under Rosetta). Read
  RELATIVE ranking + call counts, never absolute frame time. Normal-build high-level
  D9MT_PROFILE (ms/frame per bucket) or the user's eyes give the true fps.
- High-level profiler: `D9MT_PROFILE=1`; PROF lines every 256 frames (read with python,
  not grep — wide lines truncate).

See METAL4_PERF_RESEARCH.md (Apple-sourced techniques) and the d3dmetal4-audit memory
(Apple's own D3DMetal batches D3D12 lists across the wine boundary).

## Second test title — Sonic Adventure 2 (BLOCKED, game-side)
SA2 (Steam appid 213610) installed in the "DirectX 9 games" bottle (SteamCMD
Windows-platform pull; Steam fresh-installed + logged in there). Launched through
d9mt: **deterministic crash at sonic2app+0x1c13 (`fildl 4(%eax)`, EAX=0 — null-ptr
FP load) in the game's OWN startup, BEFORE d3d9 loads** (loaddll trace: 0 d3d9 loads).
NOT a driver bug — our d3d9/winemetal never enter the backtrace. This is the
notorious SA2 2012-port startup crash; needs game-side fixes (config/registry/compat
or the SA2 mod loader), a separate bring-up project. x87 sidecar not required per user.
GTA IV remains the working D3D9 test title. Revisit SA2 only if a second title is wanted.
