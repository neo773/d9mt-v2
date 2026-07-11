#!/usr/bin/env bash
# run-ladder.sh — run the programmable-shader test-exe ladder on the Metal 4
# driver, each verified via the driver-side readback (rt_dump.bin). No FF exes
# (code.c) — those need the fixed-function path (LOW priority).
#
# Builds bridge+driver once, then per exe: compile, deploy, DUMP-trigger, run,
# read rt_dump.bin, assert >2% non-clear-color coverage (a rendered scene).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"
BOTTLE="Rockstar Games Launcher"
DEPLOY="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/d9mt-test"
OUT="$V2/build/test"
mkdir -p "$OUT" "$DEPLOY"

EXES="${*:-code_shader code_texture code_blend code_depth code_stencil code_cull code_index code_strip code_dxt code_mip code_rtt code_alpha code_twodraw}"

echo "[ladder] building bridge + driver + installing bridge"
bash "$V2/scripts/build-bridge.sh" >/dev/null 2>&1
bash "$V2/scripts/install-bridge-builtins.sh" >/dev/null 2>&1
bash "$V2/scripts/build-driver.sh" >/dev/null 2>&1 || { echo "driver build FAILED"; exit 1; }
cp "$V2/build/driver/d3d9.dll" "$DEPLOY/d3d9.dll"

export WINEDLLOVERRIDES="d3d9=n;winemetal=b"
pass=0; fail=0; results=""
for name in $EXES; do
  [ -f "$V2/test/$name.c" ] || { results+=$'\n'"$name  SKIP (no source)"; continue; }
  i686-w64-mingw32-gcc -O1 "$V2/test/$name.c" -o "$DEPLOY/$name.exe" \
    -I "$V2/test" -ld3d9 -lgdi32 -luser32 2>/dev/null || { results+=$'\n'"$name  BUILD-FAIL"; fail=$((fail+1)); continue; }
  rm -f "$DEPLOY/rt_dump.bin" "$DEPLOY/${name}_d3d9.log"
  touch "$DEPLOY/DUMP"
  ( "$CX/bin/wine" --bottle "$BOTTLE" "$DEPLOY/$name.exe" >/dev/null 2>&1 & )
  sleep "${WAIT:-12}"
  pkill -9 -f "$name.exe" 2>/dev/null
  pkill -9 -f winedbg 2>/dev/null
  line=$(python3 - "$DEPLOY/rt_dump.bin" <<'PY'
import sys,struct,os
p=sys.argv[1]
if not os.path.exists(p): print("NO-DUMP"); sys.exit(0)
d=open(p,'rb').read()
if len(d)<8: print("EMPTY"); sys.exit(0)
w,h=struct.unpack('<II',d[:8]); px=d[8:]
n=w*h
# background = the most common pixel (clear color)
from collections import Counter
c=Counter(px[i:i+4] for i in range(0,min(len(px),n*4),4))
bg=c.most_common(1)[0][0]
nz=sum(1 for i in range(0,n*4,4) if abs(px[i]-bg[0])+abs(px[i+1]-bg[1])+abs(px[i+2]-bg[2])>24)
pct=100*nz//n
cx,cy=w//2,h//2; o=(cy*w+cx)*4
print(f"{w}x{h} cover={pct}% center=({px[o+2]},{px[o+1]},{px[o]}) {'PASS' if pct>2 else 'FAIL'}")
PY
)
  echo "  $name  $line"
  case "$line" in *PASS*) pass=$((pass+1)); results+=$'\n'"$name  PASS  $line";; *) fail=$((fail+1)); results+=$'\n'"$name  FAIL  $line";; esac
done
echo "===================="
echo "$results"
echo "===================="
echo "ladder: pass=$pass fail=$fail"
