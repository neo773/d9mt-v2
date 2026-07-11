#!/usr/bin/env bash
# install-bridge-builtins.sh — one-time install of OUR winemetal (the bridge
# fork with the vertex-descriptor PSO + sampler/VTF + AIR-thunk additions)
# into CrossOver's wine tree. Originals backed up as *.cx-orig.
#
# Needs write access to /Applications/CrossOver.app (macOS "App Management"):
#   EITHER grant your terminal App Management in System Settings > Privacy,
#   OR run:  sudo bash v2/scripts/install-bridge-builtins.sh
#
# Restore stock CrossOver winemetal:  bash ... --restore
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"

pairs=(
  "$V2/build/bridge/winemetal32.dll|$CX/lib/wine/i386-windows/winemetal.dll"
  "$V2/build/bridge/winemetal64.dll|$CX/lib/wine/x86_64-windows/winemetal.dll"
  "$V2/build/bridge/winemetal.so|$CX/lib/wine/x86_64-unix/winemetal.so"
)

if [ "${1:-}" = "--restore" ]; then
  for p in "${pairs[@]}"; do
    dst="${p#*|}"
    [ -f "$dst.cx-orig" ] && cp "$dst.cx-orig" "$dst" && echo "restored $dst"
  done
  exit 0
fi

for p in "${pairs[@]}"; do
  src="${p%%|*}"; dst="${p#*|}"
  [ -f "$src" ] || { echo "FATAL: $src missing — run scripts/build-bridge.sh" >&2; exit 1; }
  [ -f "$dst" ] && [ ! -f "$dst.cx-orig" ] && cp "$dst" "$dst.cx-orig"
  cp "$src" "$dst"
  echo "installed $dst"
done
echo "done — GTA + tests now see the bridge ABI (restore: --restore)"
