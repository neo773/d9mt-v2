#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace d9mt::air {

enum class MetallibFunctionType : uint8_t {
  Vertex = 0,
  Fragment = 1,
  Kernel = 2,
  Mesh = 7,
  Object = 8,
};

enum class MetallibAttributeType : uint8_t {
  Float = 0x03,
  Float2 = 0x04,
  Float3 = 0x05,
  Float4 = 0x06,
  Int = 0x21,
  Int2 = 0x22,
  Int3 = 0x23,
  Int4 = 0x24,
  Uint = 0x25,
  Uint2 = 0x26,
  Uint3 = 0x27,
  Uint4 = 0x28,
};

struct MetallibVertexAttribute {
  std::string name;
  uint8_t location = 0u;
  MetallibAttributeType type = MetallibAttributeType::Float4;
};

enum class MetallibConstantType : uint8_t {
  Bool = 0x35,
  Int = 0x1d,
  Uint = 0x21,
  Float = 0x03,
};

struct MetallibFunctionConstant {
  std::string name;
  MetallibConstantType type = MetallibConstantType::Uint;
  uint16_t index = 0u;
};

struct MetallibFunction {
  std::string name;
  MetallibFunctionType type = MetallibFunctionType::Vertex;
  std::vector<MetallibVertexAttribute> vertexAttributes;
  std::vector<MetallibFunctionConstant> functionConstants;
};

struct MetallibVersions {
  uint16_t airMajor = 2u;
  uint16_t airMinor = 9u;
  uint16_t languageMajor = 3u;
  uint16_t languageMinor = 0u;
  uint16_t osMajor = 15u;
  uint16_t osMinor = 0u;
};

std::vector<uint8_t> writeMetallib(
        const void*                           bitcode,
        size_t                                bitcodeSize,
        const std::vector<MetallibFunction>&  functions,
        const MetallibVersions&               versions = MetallibVersions());

}
