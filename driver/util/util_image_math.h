#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../mtl/mtl_types.h"


namespace dxvk::util {

  inline MtlExtent3D computeMipLevelExtent(MtlExtent3D size, uint32_t level) {
    size.width  = std::max(1u, size.width  >> level);
    size.height = std::max(1u, size.height >> level);
    size.depth  = std::max(1u, size.depth  >> level);
    return size;
  }

  inline MtlOrigin computeMipLevelOffset(MtlOrigin offset, uint32_t level) {
    offset.x = offset.x >> level;
    offset.y = offset.y >> level;
    offset.z = offset.z >> level;
    return offset;
  }

  inline MtlOrigin computeBlockOffset(MtlOrigin offset, MtlExtent3D blockSize) {
    return MtlOrigin {
      offset.x / int32_t(blockSize.width),
      offset.y / int32_t(blockSize.height),
      offset.z / int32_t(blockSize.depth) };
  }

  inline MtlExtent3D computeBlockCount(MtlExtent3D extent, MtlExtent3D blockSize) {
    return MtlExtent3D {
      (extent.width  + blockSize.width  - 1u) / blockSize.width,
      (extent.height + blockSize.height - 1u) / blockSize.height,
      (extent.depth  + blockSize.depth  - 1u) / blockSize.depth };
  }

  inline MtlExtent3D computeBlockExtent(MtlExtent3D blockCount, MtlExtent3D blockSize) {
    return MtlExtent3D {
      blockCount.width  * blockSize.width,
      blockCount.height * blockSize.height,
      blockCount.depth  * blockSize.depth };
  }

  inline MtlExtent3D snapExtent3D(MtlOrigin offset, MtlExtent3D extent, MtlExtent3D imageExtent) {
    return MtlExtent3D {
      std::min(extent.width,  imageExtent.width  - uint32_t(offset.x)),
      std::min(extent.height, imageExtent.height - uint32_t(offset.y)),
      std::min(extent.depth,  imageExtent.depth  - uint32_t(offset.z)) };
  }

  inline MtlExtent3D formatBlockSize(const MtlFormatInfo* info) {
    return MtlExtent3D { info ? info->blockWidth : 1u,
                         info ? info->blockHeight : 1u, 1u };
  }

  inline void packImageData(
          void*         dstBytes,
    const void*         srcBytes,
          MtlExtent3D   blockCount,
          MtlDeviceSize blockSize,
          MtlDeviceSize pitchPerRow,
          MtlDeviceSize pitchPerLayer) {
    if (!dstBytes || !srcBytes)
      return;
    auto dst = reinterpret_cast<char*>(dstBytes);
    auto src = reinterpret_cast<const char*>(srcBytes);
    const MtlDeviceSize bytesPerRow   = MtlDeviceSize(blockCount.width) * blockSize;
    for (uint32_t z = 0; z < blockCount.depth; z++) {
      for (uint32_t y = 0; y < blockCount.height; y++) {
        std::memcpy(dst, src + z * pitchPerLayer + y * pitchPerRow, size_t(bytesPerRow));
        dst += bytesPerRow;
      }
    }
  }

  inline MtlColor4 encodeClearBlockValue(MtlFormat , const MtlColor4& color) {
    return color;
  }

}
