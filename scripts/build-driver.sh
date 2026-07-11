#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build-driver.sh — compile the extracted DXVK-3.0 D3D9 state tracker + the
# Metal-first seam (v2/driver/). Phase A+B gate: every TU that is *expected to
# compile at this phase* must compile with mingw, and the Vulkan gate must pass.
#
# What compiles now (CORE, must pass):
#   util/  (the DXVK support lib, Vulkan-clean)   wsi/ (win32 monitor/window)
#   cs/    (deferred-recording arena, retargeted to MtlContext)
#   mtl/   (the MtlContext seam: types, resource, context stub, format stub)
# The d3d9/ KEEP files are extraction deliverables; most pull ADAPT headers
# (d3d9_device.h, d3d9_common_texture.h, …) that are Phase-C and so do NOT compile
# standalone yet — they are attempted in a non-fatal D3D9-LEAF pass that only
# reports. This mirrors build-air-compiler.sh conventions: per-TU objects, cached
# by mtime, parallel, c++17, i686 mingw.
# ---------------------------------------------------------------------------
set -euo pipefail

V2="$(cd "$(dirname "$0")/.." && pwd)"
DRV="$V2/driver"
BUILD="$V2/build/driver"
OBJ="$BUILD/obj"
mkdir -p "$OBJ"

MINGW="${MINGW:-i686-w64-mingw32-g++}"
command -v "$MINGW" >/dev/null || { echo "FATAL: need $MINGW" >&2; exit 1; }

FLAGS=(-std=c++17 -O1 -g -DDXVK_WSI_WIN32 -Wno-unused-parameter -msse -msse2 -mfpmath=sse)
# dxbc-spirv (sm3 IR parser) include root — the SM3->IR analysis passes
# (d3d9_shader_analysis / real d3d9_shader) include <sm3/...> and <util/...> from
# it. These are -I includes (third-party headers), exempt from the driver Vulkan
# gate, which scans driver/ only. third_party/dxbc-spirv is a symlink to the
# populated DXVK-3.0-uptsream/subprojects/dxbc-spirv submodule.
DXBC="$V2/third_party/dxbc-spirv"
FLAGS+=(-I"$DXBC")

# --- CORE source list (must compile) --------------------------------------
# util: everything except TUs pulling not-yet-ported deps.
#   util_unmap.cpp        -> ../dxvk/dxvk_allocator.h (Phase C)
#   com/com_guid.cpp      -> ../../d3d11 + ../../dxgi (see build-dxvk.sh note)
#   com/com_destruction_notifier.cpp -> broken ../util/thread.h path in this layout
CORE=()
for f in "$DRV"/util/*.cpp "$DRV"/util/sync/*.cpp "$DRV"/util/log/*.cpp \
         "$DRV"/util/config/*.cpp "$DRV"/util/com/*.cpp "$DRV"/util/sha1/*.cpp; do
  case "$f" in
    */util_unmap.cpp|*/com_guid.cpp|*/com_destruction_notifier.cpp) continue ;;
  esac
  CORE+=("$f")
done
# wsi: monitor/window only (wsi_edid.cpp needs libdisplay-info — Phase C).
CORE+=("$DRV"/wsi/wsi_platform.cpp "$DRV"/wsi/win32/wsi_monitor_win32.cpp \
       "$DRV"/wsi/win32/wsi_platform_win32.cpp "$DRV"/wsi/win32/wsi_window_win32.cpp)
# cs + mtl seam.
CORE+=("$DRV"/cs/dxvk_cs.cpp)
CORE+=("$DRV"/mtl/mtl_context_stub.cpp "$DRV"/mtl/mtl_format.cpp \
       "$DRV"/mtl/mtl_presenter_stub.cpp "$DRV"/mtl/mtl_device_stub.cpp)
# d3d9 — Phase-C ported ADAPT retypes + Phase-D neutral stubs that compile now.
#   d3d9_util        : enum-decoder retype (Vk*->Mtl*), ADAPT.
#   d3d9_names       : enum->string debug tables, KEEP (util_enum include fixed).
#   d3d9_format      : D3D9_MTL_FORMAT_MAPPING table, ADAPT (was D3D9-LEAF).
#   d3d9_shader      : PHASE-D neutral ID3D9MetalShaderBackend (air-free).
#   d3d9_fixed_function : PHASE-D FF module-set stub (keys kept, lookup stubbed).
CORE+=("$DRV"/d3d9/d3d9_util.cpp "$DRV"/d3d9/d3d9_names.cpp \
       "$DRV"/d3d9/d3d9_format.cpp "$DRV"/d3d9/d3d9_shader.cpp \
       "$DRV"/d3d9/d3d9_fixed_function.cpp)
# Phase C-3 additions (compile-verified):
#   d3d9_constant_copy    : backend-neutral constant-layout/copy (0 Vk, util-only).
#   d3d9_shader_analysis  : SM3->IR analysis pass; needs -I dxbc-spirv (sm3 parser).
#     VkImageViewType -> MtlTextureViewType; the only Vk touch. dxbc-spirv headers
#     are third-party -I includes, exempt from the driver Vulkan gate.
CORE+=("$DRV"/d3d9/d3d9_constant_copy.cpp "$DRV"/d3d9/d3d9_shader_analysis.cpp)
# Phase C-6 STAGE-2 ported .cpp bodies (promoted to fatal CORE as each compiles):
CORE+=("$DRV"/d3d9/d3d9_common_buffer.cpp "$DRV"/d3d9/d3d9_common_texture.cpp)
CORE+=("$DRV"/d3d9/d3d9_buffer.cpp "$DRV"/d3d9/d3d9_surface.cpp \
       "$DRV"/d3d9/d3d9_volume.cpp "$DRV"/d3d9/d3d9_texture.cpp \
       "$DRV"/d3d9/d3d9_cursor.cpp)
CORE+=("$DRV"/d3d9/d3d9_swvp_emu.cpp "$DRV"/d3d9/d3d9_annotation.cpp)
CORE+=("$DRV"/d3d9/d3d9_query.cpp "$DRV"/d3d9/d3d9_initializer.cpp)
CORE+=("$DRV"/d3d9/d3d9_constant_buffer.cpp)
# Phase C-8 STAGE-2 bodies (compile fatal; resolve link-frontier symbols):
#   d3d9_adapter             : caps tables onto MtlAdapter/MtlDeviceInfo (MtlGpuVendor
#                              seam add; CheckDeviceVkFormat dropped, MSAA via maxSampleCount).
#   d3d9_multithread         : LockContested via sync::spin (util/sync).
#   d3d9_stateblock          : Vulkan-clean KEEP body.
#   d3d9_vertex_declaration  : Vulkan-clean KEEP body.
# d3d9_format_helpers is now a header-only neutral stub (YUV converter dead — all
# conversion formats map to MtlFormat::Invalid); its .cpp is not compiled.
CORE+=("$DRV"/d3d9/d3d9_adapter.cpp "$DRV"/d3d9/d3d9_multithread.cpp \
       "$DRV"/d3d9/d3d9_stateblock.cpp "$DRV"/d3d9/d3d9_vertex_declaration.cpp \
       "$DRV"/d3d9/d3d9_monitor.cpp)
# Phase C-5 STAGE-1 probe: header-only TU proving the ported device-graph headers
#   (common_buffer/common_texture/resource/subresource/surface/volume/buffer/query)
#   form a closed, Vulkan-free compile unit on the Mtl seam. Grows as device.h /
#   swapchain.h land (see EXTRACTION-LOG PHASE-C5-CHECKPOINT).
CORE+=("$DRV"/d3d9/phasec_probe.cpp)

echo "=== [driver] CORE compile (${#CORE[@]} TUs) ==="
OBJS=(); PIDS=(); RC=0
compile_one() {
  local src="$1" obj="$2"
  "$MINGW" "${FLAGS[@]}" -c "$src" -o "$obj"
}
for src in "${CORE[@]}"; do
  obj="$OBJ/$(echo "${src#$DRV/}" | tr '/' '_').o"
  OBJS+=("$obj")
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[cc] ${src#$DRV/}"
    compile_one "$src" "$obj" & PIDS+=($!)
    if [ "${#PIDS[@]}" -ge 12 ]; then wait "${PIDS[@]}" || RC=1; PIDS=(); fi
  fi
done
[ "${#PIDS[@]}" -gt 0 ] && { wait "${PIDS[@]}" || RC=1; }
[ "$RC" -eq 0 ] || { echo "FATAL: CORE compile failed" >&2; exit 1; }

# sha1.c — the C SHA-1 core backing Sha1Hash (referenced by config.cpp). Compiled
# with the C compiler and folded into the archive so the link resolves it.
SHA1C_OBJ="$OBJ/util_sha1_sha1.c.o"
if [ ! -f "$SHA1C_OBJ" ] || [ "$DRV/util/sha1/sha1.c" -nt "$SHA1C_OBJ" ]; then
  echo "[cc] util/sha1/sha1.c"
  "${MINGW/g++/gcc}" -std=c11 -O1 -g -c "$DRV/util/sha1/sha1.c" -o "$SHA1C_OBJ"
fi
OBJS+=("$SHA1C_OBJ")

echo "=== [driver] archive libdriver_core.a ==="
rm -f "$BUILD/libdriver_core.a"
i686-w64-mingw32-ar rcs "$BUILD/libdriver_core.a" "${OBJS[@]}"
echo "  -> $BUILD/libdriver_core.a ($(wc -c < "$BUILD/libdriver_core.a") bytes)"

# --- D3D9-LEAF attempt (non-fatal; reports Phase-C readiness) --------------
# The remaining ADAPT graph (device/common_texture/common_buffer/adapter/query/
# surface/volume/texture/buffer/subresource/swvp/constant_*/shader_analysis) plus
# swapchain is one interconnected unit that additionally needs a large MtlDevice
# surface (resource create/allocate) not yet in the seam, and the dxbc_spv include
# path for the analysis/shader TUs. Those are the remaining Phase-C/D work — see
# EXTRACTION-LOG.md. They are attempted here only for readiness reporting.
echo "=== [driver] D3D9-LEAF attempt (non-fatal) ==="
D3D9_LEAF=()
for src in "${D3D9_LEAF[@]}"; do
  [ -f "$src" ] || { echo "  [skip] ${src#$DRV/} (not present)"; continue; }
  if "$MINGW" "${FLAGS[@]}" -c "$src" -o "$OBJ/$(echo "${src#$DRV/}" | tr '/' '_').o" 2>/tmp/driver_leaf_err.txt; then
    echo "  [ok]   ${src#$DRV/}"
  else
    echo "  [phase-c] ${src#$DRV/}: $(head -1 /tmp/driver_leaf_err.txt)"
  fi
done

# --- Vulkan gate ------------------------------------------------------------
# ZERO Vulkan rule: no Vk* types / vulkan headers in real code. Only comment/doc
# hits, the winevulkan.dll module-presence string, and the whitelisted Phase-C
# ADAPT transitive-dep header may remain. Anything else fails the build.
echo "=== [driver] Vulkan gate ==="
WL="$V2/scripts/build-driver.vulkan-whitelist"   # one grep -E pattern per line
raw="$(grep -rnE 'vulkan|Vk[A-Z]' --include='*.h' --include='*.cpp' "$DRV" || true)"
# Strip comments from each hit line (trailing // …, block /* …, and doc * lines),
# then keep only lines whose *code* portion still holds a Vk/vulkan token. This is
# the "must return only comment/doc hits" rule: Vk in a comment passes, Vk in code
# fails. The winevulkan.dll module-presence string and whitelisted Phase-C ADAPT
# transitive-dep headers are then removed as documented exceptions.
filtered="$(printf '%s\n' "$raw" | while IFS= read -r line; do
  [ -z "$line" ] && continue
  content="${line#*:}"; content="${content#*:}"   # strip path: and linenum: prefix
  # Whole-line comments (// …, block continuation * …, /* …) are doc hits -> skip.
  printf '%s' "$content" | grep -qE '^[[:space:]]*(//|\*|/\*)' && continue
  code="${content%%//*}"                            # drop trailing // comment
  code="$(printf '%s' "$code" | sed 's:/\*.*::')"   # drop block-comment start
  printf '%s' "$code" | grep -qE 'vulkan|Vk[A-Z]' && printf '%s\n' "$line" || true
done | grep -vE 'winevulkan' || true)"
if [ -f "$WL" ]; then
  while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    case "$pat" in \#*) continue ;; esac
    filtered="$(printf '%s\n' "$filtered" | grep -vE "$pat" || true)"
  done < "$WL"
fi
filtered="$(printf '%s\n' "$filtered" | grep -vE '^[[:space:]]*$' || true)"
if [ -n "$filtered" ]; then
  echo "FATAL: Vulkan gate found real-code Vk/vulkan references:" >&2
  printf '%s\n' "$filtered" >&2
  exit 1
fi
echo "  Vulkan gate: PASS (only comment/doc + whitelisted hits)"

# --- d3d9.dll link ATTEMPT (non-fatal; reports unresolved symbols) ---------
# Links libdriver_core.a + the winemetal32 import lib against the upstream d3d9.def
# exports. The COM export TUs (d3d9_main / d3d9_interface / d3d9_device …) are not
# yet ported, so this is EXPECTED to fail with undefined references until the
# device/interface graph lands — the unresolved list is the Phase-C/D worklist.
echo "=== [driver] d3d9.dll link ATTEMPT (non-fatal) ==="
# --- libdxbcsm3.a — the SM3 IR parser the analysis TUs reference -----------
# d3d9_shader_analysis references dxbc_spv::sm3::{Parser,IoMap,...}. The sm3
# parser + the ir/util .cpp it pulls compile standalone under mingw; archived so
# the linker extracts only the referenced objects (task C-4 step 6).
DXBCSM3_LIB="$BUILD/libdxbcsm3.a"
DXBCSM3_OBJ="$OBJ/dxbcsm3"
mkdir -p "$DXBCSM3_OBJ"
SM3_OBJS=(); SM3_PIDS=(); SM3_RC=0
while IFS= read -r f; do
  o="$DXBCSM3_OBJ/$(echo "${f#$DXBC/}" | tr '/' '_').o"
  SM3_OBJS+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
    "$MINGW" "${FLAGS[@]}" -c "$f" -o "$o" & SM3_PIDS+=($!)
    if [ "${#SM3_PIDS[@]}" -ge 12 ]; then wait "${SM3_PIDS[@]}" || SM3_RC=1; SM3_PIDS=(); fi
  fi
done < <(find -L "$DXBC/sm3" "$DXBC/ir" "$DXBC/util" -name '*.cpp' | sort)
[ "${#SM3_PIDS[@]}" -gt 0 ] && { wait "${SM3_PIDS[@]}" || SM3_RC=1; }
[ "$SM3_RC" -eq 0 ] || { echo "FATAL: libdxbcsm3 compile failed" >&2; exit 1; }
rm -f "$DXBCSM3_LIB"
i686-w64-mingw32-ar rcs "$DXBCSM3_LIB" "${SM3_OBJS[@]}"
echo "  libdxbcsm3.a: $DXBCSM3_LIB ($(wc -c < "$DXBCSM3_LIB") bytes, ${#SM3_OBJS[@]} objs)"

WMDEF="$V2/third_party/winemetal/winemetal32.def"
WMIMP="$BUILD/libwinemetal32.a"
D3D9DEF="$V2/DXVK-3.0-uptsream/src/d3d9/d3d9.def"
if command -v i686-w64-mingw32-dlltool >/dev/null && [ -f "$WMDEF" ]; then
  i686-w64-mingw32-dlltool -d "$WMDEF" -l "$WMIMP" 2>/dev/null \
    && echo "  winemetal32 import lib: $WMIMP ($(wc -c < "$WMIMP") bytes)"
fi
LINK_LOG="$BUILD/d3d9-link.log"
if i686-w64-mingw32-g++ -shared -o "$BUILD/d3d9.dll" \
      -Wl,--enable-stdcall-fixup \
      "$D3D9DEF" "${OBJS[@]}" "$DXBCSM3_LIB" "$WMIMP" \
      -lgdi32 -luser32 -lole32 -lsetupapi 2>"$LINK_LOG"; then
  echo "  d3d9.dll LINKED: $BUILD/d3d9.dll ($(wc -c < "$BUILD/d3d9.dll") bytes)"
else
  undef="$( { grep -oE 'undefined reference to [`'"'"'"][^'"'"'\"\`]+' "$LINK_LOG" || true; } | sed 's/.*to .//' | sort -u)"
  noexp="$( { grep -oE 'cannot export [A-Za-z0-9_]+' "$LINK_LOG" || true; } | sed 's/cannot export //' | sort -u)"
  nu="$(printf '%s\n' "$undef" | grep -c . || true)"
  ne="$(printf '%s\n' "$noexp" | grep -c . || true)"
  echo "  d3d9.dll link: INCOMPLETE — $ne unexported d3d9.def entries (device/main/interface graph),"
  echo "                 $nu undefined references (pure build-input gaps)."
  echo "    --- d3d9.def exports still undefined (need d3d9_main/interface/device port): ---"
  printf '%s\n' "$noexp" | sed 's/^/    /'
  echo "    --- undefined references: ---"
  printf '%s\n' "$undef" | head -40 | sed 's/^/    /'
fi

echo "=== [driver] build OK ==="
