#include "d3d9_swvp_emu.h"

#include "d3d9_device.h"
#include "d3d9_vertex_declaration.h"

// PHASE-C6 STAGE-2: ported off Vulkan onto the Mtl seam. The hash/eq operators
// (used by the m_modules cache) are backend-neutral and copied verbatim. The
// D3D9SWVPShaderGenerator (dxbc_spv IR builder → geometry ubershader) is the
// SM3→Air shader REWRITE track (Phase-D), NOT part of the device-graph body port;
// GetShaderModule is stubbed to return a null MtlShaderFunction so the link stays
// honest (doc: "STAGE 2 may leave GetShaderModule stubbed").

namespace dxvk {

  // Doesn't compare everything, only what we use in SWVP.

  size_t D3D9VertexDeclHash::operator () (const D3D9CompactVertexElements& key) const {
    DxvkHashState hash;

    std::hash<BYTE> bytehash;
    std::hash<WORD> wordhash;

    for (uint32_t i = 0; i < key.size(); i++) {
      const auto& element = key[i];
      hash.add(wordhash(element.Stream));
      hash.add(wordhash(element.Offset));
      hash.add(bytehash(element.Type));
      hash.add(bytehash(element.Method));
      hash.add(bytehash(element.Usage));
      hash.add(bytehash(element.UsageIndex));
    }

    return hash;
  }

  bool D3D9VertexDeclEq::operator () (const D3D9CompactVertexElements& a, const D3D9CompactVertexElements& b) const {
    if (a.size() != b.size())
      return false;

    bool equal = true;

    for (uint32_t i = 0; i < a.size(); i++)
      equal &= std::memcmp(&a[i], &b[i], sizeof(a[0])) == 0;

    return equal;
  }


  Rc<MtlShaderFunction> D3D9SWVPEmulator::GetShaderModule(D3D9DeviceEx* pDevice, D3D9CompactVertexElements&& elements) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);
    auto entry = m_modules.find(elements);

    if (entry != m_modules.end())
      return entry->second;

    // PHASE-E: verify — real SWVP ProcessVertices geometry ubershader is the
    // SM3→Air compile path (shader REWRITE track). Cache a null module so repeated
    // lookups are stable and the link resolves.
    Rc<MtlShaderFunction> shader = nullptr;

    m_modules.insert({ std::move(elements), shader });
    return shader;
  }

}
