#!/usr/bin/env bash
# Loopback gate for the d9mtair unixlib: for every corpus case, the PE path
# (serialize IR -> dylib compile) must produce a metallib whose LLVM module
# is IDENTICAL to air_compiler's direct host output modulo internal helper
# symbol names.
#
# Why not raw byte-compare: AirBuilder names internal helper functions by
# their IR SSA def id ("fn40"); ir_serialize densely remaps SSA ids, so the
# round-tripped module has "fn55" for the same helper. The names are
# internal linkage — semantically irrelevant. The gate therefore extracts
# the bitcode from both containers, disassembles, normalizes `@fnN` tokens
# and the ModuleID line, and requires an empty diff. (Making AirBuilder
# name helpers by walk order would restore true byte-identity — noted as a
# possible follow-up in v2/air.)
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/airlib"
REF="$OUT/ref"
LOOP="$OUT/loop"
mkdir -p "$REF" "$LOOP"

LLVM_DIS="${LLVM_DIS:-/opt/homebrew/opt/llvm@15/bin/llvm-dis}"
[ -x "$LLVM_DIS" ] || { echo "FATAL: need llvm-dis (brew llvm@15)" >&2; exit 1; }

AIRC="$V2/build/air-compiler/air_compiler"
[ -x "$AIRC" ] || bash "$V2/scripts/build-air-compiler.sh" >/dev/null
bash "$V2/scripts/build-d9mtair.sh" >/dev/null 2>&1

extract_normalized() { # metallib out.ll
  python3 - "$1" /tmp/airlib_gate.bc <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', d, 0x48)
open(sys.argv[2], 'wb').write(d[off:off+size])
EOF
  "$LLVM_DIS" /tmp/airlib_gate.bc -o - \
    | sed -E 's/@fn[0-9]+/@fnX/g; /^; ModuleID/d' > "$2"
}

pass=0; fail=0
for bin in "$V2"/air/tests/bin/*.bin; do
  name="$(basename "$bin" .bin)"
  "$AIRC" "$bin" -o "$REF/$name.metallib" >/dev/null
  if ! "$OUT/airlib_loopback" "$OUT/d9mtair.so" "$bin" "$LOOP/$name.metallib" >/dev/null; then
    printf '%-24s FAIL (loopback compile)\n' "$name"; fail=$((fail+1)); continue
  fi
  extract_normalized "$REF/$name.metallib" /tmp/airlib_ref.ll
  extract_normalized "$LOOP/$name.metallib" /tmp/airlib_loop.ll
  if diff -q /tmp/airlib_ref.ll /tmp/airlib_loop.ll >/dev/null; then
    printf '%-24s PASS (module-identical)\n' "$name"; pass=$((pass+1))
  else
    printf '%-24s FAIL (module diff)\n' "$name"
    fail=$((fail+1))
  fi
done
echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
