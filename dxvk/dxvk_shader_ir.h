#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// Real dxbc-spirv headers — the frontend's shader converters derive from
// DxvkIrShaderConverter and call into ir::Builder/ShaderStage/ScalarType, so we
// must expose the exact same types. These are kept third-party tooling headers,
// not part of the shim's Vulkan/Metal surface.
#include <dxbc/dxbc_api.h>
#include <ir/ir_builder.h>
#include <ir/passes/ir_pass_lower_io.h>

#include "dxvk_shader.h"

// IR shader (shim). The D3D9 frontend builds a DxvkIrShaderConverter subclass
// and a default DxvkIrShaderCreateInfo, then asks the device to turn them into
// a Rc<DxvkShader> via createCachedShader. Because the shim renders only a
// fixed-function triangle through D9mtBackend, the IR is never converted or
// executed: DxvkIrShader keeps the converter + name and answers trivially.
// Signatures mirror the real DXVK header so the frontend + device compile
// unmodified (SHIM_SPEC.md).

namespace dxvk {

  // Metadata that cannot be inferred from the IR. The frontend default-
  // constructs this and sets only `.options`.
  struct DxvkIrShaderCreateInfo {
    DxvkShaderOptions options;
    uint32_t          flatShadingInputs = 0u;
    int32_t           rasterizedStream  = 0;

    size_t hash() const { return 0u; }
    bool   eq(const DxvkIrShaderCreateInfo&) const { return true; }
  };


  /**
   * \brief Raw shader binary for dxbc-spirv (shim base)
   *
   * The frontend's D3D9ShaderConverter / D3D9SWVPShaderGenerator derive from
   * this and implement the four virtuals. We keep the real interface and the
   * intrusive refcount so Rc<DxvkIrShaderConverter> works identically; the shim
   * just never invokes convertShader() on the render path.
   */
  class DxvkIrShaderConverter {

  public:

    void incRef() {
      m_useCount.fetch_add(1u, std::memory_order_acquire);
    }

    void decRef() {
      if (m_useCount.fetch_sub(1u, std::memory_order_release) == 1u)
        delete this;
    }

    virtual ~DxvkIrShaderConverter() = default;

    virtual void convertShader(
            dxbc_spv::ir::Builder&    builder) = 0;

    virtual uint32_t determineResourceIndex(
            dxbc_spv::ir::ShaderStage stage,
            dxbc_spv::ir::ScalarType  type,
            uint32_t                  regSpace,
            uint32_t                  regIndex) const = 0;

    virtual void dumpSource(const std::string& path) const = 0;

    virtual std::string getDebugName() const = 0;

  private:

    std::atomic<uint32_t> m_useCount = { 0u };

  };


  /**
   * \brief DXBC-SPIRV IR shader (shim)
   *
   * Constructed by the device's createCachedShader. Holds the converter +
   * create-info for identity; produces no code (the triangle pipeline is
   * prebuilt in D9mtBackend).
   */
  class DxvkIrShader : public DxvkShader {

  public:

    // Arbitrary spec-data descriptor set, kept to match the real constant the
    // frontend references via DxvkIrShader::SpecDataSet.
    static constexpr uint32_t SpecDataSet = 7u;

    DxvkIrShader(
      const DxvkIrShaderCreateInfo&   info,
            Rc<DxvkIrShaderConverter> shader)
    : m_baseIr(std::move(shader)), m_info(info) {
      if (m_baseIr != nullptr)
        m_debugName = m_baseIr->getDebugName();
      m_metadata.flatShadingInputs = info.flatShadingInputs;
    }

    // Prebuilt-from-cache form (name + serialized IR). Retained for ABI parity;
    // the shim ignores the IR bytes.
    DxvkIrShader(
            std::string               name,
      const DxvkIrShaderCreateInfo&   info,
            DxvkShaderMetadata        metadata,
            DxvkPipelineLayoutBuilder layout,
            std::vector<uint8_t>      ir)
    : m_info(info), m_debugName(std::move(name)),
      m_layout(std::move(layout)), m_ir(std::move(ir)), m_metadata(metadata) { }

    ~DxvkIrShader() = default;

    DxvkIrShaderCreateInfo getShaderCreateInfo() const {
      return m_info;
    }

    DxvkShaderMetadata getShaderMetadata() {
      return m_metadata;
    }

    void compile() {
      // No IR conversion in the shim.
    }

    SpirvCodeBuffer getCode(
      const DxvkShaderBindingMap*       /*bindings*/,
      const DxvkShaderLinkage*          /*linkage*/) {
      return SpirvCodeBuffer();
    }

    DxvkPipelineLayoutBuilder getLayout() {
      return m_layout;
    }

    void dump(std::ostream&) {
      // No SPIR-V to dump in the shim.
    }

    // Empty serialized-IR view; nothing consumes it on the render path.
    std::pair<const uint8_t*, size_t> getSerializedIr() {
      return { m_ir.data(), m_ir.size() };
    }

    std::string debugName() {
      return m_debugName;
    }

  private:

    Rc<DxvkIrShaderConverter> m_baseIr;
    DxvkIrShaderCreateInfo    m_info;
    std::string               m_debugName;
    DxvkPipelineLayoutBuilder m_layout;
    std::vector<uint8_t>      m_ir;
    DxvkShaderMetadata        m_metadata = { };

  };

}
