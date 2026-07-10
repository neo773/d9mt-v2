#pragma once

#include <cstdint>

#include "mtl_types.h"

// ---------------------------------------------------------------------------
// mtl_format.h — thin re-export. The canonical MtlFormatInfo struct + the
// lookupFormatInfo() table now live in mtl_types.h (appended in Phase C). This
// header used to carry a SECOND, older MtlFormatInfo definition (blockSize/
// planeCount) + lookupMtlFormatInfo(); both were removed in Phase C-6 to kill the
// double-definition that surfaced once the device-graph headers pulled both this
// header and mtl_types.h into one TU. Kept as an include shim so existing
// `#include "../mtl/mtl_format.h"` sites (d3d9_format.h) keep resolving.
// ---------------------------------------------------------------------------

namespace dxvk {

  // MtlFormatInfo and lookupFormatInfo(MtlFormat) are declared in mtl_types.h.

}
