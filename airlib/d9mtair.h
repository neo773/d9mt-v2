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
  D9MT_AIR_BAD_IR = 1,
  D9MT_AIR_UNSUPPORTED = 2,
  D9MT_AIR_INTERNAL = 3,
  D9MT_AIR_BUFFER_TOO_SMALL = 4,
};

#define D9MT_AIR_FLAG_FOLD_SPEC (1u << 0)
#define D9MT_AIR_FLAG_ARG_BUFFER (1u << 1)

struct d9mt_air_version_params {
  uint32_t abi_version;
  uint32_t reserved;
};

struct d9mt_air_compile_params {
  uint64_t ir_data;
  uint64_t ir_size;
  uint64_t entry_name;
  uint32_t flags;
  uint32_t spec_count;
  uint64_t spec_data;
  uint64_t out_metallib;
  uint64_t out_size;
  uint64_t out_reflection;
  uint64_t out_reflection_size;
  uint32_t ret_status;
  uint32_t reserved;
  /* pe_* buffers: a unix-allocated pointer is unreachable from a 32-bit
   * PE process, so 32-bit callers supply their own buffers here instead. */
  uint64_t pe_metallib_buffer;
  uint64_t pe_metallib_capacity;
  uint64_t pe_reflection_buffer;
  uint64_t pe_reflection_capacity;
};

struct d9mt_air_free_params {
  uint64_t ptr;
};

enum d9mt_air_binding_kind {
  D9MT_AIR_BIND_BUFFER = 0,
  D9MT_AIR_BIND_TEXTURE_2D = 1,
  D9MT_AIR_BIND_TEXTURE_CUBE = 2,
  D9MT_AIR_BIND_TEXTURE_3D = 3,
  D9MT_AIR_BIND_SAMPLER = 4,
  D9MT_AIR_BIND_SPEC_CONSTANT = 5,
};

struct d9mt_air_reflection_header {
  uint32_t stage;
  uint32_t render_target_count;
  uint8_t  writes_depth;
  uint8_t  writes_point_size;
  uint8_t  uses_arg_buffer;
  uint8_t  arg_buffer_index;
  uint32_t entry_count;
};

#define D9MT_AIR_ENTRY_IN_AB (1u << 0)

struct d9mt_air_reflection_entry {
  uint32_t kind;
  uint32_t index;
  uint64_t byte_size;
  char     name[40];
  uint32_t reg_index;
  uint32_t flags;
};

#define D9MT_AIR_NO_REG_INDEX 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

#ifdef D9MTAIR_EXPORTS
__declspec(dllexport)
#endif
int __cdecl D9MTAir_UnixCall(unsigned int code, void *params);

#ifdef __cplusplus
}
#endif

#endif
