#pragma once

// Typed wrappers so handle type mixups (e.g. binding a texture handle where
// a buffer belongs) fail at compile time instead of at the ABI boundary.

#include <cstdint>
#include <utility>

#include "../../bridge/winemetal.h"

namespace dxvk::backend {

template<typename Tag>
class WmtRef {

public:

  WmtRef() = default;

  static WmtRef adopt(obj_handle_t handle) {
    WmtRef ref;
    ref.m_handle = handle;
    return ref;
  }

  static WmtRef retain(obj_handle_t handle) {
    if (handle)
      NSObject_retain(handle);
    return adopt(handle);
  }

  WmtRef(const WmtRef& other) = delete;
  WmtRef& operator=(const WmtRef& other) = delete;

  WmtRef(WmtRef&& other) noexcept
  : m_handle(std::exchange(other.m_handle, 0u)) { }

  WmtRef& operator=(WmtRef&& other) noexcept {
    if (this != &other) {
      reset();
      m_handle = std::exchange(other.m_handle, 0u);
    }
    return *this;
  }

  ~WmtRef() {
    reset();
  }

  void reset() {
    if (m_handle) {
      NSObject_release(m_handle);
      m_handle = 0u;
    }
  }

  obj_handle_t detach() {
    return std::exchange(m_handle, 0u);
  }

  obj_handle_t handle() const { return m_handle; }

  explicit operator bool() const { return m_handle != 0u; }

private:

  obj_handle_t m_handle = 0u;

};

struct DeviceTag;        using WmtDevice        = WmtRef<DeviceTag>;
struct QueueTag;         using WmtQueue         = WmtRef<QueueTag>;
struct CommandBufferTag; using WmtCommandBuffer = WmtRef<CommandBufferTag>;
struct BufferTag;        using WmtBuffer        = WmtRef<BufferTag>;
struct TextureTag;       using WmtTexture       = WmtRef<TextureTag>;
struct SamplerTag;       using WmtSampler       = WmtRef<SamplerTag>;
struct LibraryTag;       using WmtLibrary       = WmtRef<LibraryTag>;
struct FunctionTag;      using WmtFunction      = WmtRef<FunctionTag>;
struct RenderPsoTag;     using WmtRenderPso     = WmtRef<RenderPsoTag>;
struct DepthStencilTag;  using WmtDepthStencil  = WmtRef<DepthStencilTag>;
struct SharedEventTag;   using WmtSharedEvent   = WmtRef<SharedEventTag>;

}
