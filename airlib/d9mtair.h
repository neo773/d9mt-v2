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

#define D9MT_AIR_ABI_VERSION 1u

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
};

/* compile flags */
#define D9MT_AIR_FLAG_FOLD_SPEC (1u << 0)

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
  uint8_t  reserved0[2];
  uint32_t entry_count;
};

struct d9mt_air_reflection_entry {
  uint32_t kind;              /* d9mt_air_binding_kind                     */
  uint32_t index;             /* [[buffer/texture/sampler(index)]] or spec id */
  uint64_t byte_size;         /* buffers only                              */
  char     name[40];          /* NUL-terminated, truncated                 */
};

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
