#!/usr/bin/env bash
# Build the d9mtair companion unixlib: the Mach-O x86_64 .so hosting
# AirBuilder + LLVM on wine's unix side, plus builtin-marked PE dlls and
# the import lib the driver links. Mirrors tools/build-d9mtmetal.sh.
#
# Also builds the x86_64 host loopback driver (airlib/loopback_main.cpp).
#
#   bash scripts/build-d9mtair.sh              # build only
#   bash scripts/build-d9mtair.sh --install    # + install into CrossOver/bottle
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$V2/.." && pwd)"
SRC="$V2/airlib"
DX="$V2/third_party/dxbc-spirv"
OUT="$V2/build/airlib"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

# Toolchain LLVM was built with LLVM_BUILD_TOOLS=Off (no llvm-config), so
# flags/archives are spelled out (matches `llvm-config --libs bitwriter
# core support` on a stock 15.0.7).
LLVM="$V2/toolchains/llvm-x86_64"
[ -f "$LLVM/lib/libLLVMBitWriter.a" ] || { echo "FATAL: run scripts/build-llvm-unixside.sh first" >&2; exit 1; }

CXXFLAGS=(-std=c++17 -O2 -arch x86_64 -fvisibility=hidden
          -I"$DX" -I"$V2" -I"$LLVM/include"
          -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
# Link the full archive set (built with no codegen targets, so it is lean);
# ld64 dead-strips unused members. Explicit subsetting kept missing
# transitive deps (MC/Analysis/Object behind BitcodeWriter's symtab path).
LLVM_LIBS=("$LLVM"/lib/libLLVM*.a)

# dxbc-spirv sources both sides need. The dylib needs ir core (deserialize)
# + util; the loopback driver additionally needs sm3 + passes (legalize).
IR_CORE=("$DX"/ir/ir.cpp "$DX"/ir/ir_builder.cpp "$DX"/ir/ir_serialize.cpp
         "$DX"/ir/ir_utils.cpp "$DX"/ir/ir_dominance.cpp "$DX"/ir/ir_divergence.cpp
         "$DX"/ir/ir_disasm.cpp
         "$DX"/util/util_log.cpp "$DX"/util/util_float16.cpp "$DX"/util/util_swizzle.cpp)
FRONT=("$DX"/ir/ir_legalize.cpp "$DX"/ir/passes/*.cpp "$DX"/sm3/*.cpp "$DX"/util/util_md5.cpp)

compile() { # dstvar srcs...
  local -n dst=$1; shift
  local pids=()
  for src in "$@"; do
    local obj="$OBJ/$(echo "${src#$V2/}" | tr '/' '_').o"
    dst+=("$obj")
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
      echo "[cc] ${src#$V2/}"
      clang++ "${CXXFLAGS[@]}" -c "$src" -o "$obj" &
      pids+=($!)
      if [ "${#pids[@]}" -ge 10 ]; then wait "${pids[@]}"; pids=(); fi
    fi
  done
  [ "${#pids[@]}" -gt 0 ] && wait "${pids[@]}"
  return 0
}

CORE_OBJS=(); compile CORE_OBJS "${IR_CORE[@]}" \
  "$V2/air/air_builder.cpp" "$V2/air/air_metallib.cpp"
FRONT_OBJS=(); compile FRONT_OBJS "${FRONT[@]}"

echo "[d9mtair] unix .so (x86_64: wine's unix side runs under Rosetta)"
clang++ "${CXXFLAGS[@]}" -dynamiclib \
  -o "$OUT/d9mtair.so" \
  "$SRC/unix.cpp" "${CORE_OBJS[@]}" "${LLVM_LIBS[@]}" \
  -lm -lpthread \
  -install_name @rpath/d9mtair.so \
  -exported_symbols_list /dev/stdin <<'EOF'
_d9mtair_version
_d9mtair_compile
_d9mtair_free
___wine_unix_call_funcs
___wine_unix_call_wow64_funcs
EOF
echo "  -> $OUT/d9mtair.so ($(wc -c < "$OUT/d9mtair.so") bytes)"

echo "[d9mtair] loopback driver (x86_64 host)"
clang++ "${CXXFLAGS[@]}" \
  "$SRC/loopback_main.cpp" "${FRONT_OBJS[@]}" "${CORE_OBJS[@]}" \
  "${LLVM_LIBS[@]}" -lm -lpthread \
  -o "$OUT/airlib_loopback"

echo "[d9mtair] ntdll import libs"
cat > "$OUT/ntdll-cx32.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call@16
  NtQueryVirtualMemory@24
EOF
i686-w64-mingw32-dlltool -k -d "$OUT/ntdll-cx32.def" \
  -l "$OUT/libntdll-cx32.a" --dllname ntdll.dll
cat > "$OUT/ntdll-cx64.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call
  NtQueryVirtualMemory
EOF
x86_64-w64-mingw32-dlltool -d "$OUT/ntdll-cx64.def" \
  -l "$OUT/libntdll-cx64.a" --dllname ntdll.dll

echo "[d9mtair] PE dlls (builtin file-alignment, stripped: see d9mtmetal notes)"
mkdir -p "$OUT/i386" "$OUT/x86_64"
i686-w64-mingw32-gcc -shared -O2 -s -o "$OUT/i386/d9mtair.dll" \
  "$SRC/dll.c" -I "$SRC" "$OUT/libntdll-cx32.a" \
  -Wl,--file-alignment,0x1000
x86_64-w64-mingw32-gcc -shared -O2 -s -o "$OUT/x86_64/d9mtair.dll" \
  "$SRC/dll.c" -I "$SRC" "$OUT/libntdll-cx64.a" \
  -Wl,--file-alignment,0x1000
cp "$OUT/i386/d9mtair.dll" "$OUT/d9mtair32.dll"
cp "$OUT/x86_64/d9mtair.dll" "$OUT/d9mtair64.dll"

if [ -f "$ROOT/tools/make-builtin.py" ]; then
  python3 "$ROOT/tools/make-builtin.py" "$OUT/d9mtair32.dll"
  python3 "$ROOT/tools/make-builtin.py" "$OUT/d9mtair64.dll"
else
  echo "  [warn] tools/make-builtin.py not found; PE dlls lack builtin marker"
fi

echo "[d9mtair] import lib for d3d9.dll"
cat > "$OUT/d9mtair.def" <<'EOF'
LIBRARY d9mtair.dll
EXPORTS
  D9MTAir_UnixCall
EOF
i686-w64-mingw32-dlltool -d "$OUT/d9mtair.def" \
  -l "$OUT/libd9mtair32.a" --dllname d9mtair.dll

if [ "${1:-}" = "--install" ]; then
  CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"
  BOTTLE="${BOTTLE:-Rockstar Games Launcher}"
  echo "[d9mtair] installing (CrossOver dxmt dir + wine tree + bottle)"
  for d in "$CX/lib/dxmt" "$CX/lib/wine"; do :; done
  mkdir -p "$CX/lib/dxmt/i386-windows" "$CX/lib/dxmt/x86_64-windows" "$CX/lib/dxmt/x86_64-unix"
  cp "$OUT/d9mtair32.dll" "$CX/lib/dxmt/i386-windows/d9mtair.dll"
  cp "$OUT/d9mtair64.dll" "$CX/lib/dxmt/x86_64-windows/d9mtair.dll"
  cp "$OUT/d9mtair.so"    "$CX/lib/dxmt/x86_64-unix/d9mtair.so"
  cp "$OUT/d9mtair32.dll" "$CX/lib/wine/i386-windows/d9mtair.dll"
  cp "$OUT/d9mtair.so"    "$CX/lib/wine/i386-windows/d9mtair.so"
  cp "$OUT/d9mtair64.dll" "$CX/lib/wine/x86_64-windows/d9mtair.dll"
  cp "$OUT/d9mtair.so"    "$CX/lib/wine/x86_64-windows/d9mtair.so"
  cp "$OUT/d9mtair.so"    "$CX/lib/wine/x86_64-unix/d9mtair.so"
  BOTTLE_WIN="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/windows"
  cp "$OUT/d9mtair32.dll" "$BOTTLE_WIN/syswow64/d9mtair.dll"
  cp "$OUT/d9mtair64.dll" "$BOTTLE_WIN/system32/d9mtair.dll"
  "$CX/bin/wine" --bottle "$BOTTLE" reg add \
    'HKCU\Software\Wine\DllOverrides' /v d9mtair /d builtin /f \
    >/dev/null 2>&1 || true
fi

echo "[d9mtair] done"
