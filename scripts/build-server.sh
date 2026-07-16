#!/bin/bash
# build-server.sh — build d9mt-serverd (native arm64 render server, Phase S1)
# plus its x86_64 synthetic-client test gate (server/test_feed.c).
#
# Standalone on purpose: build-driver.sh is NOT modified. Integration point
# per docs/RENDER_SERVER_SPEC.md "Build": invoke this at the END of
# build-driver.sh, non-fatal while experimental.
#
# usage: scripts/build-server.sh [--no-test]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/server"
mkdir -p "$OUT"

echo "== d9mt-serverd (arm64) =="
clang -arch arm64 -O2 -fobjc-arc -std=gnu11 -Wall -Wextra \
  "$ROOT/server/main.m" \
  "$ROOT/server/consumer.m" \
  "$ROOT/server/iosurface_pool.m" \
  -framework Foundation -framework Metal -framework QuartzCore \
  -framework IOSurface \
  -o "$OUT/d9mt-serverd"

file "$OUT/d9mt-serverd"
file "$OUT/d9mt-serverd" | grep -q 'arm64' || {
  echo "ERROR: d9mt-serverd is not arm64" >&2
  exit 1
}

echo "== test_feed (x86_64 synthetic client) =="
clang -arch x86_64 -O2 -std=c11 -Wall -Wextra \
  "$ROOT/server/test_feed.c" \
  -o "$OUT/test_feed"
file "$OUT/test_feed"

if [[ "${1:-}" != "--no-test" ]]; then
  echo "== S1 test gate =="
  "$OUT/test_feed" "$OUT/d9mt-serverd"
fi
