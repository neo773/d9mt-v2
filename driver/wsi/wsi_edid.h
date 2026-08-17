#pragma once

#include <vector>
#include <cstdint>
#include <optional>

namespace dxvk::wsi {

  using WsiEdidData = std::vector<uint8_t>;

  struct WsiDisplayMetadata {
    bool supportsST2084;

    float redPrimary[2];
    float greenPrimary[2];
    float bluePrimary[2];
    float whitePoint[2];
    float minLuminance;
    float maxLuminance;
    float maxFullFrameLuminance;
  };

  std::optional<WsiDisplayMetadata> parseColorimetryInfo(
    const WsiEdidData&        edidData);

  inline void NormalizeDisplayMetadata(bool isHDR, wsi::WsiDisplayMetadata& metadata) {
    // Zero luminance means "undefined" per spec and isn't usable for tonemapping;
    // fall back to the same dummy values Windows DXGI uses for broken EDIDs.
    if (metadata.minLuminance == 0.0f)
      metadata.minLuminance = isHDR ? 0.01f : 0.5f;

    if (metadata.maxLuminance == 0.0f)
      metadata.maxLuminance = isHDR ? 1499.0f : 270.0f;

    if (metadata.maxFullFrameLuminance == 0.0f)
      metadata.maxFullFrameLuminance = isHDR ? 799.0f : 270.0f;

    if (metadata.redPrimary[0]   == 0.0f && metadata.redPrimary[1]   == 0.0f
     && metadata.greenPrimary[0] == 0.0f && metadata.greenPrimary[1] == 0.0f
     && metadata.bluePrimary[0]  == 0.0f && metadata.bluePrimary[1]  == 0.0f
     && metadata.whitePoint[0]   == 0.0f && metadata.whitePoint[1]   == 0.0f) {
      if (!isHDR) {
        metadata.redPrimary[0]   = 0.640f;
        metadata.redPrimary[1]   = 0.330f;
        metadata.greenPrimary[0] = 0.300f;
        metadata.greenPrimary[1] = 0.600f;
        metadata.bluePrimary[0]  = 0.150f;
        metadata.bluePrimary[1]  = 0.060f;
        metadata.whitePoint[0]   = 0.3127f;
        metadata.whitePoint[1]   = 0.3290f;
      } else {
        metadata.redPrimary[0]   = 0.680f;
        metadata.redPrimary[1]   = 0.320f;
        metadata.greenPrimary[0] = 0.265f;
        metadata.greenPrimary[1] = 0.690f;
        metadata.bluePrimary[0]  = 0.150f;
        metadata.bluePrimary[1]  = 0.060f;
        metadata.whitePoint[0]   = 0.3127f;
        metadata.whitePoint[1]   = 0.3290f;
      }
    }
  }

}
