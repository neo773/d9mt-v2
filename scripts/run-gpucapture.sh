#!/usr/bin/env bash
# Capture one Metal GPU frame (.gputrace) of a v2 test exe, openable in Xcode's
# Metal Debugger or the gpudebug CLI. This is the real GPU-level view of what the
# driver emits — buffers, textures, shaders, draw calls.
#
# How it works:
#  - Metal frame capture needs MTL_CAPTURE_ENABLED=1 in the *unix* wine process.
#    CrossOver strips it from the shell env but propagates cxbottle.conf
#    [EnvironmentVariables], so we add it there for the run and remove it after
#    (it otherwise hangs rendering after ~3 frames via the drawable pool).
#  - The driver captures the next frame when C:\d9mt-test\DO_CAPTURE exists
#    (D9MT_FUNC_CAPTURE -> MTLCaptureManager), writing to a unix path. We trigger
#    it before the first frame so the capture completes before the 3-frame hang.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE="Rockstar Games Launcher"
BD="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
CONF="$BD/cxbottle.conf"
TESTDIR="$BD/drive_c/d9mt-test"
EXE="${1:-code_alpha.exe}"
OUT="${2:-/tmp/d9mt-frame.gputrace}"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"

cleanup() { sed -i '' '/"MTL_CAPTURE_ENABLED" = "1"/d' "$CONF" 2>/dev/null || true; pkill -9 -f "$EXE" 2>/dev/null || true; }
trap cleanup EXIT

pkill -9 -f code_ 2>/dev/null || true
"$CX/bin/wineserver" -k 2>/dev/null || true
sleep 2

grep -q MTL_CAPTURE_ENABLED "$CONF" || sed -i '' 's/^\[EnvironmentVariables\]/[EnvironmentVariables]\n"MTL_CAPTURE_ENABLED" = "1"/' "$CONF"
rm -rf "$OUT"
touch "$TESTDIR/DO_CAPTURE"

bash "$V2/scripts/run.sh" "$EXE" >/dev/null 2>&1 &
echo "[gpucapture] $EXE launched; capturing frame 1..."
for i in $(seq 1 12); do
  sleep 3
  [ -d /tmp/d9mt-frame.gputrace ] && break
done

if [ -d /tmp/d9mt-frame.gputrace ]; then
  [ "$OUT" != /tmp/d9mt-frame.gputrace ] && { rm -rf "$OUT"; mv /tmp/d9mt-frame.gputrace "$OUT"; }
  echo "[gpucapture] captured: $OUT ($(du -sh "$OUT" | cut -f1))"
  echo "[gpucapture] open with: open -a Xcode \"$OUT\"   (or: gpudebug \"$OUT\")"
else
  echo "[gpucapture] FAILED — no trace produced" >&2
  exit 1
fi
