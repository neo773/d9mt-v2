#include <cstring>

#include "d3d9_initializer.h"
#include "d3d9_device.h"


namespace dxvk {

  D3D9Initializer::D3D9Initializer(
    D3D9DeviceEx*             pParent)
  : m_parent(pParent),
    m_device(pParent->GetDXVKDevice()),
    m_csChunk(m_parent->AllocCsChunk()) {

  }


  D3D9Initializer::~D3D9Initializer() {

  }


  void D3D9Initializer::NotifyContextFlush() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_transferCommands = 0;
  }


  void D3D9Initializer::InitBuffer(
          D3D9CommonBuffer*  pBuffer) {
    const bool hostVisible =
      pBuffer->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>()->mapPtr(0) != nullptr;

    hostVisible
      ? InitHostVisibleBuffer(pBuffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>())
      : InitDeviceLocalBuffer(pBuffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>());

    if (pBuffer->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      InitHostVisibleBuffer(pBuffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_STAGING>());
  }


  void D3D9Initializer::InitTexture(
          D3D9CommonTexture* pTexture,
          void*              pInitialData) {
    if (pTexture->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_NONE) {
      if (pTexture->GetImage() != nullptr)
        InitDeviceLocalTexture(pTexture);
      return;
    }

    void* mapPtr = nullptr;

    if (pTexture->Desc()->Pool != D3DPOOL_DEFAULT) {
      mapPtr = pTexture->GetData(0);
      if (mapPtr == nullptr)
        throw DxvkError("D3D9: InitTexture: map failed");
    }

    if (pTexture->GetImage() != nullptr)
      InitDeviceLocalTexture(pTexture);

    if (mapPtr != nullptr) {
      InitHostVisibleTexture(pTexture, pInitialData, mapPtr);
      pTexture->UnmapData();
    }

    SyncSharedTexture(pTexture);
  }


  void D3D9Initializer::InitDeviceLocalBuffer(
          MtlBufferSlice     Slice) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    m_transferCommands += 1;

    EmitCs([
      cBuffer = Slice.buffer()
    ] (MtlContext* ctx) {
      ctx->initBuffer(
        cBuffer);
    });

    ThrottleAllocationLocked();
  }


  void D3D9Initializer::InitHostVisibleBuffer(
          MtlBufferSlice     Slice) {
    std::memset(
      Slice.buffer()->mapPtr(Slice.offset()), 0,
      Slice.length());
  }


  void D3D9Initializer::InitDeviceLocalTexture(
          D3D9CommonTexture* pTexture) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    Rc<MtlTexture> image = pTexture->GetImage();

    EmitCs([
      cImage = std::move(image)
    ] (MtlContext* ctx) {
      ctx->initImage(cImage);
    });

    ThrottleAllocationLocked();
  }


  void D3D9Initializer::InitHostVisibleTexture(
          D3D9CommonTexture* pTexture,
          void*              pInitialData,
          void*              mapPtr) {
    if (pInitialData) {
      MtlExtent3D mipExtent = pTexture->GetExtentMip(0);
      const MtlFormatInfo* formatInfo = lookupFormatInfo(pTexture->GetFormatMapping().FormatColor);

      uint32_t blockW = std::max(1u, uint32_t(formatInfo->blockWidth));
      uint32_t blockH = std::max(1u, uint32_t(formatInfo->blockHeight));
      uint32_t blockCountW = (mipExtent.width  + blockW - 1u) / blockW;
      uint32_t blockCountH = (mipExtent.height + blockH - 1u) / blockH;
      uint32_t blockCountD = mipExtent.depth;

      uint32_t pitch = blockCountW * formatInfo->elementSize;
      uint32_t alignedPitch = align(pitch, 4);

      const uint8_t* src = reinterpret_cast<const uint8_t*>(pInitialData);
      uint8_t*       dst = reinterpret_cast<uint8_t*>(mapPtr);
      const uint32_t layers   = pTexture->Desc()->ArraySize;
      const uint32_t srcSlice = pitch * blockCountH;
      const uint32_t dstSlice = alignedPitch * blockCountH;

      for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t z = 0; z < blockCountD; z++) {
          for (uint32_t y = 0; y < blockCountH; y++) {
            std::memcpy(dst + y * alignedPitch, src + y * pitch, pitch);
          }
          src += srcSlice;
          dst += dstSlice;
        }
      }
    } else {
      // All subresources live in one chunk, so subresource 0's pointer covers them all.
      std::memset(
        mapPtr, 0,
        pTexture->GetTotalSize());
    }
  }


  void D3D9Initializer::ThrottleAllocationLocked() {
    if (m_transferCommands > MaxTransferCommands)
      ExecuteFlushLocked();
  }


  void D3D9Initializer::ExecuteFlush() {
    std::lock_guard lock(m_mutex);

    ExecuteFlushLocked();
  }


  void D3D9Initializer::ExecuteFlushLocked() {
    EmitCs([] (MtlContext* ctx) {
      ctx->flushCommandList(nullptr, nullptr);
    });

    FlushCsChunk();

    m_transferCommands = 0;
  }


  void D3D9Initializer::SyncSharedTexture(D3D9CommonTexture* pResource) {
  }


  void D3D9Initializer::FlushCsChunkLocked() {
    m_parent->InjectCsChunk(std::move(m_csChunk), false);
    m_csChunk = m_parent->AllocCsChunk();
  }

}
