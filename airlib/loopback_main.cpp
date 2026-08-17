#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <dlfcn.h>

#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>

#include "d9mtair.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <d9mtair.so> <sm3.bin> <out.metallib>\n", argv[0]);
    return 2;
  }

  std::ifstream f(argv[2], std::ios::binary | std::ios::ate);
  if (!f) {
    fprintf(stderr, "cannot read %s\n", argv[2]);
    return 1;
  }
  std::vector<uint8_t> bytecode(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(bytecode.data()),
    std::streamsize(bytecode.size()));

  dxbc_spv::util::ByteReader reader(bytecode.data(), bytecode.size());

  dxbc_spv::sm3::Converter::Options convOptions = {};
  convOptions.name = "main";
  convOptions.includeDebugNames = true;

  dxbc_spv::ir::Builder builder;
  dxbc_spv::sm3::Converter converter(reader, convOptions);
  if (!converter.convertShader(builder)) {
    fprintf(stderr, "FATAL: sm3 conversion failed\n");
    return 1;
  }

  dxbc_spv::ir::CompileOptions compileOptions = {};
  dxbc_spv::ir::legalizeIr(builder, compileOptions);

  dxbc_spv::ir::Serializer serializer(builder);
  std::vector<uint8_t> irBlob(serializer.computeSerializedSize());
  if (!serializer.serialize(irBlob.data(), irBlob.size())) {
    fprintf(stderr, "FATAL: ir serialization failed\n");
    return 1;
  }

  void* dylib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!dylib) {
    fprintf(stderr, "FATAL: dlopen: %s\n", dlerror());
    return 1;
  }

  auto compileFn = reinterpret_cast<int(*)(d9mt_air_compile_params*)>(
    dlsym(dylib, "d9mtair_compile"));
  auto freeFn = reinterpret_cast<int(*)(d9mt_air_free_params*)>(
    dlsym(dylib, "d9mtair_free"));
  auto versionFn = reinterpret_cast<int(*)(d9mt_air_version_params*)>(
    dlsym(dylib, "d9mtair_version"));

  if (!compileFn || !freeFn || !versionFn) {
    fprintf(stderr, "FATAL: missing dylib symbols\n");
    return 1;
  }

  d9mt_air_version_params version = {};
  versionFn(&version);
  if (version.abi_version != D9MT_AIR_ABI_VERSION) {
    fprintf(stderr, "FATAL: ABI mismatch: %u\n", version.abi_version);
    return 1;
  }

  const char entryName[] = "main";

  d9mt_air_compile_params params = {};
  params.ir_data = uint64_t(uintptr_t(irBlob.data()));
  params.ir_size = irBlob.size();
  params.entry_name = uint64_t(uintptr_t(entryName));

  compileFn(&params);

  if (params.ret_status != D9MT_AIR_OK) {
    fprintf(stderr, "FATAL: compile status %u\n", params.ret_status);
    return 1;
  }

  std::ofstream out(argv[3], std::ios::binary);
  out.write(reinterpret_cast<const char*>(uintptr_t(params.out_metallib)),
    std::streamsize(params.out_size));
  out.close();

  auto* header = reinterpret_cast<const d9mt_air_reflection_header*>(
    uintptr_t(params.out_reflection));
  size_t expected = sizeof(*header)
    + header->entry_count * sizeof(d9mt_air_reflection_entry);
  if (expected != params.out_reflection_size) {
    fprintf(stderr, "FATAL: reflection blob size mismatch\n");
    return 1;
  }

  uint32_t entryCount = header->entry_count;

  d9mt_air_free_params freeParams = {};
  freeParams.ptr = params.out_metallib;
  freeFn(&freeParams);
  freeParams.ptr = params.out_reflection;
  freeFn(&freeParams);

  printf("ok %s (%llu bytes, %u reflection entries)\n",
    argv[3], (unsigned long long)params.out_size, entryCount);
  return 0;
}
