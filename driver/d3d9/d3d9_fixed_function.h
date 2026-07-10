#pragma once

#include <cstdint>

#include "d3d9_include.h"
#include "d3d9_caps.h"

#include "../mtl/mtl_types.h"

#include "../util/util_flags.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

#include "d3d9_shader.h"   // neutral ID3D9MetalShaderBackend (air-free)

// ---------------------------------------------------------------------------
// d3d9_fixed_function.h — PHASE-D stub of the FF module (doc 02 REWRITE, doc 03
// §3). Fixed-function in this fork is TWO precompiled uber-shaders driven by spec
// constants + CBVs + push data — there is NO D3D9FFShaderKey (doc 03 §3). The
// Metal plan authors two MSL uber-shaders (build-time metallib) with function
// constants; the CBV/push layouts (D3D9SpecData, the FF CBVs, D3D9Ffvs/FfpsPushData
// in d3d9_state.h) are kept IDENTICAL so the state tracker is reused unchanged.
//
// Here we keep the FF *keys/enums* (blend/tex-stage flags, TCI bits) and the
// module-set shape; the actual VS/FS metallib lookup is stubbed (returns null)
// pending the build-time MSL uber-shaders. No v2/air include (doc 03 §5). The FF
// input signature accessor (g_ffIsgn, D3D9InputSignature) needs the shader-analysis
// header (dxbc_spv) and is deferred with a PHASE-D marker.
// ---------------------------------------------------------------------------

namespace dxvk {

  class D3D9DeviceEx;
  struct D3D9Options;

  enum D3D9FF_VertexBlendMode {
    D3D9FF_VertexBlendMode_Disabled,
    D3D9FF_VertexBlendMode_Normal,
    D3D9FF_VertexBlendMode_Tween,
  };

  // Texture stage properties
  enum class D3D9TextureStageStateFlag {
    UsesTexture = 0u,
    UsesCurrent = 1u,
    UsesTemp    = 2u,
  };

  using D3D9TextureStageStateFlags = Flags<D3D9TextureStageStateFlag>;

  // No idea what this is
  constexpr uint32_t TCIOffset = 16;
  constexpr uint32_t TCIMask   = 0b111 << TCIOffset;

  /**
   * \brief FF uber-shader module set (was D3D9FFShaderModuleSet over DxvkShader)
   *
   * PHASE-D: holds the neutral shader-backend handles for the two build-time FF
   * uber-shaders (VS + FS). GetShader() returns null until the MSL uber-shaders are
   * authored + compiled into the driver's build-time metallib.
   */
  class D3D9FFShaderModuleSet : public RcObject {

  public:

    D3D9FFShaderModuleSet() = default;

    template<D3D9ShaderType Stage>
    Rc<ID3D9MetalShaderBackend> GetShader() {
      // PHASE-D: return the precompiled FF uber-shader for this stage.
      return Stage == D3D9ShaderType::VertexShader ? m_vs : m_fs;
    }

  private:

    Rc<ID3D9MetalShaderBackend> m_vs;
    Rc<ID3D9MetalShaderBackend> m_fs;

  };

}
