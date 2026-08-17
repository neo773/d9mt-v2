#pragma once

#include <string>
#include <vector>

#include "d3d9_shader.h"
#include "d3d9_shader_analysis.h"

namespace dxvk {

  Rc<ID3D9MetalShaderBackend> createD3D9MetalShader(
    const std::string&        name,
          MtlShaderStage      stage,
    const D3D9ShaderOptions&  options,
    const D3D9ShaderAnalysis& analysis,
    const void*               bytecode);

}
