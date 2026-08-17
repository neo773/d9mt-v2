#pragma once

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
    const char* entryName = "main";
    bool foldSpecConstants = false;
    bool useArgumentBuffer = false;
    std::unordered_map<uint32_t, uint32_t> specValues;
  };

  static constexpr uint32_t kAbTableIndex = 21u;

  AirBuilder(const ir::Builder& builder, const Options& options);

  ~AirBuilder();

  std::vector<uint8_t> buildMetallib();

  struct BufferBinding {
    std::string name;
    uint32_t    index = 0u;
    uint64_t    byteSize = 0u;
    bool        isCbv = false;
    uint32_t    regIndex = 0u;
    int32_t     abWord = -1;
  };

  struct AbInfo {
    bool     used = false;
    uint32_t tableIndex = kAbTableIndex;
    uint32_t sizeBytes = 0u;
  };

  const AbInfo& getAbInfo() const {
    return m_abInfo;
  }

  ir::ShaderStage getStage() const {
    return m_stage;
  }

  const std::vector<BufferBinding>& getBufferBindings() const {
    return m_bufferBindings;
  }

  struct ResourceBinding {
    std::string name;
    uint32_t    index = 0u;
    std::string kind;
    int32_t     abWord = -1;
  };

  const std::vector<ResourceBinding>& getTextureBindings() const {
    return m_textureBindings;
  }

  const std::vector<ResourceBinding>& getSamplerBindings() const {
    return m_samplerBindings;
  }

  struct SpecBinding {
    std::string name;
    uint32_t    index = 0u;
  };

  const std::vector<SpecBinding>& getSpecBindings() const {
    return m_specBindings;
  }

  struct OutputInfo {
    uint32_t renderTargetCount = 0u;
    bool     writesDepth = false;
    bool     writesPointSize = false;
  };

  const OutputInfo& getOutputInfo() const {
    return m_outputInfo;
  }

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

}
