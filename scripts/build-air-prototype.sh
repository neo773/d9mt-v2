#!/usr/bin/env bash
# Build + run the AIR emission prototype (host arm64, not mingw).
#
# 1. air_prototype    — hand-builds an AIR LLVM module (llvm@15) and wraps it
#                       in our own metallib container.
# 2. metallib_load_test — loads the result with newLibraryWithData, builds a
#                       PSO, renders offscreen, verifies pixels.
#
# Requires: brew install llvm@15 (must be 15.0.7 to match AIR's LLVM era).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$V2/build/air-proto"
mkdir -p "$BUILD"

LLVM_CONFIG="${LLVM_CONFIG:-/opt/homebrew/opt/llvm@15/bin/llvm-config}"
if [ ! -x "$LLVM_CONFIG" ]; then
  echo "FATAL: llvm@15 not found at $LLVM_CONFIG (brew install llvm@15)" >&2
  exit 1
fi

LLVM_VERSION="$("$LLVM_CONFIG" --version)"
echo "[air-proto] llvm $LLVM_VERSION"

CXXFLAGS="$("$LLVM_CONFIG" --cxxflags) -std=c++17 -O1 -g"
LDFLAGS="$("$LLVM_CONFIG" --ldflags) $("$LLVM_CONFIG" --libs bitwriter core support) $("$LLVM_CONFIG" --system-libs)"

echo "[air-proto] building air_prototype"
clang++ $CXXFLAGS \
  "$V2/air/tools/air_prototype.cpp" \
  "$V2/air/air_metallib.cpp" \
  $LDFLAGS \
  -o "$BUILD/air_prototype"

echo "[air-proto] building metallib_load_test"
clang++ -std=c++17 -O1 -g -fobjc-arc \
  "$V2/air/tools/metallib_load_test.mm" \
  -framework Metal -framework Foundation \
  -o "$BUILD/metallib_load_test"

echo "[air-proto] emitting metallib"
"$BUILD/air_prototype" "$BUILD/proto.metallib" "$@"

echo "[air-proto] load + render test"
"$BUILD/metallib_load_test" "$BUILD/proto.metallib"
