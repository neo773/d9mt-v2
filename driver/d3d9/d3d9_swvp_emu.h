#pragma once

// PHASE-C6: ported off Vulkan onto the Mtl seam.
//   Rc<DxvkShader> -> Rc<MtlShaderFunction>   (the SWVP ProcessVertices module)
//   include ../dxvk/dxvk_shader.h -> ../mtl/mtl_context.h (MtlShaderFunction)
// The GetShaderModule body (d3d9_swvp_emu.cpp) builds the module via the neutral
// shader backend; this header is a pure token retype.
#include <unordered_map>

#include "d3d9_include.h"

#include "../util/util_small_vector.h"

#include "../mtl/mtl_context.h"

namespace dxvk {

  class D3D9VertexDecl;
  class D3D9DeviceEx;

  struct D3D9SwvpShaderArgs {
    float viewportX;
    float viewportY;
    float viewportW;
    float viewportH;
  };

  struct D3D9CompactVertexElement {
      uint16_t Stream : 4;
      uint16_t Type : 5;
      uint16_t Method : 3;
      uint16_t Usage : 4;
      uint16_t UsageIndex;
      uint16_t Offset;

      D3D9CompactVertexElement(const D3DVERTEXELEMENT9& element)
        : Stream(element.Stream), Type(element.Type), Method(element.Method),
          Usage(element.Usage), UsageIndex(element.UsageIndex), Offset(element.Offset) {}
  };

  using D3D9CompactVertexElements = small_vector<D3D9CompactVertexElement, 4>;

  struct D3D9VertexDeclHash {
    size_t operator () (const D3D9CompactVertexElements& key) const;
  };

  struct D3D9VertexDeclEq {
    bool operator () (const D3D9CompactVertexElements& a, const D3D9CompactVertexElements& b) const;
  };

  class D3D9SWVPEmulator {

  public:

    Rc<MtlShaderFunction> GetShaderModule(D3D9DeviceEx* pDevice,  D3D9CompactVertexElements&& elements);

    UINT GetShaderCount() const {
      return m_modules.size();
    }

  private:

    dxvk::mutex                               m_mutex;

    std::unordered_map<
      D3D9CompactVertexElements, Rc<MtlShaderFunction>,
      D3D9VertexDeclHash, D3D9VertexDeclEq>   m_modules;

  };

}
