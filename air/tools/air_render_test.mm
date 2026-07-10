/*
 * air_render_test: loads a VS metallib + PS metallib produced by the AIR
 * backend from the shadertri SM3 pair, renders the triangle offscreen with
 * the v0 binding model, and verifies interpolated corner colors.
 *
 *   air_render_test vs.metallib ps.metallib
 *
 * Bindings (must match AirBuilder's v0 model):
 *   VS: buffer(0)=cF float4[256] (identity MVP rows in [0..3]),
 *       buffer(1)=clipPlanes {u32 count; pad; float4 x6} zeroed,
 *       buffer(2)=pointArgs {i32,f32,f32,f32},
 *       vertex data via stage_in at bufferIndex 3 (attr0 pos, attr1 color).
 *   PS: buffer(0)=cF float4[224] ([0]=tint=1,1,1,1),
 *       buffer(1)=alphaTest {u32 compareOp=8(always), u32 bits, u32 ref}.
 *
 * Exit 0 = PASS.
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <array>
#include <cstdio>
#include <vector>

static id<MTLLibrary> loadLib(id<MTLDevice> device, const char* path) {
  NSData* data = [NSData dataWithContentsOfFile:
    [NSString stringWithUTF8String:path]];
  if (!data) { fprintf(stderr, "FAIL: cannot read %s\n", path); return nil; }

  dispatch_data_t ddata = dispatch_data_create(
    data.bytes, data.length, dispatch_get_main_queue(),
    DISPATCH_DATA_DESTRUCTOR_DEFAULT);

  NSError* err = nil;
  id<MTLLibrary> lib = [device newLibraryWithData:ddata error:&err];
  if (!lib)
    fprintf(stderr, "FAIL[newLibraryWithData %s]: %s\n", path,
      err.localizedDescription.UTF8String);
  return lib;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <vs.metallib> <ps.metallib>\n", argv[0]);
    return 2;
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLLibrary> vsLib = loadLib(device, argv[1]);
    id<MTLLibrary> psLib = loadLib(device, argv[2]);
    if (!vsLib || !psLib) return 1;

    id<MTLFunction> vs = [vsLib newFunctionWithName:@"main"];
    id<MTLFunction> fs = [psLib newFunctionWithName:@"main"];
    if (!vs || !fs) {
      fprintf(stderr, "FAIL: entry lookup vs=%p fs=%p\n", vs, fs);
      return 1;
    }

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 3;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 16;
    vd.attributes[1].bufferIndex = 3;
    vd.layouts[3].stride = 32;
    vd.layouts[3].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
      [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
      fprintf(stderr, "FAIL[newRenderPipelineState]: %s\n",
        err.localizedDescription.UTF8String);
      return 1;
    }
    printf("pipeline state created\n");

    /* VS constants: identity MVP rows (shader does dp4(pos, cF[row])) */
    std::vector<float> vsConstants(256 * 4, 0.f);
    vsConstants[0 * 4 + 0] = 1.f;
    vsConstants[1 * 4 + 1] = 1.f;
    vsConstants[2 * 4 + 2] = 1.f;
    vsConstants[3 * 4 + 3] = 1.f;
    id<MTLBuffer> vsCf = [device newBufferWithBytes:vsConstants.data()
      length:vsConstants.size() * 4 options:MTLResourceStorageModeShared];

    /* clipPlanes: {u32 count; pad to 16; 6x float4} all zero */
    std::vector<uint8_t> clipPlanes(16 + 6 * 16, 0);
    id<MTLBuffer> vsClip = [device newBufferWithBytes:clipPlanes.data()
      length:clipPlanes.size() options:MTLResourceStorageModeShared];

    /* pointArgs: {i32 isSprite; f32 size; f32 min; f32 max} */
    struct { int32_t sprite; float size, sizeMin, sizeMax; }
      pointArgs = { 0, 1.f, 0.f, 64.f };
    id<MTLBuffer> vsPoint = [device newBufferWithBytes:&pointArgs
      length:sizeof(pointArgs) options:MTLResourceStorageModeShared];

    /* PS constants: cF[0] = tint (1,1,1,1) */
    std::vector<float> psConstants(224 * 4, 0.f);
    psConstants[0] = psConstants[1] = psConstants[2] = psConstants[3] = 1.f;
    id<MTLBuffer> psCf = [device newBufferWithBytes:psConstants.data()
      length:psConstants.size() * 4 options:MTLResourceStorageModeShared];

    /* alphaTest: compareOp=8 (D3DCMP_ALWAYS -> shader returns true) */
    struct { uint32_t compareOp, precisionBits, alphaRef; }
      alphaTest = { 8u, 8u, 0u };
    id<MTLBuffer> psAlpha = [device newBufferWithBytes:&alphaTest
      length:sizeof(alphaTest) options:MTLResourceStorageModeShared];

    /* triangle covering most of the target, r/g/b corners */
    float verts[] = {
      -1.f, -1.f, 0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
       3.f, -1.f, 0.f, 1.f,  0.f, 1.f, 0.f, 1.f,
      -1.f,  3.f, 0.f, 1.f,  0.f, 0.f, 1.f, 1.f,
    };
    id<MTLBuffer> vb = [device newBufferWithBytes:verts length:sizeof(verts)
      options:MTLResourceStorageModeShared];

    const uint32_t W = 64, H = 64;
    MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
      width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setVertexBuffer:vsCf offset:0 atIndex:0];
    [enc setVertexBuffer:vsClip offset:0 atIndex:1];
    [enc setVertexBuffer:vsPoint offset:0 atIndex:2];
    [enc setVertexBuffer:vb offset:0 atIndex:3];
    [enc setFragmentBuffer:psCf offset:0 atIndex:0];
    [enc setFragmentBuffer:psAlpha offset:0 atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    if (cb.error) {
      fprintf(stderr, "FAIL[commandBuffer]: %s\n",
        cb.error.localizedDescription.UTF8String);
      return 1;
    }

    std::vector<uint8_t> pixels(W * H * 4);
    [target getBytes:pixels.data() bytesPerRow:W * 4
      fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];

    auto px = [&](uint32_t x, uint32_t y) {
      const uint8_t* p = &pixels[(y * W + x) * 4];
      return std::array<uint8_t, 4>{ p[2], p[1], p[0], p[3] }; /* ->RGBA */
    };

    /* bottom-left ~red, bottom-right-ish ~green, top-left-ish ~blue
     * (Metal texture row 0 = top; clip -1,-1 = bottom-left) */
    auto redPt   = px(2, H - 3);
    auto greenPt = px(W - 3, H - 3);
    auto bluePt  = px(2, 2);

    printf("red pt   RGBA: %u %u %u %u\n", redPt[0], redPt[1], redPt[2], redPt[3]);
    printf("green pt RGBA: %u %u %u %u\n", greenPt[0], greenPt[1], greenPt[2], greenPt[3]);
    printf("blue pt  RGBA: %u %u %u %u\n", bluePt[0], bluePt[1], bluePt[2], bluePt[3]);

    bool pass =
      redPt[0]   > 180 && redPt[1]   < 80 && redPt[2]   < 80 &&
      greenPt[1] > 120 && greenPt[0] < 130 &&
      bluePt[2]  > 120 && bluePt[0]  < 130;

    printf(pass ? "PASS\n" : "FAIL: colors wrong\n");
    return pass ? 0 : 1;
  }
}
