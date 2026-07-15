/* d9mtair: companion wine-unixlib hosting the AIR shader compiler
 * (AirBuilder + LLVM) on the unix side, per docs/rewrite/04-air-unixlib.md.
 *
 * The PE driver runs sm3 -> ir::Builder -> legalizeIr, serializes the IR
 * with dxbc-spirv's ir_serialize wire format, and calls D9MT_AIR_FUNC_COMPILE.
 * The unix side deserializes, runs AirBuilder, and returns metallib bytes
 * ready for winemetal's MTLDevice_newLibrary, plus a reflection blob.
 *
 * ABI rule (same as d9mtmetal): every field fixed-width, every pointer a
 * zero-extended uint64_t, so 32-bit and 64-bit param layouts are IDENTICAL
 * and the wow64 call table reuses the same entry points.
 */

#ifndef D9MTAIR_H
#define D9MTAIR_H

#include <stdint.h>

#define D9MT_AIR_ABI_VERSION 2u

enum d9mt_air_func {
  D9MT_AIR_FUNC_VERSION = 0,
  D9MT_AIR_FUNC_COMPILE = 1,
  D9MT_AIR_FUNC_FREE = 2,
  D9MT_AIR_FUNC_COUNT,
};

enum d9mt_air_status {
  D9MT_AIR_OK = 0,
  D9MT_AIR_BAD_IR = 1,          /* deserialization failed */
  D9MT_AIR_UNSUPPORTED = 2,     /* AirBuilder rejected an op */
  D9MT_AIR_INTERNAL = 3,
  D9MT_AIR_BUFFER_TOO_SMALL = 4, /* caller pe_*_buffer too small for result */
};

/* compile flags */
#define D9MT_AIR_FLAG_FOLD_SPEC (1u << 0)
/* Argument-buffer binding model: textures/samplers/CBVs become 8-byte
 * handle words in ONE struct bound at [[buffer(21)]]; the driver writes
 * gpuResourceID/gpuAddress words CPU-side instead of per-slot argument-
 * table calls. Reflection entries for AB members set
 * D9MT_AIR_ENTRY_IN_AB with index = the word index. */
#define D9MT_AIR_FLAG_ARG_BUFFER (1u << 1)

struct d9mt_air_version_params {
  uint32_t abi_version;   /* out: D9MT_AIR_ABI_VERSION of the unix side */
  uint32_t reserved;
};

struct d9mt_air_compile_params {
  uint64_t ir_data;        /* in:  serialized ir::Builder (ir_serialize)   */
  uint64_t ir_size;        /* in                                           */
  uint64_t entry_name;     /* in:  UTF-8, NUL-terminated                   */
  uint32_t flags;          /* in:  D9MT_AIR_FLAG_*                         */
  uint32_t spec_count;     /* in:  number of (id,value) pairs              */
  uint64_t spec_data;      /* in:  uint32_t pairs [id0,val0,id1,val1,...]  */
  uint64_t out_metallib;   /* out: unix-allocated; free via FUNC_FREE      */
  uint64_t out_size;       /* out                                          */
  uint64_t out_reflection; /* out: unix-allocated blob; free via FUNC_FREE */
  uint64_t out_reflection_size; /* out                                     */
  uint32_t ret_status;     /* out: d9mt_air_status                         */
  uint32_t reserved;
  /* CALLER-PROVIDED output buffers (32-bit PE mode): unix-allocated
   * pointers are unreachable from a 32-bit PE process, so the PE side
   * passes its OWN buffers here and the unix side memcpy's the result
   * into them (unix can write PE memory via the wine mapping). When
   * these are non-zero, out_metallib/out_reflection are left 0 (no
   * unix allocation, no FUNC_FREE needed). out_size / out_reflection_size
   * report the real sizes; ret_status = D9MT_AIR_BUFFER_TOO_SMALL if a
   * buffer is undersized. Host/loopback callers leave these 0. */
  uint64_t pe_metallib_buffer;   /* in: PE ptr for metallib bytes           */
  uint64_t pe_metallib_capacity; /* in                                      */
  uint64_t pe_reflection_buffer; /* in: PE ptr for reflection blob          */
  uint64_t pe_reflection_capacity; /* in                                    */
};

struct d9mt_air_free_params {
  uint64_t ptr;            /* in: a pointer previously returned by COMPILE */
};

/* --- reflection blob layout -----------------------------------------------
 * d9mt_air_reflection_header followed by entry_count fixed-size entries. */

enum d9mt_air_binding_kind {
  D9MT_AIR_BIND_BUFFER = 0,
  D9MT_AIR_BIND_TEXTURE_2D = 1,
  D9MT_AIR_BIND_TEXTURE_CUBE = 2,
  D9MT_AIR_BIND_TEXTURE_3D = 3,
  D9MT_AIR_BIND_SAMPLER = 4,
  D9MT_AIR_BIND_SPEC_CONSTANT = 5,
};

struct d9mt_air_reflection_header {
  uint32_t stage;             /* 0 = vertex, 1 = pixel                     */
  uint32_t render_target_count;
  uint8_t  writes_depth;
  uint8_t  writes_point_size;
  uint8_t  uses_arg_buffer;   /* 1 = AB binding model (ABI v2)             */
  uint8_t  arg_buffer_index;  /* AB [[buffer(n)]] table slot (21)          */
  uint32_t entry_count;
};

/* entry flags (ABI v2; 0 in v1 blobs via the old `reserved` field) */
#define D9MT_AIR_ENTRY_IN_AB (1u << 0) /* index = word in the argument buffer */

struct d9mt_air_reflection_entry {
  uint32_t kind;              /* d9mt_air_binding_kind                     */
  uint32_t index;             /* [[buffer/texture/sampler(index)]], spec id,
                               * or (D9MT_AIR_ENTRY_IN_AB) the AB word index */
  uint64_t byte_size;         /* buffers only                              */
  char     name[40];          /* NUL-terminated, truncated                 */
  /* For BUFFER entries that came from a DclCbv, reg_index is the DclCbv
   * register index (== the D3D9 frontend CbvIndex). For TEXTURE/SAMPLER
   * entries it is the D3D9 register (t#/s#). D9MT_AIR_NO_REG_INDEX for
   * legacy-builtin / push-data buffers and spec entries. Lets the driver
   * map a bound resource to where the shader reads it WITHOUT relying on
   * data-derived debug names (SOURCE_OF_TRUTH §4b bug 1). */
  uint32_t reg_index;
  uint32_t flags;             /* D9MT_AIR_ENTRY_*                          */
};

#define D9MT_AIR_NO_REG_INDEX 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

/* PE side: dispatches through wine's unixlib mechanism (see dll.c). */
#ifdef D9MTAIR_EXPORTS
__declspec(dllexport)
#endif
int __cdecl D9MTAir_UnixCall(unsigned int code, void *params);

#ifdef __cplusplus
}
#endif

#endif /* D9MTAIR_H */
