#!/usr/bin/env bash
# Build the forked winemetal bridge (v2/bridge): DXMT's winemetal + our
# additive entries (vertex-descriptor PSO, d9mtair AIR-compile thunks).
#
#   unix side : winemetal.so   (x86_64 Mach-O; links the d9mtair compile core
#               + lean LLVM; wine unix ABI via CrossOver's ntdll.so/winemac.so)
#   PE side   : winemetal.dll (x86_64) + winemetal32.dll (i686), builtin-marked
#   gate      : tests/bridge_smoke.cpp (vertex-descriptor PSO + AIR parity)
#
# Mirrors scripts/build-d9mtair.sh conventions; meson flags derived from
# upstream bridge/meson.build + bridge/unix/meson.build.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$V2/.." && pwd)"
BR="$V2/bridge"
DX="$V2/third_party/dxbc-spirv"
OUT="$V2/build/bridge"
AIRLIB_OUT="$V2/build/airlib"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

CX="${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}"
CX_UNIX="$CX/lib/wine/x86_64-unix"
[ -f "$CX_UNIX/ntdll.so" ] || { echo "FATAL: CrossOver unix libs not at $CX_UNIX" >&2; exit 1; }

LLVM="$V2/toolchains/llvm-x86_64"
[ -f "$LLVM/lib/libLLVMBitWriter.a" ] || { echo "FATAL: run scripts/build-llvm-unixside.sh first" >&2; exit 1; }

# The airlib compile-core objects (AirBuilder + metallib writer + ir core).
# build-d9mtair.sh must have run; reuse its object cache.
[ -f "$AIRLIB_OUT/d9mtair.so" ] || bash "$V2/scripts/build-d9mtair.sh"

CORE_OBJS=("$OBJ"/../../airlib/obj/third_party_dxbc-spirv_ir_*.o \
           "$OBJ"/../../airlib/obj/third_party_dxbc-spirv_util_*.o \
           "$OBJ"/../../airlib/obj/air_air_builder.cpp.o \
           "$OBJ"/../../airlib/obj/air_air_metallib.cpp.o)

CXXFLAGS_UNIX=(-std=c++17 -O2 -arch x86_64 -fvisibility=hidden
               -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib"
               -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
CFLAGS_UNIX=(-ObjC -O2 -arch x86_64 -I"$BR" -I"$BR/unix" -I"$V2/airlib")
LLVM_LIBS=("$LLVM"/lib/libLLVM*.a)

echo "[bridge] airlib compile core for the bridge (table symbols renamed:"
echo "         airlib/unix.cpp also defines __wine_unix_call_funcs; the bridge"
echo "         has its own tables, so airlib's are renamed out of the way)"
clang++ "${CXXFLAGS_UNIX[@]}" \
  -D__wine_unix_call_funcs=d9mtair_private_call_funcs \
  -D__wine_unix_call_wow64_funcs=d9mtair_private_wow64_funcs \
  -c "$V2/airlib/unix.cpp" -o "$OBJ/airlib_unix_renamed.o"

echo "[bridge] unix winemetal.so"
for src in "$BR/unix/winemetal_unix.c" "$BR/unix/cache.c" "$BR/unix/dxso_stubs.c"; do
  obj="$OBJ/$(basename "$src").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[cc] ${src#$BR/}"
    clang "${CFLAGS_UNIX[@]}" -I"$ROOT/../dxmt/src/airconv" -c "$src" -o "$obj" \
      || clang "${CFLAGS_UNIX[@]}" -c "$src" -o "$obj"
  fi
done

# exports: upstream's two tables + the d9mtair direct symbols (host testing)
cat > "$OUT/exports.list" <<'EOF'
___wine_unix_call_funcs
___wine_unix_call_wow64_funcs
_d9mtair_version
_d9mtair_compile
_d9mtair_free
EOF

clang++ -arch x86_64 -dynamiclib \
  -o "$OUT/winemetal.so" \
  "$OBJ/winemetal_unix.c.o" "$OBJ/cache.c.o" "$OBJ/dxso_stubs.c.o" \
  "$OBJ/airlib_unix_renamed.o" \
  $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_ir_*.o) \
  $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_util_*.o 2>/dev/null || true) \
  "$AIRLIB_OUT/obj/air_air_builder.cpp.o" "$AIRLIB_OUT/obj/air_air_metallib.cpp.o" \
  "${LLVM_LIBS[@]}" \
  -install_name @rpath/winemetal.so \
  -Wl,-exported_symbols_list,"$OUT/exports.list" \
  -Wl,-U,_NtSetEvent -Wl,-U,_macdrv_get_metal_layer \
  -undefined dynamic_lookup \
  -weak_framework CoreFoundation -weak_framework Metal \
  -weak_framework Foundation -weak_framework CoreGraphics \
  -weak_framework QuartzCore -weak_framework ColorSync -weak_framework Cocoa \
  -framework MetalFX \
  -weak_framework IOSurface \
  -lsqlite3 -lm -lpthread
echo "  -> $OUT/winemetal.so ($(wc -c < "$OUT/winemetal.so") bytes)"

echo "[bridge] ntdll import libs"
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

echo "[bridge] PE winemetal dlls"
PE_SRC=("$BR/main.c" "$BR/winecrt0_compat.c" "$BR/winemetal_thunks.c" "$BR/airconv_thunks.c" "$BR/d9mtair_thunks.c")
mkdir -p "$OUT/i386" "$OUT/x86_64"
i686-w64-mingw32-gcc -shared -O2 -s -o "$OUT/i386/winemetal.dll" \
  "${PE_SRC[@]}" -I"$BR" -I"$V2/airlib" -I"$ROOT/../dxmt/src/airconv" \
  "$OUT/libntdll-cx32.a" -Wl,--file-alignment,0x1000
x86_64-w64-mingw32-gcc -shared -O2 -s -o "$OUT/x86_64/winemetal.dll" \
  "${PE_SRC[@]}" -I"$BR" -I"$V2/airlib" -I"$ROOT/../dxmt/src/airconv" \
  "$OUT/libntdll-cx64.a" -Wl,--file-alignment,0x1000
cp "$OUT/i386/winemetal.dll" "$OUT/winemetal32.dll"
cp "$OUT/x86_64/winemetal.dll" "$OUT/winemetal64.dll"

if [ -f "$ROOT/tools/make-builtin.py" ]; then
  python3 "$ROOT/tools/make-builtin.py" "$OUT/winemetal32.dll"
  python3 "$ROOT/tools/make-builtin.py" "$OUT/winemetal64.dll"
fi

echo "[bridge] import lib for the driver"
"$V2/scripts/gen-bridge-def.sh" > "$OUT/winemetal.def" 2>/dev/null || {
  # derive exports from the header's WINEMETAL_API decls
  { echo "LIBRARY winemetal.dll"; echo "EXPORTS";
    /usr/bin/grep -hoE '^(WINEMETAL_API[^;]*?)([A-Za-z_][A-Za-z0-9_]*)\(' \
      "$BR/winemetal.h" "$BR/d9mtair_thunks.h" "$BR/airconv_public.h" 2>/dev/null \
      | sed -E 's/.*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*)\($/  \1/' | sort -u; } \
    > "$OUT/winemetal.def"
}
i686-w64-mingw32-dlltool -d "$OUT/winemetal.def" \
  -l "$OUT/libwinemetal32.a" --dllname winemetal.dll

echo "[bridge] smoke test (x86_64 host)"
[ -f "$V2/build/air-compiler/vs.metallib" ] || bash "$V2/scripts/build-air-compiler.sh" >/dev/null
clang++ "${CXXFLAGS_UNIX[@]}" \
  "$BR/tests/bridge_smoke.cpp" \
  $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_*.o) \
  "$AIRLIB_OUT/obj/air_air_builder.cpp.o" "$AIRLIB_OUT/obj/air_air_metallib.cpp.o" \
  "${LLVM_LIBS[@]}" -lm -lpthread \
  -o "$OUT/bridge_smoke"
(cd "$V2" && "$OUT/bridge_smoke" \
  "$OUT/winemetal.so" "$AIRLIB_OUT/d9mtair.so" \
  build/air-compiler/vs.metallib build/air-compiler/ps.metallib \
  air/tests/bin/vs_mvp.bin)

echo "[bridge] done"
