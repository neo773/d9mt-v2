#pragma once

// D9mtBackend — the single Metal backend the shim is built on. Owns the
// winemetal device/queue, the swapchain layer for the window, the one hardcoded
// fixed-function MSL pipeline (triangleVertex/triangleFragment), and a shared
// vertex arena. DxvkDevice owns one; DxvkContext records draws into it; the
// swapchain presents through it. Metal is reached only via the winemetal ABI.
//
// This is the foundation every other shim module leans on (SHIM_SPEC.md).

#include <cstdint>

namespace dxvk {

  // A render target the backend can clear + draw into and present. For the
  // backbuffer this wraps a real Metal texture; the drawable is fetched per
  // present from the swapchain layer.
  struct D9mtRenderTarget {
    uint64_t texture = 0;  // obj_handle_t MTLTexture (0 = use next drawable)
    uint32_t width   = 0;
    uint32_t height  = 0;
  };

  // One recorded draw: a slice of the shared vertex arena + a vertex count.
  struct D9mtDraw {
    uint64_t vertexBuffer = 0;  // obj_handle_t MTLBuffer
    uint64_t vertexOffset = 0;
    uint32_t vertexCount  = 0;
  };

  // One Metal buffer bound to a shader stage at a given index (vertex stream,
  // push block, argument buffer, or sampler heap — the caller decides the index
  // and stage from the shader's reflection).
  struct D9mtBufferBinding {
    uint64_t buffer        = 0;
    uint64_t offset        = 0;
    uint32_t index         = 0;
    bool     fragmentStage = false;   // false = bind to the vertex stage
  };

  // A draw through a real (translated-shader) render pipeline: bind the pipeline
  // and the given buffers, then draw a triangle list. Replaces the hardcoded-MSL
  // path once shaders are translated; see DxvkContext::draw.
  struct D9mtPipelineDraw {
    uint64_t                  pipelineState  = 0;   // MTLRenderPipelineState
    const D9mtBufferBinding*  bindings       = nullptr;
    uint32_t                  bindingCount   = 0;
    // Buffers referenced by argument buffers (by GPU address) — made resident
    // via useResource so the GPU can follow those addresses.
    const uint64_t*           residentBuffers = nullptr;
    uint32_t                  residentCount   = 0;
    uint32_t                  vertexCount    = 0;
    bool                      clear          = false;
    uint32_t                  clearColorArgb = 0;
  };

  class D9mtBackend {
  public:
    D9mtBackend();
    ~D9mtBackend();

    D9mtBackend(const D9mtBackend&) = delete;
    D9mtBackend& operator=(const D9mtBackend&) = delete;

    // Creates the winemetal device/queue and the FF pipeline. The swapchain
    // layer is attached later via attachWindow once the HWND is known.
    bool initialize();
    bool isValid() const { return m_pipelineState != 0; }

    uint64_t device() const { return m_device; }
    uint64_t queue()  const { return m_queue; }

    // Attaches a CAMetalLayer to the Wine window for presentation.
    bool attachWindow(void* windowHandle, uint32_t width, uint32_t height);

    // Allocates `byteCount` from a shared, CPU-writable, GPU-visible Metal
    // buffer. Returns the buffer handle + a CPU pointer; writes are coherent.
    // When gpuAddressOut is non-null, also returns the buffer's GPU address
    // (needed to reference it from an argument buffer).
    uint64_t createSharedBuffer(uint64_t byteCount, void** cpuPointerOut,
                                uint64_t* gpuAddressOut = nullptr);

    // Records one render pass into the next drawable: clear to clearColorArgb
    // (raw D3DCOLOR), draw the given list as a triangle list, present, wait.
    // Uses the hardcoded MSL pipeline (fallback / bring-up path).
    void renderAndPresent(const D9mtDraw& draw, bool clear, uint32_t clearColorArgb);

    // Same, but with a caller-built render pipeline + arbitrary buffer bindings
    // (the real translated-shader path).
    void renderAndPresent(const D9mtPipelineDraw& draw);

  private:
    uint64_t m_device        = 0;
    uint64_t m_queue         = 0;
    uint64_t m_layer         = 0;
    uint64_t m_view          = 0;
    uint64_t m_library       = 0;
    uint64_t m_pipelineState = 0;
    uint32_t m_width         = 0;
    uint32_t m_height        = 0;
  };

  // The one backend the active device created. Set in DxvkDevice::initialize so
  // the WSI surface stub (dxvk_wsi_stubs.cpp) can attach the Metal layer to the
  // HWND when the frontend's surface callback fires — that callback is the only
  // place the window handle is exposed to the shim.
  extern D9mtBackend* g_activeBackend;

}
