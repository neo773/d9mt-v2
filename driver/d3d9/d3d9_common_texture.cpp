#include "d3d9_common_texture.h"

#include "d3d9_util.h"
#include "d3d9_device.h"

#include <algorithm>
#include <cmath>


namespace dxvk {

  static uint32_t MipLevelCount(MtlExtent3D e) {
    uint32_t maxDim = std::max(std::max(e.width, e.height), e.depth);
    uint32_t levels = 1u;
    while (maxDim > 1u) { maxDim >>= 1; levels++; }
    return levels;
  }


  D3D9CommonTexture::D3D9CommonTexture(
          D3D9DeviceEx*             pDevice,
          IUnknown*                 pInterface,
    const D3D9_COMMON_TEXTURE_DESC* pDesc,
          D3DRESOURCETYPE           ResourceType,
          HANDLE*                   pSharedHandle)
    : m_device(pDevice), m_desc(*pDesc), m_type(ResourceType) {
    if (m_desc.Format == D3D9Format::Unknown)
      m_desc.Format = (m_desc.Usage & D3DUSAGE_DEPTHSTENCIL)
                    ? D3D9Format::D24X8
                    : D3D9Format::X8R8G8B8;

    m_exposedMipLevels = m_desc.MipLevels;

    if (m_desc.Usage & D3DUSAGE_AUTOGENMIPMAP)
      m_exposedMipLevels = 1;

    for (uint32_t i = 0; i < m_dirtyBoxes.size(); i++) {
      AddDirtyBox(nullptr, i);
    }

    if (IsPoolManaged(m_desc.Pool)) {
      SetAllNeedUpload();
    }

    m_mapping = pDevice->LookupFormat(m_desc.Format);

    m_mapMode        = DetermineMapMode();
    m_shadow         = DetermineShadowState();
    m_upgradedToD32f = ConvertFormatUnfixed(m_desc.Format).FormatColor != m_mapping.FormatColor &&
                       (m_mapping.FormatColor == MtlFormat::Depth32Float_Stencil8 || m_mapping.FormatColor == MtlFormat::Depth32Float);
    m_supportsFetch4 = DetermineFetch4Compatibility();

    if (TextureUsesImage(&m_desc)) {
      m_image = CreatePrimaryImage(ResourceType, pSharedHandle);

      if (unlikely(m_image == nullptr && (m_desc.Usage & D3DUSAGE_AUTOGENMIPMAP))) {
        m_desc.Usage &= ~D3DUSAGE_AUTOGENMIPMAP;
        m_desc.MipLevels = 1;
        m_image = CreatePrimaryImage(ResourceType, pSharedHandle);
      }

      if (unlikely(m_image == nullptr)) {
        throw DxvkError(str::format(
          "D3D9: Cannot create texture:",
          "\n  Type:    0x", std::hex, ResourceType, std::dec,
          "\n  Format:  ", m_desc.Format,
          "\n  Extent:  ", m_desc.Width,
                      "x", m_desc.Height,
                      "x", m_desc.Depth,
          "\n  Samples: ", m_desc.MultiSample,
          "\n  Layers:  ", m_desc.ArraySize,
          "\n  Levels:  ", m_desc.MipLevels,
          "\n  Usage:   0x", std::hex, m_desc.Usage, std::dec,
          "\n  Pool:    0x", std::hex, m_desc.Pool, std::dec));
      }


      const bool isRT = m_desc.Usage & D3DUSAGE_RENDERTARGET;
      const bool isDS = m_desc.Usage & D3DUSAGE_DEPTHSTENCIL;
      if (!m_desc.IsAttachmentOnly || (!isRT && !isDS))
        CreateSampleView(0);
    }

    for (uint32_t i = 0; i < CountSubresources(); i++) {
      m_memoryOffset[i] = m_totalSize;
      m_totalSize += GetMipSize(i);
    }

    if (m_image != nullptr && !IsManaged()) {
      m_size = m_totalSize;
      if (!m_device->ChangeReportedMemory(-m_size))
        throw DxvkError("D3D9: Reporting out of memory from tracking.");
    }

    // Extra padding here: some titles (e.g. Medieval: Total War 1) read/write slightly OOB on managed textures.
    uint32_t paddedSize = align(m_totalSize + 1, CACHE_LINE_SIZE);

    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)
      m_data = MemoryFileRegion(*m_device->GetAllocator(), paddedSize);
    else if (m_mapMode != D3D9_COMMON_TEXTURE_MAP_MODE_NONE && m_desc.Pool != D3DPOOL_DEFAULT)
      CreateBuffer(false, paddedSize);
  }


  D3D9CommonTexture::~D3D9CommonTexture() {
    if (m_size != 0)
      m_device->ChangeReportedMemory(m_size);

    m_device->RemoveMappedTexture(this);

    if (m_desc.Pool == D3DPOOL_DEFAULT)
      m_device->DecrementLosableCounter();
  }


  MtlSubresourceLayers D3D9CommonTexture::GetSubresourceFromIndex(
          MtlAspectMask         Aspect,
          UINT                  Subresource) const {
    MtlSubresourceLayers result;
    result.aspects        = Aspect;
    result.mipLevel       = Subresource % m_desc.MipLevels;
    result.baseLayer      = Subresource / m_desc.MipLevels;
    result.layerCount     = 1;
    return result;
  }


  HRESULT D3D9CommonTexture::NormalizeTextureProperties(
          D3D9DeviceEx*             pDevice,
          D3DRESOURCETYPE           ResourceType,
          D3D9_COMMON_TEXTURE_DESC* pDesc) {
    auto* options = pDevice->GetOptions();

    auto mapping = pDevice->LookupFormat(pDesc->Format);

    if (pDesc->Format == D3D9Format::A8       &&
       (pDesc->Usage & D3DUSAGE_RENDERTARGET) &&
        options->disableA8RT)
      return D3DERR_INVALIDCALL;

    // Depth-format cube textures are unsupported on modern native drivers and break Gothic 3;
    // older drivers allowed them and some titles (e.g. SimCity Societies) rely on that.
    if (ResourceType == D3DRTYPE_CUBETEXTURE && mapping.Aspect != MtlAspectColor
     && !options->supportCubeDepthFormats)
      return D3DERR_INVALIDCALL;


    if (!mapping.IsValid() && pDesc->Format != D3D9Format::NULL_FORMAT) {
      auto info = pDevice->UnsupportedFormatInfo(pDesc->Format);

      if (pDesc->Pool != D3DPOOL_SCRATCH || info->elementSize == 0)
        return D3DERR_INVALIDCALL;
    }


    if (pDevice->IsExtended() && pDesc->Pool == D3DPOOL_MANAGED)
      return D3DERR_INVALIDCALL;

    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->Depth == 0)
      return D3DERR_INVALIDCALL;

    if (IsDXTFormat(pDesc->Format)) {
      D3D9_FORMAT_BLOCK_SIZE blockSize = GetFormatAlignedBlockSize(pDesc->Format);

      if ((blockSize.Width  && (pDesc->Width  & (blockSize.Width  - 1)))
       || (blockSize.Height && (pDesc->Height & (blockSize.Height - 1))))
        return D3DERR_INVALIDCALL;
    }

    uint32_t sampleCount;
    HRESULT hr = DecodeMultiSampleType(pDesc->MultiSample, pDesc->MultisampleQuality, &sampleCount);
    if (FAILED(hr))
      return hr;

    if (ResourceType == D3DRTYPE_SURFACE && TextureUsesImage(pDesc)) {
      D3DMULTISAMPLE_TYPE d3dSampleCount = D3DMULTISAMPLE_TYPE(sampleCount);
      if (d3dSampleCount == D3DMULTISAMPLE_NONMASKABLE)
        d3dSampleCount = D3DMULTISAMPLE_NONE;

      hr = pDevice->GetAdapter()->CheckDeviceMultiSampleType(
        D3DDEVTYPE_HAL,
        pDesc->Format,
        false,
        d3dSampleCount,
        nullptr
      );
      if (FAILED(hr))
        return hr;
    } else if (sampleCount != 1u) {
      return D3DERR_INVALIDCALL;
    }

    if (IsPoolManaged(pDesc->Pool) && (pDesc->Usage & D3DUSAGE_DYNAMIC))
      return D3DERR_INVALIDCALL;

    if (pDesc->Usage & D3DUSAGE_WRITEONLY)
      return D3DERR_INVALIDCALL;

    constexpr DWORD usageRTOrDS = D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL;

    if (pDesc->Pool != D3DPOOL_DEFAULT && (pDesc->Usage & usageRTOrDS))
      return D3DERR_INVALIDCALL;

    if (pDesc->Pool == D3DPOOL_DEFAULT &&
        (pDesc->Usage & usageRTOrDS) &&
        (pDesc->Usage & D3DUSAGE_DYNAMIC))
      return D3DERR_INVALIDCALL;

    const bool isPlainSurface = ResourceType == D3DRTYPE_SURFACE && !(pDesc->Usage & usageRTOrDS);
    const bool isDepthStencilFormat = IsDepthStencilFormat(pDesc->Format);

    if (!isPlainSurface && pDesc->Pool != D3DPOOL_DEFAULT && isDepthStencilFormat)
      return D3DERR_INVALIDCALL;

    if (( isDepthStencilFormat && (pDesc->Usage & D3DUSAGE_RENDERTARGET)) ||
        (!isDepthStencilFormat && (pDesc->Usage & D3DUSAGE_DEPTHSTENCIL)))
      return D3DERR_INVALIDCALL;

    if (ResourceType == D3DRTYPE_VOLUMETEXTURE &&
        (pDesc->Usage & D3DUSAGE_RENDERTARGET))
      return D3DERR_INVALIDCALL;

    if (ResourceType == D3DRTYPE_VOLUMETEXTURE
      && pDesc->Pool == D3DPOOL_SCRATCH
      && (pDesc->Usage & D3DUSAGE_DYNAMIC))
      return D3DERR_INVALIDCALL;

    if (pDesc->Format == D3D9Format::ATI2
     && (pDesc->Usage & D3DUSAGE_RENDERTARGET ||
        (pDevice->IsExtended() && isPlainSurface && pDesc->Pool != D3DPOOL_SCRATCH)))
      return D3DERR_INVALIDCALL;

    if ((pDesc->Usage & D3DUSAGE_AUTOGENMIPMAP) && ResourceType == D3DRTYPE_SURFACE)
      return D3DERR_INVALIDCALL;

    const uint32_t maxMipLevelCount = pDesc->MultiSample <= D3DMULTISAMPLE_NONMASKABLE
      ? MipLevelCount({ pDesc->Width, pDesc->Height, pDesc->Depth })
      : 1u;

    if (pDesc->Usage & D3DUSAGE_AUTOGENMIPMAP)
      pDesc->MipLevels = 0;

    if (pDesc->MipLevels == 0 || pDesc->MipLevels > maxMipLevelCount)
      pDesc->MipLevels = maxMipLevelCount;

    if (unlikely(pDesc->Discard)) {
      if (!isDepthStencilFormat)
        return D3DERR_INVALIDCALL;

      if (pDesc->Format == D3D9Format::D32_LOCKABLE
       || pDesc->Format == D3D9Format::D32F_LOCKABLE
       || pDesc->Format == D3D9Format::D16_LOCKABLE
       || pDesc->Format == D3D9Format::S8_LOCKABLE)
        return D3DERR_INVALIDCALL;
    }

    if (pDesc->IsLockable && sampleCount > 1u)
        return D3DERR_INVALIDCALL;

    return D3D_OK;
  }


  void* D3D9CommonTexture::GetData(UINT Subresource) {
    if (unlikely(m_buffer))
      return m_buffer->mapPtr(m_memoryOffset[Subresource]);

    uint8_t* ptr = reinterpret_cast<uint8_t*>(m_data.map());

    if (!ptr)
      return nullptr;

    ptr += m_memoryOffset[Subresource];
    return ptr;
  }


  void D3D9CommonTexture::CreateBuffer(bool Initialize, uint32_t Size) {
    if (likely(m_buffer != nullptr))
      return;

    MtlBufferCreateInfo info;
    info.size      = Size;
    info.usage     = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Read;


    m_buffer = m_device->GetDXVKDevice()->createBuffer(info);

    if (Initialize) {
      if (m_data)
        std::memcpy(m_buffer->mapPtr(0), m_data.map(), m_totalSize);
      else
        std::memset(m_buffer->mapPtr(0), 0, m_totalSize);
    }
    m_data = {};
  }


  MtlDeviceSize D3D9CommonTexture::GetMipSize(UINT Subresource) const {
    const UINT MipLevel = Subresource % m_desc.MipLevels;

    const MtlFormatInfo* formatInfo = m_mapping.FormatColor != MtlFormat::Invalid
      ? lookupFormatInfo(m_mapping.FormatColor)
      : m_device->UnsupportedFormatInfo(m_desc.Format);

    const MtlExtent3D mipExtent = GetExtentMip(Subresource);

    uint32_t blockW = std::max(1u, uint32_t(formatInfo->blockWidth));
    uint32_t blockH = std::max(1u, uint32_t(formatInfo->blockHeight));
    uint32_t elementSize = formatInfo->elementSize;

    const uint32_t blockCountW = (mipExtent.width  + blockW - 1u) / blockW;
    const uint32_t blockCountH = (mipExtent.height + blockH - 1u) / blockH;
    const uint32_t blockCountD = mipExtent.depth;

    (void) MipLevel;

    return std::min(GetPlaneCount(), 2u)
         * align(elementSize * blockCountW, 4)
         * blockCountH
         * blockCountD;
  }


  Rc<MtlTexture> D3D9CommonTexture::CreatePrimaryImage(D3DRESOURCETYPE ResourceType, HANDLE* pSharedHandle) const {
    MtlImageCreateInfo imageInfo;
    imageInfo.type            = GetImageTypeFromResourceType(ResourceType);
    imageInfo.format          = m_mapping.ConversionFormatInfo.FormatColor != MtlFormat::Invalid
                              ? m_mapping.ConversionFormatInfo.FormatColor
                              : m_mapping.FormatColor;
    imageInfo.extent.width    = m_desc.Width;
    imageInfo.extent.height   = m_desc.Height;
    imageInfo.extent.depth    = m_desc.Depth;
    imageInfo.arrayLayers     = m_desc.ArraySize;
    imageInfo.mipLevels       = m_desc.MipLevels;
    imageInfo.sampleCount     = 1u;
    imageInfo.usage           = MtlUsage::None;
    imageInfo.cube            = ResourceType == D3DRTYPE_CUBETEXTURE;

    DecodeMultiSampleType(m_desc.MultiSample, m_desc.MultisampleQuality, &imageInfo.sampleCount);

    const bool isRT = m_desc.Usage & D3DUSAGE_RENDERTARGET;
    const bool isDS = m_desc.Usage & D3DUSAGE_DEPTHSTENCIL;
    const bool isAutoGen = m_desc.Usage & D3DUSAGE_AUTOGENMIPMAP;

    if (!m_desc.IsAttachmentOnly || (!isRT && !isDS))
      imageInfo.usage = imageInfo.usage | MtlUsage::Sampled;

    if (isRT || isAutoGen)
      imageInfo.usage = imageInfo.usage | MtlUsage::RenderTarget;

    if (isDS)
      imageInfo.usage = imageInfo.usage | MtlUsage::DepthStencil;

    if (m_desc.ImageUsage & MtlTextureUsageRenderTarget)
      imageInfo.usage = imageInfo.usage | MtlUsage::RenderTarget;
    if (m_desc.ImageUsage & MtlTextureUsageShaderRead)
      imageInfo.usage = imageInfo.usage | MtlUsage::Sampled;

    if (!CheckImageSupport(&imageInfo))
      return nullptr;

    return m_device->GetDXVKDevice()->createImage(imageInfo);
  }


  Rc<MtlTexture> D3D9CommonTexture::CreateResolveImage() const {
    MtlImageCreateInfo imageInfo;
    imageInfo.type            = GetImageTypeFromResourceType(m_type);
    imageInfo.format          = m_mapping.ConversionFormatInfo.FormatColor != MtlFormat::Invalid
                              ? m_mapping.ConversionFormatInfo.FormatColor
                              : m_mapping.FormatColor;
    imageInfo.extent.width    = m_desc.Width;
    imageInfo.extent.height   = m_desc.Height;
    imageInfo.extent.depth    = m_desc.Depth;
    imageInfo.arrayLayers     = m_desc.ArraySize;
    imageInfo.mipLevels       = m_desc.MipLevels;
    imageInfo.sampleCount     = 1u;
    imageInfo.cube            = m_type == D3DRTYPE_CUBETEXTURE;

    const bool isRT = m_desc.Usage & D3DUSAGE_RENDERTARGET;
    const bool isDS = m_desc.Usage & D3DUSAGE_DEPTHSTENCIL;
    imageInfo.usage = MtlUsage::Sampled;
    if (isRT) imageInfo.usage = imageInfo.usage | MtlUsage::RenderTarget;
    if (isDS) imageInfo.usage = imageInfo.usage | MtlUsage::DepthStencil;

    return m_device->GetDXVKDevice()->createImage(imageInfo);
  }


  BOOL D3D9CommonTexture::DetermineShadowState() const {
    constexpr std::array<D3D9Format, 3> blacklist = {
      D3D9Format::INTZ, D3D9Format::DF16, D3D9Format::DF24
    };

    return IsDepthFormat(m_desc.Format)
        && std::find(blacklist.begin(), blacklist.end(), m_desc.Format) == blacklist.end();
  }


  BOOL D3D9CommonTexture::DetermineFetch4Compatibility() const {
    constexpr std::array<D3D9Format, 8> singleChannelFormats = {
      D3D9Format::INTZ, D3D9Format::DF16, D3D9Format::DF24,
      D3D9Format::R16F, D3D9Format::R32F, D3D9Format::A8,
      D3D9Format::L8, D3D9Format::L16
    };

    return std::find(singleChannelFormats.begin(), singleChannelFormats.end(), m_desc.Format) != singleChannelFormats.end();
  }


  BOOL D3D9CommonTexture::CheckImageSupport(
    const MtlImageCreateInfo*  pImageInfo) const {
    if (!m_device->GetDXVKDevice()->supportsFormat(pImageInfo->format, pImageInfo->usage))
      return FALSE;

    const auto& info = m_device->GetDXVKDevice()->deviceInfo();
    const uint32_t maxDim = info.maxTextureSize2D;

    return (pImageInfo->extent.width  <= maxDim)
        && (pImageInfo->extent.height <= maxDim)
        && (pImageInfo->extent.depth  <= maxDim);
  }


  MtlImageType D3D9CommonTexture::GetImageTypeFromResourceType(D3DRESOURCETYPE Type) {
    switch (Type) {
      case D3DRTYPE_SURFACE:
      case D3DRTYPE_TEXTURE:       return MtlImageType::e2D;
      case D3DRTYPE_VOLUMETEXTURE: return MtlImageType::e3D;
      case D3DRTYPE_CUBETEXTURE:   return MtlImageType::e2D;
      default: throw DxvkError("D3D9CommonTexture: Unhandled resource type");
    }
  }


  MtlTextureViewType D3D9CommonTexture::GetImageViewTypeFromResourceType(
          D3DRESOURCETYPE  Dimension,
          UINT             Layer) {
    switch (Dimension) {
      case D3DRTYPE_SURFACE:
      case D3DRTYPE_TEXTURE:       return MtlTextureViewType::e2D;
      case D3DRTYPE_VOLUMETEXTURE: return MtlTextureViewType::e3D;
      case D3DRTYPE_CUBETEXTURE:   return Layer == AllLayers
                                        ? MtlTextureViewType::eCube
                                        : MtlTextureViewType::e2D;
      default: throw DxvkError("D3D9CommonTexture: Unhandled resource type");
    }
  }


  D3D9_COMMON_TEXTURE_MAP_MODE D3D9CommonTexture::DetermineMapMode() const {
    if (m_desc.Format == D3D9Format::NULL_FORMAT)
      return D3D9_COMMON_TEXTURE_MAP_MODE_NONE;

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    if (m_device->GetOptions()->textureMemory != 0 && m_desc.Pool != D3DPOOL_DEFAULT)
      return D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE;
#endif

    if (m_desc.Pool == D3DPOOL_SYSTEMMEM || m_desc.Pool == D3DPOOL_SCRATCH)
      return D3D9_COMMON_TEXTURE_MAP_MODE_SYSTEMMEM;

    return D3D9_COMMON_TEXTURE_MAP_MODE_BACKED;
  }


  Rc<MtlTextureView> D3D9CommonTexture::CreateView(
          UINT                   Layer,
          UINT                   Lod,
          MtlTextureUsage        UsageFlags,
          bool                   Srgb) {
    MtlImageViewKey viewInfo;
    viewInfo.format    = m_mapping.ConversionFormatInfo.FormatColor != MtlFormat::Invalid
                       ? PickSRGB(m_mapping.ConversionFormatInfo.FormatColor, m_mapping.ConversionFormatInfo.FormatSrgb, Srgb)
                       : PickSRGB(m_mapping.FormatColor, m_mapping.FormatSrgb, Srgb);
    viewInfo.aspects   = lookupFormatInfo(viewInfo.format)->aspects;
    viewInfo.usage     = UsageFlags;
    viewInfo.baseMip   = Lod;
    viewInfo.mipCount  = m_desc.MipLevels - Lod;
    viewInfo.baseLayer = Layer == AllLayers ? 0 : Layer;
    viewInfo.layerCount = Layer == AllLayers ? m_desc.ArraySize : 1;
    viewInfo.swizzle   = m_mapping.Swizzle;


    if (!(UsageFlags & MtlTextureUsageRenderTarget))
      viewInfo.aspects &= ~MtlAspectStencil;

    if (UsageFlags & MtlTextureUsageRenderTarget)
      viewInfo.mipCount = 1;

    return m_device->GetDXVKDevice()->createImageView(GetImage(), viewInfo);
  }


  void D3D9CommonTexture::PreLoadAll() {
    if (!IsManaged())
      return;

    auto lock = m_device->LockDevice();
    m_device->UploadManagedTexture(this);
    m_device->MarkTextureUploaded(this);
  }


  void D3D9CommonTexture::PreLoadSubresource(UINT Subresource) {
    if (IsManaged()) {
      auto lock = m_device->LockDevice();

      if (NeedsUpload(Subresource)) {
        m_device->FlushImage(this, Subresource);
        SetNeedsUpload(Subresource, false);

        if (!NeedsAnyUpload())
          m_device->MarkTextureUploaded(this);
      }
    }
  }


  void D3D9CommonTexture::CreateSampleView(UINT Lod) {
    if (unlikely(m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_SYSTEMMEM))
      return;

    m_sampleView.Color = CreateView(AllLayers, Lod,
      MtlTextureUsageShaderRead, false);

    if (IsSrgbCompatible()) {
      m_sampleView.Srgb = CreateView(AllLayers, Lod,
        MtlTextureUsageShaderRead, true);
    }
  }


  const Rc<MtlBuffer>& D3D9CommonTexture::GetBuffer() {
    return m_buffer;
  }


  MtlBufferSlice D3D9CommonTexture::GetBufferSlice(UINT Subresource) {
    return MtlBufferSlice(GetBuffer(), m_memoryOffset[Subresource], GetMipSize(Subresource));
  }


  uint32_t D3D9CommonTexture::GetPlaneCount() const {
    return 1u;
  }

}
