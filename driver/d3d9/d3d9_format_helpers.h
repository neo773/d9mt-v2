#pragma once

// PHASE-C7: D3D9FormatHelper was a Vulkan compute-pipeline YUV/packed-format
// converter (SPIR-V shaders + DxvkPipelineLayout) driving the D3D9 conversion
// formats (A4L4/UYVY/YUY2/R8G8_B8G8/G8R8_G8B8/NV12/YV12/…). Those formats all map
// to MtlFormat::Invalid in d3d9_format.cpp (the current Metal subset lacks them),
// so the ConvertFormat path is dead. Neutralized to a compiling stub; the real
// converter is a build-time MSL-compute REWRITE track (PHASE-E), matching the
// meta-blit approach in doc 01 §6.6.

#include "d3d9_include.h"
#include "d3d9_format.h"

#include "../mtl/mtl_context.h"

namespace dxvk {

  class D3D9FormatHelper {

  public:

    D3D9FormatHelper(const Rc<MtlDevice>& device)
    : m_device(device) { }

    ~D3D9FormatHelper() { }

    void Flush() { }

    // PHASE-E: real MSL-compute format conversion. The dst formats are currently
    // unsupported (Invalid), so this is never reached for a live resource.
    void ConvertFormat(
      const Rc<MtlCommandBuffer>&         ctx,
            D3D9_CONVERSION_FORMAT_INFO   conversionFormat,
      const Rc<MtlTexture>&               dstImage,
            MtlSubresourceLayers          dstSubresource,
      const MtlBufferSlice&               srcSlice) {
      Logger::warn("D3D9FormatHelper::ConvertFormat: conversion formats unsupported on Metal (PHASE-E)");
    }

  private:

    Rc<MtlDevice> m_device;

  };

}
