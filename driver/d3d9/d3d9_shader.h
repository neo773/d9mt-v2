#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <array>
#include <string>
#include <memory>
#include <unordered_map>

#include "d3d9_include.h"
#include "d3d9_util.h"
#include "d3d9_options.h"
#include "d3d9_resource.h"
#include "d3d9_shader_analysis.h"

#include "../mtl/mtl_types.h"
#include "../mtl/mtl_context.h"

#include "../util/dxvk_hash.h"
#include "../util/util_unmap.h"
#include "../util/sha1/sha1_util.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  struct D3D9MetalShaderMasks {
    uint32_t samplerMask = 0u;
    uint32_t rtMask      = 0u;
  };

  struct D3D9CbvSlotMap {
    std::array<int8_t, 8> slot;
    struct AbWord {
      uint8_t kind;
      uint8_t reg;
    };
    bool                 abUsed = false;
    uint8_t              abTableIndex = 21u;
    std::vector<AbWord>  abWords;
    D3D9CbvSlotMap() { slot.fill(int8_t(-1)); }
  };

  class ID3D9MetalShaderBackend : public RcObject {

  public:

    virtual ~ID3D9MetalShaderBackend() { }

    virtual MtlShaderStage stage() const = 0;

    virtual D3D9MetalShaderMasks masks() const = 0;

    virtual const std::vector<uint8_t>& serializedIr() const = 0;

    virtual uint64_t outputIoHash() const = 0;

    struct PairedStageIo {
      const std::vector<uint8_t>* prevStageIr    = nullptr;
      uint64_t                    prevStageIoHash = 0u;
      bool                        matchSemantics  = true;
    };

    virtual std::vector<uint8_t> getMetallib(const PairedStageIo& paired,
                                             uint64_t             specStateHash,
                                             D3D9CbvSlotMap*      outCbvMap = nullptr) = 0;
  };

  struct D3D9ShaderResourceMapping {
    enum CbvIndex : uint32_t {
      VSClipPlanes            = 0u,
      VSFixedFunction         = 1u,
      VSVertexBlendData       = 2u,
      VSStaticConstants       = 3u,
      VSDynamicConstants      = 4u,
      PSShared                = 5u,
      PSStaticConstants       = 6u,

      SpecData                = 7u,

      Count
    };

    static constexpr uint32_t computeTextureBinding(D3D9ShaderType shaderType, uint32_t index) {
      auto base = (shaderType == D3D9ShaderType::VertexShader) ? FirstVSSamplerSlot : 0u;
      return base + index;
    }

    static constexpr uint32_t getSwvpBufferIndex() {
      return caps::MaxTextures;
    }

    static constexpr std::pair<MtlStageMask, uint32_t> getTextureSlotInfo(uint32_t index) {
      return IsVSSampler(index)
        ? std::make_pair(MtlStageVertex,   index - FirstVSSamplerSlot)
        : std::make_pair(MtlStageFragment, index);
    }
  };

  static_assert(D3D9ShaderResourceMapping::CbvIndex::Count <= 8,
                "D3D9CbvSlotMap must hold every CbvIndex");

  struct D3D9ShaderOptions {
    bool isSWVP;
    D3D9FloatEmulation d3d9FloatEmulation;
    bool forceSamplerTypeSpecConstants;
  };

  static_assert(sizeof(D3D9ShaderOptions) == 3u);

  struct D3D9ShaderCreateInfo {
    D3D9ShaderOptions shaderOptions;
  };

  struct D3D9ShaderModuleKey {
    D3D9ShaderModuleKey() = default;
    D3D9ShaderModuleKey(MtlShaderStage stage, const Sha1Hash& sha)
    : m_stage(stage), m_sha(sha) { }

    MtlShaderStage stage() const { return m_stage; }

    bool eq(const D3D9ShaderModuleKey& o) const {
      return m_stage == o.m_stage && m_sha == o.m_sha;
    }

    size_t hash() const {
      DxvkHashState state;
      state.add(uint32_t(m_stage));
      for (uint32_t i = 0; i < 5u; i++)
        state.add(m_sha.dword(i));
      return state;
    }

  private:
    MtlShaderStage m_stage = MtlShaderStage::Vertex;
    Sha1Hash       m_sha   = { };
  };

  struct D3D9ShaderLinkageCache {
    dxvk::mutex                                      mutex;
    std::unordered_map<uint64_t, Rc<MtlShaderFunction>> functions;
  };

  class D3D9CommonShader {

  public:

    D3D9CommonShader() = default;

    D3D9CommonShader(
            D3D9DeviceEx*             pDevice,
      const D3D9ShaderModuleKey&      ShaderKey,
            D3D9ShaderAnalysis&&      Analysis,
      const D3D9ShaderCreateInfo&     ModuleInfo,
      const void*                     pShaderBytecode);

    Rc<MtlShaderFunction> GetShader() const {
      return m_shader;
    }

    Rc<MtlShaderFunction> GetShaderForLinkage(const D3D9CommonShader* pPrevStage) const;

    uint64_t GetOutputIoHash() const {
      return m_backend != nullptr ? m_backend->outputIoHash() : 0u;
    }

    std::string GetName() const {
      return m_name;
    }

    const D3D9InputSignature& GetInputSignature() const {
      return m_analysis.GetInputSignature();
    }

    const D3D9ShaderConstantsInfo& GetConstantsInfo() const { return m_analysis.GetConstantsInfo(); }
    const D3D9ImmediateConstantsInfo& GetImmediateConstants() const { return m_analysis.GetImmediateConstants(); }

    const D3D9ConstantBufferCopy* GetConstantLayout() const {
      return m_analysis.GetConstantLayout();
    }

    D3D9ShaderMasks GetShaderMask() const { return D3D9ShaderMasks{ m_analysis.GetSamplerMask(), m_analysis.GetRenderTargetMask() }; }

    dxbc_spv::sm3::ShaderInfo GetInfo() const { return m_analysis.GetShaderInfo(); }

  private:

    D3D9ShaderAnalysis          m_analysis;
    Rc<MtlShaderFunction>       m_shader;
    std::string                 m_name;

    Rc<ID3D9MetalShaderBackend> m_backend;
    D3D9DeviceEx*               m_device = nullptr;
    std::shared_ptr<D3D9ShaderLinkageCache> m_linkageCache;

  };

  template <typename Base>
  class D3D9Shader : public D3D9DeviceChild<Base> {

  public:

    D3D9Shader(
            D3D9DeviceEx*        pDevice,
            MemoryFilePool*      pAllocator,
      const D3D9CommonShader&    CommonShader,
      const void*                pShaderBytecode,
            uint32_t             BytecodeLength)
      : D3D9DeviceChild<Base>( pDevice )
      , m_shader             ( CommonShader )
      , m_bytecode           ( *pAllocator, BytecodeLength )
      , m_bytecodeLength     ( BytecodeLength ) {
      std::memcpy(m_bytecode.map(), pShaderBytecode, BytecodeLength);
      m_bytecode.unmap();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(Base)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      if (logQueryInterfaceError(__uuidof(Base), riid)) {
        Logger::warn("D3D9Shader::QueryInterface: Unknown interface query");
        Logger::warn(str::format(riid));
      }

      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetFunction(void* pOut, UINT* pSizeOfData) {
      if (pSizeOfData == nullptr)
        return D3DERR_INVALIDCALL;

      if (pOut == nullptr) {
        *pSizeOfData = m_bytecodeLength;
        return D3D_OK;
      }

      uint32_t copyAmount = std::min(*pSizeOfData, m_bytecodeLength);
      std::memcpy(pOut, m_bytecode.map(), copyAmount);
      m_bytecode.unmap();
      return D3D_OK;
    }

    const D3D9CommonShader* GetCommonShader() const {
      return &m_shader;
    }

  private:

    D3D9CommonShader m_shader;

    MemoryFileRegion m_bytecode;
    uint32_t         m_bytecodeLength;

  };

  class D3D9VertexShader final : public D3D9Shader<IDirect3DVertexShader9> {

  public:

    D3D9VertexShader(
            D3D9DeviceEx*        pDevice,
            MemoryFilePool*      pAllocator,
      const D3D9CommonShader&    CommonShader,
      const void*                pShaderBytecode,
            uint32_t             BytecodeLength)
      : D3D9Shader<IDirect3DVertexShader9>( pDevice, pAllocator, CommonShader, pShaderBytecode, BytecodeLength ) { }

  };

  class D3D9PixelShader final : public D3D9Shader<IDirect3DPixelShader9> {

  public:

    D3D9PixelShader(
            D3D9DeviceEx*        pDevice,
            MemoryFilePool*      pAllocator,
      const D3D9CommonShader&    CommonShader,
      const void*                pShaderBytecode,
            uint32_t             BytecodeLength)
      : D3D9Shader<IDirect3DPixelShader9>( pDevice, pAllocator, CommonShader, pShaderBytecode, BytecodeLength ) { }

  };

  class D3D9ShaderModuleSet : public RcObject {

  public:

    HRESULT GetShaderModule(
            D3D9DeviceEx*           pDevice,
      const D3D9ShaderModuleKey&    ShaderKey,
            D3D9ShaderAnalysis&&    ShaderAnalysis,
      const D3D9ShaderCreateInfo&   ModuleInfo,
      const void*                   pShaderBytecode,
            D3D9CommonShader*       pShader);

  private:

    dxvk::mutex m_mutex;

    std::unordered_map<
      D3D9ShaderModuleKey,
      D3D9CommonShader,
      DxvkHash, DxvkEq> m_modules;

  };

  template<typename T>
  const D3D9CommonShader* GetCommonShader(const T& pShader) {
    return pShader != nullptr ? pShader->GetCommonShader() : nullptr;
  }

}
