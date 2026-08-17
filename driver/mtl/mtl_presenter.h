#pragma once

#include <cstdint>

#include "mtl_types.h"
#include "mtl_resource.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class MtlDevice;

  struct MtlAcquiredDrawable {
    Rc<MtlTexture>  texture = nullptr;
    Rc<MtlTextureView> view = nullptr;
    MtlObjectHandle drawable = MtlNullHandle;
    MtlPresenterSync sync   = { };
    uint32_t        imageIndex = 0u;
    bool            valid   = false;
  };

  struct MtlPresenterDesc {
    MtlExtent2D imageExtent  = { };
    MtlFormat   imageFormat  = MtlFormat::BGRA8Unorm;
    uint32_t    imageCount   = 3u;
    bool        vsync        = true;
    intptr_t    hwnd         = 0;
  };

  class MtlPresenter : public RcObject {

  public:

    virtual ~MtlPresenter() { }

    virtual void recreate(const MtlPresenterDesc& desc) = 0;

    virtual void setSyncInterval(uint32_t syncInterval) = 0;

    // Must be called every Present: macdrv resizes the NSView asynchronously,
    // so a size latched at create/Reset goes stale for an unknown number of
    // frames, and sizing from the backbuffer instead of the live contentsScale
    // renders into the top-left quarter on Retina (scale 2).
    virtual void syncLayerGeometry(uint32_t clientWidth, uint32_t clientHeight) = 0;

    virtual MtlAcquiredDrawable acquireNextImage(uint64_t frameId) = 0;

    virtual void present(const MtlAcquiredDrawable& image) = 0;

    virtual void signalFrameLatency(uint64_t frameId) = 0;

    virtual MtlExtent2D getImageExtent() const = 0;

    // False when macOS isn't compositing the layer (occluded, other Space,
    // miniaturized): presented drawables never retire while uncomposited, so
    // acquiring anyway drains the pool and nextDrawable starts blocking ~1s
    // per frame, a backlog that persists after the window returns. Win32
    // focus doesn't substitute for this — it doesn't track Spaces.
    virtual bool isVisible() = 0;
  };

  Rc<MtlPresenter> createMtlPresenter(const Rc<MtlDevice>& device, const MtlPresenterDesc& desc);

}
