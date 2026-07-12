#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <array>
#include <string>
#include <memory>
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
   * \brief CbvIndex -> AIR Metal buffer-slot map (SOURCE_OF_TRUTH §4b bug 1).
   *
   * The frontend binds each D3D9 constant buffer at its raw CbvIndex, but the
   * AIR shader declares its constant buffers at DIFFERENT Metal [[buffer(n)]]
   * slots (air_builder assigns them). This table, built from the d9mtair
   * reflection blob per compiled shader, maps each CbvIndex (0..7) to the Metal
   * slot the shader actually reads (-1 = the shader does not declare that
   * buffer). Threaded onto MtlShaderFunction and consumed by the backend's
   * arg-table flush so a CBV lands where the shader reads it and never collides
   * with a vertex stream (kVertexBufferBaseIndex) or push data (slot 20).
   * Size 8 matches D3D9ShaderResourceMapping::CbvIndex::Count (asserted below).
   */
  struct D3D9CbvSlotMap {
    std::array<int8_t, 8> slot;
    D3D9CbvSlotMap() { slot.fill(int8_t(-1)); }
  };

  /**
   * \brief Neutral Metal shader-backend interface (was DxvkIrShader-shaped)
   *
   * One object per unique (SM3 bytecode + options). getMetallib() is the Phase-B
   * equivalent of DxvkIrShader::getCode — returns a metallib for a concrete
   * (spec-state, linkage) pair instead of SPIR-V (doc 03 §1b step 5, §5).
   *
   * Cross-stage linkage travels as the paired stage's *serialized IR* (opaque
   * bytes) plus a precomputed IO hash, NOT a bare cookie: the FS side needs the
   * VS's real output IoMap to run the VS<->PS interpolant reconciliation
   * (resolveSemanticIo / resolveMismatchedIo, doc 03 §2b — the historical
   * blackscreen / PSO-reject class). Deserialization happens inside the concrete
   * backend, so no dxbc_spv type crosses this boundary.
   */
  class ID3D9MetalShaderBackend : public RcObject {

  public:

    virtual ~ID3D9MetalShaderBackend() { }

    // Stage of this shader (Vertex / Fragment).
    virtual MtlShaderStage stage() const = 0;

    // Sampler + RT masks (D3D9CommonShader::GetShaderMask analogue).
    virtual D3D9MetalShaderMasks masks() const = 0;

    // Opaque serialized Phase-A legalized IR of THIS stage. The paired stage's
    // backend consumes it (via PairedStageIo::prevStageIr) to rebuild this
    // stage's output IoMap for reconciliation. Empty if Phase-A failed. The
    // bytes are dxbc_spv-serialized IR but are treated as opaque here.
    virtual const std::vector<uint8_t>& serializedIr() const = 0;

    // Stable 64-bit hash of this stage's OUTPUT interpolant signature (the
    // location+semantic IoMap of the shader outputs). Forms the pairedStageIoHash
    // component of the Phase-B metallib cache key, so a PS only recompiles when
    // the VS it pairs with actually changes its output layout. 0 if no outputs.
    virtual uint64_t outputIoHash() const = 0;

    // Cross-stage linkage descriptor for Phase-B (mirrors DxvkShaderLinkage's
    // prevStage / prevStageOutputs, doc 03 §1c). Neutral: carries the paired
    // (previous) stage's serialized IR + its outputIoHash + D3D9 semantic match.
    // A default-constructed value (prevStageIr == nullptr) requests the unpaired
    // default compile — flat-interp fixups only, no VS<->PS reconciliation.
    struct PairedStageIo {
      const std::vector<uint8_t>* prevStageIr    = nullptr; // paired prev-stage IR, or null
      uint64_t                    prevStageIoHash = 0u;     // == prev stage outputIoHash()
      bool                        matchSemantics  = true;   // D3D9 uses semantic-based IO
    };

    // Phase-B: metallib for a given (paired-stage linkage, spec-state) pair.
    // For a fragment shader with a non-null paired VS IR, runs resolveSemanticIo
    // + resolveMismatchedIo (per upstream DxvkIrShader::getCode) before the AIR
    // thunk when the stages are I/O-incompatible. Result is cached inside the
    // backend on (prevStageIoHash, specStateHash) — the shared bytecodeHash is
    // implicit in the object identity.
    //   specStateHash — hash of the referenced D3D9SpecData bytes.
    //   outCbvMap — if non-null, filled with the CbvIndex->AIR-slot mapping
    //   derived from the d9mtair reflection blob (SOURCE_OF_TRUTH §4b bug 1).
    virtual std::vector<uint8_t> getMetallib(const PairedStageIo& paired,
                                             uint64_t             specStateHash,
                                             D3D9CbvSlotMap*      outCbvMap = nullptr) = 0;
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

      // Fixed-function spec-constant data (D3D9SpecData). DXVK bakes this as
      // Vulkan specialization constants; the v2 FF ubershaders (auto-converted
      // from GLSL) instead read it as a plain CBV bound to both stages.
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
      // Sampler slot and binding indices match 1:1, see above
      return std::make_pair(IsVSSampler(index) ? MtlStageVertex : MtlStageFragment, index);
    }
  };

  static_assert(D3D9ShaderResourceMapping::CbvIndex::Count <= 8,
                "D3D9CbvSlotMap must hold every CbvIndex");

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

  // Shared per-common-shader cache of link-time-reconciled Metal shader
  // functions, keyed on the paired (previous) stage's outputIoHash. Held via a
  // shared_ptr so all value-copies of a D3D9CommonShader (module set, shader
  // objects) share one cache + mutex — the expensive metallib compile itself is
  // cached in the shared backend, this only memoizes the metallib->function wrap.
  struct D3D9ShaderLinkageCache {
    dxvk::mutex                                      mutex;
    std::unordered_map<uint64_t, Rc<MtlShaderFunction>> functions;
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

    // Unpaired default shader function (flat-interp fixups only). Used for the VS,
    // for the FF path, and as a fallback when no paired stage is available.
    Rc<MtlShaderFunction> GetShader() const {
      return m_shader;
    }

    // Link-time reconciled shader function for a concrete previous stage (doc 03
    // §2b). For a fragment shader this compiles + caches the VS<->PS interpolant-
    // reconciled metallib for pPrevStage's output layout; for any other case it
    // returns the unpaired GetShader(). Threaded at draw/PSO time (PrepareDraw).
    Rc<MtlShaderFunction> GetShaderForLinkage(const D3D9CommonShader* pPrevStage) const;

    // Output interpolant IO hash (VS side); 0 if no backend / no outputs. Used by
    // the draw-time linkage guard to detect when the paired layout changes.
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

    // Kept alive so Phase-B linkage variants can be compiled on demand at
    // draw/PSO time. The backend owns the (prevIoHash, spec)->metallib cache; it
    // is shared across all value-copies of this common shader (Rc semantics).
    Rc<ID3D9MetalShaderBackend> m_backend;
    D3D9DeviceEx*               m_device = nullptr;
    std::shared_ptr<D3D9ShaderLinkageCache> m_linkageCache;

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
