#!/usr/bin/env bash
# Build LLVM 15.0.7 as x86_64 Mach-O static libs for the AIR unixlib.
#
# The AIR emitter (LLVM-dependent) lives on Wine's unix side (a Mach-O
# dylib companion like d9mtmetal), NOT in the PE d3d9.dll — so LLVM is a
# plain macOS x86_64 build (wine's unix side runs x86_64 under Rosetta).
# Recipe mirrors DXMT docs/DEVELOPMENT.md; no codegen targets, no tools —
# we only need Core/BitWriter/Support.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
TC="$V2/toolchains"
mkdir -p "$TC"
grep -q toolchains "$V2/.gitignore" || printf '\n# local LLVM toolchain builds\n/toolchains/\n' >> "$V2/.gitignore"

if [ ! -d "$TC/llvm-project" ]; then
  echo "[llvm] cloning llvmorg-15.0.7 (shallow)"
  git clone --depth 1 --branch llvmorg-15.0.7 \
    https://github.com/llvm/llvm-project.git "$TC/llvm-project"
fi

cmake -B "$TC/llvm-build" -S "$TC/llvm-project/llvm" \
  -DCMAKE_INSTALL_PREFIX="$TC/llvm-x86_64" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
  -DLLVM_ENABLE_ASSERTIONS=Off \
  -DLLVM_ENABLE_ZSTD=Off \
  -DLLVM_ENABLE_ZLIB=Off \
  -DLLVM_ENABLE_LIBXML2=Off \
  -DLLVM_ENABLE_TERMINFO=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_BUILD_TOOLS=Off \
  -DLLVM_INCLUDE_TESTS=Off \
  -DLLVM_INCLUDE_BENCHMARKS=Off \
  -DLLVM_INCLUDE_EXAMPLES=Off \
  -G Ninja

cmake --build "$TC/llvm-build"
cmake --install "$TC/llvm-build"
echo "[llvm] done: $TC/llvm-x86_64"
