#include "mtl_format.h"

// ---------------------------------------------------------------------------
// mtl_format.cpp — MtlFormatInfo lookup. PHASE-C: this is where the full
// per-MtlFormat metadata table (block size / element size / aspects / planes)
// lands, mirroring DXVK's imageFormatInfo(). For this phase the table is empty and
// the lookup returns nullptr (callers treat that as "metadata unknown").
// ---------------------------------------------------------------------------

namespace dxvk {

  const MtlFormatInfo* lookupMtlFormatInfo(MtlFormat /*format*/) {
    // PHASE-C: populate + key by MtlFormat.
    return nullptr;
  }

}
