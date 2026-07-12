#pragma once
/*
 * Render-PSO memoization.
 *
 * Key design (doc 00 §1, doc 01 §5):
 *  - depth/stencil attachment formats are NOT part of the key (Metal 4
 *    removed them from the pipeline descriptor entirely).
 *  - vertex strides ARE part of the key (Metal bakes stride into the
 *    descriptor; the v2 stride bug made this explicit).
 *  - lookup must be cheap per draw: FNV over a packed POD key, memo of the
 *    last (key,pso) pair to skip the map entirely on same-material runs
 *    (v1's biggest PSO-path win).
 *
 * Builds go through MTL4Compiler_newRenderPipelineState (METAL 4 ONLY). The
 * stage functions are carried as (library handle + entry name "main"); the
 * caller passes the VS/PS metallib handles, not MTLFunction objects.
 */

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "wmt_handle.h"

namespace dxvk::backend {

  struct PsoVertexAttr {
    uint32_t format = 0u;     /* WMTAttributeFormat */
    uint16_t offset = 0u;
    uint8_t  bufferIndex = 0u;
    uint8_t  pad = 0u;
  };

  struct PsoVertexLayout {
    uint16_t stride = 0u;
    uint8_t  stepFunction = 0u;  /* per-vertex / per-instance / constant */
    uint8_t  pad = 0u;
  };

  struct PsoColorAttachment {
    uint32_t pixelFormat = 0u;   /* WMTPixelFormat */
    /* blend state, packed to match WMTColorAttachmentBlendInfo semantics */
    uint8_t  blendEnabled = 0u;
    uint8_t  writeMask = 0xfu;
    uint8_t  rgbSrcFactor = 1u;
    uint8_t  rgbDstFactor = 0u;
    uint8_t  rgbBlendOp = 0u;
    uint8_t  alphaSrcFactor = 1u;
    uint8_t  alphaDstFactor = 0u;
    uint8_t  alphaBlendOp = 0u;
  };

  /* Packed POD; hashed/compared bytewise. Zero-init before filling. */
  struct RenderPsoKey {
    uint64_t vsCookie = 0u;      /* shader identity incl. spec + linkage */
    uint64_t fsCookie = 0u;

    // 20 (>= 18) so the fixed-function ubershader's full input signature fits:
    // its stage_in declares all 18 FF vertex registers (Position0..BlendIndices),
    // and Metal requires EVERY declared attribute to be present in the vertex
    // descriptor (unlike Vulkan, which tolerates missing attributes).
    PsoVertexAttr   attrs[20] = { };
    PsoVertexLayout layouts[20] = { };
    uint8_t attrCount = 0u;
    uint8_t layoutCount = 0u;

    PsoColorAttachment colors[4] = { };
    uint8_t colorCount = 0u;

    uint8_t sampleCount = 1u;
    uint8_t topologyClass = 0u;  /* point/line/triangle — WMTPrimitiveTopologyClass */
    uint8_t alphaToCoverage = 0u;

    /* Metal-3 only suffix (absent from the Metal 4 descriptor): */
    uint32_t depthFormat = 0u;
    uint32_t stencilFormat = 0u;

    size_t hash() const {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
      uint64_t h = 0xcbf29ce484222325ull;
      for (size_t i = 0u; i < sizeof(*this); i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
      return size_t(h ^ (h >> 32));
    }

    bool operator == (const RenderPsoKey& other) const {
      return !std::memcmp(this, &other, sizeof(*this));
    }
  };

  class RenderPsoCache {

  public:

    explicit RenderPsoCache(obj_handle_t compiler);

    /* Returns the PSO handle, building it synchronously on miss.
     * vsLibrary/fsLibrary are MTLLibrary (metallib) handles (borrowed); the
     * entry point is "main". Returns 0 on build failure (caller drops+logs). */
    obj_handle_t get(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry = "main",
            const char*         fsEntry = "main");

    size_t size() const { return m_cache.size(); }

  private:

    struct KeyHash {
      size_t operator () (const RenderPsoKey& k) const { return k.hash(); }
    };

    obj_handle_t m_compiler;

    /* one-entry memo: same-material draw runs skip the map */
    RenderPsoKey m_lastKey = { };
    obj_handle_t m_lastPso = 0u;
    bool m_hasLast = false;

    std::unordered_map<RenderPsoKey, WmtRenderPso, KeyHash> m_cache;

    obj_handle_t build(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry,
            const char*         fsEntry);

  };

}
