#include "d3d9_shader.h"

// ---------------------------------------------------------------------------
// d3d9_shader.cpp — PHASE-D placeholder for the shader REWRITE module (doc 02,
// doc 03 §1/§5). The real two-phase SM3->IR->AirBuilder->metallib pipeline is the
// shader REWRITE track and deliberately does NOT include v2/air or dxbc_spv here.
// This TU exists so the interface header has a compiled home and the build set is
// complete; there is no concrete ID3D9MetalShaderBackend impl yet.
// ---------------------------------------------------------------------------

namespace dxvk {

  // PHASE-D: intentionally empty. createD3D9MetalShader() + the air-backed
  // ID3D9MetalShaderBackend implementation land in the shader REWRITE track.

}
