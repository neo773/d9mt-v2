#pragma once

#include <Metal/Metal.hpp>

namespace d9mt::metal {

  class MetalDevice {
  public:
    MetalDevice();
    ~MetalDevice();

    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    bool isValid() const { return m_device != nullptr && m_queue != nullptr; }

    MTL::Device*       device() const { return m_device; }
    MTL::CommandQueue* queue()  const { return m_queue; }

  private:
    MTL::Device*       m_device = nullptr;
    MTL::CommandQueue* m_queue  = nullptr;
  };

}
