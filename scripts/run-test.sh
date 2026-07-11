#!/usr/bin/env bash
# run-test.sh — the runtime ladder: run one test exe on the v2 driver under
# CrossOver and capture a screenshot.
#
#   scripts/run-test.sh code_shader            # build+deploy+run+screenshot
#   scripts/run-test.sh code_shader --no-build # skip rebuilds
#
# What it does:
#  1. builds bridge + driver + the test exe (i686 PE)
#  2. installs the BRIDGE winemetal builtins into CrossOver's shared wine
#     tree (originals backed up once as *.cx-orig) — same convention the old
#     v2 run.sh used; restore with --restore-winemetal
#  3. deploys our d3d9.dll + the exe into the bottle's drive_c/d9mt-test/
#  4. launches via CrossOver wine with d3d9 native-override
#  5. waits, focuses the window, screencaptures to build/test/<name>.png
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"
BOTTLE="${BOTTLE:-Rockstar Games Launcher}"
BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
DEPLOY="$BOTTLE_DIR/drive_c/d9mt-test"
OUT="$V2/build/test"

if [ "${1:-}" = "--restore-winemetal" ]; then
  for f in "$CX/lib/wine/i386-windows/winemetal.dll" \
           "$CX/lib/wine/x86_64-windows/winemetal.dll" \
           "$CX/lib/wine/x86_64-unix/winemetal.so"; do
    [ -f "$f.cx-orig" ] && cp "$f.cx-orig" "$f" && echo "restored $f"
  done
  exit 0
fi

NAME="${1:?usage: run-test.sh <code_name> [--no-build]}"
NOBUILD="${2:-}"
mkdir -p "$OUT" "$DEPLOY"

if [ "$NOBUILD" != "--no-build" ]; then
  echo "[run-test] building bridge + driver"
  bash "$V2/scripts/build-bridge.sh" >/dev/null
  bash "$V2/scripts/build-driver.sh" >/dev/null
fi

echo "[run-test] building $NAME.exe"
i686-w64-mingw32-gcc -O1 "$V2/test/$NAME.c" -o "$OUT/$NAME.exe" \
  -I "$V2/test" -ld3d9 -lgdi32 -luser32

echo "[run-test] installing bridge winemetal builtins (backing up originals)"
install_one() {
  local src="$1" dst="$2"
  [ -f "$dst" ] && [ ! -f "$dst.cx-orig" ] && cp "$dst" "$dst.cx-orig"
  cp "$src" "$dst"
}
install_one "$V2/build/bridge/winemetal32.dll" "$CX/lib/wine/i386-windows/winemetal.dll"
install_one "$V2/build/bridge/winemetal64.dll" "$CX/lib/wine/x86_64-windows/winemetal.dll"
install_one "$V2/build/bridge/winemetal.so"    "$CX/lib/wine/x86_64-unix/winemetal.so"

echo "[run-test] deploying driver + exe to bottle '$BOTTLE'"
cp "$V2/build/driver/d3d9.dll" "$DEPLOY/d3d9.dll"
cp "$OUT/$NAME.exe" "$DEPLOY/"

echo "[run-test] launching"
export WINEDLLOVERRIDES="d3d9=n;winemetal=b"
"$CX/bin/wine" --bottle "$BOTTLE" --cx-log "$OUT/$NAME-wine.log" \
  start /unix "$DEPLOY/$NAME.exe" >/dev/null 2>&1 || true

sleep "${WAIT:-12}"

echo "[run-test] screenshot"
osascript -e "tell application \"System Events\" to perform action \"AXRaise\" of (first window of (first process whose name contains \"$NAME\"))" 2>/dev/null || true
sleep 1
screencapture -x "$OUT/$NAME.png"
echo "[run-test] -> $OUT/$NAME.png"
echo "[run-test] driver log (if any): $DEPLOY/d3d9.log"
tail -5 "$DEPLOY/d3d9.log" 2>/dev/null || echo "  (no driver log yet)"
