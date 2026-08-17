#include "d3d9_fixed_function.h"

#include "d3d9_shader_analysis.h"

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"

#include "d3d9_ff_metallib.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk {

  void D3D9FFShaderModuleSet::Initialize(const Rc<MtlDevice>& device) {
    if (device == nullptr)
      return;

    using CbvIndex = D3D9ShaderResourceMapping::CbvIndex;

    m_vs = device->createShaderFunction(
      MtlShaderStage::Vertex, g_ffVsMetallib, size_t(g_ffVsMetallibSize));
    if (m_vs != nullptr) {
      m_vs->entryName = "ffVsMain";
      m_vs->cbvSlotMap = { { -1, -1, -1, -1, -1, -1, -1, -1 } };
      m_vs->cbvSlotMap[CbvIndex::SpecData]          = 0;
      m_vs->cbvSlotMap[CbvIndex::VSFixedFunction]   = 1;
      m_vs->cbvSlotMap[CbvIndex::VSClipPlanes]      = 2;
      m_vs->cbvSlotMap[CbvIndex::VSVertexBlendData] = 5;
    }

    m_fs = device->createShaderFunction(
      MtlShaderStage::Fragment, g_ffFsMetallib, size_t(g_ffFsMetallibSize));
    if (m_fs != nullptr) {
      m_fs->entryName = "ffFsMain";
      m_fs->cbvSlotMap = { { -1, -1, -1, -1, -1, -1, -1, -1 } };
      m_fs->cbvSlotMap[CbvIndex::SpecData] = 0;
      m_fs->cbvSlotMap[CbvIndex::PSShared] = 1;
    }
  }

  // Register order here must match the FF ubershader's [[attribute(n)]] locations.
  const D3D9InputSignature& GetFixedFunctionIsgn() {
    static const D3D9InputSignature s_ffIsgn = [] {
      using dxbc_spv::sm3::Semantic;
      using dxbc_spv::sm3::SemanticUsage;
      D3D9InputSignature isgn;
      isgn.add(Semantic { SemanticUsage::ePosition, 0u });
      isgn.add(Semantic { SemanticUsage::eNormal,   0u });
      isgn.add(Semantic { SemanticUsage::ePosition, 1u });
      isgn.add(Semantic { SemanticUsage::eNormal,   1u });
      for (uint32_t i = 0u; i < 8u; i++)
        isgn.add(Semantic { SemanticUsage::eTexCoord, i });
      isgn.add(Semantic { SemanticUsage::eColor,        0u });
      isgn.add(Semantic { SemanticUsage::eColor,        1u });
      isgn.add(Semantic { SemanticUsage::eFog,          0u });
      isgn.add(Semantic { SemanticUsage::ePointSize,    0u });
      isgn.add(Semantic { SemanticUsage::eBlendWeight,  0u });
      isgn.add(Semantic { SemanticUsage::eBlendIndices, 0u });
      return isgn;
    }();
    return s_ffIsgn;
  }

}
