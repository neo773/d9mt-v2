#!/usr/bin/env bash
# dump-verify.sh — autonomous backbuffer verifier (SOURCE_OF_TRUTH §4b bug 2).
#
#   scripts/dump-verify.sh code_shader             # build+deploy+run+dump+check
#   scripts/dump-verify.sh code_shader --no-build  # skip rebuilds
#
# Instead of screencapture (unreliable across Spaces), this drops the trigger
# file C:/d9mt-test/DUMP into the bottle, runs the exe, and the DRIVER copies
# its rendered backbuffer to C:/d9mt-test/rt_dump.bin (u32 w, u32 h, raw BGRA8).
# We then read that file and check the center pixels are non-black.
#
# Env: EXPECT=nonblack|r,g,b  WAIT=<launch settle secs>  POLL=<dump wait secs>
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"
BOTTLE="${BOTTLE:-Rockstar Games Launcher}"
BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
DEPLOY="$BOTTLE_DIR/drive_c/d9mt-test"
OUT="$V2/build/test"

NAME="${1:?usage: dump-verify.sh <code_name> [--no-build]}"
NOBUILD="${2:-}"
mkdir -p "$OUT" "$DEPLOY"

if [ "$NOBUILD" != "--no-build" ]; then
  echo "[dump-verify] building bridge + driver"
  bash "$V2/scripts/build-bridge.sh" >/dev/null
  bash "$V2/scripts/build-driver.sh" >/dev/null
fi

echo "[dump-verify] building $NAME.exe"
i686-w64-mingw32-gcc -O1 "$V2/test/$NAME.c" -o "$OUT/$NAME.exe" \
  -I "$V2/test" -ld3d9 -lgdi32 -luser32

echo "[dump-verify] staging bridge winemetal builtins via WINEDLLPATH"
WMTREE="$V2/build/winepath"
mkdir -p "$WMTREE/i386-windows" "$WMTREE/x86_64-windows" "$WMTREE/x86_64-unix"
cp "$V2/build/bridge/winemetal32.dll" "$WMTREE/i386-windows/winemetal.dll"
cp "$V2/build/bridge/winemetal64.dll" "$WMTREE/x86_64-windows/winemetal.dll"
cp "$V2/build/bridge/winemetal.so"    "$WMTREE/x86_64-unix/winemetal.so"
export WINEDLLPATH="$WMTREE/i386-windows:$WMTREE/x86_64-windows:$WMTREE/x86_64-unix"

echo "[dump-verify] deploying driver + exe"
cp "$V2/build/driver/d3d9.dll" "$DEPLOY/d3d9.dll"
cp "$OUT/$NAME.exe" "$DEPLOY/"

# Clear any stale dump, arm the trigger.
rm -f "$DEPLOY/rt_dump.bin"
: > "$DEPLOY/DUMP"
echo "[dump-verify] armed trigger $DEPLOY/DUMP"

echo "[dump-verify] launching (absolute path)"
export WINEDLLOVERRIDES="d3d9=n;winemetal=b"
( "$CX/bin/wine" --bottle "$BOTTLE" --cx-log "$OUT/$NAME-dump-wine.log" \
    "$DEPLOY/$NAME.exe" >/dev/null 2>&1 & )

# Poll for the dump file to appear (driver writes it on the first Present that
# sees the trigger).
POLL="${POLL:-25}"
i=0
while [ ! -f "$DEPLOY/rt_dump.bin" ] && [ "$i" -lt "$POLL" ]; do
  sleep 1; i=$((i+1))
done

pkill -9 -f "$NAME.exe" 2>/dev/null || true

if [ ! -f "$DEPLOY/rt_dump.bin" ]; then
  echo "[dump-verify] FAIL: no rt_dump.bin after ${POLL}s"
  tail -20 "$DEPLOY/${NAME}_d3d9.log" 2>/dev/null || true
  exit 1
fi

echo "[dump-verify] checking rt_dump.bin (${EXPECT:-nonblack})"
EXPECT="${EXPECT:-nonblack}" python3 - "$DEPLOY/rt_dump.bin" <<'PY'
import sys, os, struct
path = sys.argv[1]
expect = os.environ.get("EXPECT", "nonblack")
with open(path, "rb") as f:
    blob = f.read()
w, h = struct.unpack_from("<II", blob, 0)
pix = blob[8:]
def bgra(x, y):
    o = (y*w + x)*4
    b, g, r, a = pix[o], pix[o+1], pix[o+2], pix[o+3]
    return (r, g, b, a)
cx, cy = w//2, h//2
pts = [(cx,cy),(cx-10,cy),(cx+10,cy),(cx,cy-10),(cx,cy+10),
       (cx-40,cy+40),(cx+40,cy-40),(cx-80,cy),(cx+80,cy)]
samples = [bgra(x,y) for (x,y) in pts if 0<=x<w and 0<=y<h]
best = max(samples, key=lambda c: c[0]+c[1]+c[2])
print(f"size={w}x{h} center={bgra(cx,cy)} brightest={best}")
print(f"samples={samples}")
if expect == "nonblack":
    ok = (best[0]+best[1]+best[2]) > 60
else:
    tr,tg,tb = map(int, expect.split(","))
    ok = any(abs(s[0]-tr)<48 and abs(s[1]-tg)<48 and abs(s[2]-tb)<48
             for s in samples)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
