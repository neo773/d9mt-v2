#include "dxvk_backend.h"

#include <cstdio>
#include <vector>

#include <windows.h>

// Metal is reached ONLY through the winemetal obj_handle_t ABI — never
// metal-cpp (that is host tooling). See SHIM_SPEC.md §4.
#include "winemetal.h"

// Generated at build time from the embedded triangle MSL:
//   d9mt_triangle_metallib[] + d9mt_triangle_metallib_len.
// Built into build/triangle_metallib.h; the build adds that dir to -I.
#include "triangle_metallib.h"

namespace dxvk {

  namespace {
    // The shim has no logger wired this early; mirror metal_backend's file log
    // so initialize() failures are diagnosable from a fresh boot.
    void logLine(const char* message) {
      FILE* file = fopen("C:\\d9mt-test\\v2.log", "a");
      if (file) {
        fprintf(file, "[dxvk_backend] %s\n", message);
        fclose(file);
      }
    }
  }

  D9mtBackend* g_activeBackend = nullptr;

  D9mtBackend::D9mtBackend() = default;

  D9mtBackend::~D9mtBackend() {
    if (m_pipelineState) NSObject_release(m_pipelineState);
    if (m_library)       NSObject_release(m_library);
    if (m_queue)         NSObject_release(m_queue);
    // Device, layer, and view are owned by the windowing system / winemetal.
  }

  bool D9mtBackend::initialize() {
    // Pick the system's first Metal device.
    uint64_t devices = WMTCopyAllDevices();
    if (!devices || NSArray_count(devices) == 0) {
      logLine("initialize: no Metal devices");
      return false;
    }
    m_device = NSArray_object(devices, 0);
    NSObject_retain(m_device);
    NSObject_release(devices);

    m_queue = MTLDevice_newCommandQueue(m_device, 64);
    if (!m_queue) {
      logLine("initialize: newCommandQueue failed");
      return false;
    }

    // Build the one fixed-function triangle pipeline from the embedded
    // metallib. The MSL entry points are named triangleVertex/triangleFragment
    // (kept verbatim — the build-time metallib uses these names).
    uint64_t libraryData = DispatchData_alloc_init(
        reinterpret_cast<uint64_t>(d9mt_triangle_metallib),
        d9mt_triangle_metallib_len);
    uint64_t error = 0;
    m_library = MTLDevice_newLibrary(m_device, libraryData, &error);
    NSObject_release(libraryData);
    if (!m_library) {
      logLine("initialize: newLibrary failed");
      return false;
    }
    uint64_t vertexFunction = MTLLibrary_newFunction(m_library, "triangleVertex");
    uint64_t fragmentFunction = MTLLibrary_newFunction(m_library, "triangleFragment");
    if (!vertexFunction || !fragmentFunction) {
      logLine("initialize: newFunction failed");
      return false;
    }

    WMTRenderPipelineInfo pipelineInfo = {};
    pipelineInfo.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    pipelineInfo.colors[0].write_mask = WMTColorWriteMaskAll;
    pipelineInfo.colors[0].blending_enabled = false;
    pipelineInfo.rasterization_enabled = true;
    pipelineInfo.raster_sample_count = 1;
    pipelineInfo.vertex_function = vertexFunction;
    pipelineInfo.fragment_function = fragmentFunction;
    pipelineInfo.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    pipelineInfo.max_tessellation_factor = 16;
    error = 0;
    m_pipelineState = MTLDevice_newRenderPipelineState(m_device, &pipelineInfo, &error);
    NSObject_release(vertexFunction);
    NSObject_release(fragmentFunction);
    if (!m_pipelineState) {
      logLine("initialize: newRenderPipelineState failed");
      return false;
    }

    logLine("initialize: OK");
    return true;
  }

  bool D9mtBackend::attachWindow(void* windowHandle, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    // Attach a CAMetalLayer to the Wine window for presentation.
    m_view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(windowHandle),
                                     m_device, &m_layer);
    if (!m_view || !m_layer) {
      logLine("attachWindow: CreateMetalViewFromHWND failed");
      return false;
    }
    WMTLayerProps props = {};
    MetalLayer_getProps(m_layer, &props);
    props.device = m_device;
    props.contents_scale = 1.0;
    props.drawable_width = m_width;
    props.drawable_height = m_height;
    props.opaque = true;
    props.display_sync_enabled = true;
    props.framebuffer_only = true;
    props.pixel_format = WMTPixelFormatBGRA8Unorm;
    MetalLayer_setProps(m_layer, &props);

    logLine("attachWindow: OK");
    return true;
  }

  uint64_t D9mtBackend::createSharedBuffer(uint64_t byteCount, void** cpuPointerOut,
                                           uint64_t* gpuAddressOut) {
    if (cpuPointerOut)
      *cpuPointerOut = nullptr;
    if (gpuAddressOut)
      *gpuAddressOut = 0;

    // Page-aligned host storage the CPU writes and the GPU reads. Using
    // VirtualAlloc + StorageModeShared keeps writes coherent without an explicit
    // flush, matching the proven metal_backend vertex arena.
    void* memory = VirtualAlloc(nullptr, byteCount, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!memory) {
      logLine("createSharedBuffer: VirtualAlloc failed");
      return 0;
    }

    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = byteCount;
    bufferInfo.options = WMTResourceStorageModeShared;
    bufferInfo.memory.set(memory);
    uint64_t buffer = MTLDevice_newBuffer(m_device, &bufferInfo);
    if (!buffer) {
      logLine("createSharedBuffer: newBuffer failed");
      VirtualFree(memory, 0, MEM_RELEASE);
      return 0;
    }

    if (cpuPointerOut)
      *cpuPointerOut = memory;
    if (gpuAddressOut)
      *gpuAddressOut = bufferInfo.gpu_address;   // filled by MTLDevice_newBuffer
    return buffer;
  }

  void D9mtBackend::renderAndPresent(const D9mtDraw& draw, bool clear, uint32_t clearColorArgb) {
    if (!isValid() || !m_layer)
      return;

    uint64_t pool = NSAutoreleasePool_alloc_init();

    uint64_t drawable = MetalLayer_nextDrawable(m_layer);
    if (!drawable) {
      NSObject_release(pool);
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(drawable);
    uint64_t commandBuffer = MTLCommandQueue_commandBuffer(m_queue);

    // Render pass: clear the drawable (when asked), then draw the triangle list.
    // The clear color is a raw D3DCOLOR (0xAARRGGBB).
    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    uint64_t encoder = MTLCommandBuffer_renderCommandEncoder(commandBuffer, &renderPass);
    if (encoder) {
      if (draw.vertexCount && draw.vertexBuffer) {
        // POD wmtcmd packet chain: set pipeline -> bind vertex buffer -> draw.
        wmtcmd_render_setpso setPipeline = {};
        wmtcmd_render_setbuffer setVertexBuffer = {};
        wmtcmd_render_draw drawCmd = {};

        setPipeline.type = WMTRenderCommandSetPSO;
        setPipeline.next.set(&setVertexBuffer);
        setPipeline.pso = m_pipelineState;

        setVertexBuffer.type = WMTRenderCommandSetVertexBuffer;
        setVertexBuffer.next.set(&drawCmd);
        setVertexBuffer.buffer = draw.vertexBuffer;
        setVertexBuffer.offset = draw.vertexOffset;
        setVertexBuffer.index = 0;

        drawCmd.type = WMTRenderCommandDraw;
        drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
        drawCmd.vertex_start = 0;
        drawCmd.vertex_count = draw.vertexCount;
        drawCmd.instance_count = 1;
        drawCmd.base_instance = 0;

        MTLRenderCommandEncoder_encodeCommands(
            encoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
      }
      MTLCommandEncoder_endEncoding(encoder);
    }

    MTLCommandBuffer_presentDrawable(commandBuffer, drawable);
    MTLCommandBuffer_commit(commandBuffer);
    MTLCommandBuffer_waitUntilCompleted(commandBuffer);  // milestone: block

    NSObject_release(pool);
  }

  void D9mtBackend::renderAndPresent(const D9mtPipelineDraw& draw) {
    if (!m_layer || !draw.pipelineState)
      return;

    uint64_t pool = NSAutoreleasePool_alloc_init();

    uint64_t drawable = MetalLayer_nextDrawable(m_layer);
    if (!drawable) {
      NSObject_release(pool);
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(drawable);
    uint64_t commandBuffer = MTLCommandQueue_commandBuffer(m_queue);

    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = draw.clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((draw.clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((draw.clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((draw.clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((draw.clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    uint64_t encoder = MTLCommandBuffer_renderCommandEncoder(commandBuffer, &renderPass);
    if (encoder) {
      // Build the POD command chain: set pipeline -> bind every buffer -> draw.
      // The buffer packets live in a vector with stable addresses (reserved up
      // front) so the `next` links stay valid until encodeCommands consumes them.
      wmtcmd_render_setpso setPipeline = {};
      wmtcmd_render_draw   drawCmd     = {};
      std::vector<wmtcmd_render_setbuffer>    bufferCommands(draw.bindingCount);
      std::vector<wmtcmd_render_useresource>  residencyCommands(draw.residentCount);

      setPipeline.type = WMTRenderCommandSetPSO;
      setPipeline.pso  = draw.pipelineState;

      wmtcmd_base* tail = reinterpret_cast<wmtcmd_base*>(&setPipeline);

      // Make every argument-buffer-referenced resource resident so the GPU can
      // follow the addresses written into the argument buffers.
      for (uint32_t i = 0; i < draw.residentCount; i++) {
        wmtcmd_render_useresource& cmd = residencyCommands[i];
        cmd.type     = WMTRenderCommandUseResource;
        cmd.resource = draw.residentBuffers[i];
        cmd.usage    = WMTResourceUsageRead;
        cmd.stages   = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
        tail->next.set(&cmd);
        tail = reinterpret_cast<wmtcmd_base*>(&cmd);
      }

      for (uint32_t i = 0; i < draw.bindingCount; i++) {
        const D9mtBufferBinding& binding = draw.bindings[i];
        wmtcmd_render_setbuffer& cmd = bufferCommands[i];
        cmd.type   = binding.fragmentStage ? WMTRenderCommandSetFragmentBuffer
                                            : WMTRenderCommandSetVertexBuffer;
        cmd.buffer = binding.buffer;
        cmd.offset = binding.offset;
        cmd.index  = uint8_t(binding.index);
        tail->next.set(&cmd);
        tail = reinterpret_cast<wmtcmd_base*>(&cmd);
      }

      drawCmd.type           = WMTRenderCommandDraw;
      drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
      drawCmd.vertex_start   = 0;
      drawCmd.vertex_count   = draw.vertexCount;
      drawCmd.instance_count = 1;
      drawCmd.base_instance  = 0;
      tail->next.set(&drawCmd);

      MTLRenderCommandEncoder_encodeCommands(
          encoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
      MTLCommandEncoder_endEncoding(encoder);
    }

    MTLCommandBuffer_presentDrawable(commandBuffer, drawable);
    MTLCommandBuffer_commit(commandBuffer);
    MTLCommandBuffer_waitUntilCompleted(commandBuffer);

    NSObject_release(pool);
  }

  // --- Frame lifecycle (Stage A) ---------------------------------------------
  // Split renderAndPresent's per-draw acquire/encode/present/stall into a
  // per-FRAME shape: beginFrame opens one drawable + command buffer + encoder,
  // appendDraw accumulates draws into that single encoder, and
  // endFrameAndPresent ends/presents/commits/waits once.

  void D9mtBackend::beginFrame(bool clear, uint32_t clearColorArgb) {
    // Remember the clear request so a draw-less (clear-only) frame can still
    // present a cleared image when endFrameAndPresent opens the frame itself.
    m_pendingClear = clear;
    m_pendingClearColor = clearColorArgb;

    // Idempotent within a frame: only the first call per frame opens the
    // encoder; later beginFrame calls (e.g. one before every draw) are no-ops.
    if (m_frameOpen)
      return;
    if (!m_layer)
      return;

    // One autorelease pool held across the whole frame (released at frame end).
    m_framePool = NSAutoreleasePool_alloc_init();

    m_frameDrawable = MetalLayer_nextDrawable(m_layer);
    if (!m_frameDrawable) {
      // No drawable available — leave the frame closed and drop this pool.
      NSObject_release(m_framePool);
      m_framePool = 0;
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(m_frameDrawable);

    // Render pass identical to renderAndPresent: clear the drawable (when
    // asked) then load into the open encoder. clearColorArgb is raw D3DCOLOR.
    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    m_frameCommandBuffer = MTLCommandQueue_commandBuffer(m_queue);
    m_frameEncoder = MTLCommandBuffer_renderCommandEncoder(m_frameCommandBuffer, &renderPass);
    m_frameOpen = true;
  }

  void D9mtBackend::appendDraw(const D9mtPipelineDraw& draw) {
    if (!m_frameOpen || !m_frameEncoder || !draw.pipelineState)
      return;

    // Same POD command chain as renderAndPresent(D9mtPipelineDraw): set pipeline
    // -> make resources resident -> bind every buffer -> draw. The buffer/
    // residency packets live in scratch with stable addresses (sized up front)
    // so the `next` links stay valid until encodeCommands consumes them.
    // encodeCommands appends onto the still-open encoder — no end/present here.
    //
    // The scratch is function-static (the shim replays draws on one thread): its
    // capacity persists across draws, so a steady draw stream allocates nothing.
    static std::vector<wmtcmd_render_setbuffer>   bufferCommands;
    static std::vector<wmtcmd_render_useresource> residencyCommands;
    bufferCommands.assign(draw.bindingCount, {});
    residencyCommands.assign(draw.residentCount, {});

    wmtcmd_render_setpso setPipeline = {};
    wmtcmd_render_draw   drawCmd     = {};

    setPipeline.type = WMTRenderCommandSetPSO;
    setPipeline.pso  = draw.pipelineState;

    wmtcmd_base* tail = reinterpret_cast<wmtcmd_base*>(&setPipeline);

    // Make every argument-buffer-referenced resource resident so the GPU can
    // follow the addresses written into the argument buffers.
    for (uint32_t i = 0; i < draw.residentCount; i++) {
      wmtcmd_render_useresource& cmd = residencyCommands[i];
      cmd.type     = WMTRenderCommandUseResource;
      cmd.resource = draw.residentBuffers[i];
      cmd.usage    = WMTResourceUsageRead;
      cmd.stages   = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
      tail->next.set(&cmd);
      tail = reinterpret_cast<wmtcmd_base*>(&cmd);
    }

    for (uint32_t i = 0; i < draw.bindingCount; i++) {
      const D9mtBufferBinding& binding = draw.bindings[i];
      wmtcmd_render_setbuffer& cmd = bufferCommands[i];
      cmd.type   = binding.fragmentStage ? WMTRenderCommandSetFragmentBuffer
                                          : WMTRenderCommandSetVertexBuffer;
      cmd.buffer = binding.buffer;
      cmd.offset = binding.offset;
      cmd.index  = uint8_t(binding.index);
      tail->next.set(&cmd);
      tail = reinterpret_cast<wmtcmd_base*>(&cmd);
    }

    drawCmd.type           = WMTRenderCommandDraw;
    drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
    drawCmd.vertex_start   = 0;
    drawCmd.vertex_count   = draw.vertexCount;
    drawCmd.instance_count = 1;
    drawCmd.base_instance  = 0;
    tail->next.set(&drawCmd);

    MTLRenderCommandEncoder_encodeCommands(
        m_frameEncoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
  }

  void D9mtBackend::endFrameAndPresent() {
    // Clear-only frame: nobody opened the encoder, so open one now using the
    // last clear seen by beginFrame so the cleared image still gets presented.
    if (!m_frameOpen) {
      beginFrame(m_pendingClear, m_pendingClearColor);
      if (!m_frameOpen)
        return;  // still couldn't open (e.g. no drawable) — nothing to present.
    }

    MTLCommandEncoder_endEncoding(m_frameEncoder);
    MTLCommandBuffer_presentDrawable(m_frameCommandBuffer, m_frameDrawable);
    MTLCommandBuffer_commit(m_frameCommandBuffer);
    MTLCommandBuffer_waitUntilCompleted(m_frameCommandBuffer);  // Stage A: block

    if (m_framePool)
      NSObject_release(m_framePool);

    // Reset frame state — zero handles mean no frame is in flight.
    m_frameDrawable = m_frameCommandBuffer = m_frameEncoder = m_framePool = 0;
    m_frameOpen = false;
  }

}
