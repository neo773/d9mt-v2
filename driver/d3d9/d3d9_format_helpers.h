#pragma once

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
