#pragma once

#include <cstdint>
#include <array>
#include <vector>

#include "mtl_types.h"
#include "mtl_resource.h"
#include "mtl_device.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/sync/sync_signal.h"

namespace dxvk {

  class MtlShaderFunction : public RcObject {
  public:
    MtlShaderStage  stage = MtlShaderStage::Vertex;
    MtlObjectHandle library = MtlNullHandle;
    // Metal reserves "main"; hand-authored fixed-function shaders use
    // "ffVsMain"/"ffFsMain" instead.
    const char*     entryName = "main";

    std::array<int8_t, 8> cbvSlotMap = { { -1, -1, -1, -1, -1, -1, -1, -1 } };

    enum class AbWordKind : uint8_t { Cbv = 0, Texture = 1, Sampler = 2 };
    struct AbWord {
      AbWordKind kind;
      uint8_t    reg;
    };
    uint8_t             abTableIndex = 21u;
    std::vector<AbWord> abWords;
  };

  class MtlCommandBuffer : public RcObject { };

  enum class MtlQueryType : uint32_t { Occlusion, Timestamp };
  enum class MtlGpuQueryStatus : uint32_t { Invalid, Pending, Available, Failed };
  enum class MtlGpuEventStatus : uint32_t { Invalid, Pending, Signaled };

  union MtlQueryData {
    struct { uint64_t samplesPassed; } occlusion;
    struct { uint64_t time; }          timestamp;
  };

  class MtlQuery : public RcObject {
  public:
    MtlQueryType type = MtlQueryType::Occlusion;
    virtual MtlGpuQueryStatus getData(MtlQueryData& out);
  };

  class MtlEvent : public RcObject {
  public:
    virtual MtlGpuEventStatus test();
  };

  using MtlSignal = sync::Signal;
  class MtlLatencyTracker: public RcObject {
  public:
    bool needsAutoMarkers() const { return false; }
  };

  struct MtlRenderTargets {
    static constexpr uint32_t MaxColorTargets = 4;
    Rc<MtlTextureView> color[MaxColorTargets] = { };
    Rc<MtlTextureView> depthStencil           = { };
  };

  class MtlContext : public RcObject {

  public:

    explicit MtlContext(const Rc<MtlDevice>& device);
    virtual ~MtlContext();

    virtual void beginRecording();
    virtual void endFrame();
    virtual void flushCommandList(const char* reason, MtlSubmitStatus* status);
    virtual Rc<MtlCommandBuffer> beginExternalRendering();
    virtual void synchronizeWsi(MtlPresenterSync sync);
    virtual void beginLatencyTracking(const Rc<MtlLatencyTracker>&, uint64_t frameId);
    virtual void endLatencyTracking(const Rc<MtlLatencyTracker>&);

    virtual void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn);
    virtual void setFunctionConstants(MtlPipeline pipeline, uint32_t index, uint32_t count, const void* data);
    virtual void setVertexLayout(uint32_t nAttr, const struct MtlVertexAttr* attrs,
                         uint32_t nLayout, const struct MtlVertexLayout* layouts);
    virtual void setPrimitiveTopology(MtlPrimitiveTopology topology);
    virtual void setViewports(uint32_t n, const MtlViewport* viewports);
    virtual void setRasterizerState(const MtlRasterizerState& rs);
    virtual void setMultisampleState(const MtlMultisampleState& ms);
    virtual void setDepthStencilState(const MtlDepthStencilState& ds);
    virtual void setBlendMode(uint32_t rt, const MtlBlendMode& blend);
    virtual void setBlendColor(MtlColor4 color);
    virtual void setStencilReference(uint32_t reference);
    virtual void setDepthBias(MtlDepthBias bias);
    virtual void setDepthBounds(MtlDepthBounds bounds);

    virtual void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride);
    virtual void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride);
    virtual void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type);
    virtual void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice);
    virtual void bindUniformBufferRange(MtlStageMask stages, uint32_t slot, MtlDeviceSize offset, MtlDeviceSize length);
    virtual void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view);
    virtual void bindBufferView(MtlStageMask stages, uint32_t slot, Rc<MtlTexelBufferView>&& view);
    virtual void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler);
    virtual void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data);

    virtual void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop);
    virtual void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope);

    virtual void draw(uint32_t count, const MtlDrawArgs* args);
    virtual void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args);

    virtual void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard);
    virtual void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value);

    virtual void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    virtual void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes);
    virtual void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent);
    virtual void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format);
    virtual void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil);
    virtual void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter);
    virtual bool presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src);
    virtual void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter);
    virtual void transformImage(const Rc<MtlTexture>& image, MtlSubresourceRange range, MtlBarrierScope scope);

    virtual void initBuffer(const Rc<MtlBuffer>& buffer);
    virtual void initImage(const Rc<MtlTexture>& image);
    // Buffer binds resolve against the storage snapshot captured at record
    // time; a rename only reaches already-bound slices through this call's
    // CS-time refresh, so ordering here is what keeps renames from
    // retargeting draws already recorded against the old storage.
    virtual void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice);
    virtual void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size);
    virtual bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage);

    virtual void beginQuery(const Rc<MtlQuery>& query);
    virtual void endQuery(const Rc<MtlQuery>& query);
    virtual void writeTimestamp(const Rc<MtlQuery>& query);

    virtual void signal(const Rc<MtlSignal>& signal, uint64_t value);
    virtual void signalGpuEvent(const Rc<MtlEvent>& event);

    virtual void track(Rc<MtlResource>&& resource);

    virtual void beginDebugLabel(const char* label);
    virtual void endDebugLabel();
    virtual void insertDebugLabel(const char* label);

  private:

    Rc<MtlDevice> m_device;

  };

  struct MtlVertexAttr {
    uint32_t  location   = 0;
    uint32_t  binding    = 0;
    MtlFormat format     = MtlFormat::Invalid;
    uint32_t  offset     = 0;
  };

  struct MtlVertexLayout {
    uint32_t binding    = 0;
    uint32_t stride     = 0;
    bool     perInstance = false;
    uint32_t divisor    = 0;
  };

}
