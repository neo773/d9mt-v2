#!/bin/bash
# build-server.sh — build d9mt-serverd (native arm64 render server, Phase S2)
# plus its x86_64 synthetic-client test gate (server/test_feed.c).
#
# Standalone on purpose: build-driver.sh tail-calls this, non-fatal while
# experimental (docs/RENDER_SERVER_SPEC.md "Build").
#
# usage: scripts/build-server.sh [--no-test]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/server"
mkdir -p "$OUT"

echo "== d9mt-serverd (arm64, ObjC + ObjC++/METAL4 executor) =="
clang -arch arm64 -O2 -fobjc-arc -std=gnu11 -Wall -Wextra -c \
  "$ROOT/server/main.m" -o "$OUT/main.o"
clang -arch arm64 -O2 -fobjc-arc -std=gnu11 -Wall -Wextra -c \
  "$ROOT/server/consumer.m" -o "$OUT/consumer.o"
clang -arch arm64 -O2 -fobjc-arc -std=gnu11 -Wall -Wextra -c \
  "$ROOT/server/iosurface_pool.m" -o "$OUT/iosurface_pool.o"
clang++ -arch arm64 -O2 -fobjc-arc -std=gnu++17 -Wall -Wextra -c \
  "$ROOT/server/executor.mm" -o "$OUT/executor.o"
clang++ -arch arm64 -O2 \
  "$OUT/main.o" "$OUT/consumer.o" "$OUT/iosurface_pool.o" "$OUT/executor.o" \
  -framework Foundation -framework Metal -framework QuartzCore \
  -framework IOSurface \
  -o "$OUT/d9mt-serverd"

file "$OUT/d9mt-serverd"
file "$OUT/d9mt-serverd" | grep -q 'arm64' || {
  echo "ERROR: d9mt-serverd is not arm64" >&2
  exit 1
}

echo "== test stub metallib (S2 draw gate) =="
xcrun -sdk macosx metal -o "$OUT/test_stub.metallib" "$ROOT/server/test_stub.metal"

echo "== test_feed (x86_64 synthetic client) =="
clang -arch x86_64 -O2 -std=c11 -Wall -Wextra \
  "$ROOT/server/test_feed.c" \
  -framework IOSurface -framework CoreFoundation \
  -o "$OUT/test_feed"
file "$OUT/test_feed"

if [[ "${1:-}" != "--no-test" ]]; then
  echo "== S1+S2 test gate =="
  "$OUT/test_feed" "$OUT/d9mt-serverd" "$OUT/test_stub.metallib"
fi
