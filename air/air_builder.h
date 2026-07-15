#pragma once
/*
 * AirBuilder: dxbc-spirv IR -> AIR (LLVM 15 bitcode) -> metallib.
 *
 * Backend counterpart to dxbc-spirv's spirv::SpirvBuilder: walks a
 * legalized ir::Builder and emits an LLVM module targeting air64, wrapped
 * in a metallib container that MTLDevice newLibraryWithData accepts. This
 * skips SPIR-V, SPIRV-Cross and the MSL frontend entirely.
 *
 * v0 binding model (documented so the driver side can match):
 *  - DclInput            -> [[stage_in]] vertex attribute at its location
 *                           (VS), or a varying interpolant (PS), passed as
 *                           a plain vector parameter + air metadata.
 *  - DclOutput           -> member of the entry return struct; varyings are
 *                           name-matched VS<->PS via "generated(locN_user)".
 *  - DclInputBuiltIn ConstFloat        -> constant buffer at [[buffer(0)]]
 *  - other buffer-like DclInputBuiltIn (LegacyClipPlanes, LegacyPointArgs,
 *    LegacyAlphaTest, ...) -> constant buffers at [[buffer(1..)]] in
 *    declaration order. Struct layouts use natural (LLVM) alignment.
 *  - DclCbv (post io-lowering)         -> [[buffer(regIndex)]] when no
 *    legacy builtins remain, else [[buffer(8 + regIndex)]]. Accessed via
 *    DescriptorLoad + BufferLoad.
 *  - DclOutputBuiltIn Position -> return struct member 0 (air.position).
 *  - DclOutputBuiltIn PointSize -> float member (air.point_size).
 *  - DclOutputBuiltIn ClipDistance -> [N x float] member (air.clip_distance
 *    + air.clip_distance_array_size N).
 *  - DclOutputBuiltIn Depth (PS) -> trailing float member (air.depth,
 *    qualifier air.any).
 *  - DclInputBuiltIn Position / IsFrontFace / PointCoord (PS) -> stage-in
 *    raster parameters (air.position / air.front_facing / air.point_coord).
 *  - PS DclOutput at location N -> [[color(N)]] (air.render_target N).
 *  - DclSpecConstant -> AIR function constant (MTLFunctionConstantValues
 *    at PSO creation selects the value; index = the IR specId). One
 *    metallib serves all specializations. Set Options::foldSpecConstants
 *    to bake specific values instead (no function-constant plumbing
 *    required by the consumer).
 *
 * Vertex functions report their stage_in attributes for the metallib
 * VATT/VATY records via getVertexAttributes().
 */

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <ir/ir.h>
#include <ir/ir_builder.h>

#include "air_metallib.h"

namespace dxbc_spv::air {

class AirBuilder {

public:

  struct Options {
    /** Entry point name in the emitted library. */
    const char* entryName = "main";
    /** Bake spec constants to fixed values instead of emitting AIR
     *  function constants. Key = specId; missing ids use the IR default. */
    bool foldSpecConstants = false;
    /** Argument-buffer binding model: textures, samplers and constant
     *  buffers become 8-byte handle words in ONE struct bound at
     *  [[buffer(kAbTableIndex)]] (CPU writes gpuResourceID/gpuAddress
     *  words; no per-slot argument-table calls). Push data keeps its own
     *  buffer slot. Off = classic per-slot [[texture/sampler/buffer(n)]]. */
    bool useArgumentBuffer = false;
    std::unordered_map<uint32_t, uint32_t> specValues;
  };

  /** Argument-table buffer index the AB binds at (useArgumentBuffer). */
  static constexpr uint32_t kAbTableIndex = 21u;

  AirBuilder(const ir::Builder& builder, const Options& options);

  ~AirBuilder();

  /** Builds the LLVM module and wraps it into a metallib.
   *  Throws std::runtime_error on unsupported IR. */
  std::vector<uint8_t> buildMetallib();

  /** One buffer argument of the built entry point. */
  struct BufferBinding {
    std::string name;
    uint32_t    index = 0u;    /* Metal [[buffer(index)]] */
    uint64_t    byteSize = 0u; /* allocation size of the pointee */
    bool        isCbv = false; /* came from a DclCbv (vs legacy builtin / push) */
    uint32_t    regIndex = 0u; /* DclCbv register index (== D3D9 CbvIndex) */
    int32_t     abWord = -1;   /* word index in the argument buffer, -1 = own slot */
  };

  /** Argument-buffer summary (valid after buildMetallib). */
  struct AbInfo {
    bool     used = false;      /* entry consumes an AB */
    uint32_t tableIndex = kAbTableIndex;
    uint32_t sizeBytes = 0u;    /* AB struct size (wordCount * 8) */
  };

  const AbInfo& getAbInfo() const {
    return m_abInfo;
  }

  /** Shader stage seen on the IR entry point (valid after buildMetallib). */
  ir::ShaderStage getStage() const {
    return m_stage;
  }

  /** Buffer bindings of the entry point (valid after buildMetallib). */
  const std::vector<BufferBinding>& getBufferBindings() const {
    return m_bufferBindings;
  }

  /** One texture/sampler argument of the built entry point. */
  struct ResourceBinding {
    std::string name;
    uint32_t    index = 0u;  /* Metal [[texture(n)]] / [[sampler(n)]] slot,
                              * == the D3D9 register in both binding models */
    std::string kind;        /* textures: "2d" | "cube" | "3d" */
    int32_t     abWord = -1; /* word index in the argument buffer, -1 = own slot */
  };

  const std::vector<ResourceBinding>& getTextureBindings() const {
    return m_textureBindings;
  }

  const std::vector<ResourceBinding>& getSamplerBindings() const {
    return m_samplerBindings;
  }

  /** One function constant of the entry point (from DclSpecConstant). */
  struct SpecBinding {
    std::string name;
    uint32_t    index = 0u;    /* MTLFunctionConstantValues index */
  };

  const std::vector<SpecBinding>& getSpecBindings() const {
    return m_specBindings;
  }

  /** Entry output description (drives PSO/attachment setup). */
  struct OutputInfo {
    uint32_t renderTargetCount = 0u; /* PS color outputs */
    bool     writesDepth = false;    /* PS oDepth */
    bool     writesPointSize = false;
  };

  const OutputInfo& getOutputInfo() const {
    return m_outputInfo;
  }

  /** LLVM IR text of the built module, for debugging. */
  const std::string& getIrText() const {
    return m_irText;
  }

private:

  const ir::Builder& m_builder;
  Options            m_options;

  ir::ShaderStage    m_stage = ir::ShaderStage::eVertex;
  std::string        m_irText;

  std::vector<BufferBinding>   m_bufferBindings;
  std::vector<ResourceBinding> m_textureBindings;
  std::vector<ResourceBinding> m_samplerBindings;
  std::vector<SpecBinding>     m_specBindings;
  OutputInfo                   m_outputInfo;
  AbInfo                       m_abInfo;

};

} // namespace dxbc_spv::air
