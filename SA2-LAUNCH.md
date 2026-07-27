# Sonic Adventure 2 — launching on the d9mt v2 driver

## The recipe

Steam must be **fully down first**, otherwise `-applaunch` just hands off to the
running client and the `MTL_HUD_*` env never reaches the game.

```sh
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver

# 1. graceful shutdown — never SIGKILL (see below)
"$CX/bin/wine" --bottle "DirectX 9 games" --wait-children \
  "C:\\Program Files (x86)\\Steam\\steam.exe" -shutdown

# 2. wait until steam.exe AND steamwebhelper are gone
until ! pgrep -f "Steam/steam.exe" >/dev/null && ! ps aux | grep -q "[s]teamwebhelper"; do
  sleep 3
done

# 3. launch through Steam, HUD env on the steam.exe process (it propagates to the child)
env MTL_HUD_ENABLED=1 MTL_HUD_SHOW_VALUE_RANGE=1 \
  "$CX/bin/wine" --bottle "DirectX 9 games" --wait-children \
  "C:\\Program Files (x86)\\Steam\\steam.exe" -applaunch 213610
```

AppID **213610**. Bottle **"DirectX 9 games"**.
Game dir: `.../Bottles/DirectX 9 games/drive_c/Program Files (x86)/Steam/steamapps/common/Sonic Adventure 2`

Deliberately NOT setting `MTL_HUD_ENCODER_TIMING_ENABLED` — it conflicts with the
D3D9 timestamp-query path (same reasoning as `play-gta4-d9mt.sh:255`).

## Do NOT run sonic2app.exe directly

SA2 is Steam-DRM-wrapped. Steam launches **`launcher.exe`**, which sets up and
starts the game; `sonic2app.exe` started directly bypasses that and dies at the
DRM stub after ~6-13 s with:

```
Unhandled exception: page fault on read access to 0x00000004 in wow64 32-bit code (0x00401c13)
0x00401c13 sonic2app+0x1c13: fildl 4(%eax)      EAX:00000000
```

That backtrace is only 3 frames of game code off `BaseThreadInitThunk`, so it
looks like an early-init null deref and invites misdiagnosis. It is not a driver
bug. Confirm which path a run took via Steam's own log:

```sh
grep 213610 ".../Steam/logs/console_log.txt"
# healthy run:  Game process added : AppID 213610 "...\launcher.exe"
# broken run:   Game process added : AppID 213610 "...\sonic2app.exe"  → removed ~7s later
```

`launcher.exe` staying registered = the game is genuinely up.

## Do NOT `pkill -9` the game

SIGKILL leaves Steam's app-213610 state wedged; subsequent launches fail. Quit
normally, or shut Steam down with `-shutdown`.

## Driver state

The v2 driver is already deployed as `Sonic Adventure 2/d3d9.dll`
(byte-identical to `d9mt/v2/build/driver/d3d9.dll`). `d3d9.dll.prev-0726` is an
older d9mt build, **not** a stock baseline — to get a real baseline, move
`d3d9.dll` aside so wine's builtin d3d9 loads.

## Red herrings ruled out (2026-07-27)

- **`Config/UserConfig.cfg` `Res="30"`** — a display-mode *index*, and our
  `EnumAdapterModesEx` does return `D3DERR_INVALIDCALL` past the end of its
  (short) mode list. Looks like a smoking gun, but the file was already on disk
  during a run that rendered 8192 frames fine. Not the cause.
- **Bottle winver** — SA2 runs under the bottle's `win10`. A per-app `win7`
  `AppDefaults` override changes nothing here.

## SOLVED: 25-30 fps -> locked 60 (x87sidecar), 2026-07-27

SA2 is x87-heavy 32-bit code and Rosetta is very slow at x87. The driver was
never the problem (see the profiler numbers below). `./play-sa2-d9mt.sh` wires
x87sidecar in via the winetemp loader shim and the game locks 60.

**The trap that cost hours: use `x87sidecar_entitled`, NOT `x87sidecar`.**
The build emits two binaries. The plain one has *no entitlements*, so
`task_for_pid` is denied; the sidecar then **silently launches the game
unaccelerated**. No error, no crash, normal frame rate — indistinguishable
from a run with no sidecar at all. `x87sidecar_entitled` carries
`com.apple.security.cs.debugger`.

Verify attach directly — and force a pty, because the sidecar's messages go
into a block-buffered pipe and are destroyed by its `exec`, so they never
reach a redirected log:

```sh
script -q /dev/null env X87_LOGS=1 \
  x87sidecar/build/bin/x87sidecar_entitled "<winetemp>/sonic2App.exe.real"
```

Good: `Attached successfully` + `Found rosetta runtime offsets successfully!`
Bad:  `attach: task_for_pid(N) failed` -> then it runs the game anyway, slow.

Always pass `X87_LOGS=1`, and make any shim log whether its `exec` was taken —
a failed attach is otherwise invisible.

## Historical: the ~30 fps investigation (before x87sidecar)

Not yet diagnosed. Facts established so far:

- Runs fullscreen 2560x1600, swap effect DISCARD (`ResetSwapChain` log).
- ~3 command buffers per frame (`serial=24424` at `frame=8192`).
- Residency set grows monotonically over the run: `residencyNet` 27 -> 2676
  (adds 5076, removes 2400), `argTables` 4 -> 278. If per-frame residency work
  scales with set size, cost climbs the longer the run goes. **Leading lead.**

Ruled out / not evidence:

- `d3d9.maxFrameRate = -60` is a **no-op** — the Metal presenter never
  implemented the limiter (`driver/d3d9/d3d9_swapchain.cpp:975-978`;
  `m_targetFrameRate` only feeds GetRasterStatus math).
- vsync-half-rate was *hypothesised* (vsync defaults on:
  `driver/mtl/mtl_presenter.h:48`, bridge sets `layer.displaySyncEnabled` at
  `bridge/unix/winemetal_unix.c:3793`) but does **not** hold up: GTA IV reaches
  60-90 fps on this same driver, and 90 is not on any vsync ladder, so the
  presenter is not clamping. Do not re-derive this.

Next measurement: `D9MT_TRACE=1 D9MT_PROFILE=1` for the zone profiler, set on
the `steam.exe` process so it propagates like the HUD env does. The Metal HUD
also gives the CPU-vs-GPU split directly. The `DIAG frame=...` stderr lines
carry no timestamps, so they cannot yield fps on their own.
