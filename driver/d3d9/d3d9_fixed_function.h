#pragma once

#include <cstdint>

#include "d3d9_include.h"
#include "d3d9_caps.h"

#include "../mtl/mtl_types.h"

#include "../util/util_flags.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

#include "d3d9_shader.h"

namespace dxvk {

  class D3D9DeviceEx;
  struct D3D9Options;
  class MtlDevice;
  class D3D9InputSignature;

  const D3D9InputSignature& GetFixedFunctionIsgn();

  enum D3D9FF_VertexBlendMode {
    D3D9FF_VertexBlendMode_Disabled,
    D3D9FF_VertexBlendMode_Normal,
    D3D9FF_VertexBlendMode_Tween,
  };

  enum class D3D9TextureStageStateFlag {
    UsesTexture = 0u,
    UsesCurrent = 1u,
    UsesTemp    = 2u,
  };

  using D3D9TextureStageStateFlags = Flags<D3D9TextureStageStateFlag>;

  constexpr uint32_t TCIOffset = 16;
  constexpr uint32_t TCIMask   = 0b111 << TCIOffset;

  class D3D9FFShaderModuleSet : public RcObject {

  public:

    D3D9FFShaderModuleSet() = default;

    void Initialize(const Rc<MtlDevice>& device);

    template<D3D9ShaderType Stage>
    Rc<MtlShaderFunction> GetShader() {
      return Stage == D3D9ShaderType::VertexShader ? m_vs : m_fs;
    }

  private:

    Rc<MtlShaderFunction> m_vs;
    Rc<MtlShaderFunction> m_fs;

  };

}
