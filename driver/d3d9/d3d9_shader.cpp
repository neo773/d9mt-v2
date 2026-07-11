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

  // PHASE-C10: link-frontier stub for the module-set cache entry point. The real
  // two-phase SM3->IR->AIR compile is the shader REWRITE track; here we just cache
  // a default (empty) D3D9CommonShader keyed by bytecode hash so device.cpp links
  // and the CreateShader path returns a well-formed (if non-functional) module.
  HRESULT D3D9ShaderModuleSet::GetShaderModule(
          D3D9DeviceEx*           pDevice,
    const D3D9ShaderModuleKey&    ShaderKey,
          D3D9ShaderAnalysis&&    ShaderAnalysis,
    const D3D9ShaderCreateInfo&   ModuleInfo,
    const void*                   pShaderBytecode,
          D3D9CommonShader*       pShader) {
    (void)pDevice;
    (void)ShaderAnalysis;
    (void)ModuleInfo;
    (void)pShaderBytecode;

    std::lock_guard lock(m_mutex);

    auto entry = m_modules.find(ShaderKey);
    if (entry != m_modules.end()) {
      if (pShader)
        *pShader = entry->second;
      return D3D_OK;
    }

    // Default-construct (D3D9CommonShader() = default) — no air backend yet.
    auto result = m_modules.emplace(ShaderKey, D3D9CommonShader());
    if (pShader)
      *pShader = result.first->second;

    return D3D_OK;
  }

}
