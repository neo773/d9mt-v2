#include "mtl_context.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

// ---------------------------------------------------------------------------
// mtl_context_stub.cpp — Phase A+B log-and-return implementation of the MtlContext
// seam. Every method is a no-op that returns a default so the driver links and the
// CS worker can replay recorded chunks without a real backend. PHASE-C replaces
// this whole TU with the winemetal-backed Metal 3 impl (and later the Metal 4
// bridge). The (!) contracts flagged in mtl_context.h / doc 01 §3 are the
// deadlock-class ones that must be wired first when this becomes real.
// ---------------------------------------------------------------------------

namespace dxvk {

  // One-shot log so a stubbed method announces itself once, then stays quiet.
#define MTL_STUB(name)                                        \
  do {                                                        \
    static bool s_logged = false;                             \
    if (!s_logged) {                                          \
      s_logged = true;                                        \
      Logger::debug(str::format("MtlContext::", name,         \
        " — stub (PHASE-C: real Metal backend)"));            \
    }                                                         \
  } while (0)

  MtlContext::MtlContext(const Rc<MtlDevice>& device)
  : m_device(device) {
    Logger::debug("MtlContext: constructed (stub backend)");
  }

  MtlContext::~MtlContext() { }

  // --- lifecycle / submission ----------------------------------------------
  void MtlContext::beginRecording()                            { MTL_STUB("beginRecording"); }
  void MtlContext::endFrame()                                  { MTL_STUB("endFrame"); }
  void MtlContext::flushCommandList(const char*, MtlSubmitStatus* status) {
    MTL_STUB("flushCommandList");
    if (status) { status->submissionId = 0; status->completed = true; }
  }
  Rc<MtlCommandBuffer> MtlContext::beginExternalRendering()    { MTL_STUB("beginExternalRendering"); return nullptr; }
  void MtlContext::synchronizeWsi(MtlPresenterSync)            { MTL_STUB("synchronizeWsi"); }
  void MtlContext::beginLatencyTracking(const Rc<MtlLatencyTracker>&, uint64_t) { MTL_STUB("beginLatencyTracking"); }
  void MtlContext::endLatencyTracking(const Rc<MtlLatencyTracker>&) { MTL_STUB("endLatencyTracking"); }

  // --- shaders & pipeline state --------------------------------------------
  void MtlContext::bindShader(MtlShaderStage, Rc<MtlShaderFunction>&&)         { MTL_STUB("bindShader"); }
  void MtlContext::setFunctionConstants(MtlPipeline, uint32_t, uint32_t, const void*) { MTL_STUB("setFunctionConstants"); }
  void MtlContext::setVertexLayout(uint32_t, const MtlVertexAttr*, uint32_t, const MtlVertexLayout*) { MTL_STUB("setVertexLayout"); }
  void MtlContext::setPrimitiveTopology(MtlPrimitiveTopology)                  { MTL_STUB("setPrimitiveTopology"); }
  void MtlContext::setViewports(uint32_t, const MtlViewport*)                  { MTL_STUB("setViewports"); }
  void MtlContext::setRasterizerState(const MtlRasterizerState&)               { MTL_STUB("setRasterizerState"); }
  void MtlContext::setMultisampleState(const MtlMultisampleState&)             { MTL_STUB("setMultisampleState"); }
  void MtlContext::setDepthStencilState(const MtlDepthStencilState&)           { MTL_STUB("setDepthStencilState"); }
  void MtlContext::setBlendMode(uint32_t, const MtlBlendMode&)                 { MTL_STUB("setBlendMode"); }
  void MtlContext::setBlendColor(MtlColor4)                                    { MTL_STUB("setBlendColor"); }
  void MtlContext::setStencilReference(uint32_t)                               { MTL_STUB("setStencilReference"); }
  void MtlContext::setDepthBias(MtlDepthBias)                                  { MTL_STUB("setDepthBias"); }
  void MtlContext::setDepthBounds(MtlDepthBounds)                              { MTL_STUB("setDepthBounds (accepted + no-op, doc §6.3)"); }

  // --- binding --------------------------------------------------------------
  void MtlContext::bindVertexBuffer(uint32_t, MtlBufferSlice&&, uint32_t)      { MTL_STUB("bindVertexBuffer"); }
  void MtlContext::bindVertexBufferRange(uint32_t, MtlDeviceSize, MtlDeviceSize, uint32_t) { MTL_STUB("bindVertexBufferRange"); }
  void MtlContext::bindIndexBuffer(MtlBufferSlice&&, MtlIndexType)             { MTL_STUB("bindIndexBuffer"); }
  void MtlContext::bindUniformBuffer(MtlStageMask, uint32_t, MtlBufferSlice&&) { MTL_STUB("bindUniformBuffer"); }
  void MtlContext::bindUniformBufferRange(MtlStageMask, uint32_t, MtlDeviceSize, MtlDeviceSize) { MTL_STUB("bindUniformBufferRange"); }
  void MtlContext::bindTexture(MtlStageMask, uint32_t, Rc<MtlTextureView>&&)   { MTL_STUB("bindTexture"); }
  void MtlContext::bindBufferView(MtlStageMask, uint32_t, Rc<MtlTexelBufferView>&&) { MTL_STUB("bindBufferView"); }
  void MtlContext::bindSampler(MtlStageMask, uint32_t, Rc<MtlSamplerState>&&)  { MTL_STUB("bindSampler"); }
  void MtlContext::pushData(MtlStageMask, uint32_t, uint32_t, const void*)     { MTL_STUB("pushData"); }

  // --- render targets -------------------------------------------------------
  void MtlContext::bindRenderTargets(MtlRenderTargets&&, MtlFeedbackLoopMask)  { MTL_STUB("bindRenderTargets"); }
  void MtlContext::emitGraphicsBarrier(MtlStageMask, MtlBarrierScope, MtlStageMask, MtlBarrierScope) { MTL_STUB("emitGraphicsBarrier"); }

  // --- draws ----------------------------------------------------------------
  void MtlContext::draw(uint32_t, const MtlDrawArgs*)                          { MTL_STUB("draw"); }
  void MtlContext::drawIndexed(uint32_t, const MtlDrawIndexedArgs*)            { MTL_STUB("drawIndexed"); }

  // --- clears ---------------------------------------------------------------
  void MtlContext::clearRenderTarget(const Rc<MtlTextureView>&, MtlAspectMask, MtlClearValue, MtlAspectMask) { MTL_STUB("clearRenderTarget"); }
  void MtlContext::clearImageView(const Rc<MtlTextureView>&, MtlOrigin, MtlSize, MtlAspectMask, MtlClearValue) { MTL_STUB("clearImageView"); }

  // --- copies / blits / resolves / mips ------------------------------------
  void MtlContext::copyTexture(const Rc<MtlTexture>&, MtlSubresourceLayers, MtlOrigin,
                               const Rc<MtlTexture>&, MtlSubresourceLayers, MtlOrigin, MtlSize) { MTL_STUB("copyTexture"); }
  void MtlContext::copyBuffer(const Rc<MtlBuffer>&, MtlDeviceSize, const Rc<MtlBuffer>&, MtlDeviceSize, MtlDeviceSize) { MTL_STUB("copyBuffer"); }
  void MtlContext::copyTextureToBuffer(const Rc<MtlBuffer>&, MtlDeviceSize, MtlDeviceSize, MtlDeviceSize, MtlFormat,
                                       const Rc<MtlTexture>&, MtlSubresourceLayers, MtlOrigin, MtlSize) { MTL_STUB("copyTextureToBuffer"); }
  void MtlContext::copyBufferToTexture(const Rc<MtlTexture>&, MtlSubresourceLayers, MtlOrigin, MtlSize,
                                       const Rc<MtlBuffer>&, MtlDeviceSize, MtlDeviceSize, MtlDeviceSize, MtlFormat) { MTL_STUB("copyBufferToTexture"); }
  void MtlContext::resolveTexture(const Rc<MtlTexture>&, const Rc<MtlTexture>&, MtlResolveRegion, MtlFormat, MtlResolveMode, MtlResolveMode) { MTL_STUB("resolveTexture"); }
  void MtlContext::blitTextureView(const Rc<MtlTextureView>&, const MtlOffset3D[2], const Rc<MtlTextureView>&, const MtlOffset3D[2], MtlSamplerFilter) { MTL_STUB("blitTextureView"); }
  bool MtlContext::presentUpscale(const Rc<MtlTexture>&, const Rc<MtlTexture>&) { return false; }
  void MtlContext::generateMipmaps(const Rc<MtlTextureView>&, MtlSamplerFilter) { MTL_STUB("generateMipmaps"); }
  void MtlContext::transformImage(const Rc<MtlTexture>&, MtlSubresourceRange, MtlBarrierScope) { MTL_STUB("transformImage"); }

  // --- init / upload / discard ---------------------------------------------
  void MtlContext::initBuffer(const Rc<MtlBuffer>&)                            { MTL_STUB("initBuffer"); }
  void MtlContext::initImage(const Rc<MtlTexture>&)                            { MTL_STUB("initImage"); }
  void MtlContext::invalidateBuffer(const Rc<MtlBuffer>&, Rc<MtlAllocation>&&) { MTL_STUB("invalidateBuffer"); }
  void MtlContext::uploadBuffer(const Rc<MtlBuffer>&, MtlDeviceSize, const Rc<MtlBuffer>&, MtlDeviceSize, MtlDeviceSize) { MTL_STUB("uploadBuffer"); }
  // SILENT no-op (not MTL_STUB): the local path legitimately needs nothing —
  // UMA direct-map writes are already GPU-visible; only remoting overrides.
  void MtlContext::noteBufferWrite(const Rc<MtlAllocation>&, MtlDeviceSize, MtlDeviceSize) { }
  bool MtlContext::ensureTextureUsage(const Rc<MtlTexture>&, MtlTextureUsage)  { MTL_STUB("ensureTextureUsage"); return false; }

  // --- queries --------------------------------------------------------------
  void MtlContext::beginQuery(const Rc<MtlQuery>&)                             { MTL_STUB("beginQuery"); }
  void MtlContext::endQuery(const Rc<MtlQuery>&)                               { MTL_STUB("endQuery"); }
  void MtlContext::writeTimestamp(const Rc<MtlQuery>&)                         { MTL_STUB("writeTimestamp"); }

  // --- signals / events -----------------------------------------------------
  void MtlContext::signal(const Rc<MtlSignal>&, uint64_t)                      { MTL_STUB("signal"); }
  void MtlContext::signalGpuEvent(const Rc<MtlEvent>&)                         { MTL_STUB("signalGpuEvent"); }

  // --- lifetime / residency -------------------------------------------------
  void MtlContext::track(Rc<MtlResource>&&)                                    { MTL_STUB("track"); }

  // --- debug labels ---------------------------------------------------------
  void MtlContext::beginDebugLabel(const char*)                               { MTL_STUB("beginDebugLabel"); }
  void MtlContext::endDebugLabel()                                            { MTL_STUB("endDebugLabel"); }
  void MtlContext::insertDebugLabel(const char*)                              { MTL_STUB("insertDebugLabel"); }

#undef MTL_STUB

}
