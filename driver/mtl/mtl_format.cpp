#include "mtl_format.h"

// ---------------------------------------------------------------------------
// mtl_format.cpp — MtlFormatInfo lookup. PHASE-C: this is where the full
// per-MtlFormat metadata table (block size / element size / aspects / planes)
// lands, mirroring DXVK's imageFormatInfo(). For this phase the table is empty and
// the lookup returns nullptr (callers treat that as "metadata unknown").
// ---------------------------------------------------------------------------

namespace dxvk {

  // Minimal format metadata (was DxvkFormatInfo). Element sizes for the
  // formats the D3D9 mapping table produces; PHASE-C: fill as ports need.
  const MtlFormatInfo* lookupFormatInfo(MtlFormat format) {
    static const MtlFormatInfo invalid = { 0u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b1  = { 1u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b2  = { 2u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b4  = { 4u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b8  = { 8u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b16 = { 16u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo bc1 = { 8u, 4u, 4u, MtlFormatFlagCompressed, MtlAspectColor };
    static const MtlFormatInfo bc23 = { 16u, 4u, 4u, MtlFormatFlagCompressed, MtlAspectColor };
    static const MtlFormatInfo d32s8 = { 8u, 1u, 1u,   /* D32F+S8X24 packed readback layout */
      MtlFormatFlagDepth | MtlFormatFlagStencil, MtlAspectMask(MtlAspectDepth | MtlAspectStencil) };
    static const MtlFormatInfo d32 = { 4u, 1u, 1u, MtlFormatFlagDepth, MtlAspectDepth };
    static const MtlFormatInfo d16 = { 2u, 1u, 1u, MtlFormatFlagDepth, MtlAspectDepth };

    /* Numeric cases ARE MTLPixelFormat values (MtlFormat mirrors them 1:1;
     * audited against MTLPixelFormat.h). Element sizes per texel — or per
     * 4x4 block for BC: BC1 = 8 bytes/block, BC2/BC3 = 16 bytes/block. */
    switch (uint32_t(format)) {
      case 0u:  return &invalid;
      case 1u: case 10u: case 11u: case 12u: case 13u: case 14u:
        return &b1;                                    /* A8/R8-class */
      case 20u: case 22u: case 23u: case 24u: case 25u: case 30u: case 31u:
      case 32u: case 33u: case 40u: case 41u: case 42u: case 43u:
        return &b2;                                    /* R16/RG8/B5-class */
      case 70u: case 71u: case 72u: case 73u: case 74u: case 75u:
      case 80u: case 81u: case 85u: case 90u: case 91u: case 92u: case 93u:
        return &b4;                                    /* RGBA8/BGRA8/RG16/R32/RGB10A2 */
      case 100u: case 103u: case 104u: case 105u:
      case 110u: case 112u: case 113u: case 114u: case 115u:
        return &b8;                                    /* RG32 + RGBA16 (8 B/texel) */
      case 123u: case 124u: case 125u: case 126u:
        return &b16;                                   /* RGBA32 (16 B/texel) */
      case 130u: case 131u:
        return &bc1;                                   /* BC1 (8 B/block) */
      case 132u: case 133u: case 134u: case 135u:
        return &bc23;                                  /* BC2/BC3 (16 B/block) */
      case 250u: return &d16;
      case 252u: return &d32;                          /* Depth32Float */
      case 260u: case 261u: return &d32s8;             /* Depth32F_S8 / X32_S8 */
      default:  return &b4;                            /* safe default: 4B/texel */
    }
  }

}
