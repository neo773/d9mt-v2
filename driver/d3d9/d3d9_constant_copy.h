#pragma once

#include <optional>
#include <algorithm>
#include <unordered_set>

#include "../util/util_small_vector.h"
#include "../util/util_vector.h"
#include "../util/util_math.h"
#include "../util/dxvk_hash.h"
#include "../util/thread.h"

namespace dxvk {

  enum class D3D9ConstantType {
    Float,
    Int,
    Bool
  };

  struct D3D9ImmediateFloatConstant {
    uint32_t index;
    Vector4 value;
  };

  struct D3D9ConstantRange {
    uint16_t dstIndex = 0u;
    uint16_t srcIndex = 0u;
    uint16_t count = 0u;
    bool isDynamicallyIndexed = false;
    bool isShaderDefined = false;

    bool eq(const D3D9ConstantRange& other) const {
      return dstIndex             == other.dstIndex
          && srcIndex             == other.srcIndex
          && count                == other.count
          && isDynamicallyIndexed == other.isDynamicallyIndexed
          && isShaderDefined      == other.isShaderDefined;
    }

    size_t hash() const {
      DxvkHashState hash;
      hash.add(dstIndex);
      hash.add(srcIndex);
      hash.add(count);
      hash.add(uint32_t(isDynamicallyIndexed));
      hash.add(uint32_t(isShaderDefined));
      return hash;
    }
  };


  class D3D9ConstantBufferLayout {

  public:

    D3D9ConstantBufferLayout();

    D3D9ConstantBufferLayout(uint32_t maxCount);

    D3D9ConstantBufferLayout(uint32_t useLength, const uint32_t* useMask);

    D3D9ConstantBufferLayout(
            uint32_t            maxCount,
            uint32_t            useLength,
      const uint32_t*           useMask,
            size_t              defCount,
      const D3D9ImmediateFloatConstant* defData);

    ~D3D9ConstantBufferLayout();

    uint32_t getRangeCount() const {
      return uint32_t(m_ranges.size());
    }

    D3D9ConstantRange getRange(uint32_t index) const {
      return m_ranges[index];
    }

    uint32_t computeConstantCount(uint32_t valueCount) const {
      return std::clamp(valueCount, m_minCount, m_maxCount);
    }

    bool isDynamicallyIndexed() const {
      return m_dynamicIndexing;
    }

    std::optional<uint32_t> findConstant(uint32_t srcIndex) const {
      if (!m_dynamicIndexing) {
        for (const auto& e : m_ranges) {
          if (srcIndex >= e.srcIndex && srcIndex < e.srcIndex + e.count)
            return std::make_optional(e.dstIndex + (srcIndex - e.srcIndex));
        }
      }

      return std::nullopt;
    }

    const Vector4* getShaderDefinedConstants() const {
      return m_constantData.data();
    }

    bool eq(const D3D9ConstantBufferLayout& other) const;

    size_t hash() const;

  private:

    small_vector<D3D9ConstantRange, 16u> m_ranges;
    small_vector<Vector4, 16u> m_constantData;

    uint32_t m_minCount = 0u;
    uint32_t m_maxCount = 0u;

    bool m_dynamicIndexing = false;

    void addRange(const D3D9ConstantRange& range);

    void addConstantData(
      const D3D9ConstantRange&          range,
            size_t                      defCount,
      const D3D9ImmediateFloatConstant* defData);

  };


  struct D3D9ConstantBufferSizes {
    uint32_t floatBufferSize  = 0u;
    uint32_t intBufferSize    = 0u;
    uint32_t boolBufferSize   = 0u;
  };


  struct D3D9ConstantBufferCopyArgs {
    void* floatBuffer = nullptr;
    size_t floatBufferSize = 0u;
    uint32_t floatConstantCount = 0u;
    bool flushNan = false;
    void* intBuffer = nullptr;
    size_t intBufferSize = 0u;
    void* boolBuffer = nullptr;
    size_t boolBufferSize = 0u;
    const Vector4* constFloatApi = nullptr;
    const Vector4* constFloatDef = nullptr;
    const Vector4i* constIntApi = nullptr;
    const uint32_t* constBoolApi = nullptr;
  };


  class D3D9ConstantBufferCopy {

  public:

    D3D9ConstantBufferCopy();

    D3D9ConstantBufferCopy(
            D3D9ConstantBufferLayout floats,
            D3D9ConstantBufferLayout ints,
            D3D9ConstantBufferLayout bools);

    ~D3D9ConstantBufferCopy();

    const D3D9ConstantBufferLayout& getLayout(D3D9ConstantType type) const {
      if (type == D3D9ConstantType::Float)
        return m_floatLayout;
      if (type == D3D9ConstantType::Int)
        return m_intLayout;
      return m_boolLayout;
    }

    D3D9ConstantBufferSizes getAllocationSizes(uint32_t floatCount) const{
      D3D9ConstantBufferSizes result = {};
      result.floatBufferSize = m_floatLayout.computeConstantCount(floatCount) * sizeof(Vector4);
      result.intBufferSize = m_intLayout.computeConstantCount(0u) * sizeof(Vector4i);
      result.boolBufferSize = m_boolLayout.computeConstantCount(0u) * sizeof(uint32_t);
      return result;
    }

    void copyConstantData(const D3D9ConstantBufferCopyArgs& args) const;

    bool eq(const D3D9ConstantBufferCopy& other) const;

    size_t hash() const;

    static const D3D9ConstantBufferCopy* getOrCreate(
            D3D9ConstantBufferLayout floats,
            D3D9ConstantBufferLayout ints,
            D3D9ConstantBufferLayout bools);

  private:

    D3D9ConstantBufferLayout m_floatLayout  = {};
    D3D9ConstantBufferLayout m_intLayout    = {};
    D3D9ConstantBufferLayout m_boolLayout   = {};

    static dxvk::mutex s_mutex;
    static std::unordered_set<D3D9ConstantBufferCopy, DxvkHash, DxvkEq> s_layouts;

    template<bool FlushNan>
    void writeFloatBuffer(const D3D9ConstantBufferCopyArgs& args) const;

    template<bool FlushNan>
    void writeFloatRange(const D3D9ConstantBufferCopyArgs& args, const D3D9ConstantRange& range, uint32_t maxCount) const;

    void writeIntBuffer(const D3D9ConstantBufferCopyArgs& args) const;

    void writeIntRange(const D3D9ConstantBufferCopyArgs& args, const D3D9ConstantRange& range) const;

    void writeBoolBuffer(const D3D9ConstantBufferCopyArgs& args) const;

    void writeBoolRange(const D3D9ConstantBufferCopyArgs& args, const D3D9ConstantRange& range) const;

    static void pad16(void* dst, size_t start, size_t end);

  };

}
