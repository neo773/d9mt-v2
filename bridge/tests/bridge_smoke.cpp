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

  /* --- render-pass encode of the appended sampler/texture commands -------- *
   * Drives WMTRenderCommandSetFragmentSamplerState / SetVertexTexture /
   * SetVertexSamplerState (the per-slot sampler + VTF binds the upstream
   * wmtcmd ABI lacks) through a real MTLRenderCommandEncoder: bind a sampler
   * and a texture on both the fragment and vertex stages via a wmtcmd chunk
   * chain, end the pass, commit, and require the command buffer to complete
   * without error. No draw is issued (binds are independent of draw), so the
   * trivial pass reuses the vertex-descriptor PSO created above. */
  {
    /* queue + a 1x1 render-target texture (also shader-readable so it can
     * double as the bound vertex/fragment texture) */
    struct unixcall_generic_obj_uint64_obj_ret queueCall = {};
    queueCall.handle = dev0.ret;
    queueCall.arg = 1; /* maxCommandBufferCount */
    funcs[9](&queueCall);
    if (!queueCall.ret) { fprintf(stderr, "FAIL: newCommandQueue\n"); return 1; }

    WMTTextureInfo texInfo = {};
    texInfo.pixel_format = WMTPixelFormatBGRA8Unorm;
    texInfo.width = 4;
    texInfo.height = 4;
    texInfo.depth = 1;
    texInfo.array_length = 1;
    texInfo.type = WMTTextureType2D;
    texInfo.mipmap_level_count = 1;
    texInfo.sample_count = 1;
    texInfo.usage = (enum WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
    texInfo.options = WMTResourceStorageModePrivate;

    struct unixcall_mtldevice_newtexture texCall = {};
    texCall.device = dev0.ret;
    texCall.info.set(&texInfo);
    funcs[21](&texCall);
    if (!texCall.ret) { fprintf(stderr, "FAIL: newTexture\n"); return 1; }

    WMTSamplerInfo sampInfo = {};
    sampInfo.min_filter = WMTSamplerMinMagFilterLinear;
    sampInfo.mag_filter = WMTSamplerMinMagFilterLinear;
    sampInfo.mip_filter = WMTSamplerMipFilterNotMipmapped;
    sampInfo.r_address_mode = WMTSamplerAddressModeClampToEdge;
    sampInfo.s_address_mode = WMTSamplerAddressModeClampToEdge;
    sampInfo.t_address_mode = WMTSamplerAddressModeClampToEdge;
    sampInfo.border_color = WMTSamplerBorderColorTransparentBlack;
    sampInfo.compare_function = WMTCompareFunctionNever;
    sampInfo.lod_min_clamp = 0.0f;
    sampInfo.lod_max_clamp = 0.0f;
    sampInfo.max_anisotroy = 1;
    sampInfo.normalized_coords = true;

    struct unixcall_mtldevice_newsamplerstate sampCall = {};
    sampCall.device = dev0.ret;
    sampCall.info.set(&sampInfo);
    funcs[19](&sampCall);
    if (!sampCall.ret) { fprintf(stderr, "FAIL: newSamplerState\n"); return 1; }

    /* command buffer + render command encoder over a single clear-to-store
     * BGRA8 attachment (matches the PSO's colors[0] format) */
    struct unixcall_generic_obj_obj_ret cbCall = {};
    cbCall.handle = queueCall.ret;
    funcs[11](&cbCall);
    if (!cbCall.ret) { fprintf(stderr, "FAIL: commandBuffer\n"); return 1; }

    WMTRenderPassInfo pass = {};
    pass.colors[0].texture = texCall.ret;
    pass.colors[0].load_action = WMTLoadActionClear;
    pass.colors[0].store_action = WMTStoreActionStore;
    pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };
    pass.default_raster_sample_count = 1;
    pass.render_target_width = 4;
    pass.render_target_height = 4;

    struct unixcall_generic_obj_uint64_obj_ret encCall = {};
    encCall.handle = cbCall.ret;
    encCall.arg = uint64_t(uintptr_t(&pass));
    funcs[32](&encCall);
    if (!encCall.ret) { fprintf(stderr, "FAIL: renderCommandEncoder\n"); return 1; }

    /* wmtcmd chunk chain: SetPSO -> SetFragmentSampler -> SetVertexTexture ->
     * SetVertexSampler. Linked via each command's `next` pointer; encodeCommands
     * walks the chain and replays each through the encoder. */
    struct wmtcmd_render_setpso cPso = {};
    cPso.type = WMTRenderCommandSetPSO;
    cPso.pso = psoCall.ret_pso;

    struct wmtcmd_render_setsamplerstate cFragSamp = {};
    cFragSamp.type = WMTRenderCommandSetFragmentSamplerState;
    cFragSamp.sampler = sampCall.ret;
    cFragSamp.index = 0;

    struct wmtcmd_render_settexture cVtxTex = {};
    cVtxTex.type = WMTRenderCommandSetVertexTexture;
    cVtxTex.texture = texCall.ret;
    cVtxTex.index = 0;

    struct wmtcmd_render_setsamplerstate cVtxSamp = {};
    cVtxSamp.type = WMTRenderCommandSetVertexSamplerState;
    cVtxSamp.sampler = sampCall.ret;
    cVtxSamp.index = 0;

    cPso.next.set(&cFragSamp);
    cFragSamp.next.set(&cVtxTex);
    cVtxTex.next.set(&cVtxSamp);
    /* cVtxSamp.next left null-terminated */

    struct unixcall_generic_obj_cmd_noret cmdCall = {};
    cmdCall.encoder = encCall.ret;
    cmdCall.cmd_head.set(&cPso);
    funcs[38](&cmdCall);

    struct unixcall_generic_obj_noret endCall = {};
    endCall.handle = encCall.ret;
    funcs[33](&endCall);

    struct unixcall_generic_obj_noret commitCall = {};
    commitCall.handle = cbCall.ret;
    funcs[12](&commitCall);

    struct unixcall_generic_obj_noret waitCall = {};
    waitCall.handle = cbCall.ret;
    funcs[13](&waitCall);

    struct unixcall_generic_obj_uint64_ret statusCall = {};
    statusCall.handle = cbCall.ret;
    funcs[14](&statusCall);
    if (statusCall.ret != WMTCommandBufferStatusCompleted) {
      fprintf(stderr, "FAIL: render-pass command buffer status %llu (expected %d)\n",
        (unsigned long long)statusCall.ret, (int)WMTCommandBufferStatusCompleted);
      return 1;
    }
    printf("render-pass sampler/texture encode: ok (status completed)\n");
  }

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
