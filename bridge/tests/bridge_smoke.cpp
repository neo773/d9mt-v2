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

  /* --- Metal 4 surface (codes 157..200) ---------------------------------- *
   * Drives the full MTL4 path present-less: queue + allocator + command buffer,
   * an argument table, a residency set, an MTL4Compiler-built render PSO (entry
   * 196) from the same vs/ps libraries, then a real render pass encoding ONE
   * draw through an MTL4RenderCommandEncoder. Resources are made resident and
   * argument-table addresses bound so the draw does not GPU-fault. Completion
   * is observed by signaling a shared event on the queue after commit and
   * waiting on it (MTL4 has no cmdbuf status). */
  {
    struct unixcall_generic_obj_obj_ret mk = {};
    mk.handle = dev0.ret; funcs[157](&mk);
    obj_handle_t queue4 = mk.ret;
    if (!queue4) { fprintf(stderr, "FAIL: newMTL4CommandQueue\n"); return 1; }

    mk = {}; mk.handle = dev0.ret; funcs[158](&mk);
    obj_handle_t allocator = mk.ret;
    if (!allocator) { fprintf(stderr, "FAIL: newCommandAllocator\n"); return 1; }

    mk = {}; mk.handle = dev0.ret; funcs[159](&mk);
    obj_handle_t cmdbuf4 = mk.ret;
    if (!cmdbuf4) { fprintf(stderr, "FAIL: newMTL4CommandBuffer\n"); return 1; }

    mk = {}; mk.handle = dev0.ret; funcs[161](&mk);
    obj_handle_t compiler = mk.ret;
    if (!compiler) { fprintf(stderr, "FAIL: newMTL4Compiler\n"); return 1; }

    WMTArgTableInfo atInfo = {};
    atInfo.max_buffers = 32; atInfo.max_textures = 16; atInfo.max_samplers = 16;
    atInfo.initialize_bindings = 1; atInfo.support_attribute_strides = 1;
    struct unixcall_mtl4_new_from_desc atCall = {};
    atCall.device = dev0.ret; atCall.info.set(&atInfo); funcs[160](&atCall);
    obj_handle_t argtable = atCall.ret;
    if (!argtable) { fprintf(stderr, "FAIL: newArgumentTable\n"); return 1; }

    WMTResidencySetInfo rsInfo = {}; rsInfo.initial_capacity = 8;
    struct unixcall_mtl4_new_from_desc rsCall = {};
    rsCall.device = dev0.ret; rsCall.info.set(&rsInfo); funcs[162](&rsCall);
    obj_handle_t resset = rsCall.ret;
    if (!resset) { fprintf(stderr, "FAIL: newResidencySet\n"); return 1; }

    /* MTL4Compiler render PSO from the vs/ps libraries (entry 196) */
    obj_handle_t vsLibH = loadLibrary(funcs, dev0.ret, vsLib);
    obj_handle_t psLibH = loadLibrary(funcs, dev0.ret, psLib);
    const char* mainName = "main";

    WMTMtl4RenderPipelineInfo p4 = {};
    p4.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    p4.colors[0].write_mask = 0xf;
    p4.vertex_library = vsLibH;
    p4.fragment_library = psLibH;
    p4.vertex_entry_name.set((void*)mainName);
    p4.fragment_entry_name.set((void*)mainName);
    p4.raster_sample_count = 1;
    /* Unspecified: the AIR vertex shader writes [[point_size]], which Metal
     * rejects against an explicit Triangle topology (matches the code-153
     * classic path, which also leaves topology unspecified). */
    p4.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
    p4.rasterization_enabled = 1;
    p4.vertex.num_attributes = 2;
    p4.vertex.attributes[0] = { WMTAttributeFormatFloat4, 0u, 3u };
    p4.vertex.attributes[1] = { WMTAttributeFormatFloat4, 16u, 3u };
    p4.vertex.num_layouts = 4;
    p4.vertex.layouts[3] = { 32u, WMTVertexStepFunctionPerVertex, 1u };

    struct unixcall_mtl4_newrenderpso psoCall4 = {};
    psoCall4.compiler = compiler; psoCall4.info.set(&p4); funcs[196](&psoCall4);
    if (!psoCall4.ret_pso) {
      char errbuf[1024] = {0};
      if (psoCall4.ret_error) {
        struct unixcall_generic_obj_obj_ret d = {}; d.handle = psoCall4.ret_error; funcs[28](&d);
        if (d.ret) {
          struct unixcall_nsstring_getcstring gc = {};
          gc.str = d.ret; gc.buffer_ptr = uint64_t(uintptr_t(errbuf));
          gc.max_length = sizeof(errbuf); gc.encoding = WMTUTF8StringEncoding; funcs[8](&gc);
        }
      }
      fprintf(stderr, "FAIL: MTL4Compiler render PSO: %s\n", errbuf);
      return 1;
    }
    printf("MTL4 compiler PSO: ok\n");

    /* a resident buffer (bound at argument-table indices the shader reads) */
    WMTBufferInfo bufInfo = {};
    bufInfo.length = 65536;
    bufInfo.options = (enum WMTResourceOptions)0; /* shared */
    struct unixcall_mtldevice_newbuffer bufCall = {};
    bufCall.device = dev0.ret; bufCall.info.set(&bufInfo); funcs[18](&bufCall);
    if (!bufCall.ret) { fprintf(stderr, "FAIL: newBuffer(mtl4)\n"); return 1; }
    uint64_t bufAddr = bufInfo.gpu_address;

    WMTTextureInfo t4 = {};
    t4.pixel_format = WMTPixelFormatBGRA8Unorm;
    t4.width = 4; t4.height = 4; t4.depth = 1; t4.array_length = 1;
    t4.type = WMTTextureType2D; t4.mipmap_level_count = 1; t4.sample_count = 1;
    t4.usage = (enum WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
    t4.options = WMTResourceStorageModePrivate;
    struct unixcall_mtldevice_newtexture t4Call = {};
    t4Call.device = dev0.ret; t4Call.info.set(&t4); funcs[21](&t4Call);
    if (!t4Call.ret) { fprintf(stderr, "FAIL: newTexture(mtl4)\n"); return 1; }

    /* residency: add buffer + texture, commit, request */
    struct unixcall_generic_obj_obj_noret addCall = {};
    addCall.handle = resset; addCall.arg = bufCall.ret; funcs[197](&addCall);
    addCall.handle = resset; addCall.arg = t4Call.ret; funcs[197](&addCall);
    struct unixcall_generic_obj_noret rsOp = {};
    rsOp.handle = resset; funcs[198](&rsOp); /* commit */
    rsOp.handle = resset; funcs[199](&rsOp); /* requestResidency */

    /* begin command buffer + attach residency set */
    struct unixcall_generic_obj_obj_noret beginCall = {};
    beginCall.handle = cmdbuf4; beginCall.arg = allocator; funcs[164](&beginCall);
    struct unixcall_generic_obj_obj_noret useRs = {};
    useRs.handle = cmdbuf4; useRs.arg = resset; funcs[166](&useRs);

    /* bind buffer address across the slots the shader may read (stage-in buf3,
     * push buf20, plus 0..7) and the texture at texture slot 0 */
    for (uint64_t idx = 0; idx <= 20; idx++) {
      struct unixcall_mtl4_argtable_setaddress sa = {};
      sa.table = argtable; sa.gpu_address = bufAddr; sa.stride = 0; sa.index = idx;
      funcs[172](&sa);
    }
    {
      struct unixcall_mtl4_argtable_setresource st = {};
      st.table = argtable; st.gpu_resource_id = t4.gpu_resource_id; st.index = 0;
      funcs[174](&st);
    }

    /* render pass over the 4x4 RT */
    WMTRenderPassInfo pass4 = {};
    pass4.colors[0].texture = t4Call.ret;
    pass4.colors[0].load_action = WMTLoadActionClear;
    pass4.colors[0].store_action = WMTStoreActionStore;
    pass4.colors[0].clear_color = { 0.1, 0.2, 0.3, 1.0 };
    pass4.default_raster_sample_count = 1;
    pass4.render_target_width = 4; pass4.render_target_height = 4;

    struct unixcall_generic_obj_uint64_obj_ret enc4 = {};
    enc4.handle = cmdbuf4; enc4.arg = uint64_t(uintptr_t(&pass4)); funcs[176](&enc4);
    if (!enc4.ret) { fprintf(stderr, "FAIL: MTL4 renderCommandEncoder\n"); return 1; }

    struct unixcall_mtl4_setargumenttable sat = {};
    sat.encoder = enc4.ret; sat.table = argtable; sat.stages = 1 | 2; funcs[177](&sat);

    struct unixcall_generic_obj_obj_noret setPso = {};
    setPso.handle = enc4.ret; setPso.arg = psoCall4.ret_pso; funcs[178](&setPso);

    struct unixcall_mtl4_setviewport vp = {};
    vp.encoder = enc4.ret; vp.viewport = { 0, 0, 4, 4, 0, 1 }; funcs[179](&vp);
    struct unixcall_mtl4_setscissorrect sc = {};
    sc.encoder = enc4.ret; sc.rect = { 0, 0, 4, 4 }; funcs[180](&sc);

    struct unixcall_generic_obj_uint64_noret u1 = {};
    u1.handle = enc4.ret; u1.arg = WMTCullModeNone; funcs[182](&u1);        /* setCullMode */
    u1.handle = enc4.ret; u1.arg = WMTWindingClockwise; funcs[183](&u1);    /* setFrontFacingWinding */
    u1.handle = enc4.ret; u1.arg = 0; funcs[186](&u1);                      /* setStencilReferenceValue */
    struct unixcall_mtl4_setdepthbias db = {};
    db.encoder = enc4.ret; funcs[184](&db);
    struct unixcall_mtl4_setblendcolor bc = {};
    bc.encoder = enc4.ret; bc.alpha = 1.0f; funcs[185](&bc);

    struct unixcall_mtl4_draw draw = {};
    draw.encoder = enc4.ret; draw.primitive = WMTPrimitiveTypeTriangle;
    draw.vertex_start = 0; draw.vertex_count = 3; draw.instance_count = 1; draw.base_instance = 0;
    funcs[187](&draw);

    struct unixcall_generic_obj_noret endEnc = {};
    endEnc.handle = enc4.ret; funcs[33](&endEnc); /* shared MTLCommandEncoder_endEncoding */

    struct unixcall_generic_obj_noret endCb = {};
    endCb.handle = cmdbuf4; funcs[165](&endCb);

    obj_handle_t cbArray[1] = { cmdbuf4 };
    struct unixcall_mtl4_queue_commit commit4 = {};
    commit4.queue = queue4; commit4.cmdbufs.set(cbArray); commit4.count = 1;
    funcs[167](&commit4);

    /* completion: signal a shared event on the queue after the commit, wait */
    struct unixcall_generic_obj_obj_ret evMk = {};
    evMk.handle = dev0.ret; funcs[15](&evMk);
    obj_handle_t event = evMk.ret;
    if (event) {
      struct unixcall_generic_obj_obj_uint64_noret sig = {};
      sig.handle = queue4; sig.arg0 = event; sig.arg1 = 1; funcs[170](&sig);
      struct unixcall_mtlsharedevent_waituntilsignaledvalue wev = {};
      wev.event = event; wev.value = 1; wev.timeout_ms = 5000;
      funcs[130](&wev);
      /* waitUntilSignaledValue returns YES when the value is reached (the
       * unix side stores that return in ret_timeout, despite the name), NO on
       * an actual timeout. */
      if (!wev.ret_timeout) {
        fprintf(stderr, "FAIL: MTL4 command buffer did not complete (event timeout)\n");
        return 1;
      }
      printf("MTL4 render pass draw + commit: ok (event completed)\n");
    } else {
      printf("MTL4 render pass draw + commit: ok (no crash; event unavailable)\n");
    }

    /* MTL4CommandAllocator_reset (163) is safe once the buffer completed */
    struct unixcall_generic_obj_noret rst = {};
    rst.handle = allocator; funcs[163](&rst);
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
