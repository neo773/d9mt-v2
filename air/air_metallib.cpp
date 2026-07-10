#include "air_metallib.h"
#include "air_sha256.h"

namespace d9mt::air {

namespace {

/* Little-endian byte stream */
struct ByteStream {
  std::vector<uint8_t> bytes;

  size_t tell() const { return bytes.size(); }

  void raw(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
  }

  void u8(uint8_t v)   { bytes.push_back(v); }
  void u16(uint16_t v) { raw(&v, 2u); }
  void u32(uint32_t v) { raw(&v, 4u); }
  void u64(uint64_t v) { raw(&v, 8u); }

  void fourcc(const char tag[5]) { raw(tag, 4u); }

  /* NUL-terminated string */
  void cstr(const std::string& s) { raw(s.c_str(), s.size() + 1u); }

  void patchU16(size_t offset, uint16_t v) {
    bytes[offset] = uint8_t(v);
    bytes[offset + 1u] = uint8_t(v >> 8);
  }
};

constexpr uint16_t kPlatformMacOS = 0x8001u;
constexpr uint8_t  kFileTypeExecutable = 0x00u;
constexpr uint8_t  kOsMacOS = 0x81u;
constexpr uint16_t kContainerVersionMajor = 2u;
constexpr uint16_t kContainerVersionMinor = 7u;

void writeFunctionTags(
        ByteStream&             s,
        const MetallibFunction& fn,
        const Sha256Digest&     bitcodeHash,
        uint64_t                bitcodeSize,
        uint64_t                publicMdOffset,
        uint64_t                privateMdOffset,
        const MetallibVersions& v) {
  s.fourcc("NAME");
  s.u16(uint16_t(fn.name.size() + 1u));
  s.cstr(fn.name);

  s.fourcc("TYPE");
  s.u16(1u);
  s.u8(uint8_t(fn.type));

  s.fourcc("HASH");
  s.u16(uint16_t(bitcodeHash.size()));
  s.raw(bitcodeHash.data(), bitcodeHash.size());

  s.fourcc("MDSZ");
  s.u16(8u);
  s.u64(bitcodeSize);

  /* offsets within the public-md / private-md / bitcode sections */
  s.fourcc("OFFT");
  s.u16(24u);
  s.u64(publicMdOffset);
  s.u64(privateMdOffset);
  s.u64(0u); /* bitcode shared, one module at offset 0 */

  s.fourcc("VERS");
  s.u16(8u);
  s.u16(v.airMajor);
  s.u16(v.airMinor);
  s.u16(v.languageMajor);
  s.u16(v.languageMinor);

  s.fourcc("ENDT");
}

/* VATT (names/locations) + VATY (type codes) records */
void writeVertexAttributeRecords(
        ByteStream&                                  s,
        const std::vector<MetallibVertexAttribute>&  attrs) {
  s.fourcc("VATT");
  size_t sizeFixup = s.tell();
  s.u16(0u);
  s.u16(uint16_t(attrs.size()));

  for (const auto& a : attrs) {
    s.cstr(a.name);
    /* u16 bitfield: attribute:8 | reserved:5 | usage:2 | active:1 */
    s.u16(uint16_t(a.location) | uint16_t(1u << 15));
  }

  s.patchU16(sizeFixup, uint16_t(s.tell() - sizeFixup - 2u));

  s.fourcc("VATY");
  s.u16(uint16_t(2u + attrs.size()));
  s.u16(uint16_t(attrs.size()));

  for (const auto& a : attrs)
    s.u8(uint8_t(a.type));
}

/* CNST record: u16 size (incl. count), u16 count, then per constant:
 * name\0, u8 MTLDataType, u16 index, u8 active */
void writeFunctionConstantRecord(
        ByteStream&                                  s,
        const std::vector<MetallibFunctionConstant>& constants) {
  s.fourcc("CNST");
  size_t sizeFixup = s.tell();
  s.u16(0u);
  s.u16(uint16_t(constants.size()));

  for (const auto& c : constants) {
    s.cstr(c.name);
    s.u8(uint8_t(c.type));
    s.u16(c.index);
    s.u8(1u); /* active */
  }

  s.patchU16(sizeFixup, uint16_t(s.tell() - sizeFixup - 2u));
}

} // namespace

std::vector<uint8_t> writeMetallib(
        const void*                           bitcode,
        size_t                                bitcodeSize,
        const std::vector<MetallibFunction>&  functions,
        const MetallibVersions&               versions) {
  Sha256Digest hash = sha256(bitcode, bitcodeSize);

  /* Build the three variable sections first so offsets are known. */
  ByteStream publicMd;
  ByteStream privateMd;
  ByteStream fnList;

  for (const auto& fn : functions) {
    /* each entry: self-inclusive u32 size, then its tag stream */
    ByteStream tags;
    writeFunctionTags(tags, fn, hash, bitcodeSize,
      publicMd.tell(), privateMd.tell(), versions);
    fnList.u32(uint32_t(tags.tell() + 4u));
    fnList.raw(tags.bytes.data(), tags.bytes.size());

    ByteStream fnPublic;
    if (fn.type == MetallibFunctionType::Vertex && !fn.vertexAttributes.empty())
      writeVertexAttributeRecords(fnPublic, fn.vertexAttributes);
    if (!fn.functionConstants.empty())
      writeFunctionConstantRecord(fnPublic, fn.functionConstants);
    fnPublic.fourcc("ENDT");

    publicMd.u32(uint32_t(fnPublic.tell()));
    publicMd.raw(fnPublic.bytes.data(), fnPublic.bytes.size());

    privateMd.u32(4u);
    privateMd.fourcc("ENDT");
  }

  constexpr uint64_t headerSize = 88u;
  /* size counts the entries only, not the function-count u32 */
  uint64_t functionListSize = fnList.tell();
  /* header | fn count | entries | ENDT | public | private | bitcode */
  uint64_t publicMdOffset = headerSize + 4u + fnList.tell() + 4u;
  uint64_t privateMdOffset = publicMdOffset + publicMd.tell();
  uint64_t bitcodeOffset = privateMdOffset + privateMd.tell();
  uint64_t fileSize = bitcodeOffset + bitcodeSize;

  ByteStream out;
  out.bytes.reserve(size_t(fileSize));

  out.fourcc("MTLB");
  out.u16(kPlatformMacOS);
  out.u16(kContainerVersionMajor);
  out.u16(kContainerVersionMinor);
  out.u8(kFileTypeExecutable);
  out.u8(kOsMacOS);
  out.u16(versions.osMajor);
  out.u16(versions.osMinor);
  out.u64(fileSize);
  out.u64(headerSize);         /* function list offset */
  out.u64(functionListSize);
  out.u64(publicMdOffset);
  out.u64(publicMd.tell());
  out.u64(privateMdOffset);
  out.u64(privateMd.tell());
  out.u64(bitcodeOffset);
  out.u64(bitcodeSize);

  out.u32(uint32_t(functions.size()));
  out.raw(fnList.bytes.data(), fnList.bytes.size());
  out.fourcc("ENDT");
  out.raw(publicMd.bytes.data(), publicMd.bytes.size());
  out.raw(privateMd.bytes.data(), privateMd.bytes.size());
  out.raw(bitcode, bitcodeSize);

  return out.bytes;
}

} // namespace d9mt::air
