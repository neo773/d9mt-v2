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

echo "[run-test] staging bridge winemetal builtins via WINEDLLPATH (no CX-tree writes)"
# CrossOver's app bundle is TCC-protected; wine honors WINEDLLPATH as an
# extra builtin search root instead. Layout mirrors the wine tree.
WMTREE="$V2/build/winepath"
mkdir -p "$WMTREE/i386-windows" "$WMTREE/x86_64-windows" "$WMTREE/x86_64-unix"
cp "$V2/build/bridge/winemetal32.dll" "$WMTREE/i386-windows/winemetal.dll"
cp "$V2/build/bridge/winemetal64.dll" "$WMTREE/x86_64-windows/winemetal.dll"
cp "$V2/build/bridge/winemetal.so"    "$WMTREE/x86_64-unix/winemetal.so"
export WINEDLLPATH="$WMTREE/i386-windows:$WMTREE/x86_64-windows:$WMTREE/x86_64-unix"

echo "[run-test] deploying driver + exe to bottle '$BOTTLE'"
cp "$V2/build/driver/d3d9.dll" "$DEPLOY/d3d9.dll"
cp "$OUT/$NAME.exe" "$DEPLOY/"

echo "[run-test] launching (absolute path)"
export WINEDLLOVERRIDES="d3d9=n;winemetal=b"
( "$CX/bin/wine" --bottle "$BOTTLE" --cx-log "$OUT/$NAME-wine.log" \
    "$DEPLOY/$NAME.exe" >/dev/null 2>&1 & )

sleep "${WAIT:-14}"

# Raise the wine window (accessibility) then window-capture it (screen
# recording). Both permissions are required; without them the capture is
# blank or of the wrong surface.
echo "[run-test] raise + capture"
osascript -e "tell application \"System Events\" to set frontmost of (first process whose name contains \"$NAME\") to true" 2>/dev/null || true
osascript -e "tell application \"System Events\" to perform action \"AXRaise\" of (first window of (first process whose name contains \"$NAME\"))" 2>/dev/null || true
sleep 1
# capture the focused window only (-o no shadow, -l window id if resolvable)
WID=$(osascript -e "tell application \"System Events\" to id of (first window of (first process whose name contains \"$NAME\"))" 2>/dev/null || true)
if [ -n "${WID:-}" ] && [ "$WID" != "0" ]; then
  screencapture -x -o -l "$WID" "$OUT/$NAME.png" 2>/dev/null || screencapture -x "$OUT/$NAME.png"
else
  screencapture -x "$OUT/$NAME.png"
fi

echo "[run-test] driver log:"
tail -6 "$OUT/../.." 2>/dev/null; tail -8 "$DEPLOY/${NAME}_d3d9.log" 2>/dev/null || echo "  (no driver log)"

echo "[run-test] verify pixels (${EXPECT:-nonblack}):"
python3 "$V2/scripts/verify-pixels.py" "$OUT/$NAME.png" "${EXPECT:-nonblack}"
RESULT=$?
pkill -9 -f "$NAME.exe" 2>/dev/null || true
exit $RESULT
