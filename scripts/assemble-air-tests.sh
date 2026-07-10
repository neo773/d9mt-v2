#!/usr/bin/env bash
# Assemble the AIR differential-test corpus (v2/air/tests/asm/*.asm) into raw
# SM3 bytecode at v2/air/tests/bin/*.bin via D3DXAssembleShader under Wine
# (same mechanism as scripts/assemble-shaders.sh). Commit the .bin files —
# the diff harness reads those, so Wine is only needed when adding cases.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
BOTTLE="Rockstar Games Launcher"
TESTDIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/d9mt-test"

mkdir -p "$TESTDIR" "$V2/air/tests/bin"
cp "$V2/build/asm_shader.exe" "$TESTDIR/"

for asm in "$V2"/air/tests/asm/*.asm; do
  name="$(basename "$asm" .asm)"
  bin="$V2/air/tests/bin/$name.bin"
  if [ -f "$bin" ] && [ ! "$asm" -nt "$bin" ]; then
    continue
  fi
  cp "$asm" "$TESTDIR/$name.asm"
  ( cd "$TESTDIR" && "$WINE" --bottle "$BOTTLE" "$TESTDIR/asm_shader.exe" "$name.asm" "$name.bin" )
  cp "$TESTDIR/$name.bin" "$bin"
  echo "[asm] $name.asm -> tests/bin/$name.bin"
done
