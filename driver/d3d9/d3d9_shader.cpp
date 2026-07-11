#include "d3d9_shader.h"
#include "d3d9_shader_compiler.h"
#include "d3d9_device.h"

// ---------------------------------------------------------------------------
// d3d9_shader.cpp — PHASE-D: the real two-phase SM3->AIR shader module wired
// into the D3D9 frontend (doc 03 §1/§5). The heavy lifting (SM3->IR, the
// legacy-input pass, legalization, the AIR compile-thunk boundary) lives in
// d3d9_shader_compiler.{h,cpp}; this TU just drives it from D3D9CommonShader
// and caches modules in D3D9ShaderModuleSet.
// ---------------------------------------------------------------------------

namespace dxvk {

  D3D9CommonShader::D3D9CommonShader(
          D3D9DeviceEx*             pDevice,
    const D3D9ShaderModuleKey&      ShaderKey,
          D3D9ShaderAnalysis&&      Analysis,
    const D3D9ShaderCreateInfo&     ModuleInfo,
    const void*                     pShaderBytecode)
  : m_analysis(std::move(Analysis)) {
    const MtlShaderStage stage = ShaderKey.stage();

    m_name = str::format(stage == MtlShaderStage::Vertex ? "vs_" : "ps_", ShaderKey.hash());
    Logger::debug(str::format("Compiling shader ", m_name));

    // Phase A: build the air-backed backend (SM3 -> legalized IR, cached).
    Rc<ID3D9MetalShaderBackend> backend = createD3D9MetalShader(
      m_name, stage, ModuleInfo.shaderOptions, m_analysis, pShaderBytecode);

    if (backend == nullptr) {
      Logger::err(str::format("D3D9 shader: Phase-A build failed for ", m_name));
      return;
    }

    // Phase B: compile a metallib for the default (unpaired) linkage and wrap
    // it into a bindable Metal shader function. VS<->PS linkage-specialized
    // variants are produced through backend->getMetallib(cookie, spec) when the
    // PSO layer carries the paired IoMap (doc 03 §2b/§2d).
    std::vector<uint8_t> metallib = backend->getMetallib(0u, 0u);

    if (metallib.empty()) {
      Logger::err(str::format("D3D9 shader: Phase-B metallib empty for ", m_name));
      return;
    }

    m_shader = pDevice->GetDXVKDevice()->createShaderFunction(
      stage, metallib.data(), metallib.size());

    if (m_shader == nullptr)
      Logger::err(str::format("D3D9 shader: newLibrary failed for ", m_name));
  }


  HRESULT D3D9ShaderModuleSet::GetShaderModule(
          D3D9DeviceEx*           pDevice,
    const D3D9ShaderModuleKey&    ShaderKey,
          D3D9ShaderAnalysis&&    ShaderAnalysis,
    const D3D9ShaderCreateInfo&   ModuleInfo,
    const void*                   pShaderBytecode,
          D3D9CommonShader*       pShader) {
    std::lock_guard lock(m_mutex);

    // Cache keyed by (stage, SHA-1 of the SM3 bytecode) — the Phase-A key
    // (doc 03 §2d). Phase-B metallib specialization keys off the linkage/spec
    // cookies inside the backend.
    auto entry = m_modules.find(ShaderKey);
    if (entry != m_modules.end()) {
      if (pShader)
        *pShader = entry->second;
      return D3D_OK;
    }

    D3D9CommonShader module(pDevice, ShaderKey, std::move(ShaderAnalysis), ModuleInfo, pShaderBytecode);

    auto result = m_modules.emplace(ShaderKey, std::move(module));
    if (pShader)
      *pShader = result.first->second;

    return D3D_OK;
  }

}
