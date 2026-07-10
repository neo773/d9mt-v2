# 04 — AIR Unixlib Architecture (PE ↔ unix shader compilation)

Status: DESIGN (2026-07-11). Depends on: 03-shader-path.md, the v2/air library
(complete, 46-case differential suite green).

## The insight

LLVM never enters the PE DLL. DXMT's own recipe builds LLVM as **x86_64
Mach-O** because the LLVM-dependent code runs on Wine's *unix side*. We adopt
the same split, which our v1/v2 `d9mtmetal` companion already proves out:

```
 PE side (i686 / x86_64 PE, mingw)          unix side (x86_64 Mach-O dylib)
 ─────────────────────────────────          ────────────────────────────────
 d3d9.dll                                    d9mtair.so (new)
   d3d9 state tracking (v2/driver)             AirBuilder (v2/air)
   sm3::Converter + legalizeIr                 LLVM 15 (static, Mach-O)
   Phase-A/B shader passes                     metallib writer
   VS↔PS linkage resolution                    ── returns metallib bytes ──
   ir_serialize ──── unixlib ABI ────▶
                                             d9mtmetal.so (existing pattern)
                                               MTLDevice_newLibraryWithData
```

- dxbc-spirv (sm3 + ir + passes) compiles clean under mingw — proven by the
  old v2 build. It runs on the PE side where the d3d9 frontend lives.
- `ir_serialize.{h,cpp}` (upstream dxbc-spirv) is the wire format across the
  boundary — no custom protocol, no version skew (same vendored tree compiled
  on both sides).
- The unix side deserializes, runs `AirBuilder`, wraps the metallib, and
  returns bytes. The PE side hands them to winemetal's newLibraryWithData.

## Why not LLVM-in-PE (rejected)

- mingw LLVM static build is hours-long, fattens d3d9.dll by tens of MB, and
  the whole thing executes under Rosetta x86 emulation.
- Unix-side LLVM is a lean native-recipe build (no codegen targets, no tools:
  Core/BitWriter/Support only — `scripts/build-llvm-unixside.sh`), and the
  compile work runs at Rosetta x86_64 speed *outside* the game's PE space,
  callable from a background shader-compile thread.

## ABI sketch (mirrors d9mtmetal conventions)

All boundary pointers are `uint64_t` (wow64-safe, matching the existing
winemetal/d9mtmetal ABI style — memory: arm64-ready ABI decisions).

```c
/* PE → unix */
struct d9mt_air_compile_params {
  uint64_t ir_data;        /* serialized ir::Builder (ir_serialize) */
  uint64_t ir_size;
  uint64_t linkage_data;   /* serialized linkage/spec metadata block */
  uint64_t linkage_size;
  uint64_t out_metallib;   /* unix-allocated, freed via d9mt_air_free */
  uint64_t out_size;
  uint32_t stage;          /* vertex / fragment */
  uint32_t flags;          /* fold_spec_constants, debug_names, ... */
};
D9MT_AIR_FUNC_COMPILE, D9MT_AIR_FUNC_FREE, D9MT_AIR_FUNC_VERSION
```

Reflection (binding slots, spec-constant ids) travels back as a small
fixed struct appended to the metallib buffer — the PE side already knows
most of it from its own IoMap; only AIR-assigned indices return.

## Caching

- Phase-A IR cache: PE side, keyed bytecode-hash + options (per doc 03).
- Metallib cache: replaced by MTL4PipelineDataSetSerializer at the PSO layer
  (doc 00); no on-disk metallib cache of our own → the poisoned-toolchain
  cache class stays dead.

## Testing

The host diff harness stays authoritative for codegen. The unixlib gets a
loopback test: PE-side serialize → unix-side compile (both built from the
same tree) → byte-compare against direct host AirBuilder output for the
46-case corpus.

## Bridge policy (user directive, 2026-07-11)

winemetal and the AIR bridge follow DXMT's design verbatim — DXMT is the gold
standard for the Wine↔Metal seam and we do not reinvent it:

1. One bridge module, like DXMT ships one: the final shape folds d9mtair's
   thunk table into the winemetal module (DXMT's airconv_thunks.c precedent),
   not a permanent second DLL pair. Standalone d9mtair remains only as the
   loopback-test stepping stone until the fold.
2. Extensions are additive patches in DXMT's own style: new WMT*Info struct +
   WINEMETAL_API entry + thunk pair + wow64 twin, formatted so `diff` against
   upstream DXMT stays a clean cherry-pickable patch. First (and currently
   only) planned addition: vertex-descriptor-capable render-PSO creation.
3. Never wrap, re-abstract, or restyle their ABI: obj_handle_t model, wmtcmd
   command batching, struct-param conventions, __wine_unix_call dispatch all
   stay byte-compatible with upstream.
4. LGPL boundary: the bridge stays its own dll/so; driver (MIT) links it
   dynamically; our bridge patches get published.
