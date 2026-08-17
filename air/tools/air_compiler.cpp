#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ir/ir_builder.h>
#include <ir/ir_disasm.h>
#include <ir/ir_legalize.h>
#include <ir/passes/ir_pass_lower_io.h>
#include <sm3/sm3_converter.h>
#include <spirv/spirv_builder.h>
#include <util/util_byte_stream.h>
#include <util/util_log.h>

#include <spirv_msl.hpp>

#include "../air_builder.h"

namespace {

const uint8_t kTestVs[] = {
#include "../../test/shadertri_vs_bytecode.inc"
};

const uint8_t kTestPs[] = {
#include "../../test/shadertri_ps_bytecode.inc"
};

struct Args {
  std::vector<uint8_t> bytecode;
  std::string outPath;
  std::string refMslPath;
  std::string manifestPath;
  std::string name = "main";
  bool dumpIr = false;
  bool dumpAir = false;
  bool lowerLegacy = false;
  bool argBuffer = false;
  bool testSpec = false;
};

void buildSpecTestIr(dxbc_spv::ir::Builder& b) {
  using namespace dxbc_spv;

  auto fn = b.add(ir::Op::Function(ir::ScalarType::eVoid));

  auto entry = b.add(ir::Op::EntryPoint(fn, ir::ShaderStage::ePixel));
  auto spec = b.add(ir::Op::DclSpecConstant(
    ir::ScalarType::eU32, entry, 0u, 0u));
  auto out = b.add(ir::Op::DclOutput(
    ir::BasicType(ir::ScalarType::eF32, 4u), entry, 0u, 0u));

  auto one = b.add(ir::Op::Constant(uint32_t(1u)));
  auto two = b.add(ir::Op::Constant(uint32_t(2u)));
  auto red = b.add(ir::Op::Constant(1.0f, 0.0f, 0.0f, 1.0f));
  auto green = b.add(ir::Op::Constant(0.0f, 1.0f, 0.0f, 1.0f));
  auto blue = b.add(ir::Op::Constant(0.0f, 0.0f, 1.0f, 1.0f));

  ir::Type vec4(ir::ScalarType::eF32, 4u);

  b.add(ir::Op::Label());
  auto isOne = b.add(ir::Op::IEq(
    ir::ScalarType::eBool, spec, one));
  auto isTwo = b.add(ir::Op::IEq(
    ir::ScalarType::eBool, spec, two));
  auto pick2 = b.add(ir::Op::Select(vec4, isTwo, green, blue));
  auto pick1 = b.add(ir::Op::Select(vec4, isOne, red, pick2));
  b.add(ir::Op::OutputStore(out, ir::SsaDef(), pick1));
  b.add(ir::Op::Return());
  b.add(ir::Op::FunctionEnd());
}

struct Manifest {
  std::string entry;
  std::string stage;
  struct Buffer { std::string name; uint32_t index; uint64_t size; };
  struct Resource { std::string name; uint32_t index; std::string kind; };
  std::vector<Buffer> buffers;
  std::vector<Resource> textures;
  std::vector<Resource> samplers;
  struct Spec { std::string name; uint32_t index; };
  std::vector<Spec> specConstants;
  uint32_t renderTargets = 1u;
  bool depthOut = false;
  bool pointSize = false;
};

void scanIrFeatures(const dxbc_spv::ir::Builder& builder, Manifest& m) {
  bool isPixel = false;
  for (auto iter = builder.getDeclarations().first;
       iter != builder.getDeclarations().second; iter++) {
    if (iter->getOpCode() == dxbc_spv::ir::OpCode::eEntryPoint)
      isPixel = dxbc_spv::ir::ShaderStage(
        iter->getOperand(iter->getOperandCount() - 1u)) ==
        dxbc_spv::ir::ShaderStage::ePixel;
  }
  uint32_t rts = 0u;
  for (auto iter = builder.getDeclarations().first;
       iter != builder.getDeclarations().second; iter++) {
    switch (iter->getOpCode()) {
      case dxbc_spv::ir::OpCode::eDclOutput:
        if (isPixel)
          rts = std::max(rts, uint32_t(iter->getOperand(1u)) + 1u);
        break;
      case dxbc_spv::ir::OpCode::eDclOutputBuiltIn: {
        auto builtIn = dxbc_spv::ir::BuiltIn(iter->getOperand(1u));
        if (builtIn == dxbc_spv::ir::BuiltIn::eDepth)
          m.depthOut = true;
        if (builtIn == dxbc_spv::ir::BuiltIn::ePointSize)
          m.pointSize = true;
      } break;
      case dxbc_spv::ir::OpCode::eDclSpecConstant:
        m.specConstants.push_back({
          "spec" + std::to_string(uint32_t(iter->getOperand(1u))),
          uint32_t(iter->getOperand(1u)) });
        break;
      default:
        break;
    }
  }
  if (isPixel && rts)
    m.renderTargets = rts;
}

void writeManifest(const Manifest& m, const std::string& path) {
  std::ofstream out(path);
  out << "{\"entry\":\"" << m.entry << "\",\"stage\":\"" << m.stage << "\",";
  out << "\"renderTargets\":" << m.renderTargets
      << ",\"depthOut\":" << (m.depthOut ? "true" : "false")
      << ",\"pointSize\":" << (m.pointSize ? "true" : "false") << ",";
  out << "\"specConstants\":[";
  for (size_t i = 0u; i < m.specConstants.size(); i++)
    out << (i ? "," : "") << "{\"name\":\"" << m.specConstants[i].name
        << "\",\"index\":" << m.specConstants[i].index << "}";
  out << "],";
  auto writeBuffers = [&out](const std::vector<Manifest::Buffer>& v) {
    for (size_t i = 0u; i < v.size(); i++)
      out << (i ? "," : "") << "{\"name\":\"" << v[i].name
          << "\",\"index\":" << v[i].index
          << ",\"size\":" << v[i].size << "}";
  };
  auto writeResources = [&out](const std::vector<Manifest::Resource>& v) {
    for (size_t i = 0u; i < v.size(); i++)
      out << (i ? "," : "") << "{\"name\":\"" << v[i].name
          << "\",\"index\":" << v[i].index
          << ",\"kind\":\"" << v[i].kind << "\"}";
  };
  out << "\"buffers\":[";  writeBuffers(m.buffers);    out << "],";
  out << "\"textures\":["; writeResources(m.textures); out << "],";
  out << "\"samplers\":["; writeResources(m.samplers); out << "]}\n";
}

void lowerLegacyBuiltins(dxbc_spv::ir::Builder& builder) {
  std::vector<dxbc_spv::ir::BuiltIn> legacyBuiltins;
  for (auto iter = builder.getDeclarations().first;
       iter != builder.getDeclarations().second; iter++) {
    if (iter->getOpCode() == dxbc_spv::ir::OpCode::eDclInputBuiltIn) {
      auto builtIn = dxbc_spv::ir::BuiltIn(iter->getOperand(1u));
      if (builtIn >= dxbc_spv::ir::BuiltIn::eLegacyAlphaTest &&
          builtIn <= dxbc_spv::ir::BuiltIn::eLegacyConstFloat)
        legacyBuiltins.push_back(builtIn);
    }
  }

  dxbc_spv::ir::LowerIoPass ioPass(builder);
  uint32_t regIndex = 0u;
  for (auto builtIn : legacyBuiltins)
    ioPass.lowerLegacyBuiltInToCbv(builtIn, 0u, regIndex++);
}

bool emitReferenceMsl(
        const dxbc_spv::ir::Builder&  srcBuilder,
        const std::string&            mslPath,
        const std::string&            manifestPath) {
  dxbc_spv::ir::Builder builder = srcBuilder;
  lowerLegacyBuiltins(builder);

  dxbc_spv::spirv::BasicResourceMapping mapping;
  dxbc_spv::spirv::SpirvBuilder::Options spvOptions = {};

  dxbc_spv::spirv::SpirvBuilder spvBuilder(builder, mapping, spvOptions);
  spvBuilder.buildSpirvBinary();
  std::vector<uint32_t> spirv = spvBuilder.getSpirvBinary();

  spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());

  spirv_cross::CompilerMSL::Options mslOptions;
  mslOptions.set_msl_version(3u, 0u);
  mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
  compiler.set_msl_options(mslOptions);

  std::string msl = compiler.compile();

  std::ofstream out(mslPath);
  out << msl;
  out.close();

  if (manifestPath.empty())
    return true;

  Manifest m;
  m.entry = compiler.get_cleansed_entry_point_name(
    compiler.get_entry_points_and_stages()[0].name,
    compiler.get_execution_model());
  m.stage = compiler.get_execution_model() == spv::ExecutionModelVertex
    ? "vertex" : "fragment";

  auto resources = compiler.get_shader_resources();

  auto addBuffers = [&](const auto& list) {
    for (const auto& r : list) {
      const auto& type = compiler.get_type(r.base_type_id);
      std::string name = compiler.get_name(r.id);
      if (name.empty())
        name = r.name;
      m.buffers.push_back({ name,
        compiler.get_automatic_msl_resource_binding(r.id),
        compiler.get_declared_struct_size(type) });
    }
  };
  addBuffers(resources.uniform_buffers);
  addBuffers(resources.storage_buffers);
  addBuffers(resources.push_constant_buffers);

  for (const auto& r : resources.separate_images) {
    const auto& type = compiler.get_type(r.type_id);
    std::string kind;
    switch (type.image.dim) {
      case spv::DimCube: kind = "cube"; break;
      case spv::Dim3D:   kind = "3d";   break;
      default:           kind = "2d";   break;
    }
    if (type.image.depth)
      kind = "depth" + kind;
    m.textures.push_back({ compiler.get_name(r.id),
      compiler.get_automatic_msl_resource_binding(r.id), kind });
  }
  for (const auto& r : resources.separate_samplers)
    m.samplers.push_back({ compiler.get_name(r.id),
      compiler.get_automatic_msl_resource_binding(r.id), "" });

  scanIrFeatures(builder, m);

  writeManifest(m, manifestPath);
  return true;
}

bool parseArgs(int argc, char** argv, Args& args) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--test-vs") {
      args.bytecode.assign(kTestVs, kTestVs + sizeof(kTestVs));
    } else if (a == "--test-ps") {
      args.bytecode.assign(kTestPs, kTestPs + sizeof(kTestPs));
    } else if (a == "--dump-ir") {
      args.dumpIr = true;
    } else if (a == "--dump-air") {
      args.dumpAir = true;
    } else if (a == "--lower-legacy") {
      args.lowerLegacy = true;
    } else if (a == "--arg-buffer") {
      args.argBuffer = true;
    } else if (a == "--test-spec") {
      args.testSpec = true;
    } else if (a == "-o" && i + 1 < argc) {
      args.outPath = argv[++i];
    } else if (a == "--ref-msl" && i + 1 < argc) {
      args.refMslPath = argv[++i];
    } else if (a == "--manifest" && i + 1 < argc) {
      args.manifestPath = argv[++i];
    } else if (a == "--name" && i + 1 < argc) {
      args.name = argv[++i];
    } else if (a[0] != '-') {
      std::ifstream f(a, std::ios::binary | std::ios::ate);
      if (!f) {
        fprintf(stderr, "cannot read %s\n", a.c_str());
        return false;
      }
      args.bytecode.resize(size_t(f.tellg()));
      f.seekg(0);
      f.read(reinterpret_cast<char*>(args.bytecode.data()),
        std::streamsize(args.bytecode.size()));
    } else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      return false;
    }
  }
  return !args.bytecode.empty() || args.testSpec;
}

}

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) {
    fprintf(stderr,
      "usage: %s (--test-vs | --test-ps | <sm3.bin>) [--dump-ir] [-o out.metallib]\n",
      argv[0]);
    return 2;
  }

  dxbc_spv::ir::Builder builder;

  if (args.testSpec) {
    buildSpecTestIr(builder);
  } else {
    dxbc_spv::util::ByteReader reader(
      args.bytecode.data(), args.bytecode.size());

    dxbc_spv::sm3::Converter::Options convOptions = {};
    convOptions.name = args.name.c_str();
    convOptions.includeDebugNames = true;

    dxbc_spv::sm3::Converter converter(reader, convOptions);
    if (!converter.convertShader(builder)) {
      fprintf(stderr, "FATAL: sm3 conversion failed\n");
      return 1;
    }

    dxbc_spv::ir::CompileOptions compileOptions = {};
    dxbc_spv::ir::legalizeIr(builder, compileOptions);

    if (args.lowerLegacy)
      lowerLegacyBuiltins(builder);
  }

  if (args.dumpIr) {
    dxbc_spv::ir::Disassembler::Options disasmOptions = {};
    dxbc_spv::ir::Disassembler disasm(builder, disasmOptions);
    std::stringstream str;
    disasm.disassemble(str);
    printf("%s\n", str.str().c_str());
  }

  if (!args.refMslPath.empty()) {
    try {
      if (!emitReferenceMsl(builder, args.refMslPath, args.manifestPath))
        return 1;
      printf("wrote %s\n", args.refMslPath.c_str());
    } catch (const std::exception& e) {
      fprintf(stderr, "FATAL[ref-msl]: %s\n", e.what());
      return 1;
    }
    return 0;
  }

  if (args.outPath.empty() && !args.dumpAir)
    return 0;

  try {
    dxbc_spv::air::AirBuilder::Options airOptions = {};
    airOptions.entryName = args.name.c_str();
    airOptions.useArgumentBuffer = args.argBuffer;

    dxbc_spv::air::AirBuilder airBuilder(builder, airOptions);
    std::vector<uint8_t> metallib = airBuilder.buildMetallib();

    if (args.dumpAir)
      printf("%s\n", airBuilder.getIrText().c_str());

    if (!args.outPath.empty()) {
      std::ofstream out(args.outPath, std::ios::binary);
      out.write(reinterpret_cast<const char*>(metallib.data()),
        std::streamsize(metallib.size()));
      printf("wrote %s (%zu bytes)\n", args.outPath.c_str(), metallib.size());
    }

    if (!args.manifestPath.empty()) {
      Manifest m;
      m.entry = args.name;
      m.stage = airBuilder.getStage() == dxbc_spv::ir::ShaderStage::eVertex
        ? "vertex" : "fragment";
      for (const auto& b : airBuilder.getBufferBindings())
        m.buffers.push_back({ b.name, b.index, b.byteSize });
      for (const auto& t : airBuilder.getTextureBindings())
        m.textures.push_back({ t.name, t.index, t.kind });
      for (const auto& s : airBuilder.getSamplerBindings())
        m.samplers.push_back({ s.name, s.index, "" });
      scanIrFeatures(builder, m);
      writeManifest(m, args.manifestPath);
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "FATAL: %s\n", e.what());
    return 1;
  }

  return 0;
}
