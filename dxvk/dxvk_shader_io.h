#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dxvk_hash.h"
#include "dxvk_include.h"

// Minimal shader-I/O shim. The real header (../dxvk-ref/dxvk_shader_io.h)
// describes SPIR-V I/O variables and checks cross-stage compatibility for
// Vulkan pipeline linking. The Metal shim links no pipelines, so DxvkShaderIo
// only needs to be a default-constructible, copyable value type: it appears
// solely as `={}` members of DxvkShaderMetadata / DxvkShaderLinkage, which the
// shim never inspects. We keep getVarCount/add/computeMask so any incidental
// caller compiles. See SHIM_SPEC.md (shader module = opaque).

namespace dxvk {

  struct DxvkShaderIoVar {
    uint8_t     location       = 0u;
    uint8_t     componentIndex = 0u;
    uint8_t     componentCount = 0u;
    bool        isPatchConstant = false;
    uint32_t    semanticIndex  = 0u;
    std::string semanticName   = { };
  };

  class DxvkShaderIo {

  public:

    DxvkShaderIo() = default;

    uint32_t getVarCount() const { return uint32_t(m_vars.size()); }
    DxvkShaderIoVar getVar(uint32_t i) const { return m_vars[i]; }

    void add(DxvkShaderIoVar var) { m_vars.push_back(std::move(var)); }

    // No real linking in the shim; report an empty location mask.
    uint32_t computeMask() const { return 0u; }

    static DxvkShaderIo forVertexBindings(uint32_t /*bindingMask*/) {
      return DxvkShaderIo();
    }

  private:

    std::vector<DxvkShaderIoVar> m_vars;

  };

}
