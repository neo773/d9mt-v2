#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <array>
#include <string>
#include <unordered_map>

#include "d3d9_include.h"
#include "d3d9_util.h"        // D3D9ShaderType, FirstVSSamplerSlot, IsVSSampler, caps::, D3D9ShaderMasks
#include "d3d9_options.h"     // D3D9FloatEmulation
#include "d3d9_resource.h"
#include "d3d9_shader_analysis.h"  // D3D9ShaderAnalysis (dxbc_spv, -I dxbc-spirv, gate-exempt)

#include "../mtl/mtl_types.h"
#include "../mtl/mtl_context.h"    // MtlShaderFunction (was Rc<DxvkShader>)

#include "../util/dxvk_hash.h"     // DxvkHash / DxvkEq (module-set map)
#include "../util/util_unmap.h"    // MemoryFilePool / MemoryFileRegion
#include "../util/sha1/sha1_util.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

// ---------------------------------------------------------------------------
// d3d9_shader.h — PHASE-D neutral shader-backend interface (doc 02 REWRITE,
// doc 03 §5). The upstream d3d9_shader.{h,cpp} holds compiled DxvkShader /
// DxvkSpirvShader / DxvkIrShaderConverter and emits SPIR-V; that is the REWRITE
// module. Here we expose only the *neutral* surface the d3d9 frontend needs
// (doc 03 §5 "minimal API our shader module must expose"), with NO include of
// v2/air (the AirBuilder stream) and NO dxbc_spv/SPIR-V types leaking in.
//
// PHASE-D: this is an interface skeleton only. The real backend (SM3 bytecode ->
// dxbc_spv::sm3::Converter -> legalized IR -> AirBuilder -> metallib, two-phase
// cached per doc 03 §1b/§2) is wired in the shader REWRITE track and lives behind
// this interface so the frontend never sees air/ or dxbc_spv headers.
// ---------------------------------------------------------------------------

namespace dxvk {

  // Sampler/RT usage masks the binder reads (was D3D9ShaderMasks in d3d9_util.h;
  // duplicated minimally here to keep this header standalone / air-free).
  struct D3D9MetalShaderMasks {
    uint32_t samplerMask = 0u;
    uint32_t rtMask      = 0u;
  };

  /**
   * \brief Neutral Metal shader-backend interface (was DxvkIrShader-shaped)
   *
   * One object per unique (SM3 bytecode + options). getMetallib() is the Phase-B
   * equivalent of DxvkIrShader::getCode — returns a metallib for a concrete
   * (spec-state, linkage) pair instead of SPIR-V (doc 03 §1b step 5, §5). The
   * linkage/spec parameters are passed as opaque cookies so no dxbc_spv type
   * crosses this boundary.
   */
  class ID3D9MetalShaderBackend : public RcObject {

  public:

    virtual ~ID3D9MetalShaderBackend() { }

    // Stage of this shader (Vertex / Fragment).
    virtual MtlShaderStage stage() const = 0;

    // Sampler + RT masks (D3D9CommonShader::GetShaderMask analogue).
    virtual D3D9MetalShaderMasks masks() const = 0;

    // Phase-B: metallib for a given linkage/spec cookie pair. PHASE-D: returns {}.
    //   linkageCookie — hash of prevStage + prevStageOutputs IoMap + flat/swizzle/
    //                   dual-src/sample (reuse DxvkShaderLinkage::hash, doc 03 §2d).
    //   specStateHash — hash of the referenced D3D9SpecData bytes.
    virtual std::vector<uint8_t> getMetallib(uint64_t linkageCookie,
                                             uint64_t specStateHash) = 0;
  };

  /**
   * \brief Shader resource mapping (KEEP from upstream d3d9_shader.h)
   *
   * Helper to compute backend resource indices for D3D9 binding slots. The one
   * Vulkan touch — getTextureSlotInfo's VkShaderStageFlags — is retyped to the
   * seam's MtlStageMask (doc 02 §1 1:1 stage retype).
   */
  struct D3D9ShaderResourceMapping {
    enum CbvIndex : uint32_t {
      VSClipPlanes            = 0u,
      VSFixedFunction         = 1u,
      VSVertexBlendData       = 2u,
      VSStaticConstants       = 3u,
      VSDynamicConstants      = 4u,
      PSShared                = 5u,
      PSStaticConstants       = 6u,

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
      // Sampler slot and binding indices match 1:1, see above
      return std::make_pair(IsVSSampler(index) ? MtlStageVertex : MtlStageFragment, index);
    }
  };

  // PHASE-D: factory that will build an ID3D9MetalShaderBackend from SM3 bytecode +
  // options + analysis. Declared here; the impl (air-backed) lives in the REWRITE
  // track. Kept as a declaration so the frontend can be ported against the seam.
  //
  //   Rc<ID3D9MetalShaderBackend> createD3D9MetalShader(
  //       const void* sm3Bytecode, size_t len, D3D9ShaderType type, bool isSWVP,
  //       const D3D9ShaderAnalysis& analysis);

  // -------------------------------------------------------------------------
  // Full shader class graph, ported off Vulkan onto the Mtl seam (PHASE-C6).
  // The frontend (d3d9_state.h holds Com<D3D9VertexShader>/Com<D3D9PixelShader>,
  // d3d9_device.h drives D3D9CommonShader/D3D9ShaderModuleSet) needs the concrete
  // shape. Retypes vs upstream d3d9_shader.h:
  //   Rc<DxvkShader>         -> Rc<MtlShaderFunction>   (the compiled module)
  //   DxvkShaderHash (key)   -> D3D9ShaderModuleKey     (Sha1Hash + stage; neutral)
  //   DxvkIrShaderCreateInfo -> DROPPED from D3D9ShaderCreateInfo (SPIR-V; REWRITE)
  // The SM3->IR->Air->metallib compile that fills m_shader is the shader REWRITE
  // track; here the ctor/GetShaderModule bodies are declared (d3d9_shader.cpp).
  // -------------------------------------------------------------------------

  struct D3D9ShaderOptions {
    /** Whether the device is configured to do SWVP. VS only. */
    bool isSWVP;
    /** Whether to emulate d3d9 float behaviour using clampps. */
    D3D9FloatEmulation d3d9FloatEmulation;
    /** Always use a spec constant to determine sampler type (Halo CE workaround). */
    bool forceSamplerTypeSpecConstants;
  };

  static_assert(sizeof(D3D9ShaderOptions) == 3u);

  struct D3D9ShaderCreateInfo {
    // DxvkIrShaderCreateInfo irCreateInfo — DROPPED (SPIR-V; shader REWRITE track).
    D3D9ShaderOptions shaderOptions;
  };

  // Neutral shader look-up key (was DxvkShaderHash, which carried a
  // VkShaderStageFlagBits). Sha1 of the SM3 bytecode + the stage.
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

  /**
   * \brief Common shader object
   *
   * Stores the compiled Metal shader function and the SHA-1 hash of the original
   * DXBC/SM3 bytecode, which can be used to identify the shader.
   */
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

    D3D9ShaderAnalysis    m_analysis;
    Rc<MtlShaderFunction> m_shader;
    std::string           m_name;

  };

  /**
   * \brief Common shader interface
   *
   * Implements methods for all D3D9*Shader interfaces and stores the actual
   * shader module object.
   */
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

  // Needs their own classes and not usings for forward decl.

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

  /**
   * \brief Shader module set
   *
   * Caches compiled shader modules keyed by bytecode hash. Thread-safe.
   */
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
