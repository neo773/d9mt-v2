#!/usr/bin/env bash
# Differential test runner: every corpus case is compiled through BOTH
# backends (AirBuilder -> metallib, and SpirvBuilder -> SPIRV-Cross -> MSL ->
# xcrun metal) and rendered; the two images must match within tolerance.
#
# Cases are (VS, PS) pairs. EXPECTED_FAIL lists cases for planned-but-
# unimplemented features; they report XFAIL (or XPASS once fixed).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$V2/build/air-compiler"
BIN="$V2/air/tests/bin"
OUT="$BUILD/diff"
mkdir -p "$OUT"

# case = name:vs:ps
CASES=(
  "vs_base:vs_base:ps_show"
  "vs_mvp:vs_mvp:ps_show"
  "vs_alu_mix:vs_alu_mix:ps_show"
  "vs_rcp:vs_rcp:ps_show"
  "vs_cmp:vs_cmp:ps_show"
  "vs_swizzle:vs_swizzle:ps_show"
  "vs_writemask:vs_writemask:ps_show"
  "texcoords:vs_texcoords:ps_texcoords"
  "ps_alu_mix:vs_base:ps_alu_mix"
  "ps_cmp:vs_base:ps_cmp"
  "ps_modifiers:vs_base:ps_modifiers"
  "ps_texld:vs_texcoords:ps_texld"
  "ps_texld_swiz:vs_texcoords:ps_texld_swiz"
  "ps_texldp:vs_texcoords:ps_texldp"
  "ps_texldb:vs_texcoords:ps_texldb"
  "ps_texldl:vs_texcoords:ps_texldl"
  "ps_texldd:vs_texcoords:ps_texldd"
  "vs_texldl:vs_texldl:ps_show"
  "ps_texcube:vs_texcoords:ps_texcube"
  "ps_tex3d:vs_texcoords:ps_tex3d"
  "ps_dsx_dsy:vs_texcoords:ps_dsx_dsy"
  "ps_texkill_tex:vs_texcoords:ps_texkill_tex"
  "ps_sincos:vs_base:ps_sincos"
  "vs_loop:vs_loop:ps_show"
  "vs_loop_nested:vs_loop_nested:ps_show"
  "vs_loop_al:vs_loop_al:ps_show"
  "vs_loop_break:vs_loop_break:ps_show"
  "ps_if_else:vs_base:ps_if_else"
  "ps_if_nested:vs_base:ps_if_nested"
  "vs_arel:vs_arel:ps_show"
  "vs_exp_log:vs_exp_log:ps_show"
  "ps_pow:vs_base:ps_pow"
  "ps_nrm:vs_base:ps_nrm"
  "vs_dst_lit:vs_dst_lit:ps_show"
  "ps_pp:vs_base:ps_pp"
  # @cbv: AIR side also runs the driver-style legacy-builtin->CBV lowering,
  # exercising DclCbv / DescriptorLoad / BufferLoad in AirBuilder
  "vs_mvp_cbv:vs_mvp@cbv:ps_show@cbv"
  "ps_texld_cbv:vs_texcoords@cbv:ps_texld@cbv"
  "vs_loop_al_cbv:vs_loop_al@cbv:ps_show@cbv"
  # wave 4: interface builtins
  "ps_vface:vs_base:ps_vface"
  "ps_vpos:vs_base:ps_vpos"
  "ps_odepth:vs_base:ps_odepth"
  "ps_mrt2:vs_base:ps_mrt2"
  "vs_pointsize:vs_pointsize:ps_show"
  # wave 4: spec constants (hand-built IR via --test-spec; the 4th field is
  # extra air_diff_test args — same values reach both paths)
  "ps_spec_default:vs_base:spec@spec"
  "ps_spec_red:vs_base:spec@spec:--spec 0=1"
  "ps_spec_green:vs_base:spec@spec:--spec 0=2"
)

EXPECTED_FAIL=""

bash "$V2/scripts/build-air-compiler.sh" >/dev/null || {
  echo "FATAL: build failed (run build-air-compiler.sh for details)" >&2
  exit 1
}

# compile one shader through both paths; caches on mtime
compile_shader() { # <name>[@cbv|@spec]
  local name="$1"
  local mode=""
  local input=""
  case "$name" in
    *@cbv)  mode="--lower-legacy"; name="${name%@cbv}";;
    *@spec) mode="--test-spec";    name="${name%@spec}";;
  esac
  local tag="$name${mode:+.${mode#--}}"

  if [ "$mode" != "--test-spec" ]; then
    input="$BIN/$name.bin"
    [ -f "$input" ] || { echo "missing $input (run assemble-air-tests.sh)" >&2; return 1; }
  fi

  if [ ! -f "$OUT/$tag.air.metallib" ] \
     || { [ -n "$input" ] && [ "$input" -nt "$OUT/$tag.air.metallib" ]; } \
     || [ "$BUILD/air_compiler" -nt "$OUT/$tag.air.metallib" ]; then
    "$BUILD/air_compiler" $input $mode \
      -o "$OUT/$tag.air.metallib" --manifest "$OUT/$tag.air.json" \
      > /dev/null || return 1
    "$BUILD/air_compiler" $input ${mode/--lower-legacy/} \
      --ref-msl "$OUT/$tag.ref.metal" --manifest "$OUT/$tag.ref.json" \
      > /dev/null || return 1
    xcrun -sdk macosx metal -std=metal3.0 \
      "$OUT/$tag.ref.metal" -o "$OUT/$tag.ref.metallib" || return 1
  fi
  return 0
}

art_tag() { # <name>[@cbv|@spec] -> artifact tag
  local name="$1"
  case "$name" in
    *@cbv)  echo "${name%@cbv}.lower-legacy";;
    *@spec) echo "${name%@spec}.test-spec";;
    *)      echo "$name";;
  esac
}

pass=0; fail=0; xfail=0; xpass=0
for entry in "${CASES[@]}"; do
  IFS=':' read -r name vs ps extra <<< "$entry"

  expected_fail=0
  for x in $EXPECTED_FAIL; do [ "$x" = "$name" ] && expected_fail=1; done

  if ! compile_shader "$vs" || ! compile_shader "$ps"; then
    if [ "$expected_fail" = 1 ]; then
      printf '%-24s XFAIL (compile)\n' "$name"; xfail=$((xfail+1))
    else
      printf '%-24s FAIL (compile)\n' "$name"; fail=$((fail+1))
    fi
    continue
  fi

  vstag="$(art_tag "$vs")"; pstag="$(art_tag "$ps")"
  if "$BUILD/air_diff_test" "$name" \
      "$OUT/$vstag.air.metallib" "$OUT/$vstag.air.json" \
      "$OUT/$pstag.air.metallib" "$OUT/$pstag.air.json" \
      "$OUT/$vstag.ref.metallib" "$OUT/$vstag.ref.json" \
      "$OUT/$pstag.ref.metallib" "$OUT/$pstag.ref.json" \
      ${extra:-}; then
    if [ "$expected_fail" = 1 ]; then xpass=$((xpass+1)); else pass=$((pass+1)); fi
  else
    if [ "$expected_fail" = 1 ]; then
      printf '%-24s XFAIL\n' "$name"; xfail=$((xfail+1))
    else
      fail=$((fail+1))
    fi
  fi
done

echo "----"
echo "pass=$pass fail=$fail xfail=$xfail xpass=$xpass"
[ "$fail" = 0 ]
