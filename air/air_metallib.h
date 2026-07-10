#pragma once
/*
 * Self-contained .metallib (MTLB) container writer.
 *
 * Wraps an AIR bitcode blob (an LLVM module targeting air64) into the
 * container format that MTLDevice newLibraryWithData: accepts, with no
 * dependency on Apple tooling. Layout (all little-endian):
 *
 *   MTLB header
 *   u32 function count
 *   per-function entry: u32 self-inclusive size +
 *                       tag stream (NAME/TYPE/HASH/MDSZ/OFFT/VERS/ENDT)
 *   "ENDT"                      (extended-header terminator)
 *   public metadata section     (per function: u32 size + tag stream)
 *   private metadata section    (per function: u32 size + "ENDT")
 *   bitcode
 *
 * The per-function OFFT tag holds offsets *within* the three sections.
 * Every function's HASH tag is the SHA-256 of the (shared) bitcode blob.
 *
 * Vertex functions that consume [[stage_in]] attributes must carry
 * VATT/VATY records in their public metadata or pipeline creation fails.
 *
 * Container knowledge derived from the format itself (Metal-produced
 * libraries) and the layout notes in DXMT's docs; implementation is our own.
 */

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

/* MTLVertexFormat-ish type codes used by the VATY record. */
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

/* MTLDataType codes used by the CNST record */
enum class MetallibConstantType : uint8_t {
  Bool = 0x35,
  Int = 0x1d,
  Uint = 0x21,
  Float = 0x03,
};

struct MetallibFunctionConstant {
  std::string name;
  MetallibConstantType type = MetallibConstantType::Uint;
  uint16_t index = 0u;    /* MTLFunctionConstantValues index */
};

struct MetallibFunction {
  std::string name;
  MetallibFunctionType type = MetallibFunctionType::Vertex;
  /* stage_in attributes; vertex functions only */
  std::vector<MetallibVertexAttribute> vertexAttributes;
  /* AIR function constants referenced by this function */
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

/* Assembles the container. bitcode = WriteBitcodeToFile output. */
std::vector<uint8_t> writeMetallib(
        const void*                           bitcode,
        size_t                                bitcodeSize,
        const std::vector<MetallibFunction>&  functions,
        const MetallibVersions&               versions = MetallibVersions());

} // namespace d9mt::air
