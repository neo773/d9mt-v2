#!/usr/bin/env bash
# run-gta4.sh — launch GTA IV on the v2 Metal-4 driver.
#
# Deploys our v2 d3d9.dll + installs the v2 winemetal bridge builtins, then
# hands off to the parent's play-gta4-d9mt.sh for x87sidecar + the signed
# loader (NOT --deploy: that would overwrite our dll with v1's d3d9fe.dll).
#
#   scripts/run-gta4.sh            # build v2, deploy, install bridge, launch
#   scripts/run-gta4.sh --no-build
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
LAUNCHER="$(cd "$V2/../.." && pwd)"          # /Users/huzef/Desktop/L4D2-launcher
PLAY="$LAUNCHER/play-gta4-d9mt.sh"
BOTTLE="Rockstar Games Launcher"
GAME_DIR="${GTA4_GAME_DIR:-$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/Program Files (x86)/Steam/steamapps/common/GTA IV DRM-Free}"

[ -f "$PLAY" ] || { echo "FATAL: $PLAY missing" >&2; exit 1; }

if [ "${1:-}" != "--no-build" ]; then
  echo "[gta4] building bridge + driver"
  bash "$V2/scripts/build-bridge.sh" >/dev/null
  bash "$V2/scripts/build-driver.sh" >/dev/null
fi

echo "[gta4] installing v2 bridge winemetal builtins"
bash "$V2/scripts/install-bridge-builtins.sh" >/dev/null

echo "[gta4] deploying v2 d3d9.dll into the game dir"
[ -d "$GAME_DIR" ] || { echo "FATAL: game dir not found: $GAME_DIR" >&2; exit 1; }
[ -f "$GAME_DIR/d3d9.dll.v1bak" ] || cp "$GAME_DIR/d3d9.dll" "$GAME_DIR/d3d9.dll.v1bak" 2>/dev/null || true
cp "$V2/build/driver/d3d9.dll" "$GAME_DIR/d3d9.dll"
echo "[gta4]   -> $(ls -la "$V2/build/driver/d3d9.dll" | awk '{print $5}') bytes"

# Hand off to the x87 + loader path WITHOUT --deploy (keep our dll).
echo "[gta4] launching via play-gta4-d9mt.sh (x87 accel)"
cd "$LAUNCHER"
exec ./play-gta4-d9mt.sh "${@:---no-op}"
