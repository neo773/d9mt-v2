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

# --- Fixed-function MSL ubershaders -> metallib -> embedded byte-array header --
# The full FF pipeline is AUTO-CONVERTED from DXVK 3.0's GLSL ubershaders (kept,
# lightly v2-adapted, under driver/d3d9/shaders/) rather than hand-written:
#   glslangValidator  GLSL/vert/frag -> SPIR-V
#   spirv-cross       SPIR-V -> MSL  (--msl-version 30000, entry renamed to
#                     ffVsMain/ffFsMain since Metal reserves "main")
#   post-process      pin each CBV/push/texture/sampler [[buffer/texture/sampler(n)]]
#                     index to the v2 argument-table slots (matched in
#                     d3d9_fixed_function.cpp cbvSlotMap + flushResources):
#                       VS: SpecConsts=0 ShaderData=1 ClipPlanes=2 VertexBlend=5
#                           RenderStates=20 (kPushConstantIndex)
#                       FS: SpecConsts=0 SharedData=1 RenderStates=20
#                           t2d=texture(0) tcube=8 t3d=16 (+shadow arrays 24/32)
#                           samplers=sampler(0)
#   xcrun metal+metallib  MSL -> .metallib, embedded as g_ffVsMetallib/g_ffFsMetallib.
# Regenerated when any GLSL source (or this script) is newer than the header.
FFSHADERS="$DRV/d3d9/shaders"
FFHDR="$DRV/d3d9/d3d9_ff_metallib.h"
FFVSRC="$FFSHADERS/d3d9_fixed_function_vert.vert"
FFFSRC="$FFSHADERS/d3d9_fixed_function_frag.frag"
ff_stale=0
[ -f "$FFHDR" ] || ff_stale=1
for src in "$FFVSRC" "$FFFSRC" \
           "$FFSHADERS/d3d9_fixed_function_frag.glsl" \
           "$FFSHADERS/d3d9_fixed_function_common.glsl" "$0"; do
  [ -f "$src" ] && [ "$src" -nt "$FFHDR" ] && ff_stale=1
done
if [ -f "$FFVSRC" ] && [ "$ff_stale" = 1 ]; then
  if command -v glslangValidator >/dev/null && command -v spirv-cross >/dev/null && command -v xcrun >/dev/null; then
    echo "=== [driver] convert DXVK FF GLSL -> SPIR-V -> MSL -> metallib ==="
    FFTMP="$OBJ/ff"; mkdir -p "$FFTMP"

    glslangValidator --target-env vulkan1.3 -I"$FFSHADERS" -V "$FFVSRC" -o "$FFTMP/vs.spv"
    glslangValidator --target-env vulkan1.3 -I"$FFSHADERS" -V "$FFFSRC" -o "$FFTMP/fs.spv"

    spirv-cross --msl --msl-version 30000 --rename-entry-point main ffVsMain vert \
      "$FFTMP/vs.spv" --output "$FFTMP/vs.metal"
    spirv-cross --msl --msl-version 30000 --rename-entry-point main ffFsMain frag \
      "$FFTMP/fs.spv" --output "$FFTMP/fs.metal"

    # Pin resource slots. spirv-cross only emits [[buffer/texture/sampler(n)]] on
    # the entry-point signature; internal helpers take by-ref params (no attrs), so
    # these type-anchored rewrites are safe and localized.
    perl -0pi -e '
      s/(SpecConsts& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(0)]]/;
      s/(ShaderData& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(1)]]/;
      s/(ClipPlanes& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(2)]]/;
      s/(VertexBlendData& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(5)]]/;
      s/(RenderStates& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(20)]]/;
    ' "$FFTMP/vs.metal"
    perl -0pi -e '
      s/(SpecConsts& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(0)]]/;
      s/(SharedData& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(1)]]/;
      s/(RenderStates& \w+ )\[\[buffer\(\d+\)\]\]/${1}[[buffer(20)]]/;
      s/(> t2d )\[\[texture\(\d+\)\]\]/${1}[[texture(0)]]/;
      s/(> tcube )\[\[texture\(\d+\)\]\]/${1}[[texture(8)]]/;
      s/(> t3d )\[\[texture\(\d+\)\]\]/${1}[[texture(16)]]/;
      s/(> t2dShadow )\[\[texture\(\d+\)\]\]/${1}[[texture(24)]]/;
      s/(> tcubeShadow )\[\[texture\(\d+\)\]\]/${1}[[texture(32)]]/;
      s/(> samplers )\[\[sampler\(\d+\)\]\]/${1}[[sampler(0)]]/;
    ' "$FFTMP/fs.metal"

    xcrun -sdk macosx metal -O2 -c "$FFTMP/vs.metal" -o "$FFTMP/vs.air"
    xcrun -sdk macosx metal -O2 -c "$FFTMP/fs.metal" -o "$FFTMP/fs.air"
    xcrun -sdk macosx metallib "$FFTMP/vs.air" -o "$FFTMP/vs.metallib"
    xcrun -sdk macosx metallib "$FFTMP/fs.air" -o "$FFTMP/fs.metallib"
    {
      echo '// GENERATED by scripts/build-driver.sh: DXVK FF GLSL -> spirv-cross MSL -> metallib. DO NOT EDIT.'
      echo '#pragma once'
      echo 'namespace dxvk {'
      echo 'static const unsigned char g_ffVsMetallib[] = {'
      xxd -i < "$FFTMP/vs.metallib"
      echo '};'
      echo 'static const unsigned char g_ffFsMetallib[] = {'
      xxd -i < "$FFTMP/fs.metallib"
      echo '};'
      echo 'static const unsigned long g_ffVsMetallibSize = sizeof(g_ffVsMetallib);'
      echo 'static const unsigned long g_ffFsMetallibSize = sizeof(g_ffFsMetallib);'
      echo '}'
    } > "$FFHDR"
    echo "  -> $FFHDR ($(wc -c < "$FFHDR") bytes)"
  else
    echo "  [warn] glslang/spirv-cross/xcrun missing; keeping existing $FFHDR" >&2
    [ -f "$FFHDR" ] || { echo "FATAL: FF toolchain missing and no $FFHDR" >&2; exit 1; }
  fi
fi

# --- Meta-blit MSL -> metallib -> embedded byte-array header -------------------
# blitTextureView's sampled fullscreen-triangle pipeline (mtl_context_transfer.cpp,
# mtl_context.h §6.6). Hand-authored MSL with entry points blitVsMain/blitFsMain
# (Metal reserves "main"), embedded as g_blitMetallib. Regenerated when the
# source (or this script) is newer than the header.
BLITSRC="$DRV/backend/shaders/mtl_blit.metal"
BLITHDR="$DRV/backend/mtl_blit_metallib.h"
blit_stale=0
[ -f "$BLITHDR" ] || blit_stale=1
for src in "$BLITSRC" "$0"; do
  [ -f "$src" ] && [ "$src" -nt "$BLITHDR" ] && blit_stale=1
done
if [ -f "$BLITSRC" ] && [ "$blit_stale" = 1 ]; then
  if command -v xcrun >/dev/null; then
    echo "=== [driver] compile meta-blit MSL -> metallib ==="
    BLITTMP="$OBJ/blit"; mkdir -p "$BLITTMP"
    xcrun -sdk macosx metal -O2 -c "$BLITSRC" -o "$BLITTMP/blit.air"
    xcrun -sdk macosx metallib "$BLITTMP/blit.air" -o "$BLITTMP/blit.metallib"
    {
      echo '// GENERATED by scripts/build-driver.sh: backend/shaders/mtl_blit.metal -> metallib. DO NOT EDIT.'
      echo '#pragma once'
      echo 'namespace dxvk {'
      echo 'static const unsigned char g_blitMetallib[] = {'
      xxd -i < "$BLITTMP/blit.metallib"
      echo '};'
      echo 'static const unsigned long g_blitMetallibSize = sizeof(g_blitMetallib);'
      echo '}'
    } > "$BLITHDR"
    echo "  -> $BLITHDR ($(wc -c < "$BLITHDR") bytes)"
  else
    echo "  [warn] xcrun missing; keeping existing $BLITHDR" >&2
    [ -f "$BLITHDR" ] || { echo "FATAL: metal toolchain missing and no $BLITHDR" >&2; exit 1; }
  fi
fi

MINGW="${MINGW:-i686-w64-mingw32-g++}"
command -v "$MINGW" >/dev/null || { echo "FATAL: need $MINGW" >&2; exit 1; }

# -O2: the release optimization level (upstream DXVK ships mingw PE builds at
# -O2). The driver shipped at -O1 until 2026-07-17 — under Rosetta translation
# un-optimized code pays double, so this is a whole-driver speedup.
# -fno-strict-aliasing: the PE<->unix boundary structs and wmtcmd stream do
# type punning that -O2's aliasing assumptions would otherwise miscompile.
FLAGS=(-std=c++17 -O2 -fno-strict-aliasing -g -DDXVK_WSI_WIN32 -Wno-unused-parameter -msse -msse2 -mfpmath=sse)

# --- PROFILE_INSTRUMENT=1 — per-function CPU profiler build mode -------------
# Compiles ONLY the backend hot-path TUs (the CS-thread draw loop) with GCC
# -finstrument-functions (mingw-GCC has no clang -after-inlining variant) and
# adds the hook TU backend/mtl_instrument.cpp (itself NEVER instrumented).
# Instrumented objects get a distinct .prof.o name so the normal build's cached
# objects — and its link input list — are byte-for-byte unaffected when the
# flag is unset. Runtime is additionally gated on D9MT_PROFILE_INSTRUMENT=1 in
# the environment (hooks return immediately otherwise).
PROFILE_INSTRUMENT="${PROFILE_INSTRUMENT:-0}"
INSTRUMENT_TUS=" mtl_context_record.cpp mtl_context_transfer.cpp mtl_pso_cache.cpp mtl_submission.cpp mtl_device_impl.cpp mtl_ring.cpp mtl_residency.cpp "
# GCC instruments pre-inline, so trivial getters (std::array::operator[],
# Rc<>::operator->, MtlBufferSlice::defined, ...) each cost two hook calls that
# land in their CALLER's self time (400k+ calls/10s observed in GTA IV). The
# exclude list (substring match, mangled + demangled) is GCC's stand-in for
# clang's -finstrument-functions-after-inlining.
INSTRUMENT_EXCLUDE="std::,dxvk::Rc,_ZNSt,_ZNKSt,definedEv,MtlBufferSlice"
[ "$PROFILE_INSTRUMENT" = 1 ] && echo "=== [driver] PROFILE_INSTRUMENT=1: instrumenting backend hot TUs ==="
# dxbc-spirv (sm3 IR parser) include root — the SM3->IR analysis passes
# (d3d9_shader_analysis / real d3d9_shader) include <sm3/...> and <util/...> from
# it. These are -I includes (third-party headers), exempt from the driver Vulkan
# gate, which scans driver/ only. third_party/dxbc-spirv is a symlink to the
# populated DXVK-3.0-uptsream/subprojects/dxbc-spirv submodule.
DXBC="$V2/third_party/dxbc-spirv"
FLAGS+=(-I"$DXBC")
# airlib/ holds the pure-C AIR compile ABI header (d9mtair.h) that the real
# shader module (d3d9_shader_compiler.cpp) marshals into. Header-only include;
# the D9MTAir* thunk symbols resolve through the winemetal import lib below.
FLAGS+=(-I"$V2/airlib")

# --- CORE source list (must compile) --------------------------------------
# util: everything except TUs pulling not-yet-ported deps.
#   util_unmap.cpp        -> ../dxvk/dxvk_allocator.h (Phase C)
#   com/com_guid.cpp      -> ../../d3d11 + ../../dxgi (see build-dxvk.sh note)
#   com/com_destruction_notifier.cpp -> broken ../util/thread.h path in this layout
CORE=()
for f in "$DRV"/util/*.cpp "$DRV"/util/sync/*.cpp "$DRV"/util/log/*.cpp \
         "$DRV"/util/config/*.cpp "$DRV"/util/com/*.cpp "$DRV"/util/sha1/*.cpp; do
  case "$f" in
    */com_guid.cpp|*/com_destruction_notifier.cpp) continue ;;
  esac
  CORE+=("$f")
done
# wsi: monitor/window only (wsi_edid.cpp needs libdisplay-info — Phase C).
CORE+=("$DRV"/wsi/wsi_platform.cpp "$DRV"/wsi/win32/wsi_monitor_win32.cpp \
       "$DRV"/wsi/win32/wsi_platform_win32.cpp "$DRV"/wsi/win32/wsi_window_win32.cpp)
# cs + mtl seam.
CORE+=("$DRV"/cs/dxvk_cs.cpp)
# dxvk_allocator: the page/pool allocator backing MemoryFilePool (util_unmap.cpp).
# Vulkan-free (pure bit/page math); copied from upstream to the driver tree.
CORE+=("$DRV"/dxvk/dxvk_allocator.cpp)
CORE+=("$DRV"/mtl/mtl_context_stub.cpp "$DRV"/mtl/mtl_format.cpp \
       "$DRV"/mtl/mtl_presenter_stub.cpp "$DRV"/mtl/mtl_device_stub.cpp)
# backend/ — the real winemetal-backed Metal 3 backend (MtlContextImpl +
# device/ring/submission/pso-cache/presenter). These override the seam-base
# stub methods; the stubs still supply the MtlContext/MtlDevice base vtable
# bodies for the few methods the impl leaves to the base. mtl_pso_cache.cpp
# includes v2/bridge/winemetal.h (superset of third_party's, shared
# _WINEMETAL_H guard) for the vertex-descriptor pipeline entry (code 153,
# MTLDevice_newRenderPipelineStateWithVertexDescriptor); that import symbol
# lives in the bridge thunks and shows as an expected undefined in the
# non-fatal d3d9.dll link until a bridge import lib is wired for shipping.
CORE+=("$DRV"/backend/mtl_context_record.cpp "$DRV"/backend/mtl_context_transfer.cpp \
       "$DRV"/backend/mtl_pso_cache.cpp "$DRV"/backend/mtl_presenter_impl.cpp \
       "$DRV"/backend/mtl_device_impl.cpp "$DRV"/backend/mtl_ring.cpp \
       "$DRV"/backend/mtl_submission.cpp "$DRV"/backend/mtl_residency.cpp \
       "$DRV"/backend/mtl_wire_context.cpp)
# PROFILE_INSTRUMENT: the __cyg_profile_func_enter/exit hook TU (not instrumented).
[ "$PROFILE_INSTRUMENT" = 1 ] && CORE+=("$DRV"/backend/mtl_instrument.cpp)
# d3d9 — Phase-C ported ADAPT retypes + Phase-D neutral stubs that compile now.
#   d3d9_util        : enum-decoder retype (Vk*->Mtl*), ADAPT.
#   d3d9_names       : enum->string debug tables, KEEP (util_enum include fixed).
#   d3d9_format      : D3D9_MTL_FORMAT_MAPPING table, ADAPT (was D3D9-LEAF).
#   d3d9_shader      : PHASE-D neutral ID3D9MetalShaderBackend (air-free).
#   d3d9_fixed_function : PHASE-D FF module-set stub (keys kept, lookup stubbed).
CORE+=("$DRV"/d3d9/d3d9_util.cpp "$DRV"/d3d9/d3d9_names.cpp \
       "$DRV"/d3d9/d3d9_format.cpp "$DRV"/d3d9/d3d9_shader.cpp \
       "$DRV"/d3d9/d3d9_shader_compiler.cpp \
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
# Phase C-9 STAGE-2: the device linchpin (semantic port onto the Mtl seam) + the
# swapchain (MtlPresenter) + interface/main (the 20 d3d9.def COM exports).
CORE+=("$DRV"/d3d9/d3d9_device.cpp)
# Phase C-10 link-frontier resolvers (promoted to fatal CORE):
#   d3d9_state       : D3D9State<dynamic_item>/<static_item> ctor/dtor instantiation.
#   d3d9_options     : D3D9Options ctor ported off DxvkDevice->MtlDevice (VK_DRIVER_ID
#                      driver-quirk probe neutered — no Metal analogue).
#   d3d9_bridge      : DxvkD3D8Bridge/InterfaceBridge (VkOffset3D->MtlOrigin).
#   d3d9_window      : HookWindowProc/ResetWindowProc/D3D9WindowMessageFilter/
#                      ActivateFocusWindow used by swapchain + interface.
#   d3d9_com_guid    : minimal local logQueryInterfaceError/operator<<(GUID) firewall
#                      (util/com/com_guid.cpp stays excluded — pulls d3d11/dxgi).
CORE+=("$DRV"/d3d9/d3d9_state.cpp "$DRV"/d3d9/d3d9_options.cpp \
       "$DRV"/d3d9/d3d9_bridge.cpp "$DRV"/d3d9/d3d9_window.cpp \
       "$DRV"/d3d9/d3d9_com_guid.cpp "$DRV"/d3d9/d3d9_shader_validator.cpp)
# swapchain (MtlPresenter) + interface/main (the 20 d3d9.def COM exports).
[ -f "$DRV"/d3d9/d3d9_swapchain.cpp ] && CORE+=("$DRV"/d3d9/d3d9_swapchain.cpp)
[ -f "$DRV"/d3d9/d3d9_interface.cpp ] && CORE+=("$DRV"/d3d9/d3d9_interface.cpp)
[ -f "$DRV"/d3d9/d3d9_main.cpp ] && CORE+=("$DRV"/d3d9/d3d9_main.cpp)
# Phase C-5 STAGE-1 probe: header-only TU proving the ported device-graph headers
#   (common_buffer/common_texture/resource/subresource/surface/volume/buffer/query)
#   form a closed, Vulkan-free compile unit on the Mtl seam. Grows as device.h /
#   swapchain.h land (see EXTRACTION-LOG PHASE-C5-CHECKPOINT).
CORE+=("$DRV"/d3d9/phasec_probe.cpp)

echo "=== [driver] CORE compile (${#CORE[@]} TUs) ==="
OBJS=(); PIDS=(); RC=0
compile_one() {
  local src="$1" obj="$2"; shift 2
  "$MINGW" "${FLAGS[@]}" "$@" -c "$src" -o "$obj"
}
for src in "${CORE[@]}"; do
  psuffix=""; pflag=""
  if [ "$PROFILE_INSTRUMENT" = 1 ]; then
    case "$INSTRUMENT_TUS" in
      *" $(basename "$src") "*) psuffix=".prof"; pflag="-finstrument-functions -finstrument-functions-exclude-function-list=$INSTRUMENT_EXCLUDE" ;;
    esac
  fi
  obj="$OBJ/$(echo "${src#$DRV/}" | tr '/' '_')$psuffix.o"
  OBJS+=("$obj")
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[cc] ${src#$DRV/}${pflag:+ ($pflag)}"
    compile_one "$src" "$obj" $pflag & PIDS+=($!)
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
  "${MINGW/g++/gcc}" -std=c11 -O2 -g -c "$DRV/util/sha1/sha1.c" -o "$SHA1C_OBJ"
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
for src in "${D3D9_LEAF[@]+"${D3D9_LEAF[@]}"}"; do
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

# winemetal import lib: prefer the bridge-generated def (build-bridge.sh emits the
# full winemetal.def incl. MTLDevice_newRenderPipelineStateWithVertexDescriptor,
# the vertex-descriptor pipeline entry the pso-cache imports). Fall back to the
# older third_party def only if the bridge hasn't been built.
WMDEF="$V2/build/bridge/winemetal.def"
[ -f "$WMDEF" ] || WMDEF="$V2/third_party/winemetal/winemetal32.def"
WMIMP="$BUILD/libwinemetal32.a"
D3D9DEF="$V2/DXVK-3.0-uptsream/src/d3d9/d3d9.def"
if command -v i686-w64-mingw32-dlltool >/dev/null && [ -f "$WMDEF" ]; then
  # The real shader module (d3d9_shader_compiler.cpp) imports the d9mtair AIR
  # compile thunks (D9MTAirCompile/Free/Version). The bridge-generated def
  # already carries them; when we fall back to the older third_party def it does
  # not, so union in the thunk names from bridge/d9mtair_thunks.h (same grep the
  # bridge uses) before generating the import lib. Keeps the driver link FATAL
  # green whether or not build-bridge.sh has been run.
  COMBDEF="$BUILD/winemetal-driver.def"
  cp "$WMDEF" "$COMBDEF"
  if ! grep -q 'D9MTAirCompile' "$COMBDEF"; then
    /usr/bin/grep -hoE '[A-Za-z_][A-Za-z0-9_]*\(' "$V2/bridge/d9mtair_thunks.h" \
      | sed -E 's/\($//; s/^/  /' >> "$COMBDEF"
  fi
  i686-w64-mingw32-dlltool -d "$COMBDEF" -l "$WMIMP" --dllname winemetal.dll 2>/dev/null \
    && echo "  winemetal32 import lib: $WMIMP ($(wc -c < "$WMIMP") bytes, from ${WMDEF#$V2/} + d9mtair thunks)"
fi
LINK_LOG="$BUILD/d3d9-link.log"
# Phase C-10: the link is now FATAL. The full CORE graph (device/swapchain/
# interface/main + seam resolvers) links d3d9.dll with all 20 d3d9.def exports;
# the winemetal MTLDevice_* imports resolve via the bridge import lib above.
if i686-w64-mingw32-g++ -shared -static -static-libgcc -static-libstdc++ -o "$BUILD/d3d9.dll" \
      -Wl,--enable-stdcall-fixup \
      "$D3D9DEF" "${OBJS[@]}" "$DXBCSM3_LIB" "$WMIMP" \
      -lgdi32 -luser32 -lole32 -lsetupapi 2>"$LINK_LOG"; then
  echo "  d3d9.dll LINKED: $BUILD/d3d9.dll ($(wc -c < "$BUILD/d3d9.dll") bytes)"
  nexp="$(i686-w64-mingw32-objdump -p "$BUILD/d3d9.dll" | grep -cE 'Export RVA' || true)"
  echo "  d3d9.dll exports (Export Address Table RVA entries): $nexp"
else
  undef="$( { grep -oE 'undefined reference to [`'"'"'"][^'"'"'\"\`]+' "$LINK_LOG" || true; } | sed 's/.*to .//' | sort -u)"
  noexp="$( { grep -oE 'cannot export [A-Za-z0-9_]+' "$LINK_LOG" || true; } | sed 's/cannot export //' | sort -u)"
  nu="$(printf '%s\n' "$undef" | grep -c . || true)"
  ne="$(printf '%s\n' "$noexp" | grep -c . || true)"
  echo "  d3d9.dll link: FAILED — $ne unexported d3d9.def entries, $nu undefined references." >&2
  echo "    --- d3d9.def exports still undefined: ---" >&2
  printf '%s\n' "$noexp" | sed 's/^/    /' >&2
  echo "    --- undefined references: ---" >&2
  printf '%s\n' "$undef" | head -40 | sed 's/^/    /' >&2
  echo "FATAL: d3d9.dll link failed" >&2
  exit 1
fi

echo "=== [driver] build OK ==="

# --- render server (Phase S1, docs/RENDER_SERVER_SPEC.md "Build") -----------
# NON-FATAL by design: the driver build result above must not change on
# machines without the server tree or when the server build/test gate fails.
if [ -d "$V2/server" ] && [ -f "$V2/scripts/build-server.sh" ]; then
  echo "=== [driver] render server (non-fatal) ==="
  if bash "$V2/scripts/build-server.sh"; then
    echo "=== [driver] render server build OK ==="
  else
    echo "WARNING: build-server.sh failed (non-fatal; driver build unaffected)" >&2
  fi
else
  echo "[driver] server/ not present; skipping render-server build"
fi
