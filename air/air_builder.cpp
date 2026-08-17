#include "air_builder.h"

#include <stdexcept>
#include <unordered_map>

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace dxbc_spv::air {

namespace {

using namespace llvm;

const char* kDataLayout =
  "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64"
  "-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64"
  "-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256"
  "-v512:512:512-v1024:1024:1024-n8:16:32";

const char* kTriple = "air64-apple-macosx15.0.0";

constexpr unsigned kAsConstant = 2u;
constexpr unsigned kAsTexture = 1u;
constexpr unsigned kAsSampler = 2u;

[[noreturn]] void unsupported(const std::string& what) {
  throw std::runtime_error("AirBuilder: unsupported " + what);
}

std::string opCodeName(ir::OpCode code) {
  return "op #" + std::to_string(uint32_t(code));
}


struct OutputVar {
  ir::SsaDef def = {};
  ir::BuiltIn builtin = ir::BuiltIn::ePosition;
  bool isBuiltin = false;
  uint32_t location = 0u;
  uint32_t orderKey = 0u;
  AllocaInst* storage = nullptr;
  Type* type = nullptr;
  int32_t retIndex = -1;
};

struct SpecVar {
  ir::SsaDef def = {};
  uint32_t specId = 0u;
  uint32_t defaultValue = 0u;
  GlobalVariable* workingGlobal = nullptr;
  GlobalVariable* initGlobal = nullptr;
};

struct InputVar {
  ir::SsaDef def = {};
  uint32_t location = 0u;
  uint32_t argIndex = 0u;
  Type* type = nullptr;
  bool isBuiltin = false;
  ir::BuiltIn builtin = ir::BuiltIn::ePosition;
};

struct BufferVar {
  ir::SsaDef def = {};
  ir::BuiltIn builtin = ir::BuiltIn::eLegacyConstFloat;
  bool isCbv = false;
  bool isPushData = false;
  uint32_t regIndex = 0u;
  uint32_t bufferIndex = 0u;
  uint32_t argIndex = 0u;
  int32_t abWord = -1;
  Type* type = nullptr;
  std::string name;
};

struct TextureVar {
  ir::SsaDef def = {};
  ir::ResourceKind kind = ir::ResourceKind::eImage2D;
  uint32_t slot = 0u;
  uint32_t argIndex = 0u;
  int32_t abWord = -1;
  Type* handleType = nullptr;
  std::string name;
};

struct SamplerVar {
  ir::SsaDef def = {};
  uint32_t slot = 0u;
  uint32_t argIndex = 0u;
  int32_t abWord = -1;
  std::string name;
};

constexpr uint32_t kPushSharedBlockSize = 32u;

}


struct AirBuilderImpl {

  const ir::Builder& ir;
  const AirBuilder::Options& options;

  LLVMContext ctx;
  std::unique_ptr<Module> mod;
  std::unique_ptr<IRBuilder<>> b;

  ir::ShaderStage stage = ir::ShaderStage::eVertex;
  ir::SsaDef entryDef;

  std::unordered_map<uint32_t, Value*> values;
  std::unordered_map<uint32_t, Function*> functions;
  std::unordered_map<uint32_t, BasicBlock*> blocks;

  /* back-edge phi operands may reference forward defs, so they are wired
   * only after the whole function body has been emitted */
  std::vector<std::pair<PHINode*, const ir::Op*>> pendingPhis;

  std::unordered_map<uint32_t, Type*> scratchTypes;
  std::unordered_map<uint64_t, AllocaInst*> scratchAllocas;

  std::unordered_map<uint32_t, std::vector<uint32_t>> fnHiddenBuffers;
  bool m_havePush = false;
  uint32_t m_pushBytes = 0u;
  ir::SsaDef m_pushDef;
  std::unordered_map<uint32_t, uint32_t> m_pushOffset;
  std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> fnBufferArgs;

  std::vector<InputVar>   inputs;
  std::vector<OutputVar>  outputs;
  std::vector<BufferVar>  buffers;
  std::vector<TextureVar> textures;
  std::vector<SamplerVar> samplers;
  std::vector<SpecVar>    specs;

  bool useAB = false;
  StructType* abStruct = nullptr;
  uint32_t abWordCount = 0u;
  uint32_t entryAbArgIndex = ~0u;
  static constexpr uint32_t kAbDeclId = 0xFFFFFFFEu;

  std::unordered_map<std::string, Function*> airIntrinsics;

  Function* entryFn = nullptr;
  StructType* entryRetType = nullptr;

  Function* discardFn = nullptr;

  AirBuilderImpl(const ir::Builder& irBuilder, const AirBuilder::Options& opt)
  : ir(irBuilder), options(opt), useAB(opt.useArgumentBuffer) {
    ctx.setOpaquePointers(false); /* AIR is pre-opaque-pointer LLVM */
    mod = std::make_unique<Module>("air_shader", ctx);
    mod->setDataLayout(kDataLayout);
    mod->setTargetTriple(kTriple);
    b = std::make_unique<IRBuilder<>>(ctx);

    FastMathFlags fmf;
    fmf.setFast();
    b->setFastMathFlags(fmf);
  }

  Type* mapScalarType(ir::ScalarType t, bool inMemory) {
    switch (t) {
      case ir::ScalarType::eVoid: return Type::getVoidTy(ctx);
      case ir::ScalarType::eBool: return inMemory
        ? Type::getInt32Ty(ctx) : Type::getInt1Ty(ctx);
      case ir::ScalarType::eI32:
      case ir::ScalarType::eU32: return Type::getInt32Ty(ctx);
      case ir::ScalarType::eI16:
      case ir::ScalarType::eU16:
      case ir::ScalarType::eMinI16:
      case ir::ScalarType::eMinU16: return Type::getInt16Ty(ctx);
      case ir::ScalarType::eI8:
      case ir::ScalarType::eU8: return Type::getInt8Ty(ctx);
      case ir::ScalarType::eF16: return Type::getHalfTy(ctx);
      case ir::ScalarType::eF32: return Type::getFloatTy(ctx);
      default:
        unsupported("scalar type #" + std::to_string(uint32_t(t)));
    }
  }

  Type* mapBasicType(ir::BasicType t, bool inMemory) {
    Type* scalar = mapScalarType(t.getBaseType(), inMemory);
    return t.isVector()
      ? FixedVectorType::get(scalar, t.getVectorSize())
      : scalar;
  }

  Type* mapType(const ir::Type& t, bool inMemory) {
    Type* base = nullptr;

    if (t.isVoidType())
      return Type::getVoidTy(ctx);

    if (t.getStructMemberCount() <= 1u) {
      base = mapBasicType(t.getBaseType(0u), inMemory);
    } else {
      SmallVector<Type*, 8> members;
      for (uint32_t i = 0u; i < t.getStructMemberCount(); i++)
        members.push_back(mapBasicType(t.getBaseType(i), inMemory));
      base = StructType::get(ctx, members,  false);
    }

    for (uint32_t i = 0u; i < t.getArrayDimensions(); i++)
      base = ArrayType::get(base, t.getArraySize(i));

    return base;
  }

  Constant* mapConstant(const ir::Op& op) {
    const ir::Type& t = op.getType();

    if (t.isBasicType() && !t.isArrayType()) {
      ir::BasicType bt = t.getBaseType(0u);
      SmallVector<Constant*, 4> elems;

      for (uint32_t i = 0u; i < bt.getVectorSize(); i++) {
        ir::Operand operand = op.getOperand(i);
        switch (bt.getBaseType()) {
          case ir::ScalarType::eBool:
            elems.push_back(ConstantInt::get(Type::getInt1Ty(ctx),
              bool(operand) ? 1u : 0u));
            break;
          case ir::ScalarType::eF32:
            elems.push_back(ConstantFP::get(Type::getFloatTy(ctx),
              double(float(operand))));
            break;
          case ir::ScalarType::eF16:
            elems.push_back(ConstantFP::get(Type::getHalfTy(ctx),
              double(float(util::float16_t(operand)))));
            break;
          case ir::ScalarType::eI32:
          case ir::ScalarType::eU32:
            elems.push_back(ConstantInt::get(Type::getInt32Ty(ctx),
              uint64_t(uint32_t(operand)), false));
            break;
          default:
            unsupported("constant scalar type");
        }
      }

      if (elems.size() == 1u)
        return elems[0];
      return ConstantVector::get(elems);
    }

    unsupported("aggregate constant");
  }

  Value* getValue(ir::SsaDef def) {
    auto it = values.find(def.getId());
    if (it != values.end())
      return it->second;

    const ir::Op& op = this->ir.getOp(def);
    if (op.getOpCode() == ir::OpCode::eConstant) {
      Value* v = mapConstant(op);
      values.emplace(def.getId(), v);
      return v;
    }
    if (op.getOpCode() == ir::OpCode::eUndef) {
      Value* v = UndefValue::get(mapType(op.getType(), false));
      values.emplace(def.getId(), v);
      return v;
    }
    if (op.getOpCode() == ir::OpCode::eDclSpecConstant) {
      for (const SpecVar& spec : specs) {
        if (spec.def != def)
          continue;
        if (options.foldSpecConstants) {
          auto vIt = options.specValues.find(spec.specId);
          uint32_t v = vIt != options.specValues.end()
            ? vIt->second : spec.defaultValue;
          return ConstantInt::get(Type::getInt32Ty(ctx), v);
        }
        /* fresh load per use; a cached load would not dominate all uses */
        return b->CreateLoad(Type::getInt32Ty(ctx), spec.workingGlobal);
      }
    }

    unsupported("forward value reference to " + opCodeName(op.getOpCode()));
  }

  void setValue(const ir::Op& op, Value* v) {
    values.emplace(op.getDef().getId(), v);
  }

  ir::SsaDef operandDef(const ir::Op& op, uint32_t idx) {
    return ir::SsaDef(op.getOperand(idx));
  }

  uint32_t constantAddress(ir::SsaDef def) {
    if (!def)
      return ~0u;
    const ir::Op& op = this->ir.getOp(def);
    if (op.getOpCode() != ir::OpCode::eConstant || !op.getType().isScalarType())
      unsupported("non-constant address");
    return uint32_t(op.getOperand(0u));
  }

  void scanDeclarations() {
    auto [declBegin, declEnd] = ir.getDeclarations();

    for (auto iter = declBegin; iter != declEnd; ++iter) {
      const ir::Op& op = *iter;

      switch (op.getOpCode()) {
        case ir::OpCode::eEntryPoint: {
          entryDef = operandDef(op, 0u);
          stage = ir::ShaderStage(op.getOperand(op.getOperandCount() - 1u));
          if (stage != ir::ShaderStage::eVertex &&
              stage != ir::ShaderStage::ePixel)
            unsupported("shader stage");
        } break;

        case ir::OpCode::eDclInput: {
          InputVar in;
          in.def = op.getDef();
          in.location = uint32_t(op.getOperand(1u));
          in.type = mapType(op.getType(), false);
          inputs.push_back(in);
        } break;

        case ir::OpCode::eDclInputBuiltIn: {
          auto builtin = ir::BuiltIn(op.getOperand(1u));
          if (isStageInBuiltin(builtin)) {
            InputVar in;
            in.def = op.getDef();
            in.isBuiltin = true;
            in.builtin = builtin;
            in.location = 0x10000u + uint32_t(builtin);
            in.type = mapType(op.getType(), false);
            inputs.push_back(in);
          } else {
            BufferVar buf;
            buf.def = op.getDef();
            buf.builtin = builtin;
            buf.type = mapType(op.getType(), true);
            buf.name = builtinBufferName(buf.builtin);
            buffers.push_back(buf);
          }
        } break;

        case ir::OpCode::eDclOutput: {
          OutputVar out;
          out.def = op.getDef();
          out.location = uint32_t(op.getOperand(1u));
          out.orderKey = 0x10000u + out.location;
          out.type = mapType(op.getType(), false);
          outputs.push_back(out);
        } break;

        case ir::OpCode::eDclOutputBuiltIn: {
          OutputVar out;
          out.def = op.getDef();
          out.isBuiltin = true;
          out.builtin = ir::BuiltIn(op.getOperand(1u));
          out.type = mapType(op.getType(), false);
          switch (out.builtin) {
            case ir::BuiltIn::ePosition:     out.orderKey = 0u; break;
            case ir::BuiltIn::ePointSize:    out.orderKey = 1u; break;
            case ir::BuiltIn::eClipDistance: out.orderKey = 2u; break;
            case ir::BuiltIn::eDepth:        out.orderKey = 0x30000u; break;
            default:
              unsupported("output builtin #" +
                std::to_string(uint32_t(out.builtin)));
          }
          outputs.push_back(out);
        } break;

        case ir::OpCode::eDclSpecConstant: {
          SpecVar spec;
          spec.def = op.getDef();
          spec.specId = uint32_t(op.getOperand(1u));
          spec.defaultValue = uint32_t(op.getOperand(2u));
          specs.push_back(spec);
        } break;

        case ir::OpCode::eDclSrv: {
          TextureVar tex;
          tex.def = op.getDef();
          tex.kind = ir::ResourceKind(op.getOperand(4u));
          tex.slot = uint32_t(op.getOperand(2u));
          tex.handleType = textureHandleType(tex.kind, false);
          tex.name = "t" + std::to_string(tex.slot);
          textures.push_back(tex);
        } break;

        case ir::OpCode::eDclSampler: {
          SamplerVar smp;
          smp.def = op.getDef();
          smp.slot = uint32_t(op.getOperand(2u));
          smp.name = "smp" + std::to_string(smp.slot);
          samplers.push_back(smp);
        } break;

        case ir::OpCode::eDclCbv: {
          BufferVar buf;
          buf.def = op.getDef();
          buf.isCbv = true;
          buf.regIndex = uint32_t(op.getOperand(2u));
          buf.type = mapType(op.getType(), true);
          buf.name = declDebugName(op.getDef(),
            "cbv" + std::to_string(buf.regIndex));
          buffers.push_back(buf);
        } break;

        case ir::OpCode::eDclPushData: {
          /* stage mask selects the block: empty = shared (byte 0),
           * single-stage = per-stage block past the shared reservation;
           * must match the frontend's rule or the two blocks alias */
          uint32_t off = uint32_t(op.getOperand(1u));
          if (uint32_t(op.getOperand(2u)) != 0u)
            off += kPushSharedBlockSize;
          Type* t = mapType(op.getType(), true);
          uint32_t bytes = off +
            uint32_t(mod->getDataLayout().getTypeAllocSize(t));
          m_pushBytes = m_pushBytes > bytes ? m_pushBytes : bytes;
          m_pushOffset[op.getDef().getId()] = off;
          if (!m_havePush) { m_havePush = true; m_pushDef = op.getDef(); }
        } break;

        case ir::OpCode::eDclScratch:
          scratchTypes.emplace(op.getDef().getId(), mapType(op.getType(), false));
          break;

        default:
          break;
      }
    }

    std::sort(inputs.begin(), inputs.end(),
      [](const InputVar& a, const InputVar& b) {
        return a.location < b.location;
      });
    std::sort(outputs.begin(), outputs.end(),
      [](const OutputVar& a, const OutputVar& b) {
        return a.orderKey < b.orderKey;
      });

    if (m_havePush) {
      BufferVar buf;
      buf.def = m_pushDef;
      buf.isPushData = true;
      buf.type = ArrayType::get(Type::getInt8Ty(ctx), m_pushBytes);
      buf.name = "pushData";
      buffers.push_back(buf);
    }

    uint32_t nextIndex = 1u;
    bool haveBuiltins = false;
    for (const auto& buf : buffers)
      haveBuiltins |= !buf.isCbv;
    for (auto& buf : buffers) {
      if (buf.isPushData)
        buf.bufferIndex = 20u;
      else if (buf.isCbv)
        buf.bufferIndex = (haveBuiltins ? 8u : 0u) + buf.regIndex;
      else if (buf.builtin == ir::BuiltIn::eLegacyConstFloat)
        buf.bufferIndex = 0u;
      else
        buf.bufferIndex = nextIndex++;
    }

    if (useAB) {
      uint32_t word = 0u;
      for (auto& buf : buffers) {
        if (!buf.isPushData)
          buf.abWord = int32_t(word++);
      }
      for (auto& tex : textures)
        tex.abWord = int32_t(word++);
      for (auto& smp : samplers)
        smp.abWord = int32_t(word++);
      abWordCount = word;
    }
  }

  std::string declDebugName(ir::SsaDef def, const std::string& fallback) {
    auto [a, b] = ir.getUses(def);
    for (auto i = a; i != b; i++) {
      if (i->getOpCode() == ir::OpCode::eDebugName)
        return i->getLiteralString(i->getFirstLiteralOperandIndex());
    }
    return fallback;
  }

  static bool isStageInBuiltin(ir::BuiltIn builtin) {
    return builtin == ir::BuiltIn::ePointCoord ||
           builtin == ir::BuiltIn::eIsFrontFace ||
           builtin == ir::BuiltIn::ePosition;
  }

  static const char* stageInBuiltinMdName(ir::BuiltIn builtin) {
    switch (builtin) {
      case ir::BuiltIn::ePointCoord:  return "air.point_coord";
      case ir::BuiltIn::eIsFrontFace: return "air.front_facing";
      default:
        unsupported("stage-in builtin #" + std::to_string(uint32_t(builtin)));
    }
  }

  static std::string builtinBufferName(ir::BuiltIn builtin) {
    switch (builtin) {
      case ir::BuiltIn::eLegacyConstFloat:        return "cF";
      case ir::BuiltIn::eLegacyConstInt:          return "cI";
      case ir::BuiltIn::eLegacyConstBool:         return "cB";
      case ir::BuiltIn::eLegacyClipPlanes:   return "clipPlanes";
      case ir::BuiltIn::eLegacyPointArgs:    return "pointArgs";
      case ir::BuiltIn::eLegacyAlphaTest:    return "alphaTest";
      case ir::BuiltIn::eLegacyFog:          return "fog";
      case ir::BuiltIn::eLegacySamplerState: return "sampler_state";
      case ir::BuiltIn::eLegacyTextureStage: return "textureStage";
      default:
        unsupported("input builtin #" + std::to_string(uint32_t(builtin)));
    }
  }

  static const char* textureKindSuffix(ir::ResourceKind kind, bool depth) {
    switch (kind) {
      case ir::ResourceKind::eImage2D:   return depth ? "depth_2d"   : "texture_2d";
      case ir::ResourceKind::eImageCube: return depth ? "depth_cube" : "texture_cube";
      case ir::ResourceKind::eImage3D:   return depth ? nullptr      : "texture_3d";
      default:                           return nullptr;
    }
  }

  static uint32_t textureCoordDims(ir::ResourceKind kind) {
    return kind == ir::ResourceKind::eImage2D ? 2u : 3u;
  }

  static bool textureHasOffsetParams(ir::ResourceKind kind) {
    return kind != ir::ResourceKind::eImageCube;
  }

  static const char* textureMslTypeName(ir::ResourceKind kind) {
    switch (kind) {
      case ir::ResourceKind::eImage2D:   return "texture2d<float, sample>";
      case ir::ResourceKind::eImageCube: return "texturecube<float, sample>";
      case ir::ResourceKind::eImage3D:   return "texture3d<float, sample>";
      default:
        unsupported("texture kind #" + std::to_string(uint32_t(kind)));
    }
  }

  static const char* textureManifestKind(ir::ResourceKind kind) {
    switch (kind) {
      case ir::ResourceKind::eImage2D:   return "2d";
      case ir::ResourceKind::eImageCube: return "cube";
      case ir::ResourceKind::eImage3D:   return "3d";
      default:
        unsupported("texture kind #" + std::to_string(uint32_t(kind)));
    }
  }

  StructType* getOpaqueStruct(const std::string& name) {
    if (StructType* existing = StructType::getTypeByName(ctx, name))
      return existing;
    return StructType::create(ctx, name);
  }

  PointerType* textureHandleType(ir::ResourceKind kind, bool depth) {
    const char* suffix = textureKindSuffix(kind, depth);
    if (!suffix)
      unsupported("texture kind #" + std::to_string(uint32_t(kind)));
    return PointerType::get(
      getOpaqueStruct(std::string("struct._") + suffix + "_t"), kAsTexture);
  }

  PointerType* samplerHandleType() {
    return PointerType::get(getOpaqueStruct("struct._sampler_t"), kAsSampler);
  }

  StructType* wrapperStruct(const std::string& name, Type* inner) {
    if (StructType* existing = StructType::getTypeByName(ctx, name))
      return existing;
    return StructType::create(ctx, { inner }, name);
  }

  StructType* textureWrapperType(ir::ResourceKind kind) {
    const char* suffix = textureKindSuffix(kind, false);
    if (!suffix)
      unsupported("texture kind #" + std::to_string(uint32_t(kind)));
    std::string msl = suffix;
    msl.erase(std::remove(msl.begin(), msl.end(), '_'), msl.end());
    return wrapperStruct("struct.metal::" + msl,
      textureHandleType(kind, false));
  }

  StructType* samplerWrapperType() {
    return wrapperStruct("struct.metal::sampler", samplerHandleType());
  }

  StructType* argumentBufferType() {
    if (abStruct)
      return abStruct;
    SmallVector<Type*, 32> members(abWordCount, nullptr);
    for (const auto& buf : buffers) {
      if (buf.abWord >= 0)
        members[buf.abWord] = PointerType::get(buf.type, kAsConstant);
    }
    for (const auto& tex : textures)
      members[tex.abWord] = textureWrapperType(tex.kind);
    for (const auto& smp : samplers)
      members[smp.abWord] = samplerWrapperType();
    abStruct = StructType::create(ctx, members, "struct.d9mt_ab");
    return abStruct;
  }

  const TextureVar* findTexture(ir::SsaDef def) const {
    for (const auto& tex : textures) {
      if (tex.def == def)
        return &tex;
    }
    return nullptr;
  }

  const SamplerVar* findSampler(ir::SsaDef def) const {
    for (const auto& smp : samplers) {
      if (smp.def == def)
        return &smp;
    }
    return nullptr;
  }

  Function* getAirIntrinsic(
          const std::string&  name,
          Type*               retType,
          ArrayRef<Type*>     params) {
    auto it = airIntrinsics.find(name);
    if (it != airIntrinsics.end())
      return it->second;

    Function* fn = Function::Create(
      FunctionType::get(retType, params, false),
      GlobalValue::ExternalLinkage, name, *mod);
    fn->addFnAttr(Attribute::ArgMemOnly);
    fn->addFnAttr(Attribute::Convergent);
    fn->addFnAttr(Attribute::NoFree);
    fn->addFnAttr(Attribute::NoUnwind);
    fn->addFnAttr(Attribute::ReadOnly);
    fn->addFnAttr(Attribute::WillReturn);
    airIntrinsics.emplace(name, fn);
    return fn;
  }

  void createEntryFunction() {
    SmallVector<Type*, 8> params;

    uint32_t argIndex = 0u;
    for (auto& in : inputs) {
      in.argIndex = argIndex++;
      params.push_back(in.type);
    }

    int32_t abArgIndex = -1;
    if (useAB) {
      for (auto& buf : buffers) {
        if (!buf.isPushData)
          continue;
        buf.argIndex = argIndex++;
        params.push_back(PointerType::get(buf.type, kAsConstant));
      }
      if (abWordCount > 0u) {
        abArgIndex = int32_t(argIndex++);
        params.push_back(PointerType::get(argumentBufferType(), kAsConstant));
      }
    } else {
      for (auto& buf : buffers) {
        buf.argIndex = argIndex++;
        params.push_back(PointerType::get(buf.type, kAsConstant));
      }
      for (auto& tex : textures) {
        tex.argIndex = argIndex++;
        params.push_back(tex.handleType);
      }
      for (auto& smp : samplers) {
        smp.argIndex = argIndex++;
        params.push_back(samplerHandleType());
      }
    }

    SmallVector<Type*, 4> retMembers;
    int32_t retIndex = 0;
    for (auto& out : outputs) {
      out.retIndex = retIndex++;
      retMembers.push_back(out.type);
    }

    Type* retType = nullptr;
    if (retMembers.size() == 1u) {
      retType = retMembers[0];
    } else {
      entryRetType = StructType::get(ctx, retMembers,  true);
      retType = entryRetType;
    }

    entryFn = Function::Create(
      FunctionType::get(retType, params, false),
      GlobalValue::ExternalLinkage, options.entryName, *mod);
    addShaderFnAttrs(entryFn);

    if (useAB) {
      for (auto& buf : buffers) {
        if (!buf.isPushData)
          continue;
        entryFn->getArg(buf.argIndex)->addAttr(Attribute::NoCapture);
        entryFn->getArg(buf.argIndex)->addAttr(Attribute::ReadOnly);
      }
      if (abArgIndex >= 0) {
        Argument* ab = entryFn->getArg(uint32_t(abArgIndex));
        ab->addAttr(Attribute::NoCapture);
        ab->addAttr(Attribute::ReadOnly);
        ab->addAttr(Attribute::NoUndef);
        ab->addAttr(Attribute::getWithAlignment(ctx, Align(8u)));
        ab->addAttr(Attribute::getWithDereferenceableBytes(
          ctx, uint64_t(abWordCount) * 8u));
        ab->addAttr(Attribute::get(ctx, "air-buffer-no-alias"));
        entryAbArgIndex = uint32_t(abArgIndex);
      }
    } else {
      for (auto& buf : buffers) {
        entryFn->getArg(buf.argIndex)->addAttr(Attribute::NoCapture);
        entryFn->getArg(buf.argIndex)->addAttr(Attribute::ReadOnly);
      }
      for (auto& tex : textures) {
        entryFn->getArg(tex.argIndex)->addAttr(Attribute::NoCapture);
        entryFn->getArg(tex.argIndex)->addAttr(Attribute::ReadOnly);
      }
      for (auto& smp : samplers) {
        entryFn->getArg(smp.argIndex)->addAttr(Attribute::NoCapture);
        entryFn->getArg(smp.argIndex)->addAttr(Attribute::ReadOnly);
      }
    }

    functions.emplace(entryDef.getId(), entryFn);
  }

  void addShaderFnAttrs(Function* fn) {
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

  const BufferVar* findBuffer(ir::SsaDef def) const {
    for (const auto& buf : buffers) {
      if (buf.def == def)
        return &buf;
    }
    return nullptr;
  }

  const InputVar* findInput(ir::SsaDef def) const {
    for (const auto& in : inputs) {
      if (in.def == def)
        return &in;
    }
    return nullptr;
  }

  Type* hiddenParamType(ir::SsaDef declDef) {
    if (useAB && declDef.getId() == kAbDeclId)
      return PointerType::get(argumentBufferType(), kAsConstant);
    if (const BufferVar* buf = findBuffer(declDef))
      return PointerType::get(buf->type, kAsConstant);
    if (const TextureVar* tex = findTexture(declDef))
      return tex->handleType;
    if (findSampler(declDef))
      return samplerHandleType();
    if (const OutputVar* out = findOutput(declDef))
      return PointerType::get(out->type, 0u);
    if (const InputVar* in = findInput(declDef))
      return in->type;
    unsupported("hidden param for unknown declaration");
  }

  uint32_t hiddenIdForDecl(ir::SsaDef declDef) {
    if (!useAB)
      return declDef.getId();
    if (const BufferVar* buf = findBuffer(declDef)) {
      if (!buf->isPushData)
        return kAbDeclId;
    } else if (findTexture(declDef) || findSampler(declDef)) {
      return kAbDeclId;
    }
    return declDef.getId();
  }

  void computeHiddenBuffers() {
    auto [codeBegin, codeEnd] = ir.getCode();

    bool changed = true;
    while (changed) {
      changed = false;

      ir::SsaDef currentFnDef;
      for (auto iter = codeBegin; iter != codeEnd; ++iter) {
        const ir::Op& op = *iter;

        if (op.getOpCode() == ir::OpCode::eFunction) {
          currentFnDef = op.getDef();
          continue;
        }

        auto addUse = [&](uint32_t bufId) {
          auto& list = fnHiddenBuffers[currentFnDef.getId()];
          if (std::find(list.begin(), list.end(), bufId) == list.end()) {
            list.push_back(bufId);
            changed = true;
          }
        };

        if (op.getOpCode() == ir::OpCode::eInputLoad) {
          if (const BufferVar* buf = findBuffer(operandDef(op, 0u)))
            addUse(hiddenIdForDecl(buf->def));
          else if (const InputVar* in = findInput(operandDef(op, 0u)))
            addUse(in->def.getId());
        } else if (op.getOpCode() == ir::OpCode::eOutputStore) {
          if (findOutput(operandDef(op, 0u)))
            addUse(operandDef(op, 0u).getId());
        } else if (op.getOpCode() == ir::OpCode::eDescriptorLoad) {
          ir::SsaDef decl = operandDef(op, 0u);
          if (findTexture(decl) || findSampler(decl) || findBuffer(decl))
            addUse(hiddenIdForDecl(decl));
        } else if (op.getOpCode() == ir::OpCode::ePushDataLoad) {
          if (m_havePush)
            addUse(m_pushDef.getId());
        } else if (op.getOpCode() == ir::OpCode::eFunctionCall) {
          for (uint32_t bufId : fnHiddenBuffers[operandDef(op, 0u).getId()])
            addUse(bufId);
        }
      }
    }
  }

  void createFunctions() {
    computeHiddenBuffers();

    auto [codeBegin, codeEnd] = ir.getCode();

    for (auto iter = codeBegin; iter != codeEnd; ++iter) {
      const ir::Op& op = *iter;
      if (op.getOpCode() != ir::OpCode::eFunction)
        continue;

      if (op.getDef() == entryDef) {
        createEntryFunction();
        auto& argMap = fnBufferArgs[entryDef.getId()];
        if (useAB) {
          for (const auto& buf : buffers) {
            if (buf.isPushData)
              argMap.emplace(buf.def.getId(), buf.argIndex);
          }
          if (abWordCount > 0u)
            argMap.emplace(kAbDeclId, entryAbArgIndex);
        } else {
          for (const auto& buf : buffers)
            argMap.emplace(buf.def.getId(), buf.argIndex);
          for (const auto& tex : textures)
            argMap.emplace(tex.def.getId(), tex.argIndex);
          for (const auto& smp : samplers)
            argMap.emplace(smp.def.getId(), smp.argIndex);
        }
        for (const auto& in : inputs)
          argMap.emplace(in.def.getId(), in.argIndex);
        continue;
      }

      SmallVector<Type*, 4> params;
      for (uint32_t i = 0u; i < op.getOperandCount(); i++) {
        const ir::Op& param = ir.getOpForOperand(op, i);
        params.push_back(mapType(param.getType(), false));
      }

      auto& argMap = fnBufferArgs[op.getDef().getId()];
      for (uint32_t declId : fnHiddenBuffers[op.getDef().getId()]) {
        argMap.emplace(declId, uint32_t(params.size()));
        params.push_back(hiddenParamType(ir::SsaDef(declId)));
      }

      Function* fn = Function::Create(
        FunctionType::get(mapType(op.getType(), false), params, false),
        GlobalValue::InternalLinkage,
        "fn" + std::to_string(functions.size()), *mod);
      addShaderFnAttrs(fn);
      functions.emplace(op.getDef().getId(), fn);
    }
  }

  Function* currentFn = nullptr;
  const ir::Op* currentFnOp = nullptr;
  bool isEntry = false;

  void emitBodies() {
    auto [codeBegin, codeEnd] = ir.getCode();

    for (auto iter = codeBegin; iter != codeEnd; ++iter) {
      const ir::Op& op = *iter;
      if (op.getOpCode() != ir::OpCode::eFunction)
        continue;

      currentFnOp = &op;
      currentFn = functions.at(op.getDef().getId());
      isEntry = op.getDef() == entryDef;

      blocks.clear();
      bool first = true;
      for (auto scan = iter; scan != codeEnd; ++scan) {
        if (scan->getOpCode() == ir::OpCode::eFunctionEnd)
          break;
        if (scan->getOpCode() == ir::OpCode::eLabel) {
          blocks.emplace(scan->getDef().getId(),
            BasicBlock::Create(ctx, first ? "entry" : "", currentFn));
          first = false;
        }
      }

      bool entrySetupDone = false;
      for (++iter; iter != codeEnd; ++iter) {
        const ir::Op& code = *iter;
        if (code.getOpCode() == ir::OpCode::eFunctionEnd)
          break;

        if (code.getOpCode() == ir::OpCode::eLabel) {
          b->SetInsertPoint(blocks.at(code.getDef().getId()));
          if (isEntry && !entrySetupDone) {
            emitEntryPrologue();
            entrySetupDone = true;
          }
          continue;
        }

        emitInstruction(code);
      }

      resolvePendingPhis();
    }
  }

  void emitEntryPrologue() {
    for (auto& out : outputs) {
      out.storage = b->CreateAlloca(out.type);
      b->CreateStore(Constant::getNullValue(out.type), out.storage);
    }
  }

  OutputVar* findOutput(ir::SsaDef def) {
    for (auto& out : outputs) {
      if (out.def == def)
        return &out;
    }
    return nullptr;
  }

  void emitInstruction(const ir::Op& op) {
    switch (op.getOpCode()) {
      case ir::OpCode::eInputLoad:    return emitInputLoad(op);
      case ir::OpCode::eOutputStore:  return emitOutputStore(op);
      case ir::OpCode::eParamLoad:    return emitParamLoad(op);
      case ir::OpCode::eFunctionCall: return emitFunctionCall(op);
      case ir::OpCode::eReturn:       return emitReturn(op);
      case ir::OpCode::eBranch:
        b->CreateBr(blocks.at(operandDef(op, 0u).getId()));
        return;
      case ir::OpCode::eBranchConditional:
        b->CreateCondBr(getValue(operandDef(op, 0u)),
          blocks.at(operandDef(op, 1u).getId()),
          blocks.at(operandDef(op, 2u).getId()));
        return;
      case ir::OpCode::eSwitch:       return emitSwitch(op);
      case ir::OpCode::eUnreachable:
        b->CreateUnreachable();
        return;
      case ir::OpCode::eDemote:
        b->CreateCall(getDiscardFn());
        return;
      case ir::OpCode::ePhi:          return emitPhi(op);

      case ir::OpCode::eCompositeExtract:   return emitCompositeExtract(op);
      case ir::OpCode::eCompositeConstruct: return emitCompositeConstruct(op);

      case ir::OpCode::eSelect:
        setValue(op, b->CreateSelect(
          getValue(operandDef(op, 0u)),
          getValue(operandDef(op, 1u)),
          getValue(operandDef(op, 2u))));
        return;

      case ir::OpCode::eFAdd: return emitFBin(op, Instruction::FAdd);
      case ir::OpCode::eFSub: return emitFBin(op, Instruction::FSub);
      case ir::OpCode::eFMul: return emitFBin(op, Instruction::FMul);
      case ir::OpCode::eFDiv: return emitFBin(op, Instruction::FDiv);
      case ir::OpCode::eFMad:
        setValue(op, b->CreateIntrinsic(Intrinsic::fma,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)),
            getValue(operandDef(op, 1u)),
            getValue(operandDef(op, 2u)) }));
        return;
      case ir::OpCode::eFMin:
        setValue(op, b->CreateIntrinsic(Intrinsic::minnum,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u)) }));
        return;
      case ir::OpCode::eFMax:
        setValue(op, b->CreateIntrinsic(Intrinsic::maxnum,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u)) }));
        return;
      case ir::OpCode::eFNeg:
        setValue(op, b->CreateFNeg(getValue(operandDef(op, 0u))));
        return;
      case ir::OpCode::eFAbs:
        setValue(op, b->CreateIntrinsic(Intrinsic::fabs,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)) }));
        return;
      case ir::OpCode::eFSqrt:
        setValue(op, b->CreateIntrinsic(Intrinsic::sqrt,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)) }));
        return;
      case ir::OpCode::eFRcp:
        setValue(op, b->CreateFDiv(
          ConstantFP::get(mapType(op.getType(), false), 1.0),
          getValue(operandDef(op, 0u))));
        return;
      case ir::OpCode::eFClamp:
        setValue(op, b->CreateIntrinsic(Intrinsic::minnum,
          { mapType(op.getType(), false) },
          { b->CreateIntrinsic(Intrinsic::maxnum,
              { mapType(op.getType(), false) },
              { getValue(operandDef(op, 0u)),
                getValue(operandDef(op, 1u)) }),
            getValue(operandDef(op, 2u)) }));
        return;

      case ir::OpCode::eFSin:   return emitAirMathUnary(op, "air.fast_sin");
      case ir::OpCode::eFCos:   return emitAirMathUnary(op, "air.fast_cos");
      case ir::OpCode::eFExp2:  return emitAirMathUnary(op, "air.fast_exp2");
      case ir::OpCode::eFLog2:  return emitAirMathUnary(op, "air.fast_log2");
      case ir::OpCode::eFRsq:   return emitAirMathUnary(op, "air.fast_rsqrt");
      case ir::OpCode::eFFract: return emitAirMathUnary(op, "air.fast_fract");
      case ir::OpCode::eFSgn:   return emitAirMathUnary(op, "air.sign");
      case ir::OpCode::eFPow:   return emitAirMathBinary(op, "air.fast_pow");

      case ir::OpCode::eFIsNan:
        setValue(op, b->CreateFCmpUNO(
          getValue(operandDef(op, 0u)), getValue(operandDef(op, 0u))));
        return;

      case ir::OpCode::eSMax: return emitAirIntBinary(op, "air.max.s.i32");
      case ir::OpCode::eSMin: return emitAirIntBinary(op, "air.min.s.i32");
      case ir::OpCode::eUMax: return emitAirIntBinary(op, "air.max.u.i32");
      case ir::OpCode::eUMin: return emitAirIntBinary(op, "air.min.u.i32");
      case ir::OpCode::eIAbs: {
        Type* type = requireScalarI32(op);
        setValue(op, b->CreateCall(
          getAirMathFn("air.abs.s.i32", type, { type }),
          { getValue(operandDef(op, 0u)) }));
        return;
      }
      case ir::OpCode::eSClamp: return emitIntClamp(op, true);
      case ir::OpCode::eUClamp: return emitIntClamp(op, false);

      case ir::OpCode::eUDiv:
        setValue(op, b->CreateUDiv(
          getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u))));
        return;
      case ir::OpCode::eUMod:
        setValue(op, b->CreateURem(
          getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u))));
        return;

      case ir::OpCode::eConvertFtoF: {
        Value* v = getValue(operandDef(op, 0u));
        Type* dst = mapType(op.getType(), false);
        bool narrowing = dst->getScalarSizeInBits() <
          v->getType()->getScalarSizeInBits();
        setValue(op, narrowing ? b->CreateFPTrunc(v, dst)
                               : b->CreateFPExt(v, dst));
        return;
      }
      case ir::OpCode::eConvertItoI: {
        const ir::Op& src = this->ir.getOpForOperand(op, 0u);
        bool srcSigned =
          src.getType().getBaseType(0u).getBaseType() == ir::ScalarType::eI16 ||
          src.getType().getBaseType(0u).getBaseType() == ir::ScalarType::eI32;
        Value* v = getValue(operandDef(op, 0u));
        Type* dst = mapType(op.getType(), false);
        setValue(op, b->CreateIntCast(v, dst, srcSigned));
        return;
      }
      case ir::OpCode::eCast:
      case ir::OpCode::eConsumeAs:
        setValue(op, b->CreateBitCast(
          getValue(operandDef(op, 0u)), mapType(op.getType(), false)));
        return;

      case ir::OpCode::eScratchLoad:  return emitScratchLoad(op);
      case ir::OpCode::eScratchStore: return emitScratchStore(op);

      case ir::OpCode::eBufferLoad:   return emitBufferLoad(op);
      case ir::OpCode::ePushDataLoad:  return emitPushDataLoad(op);
      case ir::OpCode::eDclPushData:   return;

      case ir::OpCode::eDescriptorLoad:  return emitDescriptorLoad(op);
      case ir::OpCode::eImageSample:     return emitImageSample(op);
      case ir::OpCode::eImageGather:     return emitImageGather(op);
      case ir::OpCode::eImageQuerySize:  return emitImageQuerySize(op);

      case ir::OpCode::eDerivX:
      case ir::OpCode::eDerivY:          return emitDeriv(op);

      case ir::OpCode::eFRound: {
        auto mode = ir::RoundMode(uint32_t(op.getOperand(1u)));
        Intrinsic::ID id;
        switch (mode) {
          case ir::RoundMode::eZero:        id = Intrinsic::trunc; break;
          case ir::RoundMode::eNearestEven: id = Intrinsic::rint;  break;
          case ir::RoundMode::eNegativeInf: id = Intrinsic::floor; break;
          case ir::RoundMode::ePositiveInf: id = Intrinsic::ceil;  break;
          default: unsupported("round mode");
        }
        setValue(op, b->CreateIntrinsic(id,
          { mapType(op.getType(), false) },
          { getValue(operandDef(op, 0u)) }));
        return;
      }

      case ir::OpCode::eFEq: return emitFCmp(op, CmpInst::FCMP_OEQ);
      case ir::OpCode::eFNe: return emitFCmp(op, CmpInst::FCMP_UNE);
      case ir::OpCode::eFLt: return emitFCmp(op, CmpInst::FCMP_OLT);
      case ir::OpCode::eFLe: return emitFCmp(op, CmpInst::FCMP_OLE);
      case ir::OpCode::eFGt: return emitFCmp(op, CmpInst::FCMP_OGT);
      case ir::OpCode::eFGe: return emitFCmp(op, CmpInst::FCMP_OGE);

      case ir::OpCode::eIAdd: return emitIBin(op, Instruction::Add);
      case ir::OpCode::eISub: return emitIBin(op, Instruction::Sub);
      case ir::OpCode::eIMul: return emitIBin(op, Instruction::Mul);
      case ir::OpCode::eIAnd: return emitIBin(op, Instruction::And);
      case ir::OpCode::eIOr:  return emitIBin(op, Instruction::Or);
      case ir::OpCode::eIXor: return emitIBin(op, Instruction::Xor);
      case ir::OpCode::eIShl: return emitIBin(op, Instruction::Shl);
      case ir::OpCode::eUShr: return emitIBin(op, Instruction::LShr);
      case ir::OpCode::eSShr: return emitIBin(op, Instruction::AShr);
      case ir::OpCode::eUBitExtract: return emitBitExtract(op, false);
      case ir::OpCode::eSBitExtract: return emitBitExtract(op, true);
      case ir::OpCode::eIBitInsert:  return emitBitInsert(op);
      case ir::OpCode::eINeg:
        setValue(op, b->CreateNeg(getValue(operandDef(op, 0u))));
        return;
      case ir::OpCode::eINot:
        setValue(op, b->CreateNot(getValue(operandDef(op, 0u))));
        return;

      case ir::OpCode::eIEq: return emitICmp(op, CmpInst::ICMP_EQ);
      case ir::OpCode::eINe: return emitICmp(op, CmpInst::ICMP_NE);
      case ir::OpCode::eULt: return emitICmp(op, CmpInst::ICMP_ULT);
      case ir::OpCode::eULe: return emitICmp(op, CmpInst::ICMP_ULE);
      case ir::OpCode::eUGt: return emitICmp(op, CmpInst::ICMP_UGT);
      case ir::OpCode::eUGe: return emitICmp(op, CmpInst::ICMP_UGE);
      case ir::OpCode::eSLt: return emitICmp(op, CmpInst::ICMP_SLT);
      case ir::OpCode::eSLe: return emitICmp(op, CmpInst::ICMP_SLE);
      case ir::OpCode::eSGt: return emitICmp(op, CmpInst::ICMP_SGT);
      case ir::OpCode::eSGe: return emitICmp(op, CmpInst::ICMP_SGE);

      case ir::OpCode::eBAnd: return emitIBin(op, Instruction::And);
      case ir::OpCode::eBOr:  return emitIBin(op, Instruction::Or);
      case ir::OpCode::eBNot:
        setValue(op, b->CreateNot(getValue(operandDef(op, 0u))));
        return;

      case ir::OpCode::eConvertItoF: {
        const ir::Op& src = this->ir.getOpForOperand(op, 0u);
        bool isSigned =
          src.getType().getBaseType(0u).getBaseType() == ir::ScalarType::eI32;
        Value* v = getValue(operandDef(op, 0u));
        Type* dst = mapType(op.getType(), false);
        setValue(op, isSigned ? b->CreateSIToFP(v, dst)
                              : b->CreateUIToFP(v, dst));
        return;
      }
      case ir::OpCode::eConvertFtoI: {
        bool isSigned =
          op.getType().getBaseType(0u).getBaseType() == ir::ScalarType::eI32;
        Value* v = getValue(operandDef(op, 0u));
        Type* dst = mapType(op.getType(), false);
        setValue(op, isSigned ? b->CreateFPToSI(v, dst)
                              : b->CreateFPToUI(v, dst));
        return;
      }

      case ir::OpCode::eConstant:
      case ir::OpCode::eUndef:
        return;

      default:
        unsupported(opCodeName(op.getOpCode()));
    }
  }

  void emitFBin(const ir::Op& op, Instruction::BinaryOps kind) {
    Value* v = b->CreateBinOp(kind,
      getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u)));
    setValue(op, v);
  }

  void emitIBin(const ir::Op& op, Instruction::BinaryOps kind) {
    setValue(op, b->CreateBinOp(kind,
      getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u))));
  }

  void emitBitExtract(const ir::Op& op, bool sign) {
    Value* base   = getValue(operandDef(op, 0u));
    Value* offset = getValue(operandDef(op, 1u));
    Value* count  = getValue(operandDef(op, 2u));
    Type*  ty     = base->getType();
    Value* width  = ConstantInt::get(ty, ty->getIntegerBitWidth());
    Value* zero   = ConstantInt::get(ty, 0u);

    Value* left  = b->CreateSub(width, b->CreateAdd(offset, count));
    Value* shl   = b->CreateShl(base, left);
    Value* right = b->CreateSub(width, count);
    Value* res   = sign ? b->CreateAShr(shl, right) : b->CreateLShr(shl, right);
    /* count == 0 must map to 0: shift-by-width is undefined in LLVM */
    res = b->CreateSelect(b->CreateICmpEQ(count, zero), zero, res);
    setValue(op, res);
  }

  void emitBitInsert(const ir::Op& op) {
    Value* base   = getValue(operandDef(op, 0u));
    Value* insert = getValue(operandDef(op, 1u));
    Value* offset = getValue(operandDef(op, 2u));
    Value* count  = getValue(operandDef(op, 3u));
    Type*  ty     = base->getType();
    Value* one    = ConstantInt::get(ty, 1u);

    Value* countMask = b->CreateSub(b->CreateShl(one, count), one);
    Value* mask      = b->CreateShl(countMask, offset);
    Value* keep      = b->CreateAnd(base, b->CreateNot(mask));
    Value* ins       = b->CreateAnd(b->CreateShl(insert, offset), mask);
    Value* res       = b->CreateOr(keep, ins);
    res = b->CreateSelect(
      b->CreateICmpEQ(count, ConstantInt::get(ty, 0u)), base, res);
    setValue(op, res);
  }

  void emitFCmp(const ir::Op& op, CmpInst::Predicate pred) {
    setValue(op, b->CreateFCmp(pred,
      getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u))));
  }

  void emitICmp(const ir::Op& op, CmpInst::Predicate pred) {
    setValue(op, b->CreateICmp(pred,
      getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u))));
  }

  Function* getDiscardFn() {
    if (!discardFn) {
      discardFn = Function::Create(
        FunctionType::get(Type::getVoidTy(ctx), false),
        GlobalValue::ExternalLinkage, "air.discard_fragment", *mod);
      discardFn->addFnAttr(Attribute::NoUnwind);
      discardFn->addFnAttr(Attribute::WillReturn);
      discardFn->addFnAttr(Attribute::MustProgress);
    }
    return discardFn;
  }

  void appendAddressIndices(ir::SsaDef addrDef, SmallVectorImpl<Value*>& idx) {
    const ir::Op& addrOp = this->ir.getOp(addrDef);
    ir::BasicType bt = addrOp.getType().getBaseType(0u);

    if (addrOp.getOpCode() == ir::OpCode::eConstant) {
      for (uint32_t i = 0u; i < bt.getVectorSize(); i++)
        idx.push_back(b->getInt32(uint32_t(addrOp.getOperand(i))));
      return;
    }

    Value* v = getValue(addrDef);
    if (!bt.isVector()) {
      idx.push_back(v);
      return;
    }
    for (uint32_t i = 0u; i < bt.getVectorSize(); i++)
      idx.push_back(b->CreateExtractElement(v, uint64_t(i)));
  }

  void emitInputLoad(const ir::Op& op) {
    ir::SsaDef decl = operandDef(op, 0u);
    ir::SsaDef addrDef = operandDef(op, 1u);

    for (const auto& in : inputs) {
      if (in.def != decl)
        continue;
      const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
      Value* param = currentFn->getArg(argMap.at(in.def.getId()));
      setValue(op, !addrDef
        ? param
        : b->CreateExtractElement(param,
            uint64_t(constantAddress(addrDef))));
      return;
    }

    for (const auto& buf : buffers) {
      if (buf.def != decl)
        continue;
      Value* base = nullptr;
      if (useAB && !buf.isPushData) {
        base = loadAbHandle(buf.abWord, false);
      } else {
        const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
        base = currentFn->getArg(argMap.at(buf.def.getId()));
      }
      Value* v = nullptr;

      if (!addrDef) {
        v = b->CreateLoad(buf.type, base);
      } else {
        SmallVector<Value*, 4> idx;
        idx.push_back(b->getInt64(0));
        appendAddressIndices(addrDef, idx);
        Value* ptr = b->CreateInBoundsGEP(buf.type, base, idx);
        Type* elem =
          cast<GEPOperator>(ptr)->getResultElementType();
        v = b->CreateLoad(elem, ptr);
      }

      const ir::Type& resType = op.getType();
      if (resType.isScalarType() &&
          resType.getBaseType(0u).getBaseType() == ir::ScalarType::eBool)
        v = b->CreateICmpNE(v, b->getInt32(0));

      setValue(op, v);
      return;
    }

    unsupported("InputLoad from unknown declaration");
  }

  void emitOutputStore(const ir::Op& op) {
    OutputVar* out = findOutput(operandDef(op, 0u));
    if (!out)
      unsupported("OutputStore to unknown declaration");

    Value* target = out->storage;
    if (!isEntry) {
      const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
      target = currentFn->getArg(argMap.at(out->def.getId()));
    }

    ir::SsaDef addrDef = operandDef(op, 1u);
    Value* value = getValue(operandDef(op, 2u));

    if (!addrDef) {
      b->CreateStore(value, target);
    } else if (out->type->isArrayTy()) {
      SmallVector<Value*, 2> idx;
      idx.push_back(b->getInt64(0));
      appendAddressIndices(addrDef, idx);
      b->CreateStore(value, b->CreateInBoundsGEP(out->type, target, idx));
    } else {
      uint32_t address = constantAddress(addrDef);
      Value* current = b->CreateLoad(out->type, target);
      Value* updated = address != ~0u
        ? b->CreateInsertElement(current, value, uint64_t(address))
        : b->CreateInsertElement(current, value, getValue(addrDef));
      b->CreateStore(updated, target);
    }
  }

  void emitParamLoad(const ir::Op& op) {
    ir::SsaDef fnDef = operandDef(op, 0u);
    ir::SsaDef paramDef = operandDef(op, 1u);

    const ir::Op& fnOp = this->ir.getOp(fnDef);
    for (uint32_t i = 0u; i < fnOp.getOperandCount(); i++) {
      if (ir::SsaDef(fnOp.getOperand(i)) == paramDef) {
        setValue(op, functions.at(fnDef.getId())->getArg(i));
        return;
      }
    }
    unsupported("ParamLoad of unknown parameter");
  }

  void emitFunctionCall(const ir::Op& op) {
    ir::SsaDef calleeDef = operandDef(op, 0u);
    Function* callee = functions.at(calleeDef.getId());

    SmallVector<Value*, 4> args;
    for (uint32_t i = 1u; i < op.getOperandCount(); i++)
      args.push_back(getValue(operandDef(op, i)));

    const auto& ownArgs = fnBufferArgs.at(currentFnOp->getDef().getId());
    for (uint32_t declId : fnHiddenBuffers[calleeDef.getId()]) {
      auto argIt = ownArgs.find(declId);
      if (argIt != ownArgs.end()) {
        args.push_back(currentFn->getArg(argIt->second));
      } else if (OutputVar* out = findOutput(ir::SsaDef(declId));
                 out && isEntry) {
        args.push_back(out->storage);
      } else {
        unsupported("hidden param forwarding for decl #" +
          std::to_string(declId));
      }
    }

    Value* v = b->CreateCall(callee, args);
    if (!op.getType().isVoidType())
      setValue(op, v);
  }

  void emitReturn(const ir::Op& op) {
    if (!isEntry) {
      if (op.getOperandCount())
        b->CreateRet(getValue(operandDef(op, 0u)));
      else
        b->CreateRetVoid();
      return;
    }

    SmallVector<Value*, 4> loaded;
    for (auto& out : outputs)
      loaded.push_back(b->CreateLoad(out.type, out.storage));

    if (loaded.size() == 1u) {
      b->CreateRet(loaded[0]);
      return;
    }

    Value* ret = UndefValue::get(entryRetType);
    for (uint32_t i = 0u; i < loaded.size(); i++)
      ret = b->CreateInsertValue(ret, loaded[i], { i });
    b->CreateRet(ret);
  }

  void emitSwitch(const ir::Op& op) {
    Value* selector = getValue(operandDef(op, 0u));
    BasicBlock* defaultBlock = blocks.at(operandDef(op, 1u).getId());

    uint32_t caseCount = (op.getOperandCount() - 2u) / 2u;
    SwitchInst* sw = b->CreateSwitch(selector, defaultBlock, caseCount);

    for (uint32_t i = 0u; i < caseCount; i++) {
      uint32_t value = constantAddress(operandDef(op, 2u + 2u * i));
      BasicBlock* block = blocks.at(operandDef(op, 3u + 2u * i).getId());
      sw->addCase(b->getInt32(value), block);
    }
  }

  void emitPhi(const ir::Op& op) {
    PHINode* phi = b->CreatePHI(
      mapType(op.getType(), false), op.getOperandCount() / 2u);
    setValue(op, phi);
    pendingPhis.emplace_back(phi, &op);
  }

  void resolvePendingPhis() {
    for (auto& [phi, op] : pendingPhis) {
      for (uint32_t i = 0u; i + 1u < op->getOperandCount(); i += 2u) {
        phi->addIncoming(
          getValue(operandDef(*op, i + 1u)),
          blocks.at(operandDef(*op, i).getId()));
      }
    }
    pendingPhis.clear();
  }

  void emitCompositeExtract(const ir::Op& op) {
    Value* composite = getValue(operandDef(op, 0u));
    uint32_t index = constantAddress(operandDef(op, 1u));

    setValue(op, composite->getType()->isVectorTy()
      ? b->CreateExtractElement(composite, uint64_t(index))
      : b->CreateExtractValue(composite, { index }));
  }

  void emitCompositeConstruct(const ir::Op& op) {
    Type* type = mapType(op.getType(), false);
    Value* v = UndefValue::get(type);

    for (uint32_t i = 0u; i < op.getOperandCount(); i++) {
      Value* elem = getValue(operandDef(op, i));
      v = type->isVectorTy()
        ? b->CreateInsertElement(v, elem, uint64_t(i))
        : b->CreateInsertValue(v, elem, { i });
    }
    setValue(op, v);
  }

  Function* getAirMathFn(
          const std::string&  name,
          Type*               retType,
          ArrayRef<Type*>     params) {
    auto it = airIntrinsics.find(name);
    if (it != airIntrinsics.end())
      return it->second;

    Function* fn = Function::Create(
      FunctionType::get(retType, params, false),
      GlobalValue::ExternalLinkage, name, *mod);
    fn->addFnAttr(Attribute::MustProgress);
    fn->addFnAttr(Attribute::NoFree);
    fn->addFnAttr(Attribute::NoUnwind);
    fn->addFnAttr(Attribute::ReadNone);
    fn->addFnAttr(Attribute::WillReturn);
    airIntrinsics.emplace(name, fn);
    return fn;
  }

  /* half operands compute in float and truncate the result: there is no
   * guaranteed half variant of these fast intrinsics, and getAirMathFn
   * caches by name, so a half call would collide with the cached f32 decl */
  Value* callScalarMath(const std::string& base, Type* scalarTy,
                        std::initializer_list<Value*> args) {
    const bool isHalf = scalarTy->isHalfTy();
    Type* calcTy = isHalf ? Type::getFloatTy(ctx) : scalarTy;
    std::vector<Value*> callArgs;
    for (Value* v : args)
      callArgs.push_back(isHalf ? b->CreateFPExt(v, calcTy) : v);
    CallInst* call = b->CreateCall(
      getAirMathFn(base + ".f32", calcTy,
        std::vector<Type*>(callArgs.size(), calcTy)),
      callArgs);
    call->setFastMathFlags(FastMathFlags::getFast());
    return isHalf ? b->CreateFPTrunc(call, scalarTy) : static_cast<Value*>(call);
  }

  void emitAirMathUnary(const ir::Op& op, const std::string& base) {
    Type* type = mapType(op.getType(), false);
    Value* a = getValue(operandDef(op, 0u));
    if (auto* vec = dyn_cast<FixedVectorType>(type)) {
      Type* sc = vec->getElementType();
      Value* res = UndefValue::get(vec);
      for (unsigned i = 0u; i < vec->getNumElements(); i++)
        res = b->CreateInsertElement(res,
          callScalarMath(base, sc, { b->CreateExtractElement(a, i) }), i);
      setValue(op, res);
      return;
    }
    setValue(op, callScalarMath(base, type, { a }));
  }

  void emitAirMathBinary(const ir::Op& op, const std::string& base) {
    Type* type = mapType(op.getType(), false);
    Value* a = getValue(operandDef(op, 0u));
    Value* c = getValue(operandDef(op, 1u));
    if (auto* vec = dyn_cast<FixedVectorType>(type)) {
      Type* sc = vec->getElementType();
      Value* res = UndefValue::get(vec);
      for (unsigned i = 0u; i < vec->getNumElements(); i++)
        res = b->CreateInsertElement(res,
          callScalarMath(base, sc,
            { b->CreateExtractElement(a, i), b->CreateExtractElement(c, i) }), i);
      setValue(op, res);
      return;
    }
    setValue(op, callScalarMath(base, type, { a, c }));
  }

  Type* requireScalarI32(const ir::Op& op) {
    Type* type = mapType(op.getType(), false);
    if (type->isVectorTy() || !type->isIntegerTy(32u))
      unsupported("non-scalar-i32 integer op (expected scalarized IR)");
    return type;
  }

  void emitAirIntBinary(const ir::Op& op, const std::string& name) {
    Type* type = requireScalarI32(op);
    setValue(op, b->CreateCall(
      getAirMathFn(name, type, { type, type }),
      { getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u)) }));
  }

  void emitIntClamp(const ir::Op& op, bool isSigned) {
    Type* type = requireScalarI32(op);
    const char* maxName = isSigned ? "air.max.s.i32" : "air.max.u.i32";
    const char* minName = isSigned ? "air.min.s.i32" : "air.min.u.i32";
    Value* lo = b->CreateCall(getAirMathFn(maxName, type, { type, type }),
      { getValue(operandDef(op, 0u)), getValue(operandDef(op, 1u)) });
    setValue(op, b->CreateCall(getAirMathFn(minName, type, { type, type }),
      { lo, getValue(operandDef(op, 2u)) }));
  }

  AllocaInst* scratchAlloca(ir::SsaDef decl) {
    uint64_t key = (uint64_t(currentFnOp->getDef().getId()) << 32) |
      decl.getId();
    auto it = scratchAllocas.find(key);
    if (it != scratchAllocas.end())
      return it->second;

    Type* type = scratchTypes.at(decl.getId());
    BasicBlock& entry = currentFn->getEntryBlock();
    IRBuilder<> entryBuilder(&entry, entry.begin());
    AllocaInst* alloca = entryBuilder.CreateAlloca(type);
    scratchAllocas.emplace(key, alloca);
    return alloca;
  }

  void emitScratchLoad(const ir::Op& op) {
    ir::SsaDef decl = operandDef(op, 0u);
    AllocaInst* base = scratchAlloca(decl);

    SmallVector<Value*, 4> idx;
    idx.push_back(b->getInt32(0));
    appendAddressIndices(operandDef(op, 1u), idx);

    Value* ptr = b->CreateInBoundsGEP(base->getAllocatedType(), base, idx);
    setValue(op, b->CreateLoad(
      cast<GEPOperator>(ptr)->getResultElementType(), ptr));
  }

  void emitScratchStore(const ir::Op& op) {
    ir::SsaDef decl = operandDef(op, 0u);
    AllocaInst* base = scratchAlloca(decl);

    SmallVector<Value*, 4> idx;
    idx.push_back(b->getInt32(0));
    appendAddressIndices(operandDef(op, 1u), idx);

    Value* ptr = b->CreateInBoundsGEP(base->getAllocatedType(), base, idx);
    b->CreateStore(getValue(operandDef(op, 2u)), ptr);
  }

  void emitPushDataLoad(const ir::Op& op) {
    ir::SsaDef declDef = operandDef(op, 0u);
    uint32_t off = m_pushOffset.at(declDef.getId());

    const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
    auto it = argMap.find(m_pushDef.getId());
    if (it == argMap.end())
      unsupported("PushDataLoad outside push arg map");

    Type* i8ty = Type::getInt8Ty(ctx);
    Value* base = currentFn->getArg(it->second);
    Value* i8base = b->CreateBitCast(base, PointerType::get(i8ty, kAsConstant));

    /* resolve via ir::Type::byteOffset, not member-index * load-size: the
     * push layout is alignment-aware and heterogeneous, so scaling by the
     * load size is wrong for any member past the first */
    Type* loadTy = mapType(op.getType(), false);
    ir::SsaDef addrDef = operandDef(op, 1u);
    if (addrDef) {
      const ir::Op& addrOp = this->ir.getOp(addrDef);
      if (addrOp.getOpCode() != ir::OpCode::eConstant)
        unsupported("non-constant push data address");
      ir::Type curType = this->ir.getOp(declDef).getType();
      for (uint32_t i = 0u; i < addrOp.getOperandCount(); i++) {
        uint32_t index = uint32_t(addrOp.getOperand(i));
        off += curType.byteOffset(index);
        curType = curType.getSubType(index);
      }
    }
    Value* elem = b->CreateInBoundsGEP(i8ty, i8base, b->getInt64(off));

    /* largest power of two dividing the byte offset (capped at 4) is a
     * safe load alignment; sub-word members must not claim align 4 */
    uint64_t align = uint64_t(off | 4u) & -uint64_t(off | 4u);
    Value* typed = b->CreateBitCast(elem, PointerType::get(loadTy, kAsConstant));
    Value* v = b->CreateAlignedLoad(loadTy, typed, MaybeAlign(align));

    const ir::Type& resType = op.getType();
    if (resType.isScalarType() &&
        resType.getBaseType(0u).getBaseType() == ir::ScalarType::eBool)
      v = b->CreateICmpNE(v, b->getInt32(0));

    setValue(op, v);
  }

  void emitBufferLoad(const ir::Op& op) {
    const ir::Op& descriptor = this->ir.getOpForOperand(op, 0u);
    if (descriptor.getOpCode() != ir::OpCode::eDescriptorLoad)
      unsupported("BufferLoad without DescriptorLoad");
    const BufferVar* buf = findBuffer(operandDef(descriptor, 0u));
    if (!buf)
      unsupported("BufferLoad of unknown constant buffer");

    Value* base = getValue(descriptor.getDef());
    ir::SsaDef addrDef = operandDef(op, 1u);
    Value* v = nullptr;

    if (!addrDef) {
      v = b->CreateLoad(buf->type, base);
    } else {
      SmallVector<Value*, 4> idx;
      idx.push_back(b->getInt64(0));
      appendAddressIndices(addrDef, idx);
      Value* ptr = b->CreateInBoundsGEP(buf->type, base, idx);
      v = b->CreateLoad(cast<GEPOperator>(ptr)->getResultElementType(), ptr);
    }

    const ir::Type& resType = op.getType();
    if (resType.isScalarType() &&
        resType.getBaseType(0u).getBaseType() == ir::ScalarType::eBool)
      v = b->CreateICmpNE(v, b->getInt32(0));

    setValue(op, v);
  }

  const TextureVar& textureForDescriptor(ir::SsaDef descriptorLoadDef) {
    const ir::Op& load = this->ir.getOp(descriptorLoadDef);
    if (load.getOpCode() != ir::OpCode::eDescriptorLoad)
      unsupported("image access without DescriptorLoad");
    const TextureVar* tex = findTexture(operandDef(load, 0u));
    if (!tex)
      unsupported("DescriptorLoad of unknown texture");
    return *tex;
  }

  Value* abBasePointer() {
    const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
    auto it = argMap.find(kAbDeclId);
    if (it == argMap.end())
      unsupported("argument-buffer access outside AB arg map");
    return currentFn->getArg(it->second);
  }

  Value* loadAbHandle(int32_t word, bool wrapped) {
    Value* base = abBasePointer();
    SmallVector<Value*, 3> idx;
    idx.push_back(b->getInt64(0));
    idx.push_back(b->getInt32(uint32_t(word)));
    if (wrapped)
      idx.push_back(b->getInt32(0u));
    Value* ptr = b->CreateInBoundsGEP(argumentBufferType(), base, idx);
    return b->CreateAlignedLoad(
      cast<GEPOperator>(ptr)->getResultElementType(), ptr, MaybeAlign(8u));
  }

  void emitDescriptorLoad(const ir::Op& op) {
    ir::SsaDef decl = operandDef(op, 0u);

    if (useAB) {
      if (const TextureVar* tex = findTexture(decl)) {
        setValue(op, loadAbHandle(tex->abWord, true));
        return;
      }
      if (const SamplerVar* smp = findSampler(decl)) {
        setValue(op, loadAbHandle(smp->abWord, true));
        return;
      }
      if (const BufferVar* buf = findBuffer(decl); buf && !buf->isPushData) {
        setValue(op, loadAbHandle(buf->abWord, false));
        return;
      }
    }

    const auto& argMap = fnBufferArgs.at(currentFnOp->getDef().getId());
    auto it = argMap.find(decl.getId());
    if (it == argMap.end())
      unsupported("DescriptorLoad outside resource arg map");
    setValue(op, currentFn->getArg(it->second));
  }

  void emitImageSample(const ir::Op& op) {
    const TextureVar& tex = textureForDescriptor(operandDef(op, 0u));

    Value* texHandle = getValue(operandDef(op, 0u));
    Value* smpHandle = getValue(operandDef(op, 1u));

    ir::SsaDef layerDef    = operandDef(op, 2u);
    ir::SsaDef coordDef    = operandDef(op, 3u);
    ir::SsaDef offsetDef   = operandDef(op, 4u);
    ir::SsaDef lodIndexDef = operandDef(op, 5u);
    ir::SsaDef lodBiasDef  = operandDef(op, 6u);
    ir::SsaDef lodClampDef = operandDef(op, 7u);
    ir::SsaDef derivXDef   = operandDef(op, 8u);
    ir::SsaDef derivYDef   = operandDef(op, 9u);
    ir::SsaDef drefDef     = operandDef(op, 10u);

    if (layerDef)
      unsupported("array image sample");
    if (offsetDef)
      unsupported("image sample with offset");

    bool isDref = bool(drefDef);
    bool isGrad = bool(derivXDef);

    if (isDref && tex.kind == ir::ResourceKind::eImage3D)
      unsupported("3d depth-compare sample");

    Type* f32 = Type::getFloatTy(ctx);
    Type* i1 = Type::getInt1Ty(ctx);
    Type* i32 = Type::getInt32Ty(ctx);
    uint32_t dims = textureCoordDims(tex.kind);
    Type* coordType = FixedVectorType::get(f32, dims);
    Type* offsetType = FixedVectorType::get(i32, dims);
    bool hasOffset = textureHasOffsetParams(tex.kind);

    Value* coord = getValue(coordDef);
    Value* minLod = lodClampDef
      ? getValue(lodClampDef) : ConstantFP::get(f32, 0.0);
    Value* isLevel = b->getInt1(bool(lodIndexDef));
    Value* lodOrBias = lodIndexDef
      ? getValue(lodIndexDef)
      : (lodBiasDef ? getValue(lodBiasDef) : ConstantFP::get(f32, 0.0));

    Value* handle = texHandle;
    if (isDref)
      handle = b->CreateBitCast(texHandle, textureHandleType(tex.kind, true));

    std::string name = isDref
      ? std::string("air.sample_compare_") + textureKindSuffix(tex.kind, true)
      : std::string("air.sample_") + textureKindSuffix(tex.kind, false);
    name += isGrad ? "_grad" : "";
    name += isDref ? ".f32" : ".v4f32";

    SmallVector<Type*, 12> paramTypes;
    SmallVector<Value*, 12> args;

    paramTypes.push_back(handle->getType());  args.push_back(handle);
    paramTypes.push_back(smpHandle->getType()); args.push_back(smpHandle);

    if (isDref) {
      paramTypes.push_back(i32); args.push_back(b->getInt32(1u));
    }

    paramTypes.push_back(coordType); args.push_back(coord);

    if (isDref) {
      paramTypes.push_back(f32); args.push_back(getValue(drefDef));
    }

    if (isGrad) {
      paramTypes.push_back(coordType); args.push_back(getValue(derivXDef));
      paramTypes.push_back(coordType); args.push_back(getValue(derivYDef));
      paramTypes.push_back(f32); args.push_back(minLod);
      if (hasOffset) {
        paramTypes.push_back(i1); args.push_back(b->getInt1(true));
        paramTypes.push_back(offsetType);
        args.push_back(Constant::getNullValue(offsetType));
      }
    } else {
      if (hasOffset) {
        paramTypes.push_back(i1); args.push_back(b->getInt1(true));
        paramTypes.push_back(offsetType);
        args.push_back(Constant::getNullValue(offsetType));
      }
      paramTypes.push_back(i1); args.push_back(isLevel);
      paramTypes.push_back(f32); args.push_back(lodOrBias);
      paramTypes.push_back(f32); args.push_back(minLod);
    }

    paramTypes.push_back(i32); args.push_back(b->getInt32(0u));

    Type* texelType = isDref
      ? f32 : static_cast<Type*>(FixedVectorType::get(f32, 4u));
    StructType* retType = StructType::get(ctx,
      { texelType, Type::getInt8Ty(ctx) });

    Function* intrinsic = getAirIntrinsic(name, retType, paramTypes);
    Value* result = b->CreateCall(intrinsic, args);
    setValue(op, b->CreateExtractValue(result, { 0u }));
  }

  void emitImageGather(const ir::Op& op) {
    const TextureVar& tex = textureForDescriptor(operandDef(op, 0u));
    if (tex.kind == ir::ResourceKind::eImage3D)
      unsupported("gather on 3d texture");
    if (operandDef(op, 2u) || operandDef(op, 4u) || operandDef(op, 5u))
      unsupported("gather with layer/offset/dref");

    uint32_t component = uint32_t(op.getOperand(6u));

    Type* f32 = Type::getFloatTy(ctx);
    Type* i32 = Type::getInt32Ty(ctx);
    uint32_t dims = textureCoordDims(tex.kind);
    Type* coordType = FixedVectorType::get(f32, dims);
    Type* offsetType = FixedVectorType::get(i32, dims);
    bool hasOffset = textureHasOffsetParams(tex.kind);

    Value* texHandle = getValue(operandDef(op, 0u));
    Value* smpHandle = getValue(operandDef(op, 1u));

    StructType* retType = StructType::get(ctx,
      { FixedVectorType::get(f32, 4u), Type::getInt8Ty(ctx) });

    SmallVector<Type*, 8> paramTypes {
      texHandle->getType(), smpHandle->getType(), coordType };
    SmallVector<Value*, 8> args {
      texHandle, smpHandle, getValue(operandDef(op, 3u)) };

    if (hasOffset) {
      paramTypes.push_back(Type::getInt1Ty(ctx));
      args.push_back(b->getInt1(true));
      paramTypes.push_back(offsetType);
      args.push_back(Constant::getNullValue(offsetType));
    }
    paramTypes.push_back(i32); args.push_back(b->getInt32(component));
    paramTypes.push_back(i32); args.push_back(b->getInt32(0u));

    std::string name = std::string("air.gather_") +
      textureKindSuffix(tex.kind, false) + ".v4f32";

    Value* result = b->CreateCall(
      getAirIntrinsic(name, retType, paramTypes), args);
    setValue(op, b->CreateExtractValue(result, { 0u }));
  }

  void emitImageQuerySize(const ir::Op& op) {
    const TextureVar& tex = textureForDescriptor(operandDef(op, 0u));
    Value* texHandle = getValue(operandDef(op, 0u));
    Value* lod = getValue(operandDef(op, 1u));

    const char* suffix = textureKindSuffix(tex.kind, false);
    Type* i32 = Type::getInt32Ty(ctx);

    auto query = [&](const char* what, bool takesLod) -> Value* {
      std::string name = std::string("air.get_") + what + "_" + suffix;
      Function* fn = takesLod
        ? getAirIntrinsic(name, i32, { texHandle->getType(), i32 })
        : getAirIntrinsic(name, i32, { texHandle->getType() });
      return takesLod
        ? b->CreateCall(fn, { texHandle, lod })
        : b->CreateCall(fn, { texHandle });
    };

    uint32_t dims = op.getType().getBaseType(0u).getVectorSize();
    Value* size = UndefValue::get(FixedVectorType::get(i32, dims));
    size = b->CreateInsertElement(size, query("width", true), uint64_t(0u));
    if (dims > 1u)
      size = b->CreateInsertElement(size, query("height", true), uint64_t(1u));
    if (dims > 2u)
      size = b->CreateInsertElement(size, query("depth", true), uint64_t(2u));

    Value* result = UndefValue::get(mapType(op.getType(), false));
    result = b->CreateInsertValue(result, size, { 0u });
    result = b->CreateInsertValue(result,
      query("num_mip_levels", false), { 1u });
    setValue(op, result);
  }

  void emitDeriv(const ir::Op& op) {
    Type* type = mapType(op.getType(), false);

    std::string mangle = type->isVectorTy()
      ? ".v" + std::to_string(
          cast<FixedVectorType>(type)->getNumElements()) + "f32"
      : ".f32";
    std::string name =
      (op.getOpCode() == ir::OpCode::eDerivX ? "air.dfdx" : "air.dfdy") +
      mangle;

    Function* fn = getAirIntrinsic(name, type, { type });
    CallInst* call = b->CreateCall(fn, { getValue(operandDef(op, 0u)) });
    call->setFastMathFlags(FastMathFlags::getFast());
    setValue(op, call);
  }

  Metadata* mdString(StringRef s) { return MDString::get(ctx, s); }
  Metadata* mdI32(uint32_t v) {
    return ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(ctx), v));
  }
  MDNode* mdTuple(ArrayRef<Metadata*> ops) { return MDTuple::get(ctx, ops); }

  static std::string varyingName(uint32_t location) {
    return "generated(loc" + std::to_string(location) + "_user)";
  }

  MDNode* buildAbMetadata() {
    struct MemberDesc {
      uint32_t    word;
      std::string typeName;
      std::string name;
      Metadata*   sub;
    };
    std::vector<MemberDesc> members(abWordCount);

    for (const auto& buf : buffers) {
      if (buf.abWord < 0)
        continue;
      uint32_t byteSize =
        uint32_t(mod->getDataLayout().getTypeAllocSize(buf.type));
      members[buf.abWord] = { uint32_t(buf.abWord), buf.name, buf.name,
        mdTuple({
          mdI32(uint32_t(buf.abWord)), mdString("air.buffer"),
          mdString("air.location_index"), mdI32(uint32_t(buf.abWord)), mdI32(1u),
          mdString("air.read"),
          mdString("air.address_space"), mdI32(kAsConstant),
          mdString("air.arg_type_size"), mdI32(byteSize),
          mdString("air.arg_type_align_size"), mdI32(16u),
          mdString("air.arg_type_name"), mdString(buf.name),
          mdString("air.arg_name"), mdString(buf.name) }) };
    }
    for (const auto& tex : textures) {
      members[tex.abWord] = { uint32_t(tex.abWord),
        textureMslTypeName(tex.kind), tex.name,
        mdTuple({
          mdI32(uint32_t(tex.abWord)), mdString("air.texture"),
          mdString("air.location_index"), mdI32(uint32_t(tex.abWord)), mdI32(1u),
          mdString("air.sample"),
          mdString("air.arg_type_name"), mdString(textureMslTypeName(tex.kind)),
          mdString("air.arg_name"), mdString(tex.name) }) };
    }
    for (const auto& smp : samplers) {
      members[smp.abWord] = { uint32_t(smp.abWord), "sampler", smp.name,
        mdTuple({
          mdI32(uint32_t(smp.abWord)), mdString("air.sampler"),
          mdString("air.location_index"), mdI32(uint32_t(smp.abWord)), mdI32(1u),
          mdString("air.arg_type_name"), mdString("sampler"),
          mdString("air.arg_name"), mdString(smp.name) }) };
    }

    SmallVector<Metadata*, 64> info;
    for (const auto& m : members) {
      info.push_back(mdI32(m.word * 8u));
      info.push_back(mdI32(8u));
      info.push_back(mdI32(0u));
      info.push_back(mdString(m.typeName));
      info.push_back(mdString(m.name));
      info.push_back(mdString("air.indirect_argument"));
      info.push_back(m.sub);
    }

    uint32_t abSize = abWordCount * 8u;
    return mdTuple({
      mdI32(entryAbArgIndex), mdString("air.indirect_buffer"),
      mdString("air.buffer_size"), mdI32(abSize),
      mdString("air.location_index"), mdI32(AirBuilder::kAbTableIndex), mdI32(1u),
      mdString("air.read"),
      mdString("air.address_space"), mdI32(kAsConstant),
      mdString("air.struct_type_info"), mdTuple(info),
      mdString("air.arg_type_size"), mdI32(abSize),
      mdString("air.arg_type_align_size"), mdI32(8u),
      mdString("air.arg_type_name"), mdString("d9mt_ab"),
      mdString("air.arg_name"), mdString("ab") });
  }

  void buildMetadata() {
    SmallVector<Metadata*, 4> outputMd;
    for (const auto& out : outputs) {
      if (out.isBuiltin && out.builtin == ir::BuiltIn::ePosition) {
        outputMd.push_back(mdTuple({
          mdString("air.position"),
          mdString("air.arg_type_name"), mdString("float4"),
          mdString("air.arg_name"), mdString("pos") }));
      } else if (out.isBuiltin && out.builtin == ir::BuiltIn::ePointSize) {
        outputMd.push_back(mdTuple({
          mdString("air.point_size"),
          mdString("air.arg_type_name"), mdString("float"),
          mdString("air.arg_name"), mdString("psize") }));
      } else if (out.isBuiltin && out.builtin == ir::BuiltIn::eClipDistance) {
        uint32_t arraySize =
          uint32_t(cast<ArrayType>(out.type)->getNumElements());
        outputMd.push_back(mdTuple({
          mdString("air.clip_distance"),
          mdString("air.clip_distance_array_size"), mdI32(arraySize),
          mdString("air.arg_type_name"), mdString("float"),
          mdString("air.arg_name"), mdString("clip") }));
      } else if (out.isBuiltin && out.builtin == ir::BuiltIn::eDepth) {
        outputMd.push_back(mdTuple({
          mdString("air.depth"),
          mdString("air.depth_qualifier"), mdString("air.any"),
          mdString("air.arg_type_name"), mdString("float"),
          mdString("air.arg_name"), mdString("depth") }));
      } else if (stage == ir::ShaderStage::eVertex) {
        outputMd.push_back(mdTuple({
          mdString("air.vertex_output"),
          mdString(varyingName(out.location)),
          mdString("air.arg_type_name"), mdString("float4"),
          mdString("air.arg_name"),
          mdString("o" + std::to_string(out.location)) }));
      } else {
        outputMd.push_back(mdTuple({
          mdString("air.render_target"), mdI32(out.location), mdI32(0u),
          mdString("air.arg_type_name"), mdString("float4") }));
      }
    }

    SmallVector<Metadata*, 8> inputMd;
    for (const auto& in : inputs) {
      if (in.isBuiltin && in.builtin == ir::BuiltIn::ePosition) {
        inputMd.push_back(mdTuple({
          mdI32(in.argIndex), mdString("air.position"),
          mdString("air.center"), mdString("air.no_perspective"),
          mdString("air.arg_type_name"), mdString("float4"),
          mdString("air.arg_name"), mdString("pos") }));
      } else if (in.isBuiltin) {
        uint32_t width = in.type->isVectorTy()
          ? uint32_t(cast<FixedVectorType>(in.type)->getNumElements()) : 1u;
        std::string typeName = in.type->isIntOrIntVectorTy(1u)
          ? "bool"
          : width > 1u ? "float" + std::to_string(width) : "float";
        inputMd.push_back(mdTuple({
          mdI32(in.argIndex), mdString(stageInBuiltinMdName(in.builtin)),
          mdString("air.arg_type_name"), mdString(typeName),
          mdString("air.arg_name"),
          mdString("builtin" + std::to_string(uint32_t(in.builtin))) }));
      } else if (stage == ir::ShaderStage::eVertex) {
        inputMd.push_back(mdTuple({
          mdI32(in.argIndex), mdString("air.vertex_input"),
          mdString("air.location_index"), mdI32(in.location), mdI32(1u),
          mdString("air.arg_type_name"), mdString("float4"),
          mdString("air.arg_name"),
          mdString("v" + std::to_string(in.location)) }));
      } else {
        inputMd.push_back(mdTuple({
          mdI32(in.argIndex), mdString("air.fragment_input"),
          mdString(varyingName(in.location)),
          mdString("air.center"), mdString("air.perspective"),
          mdString("air.arg_type_name"), mdString("float4"),
          mdString("air.arg_name"),
          mdString("v" + std::to_string(in.location)) }));
      }
    }

    if (useAB) {
      for (const auto& buf : buffers) {
        if (!buf.isPushData)
          continue;
        uint32_t byteSize =
          uint32_t(mod->getDataLayout().getTypeAllocSize(buf.type));
        inputMd.push_back(mdTuple({
          mdI32(buf.argIndex), mdString("air.buffer"),
          mdString("air.buffer_size"), mdI32(byteSize),
          mdString("air.location_index"), mdI32(buf.bufferIndex), mdI32(1u),
          mdString("air.read"),
          mdString("air.address_space"), mdI32(kAsConstant),
          mdString("air.arg_type_size"), mdI32(byteSize),
          mdString("air.arg_type_align_size"), mdI32(16u),
          mdString("air.arg_type_name"), mdString(buf.name),
          mdString("air.arg_name"), mdString(buf.name) }));
      }

      if (abWordCount > 0u)
        inputMd.push_back(buildAbMetadata());
    } else {
      for (const auto& buf : buffers) {
        uint32_t byteSize =
          uint32_t(mod->getDataLayout().getTypeAllocSize(buf.type));
        inputMd.push_back(mdTuple({
          mdI32(buf.argIndex), mdString("air.buffer"),
          mdString("air.buffer_size"), mdI32(byteSize),
          mdString("air.location_index"), mdI32(buf.bufferIndex), mdI32(1u),
          mdString("air.read"),
          mdString("air.address_space"), mdI32(kAsConstant),
          mdString("air.arg_type_size"), mdI32(byteSize),
          mdString("air.arg_type_align_size"), mdI32(16u),
          mdString("air.arg_type_name"), mdString(buf.name),
          mdString("air.arg_name"), mdString(buf.name) }));
      }

      for (const auto& tex : textures) {
        inputMd.push_back(mdTuple({
          mdI32(tex.argIndex), mdString("air.texture"),
          mdString("air.location_index"), mdI32(tex.slot), mdI32(1u),
          mdString("air.sample"),
          mdString("air.arg_type_name"), mdString(textureMslTypeName(tex.kind)),
          mdString("air.arg_name"), mdString(tex.name) }));
      }
      for (const auto& smp : samplers) {
        inputMd.push_back(mdTuple({
          mdI32(smp.argIndex), mdString("air.sampler"),
          mdString("air.location_index"), mdI32(smp.slot), mdI32(1u),
          mdString("air.arg_type_name"), mdString("sampler"),
          mdString("air.arg_name"), mdString(smp.name) }));
      }
    }

    const char* stageList =
      stage == ir::ShaderStage::eVertex ? "air.vertex" : "air.fragment";
    mod->getOrInsertNamedMetadata(stageList)->addOperand(mdTuple({
      ValueAsMetadata::get(entryFn),
      mdTuple(outputMd), mdTuple(inputMd) }));

    mod->addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8u);
    mod->addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16u);

    auto* opts = mod->getOrInsertNamedMetadata("air.compile_options");
    opts->addOperand(mdTuple({ mdString("air.compile.denorms_disable") }));
    opts->addOperand(mdTuple({ mdString("air.compile.fast_math_enable") }));
    opts->addOperand(mdTuple({ mdString("air.compile.framebuffer_fetch_enable") }));

    mod->getOrInsertNamedMetadata("air.version")->addOperand(
      mdTuple({ mdI32(2u), mdI32(7u), mdI32(0u) }));
    mod->getOrInsertNamedMetadata("air.language_version")->addOperand(
      mdTuple({ mdString("Metal"), mdI32(3u), mdI32(0u), mdI32(0u) }));
    mod->getOrInsertNamedMetadata("air.source_file_name")->addOperand(
      mdTuple({ mdString("dxbc-spirv-air") }));
    mod->getOrInsertNamedMetadata("llvm.ident")->addOperand(
      mdTuple({ mdString("d9mt dxbc-spirv AIR backend") }));
  }

  void createSpecConstants() {
    if (specs.empty() || options.foldSpecConstants)
      return;

    Type* i32 = Type::getInt32Ty(ctx);
    SmallVector<Metadata*, 4> fcMd;

    for (auto& spec : specs) {
      std::string name = "spec" + std::to_string(spec.specId);
      std::string lenName = std::to_string(name.size()) + name;

      spec.workingGlobal = new GlobalVariable(*mod, i32, false,
        GlobalValue::InternalLinkage, UndefValue::get(i32),
        "_ZL" + lenName, nullptr, GlobalValue::NotThreadLocal, kAsConstant);
      spec.workingGlobal->setAlignment(Align(4u));

      spec.initGlobal = new GlobalVariable(*mod, i32, true,
        GlobalValue::LinkOnceODRLinkage, UndefValue::get(i32),
        "_Z" + lenName + ".MTL_FC_INIT_" +
          std::to_string(spec.specId) + "_j",
        nullptr, GlobalValue::NotThreadLocal, kAsConstant);
      spec.initGlobal->setVisibility(GlobalValue::HiddenVisibility);
      spec.initGlobal->setExternallyInitialized(true);
      spec.initGlobal->setSection("air.fc_initializer");
      spec.initGlobal->setAlignment(Align(4u));

      fcMd.push_back(mdTuple({
        ConstantAsMetadata::get(spec.initGlobal),
        mdString("uint"), mdString(name), mdI32(spec.specId) }));
    }

    Function* ctor = Function::Create(
      FunctionType::get(Type::getVoidTy(ctx), false),
      GlobalValue::InternalLinkage, "_GLOBAL__sub_I_air", *mod);
    ctor->setSection("air.static_init");
    ctor->addFnAttr(Attribute::NoUnwind);
    ctor->addFnAttr(Attribute::WillReturn);
    ctor->addFnAttr(Attribute::MustProgress);

    IRBuilder<> cb(BasicBlock::Create(ctx, "", ctor));
    for (const auto& spec : specs) {
      cb.CreateStore(
        cb.CreateLoad(i32, spec.initGlobal), spec.workingGlobal);
    }
    cb.CreateRetVoid();

    StructType* ctorTy = StructType::get(
      i32, ctor->getType(), Type::getInt8PtrTy(ctx));
    Constant* entry = ConstantStruct::get(ctorTy,
      ConstantInt::get(i32, 65535u), ctor,
      ConstantPointerNull::get(Type::getInt8PtrTy(ctx)));
    new GlobalVariable(*mod, ArrayType::get(ctorTy, 1u), false,
      GlobalValue::AppendingLinkage,
      ConstantArray::get(ArrayType::get(ctorTy, 1u), { entry }),
      "llvm.global_ctors");

    auto* fcList = mod->getOrInsertNamedMetadata("air.function_constants");
    for (Metadata* md : fcMd)
      fcList->addOperand(cast<MDNode>(md));
  }

  std::vector<uint8_t> run(std::string& irTextOut) {
    scanDeclarations();
    createSpecConstants();
    createFunctions();
    emitBodies();
    buildMetadata();

    std::string verifyLog;
    raw_string_ostream verifyStream(verifyLog);
    if (verifyModule(*mod, &verifyStream))
      throw std::runtime_error("AirBuilder: verification failed:\n" + verifyLog);

    raw_string_ostream irStream(irTextOut);
    mod->print(irStream, nullptr);

    SmallVector<char, 0> bitcode;
    raw_svector_ostream bcStream(bitcode);
    WriteBitcodeToFile(*mod, bcStream);

    d9mt::air::MetallibFunction fn;
    fn.name = options.entryName;
    if (stage == ir::ShaderStage::eVertex) {
      fn.type = d9mt::air::MetallibFunctionType::Vertex;
      for (const auto& in : inputs) {
        if (in.isBuiltin)
          continue;
        fn.vertexAttributes.push_back({
          "v" + std::to_string(in.location),
          uint8_t(in.location),
          d9mt::air::MetallibAttributeType::Float4 });
      }
    } else {
      fn.type = d9mt::air::MetallibFunctionType::Fragment;
    }

    if (!options.foldSpecConstants) {
      for (const auto& spec : specs) {
        fn.functionConstants.push_back({
          "spec" + std::to_string(spec.specId),
          d9mt::air::MetallibConstantType::Uint,
          uint16_t(spec.specId) });
      }
    }

    return d9mt::air::writeMetallib(
      bitcode.data(), bitcode.size(), { fn });
  }
};


AirBuilder::AirBuilder(const ir::Builder& builder, const Options& options)
: m_builder(builder), m_options(options) { }

AirBuilder::~AirBuilder() { }

std::vector<uint8_t> AirBuilder::buildMetallib() {
  AirBuilderImpl impl(m_builder, m_options);
  auto result = impl.run(m_irText);
  m_stage = impl.stage;

  m_bufferBindings.clear();
  for (const auto& buf : impl.buffers) {
    BufferBinding binding;
    binding.name = buf.name;
    binding.index = buf.bufferIndex;
    binding.isCbv = buf.isCbv;
    binding.regIndex = buf.regIndex;
    binding.abWord = buf.abWord;
    binding.byteSize = buf.type
      ? impl.mod->getDataLayout().getTypeAllocSize(buf.type).getFixedSize()
      : 0u;
    m_bufferBindings.push_back(std::move(binding));
  }

  m_textureBindings.clear();
  for (const auto& tex : impl.textures) {
    m_textureBindings.push_back({ tex.name, tex.slot,
      AirBuilderImpl::textureManifestKind(tex.kind), tex.abWord });
  }

  m_samplerBindings.clear();
  for (const auto& smp : impl.samplers)
    m_samplerBindings.push_back({ smp.name, smp.slot, "", smp.abWord });

  m_abInfo = AbInfo();
  m_abInfo.used = impl.useAB && impl.abWordCount > 0u;
  m_abInfo.sizeBytes = impl.abWordCount * 8u;

  m_specBindings.clear();
  if (!m_options.foldSpecConstants) {
    for (const auto& spec : impl.specs)
      m_specBindings.push_back({
        "spec" + std::to_string(spec.specId), spec.specId });
  }

  m_outputInfo = OutputInfo();
  for (const auto& out : impl.outputs) {
    if (!out.isBuiltin && impl.stage == ir::ShaderStage::ePixel)
      m_outputInfo.renderTargetCount =
        std::max(m_outputInfo.renderTargetCount, out.location + 1u);
    if (out.isBuiltin && out.builtin == ir::BuiltIn::eDepth)
      m_outputInfo.writesDepth = true;
    if (out.isBuiltin && out.builtin == ir::BuiltIn::ePointSize)
      m_outputInfo.writesPointSize = true;
  }

  return result;
}

}
