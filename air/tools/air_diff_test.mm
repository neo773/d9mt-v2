#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSize = 128;
constexpr uint32_t kVertexBufferIndex = 27;
constexpr int kTolerance = 2;

struct ManifestBuffer {
  std::string name;
  uint32_t index;
  uint64_t size;
};

struct ManifestResource {
  std::string name;
  uint32_t index;
  std::string kind;
};

struct ManifestSpec {
  std::string name;
  uint32_t index;
};

struct Manifest {
  std::string entry;
  std::string stage;
  std::vector<ManifestBuffer> buffers;
  std::vector<ManifestResource> textures;
  std::vector<ManifestResource> samplers;
  std::vector<ManifestSpec> specConstants;
  uint32_t renderTargets = 1;
  bool depthOut = false;
  bool pointSize = false;
};

struct SpecValues {
  std::vector<std::pair<uint32_t, uint32_t>> values;
};

bool loadManifest(const char* path, Manifest& m) {
  NSData* data = [NSData dataWithContentsOfFile:
    [NSString stringWithUTF8String:path]];
  if (!data)
    return false;
  NSError* err = nil;
  NSDictionary* root = [NSJSONSerialization JSONObjectWithData:data
    options:0 error:&err];
  if (!root)
    return false;

  m.entry = [root[@"entry"] UTF8String];
  m.stage = [root[@"stage"] UTF8String];
  for (NSDictionary* b in root[@"buffers"])
    m.buffers.push_back({ [b[@"name"] UTF8String],
      uint32_t([b[@"index"] unsignedIntValue]),
      uint64_t([b[@"size"] unsignedLongLongValue]) });
  for (NSDictionary* t in root[@"textures"])
    m.textures.push_back({ [t[@"name"] UTF8String],
      uint32_t([t[@"index"] unsignedIntValue]),
      t[@"kind"] ? [t[@"kind"] UTF8String] : "2d" });
  for (NSDictionary* s in root[@"samplers"])
    m.samplers.push_back({ [s[@"name"] UTF8String],
      uint32_t([s[@"index"] unsignedIntValue]), "" });
  for (NSDictionary* s in root[@"specConstants"])
    m.specConstants.push_back({ [s[@"name"] UTF8String],
      uint32_t([s[@"index"] unsignedIntValue]) });
  if (root[@"renderTargets"])
    m.renderTargets = uint32_t([root[@"renderTargets"] unsignedIntValue]);
  m.depthOut = [root[@"depthOut"] boolValue];
  m.pointSize = [root[@"pointSize"] boolValue];
  return true;
}

id<MTLBuffer> makeConstantBuffer(id<MTLDevice> device, const ManifestBuffer& mb) {
  uint64_t size = mb.size ? (mb.size + 15u) & ~15ull : 16u;
  std::vector<float> data(size / 4u, 0.f);

  bool isConstFloat = mb.name.find("cF") != std::string::npos ||
                      mb.name.find("ConstFloat") != std::string::npos;
  if (isConstFloat) {
    for (uint64_t reg = 0u; reg < size / 16u; reg++) {
      data[reg * 4u + 0u] = float(reg) * 0.13f + 0.01f;
      data[reg * 4u + 1u] = float(reg) * 0.07f + 0.02f;
      data[reg * 4u + 2u] = float(reg) * 0.03f + 0.03f;
      data[reg * 4u + 3u] = 1.0f;
    }
  }
  return [device newBufferWithBytes:data.data() length:size
    options:MTLResourceStorageModeShared];
}

void fillCheckerPixels(uint8_t* pixels, uint32_t n) {
  for (uint32_t y = 0; y < n; y++) {
    for (uint32_t x = 0; x < n; x++) {
      bool on = ((x ^ y) & 1u) != 0u;
      uint8_t* p = &pixels[(y * n + x) * 4];
      p[0] = on ? 255 : 32; p[1] = on ? 128 : 64;
      p[2] = on ? 32 : 255; p[3] = 255;
    }
  }
}

id<MTLTexture> makeCheckerTexture(
        id<MTLDevice>       device,
        id<MTLCommandQueue> queue,
        const std::string&  kind) {
  uint8_t pixels[4 * 4 * 4];
  fillCheckerPixels(pixels, 4);

  if (kind == "depth2d" || kind == "depthcube") {
    // depth-format textures reject replaceRegion, so fill via blit instead
    MTLTextureDescriptor* td;
    if (kind == "depthcube") {
      td = [MTLTextureDescriptor
        textureCubeDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        size:4 mipmapped:NO];
    } else {
      td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:4 height:4 mipmapped:NO];
    }
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];

    float depths[16];
    for (uint32_t i = 0; i < 16; i++)
      depths[i] = float(i) / 16.f;
    id<MTLBuffer> staging = [device newBufferWithBytes:depths
      length:sizeof(depths) options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    uint32_t slices = kind == "depthcube" ? 6u : 1u;
    for (uint32_t s = 0; s < slices; s++) {
      [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:16
        sourceBytesPerImage:64 sourceSize:MTLSizeMake(4, 4, 1)
        toTexture:tex destinationSlice:s destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return tex;
  }

  if (kind == "cube") {
    MTLTextureDescriptor* td = [MTLTextureDescriptor
      textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
      size:4 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];
    for (uint32_t s = 0; s < 6; s++) {
      [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0
        slice:s withBytes:pixels bytesPerRow:16 bytesPerImage:64];
    }
    return tex;
  }

  if (kind == "3d") {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType3D;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 4; td.height = 4; td.depth = 4;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];
    for (uint32_t z = 0; z < 4; z++) {
      [tex replaceRegion:MTLRegionMake3D(0, 0, z, 4, 4, 1) mipmapLevel:0
        slice:0 withBytes:pixels bytesPerRow:16 bytesPerImage:64];
    }
    return tex;
  }

  MTLTextureDescriptor* td = [MTLTextureDescriptor
    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
    width:4 height:4 mipmapped:NO];
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> tex = [device newTextureWithDescriptor:td];
  [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0
    withBytes:pixels bytesPerRow:16];
  return tex;
}

bool renderPath(
        id<MTLDevice>    device,
        const char*      vsLibPath,
        const Manifest&  vsManifest,
        const char*      psLibPath,
        const Manifest&  psManifest,
        const SpecValues& specValues,
        const char*      label,
        std::vector<uint8_t>& pixels,
        std::vector<float>&   depthPixels) {
  NSError* err = nil;

  auto loadLib = [&](const char* path) -> id<MTLLibrary> {
    NSData* data = [NSData dataWithContentsOfFile:
      [NSString stringWithUTF8String:path]];
    if (!data) {
      fprintf(stderr, "FAIL[%s]: cannot read %s\n", label, path);
      return nil;
    }
    dispatch_data_t ddata = dispatch_data_create(data.bytes, data.length,
      dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> lib = [device newLibraryWithData:ddata error:&err];
    if (!lib)
      fprintf(stderr, "FAIL[%s newLibraryWithData %s]: %s\n",
        label, path, err.localizedDescription.UTF8String);
    return lib;
  };

  id<MTLLibrary> vsLib = loadLib(vsLibPath);
  id<MTLLibrary> psLib = loadLib(psLibPath);
  if (!vsLib || !psLib)
    return false;

  auto loadFn = [&](id<MTLLibrary> lib, const Manifest& m) -> id<MTLFunction> {
    NSString* name = [NSString stringWithUTF8String:m.entry.c_str()];
    if (m.specConstants.empty())
      return [lib newFunctionWithName:name];

    MTLFunctionConstantValues* cv = [MTLFunctionConstantValues new];
    for (const auto& spec : m.specConstants) {
      uint32_t value = 0u;
      for (const auto& kv : specValues.values) {
        if (kv.first == spec.index)
          value = kv.second;
      }
      [cv setConstantValue:&value type:MTLDataTypeUInt atIndex:spec.index];
    }
    NSError* fnErr = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:name
      constantValues:cv error:&fnErr];
    if (!fn)
      fprintf(stderr, "FAIL[%s specialization]: %s\n", label,
        fnErr.localizedDescription.UTF8String);
    return fn;
  };

  id<MTLFunction> vs = loadFn(vsLib, vsManifest);
  id<MTLFunction> ps = loadFn(psLib, psManifest);
  if (!vs || !ps) {
    fprintf(stderr, "FAIL[%s]: entry lookup vs=%p ps=%p\n", label, vs, ps);
    return false;
  }

  MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
  vd.attributes[0].format = MTLVertexFormatFloat4;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = kVertexBufferIndex;
  vd.attributes[1].format = MTLVertexFormatFloat4;
  vd.attributes[1].offset = 16;
  vd.attributes[1].bufferIndex = kVertexBufferIndex;
  vd.attributes[2].format = MTLVertexFormatFloat2;
  vd.attributes[2].offset = 32;
  vd.attributes[2].bufferIndex = kVertexBufferIndex;
  vd.layouts[kVertexBufferIndex].stride = 48;
  vd.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;

  uint32_t rtCount = psManifest.renderTargets;
  bool depthOut = psManifest.depthOut;
  bool drawPoints = vsManifest.pointSize;

  MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = vs;
  pd.fragmentFunction = ps;
  pd.vertexDescriptor = vd;
  for (uint32_t rt = 0; rt < rtCount; rt++)
    pd.colorAttachments[rt].pixelFormat = MTLPixelFormatBGRA8Unorm;
  if (depthOut)
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

  id<MTLRenderPipelineState> pso =
    [device newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!pso) {
    fprintf(stderr, "FAIL[%s newRenderPipelineState]: %s\n",
      label, err.localizedDescription.UTF8String);
    return false;
  }

  std::vector<id<MTLTexture>> targets;
  for (uint32_t rt = 0; rt < rtCount; rt++) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
      width:kSize height:kSize mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    targets.push_back([device newTextureWithDescriptor:td]);
  }
  id<MTLTexture> target = targets[0];

  id<MTLTexture> depthTarget = nil;
  if (depthOut) {
    MTLTextureDescriptor* dd = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
      width:kSize height:kSize mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    depthTarget = [device newTextureWithDescriptor:dd];
  }

  float verts[] = {
    -1.f, -1.f, 0.5f, 1.f,    1.f, 0.f, 0.f, 1.f,   0.f, 0.f, 0.f, 0.f,
     3.f, -1.f, 0.5f, 1.f,    0.f, 1.f, 0.f, 1.f,   2.f, 0.f, 0.f, 0.f,
    -1.f,  3.f, 0.5f, 1.f,    0.f, 0.f, 1.f, 1.f,   0.f, 2.f, 0.f, 0.f,
  };
  id<MTLBuffer> vb = [device newBufferWithBytes:verts length:sizeof(verts)
    options:MTLResourceStorageModeShared];

  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> cb = [queue commandBuffer];

  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  for (uint32_t rt = 0; rt < rtCount; rt++) {
    rp.colorAttachments[rt].texture = targets[rt];
    rp.colorAttachments[rt].loadAction = MTLLoadActionClear;
    rp.colorAttachments[rt].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[rt].storeAction = MTLStoreActionStore;
  }
  if (depthOut) {
    rp.depthAttachment.texture = depthTarget;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
  }

  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:pso];
  [enc setVertexBuffer:vb offset:0 atIndex:kVertexBufferIndex];

  for (const auto& mb : vsManifest.buffers)
    [enc setVertexBuffer:makeConstantBuffer(device, mb) offset:0
      atIndex:mb.index];
  for (const auto& mb : psManifest.buffers)
    [enc setFragmentBuffer:makeConstantBuffer(device, mb) offset:0
      atIndex:mb.index];

  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.minFilter = MTLSamplerMinMagFilterNearest;
  sd.magFilter = MTLSamplerMinMagFilterNearest;
  sd.sAddressMode = MTLSamplerAddressModeRepeat;
  sd.tAddressMode = MTLSamplerAddressModeRepeat;
  sd.rAddressMode = MTLSamplerAddressModeRepeat;
  sd.compareFunction = MTLCompareFunctionLessEqual;
  id<MTLSamplerState> smp = [device newSamplerStateWithDescriptor:sd];

  for (const auto& t : vsManifest.textures)
    [enc setVertexTexture:makeCheckerTexture(device, queue, t.kind)
      atIndex:t.index];
  for (const auto& s : vsManifest.samplers)
    [enc setVertexSamplerState:smp atIndex:s.index];

  for (const auto& t : psManifest.textures)
    [enc setFragmentTexture:makeCheckerTexture(device, queue, t.kind)
      atIndex:t.index];
  for (const auto& s : psManifest.samplers)
    [enc setFragmentSamplerState:smp atIndex:s.index];

  if (depthOut) {
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionAlways;
    ds.depthWriteEnabled = YES;
    [enc setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
  }

  [enc drawPrimitives:
    drawPoints ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle
    vertexStart:0 vertexCount:3];
  [enc endEncoding];

  id<MTLBuffer> depthReadback = nil;
  if (depthOut) {
    // Depth32Float textures don't support getBytes; stage through a blit
    depthReadback = [device newBufferWithLength:kSize * kSize * 4
      options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:depthTarget sourceSlice:0 sourceLevel:0
      sourceOrigin:MTLOriginMake(0, 0, 0)
      sourceSize:MTLSizeMake(kSize, kSize, 1)
      toBuffer:depthReadback destinationOffset:0
      destinationBytesPerRow:kSize * 4
      destinationBytesPerImage:kSize * kSize * 4];
    [blit endEncoding];
  }

  [cb commit];
  [cb waitUntilCompleted];

  if (cb.error) {
    fprintf(stderr, "FAIL[%s commandBuffer]: %s\n",
      label, cb.error.localizedDescription.UTF8String);
    return false;
  }

  pixels.resize(kSize * kSize * 4 * rtCount);
  for (uint32_t rt = 0; rt < rtCount; rt++) {
    [targets[rt] getBytes:pixels.data() + size_t(rt) * kSize * kSize * 4
      bytesPerRow:kSize * 4
      fromRegion:MTLRegionMake2D(0, 0, kSize, kSize) mipmapLevel:0];
  }

  if (depthOut) {
    depthPixels.resize(kSize * kSize);
    memcpy(depthPixels.data(), depthReadback.contents, kSize * kSize * 4);
  }
  return true;
}

}

int main(int argc, char** argv) {
  if (argc < 10) {
    fprintf(stderr, "usage: %s <name> <air_vs.mtl> <air_vs.json> "
      "<air_ps.mtl> <air_ps.json> <ref_vs.mtl> <ref_vs.json> "
      "<ref_ps.mtl> <ref_ps.json> [--spec idx=value]...\n", argv[0]);
    return 2;
  }
  const char* name = argv[1];

  SpecValues specValues;
  for (int i = 10; i + 1 < argc; i++) {
    if (std::string(argv[i]) == "--spec") {
      uint32_t idx = 0u, value = 0u;
      if (sscanf(argv[i + 1], "%u=%u", &idx, &value) == 2)
        specValues.values.emplace_back(idx, value);
      i++;
    }
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }

    Manifest airVs, airPs, refVs, refPs;
    if (!loadManifest(argv[3], airVs) || !loadManifest(argv[5], airPs) ||
        !loadManifest(argv[7], refVs) || !loadManifest(argv[9], refPs)) {
      fprintf(stderr, "FAIL[%s]: manifest load\n", name);
      return 1;
    }

    std::vector<uint8_t> airPixels, refPixels;
    std::vector<float> airDepth, refDepth;
    if (!renderPath(device, argv[2], airVs, argv[4], airPs, specValues,
          "air", airPixels, airDepth))
      return 1;
    if (!renderPath(device, argv[6], refVs, argv[8], refPs, specValues,
          "ref", refPixels, refDepth))
      return 1;

    if (airPixels.size() != refPixels.size() ||
        airDepth.size() != refDepth.size()) {
      fprintf(stderr, "FAIL[%s]: output interface mismatch\n", name);
      return 1;
    }

    int maxDiff = 0;
    uint64_t badPixels = 0;
    for (size_t i = 0; i < airPixels.size(); i += 4) {
      int pixelMax = 0;
      for (size_t c = 0; c < 4; c++) {
        int d = std::abs(int(airPixels[i + c]) - int(refPixels[i + c]));
        pixelMax = std::max(pixelMax, d);
      }
      maxDiff = std::max(maxDiff, pixelMax);
      if (pixelMax > kTolerance)
        badPixels++;
    }

    double maxDepthDiff = 0.0;
    for (size_t i = 0; i < airDepth.size(); i++) {
      double d = std::abs(double(airDepth[i]) - double(refDepth[i]));
      maxDepthDiff = std::max(maxDepthDiff, d);
      if (d > 1.0 / 1024.0)
        badPixels++;
    }

    double badPct = 100.0 * double(badPixels) / double(kSize * kSize);
    bool pass = badPixels == 0;
    if (airDepth.empty()) {
      printf("%-24s %s  maxdiff=%d  bad=%.2f%%\n",
        name, pass ? "PASS" : "FAIL", maxDiff, badPct);
    } else {
      printf("%-24s %s  maxdiff=%d  depthdiff=%.5f  bad=%.2f%%\n",
        name, pass ? "PASS" : "FAIL", maxDiff, maxDepthDiff, badPct);
    }
    return pass ? 0 : 1;
  }
}
