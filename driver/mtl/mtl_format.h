#pragma once

#include <cstdint>

#include "mtl_types.h"

// ---------------------------------------------------------------------------
// mtl_format.h — MtlFormatInfo, the Metal-first replacement for DxvkFormatInfo
// (doc 02 §2: "block size / aspect / component metadata -> our MtlFormatInfo table
// keyed by WMTPixelFormat"). Consumed by d3d9_format.cpp's format table and by the
// common_texture ADAPT track (Phase C). Kept minimal for this phase.
// ---------------------------------------------------------------------------

namespace dxvk {

  struct MtlFormatInfo {
    // Bytes per texel block (element size). For BCn this is the compressed block.
    uint32_t      elementSize = 0;
    // Compressed block dimensions in texels (1x1x1 for uncompressed).
    MtlSize       blockSize   = { 1, 1, 1 };
    // Which aspects the format exposes (color / depth / stencil).
    MtlAspectMask aspects     = MtlAspectColor;
    // Number of planes (1 for interleaved, 2/3 for planar YUV).
    uint32_t      planeCount  = 1;
  };

  // PHASE-C: full MtlFormatInfo table keyed by MtlFormat (mirrors DxvkFormatInfo
  // lookup). For now, callers that need metadata for a format the table does not
  // cover should treat elementSize == 0 as "unknown".
  const MtlFormatInfo* lookupMtlFormatInfo(MtlFormat format);

}
