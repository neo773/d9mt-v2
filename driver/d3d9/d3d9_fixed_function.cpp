#include "d3d9_fixed_function.h"

// ---------------------------------------------------------------------------
// d3d9_fixed_function.cpp — PHASE-D placeholder. The real FF path authors two MSL
// uber-shaders (VS/FS) compiled at BUILD TIME into the driver's own metallib, with
// Metal function constants replacing the SPIR-V spec constants (doc 03 §3). The CBV
// and push-data layouts (D3D9SpecData / FF CBVs / D3D9Ffvs+FfpsPushData) are kept
// byte-identical to the DXVK originals so the D3D9 state tracker that fills them is
// reused unchanged. No v2/air include here.
// ---------------------------------------------------------------------------

namespace dxvk {

  // PHASE-D: buildVs()/buildFs() (build-time MSL uber-shader load) land in the FF
  // REWRITE track; the module set default-constructs with null shader handles.

}
