#include "d3d9_fixed_function.h"

#include "d3d9_shader_analysis.h"   // D3D9InputSignature + dxbc_spv::sm3::Semantic

#include "../mtl/mtl_device.h"      // MtlDevice::createShaderFunction
#include "../mtl/mtl_context.h"     // MtlShaderFunction (entryName, cbvSlotMap)

// Build-time-generated: g_ffVsMetallib[] / g_ffFsMetallib[] byte arrays (the two
// FF MSL ubershaders compiled to .metallib by scripts/build-driver.sh).
#include "d3d9_ff_metallib.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

// ---------------------------------------------------------------------------
// d3d9_fixed_function.cpp — the D3D9 fixed-function pipeline for the Metal
// backend. Two hand-written MSL ubershaders (driver/d3d9/shaders/d3d9_ff_*.metal),
// compiled to a .metallib at build time and embedded via d3d9_ff_metallib.h, are
// wrapped into MtlShaderFunction handles here. The CBV/push layouts
// (D3D9FixedFunctionVS/PS in d3d9_state.h) are byte-identical to the DXVK
// originals so the D3D9 state tracker (UpdateFixedFunctionVS/PS) fills them
// unchanged; the MSL reads that exact layout.
// ---------------------------------------------------------------------------

namespace dxvk {

  void D3D9FFShaderModuleSet::Initialize(const Rc<MtlDevice>& device) {
    if (device == nullptr)
      return;

    using CbvIndex = D3D9ShaderResourceMapping::CbvIndex;

    // Vertex ubershader (auto-converted from DXVK's d3d9_fixed_function_vert.vert
    // via glslang + spirv-cross). Its CBV/push [[buffer(n)]] indices are pinned by
    // the build (scripts/build-driver.sh post-processing) to:
    //   SpecConsts  -> buffer(0)   ClipPlanes    -> buffer(2)
    //   ShaderData  -> buffer(1)   VertexBlend   -> buffer(5)
    //   RenderStates-> buffer(20)  (kPushConstantIndex, via the push path)
    // cbvSlotMap maps the frontend's CbvIndex to the AIR buffer slot above.
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

    // Fragment ubershader (auto-converted from d3d9_fixed_function_frag.frag).
    // Pinned [[buffer(n)]]: SpecConsts -> buffer(0), SharedData(D3D9SharedPS) ->
    // buffer(1), RenderStates -> buffer(20) (push). Textures land at [[texture(0..7)]]
    // (t2d), samplers at [[sampler(0..7)]] — matching flushResources' per-stage
    // texture/sampler binding.
    m_fs = device->createShaderFunction(
      MtlShaderStage::Fragment, g_ffFsMetallib, size_t(g_ffFsMetallibSize));
    if (m_fs != nullptr) {
      m_fs->entryName = "ffFsMain";
      m_fs->cbvSlotMap = { { -1, -1, -1, -1, -1, -1, -1, -1 } };
      m_fs->cbvSlotMap[CbvIndex::SpecData] = 0;
      m_fs->cbvSlotMap[CbvIndex::PSShared] = 1;
    }
  }

  // Fixed-function vertex input signature. Register order MUST match the FF
  // ubershader's [[attribute(n)]] locations (ported verbatim from DXVK 3.0's
  // CreateFixedFunctionIsgn): 0=Position0, 1=Normal0, 2=Position1, 3=Normal1,
  // 4..11=Texcoord0..7, 12=Color0, 13=Color1, 14=Fog, 15=PointSize,
  // 16=BlendWeight, 17=BlendIndices.
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
