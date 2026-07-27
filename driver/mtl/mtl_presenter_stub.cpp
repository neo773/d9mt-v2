#include "mtl_presenter.h"
#include "mtl_context.h"   // for MtlDevice

#include "../util/log/log.h"
#include "../util/util_string.h"

// ---------------------------------------------------------------------------
// mtl_presenter_stub.cpp — PHASE-D log-and-return MtlPresenter. No CAMetalLayer,
// no winemetal, no drawable pool. Lets the swapchain REWRITE compile and link
// against the present seam. The real impl (CAMetalLayer + drawable present +
// frame-latency shared event) lands in the swapchain REWRITE track.
// ---------------------------------------------------------------------------

namespace dxvk {

  class MtlPresenterStub : public MtlPresenter {

  public:

    MtlPresenterStub(const Rc<MtlDevice>& device, const MtlPresenterDesc& desc)
    : m_device(device), m_desc(desc) {
      Logger::debug("MtlPresenter: constructed (PHASE-D stub, no CAMetalLayer)");
    }

    void recreate(const MtlPresenterDesc& desc) override {
      m_desc = desc;
      logOnce("recreate");
    }

    MtlAcquiredDrawable acquireNextImage(uint64_t frameId) override {
      logOnce("acquireNextImage");
      MtlAcquiredDrawable image = { };
      image.sync.frameId = frameId;
      image.valid = false;   // no real drawable in the stub
      return image;
    }

    void present(const MtlAcquiredDrawable&) override { logOnce("present"); }

    void signalFrameLatency(uint64_t) override {
      // PHASE-D: contract 3e — real impl posts this from the present completion
      // handler; stubbing it live deadlocks the frame-limiter.
      logOnce("signalFrameLatency");
    }

    void setSyncInterval(uint32_t) override { }

    bool isVisible() override { return true; }

    void syncLayerGeometry(uint32_t, uint32_t) override { }

    MtlExtent2D getImageExtent() const override { return m_desc.imageExtent; }

  private:

    Rc<MtlDevice>    m_device;
    MtlPresenterDesc m_desc;

    void logOnce(const char* what) {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        Logger::debug(str::format("MtlPresenter::", what, " — stub (PHASE-D)"));
      }
    }
  };

  Rc<MtlPresenter> createMtlPresenter(const Rc<MtlDevice>& device, const MtlPresenterDesc& desc) {
    if (desc.hwnd) {
      extern Rc<MtlPresenter> backendCreatePresenter(
        const Rc<MtlDevice>&, intptr_t, const MtlPresenterDesc&);
      Rc<MtlPresenter> real = backendCreatePresenter(device, desc.hwnd, desc);
      if (real != nullptr)
        return real;
      Logger::err("createMtlPresenter: backend presenter failed, using stub");
    }
    return new MtlPresenterStub(device, desc);
  }

}
