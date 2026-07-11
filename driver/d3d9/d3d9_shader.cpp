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
  : m_analysis(std::move(Analysis))
  , m_device(pDevice)
  , m_linkageCache(std::make_shared<D3D9ShaderLinkageCache>()) {
    const MtlShaderStage stage = ShaderKey.stage();

    m_name = str::format(stage == MtlShaderStage::Vertex ? "vs_" : "ps_", ShaderKey.hash());
    Logger::debug(str::format("Compiling shader ", m_name));

    // Phase A: build the air-backed backend (SM3 -> legalized IR, cached). Kept
    // alive as m_backend so Phase-B linkage variants can be produced on demand at
    // draw/PSO time (GetShaderForLinkage).
    m_backend = createD3D9MetalShader(
      m_name, stage, ModuleInfo.shaderOptions, m_analysis, pShaderBytecode);

    if (m_backend == nullptr) {
      Logger::err(str::format("D3D9 shader: Phase-A build failed for ", m_name));
      return;
    }

    // Phase B: compile a metallib for the default (unpaired) linkage and wrap it
    // into a bindable Metal shader function. This is the VS/FF shader and the
    // fallback PS; VS<->PS interpolant-reconciled PS variants are produced lazily
    // through GetShaderForLinkage() once the paired VS is known (doc 03 §2b/§2d).
    std::vector<uint8_t> metallib = m_backend->getMetallib(
      ID3D9MetalShaderBackend::PairedStageIo { }, 0u);

    if (metallib.empty()) {
      Logger::err(str::format("D3D9 shader: Phase-B metallib empty for ", m_name));
      return;
    }

    m_shader = pDevice->GetDXVKDevice()->createShaderFunction(
      stage, metallib.data(), metallib.size());

    if (m_shader == nullptr)
      Logger::err(str::format("D3D9 shader: newLibrary failed for ", m_name));
  }


  Rc<MtlShaderFunction> D3D9CommonShader::GetShaderForLinkage(
    const D3D9CommonShader* pPrevStage) const {
    // Only the fragment stage is rewritten for VS<->PS interpolant reconciliation,
    // and only when a paired previous stage with a live backend is available.
    // Everything else (VS, FF, missing backend) uses the unpaired default.
    if (m_backend == nullptr
     || m_backend->stage() != MtlShaderStage::Fragment
     || pPrevStage == nullptr
     || pPrevStage->m_backend == nullptr)
      return m_shader;

    const uint64_t prevIoHash = pPrevStage->m_backend->outputIoHash();

    std::lock_guard lock(m_linkageCache->mutex);

    auto it = m_linkageCache->functions.find(prevIoHash);
    if (it != m_linkageCache->functions.end())
      return it->second;

    ID3D9MetalShaderBackend::PairedStageIo paired = { };
    paired.prevStageIr     = &pPrevStage->m_backend->serializedIr();
    paired.prevStageIoHash = prevIoHash;
    paired.matchSemantics  = true;

    // The backend memoizes the compiled metallib on (prevIoHash, spec); here we
    // additionally memoize the metallib->MtlShaderFunction wrap per prev layout.
    std::vector<uint8_t> metallib = m_backend->getMetallib(paired, 0u);

    Rc<MtlShaderFunction> fn = m_shader;  // fall back to the unpaired shader

    if (!metallib.empty()) {
      auto compiled = m_device->GetDXVKDevice()->createShaderFunction(
        m_backend->stage(), metallib.data(), metallib.size());

      if (compiled != nullptr)
        fn = compiled;
      else
        Logger::err(str::format("D3D9 shader: linkage newLibrary failed for ", m_name));
    }

    m_linkageCache->functions.emplace(prevIoHash, fn);
    return fn;
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
