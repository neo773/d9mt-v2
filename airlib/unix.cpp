#include <cstdio>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ir/ir_builder.h>
#include <ir/ir_serialize.h>

#include "../air/air_builder.h"
#include "d9mtair.h"

namespace {

using dxbc_spv::air::AirBuilder;

int compileImpl(d9mt_air_compile_params* p) {
  p->out_metallib = 0u;
  p->out_size = 0u;
  p->out_reflection = 0u;
  p->out_reflection_size = 0u;
  p->ret_status = D9MT_AIR_INTERNAL;

  dxbc_spv::ir::Builder builder;
  dxbc_spv::ir::Deserializer deserializer(
    reinterpret_cast<const uint8_t*>(uintptr_t(p->ir_data)),
    size_t(p->ir_size));

  if (!deserializer.deserialize(builder)) {
    p->ret_status = D9MT_AIR_BAD_IR;
    return 0;
  }

  AirBuilder::Options options;
  options.entryName = p->entry_name
    ? reinterpret_cast<const char*>(uintptr_t(p->entry_name))
    : "main";
  options.foldSpecConstants = (p->flags & D9MT_AIR_FLAG_FOLD_SPEC) != 0u;
  options.useArgumentBuffer = (p->flags & D9MT_AIR_FLAG_ARG_BUFFER) != 0u;

  const uint32_t* spec =
    reinterpret_cast<const uint32_t*>(uintptr_t(p->spec_data));
  for (uint32_t i = 0u; i < p->spec_count; i++)
    options.specValues.emplace(spec[2u * i], spec[2u * i + 1u]);

  std::vector<uint8_t> metallib;
  AirBuilder airBuilder(builder, options);

  try {
    metallib = airBuilder.buildMetallib();
  } catch (const std::exception& e) {
    FILE* f = fopen("/tmp/d9mtair_err.log", "a");
    if (f) { fprintf(f, "AirBuilder rejected: %s\n", e.what()); fclose(f); }
    p->ret_status = D9MT_AIR_UNSUPPORTED;
    return 0;
  }

  std::vector<d9mt_air_reflection_entry> entries;

  auto addEntry = [&entries](uint32_t kind, uint32_t index,
                             uint64_t byteSize, const std::string& name,
                             uint32_t regIndex = D9MT_AIR_NO_REG_INDEX,
                             uint32_t flags = 0u) {
    d9mt_air_reflection_entry entry = {};
    entry.kind = kind;
    entry.index = index;
    entry.byte_size = byteSize;
    entry.reg_index = regIndex;
    entry.flags = flags;
    std::strncpy(entry.name, name.c_str(), sizeof(entry.name) - 1u);
    entries.push_back(entry);
  };

  for (const auto& b : airBuilder.getBufferBindings())
    addEntry(D9MT_AIR_BIND_BUFFER,
             b.abWord >= 0 ? uint32_t(b.abWord) : b.index,
             b.byteSize, b.name,
             b.isCbv ? b.regIndex : D9MT_AIR_NO_REG_INDEX,
             b.abWord >= 0 ? D9MT_AIR_ENTRY_IN_AB : 0u);

  for (const auto& t : airBuilder.getTextureBindings()) {
    uint32_t kind = D9MT_AIR_BIND_TEXTURE_2D;
    if (t.kind == "cube") kind = D9MT_AIR_BIND_TEXTURE_CUBE;
    if (t.kind == "3d")   kind = D9MT_AIR_BIND_TEXTURE_3D;
    addEntry(kind, t.abWord >= 0 ? uint32_t(t.abWord) : t.index, 0u, t.name,
             t.index, t.abWord >= 0 ? D9MT_AIR_ENTRY_IN_AB : 0u);
  }

  for (const auto& s : airBuilder.getSamplerBindings())
    addEntry(D9MT_AIR_BIND_SAMPLER,
             s.abWord >= 0 ? uint32_t(s.abWord) : s.index, 0u, s.name,
             s.index, s.abWord >= 0 ? D9MT_AIR_ENTRY_IN_AB : 0u);

  for (const auto& s : airBuilder.getSpecBindings())
    addEntry(D9MT_AIR_BIND_SPEC_CONSTANT, s.index, 0u, s.name);

  d9mt_air_reflection_header header = {};
  header.stage = airBuilder.getStage() == dxbc_spv::ir::ShaderStage::ePixel ? 1u : 0u;
  header.render_target_count = airBuilder.getOutputInfo().renderTargetCount;
  header.writes_depth = airBuilder.getOutputInfo().writesDepth ? 1u : 0u;
  header.writes_point_size = airBuilder.getOutputInfo().writesPointSize ? 1u : 0u;
  header.uses_arg_buffer = airBuilder.getAbInfo().used ? 1u : 0u;
  header.arg_buffer_index = uint8_t(airBuilder.getAbInfo().tableIndex);
  header.entry_count = uint32_t(entries.size());

  size_t reflectionSize = sizeof(header)
    + entries.size() * sizeof(d9mt_air_reflection_entry);

  auto writeReflection = [&](void* dst) {
    std::memcpy(dst, &header, sizeof(header));
    if (!entries.empty())
      std::memcpy(static_cast<uint8_t*>(dst) + sizeof(header),
        entries.data(), entries.size() * sizeof(d9mt_air_reflection_entry));
  };

  p->out_size = metallib.size();
  p->out_reflection_size = reflectionSize;

  if (p->pe_metallib_buffer || p->pe_reflection_buffer) {
    if (metallib.size() > p->pe_metallib_capacity
     || reflectionSize > p->pe_reflection_capacity) {
      p->ret_status = D9MT_AIR_BUFFER_TOO_SMALL;
      return 0;
    }
    std::memcpy(reinterpret_cast<void*>(uintptr_t(p->pe_metallib_buffer)),
      metallib.data(), metallib.size());
    writeReflection(reinterpret_cast<void*>(uintptr_t(p->pe_reflection_buffer)));
    p->out_metallib = 0u;
    p->out_reflection = 0u;
    p->ret_status = D9MT_AIR_OK;
    return 0;
  }

  void* metallibOut = std::malloc(metallib.size());
  void* reflectionOut = std::malloc(reflectionSize);

  if (!metallibOut || !reflectionOut) {
    std::free(metallibOut);
    std::free(reflectionOut);
    p->ret_status = D9MT_AIR_INTERNAL;
    return 0;
  }

  std::memcpy(metallibOut, metallib.data(), metallib.size());
  writeReflection(reflectionOut);

  p->out_metallib = uint64_t(uintptr_t(metallibOut));
  p->out_reflection = uint64_t(uintptr_t(reflectionOut));
  p->ret_status = D9MT_AIR_OK;
  return 0;
}

}

extern "C" {

__attribute__((visibility("default")))
int d9mtair_version(d9mt_air_version_params* p) {
  p->abi_version = D9MT_AIR_ABI_VERSION;
  return 0;
}

__attribute__((visibility("default")))
int d9mtair_compile(d9mt_air_compile_params* p) {
  return compileImpl(p);
}

__attribute__((visibility("default")))
int d9mtair_free(d9mt_air_free_params* p) {
  std::free(reinterpret_cast<void*>(uintptr_t(p->ptr)));
  return 0;
}

typedef int NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void* args);

static NTSTATUS air_version_entry(void* args) {
  return d9mtair_version(static_cast<d9mt_air_version_params*>(args));
}

static NTSTATUS air_compile_entry(void* args) {
  return d9mtair_compile(static_cast<d9mt_air_compile_params*>(args));
}

static NTSTATUS air_free_entry(void* args) {
  return d9mtair_free(static_cast<d9mt_air_free_params*>(args));
}

/* `extern` is required: this is C++, where const namespace-scope objects
 * default to internal linkage even inside extern "C". */
__attribute__((visibility("default")))
extern const unixlib_entry_t __wine_unix_call_funcs[D9MT_AIR_FUNC_COUNT] = {
  air_version_entry,
  air_compile_entry,
  air_free_entry,
};

__attribute__((visibility("default")))
extern const unixlib_entry_t __wine_unix_call_wow64_funcs[D9MT_AIR_FUNC_COUNT] = {
  air_version_entry,
  air_compile_entry,
  air_free_entry,
};

}
