#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/raw_ostream.h"

#include "../air_metallib.h"

using namespace llvm;

namespace {

const char* kDataLayout =
  "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64"
  "-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64"
  "-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256"
  "-v512:512:512-v1024:1024:1024-n8:16:32";

const char* kTriple = "air64-apple-macosx15.0.0";

MDNode* mdTuple(LLVMContext& ctx, ArrayRef<Metadata*> ops) {
  return MDTuple::get(ctx, ops);
}

Metadata* mdString(LLVMContext& ctx, StringRef s) {
  return MDString::get(ctx, s);
}

Metadata* mdI32(LLVMContext& ctx, uint32_t v) {
  return ConstantAsMetadata::get(
    ConstantInt::get(Type::getInt32Ty(ctx), v));
}

void addFastMathFnAttrs(Function* fn) {
  fn->addFnAttr(Attribute::NoUnwind);
  fn->addFnAttr(Attribute::WillReturn);
  fn->addFnAttr(Attribute::MustProgress);
  fn->addFnAttr("approx-func-fp-math", "true");
  fn->addFnAttr("no-infs-fp-math", "true");
  fn->addFnAttr("no-nans-fp-math", "true");
  fn->addFnAttr("no-signed-zeros-fp-math", "true");
  fn->addFnAttr("no-trapping-math", "true");
  fn->addFnAttr("unsafe-fp-math", "true");
  fn->addFnAttr("frame-pointer", "all");
  fn->addFnAttr("min-legal-vector-width", "128");
  fn->addFnAttr("no-builtins");
}

void addModuleLevelMetadata(Module& mod) {
  LLVMContext& ctx = mod.getContext();

  mod.addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8u);
  mod.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16u);

  mod.getOrInsertNamedMetadata("air.compile_options")->addOperand(
    mdTuple(ctx, { mdString(ctx, "air.compile.denorms_disable") }));
  mod.getNamedMetadata("air.compile_options")->addOperand(
    mdTuple(ctx, { mdString(ctx, "air.compile.fast_math_enable") }));
  mod.getNamedMetadata("air.compile_options")->addOperand(
    mdTuple(ctx, { mdString(ctx, "air.compile.framebuffer_fetch_enable") }));

  mod.getOrInsertNamedMetadata("air.version")->addOperand(
    mdTuple(ctx, { mdI32(ctx, 2u), mdI32(ctx, 7u), mdI32(ctx, 0u) }));
  mod.getOrInsertNamedMetadata("air.language_version")->addOperand(
    mdTuple(ctx, { mdString(ctx, "Metal"),
                   mdI32(ctx, 3u), mdI32(ctx, 0u), mdI32(ctx, 0u) }));
  mod.getOrInsertNamedMetadata("air.source_file_name")->addOperand(
    mdTuple(ctx, { mdString(ctx, "air_prototype") }));
  mod.getOrInsertNamedMetadata("llvm.ident")->addOperand(
    mdTuple(ctx, { mdString(ctx, "d9mt air prototype") }));
}

Function* buildVertexFunction(Module& mod) {
  LLVMContext& ctx = mod.getContext();
  Type* f32 = Type::getFloatTy(ctx);
  Type* float4 = FixedVectorType::get(f32, 4u);

  StructType* retType = StructType::get(ctx, { float4, float4 },
    true);

  FunctionType* fnType =
    FunctionType::get(retType, { float4, float4 }, false);
  Function* fn = Function::Create(fnType,
    GlobalValue::ExternalLinkage, "vs_main", mod);
  addFastMathFnAttrs(fn);

  BasicBlock* entry = BasicBlock::Create(ctx, "", fn);
  IRBuilder<> b(entry);
  b.setFastMathFlags(FastMathFlags::getFast());

  Value* ret = UndefValue::get(retType);
  ret = b.CreateInsertValue(ret, fn->getArg(0u), { 0u });
  ret = b.CreateInsertValue(ret, fn->getArg(1u), { 1u });
  b.CreateRet(ret);

  Metadata* outputs = mdTuple(ctx, {
    mdTuple(ctx, {
      mdString(ctx, "air.position"),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "pos") }),
    mdTuple(ctx, {
      mdString(ctx, "air.vertex_output"),
      mdString(ctx, "generated(5colorDv4_f)"),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "color") }),
  });

  Metadata* inputs = mdTuple(ctx, {
    mdTuple(ctx, {
      mdI32(ctx, 0u), mdString(ctx, "air.vertex_input"),
      mdString(ctx, "air.location_index"), mdI32(ctx, 0u), mdI32(ctx, 1u),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "pos") }),
    mdTuple(ctx, {
      mdI32(ctx, 1u), mdString(ctx, "air.vertex_input"),
      mdString(ctx, "air.location_index"), mdI32(ctx, 1u), mdI32(ctx, 1u),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "color") }),
  });

  mod.getOrInsertNamedMetadata("air.vertex")->addOperand(
    mdTuple(ctx, { ValueAsMetadata::get(fn), outputs, inputs }));

  return fn;
}

Function* buildFragmentFunction(Module& mod) {
  LLVMContext& ctx = mod.getContext();
  Type* f32 = Type::getFloatTy(ctx);
  Type* float4 = FixedVectorType::get(f32, 4u);

  FunctionType* fnType =
    FunctionType::get(float4, { float4, float4 }, false);
  Function* fn = Function::Create(fnType,
    GlobalValue::ExternalLinkage, "fs_main", mod);
  addFastMathFnAttrs(fn);

  BasicBlock* entry = BasicBlock::Create(ctx, "", fn);
  IRBuilder<> b(entry);
  b.setFastMathFlags(FastMathFlags::getFast());
  b.CreateRet(fn->getArg(1u));

  Metadata* outputs = mdTuple(ctx, {
    mdTuple(ctx, {
      mdString(ctx, "air.render_target"), mdI32(ctx, 0u), mdI32(ctx, 0u),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4") }),
  });

  Metadata* inputs = mdTuple(ctx, {
    mdTuple(ctx, {
      mdI32(ctx, 0u), mdString(ctx, "air.position"),
      mdString(ctx, "air.center"), mdString(ctx, "air.no_perspective"),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "pos"),
      mdString(ctx, "air.arg_unused") }),
    mdTuple(ctx, {
      mdI32(ctx, 1u), mdString(ctx, "air.fragment_input"),
      mdString(ctx, "generated(5colorDv4_f)"),
      mdString(ctx, "air.center"), mdString(ctx, "air.perspective"),
      mdString(ctx, "air.arg_type_name"), mdString(ctx, "float4"),
      mdString(ctx, "air.arg_name"), mdString(ctx, "color") }),
  });

  mod.getOrInsertNamedMetadata("air.fragment")->addOperand(
    mdTuple(ctx, { ValueAsMetadata::get(fn), outputs, inputs }));

  return fn;
}

}

int main(int argc, char** argv) {
  std::string outPath = argc > 1 ? argv[1] : "air_prototype.metallib";
  bool dumpIr = argc > 2 && std::string(argv[2]) == "--dump-ir";

  LLVMContext ctx;
  // Metal's bitcode ingestion rejects opaque `ptr` (LLVM 15's default) with
  // "Failed to upgrade function bitcode"; AIR requires typed pointers.
  ctx.setOpaquePointers(false);
  Module mod("air_prototype", ctx);
  mod.setDataLayout(kDataLayout);
  mod.setTargetTriple(kTriple);
  mod.setSDKVersion(VersionTuple(15u, 0u));

  buildVertexFunction(mod);
  buildFragmentFunction(mod);
  addModuleLevelMetadata(mod);

  if (verifyModule(mod, &errs())) {
    fprintf(stderr, "FATAL: module verification failed\n");
    return 1;
  }

  if (dumpIr)
    mod.print(outs(), nullptr);

  SmallVector<char, 0> bitcode;
  raw_svector_ostream bcStream(bitcode);
  WriteBitcodeToFile(mod, bcStream);

  std::vector<d9mt::air::MetallibFunction> functions;
  functions.push_back({
    "vs_main", d9mt::air::MetallibFunctionType::Vertex,
    {
      { "pos",   0u, d9mt::air::MetallibAttributeType::Float4 },
      { "color", 1u, d9mt::air::MetallibAttributeType::Float4 },
    } });
  functions.push_back({
    "fs_main", d9mt::air::MetallibFunctionType::Fragment, {} });

  d9mt::air::MetallibVersions versions;
  versions.airMajor = 2u;
  versions.airMinor = 7u;
  versions.languageMajor = 3u;
  versions.languageMinor = 0u;
  versions.osMajor = 15u;
  versions.osMinor = 0u;

  std::vector<uint8_t> metallib = d9mt::air::writeMetallib(
    bitcode.data(), bitcode.size(), functions, versions);

  std::ofstream out(outPath, std::ios::binary);
  out.write(reinterpret_cast<const char*>(metallib.data()),
    std::streamsize(metallib.size()));
  out.close();

  std::ofstream bcOut(outPath + ".air", std::ios::binary);
  bcOut.write(bitcode.data(), std::streamsize(bitcode.size()));
  bcOut.close();

  printf("wrote %s (%zu bytes, bitcode %zu bytes)\n",
    outPath.c_str(), metallib.size(), size_t(bitcode.size()));
  return 0;
}
