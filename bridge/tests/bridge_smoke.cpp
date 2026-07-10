/*
 * bridge_smoke: host-side gate for the winemetal bridge fork.
 *
 * 1. dlopens build/bridge/winemetal.so, indexes __wine_unix_call_funcs.
 * 2. WMTCopyAllDevices -> device.
 * 3. Loads the AIR-generated vs/ps metallibs, creates MTLFunctions, and
 *    builds a render PSO through the NEW vertex-descriptor entry (code 153).
 * 4. AIR-thunk parity: compiles a serialized IR blob through the bridge's
 *    d9mtair entry AND through the standalone d9mtair.so; byte-compares.
 *
 * x86_64 host binary (the .so is wine-unix-side x86_64; runs under Rosetta).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <vector>

#include "../winemetal.h"
#include "../winemetal_thunks.h"
#include "../../airlib/d9mtair.h"

/* front half: SM3 bytecode -> legalized serialized IR (same as loopback) */
#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>

/* wine-side imports resolved flat-namespace at load: under wine these come
 * from ntdll.so/winemac.so; for the host gate we provide inert stand-ins
 * (the smoke path never exercises them). */
extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

typedef int (*unix_fn)(void*);

static std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { fprintf(stderr, "FAIL: cannot read %s\n", path); exit(1); }
  std::vector<uint8_t> data(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
  return data;
}

static obj_handle_t loadLibrary(unix_fn* funcs, obj_handle_t device, const char* path) {
  std::vector<uint8_t> bytes = readFile(path);

  struct unixcall_generic_obj_uint64_obj_ret dd = {};
  dd.handle = obj_handle_t(uintptr_t(bytes.data()));
  dd.arg = bytes.size();
  funcs[118](&dd);
  if (!dd.ret) { fprintf(stderr, "FAIL: DispatchData(%s)\n", path); exit(1); }

  struct unixcall_mtldevice_newlibrary lib = {};
  lib.device = device;
  lib.data = dd.ret;
  funcs[25](&lib);
  if (!lib.ret_library) { fprintf(stderr, "FAIL: newLibrary(%s)\n", path); exit(1); }
  return lib.ret_library;
}

static obj_handle_t getFunction(unix_fn* funcs, obj_handle_t library, const char* name) {
  struct unixcall_generic_obj_uint64_obj_ret params = {};
  params.handle = library;
  params.arg = uint64_t(uintptr_t(name));
  funcs[26](&params);
  if (!params.ret) { fprintf(stderr, "FAIL: newFunction(%s)\n", name); exit(1); }
  return params.ret;
}

static std::vector<uint8_t> serializeSm3(const char* binPath) {
  std::vector<uint8_t> bytecode = readFile(binPath);

  dxbc_spv::util::ByteReader reader(bytecode.data(), bytecode.size());
  dxbc_spv::sm3::Converter::Options options = {};
  options.name = "main";

  dxbc_spv::ir::Builder builder;
  dxbc_spv::sm3::Converter converter(reader, options);
  if (!converter.convertShader(builder)) {
    fprintf(stderr, "FAIL: sm3 conversion (%s)\n", binPath); exit(1);
  }
  dxbc_spv::ir::legalizeIr(builder, dxbc_spv::ir::CompileOptions());

  dxbc_spv::ir::Serializer serializer(builder);
  std::vector<uint8_t> blob(serializer.computeSerializedSize());
  if (!serializer.serialize(blob.data(), blob.size())) {
    fprintf(stderr, "FAIL: ir serialize\n"); exit(1);
  }
  return blob;
}

int main(int argc, char** argv) {
  const char* bridgeSo = argc > 1 ? argv[1] : "build/bridge/winemetal.so";
  const char* airSo    = argc > 2 ? argv[2] : "build/airlib/d9mtair.so";
  const char* vsLib    = argc > 3 ? argv[3] : "build/air-compiler/vs.metallib";
  const char* psLib    = argc > 4 ? argv[4] : "build/air-compiler/ps.metallib";
  const char* sm3Bin   = argc > 5 ? argv[5] : "air/tests/bin/vs_mvp.bin";

  void* bridge = dlopen(bridgeSo, RTLD_NOW);
  if (!bridge) { fprintf(stderr, "FAIL: dlopen bridge: %s\n", dlerror()); return 1; }
  unix_fn* funcs = reinterpret_cast<unix_fn*>(dlsym(bridge, "__wine_unix_call_funcs"));
  if (!funcs) { fprintf(stderr, "FAIL: no call table\n"); return 1; }

  /* --- device ------------------------------------------------------------ */
  struct unixcall_generic_obj_ret devs = {};
  funcs[4](&devs);
  struct unixcall_generic_obj_uint64_obj_ret dev0 = {};
  dev0.handle = devs.ret;
  dev0.arg = 0;
  funcs[2](&dev0);
  if (!dev0.ret) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
  printf("device handle: ok\n");

  /* --- PSO through the vertex-descriptor entry (code 153) ---------------- */
  obj_handle_t vs = getFunction(funcs, loadLibrary(funcs, dev0.ret, vsLib), "main");
  obj_handle_t ps = getFunction(funcs, loadLibrary(funcs, dev0.ret, psLib), "main");

  WMTRenderPipelineInfo info = {};
  info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  info.colors[0].write_mask = 0xf;
  info.rasterization_enabled = true;
  info.max_tessellation_factor = 64;   /* Metal validation floor */
  info.raster_sample_count = 1;
  info.vertex_function = vs;
  info.fragment_function = ps;

  WMTRenderPipelineVertexInfo vertexInfo = {};
  vertexInfo.num_attributes = 2;
  vertexInfo.attributes[0] = { WMTAttributeFormatFloat4, 0u, 3u };
  vertexInfo.attributes[1] = { WMTAttributeFormatFloat4, 16u, 3u };
  vertexInfo.num_layouts = 4;
  vertexInfo.layouts[3] = { 32u, WMTVertexStepFunctionPerVertex, 1u };

  struct unixcall_mtldevice_newrenderpso_vtx psoCall = {};
  psoCall.device = dev0.ret;
  psoCall.info.set(&info);
  psoCall.vertex_info.set(&vertexInfo);
  funcs[153](&psoCall);

  if (!psoCall.ret_pso) {
    fprintf(stderr, "FAIL: vertex-descriptor PSO (err handle %llu)\n",
      (unsigned long long)psoCall.ret_error);
    return 1;
  }
  printf("vertex-descriptor PSO: ok\n");

  /* --- AIR thunk parity --------------------------------------------------- */
  std::vector<uint8_t> irBlob = serializeSm3(sm3Bin);

  auto compileWith = [&](int (*fn)(d9mt_air_compile_params*))
      -> std::vector<uint8_t> {
    d9mt_air_compile_params params = {};
    params.ir_data = uint64_t(uintptr_t(irBlob.data()));
    params.ir_size = irBlob.size();
    params.entry_name = uint64_t(uintptr_t("main"));
    int rc = fn(&params);
    if (rc != 0 || params.ret_status != D9MT_AIR_OK || !params.out_metallib) {
      fprintf(stderr, "FAIL: air compile rc=%d status=%u\n", rc, params.ret_status);
      exit(1);
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(uintptr_t(params.out_metallib));
    return std::vector<uint8_t>(p, p + params.out_size);
  };

  auto* bridgeCompile = reinterpret_cast<int (*)(d9mt_air_compile_params*)>(
    dlsym(bridge, "d9mtair_compile"));
  if (!bridgeCompile) { fprintf(stderr, "FAIL: bridge lacks d9mtair_compile\n"); return 1; }

  void* air = dlopen(airSo, RTLD_NOW);
  if (!air) { fprintf(stderr, "FAIL: dlopen d9mtair.so: %s\n", dlerror()); return 1; }
  auto* directCompile = reinterpret_cast<int (*)(d9mt_air_compile_params*)>(
    dlsym(air, "d9mtair_compile"));

  std::vector<uint8_t> viaBridge = compileWith(bridgeCompile);
  std::vector<uint8_t> viaDirect = compileWith(directCompile);

  if (viaBridge != viaDirect) {
    fprintf(stderr, "FAIL: bridge/direct metallib mismatch (%zu vs %zu bytes)\n",
      viaBridge.size(), viaDirect.size());
    return 1;
  }
  printf("air thunk parity: ok (%zu bytes, byte-identical)\n", viaBridge.size());

  printf("PASS\n");
  return 0;
}
