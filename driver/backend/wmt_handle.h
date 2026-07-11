#pragma once
/*
 * Typed RAII wrappers over winemetal obj_handle_t.
 *
 * winemetal hands out opaque uint64 handles to Obj-C objects living on the
 * unix side; lifetime is manual NSObject_retain/release. These wrappers make
 * ownership explicit and let handle type mixups fail at compile time
 * (binding a texture handle where a buffer belongs was a real v1 bug class).
 */

#include <cstdint>
#include <utility>

/* The BRIDGE header is the canonical ABI: upstream winemetal plus our
 * additive entries (vertex-descriptor PSO, per-slot samplers, vertex
 * textures). Same _WINEMETAL_H guard — supersedes third_party's copy. */
#include "../../bridge/winemetal.h"

namespace dxvk::backend {

/* Owned reference: releases on destruction. Tag makes each alias distinct. */
template<typename Tag>
class WmtRef {

public:

  WmtRef() = default;

  /* Takes ownership of a +1 handle (new- and copy-family returns) */
  static WmtRef adopt(obj_handle_t handle) {
    WmtRef ref;
    ref.m_handle = handle;
    return ref;
  }

  /* Retains a borrowed handle */
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

} // namespace dxvk::backend
