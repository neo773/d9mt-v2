# d9mt v2 — SOURCE OF TRUTH

This is the ONLY authoritative document. All prior docs/memories were deleted
(2026-07-11) because a retired agent's stale "Metal 3 first" plan caused drift.
If anything you read elsewhere contradicts this file, this file wins. Update
THIS file when the plan changes; do not create parallel planning docs.

## 1. What we are building

A Direct3D9 → Metal driver for running Windows D3D9 games (primary target:
GTA IV) on macOS under CrossOver/Wine. Ships as `d3d9.dll` (a Wine PE builtin
override) plus a `winemetal` bridge (Wine unixlib) that reaches Metal.

Three layers, bottom to top:
1. **d3d9 state tracker** — extracted from upstream DXVK 3.0, Vulkan stripped.
   Handles the entire D3D9 API surface (draws, Lock/Unlock, pools, Reset,
   state blocks, resources). API-neutral; Metal-agnostic. KEEP.
2. **AIR shader emitter** — our own dxbc-spirv IR → Apple AIR (LLVM 15
   bitcode) → metallib. NO SPIR-V, NO SPIRV-Cross, NO MSL, NO Apple tooling
   at runtime. Lives in `v2/air`. KEEP.
3. **Metal backend** — turns the state tracker's command stream into Metal
   work. THIS is what talks to the GPU. **METAL 4 ONLY (see §2).**

## 2. THE HARD RULE: METAL 4 ONLY

**No Metal 3. No Metal 3 fallback. No "ship Metal 3, swap later."** The user
was explicit and emphatic. Everything the backend does uses the Metal 4 API:

| Concept | Metal 4 API we use |
|---|---|
| Queue | `MTL4CommandQueue` (`newMTL4CommandQueue`) |
| Command memory | `MTL4CommandAllocator` (per-frame, `reset`) |
| Command buffer | `MTL4CommandBuffer` (device-created; `beginCommandBuffer`/`endCommandBuffer`; batched `commit`) |
| Resource binding | `MTL4ArgumentTable` — `setAddress`(gpuAddress) / `setTexture`(gpuResourceID) / `setSamplerState`. **Replaces ALL per-slot binding.** `maxBufferBindCount >= 21` (we use `[[buffer(20)]]` for push data) |
| Render encode | `MTL4RenderCommandEncoder` + `setArgumentTable:atStages:` |
| Compute/blit | `MTL4ComputeCommandEncoder` — ALL copies/blits/mips (no blit encoder in MTL4) |
| Pipelines | `MTL4Compiler` + `MTL4RenderPipelineDescriptor` + `MTL4LibraryFunctionDescriptor`. Depth/stencil format NOT in the descriptor. Async compile. |
| Residency | `MTLResidencySet` attached to queue/cmdbuf (replaces `useResource`/`useHeap`) |
| Present | `queue waitForDrawable` → `commit` → `queue signalDrawable` → `drawable present` |
| Completion | `MTL4CommitOptions` feedback (no `addCompletedHandler`) |
| Sync | `MTLEvent` (GPU↔GPU), `MTLSharedEvent` (CPU involved), fences same-queue only |

`set*Bytes` is GONE in MTL4 → per-draw constants/push data go through a
transient buffer (our `UploadRing`), whose `gpuAddress` is bound via the
argument table. Host is macOS 26 (Darwin 27) → MTL4 is available.

Reference: the `game-porting-skills:translating-to-metal4-api` skill is the
API bible. Load it when touching backend Metal code.

## 3. The bridge (winemetal) — DXMT fork, additive only

The ONLY path from the PE `d3d9.dll` to Metal is the `winemetal` unixlib
(a fork of DXMT's, in `v2/bridge/`). Policy:
- DXMT is the gold standard for the Wine↔Metal seam. We do not reinvent it.
- Our additions are additive patches in DXMT's exact style (new `WMT*` structs,
  `WINEMETAL_API` entry, unix-side thunk, wow64 twin, appended dispatch
  indices — never renumber existing). Formatted so `diff` vs upstream DXMT
  stays a clean cherry-pickable patch. Upstream pinned in `v2/bridge/UPSTREAM`.
- **BRIDGE MTL4 SURFACE: DONE (2026-07-11, commit b0f0c37).** 44 additive
  entries (dispatch codes 157-200): command queue/allocator/buffer, argument
  table, render + compute encoders, MTL4Compiler + pipeline, residency sets,
  drawable present. The bridge smoke test drives a real MTL4 render pass — a
  PSO compiled from OUR AIR vs/ps libs, one draw, commit, queue-signaled event
  completion — GPU-confirmed. **The entire Metal 4 path works with our shaders.**
  SDK notes: blend enable = `MTL4BlendState` enum (not BOOL), a2c =
  `MTL4AlphaToCoverageState`; functions via `MTL4LibraryFunctionDescriptor`;
  compiler = `newRenderPipelineStateWithDescriptor:compilerTaskOptions:error:`;
  `endEncoding` (code 33) ends MTL4 encoders too; queue commit = `commit:count:`.
  A VS writing `[[point_size]]` needs Unspecified topology (not Triangle).
- AIR compilation also rides the bridge: `d9mtair` thunks (codes 154-156) call
  our AirBuilder+LLVM compile core on the unix side. Already working.
- LGPL: the bridge stays its own dll/so; our MIT driver links it dynamically.

## 4. Current state (2026-07-11)

**DONE and working:**
- AIR emitter: full SM3 op coverage, differential-tested pixel-identical to
  Apple's own MSL compiler (46/46, `scripts/run-air-diff.sh`). Push data,
  bitfield ops, textures, control flow, spec constants — all in.
- `d9mtair` unixlib: AIR compile across the real Wine PE↔unix boundary, 42/42
  loopback (`scripts/run-airlib-loopback.sh`).
- d3d9 state tracker: fully extracted from DXVK 3.0, `d3d9.dll` LINKS (20/20
  exports), zero Vulkan (grep-gated). `scripts/build-driver.sh`.
- Live bring-up: `code_shader.exe` under Wine reaches device → context →
  swapchain → CAMetalLayer window → **VS+PS AIR compile succeed**. The whole
  frontend + shader pipeline runs.

**IN PROGRESS — the Metal 4 backend:**
- The current backend (`v2/driver/backend/mtl_context_impl*`, `mtl_device_impl`)
  was written against Metal-3 winemetal calls (`wmtcmd` per-slot stream,
  `MTLCommandQueue`). **This is being REPLACED with Metal 4**, per §2.
- Blocking first pixels: the backend must issue MTL4 draws through an argument
  table + MTL4 encoder. Once the bridge exposes MTL4 and the backend uses it,
  `code_shader.exe` should draw its triangle.

**NOT STARTED:** fixed-function pipeline (LOW priority — GTA IV is fully
programmable-shader; FF is conformance-tail only, do last), the test-exe
ladder, GTA IV bring-up.

## 4b. LIVE STATE (2026-07-11): Metal 4 backend RUNS; two render bugs

`code_shader.exe` runs the full Metal 4 pipeline with no crash: device →
context → swapchain → CAMetalLayer window → VS+PS AIR compile → MTL4Compiler
PSO → MTL4 draw → present. Fixed on-device: d9mtair returns metallib into
caller-provided PE buffers (32-bit PE can't read unix pointers); AirBuilder
eU8/eI8 scalar types (D3DCOLOR unpack); PSO topology Unspecified (D3D9 VS
always writes point_size). TWO REMAINING RENDER BUGS:

1. **CBV slot-mapping (correctness — blocks the triangle).** The backend
   binds constant buffers to Metal argument-table slot = the frontend's raw
   `CbvIndex` (d3d9_shader.h: VSClipPlanes=0, VSFixedFunction=1, …,
   VSStaticConstants=3=the cF/MVP float constants, PSShared=5). But the AIR
   shader reads them at DIFFERENT Metal buffer indices (air_builder slot
   assignment: eLegacyConstFloat/cF → buffer(0), other legacy builtins →
   1,2,…, DclCbv → (haveBuiltins?8:0)+regIndex; vertex streams →
   kVertexBufferBaseIndex=3+; push → 20). So cF (MVP) is bound at slot 3 but
   the VS reads buffer(0) → wrong/zero MVP → no visible triangle; worse, a CBV
   at slot 3 COLLIDES with vertex stream 0 (buffer 3). FIX: the backend must
   bind each D3D9 constant buffer to the Metal slot the AIR shader actually
   declares — use the d9mtair REFLECTION blob (buffer bindings carry index+
   name; currently requested but unread in d3d9_shader_compiler getMetallib)
   to build a per-shader CbvIndex→AIR-slot table, and thread it to
   MtlContext binding. Guard currently skips CBVs with invalid storage
   (mtl_context_impl2 flushResources) — that's a band-aid; the mapping is the
   real fix. A fragment CBV (st=1 slot=0) was also binding a stale/garbage
   buffer (size=29, storage=0x6) — likely a lifetime issue on a PS constant
   buffer never Alloc'd; the reflection-driven binding (only bind CBVs the
   shader declares) should also resolve it.

2. **Pixel verification tooling.** screencapture + osascript window bounds is
   unreliable (wine window on another Space / minimized / bounds query empty).
   NEED a driver-side backbuffer readback: gated by a trigger file, after the
   app's draws copy the backbuffer MtlTexture → a Read-storage buffer (MTL4
   compute-encoder copyTextureToBuffer), flush+wait, write raw BGRA to
   C:/d9mt-test/rt_dump.bin; a host script reads it + checks pixels. This is
   the reliable autonomous verifier for the whole test ladder AND GTA.

## 4c. THE LAST BUG (2026-07-11): missing Metal 4 barriers

Verified live for code_shader: MVP is identity at the correct AIR slot (11),
tint white (14), vertex stream bound (stream 0, stride 16, valid gpuAddress,
2 attrs) — everything correct — yet 0 covered fragments (rt_dump = pure clear
color). ROOT CAUSE: **Metal 4 resources are UNTRACKED and the backend emits
ZERO barriers.** The DEFAULT-pool vertex buffer is Private; its data is
uploaded at Unlock via FlushBuffer -> ctx->copyBuffer (an MTL4 compute-encoder
copyFromBufferToBuffer). The later draw (render encoder) reads that VB WITHOUT
a barrier, so the GPU does not guarantee the compute write is visible -> the
VS reads stale/empty vertices -> degenerate (zero-area) triangle. Same class
affects RT->texture sampling, the present backbuffer->drawable copy, etc.

FIX: add cross-encoder Metal 4 barriers. The bridge has only within-encoder
memory barriers (WMTCompute/RenderCommandMemoryBarrier); ADD an
MTL4CommandBuffer-level barrier (barrierAfterStages:beforeQueueStages:
visibilityOptions:, per the translating-to-metal4-api / managing-metal4-
synchronization skills) as an additive bridge entry, and place it in the
backend: after buffer/texture upload copies before the consuming draw; after
a render pass that writes an RT before it is sampled; before the present copy.
Simplest-correct first (coarse queue barrier after each compute copy so
subsequent render sees writes), optimize later. Verify with the readback:
code_shader must show a non-clear-color triangle (RGB interpolated) at center.

## 5. Build & run

- Shader diff suite: `bash scripts/run-air-diff.sh` (46 cases, must stay green).
- AIR unixlib loopback: `bash scripts/run-airlib-loopback.sh` (42 cases).
- Driver: `bash scripts/build-driver.sh` (i686-mingw, links `d3d9.dll`,
  Vulkan-free grep gate, link is a fatal gate).
- Bridge: `bash scripts/build-bridge.sh` (winemetal.so + PE dlls; smoke test).
- Install bridge into CrossOver: `sudo bash scripts/install-bridge-builtins.sh`
  (needs App Management perm; `--restore` reverts). One-time per bridge change.
- Run a test exe: `bash scripts/run-test.sh code_shader`.
- LLVM (x86_64 Mach-O, for the unix AIR core): `scripts/build-llvm-unixside.sh`
  → `v2/toolchains/llvm-x86_64`. LLVM 15.0.7 exact.

**Op-coverage debug loop** (when AirBuilder rejects an op at runtime): edit
`air/air_builder.cpp` → `rm build/airlib/obj/air_air_builder.cpp.o` →
`build-d9mtair.sh` → `build-bridge.sh` → `install-bridge-builtins.sh` → run.
AirBuilder rejections log to `/tmp/d9mtair_err.log`. Decode op# via the
`OpCode` enum in `third_party/dxbc-spirv/ir/ir.h` (explicit uint16 values).

## 6. Environment facts (still valid, folded from old memories)

- **GTA IV launch**: via `../play-gta4-d9mt.sh` (parent dir), which wires
  **x87sidecar** (JIT-patches Rosetta's x87 handlers) — GTA IV is 32-bit
  x87-FPU-heavy; without it, ~25 fps under emulation; with it, ~60. Always
  RELEASE build for real fps. Game takes ~2 min first boot (shader compile),
  ~10 s subsequent to gameplay.
- **Env knobs** reach the Windows side ONLY via the bottle's `cxbottle.conf`
  `[EnvironmentVariables]` (shell `export` does NOT propagate through
  CrossOver). Bottle: `Rockstar Games Launcher`. Windows-side `fopen` accepts
  `C:/...` forward-slash paths.
- Keep `CX_GRAPHICS_BACKEND = "dxmt"` in the bottle conf or GTA IV fullscreen
  breaks into a trapped borderless window.
- Driver must be **game-agnostic** — no per-game flags, no game names in code.
  Fix root causes in the shared path (DXVK-style). Math mode fixed `.fast`.
- Deploy dir for test exes: bottle `drive_c/d9mt-test`. Driver logs to
  `<exe-name>_d3d9.log` in the launch CWD; enable with `DXVK_LOG_LEVEL=debug`
  + `DXVK_LOG_PATH` in cxbottle.conf. Launch exes with ABSOLUTE path (relative
  → winewrapper "cannot execute").
- Crash backtrace: user can paste the winedbg dump; symbolize with
  `i686-w64-mingw32-addr2line -e build/driver/d3d9.dll -f <imagebase+RVA>`
  (imagebase from `objdump -p`).
- screencapture fights Spaces/window-focus — prefer driver logs +
  `/tmp/d9mtair_err.log` as ground truth over screenshots.

## 7. Hard-won lessons (do not relearn)

- **32-bit PE rule**: CPU-visible buffer memory MUST be `VirtualAlloc`'d in the
  PE address space; the unix side wraps it via `newBufferWithBytesNoCopy`.
  Unix-allocated host pointers are unreachable from the 32-bit game process.
- **Metal texture arrayLength >= 1** even for non-array 2D (0 is rejected).
- **Verification ghosts**: if ONE test exe misbehaves while others render,
  clone/rename the test harness before blaming the driver.
- **Missing MVP/constant collapses geometry to the origin** → looks like a
  driver bug, is a test bug. Sample center pixels, not corners.
- **AirBuilder helper functions named by walk order** (not SSA id) for
  byte-determinism across `ir_serialize` round-trips.
- **VS↔PS interpolant reconciliation** (resolveMismatchedIo/resolveSemanticIo)
  runs per VS/PS pair — this is the historical black-screen class; already
  implemented in `d3d9_shader_compiler.cpp`.

## 8. Repo

Private: `github.com/neo773/d9mt-v2`, branch `main`. Vendored deps
(`third_party/dxbc-spirv`, `DXVK-3.0-uptsream/`, LLVM toolchain) are
gitignored; reproduce via `scripts/fetch-deps.sh` + `build-llvm-unixside.sh`.
Commits are gate-verified. `scripts/audit-port-diff.sh` checks ported d3d9
files stay diffable against upstream DXVK (surviving hunks must trace to a
decision here).
