# 00 — System Overview

d9mt v2: a **Direct3D9 → Metal 4** translation layer running Windows D3D9 games
(primary title: GTA IV — fully playable) under CrossOver/Wine on Apple Silicon.
Ships as a Wine PE builtin override `d3d9.dll` plus a forked `winemetal`
unixlib bridge. **METAL 4 ONLY** — no Metal 3, no fallback
(docs/SOURCE_OF_TRUTH.md §2 is the hard rule; that file wins on any conflict).

## Process picture

```
┌──────────────────── wine process (ALL x86_64 under Rosetta) ────────────────────┐
│  game.exe (x86 PE)                                                              │
│    └─ d3d9.dll            ← OUR DRIVER (i686 mingw PE; driver/)                 │
│         app threads: D3D9 API → EmitCs closures → CS chunk queue                │
│         dxvk-cs thread:  replays closures → MtlContext seam → MtlContextImpl    │
│           └─ winemetal.dll (PE thunks, bridge/winemetal_thunks.c)               │
│                └─ __wine_unix_call ─────────────┐                               │
│    wine unix side: winemetal.so (bridge/unix/)  ▼                               │
│         dispatch table → ObjC Metal 4 calls → Metal.framework → GPU             │
│         also: spawns d9mt-serverd, maps the wire ring into a shm file           │
└─────────────────────────────────────────────────────────────────────────────────┘
        │  SPSC ring in $TMPDIR/d9mt-<pid>/ (mmap MAP_SHARED)
        ▼
   d9mt-serverd (server/) — separate NATIVE arm64 process, the only Rosetta
   escape (docs/RENDER_SERVER_SPEC.md). S1: consumes/validates/counts records.
```

Key measured fact (2026-07-17, docs/NATIVE_BACKEND_PLAN.md header): the ENTIRE
wine process — wineserver, ntdll.so, winemetal.so, even Metal.framework code
executing in-process — runs x86_64-translated. An in-process "native unix side"
does not exist; only a separate arm64 process escapes Rosetta.

## The three layers

1. **d3d9 state tracker** (`driver/d3d9/`, `driver/cs/`) — extracted from DXVK
   3.0, Vulkan stripped, API-neutral. Handles the whole D3D9 surface: draws,
   Lock/Unlock, pools, Reset, state blocks, queries. → [10-frontend.md](10-frontend.md)
2. **AIR shader emitter** (`air/`, driven from `driver/d3d9/d3d9_shader_compiler.cpp`)
   — DXSO (SM1-3) → dxbc-spirv IR → LLVM 15 AIR bitcode → metallib. No SPIR-V,
   no MSL, no Apple tooling at runtime. → [20-shaders.md](20-shaders.md)
3. **Metal 4 backend** (`driver/backend/`) — turns the seam's command stream
   into MTL4 encoders/argument tables/residency sets. → [30-backend.md](30-backend.md)

Cross-cutting: the PE↔unix **bridge** ([40-bridge.md](40-bridge.md)), resource
lifetime ([50-resources.md](50-resources.md)), present
([60-present.md](60-present.md)), the render server
([70-render-server.md](70-render-server.md)), debugging history
([90-history.md](90-history.md)).

## Repo map (v2/ — a NESTED git repo; run all git INSIDE v2/)

| Path | What |
|---|---|
| `docs/SOURCE_OF_TRUTH.md` | THE authoritative plan doc. §2 = Metal-4-only rule. §4/4b/4c "current state" sections are stale (2026-07-11 bring-up era). |
| `docs/NATIVE_BACKEND_PLAN.md` | In-process native plan — **premise falsified**, phases 2-3 cancelled; wire transport survived. |
| `docs/RENDER_SERVER_SPEC.md` | Cross-process arm64 render server spec (current native effort). |
| `docs/PERF_ROADMAP.md`, `docs/METAL4_PERF_RESEARCH.md` | Perf levers + Metal 4 research. |
| `driver/d3d9/` | DXVK-3.0-derived D3D9 frontend (state tracker). |
| `driver/cs/` | `dxvk_cs.{h,cpp}` — the CS chunk/thread machinery, byte-preserved from DXVK. |
| `driver/mtl/` | The frontend↔backend seam: `mtl_context.h` (~40-method contract), `mtl_device.h`, `mtl_resource.h`, `mtl_presenter.h`, stubs, `mtl_wire.h` (wire ABI), `mtl_format.*`. |
| `driver/backend/` | The real Metal 4 backend: `mtl_context_impl.h` (+ `mtl_context_record.cpp` / `mtl_context_transfer.cpp` split), `mtl_device_impl.*`, `mtl_presenter_impl.*`, `mtl_pso_cache.*`, `mtl_residency.*`, `mtl_submission.*`, `mtl_ring.*`, `mtl_cmd_chunk.h`, `mtl_profiler.h`, `mtl_wire_context.*`, `wmt_handle.h`, `shaders/` (meta-blit MSL). |
| `driver/dxvk/` | `dxvk_allocator.*` (kept memory allocator). |
| `driver/util/`, `driver/wsi/` | DXVK util + win32-only WSI (monitor/window; Vulkan surface path removed). |
| `driver/EXTRACTION-LOG.md` | Every deviation from upstream DXVK during extraction. |
| `air/` | `air_builder.{h,cpp}` (IR→AIR), `air_metallib.*` (metallib container writer), tests. |
| `bridge/` | winemetal fork: `winemetal_thunks.{h,c}` (PE side), `unix/winemetal_unix.c` (unix side, ~7k lines), `d9mtair_thunks.*` (AIR-compile thunks), `UPSTREAM` (pinned DXMT ref). LGPL — separate dll/so, driver links dynamically. |
| `server/` | `d9mt-serverd`: `main.m`, `consumer.m`, `iosurface_pool.m`, `d9mt_status.h` (status ABI), `test_feed.c` (x86_64 test gate), `HANDOFF-S1-SERVER.md`. |
| `scripts/` | All build/run entry points (below). |
| `third_party/dxbc-spirv`, `DXVK-3.0-uptsream/`, `toolchains/` | Gitignored vendored deps; `scripts/fetch-deps.sh` + `build-llvm-unixside.sh` reproduce. |

## Build entry points

- **`scripts/build-driver.sh`** → `build/driver/d3d9.dll` (i686-w64-mingw32-g++,
  `-O2 -fno-strict-aliasing -g`). Also: regenerates the FF ubershader metallib
  header (`d3d9_ff_metallib.h`, needs glslang+spirv-cross+xcrun) and the
  meta-blit metallib header when their sources are newer; runs the Vulkan grep
  gate (`build-driver.vulkan-whitelist`); link failure is fatal; tail-calls
  `build-server.sh` (non-fatal).
  **GOTCHA (memorize): the script has NO header dependency tracking — it
  recompiles a .cpp only when the .cpp mtime changed. After editing ANY .h,
  `rm -rf build/driver/obj` or you ship mixed struct layouts and get
  "impossible" segfaults.**
- **`scripts/build-bridge.sh`** → `build/bridge/winemetal.so` (x86_64 Mach-O,
  links the d9mtair AIR compile core + LLVM against CrossOver's
  `x86_64-unix/ntdll.so`) + `winemetal64.dll` + `winemetal32.dll` (PE), then
  runs `bridge/tests/bridge_smoke.cpp` (real MTL4 render pass).
- **`scripts/install-bridge-builtins.sh`** — **SYSTEM-WIDE effect**: copies the
  built winemetal into `/Applications/CrossOver.app/.../lib/wine/{i386-windows,
  x86_64-windows,x86_64-unix}/` (originals backed up as `*.cx-orig`; `--restore`
  reverts). Needs sudo or App Management permission. Required once after every
  bridge change or the installed CrossOver keeps running the stale bridge.
- **`scripts/build-server.sh`** → `build/server/d9mt-serverd` (clang
  `-arch arm64 -O2`, direct Metal/QuartzCore/IOSurface — NO bridge, NO thunks)
  + `build/server/test_feed` (x86_64 on purpose: every test run crosses the
  same Rosetta↔native boundary as the real client). Builds AND runs the gate.
- **`scripts/build-d9mtair.sh`**, **`build-llvm-unixside.sh`** (LLVM 15.0.7
  x86_64 → `toolchains/llvm-x86_64`) — the AIR compile core the bridge links.
- Run/verify: `run-test.sh <exe>`, `run-air-diff.sh` (46 differential shader
  cases, must stay green), `run-airlib-loopback.sh` (42), `run-ladder.sh`,
  `run-gta4.sh` (arg passthrough broken; use `../play-gta4-d9mt.sh` which wires
  the x87 sidecar), `run-apitrace.sh`, `audit-port-diff.sh` (ported d3d9 files
  must stay diffable vs upstream DXVK).

## Environment variable inventory (all read-once at first use)

Windows-side vars reach the game ONLY via the bottle's `cxbottle.conf`
`[EnvironmentVariables]` — shell `export` does NOT propagate through CrossOver.

| Var | Where read | Effect |
|---|---|---|
| `D9MT_PROFILE` | `backend/mtl_profiler.h` | Per-frame CPU profiler; one `PROF` line every 256 frames (see 30-backend.md for field decode). Dormant otherwise. Also gates the wire-stats suffix. |
| `D9MT_PROFILE_INSTRUMENT` | `backend/mtl_instrument.cpp` | Per-function `-finstrument-functions` profiler; needs a `PROFILE_INSTRUMENT=1` build. **Known to inflate/lie under Rosetta — use for ranking only, never absolutes** (see 90-history.md, AB-swap saga). |
| `D9MT_DEFERRED=1` | `backend/mtl_context_impl.h` `deferredPassesEnabled()` | OPT-IN frame-deferred pass encoding (decision 10). Default OFF: measured hard fps regression (serializes CPU/GPU). |
| `D9MT_NO_RULE_B` / `_C` / `_E` / `_F`, `D9MT_NO_PASSMERGE` | `mtl_context_record.cpp` | Disable individual deferred-flush load/store rules / pass merging (only matter under `D9MT_DEFERRED`). |
| `D9MT_NO_PSOMRU` | `mtl_context_transfer.cpp` | Disable the 4-entry PSO snapshot MRU. |
| `D9MT_NOPSOCACHE` | `mtl_pso_cache.cpp` | Bypass PSO memoization (diagnostic). |
| `D9MT_NO_AB` | via `env::getEnvVar`, shader compiler | Revert argument-buffer binding model to classic per-slot. |
| `D9MT_METALFX=1` | `mtl_context_transfer.cpp` | MetalFX spatial upscale on the scaled present path. |
| `D9MT_RTSKIP=1` | `mtl_context_record.cpp` | Same-target pass-keep-open hack. Default OFF — wedges after display-mode Reset; superseded by deferred pass merge. |
| `D9MT_NOCOLORCLEAR` | `mtl_context_record.cpp` | Diagnostic: drop color clears. |
| `D9MT_GPUCAPTURE=trigger`, `D9MT_GPUCAPTURE_COMMITS=N` | `mtl_context_record.cpp` | GPU capture: with `MTL_CAPTURE_ENABLED=1`, `touch <bottle>/drive_c/d9mt-test/GPUCAP` → `/tmp/d9mt.gputrace` (N commits). Analyze with `gpudebug` CLI. |
| `D9MT_TRACE` | `util/log/log.h` | Extra trace logging. |
| `D9MT_WSILOG` | `bridge/unix/winemetal_unix.c` | Unix-side window/layer/present tracing. |
| `D9MT_SERVERD_PATH` | `bridge/unix/winemetal_unix.c` | Render-server binary override (else: next to winemetal.so, else `/usr/local/lib/d9mt/d9mt-serverd`). |
| `DXVK_LOG_LEVEL`, `DXVK_LOG_PATH` | `util_env` | Driver log (`<exe>_d3d9.log` in launch CWD; `debug` level for bring-up). |
| `DXVK_CONFIG`, `DXVK_CONFIG_FILE`, `DXVK_SHADER_DUMP_PATH`, `DXVK_FORCE_WINDOWED`, `DXVK_WSI_DRIVER` | inherited DXVK knobs | As upstream. |

Note the transport/server has **no** enable flag by policy: each phase is
default branch behavior; a broken phase is reverted via git
(NATIVE_BACKEND_PLAN invariants).

## Non-negotiable invariants

- **METAL 4 ONLY** — every backend call is MTL4 (queue, allocator, cmdbuf,
  argument tables, MTL4Compiler, residency sets, queue-timeline present).
- **Game-agnostic driver** — no per-game flags or names in code; fix root
  causes in the shared path.
- **32-bit PE rule** — CPU-visible buffer memory is `VirtualAlloc`'d PE-side,
  wrapped unix-side via `newBufferWithBytesNoCopy`; unix heap pointers are
  unreachable from the 32-bit game.
- **Wire/status ABI** — fixed-width fields, explicit padding,
  `static_assert(sizeof)` compiled on BOTH sides.
- **Full-clean rebuild before any perf number** (`rm -rf build/driver/obj`).
