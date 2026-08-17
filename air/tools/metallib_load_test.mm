#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.metallib>\n", argv[0]);
    return 2;
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }

    NSData* data = [NSData dataWithContentsOfFile:
      [NSString stringWithUTF8String:argv[1]]];
    if (!data) { fprintf(stderr, "FAIL: cannot read %s\n", argv[1]); return 1; }

    dispatch_data_t ddata = dispatch_data_create(
      data.bytes, data.length, dispatch_get_main_queue(),
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:ddata error:&err];
    if (!lib) {
      fprintf(stderr, "FAIL[newLibraryWithData]: %s\n",
        err.localizedDescription.UTF8String);
      return 1;
    }
    printf("library loaded, functions: %s\n",
      lib.functionNames.description.UTF8String);

    id<MTLFunction> vs = [lib newFunctionWithName:@"vs_main"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"fs_main"];
    if (!vs || !fs) {
      fprintf(stderr, "FAIL: vs_main=%p fs_main=%p\n", vs, fs);
      return 1;
    }

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 16;
    vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 32;
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    id<MTLRenderPipelineState> pso =
      [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
      fprintf(stderr, "FAIL[newRenderPipelineState]: %s\n",
        err.localizedDescription.UTF8String);
      return 1;
    }
    printf("pipeline state created\n");

    const uint32_t W = 64, H = 64;
    MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
      width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];

    float verts[] = {
      -1.f, -1.f, 0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
       3.f, -1.f, 0.f, 1.f,  0.f, 1.f, 0.f, 1.f,
      -1.f,  3.f, 0.f, 1.f,  0.f, 0.f, 1.f, 1.f,
    };
    id<MTLBuffer> vb = [device newBufferWithBytes:verts length:sizeof(verts)
      options:MTLResourceStorageModeShared];

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
    [enc setVertexBuffer:vb offset:0 atIndex:0];
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
      return std::array<uint8_t, 4>{ p[2], p[1], p[0], p[3] };
    };
    auto center = px(W / 2, H / 2);
    printf("center pixel RGBA: %u %u %u %u\n",
      center[0], center[1], center[2], center[3]);

    bool pass = (center[0] + center[1] + center[2]) > 60;
    printf(pass ? "PASS\n" : "FAIL: center pixel black\n");
    return pass ? 0 : 1;
  }
}
