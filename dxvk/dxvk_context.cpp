// DxvkContext shim implementation — see dxvk_context.h.
//
// Only the live fixed-function-triangle path has real bodies here. The state
// setters and the long tail of recorded commands are inline no-ops in the
// header. This file folds the recorded clear/bind/draw into one Metal
// renderAndPresent() per draw call.

#include "dxvk_context.h"

#include "dxvk_device.h"   // DxvkDevice::backend() — owner of the D9mtBackend

#include <algorithm>
#include <cstdio>

namespace {
  // First Metal vertex-buffer slot used for D3D9 vertex streams (stream b binds
  // at buffer(VertexBufferBase + b)). 14 leaves the low slots for the argument
  // buffer / push / sampler-heap indices SPIRV-Cross assigns. Matches v1.
  constexpr uint32_t VertexBufferBase = 14u;

  // Descriptor set the FF spec-constant buffer lives in (D3D9FFShaderModuleSet
  // SpecDataSet). Its data comes from setSpecConstants, NOT bindUniformBuffer,
  // so resources in this set bind the uploaded spec buffer rather than a CBV.
  constexpr uint32_t SpecDataSet = 3u;

  void logLine(const char* message) {
    if (FILE* file = std::fopen("C:\\d9mt-test\\v2.log", "a")) {
      std::fprintf(file, "[context] %s\n", message);
      std::fclose(file);
    }
  }
}

namespace dxvk {

  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device(device) {
    // The device owns the single Metal backend; the context records into it.
    // backend() is the agreed accessor on the device shim (SHIM_SPEC: "DxvkDevice
    // owns it; DxvkContext records into it").
    m_backend = device->backend();
  }


  DxvkContext::~DxvkContext() {
  }


  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) {
    (void) cmdList;

    // Reset per-frame recorded state at the start of each frame. Bound vertex
    // streams / uniform buffers persist (the frontend rebinds what changes).
    m_clearPending   = false;
    m_clearColorArgb = 0;
  }


  Rc<DxvkCommandList> DxvkContext::endRecording(const VkDebugUtilsLabelEXT* reason) {
    (void) reason;
    // Nothing is buffered: draw() already presented. The frontend does not use
    // the returned command list on this path.
    return nullptr;
  }


  void DxvkContext::flushCommandList(
      const VkDebugUtilsLabelEXT* reason,
            DxvkSubmitStatus*     status) {
    (void) reason;
    (void) status;
    // The present lambda calls this after the blitter; present already happened
    // inside draw()/blitter present, so there is nothing to submit. DxvkSubmitStatus
    // defaults to VK_SUCCESS, so leaving *status untouched is the success path.
  }


  Rc<DxvkCommandList> DxvkContext::beginExternalRendering() {
    // The swapchain present lambda passes the result to the blitter as
    // "context objects". With no Vulkan command list, the shim hands back null;
    // the blitter shim is expected to record through the backend directly.
    return nullptr;
  }


  void DxvkContext::bindRenderTargets(
            DxvkRenderTargets&&   targets,
            VkImageAspectFlags    feedbackLoop) {
    (void) feedbackLoop;
    // The backbuffer color target is color[0]. The backend presents to the next
    // drawable, so we only need to know a target was bound; the actual Metal
    // texture is resolved by the backend at present time. We keep the move to
    // honour the rvalue contract and drop it (no per-RT state needed yet).
    (void) targets;
  }


  void DxvkContext::clearRenderTarget(
      const Rc<DxvkImageView>&    imageView,
            VkImageAspectFlags    clearAspects,
            VkClearValue          clearValue,
            VkImageAspectFlags    discardAspects) {
    (void) imageView;
    (void) discardAspects;

    // Only a color clear matters for the triangle milestone.
    if (!(clearAspects & VK_IMAGE_ASPECT_COLOR_BIT))
      return;

    // VkClearValue carries normalized float RGBA. The backend wants a raw
    // D3DCOLOR-style ARGB word, so pack [0,1] floats back into 8-bit channels.
    const float* rgba = clearValue.color.float32;

    auto toByte = [](float v) -> uint32_t {
      float c = std::min(1.0f, std::max(0.0f, v));
      return uint32_t(c * 255.0f + 0.5f);
    };

    uint32_t a = toByte(rgba[3]);
    uint32_t r = toByte(rgba[0]);
    uint32_t g = toByte(rgba[1]);
    uint32_t b = toByte(rgba[2]);

    m_clearColorArgb = (a << 24) | (r << 16) | (g << 8) | b;
    m_clearPending   = true;
  }


  void DxvkContext::bindVertexBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            uint32_t              stride) {
    (void) stride;

    // Remember every bound stream (not just binding 0) — the FF input layout
    // also references a null stream for unused attributes, and Metal drops the
    // draw if any referenced vertex buffer index is unbound. An empty slice
    // (the frontend's unbind) yields handle 0, which we skip at draw time.
    BoundVertexStream& stream = m_vertexBuffers[binding];
    stream.handle = buffer.bufferHandle();
    stream.offset = buffer.offset();
  }


  void DxvkContext::setViewports(
            uint32_t              viewportCount,
      const DxvkViewport*         viewports) {
    (void) viewportCount;
    (void) viewports;
    // The triangle is emitted in NDC, so the backend ignores viewport. The call
    // is kept so the frontend's recorded setViewports binds to a real method.
  }


  const DxvkContext::CompiledShaderStage* DxvkContext::resolveStage(const Rc<DxvkShader>& shader) {
    if (shader == nullptr)
      return nullptr;

    uint32_t cookie = uint32_t(shader->getCookie());
    auto cached = m_compiledStages.find(cookie);
    if (cached != m_compiledStages.end())
      return cached->second.function ? &cached->second : nullptr;

    CompiledShaderStage stage;
    // Translate (no binding remap — the shim runs no Vulkan pipeline) then
    // compile + specialize. samplerHeapSet ~0u: the FF triangle binds no
    // textures, so the heap is never referenced.
    SpirvCodeBuffer code = shader->getCode(nullptr, nullptr);
    stage.translation = convertSpirvToMsl(
      code, shader->getLayout(), shader->metadata().stage, ~0u);

    if (stage.translation.ok) {
      uint64_t device = m_backend->device();
      stage.library = metalCompileLibrary(device, stage.translation.source);
      if (stage.library) {
        stage.function = metalResolveFunction(stage.library,
          stage.translation.reflection.specConstants,
          m_specConstantData.data(), uint32_t(m_specConstantData.size()));
      }
    }
    if (!stage.function)
      logLine(("shader translate/compile failed: " + shader->debugName()).c_str());

    auto& slot = m_compiledStages.emplace(cookie, std::move(stage)).first->second;
    return slot.function ? &slot : nullptr;
  }


  MetalVertexLayout DxvkContext::buildVertexLayout() const {
    MetalVertexLayout layout;

    for (const DxvkVertexInput& input : m_vertexAttributes) {
      DxvkVertexAttribute attribute = input.attribute();
      MetalVertexAttribute metal;
      metal.metalFormat = metalVertexFormatFromVulkan(uint32_t(attribute.format));
      metal.offset      = attribute.offset;
      metal.bufferIndex = VertexBufferBase + attribute.binding;
      metal.location    = attribute.location;
      layout.attributes.push_back(metal);
    }

    for (const DxvkVertexInput& input : m_vertexBindings) {
      DxvkVertexBinding binding = input.binding();
      MetalVertexStream stream;
      stream.bufferIndex  = VertexBufferBase + binding.binding;
      stream.stride       = binding.extent ? binding.extent : 4u;  // Metal rejects stride 0
      stream.stepFunction = binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2u : 1u;
      stream.stepRate     = 1u;
      layout.streams.push_back(stream);
    }

    return layout;
  }


  uint64_t DxvkContext::uploadPushData(const MslReflection& reflection, uint32_t slot) {
    if (!reflection.pushDataSize)
      return 0u;

    if (!m_pushUploadBuffer[slot]) {
      m_pushUploadBuffer[slot] = m_backend->createSharedBuffer(
        MaxTotalPushDataSize, &m_pushUploadBufferCpu[slot]);
    }
    if (!m_pushUploadBufferCpu[slot])
      return 0u;

    auto* dst = static_cast<uint8_t*>(m_pushUploadBufferCpu[slot]);
    std::memset(dst, 0, reflection.pushDataSize);

    // Each push block copies `size` bytes from the constant store (where the
    // frontend wrote them, at absolute push offset srcOffset) into push space
    // at dstOffset — exactly the layout the translated shader reads.
    for (const MslPushBlock& block : reflection.pushBlocks) {
      if (block.srcOffset + block.size <= m_pushConstantStore.size()
       && block.dstOffset + block.size <= MaxTotalPushDataSize) {
        std::memcpy(dst + block.dstOffset,
                    m_pushConstantStore.data() + block.srcOffset, block.size);
      }
    }
    return m_pushUploadBuffer[slot];
  }


  uint64_t DxvkContext::uploadSpecData() {
    if (m_specConstantData.empty())
      return 0u;

    size_t bytes = m_specConstantData.size() * sizeof(uint32_t);
    if (!m_specDataBuffer) {
      m_specDataBuffer = m_backend->createSharedBuffer(
        MaxNumSpecConstants * sizeof(uint32_t), &m_specDataBufferCpu, &m_specDataAddress);
    }
    if (!m_specDataBufferCpu)
      return 0u;

    std::memcpy(m_specDataBufferCpu, m_specConstantData.data(), bytes);
    return m_specDataAddress;
  }


  void DxvkContext::bindArgumentBuffers(
    const MslReflection&             reflection,
          bool                       fragmentStage,
          uint32_t                   stageKey,
          std::vector<D9mtBufferBinding>& bindings,
          std::vector<uint64_t>&     residentBuffers) {
    if (reflection.resources.empty())
      return;

    // Group resources by descriptor set; each set becomes one argument buffer
    // bound at buffer(set). entryCount = highest AB slot + 1 in that set.
    uint32_t entryCountPerSet[16] = { };
    for (const MslResourceRef& ref : reflection.resources) {
      if (ref.set < 16u)
        entryCountPerSet[ref.set] = std::max<uint32_t>(entryCountPerSet[ref.set], ref.abId + 1u);
    }

    for (uint32_t set = 0; set < 16u; set++) {
      uint32_t entryCount = entryCountPerSet[set];
      if (!entryCount)
        continue;

      uint32_t bufferKey = (stageKey << 8) | set;
      ArgumentBuffer& argumentBuffer = m_argumentBuffers[bufferKey];
      if (!argumentBuffer.buffer) {
        argumentBuffer.buffer = m_backend->createSharedBuffer(
          16u * 8u, &argumentBuffer.cpu);   // 16 slots is the most any FF set uses
      }
      if (!argumentBuffer.cpu)
        continue;

      // Each AB slot holds the GPU address of the uniform buffer bound at the
      // resource's DXVK slot (textures/samplers are off the FF triangle path, so
      // their slots stay zero — a null descriptor the shader must not deref).
      auto* slots = static_cast<uint64_t*>(argumentBuffer.cpu);
      std::memset(slots, 0, entryCount * sizeof(uint64_t));
      for (const MslResourceRef& ref : reflection.resources) {
        if (ref.set != set || ref.abId >= entryCount)
          continue;
        if (!ref.isUniformBuffer)
          continue;

        if (ref.set == SpecDataSet) {
          // Spec-constant buffer: filled from setSpecConstants, not a CBV.
          uint64_t address = uploadSpecData();
          if (address) {
            slots[ref.abId] = address;
            residentBuffers.push_back(m_specDataBuffer);
          }
        } else {
          auto bound = m_uniformBuffers.find(ref.slot);
          if (bound != m_uniformBuffers.end() && bound->second.address) {
            slots[ref.abId] = bound->second.address;
            residentBuffers.push_back(bound->second.handle);
          }
        }
      }

      bindings.push_back({ argumentBuffer.buffer, 0u, set, fragmentStage });
    }
  }


  void DxvkContext::draw(
            uint32_t              count,
      const VkDrawIndirectCommand* draws) {
    if (!m_backend || !count || !draws)
      return;

    const VkDrawIndirectCommand& d = draws[0];

    // Translate + compile the bound shaders (cached). The vertex shader is
    // mandatory; the fragment shader is optional (depth-only passes have none).
    const CompiledShaderStage* vertexStage   = resolveStage(m_boundVertexShader);
    const CompiledShaderStage* fragmentStage = resolveStage(m_boundFragmentShader);
    if (!vertexStage)
      return;

    MetalVertexLayout layout = buildVertexLayout();

    // Pipeline cache keyed by the shader pair (the FF triangle's layout is
    // stable; a layout hash is added when variable-layout draws appear).
    uint64_t key = (uint64_t(DxvkShader::getCookie(m_boundVertexShader)) << 32)
                 |  uint64_t(DxvkShader::getCookie(m_boundFragmentShader));
    uint64_t pipeline = 0;
    auto cachedPipeline = m_pipelineStates.find(key);
    if (cachedPipeline != m_pipelineStates.end()) {
      pipeline = cachedPipeline->second;
    } else {
      pipeline = metalBuildRenderPipeline(m_backend->device(),
        vertexStage->function, fragmentStage ? fragmentStage->function : 0u,
        layout, /* MTLPixelFormatBGRA8Unorm */ 80u);
      m_pipelineStates.emplace(key, pipeline);
      if (!pipeline)
        logLine("render pipeline build failed");
    }
    if (!pipeline)
      return;

    // Bind the vertex stream (binding 0 at VertexBufferBase) and each stage's
    // push block at its reflected index.
    // Bind every stream the input layout references: the frontend's real
    // buffer where bound, else a shared zero buffer (the FF null stream for
    // unused attributes — Metal drops the draw if any referenced index is
    // unbound, and the frontend never binds that stream).
    // Reuse persistent scratch (capacity survives clear()) so a steady draw
    // stream allocates nothing per draw.
    std::vector<D9mtBufferBinding>& bindings = m_drawBindings;
    std::vector<uint64_t>&          residentBuffers = m_drawResidentBuffers;
    bindings.clear();
    residentBuffers.clear();

    for (const DxvkVertexInput& input : m_vertexBindings) {
      uint32_t streamBinding = input.binding().binding;
      auto bound = m_vertexBuffers.find(streamBinding);
      if (bound != m_vertexBuffers.end() && bound->second.handle) {
        bindings.push_back({ bound->second.handle, bound->second.offset,
                             VertexBufferBase + streamBinding, false });
      } else {
        if (!m_zeroVertexBuffer)
          m_zeroVertexBuffer = m_backend->createSharedBuffer(256u, &m_zeroVertexBufferCpu);
        bindings.push_back({ m_zeroVertexBuffer, 0u,
                             VertexBufferBase + streamBinding, false });
      }
    }

    if (vertexStage->translation.reflection.pushBufferIndex >= 0) {
      if (uint64_t push = uploadPushData(vertexStage->translation.reflection, 0u))
        bindings.push_back({ push, 0u,
          uint32_t(vertexStage->translation.reflection.pushBufferIndex), false });
    }
    if (fragmentStage && fragmentStage->translation.reflection.pushBufferIndex >= 0) {
      if (uint64_t push = uploadPushData(fragmentStage->translation.reflection, 1u))
        bindings.push_back({ push, 0u,
          uint32_t(fragmentStage->translation.reflection.pushBufferIndex), true });
    }

    // Argument buffers carrying the FF constant buffers (transform, clip planes,
    // spec data) the translated shaders read from descriptor sets 2/3. The
    // buffers they point to must be made resident (residentBuffers).
    bindArgumentBuffers(vertexStage->translation.reflection, false, 0u, bindings, residentBuffers);
    if (fragmentStage)
      bindArgumentBuffers(fragmentStage->translation.reflection, true, 1u, bindings, residentBuffers);

    D9mtPipelineDraw pipelineDraw;
    pipelineDraw.pipelineState   = pipeline;
    pipelineDraw.bindings        = bindings.data();
    pipelineDraw.bindingCount    = uint32_t(bindings.size());
    pipelineDraw.residentBuffers = residentBuffers.data();
    pipelineDraw.residentCount   = uint32_t(residentBuffers.size());
    pipelineDraw.vertexCount     = d.vertexCount;
    pipelineDraw.clear           = m_clearPending;
    pipelineDraw.clearColorArgb  = m_clearColorArgb;
    m_backend->beginFrame(m_clearPending, m_clearColorArgb);
    m_backend->appendDraw(pipelineDraw);

    m_clearPending = false;
  }

}
