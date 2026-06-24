# Metal GPU Capture for the v2 D3D9→Metal driver

How to capture a real `.gputrace` of what the driver emits to Metal — buffers,
textures, shaders, draw calls, the rendered output — and inspect it with
`gpudebug` / Xcode Metal Debugger. This is the GPU-level view that printf and
buffer tracing cannot give.

## TL;DR — capture a frame

```sh
bash v2/scripts/run-gpucapture.sh code_alpha.exe /tmp/alpha.gputrace
```

Produces a ~10 MB `/tmp/alpha.gputrace`. Then inspect:

```sh
gpudebug -q -t /tmp/alpha.gputrace list          # opens a session, prints id
gpudebug -s <id> -c "go api_calls"               # the Metal API call stream
gpudebug -s <id> -c "go /commands/cb0/re0"       # render encoder + attachments
gpudebug -s <id> -c "fetch @tex2"                # dump the rendered drawable to PNG
```

Or open the bundle in Xcode's Metal Debugger.

## Why this was hard (the two-hour ordeal)

The driver runs as a Windows `d3d9.dll` inside CrossOver (wine) under Rosetta,
talking to Metal through DXMT's `winemetal` unixlib. Every "normal" GPU-capture
path is blocked by something in that stack:

### Dead end 1 — external `gpucapture` attach (Rosetta wall)
`/usr/bin/gpucapture` (macOS 27) lists the wine process but always tags it
`[non-debuggable]`. The fix for that is the `com.apple.security.get-task-allow`
entitlement on the target. We re-signed `wineloader`, `wineserver`, the
`CrossOver-Hosted Application/wineloader`, and ~49 wine binaries with it. Still
`[non-debuggable]`: the wine processes are **x86_64 under Rosetta**, and the
entitlement isn't honored for GPU-tools attach on translated processes.

> Side damage: re-signing the `.so` files with entitlements (entitlements belong
> on executables, not dylibs) and stripping wine's own entitlements briefly
> looked like it broke rendering. It didn't — the breakage was a phantom from a
> flaky `osascript` window-focus producing blank screenshots. Restoring wine's
> real entitlements (`allow-jit`, `disable-library-validation`, …) on the
> executables and signing the `.so` plain ad-hoc is the safe form if you must.

### Dead end 2 — apitrace forwarding to our driver
apitrace's standalone d3d9 proxy DLL **does** trace D3D9 calls in the bottle and
`apitrace dump` decodes them. But forwarding the session *through our driver*
returns `Direct3DCreate9 = NULL` — our driver won't `LoadLibrary` via apitrace's
forward under wine's DLL resolution (tried system32, `APITRACE_FORCE_MODULE_PATH`,
the `d9mtmetal=b` override). Also: apitrace traces the **D3D9 API** (the app's
input calls), not the **Metal** commands — so it can't show where our Metal
translation goes wrong anyway. Harness kept as `v2/scripts/run-apitrace.sh` for
replay/repro use, but it is not the GPU-level tool.

### What actually worked — programmatic capture via d9mtmetal
DXMT's `d9mtmetal` unixlib already has `D9MT_FUNC_CAPTURE` calling
`MTLCaptureManager`. The driver triggers it (file `C:\d9mt-test\DO_CAPTURE` ->
`maybeBeginGpuCapture`). Three fixes turned this from silently-failing into a
real `.gputrace`:

1. **Output path must be a unix path.** The unix side builds the URL with
   `fileURLWithPath`, so a Windows `C:\…\frame.gputrace` made `startCapture`
   fail silently. Use `/tmp/d9mt-frame.gputrace`.
2. **`MTL_CAPTURE_ENABLED=1` must reach the *unix* wine process.** CrossOver
   strips it from the shell env, but propagates `cxbottle.conf`
   `[EnvironmentVariables]` (same channel as `DYLD_FALLBACK_LIBRARY_PATH`). It
   hangs rendering at the 3-drawable pool, so `run-gpucapture.sh` adds it only
   for the capture run and removes it after.
3. **Trigger before the first frame.** With capture enabled the app deadlocks
   after ~3 frames (CAMetalLayer drawable pool, capture holds drawables). One
   frame is enough — pre-create `DO_CAPTURE` so frame 1 is captured and the
   `.gputrace` is flushed before the hang.

## Still-open follow-ups
- The 3-frame drawable-pool hang under `MTL_CAPTURE_ENABLED` limits captures to a
  single frame. Investigating winemetal's drawable lifecycle would allow
  multi-frame capture. `winemetal` source: `dxmt/src/winemetal/unix/winemetal_unix.c`.
- External `gpucapture` attach would need native arm64 wine (not Rosetta).
