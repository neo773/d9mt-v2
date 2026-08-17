#include "d3d9_shader.h"
#include "d3d9_shader_compiler.h"
#include "d3d9_device.h"

namespace dxvk {

  static void ApplyCbvSlotMap(const Rc<MtlShaderFunction>& fn, const D3D9CbvSlotMap& map) {
    if (fn == nullptr)
      return;
    for (size_t i = 0u; i < fn->cbvSlotMap.size(); i++)
      fn->cbvSlotMap[i] = (i < map.slot.size()) ? map.slot[i] : int8_t(-1);

    fn->abTableIndex = map.abTableIndex;
    fn->abWords.clear();
    if (map.abUsed) {
      fn->abWords.reserve(map.abWords.size());
      for (const auto& w : map.abWords)
        fn->abWords.push_back({ MtlShaderFunction::AbWordKind(w.kind), w.reg });
    }
  }

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

    m_backend = createD3D9MetalShader(
      m_name, stage, ModuleInfo.shaderOptions, m_analysis, pShaderBytecode);

    if (m_backend == nullptr) {
      Logger::err(str::format("D3D9 shader: Phase-A build failed for ", m_name));
      return;
    }

    D3D9CbvSlotMap cbvMap;
    std::vector<uint8_t> metallib = m_backend->getMetallib(
      ID3D9MetalShaderBackend::PairedStageIo { }, 0u, &cbvMap);

    if (metallib.empty()) {
      Logger::err(str::format("D3D9 shader: Phase-B metallib empty for ", m_name));
      return;
    }

    m_shader = pDevice->GetDXVKDevice()->createShaderFunction(
      stage, metallib.data(), metallib.size());

    if (m_shader == nullptr)
      Logger::err(str::format("D3D9 shader: newLibrary failed for ", m_name));
    else
      ApplyCbvSlotMap(m_shader, cbvMap);
  }


  Rc<MtlShaderFunction> D3D9CommonShader::GetShaderForLinkage(
    const D3D9CommonShader* pPrevStage) const {
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

    D3D9CbvSlotMap cbvMap;
    std::vector<uint8_t> metallib = m_backend->getMetallib(paired, 0u, &cbvMap);

    Rc<MtlShaderFunction> fn = m_shader;

    if (!metallib.empty()) {
      auto compiled = m_device->GetDXVKDevice()->createShaderFunction(
        m_backend->stage(), metallib.data(), metallib.size());

      if (compiled != nullptr) {
        ApplyCbvSlotMap(compiled, cbvMap);
        fn = compiled;
      } else {
        Logger::err(str::format("D3D9 shader: linkage newLibrary failed for ", m_name));
      }
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
