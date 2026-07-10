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

FLAGS=(-std=c++17 -O1 -g -DDXVK_WSI_WIN32 -Wno-unused-parameter)

# --- CORE source list (must compile) --------------------------------------
# util: everything except TUs pulling not-yet-ported deps.
#   util_unmap.cpp        -> ../dxvk/dxvk_allocator.h (Phase C)
#   com/com_guid.cpp      -> ../../d3d11 + ../../dxgi (see build-dxvk.sh note)
#   com/com_destruction_notifier.cpp -> broken ../util/thread.h path in this layout
CORE=()
for f in "$DRV"/util/*.cpp "$DRV"/util/sync/*.cpp "$DRV"/util/log/*.cpp \
         "$DRV"/util/config/*.cpp "$DRV"/util/com/*.cpp; do
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
CORE+=("$DRV"/mtl/mtl_context_stub.cpp "$DRV"/mtl/mtl_format.cpp)

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

echo "=== [driver] archive libdriver_core.a ==="
rm -f "$BUILD/libdriver_core.a"
i686-w64-mingw32-ar rcs "$BUILD/libdriver_core.a" "${OBJS[@]}"
echo "  -> $BUILD/libdriver_core.a ($(wc -c < "$BUILD/libdriver_core.a") bytes)"

# --- D3D9-LEAF attempt (non-fatal; reports Phase-C readiness) --------------
echo "=== [driver] D3D9-LEAF attempt (non-fatal) ==="
D3D9_LEAF=("$DRV"/d3d9/d3d9_names.cpp "$DRV"/d3d9/d3d9_format.cpp)
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

echo "=== [driver] build OK ==="
