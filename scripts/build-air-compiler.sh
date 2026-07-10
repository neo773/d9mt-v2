#!/usr/bin/env bash
# Build the host-side AIR compiler CLI (SM3 bytecode -> IR -> AIR/metallib).
#
# Links the vendored dxbc-spirv front-end (sm3 + ir + util) with the v2 AIR
# backend (v2/air). Host arm64 tool — the PE integration reuses the same
# sources via build-dxvk.sh later.
#
# Objects cached per-TU in build/air-compiler/obj; stale check is mtime-based.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
DX="$V2/third_party/dxbc-spirv"
BUILD="$V2/build/air-compiler"
OBJ="$BUILD/obj"
mkdir -p "$OBJ"

LLVM_CONFIG="${LLVM_CONFIG:-/opt/homebrew/opt/llvm@15/bin/llvm-config}"
[ -x "$LLVM_CONFIG" ] || { echo "FATAL: need brew llvm@15" >&2; exit 1; }

CXX="${CXX:-clang++}"
ROOT="$(cd "$V2/.." && pwd)"
SC="$ROOT/vendor/spirv-cross"
COMMON_FLAGS=(-std=c++17 -O1 -g -I"$DX" -I"$DX/ir" -I"$SC" -Wno-unused-parameter)
LLVM_CXXFLAGS="$("$LLVM_CONFIG" --cxxflags | sed 's/-std=c++14//')"

SRCS=(
  "$DX"/ir/*.cpp
  "$DX"/ir/passes/*.cpp
  "$DX"/sm3/*.cpp
  "$DX"/spirv/*.cpp
  "$DX"/util/*.cpp
  "$SC"/spirv_cross.cpp
  "$SC"/spirv_parser.cpp
  "$SC"/spirv_cross_parsed_ir.cpp
  "$SC"/spirv_cfg.cpp
  "$SC"/spirv_glsl.cpp
  "$SC"/spirv_msl.cpp
  "$V2"/air/*.cpp
)

OBJS=()
PIDS=()
for src in "${SRCS[@]}"; do
  obj="$OBJ/$(echo "${src#$V2/}" | tr '/' '_').o"
  OBJS+=("$obj")
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[cc] ${src#$V2/}"
    case "$src" in
      */air/*) $CXX "${COMMON_FLAGS[@]}" $LLVM_CXXFLAGS -c "$src" -o "$obj" & ;;
      *)       $CXX "${COMMON_FLAGS[@]}" -c "$src" -o "$obj" & ;;
    esac
    PIDS+=($!)
    if [ "${#PIDS[@]}" -ge 10 ]; then wait "${PIDS[@]}"; PIDS=(); fi
  fi
done
[ "${#PIDS[@]}" -gt 0 ] && wait "${PIDS[@]}"

echo "[link] air_compiler"
$CXX "${COMMON_FLAGS[@]}" $LLVM_CXXFLAGS \
  "$V2/air/tools/air_compiler.cpp" \
  "${OBJS[@]}" \
  $("$LLVM_CONFIG" --ldflags --libs bitwriter core support) \
  $("$LLVM_CONFIG" --system-libs) \
  -o "$BUILD/air_compiler"

echo "[build] air_render_test"
clang++ -std=c++17 -O1 -g -fobjc-arc \
  "$V2/air/tools/air_render_test.mm" \
  -framework Metal -framework Foundation \
  -o "$BUILD/air_render_test"

echo "[build] air_diff_test"
clang++ -std=c++17 -O1 -g -fobjc-arc \
  "$V2/air/tools/air_diff_test.mm" \
  -framework Metal -framework Foundation \
  -o "$BUILD/air_diff_test"

echo "[run] shadertri VS/PS -> AIR -> render"
"$BUILD/air_compiler" --test-vs -o "$BUILD/vs.metallib"
"$BUILD/air_compiler" --test-ps -o "$BUILD/ps.metallib"
"$BUILD/air_render_test" "$BUILD/vs.metallib" "$BUILD/ps.metallib"
