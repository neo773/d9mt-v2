#pragma once

#include <string>
#include <vector>

#include "d3d9_shader.h"            // ID3D9MetalShaderBackend, D3D9ShaderOptions, masks
#include "d3d9_shader_analysis.h"   // D3D9ShaderAnalysis (dxbc_spv, -I dxbc-spirv)

// ---------------------------------------------------------------------------
// d3d9_shader_compiler.{h,cpp} — the REAL SM3 -> AIR shader module (doc 03,
// doc 04). This is the concrete air-backed implementation behind the neutral
// ID3D9MetalShaderBackend interface declared in d3d9_shader.h.
//
// Two-phase, mirroring DXVK's DxvkIrShader (doc 03 §1b):
//   Phase A (createD3D9MetalShader): SM3 bytecode
//       -> dxbc_spv::sm3::Converter::convertShader
//       -> D3D9ShaderLowerLegacyInputPass  (ported verbatim from upstream
//          DXVK-3.0 src/d3d9/d3d9_shader.cpp:13-1091; backend-neutral)
//       -> dxbc_spv::ir::legalizeIr
//       -> ir::Serializer  (legalized IR cached in the backend object)
//   Phase B (getMetallib): deserialize the legalized IR
//       -> dxbc_spv::ir::LowerIoPass link-time fixups (flat interp; the
//          full VS<->PS resolveMismatchedIo pairing needs the paired IoMap,
//          which the neutral cookie interface does not carry — see .cpp note)
//       -> ir::Serializer
//       -> D9MTAirCompile bridge thunk (PE -> unix AirBuilder -> metallib)
//       -> metallib bytes.
//
// No SPIR-V, no LLVM on the PE side: the sm3 + ir + ir/passes + ir_serialize
// TUs compile into the DLL (libdxbcsm3.a); AirBuilder+LLVM live unix-side and
// are reached only through the D9MTAir* bridge thunk.
// ---------------------------------------------------------------------------

namespace dxvk {

  /**
   * \brief Builds the air-backed shader backend (Phase A).
   *
   * Runs SM3->IR conversion, the legacy-input lowering pass and legalization,
   * and caches the serialized legalized IR inside the returned object. The
   * returned backend's getMetallib() performs Phase B on demand.
   *
   * \param [in] name   Debug/entry-point name (sm3 Options.name).
   * \param [in] stage  Vertex or Fragment.
   * \param [in] options SWVP / float-emulation / force-sampler-type flags.
   * \param [in] analysis SM3 analysis (constant layout, masks, signatures).
   * \param [in] bytecode SM3 DXBC bytecode (length taken from analysis).
   * \returns backend, or nullptr on conversion failure.
   */
  Rc<ID3D9MetalShaderBackend> createD3D9MetalShader(
    const std::string&        name,
          MtlShaderStage      stage,
    const D3D9ShaderOptions&  options,
    const D3D9ShaderAnalysis& analysis,
    const void*               bytecode);

}
