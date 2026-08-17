#pragma once

#include "d3d9_format.h"
#include "d3d9_util.h"
#include "d3d9_caps.h"

#include "../mtl/mtl_device.h"

#include "../util/util_bit.h"
#include "../util/util_unmap.h"

namespace dxvk {

  class D3D9DeviceEx;

  enum D3D9_COMMON_TEXTURE_MAP_MODE {
    D3D9_COMMON_TEXTURE_MAP_MODE_NONE,
    D3D9_COMMON_TEXTURE_MAP_MODE_BACKED,
    D3D9_COMMON_TEXTURE_MAP_MODE_SYSTEMMEM,
    D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE,
  };

  struct D3D9_COMMON_TEXTURE_DESC {
    UINT                Width;
    UINT                Height;
    UINT                Depth;
    UINT                ArraySize;
    UINT                MipLevels;
    DWORD               Usage;
    D3D9Format          Format;
    D3DPOOL             Pool;
    D3DMULTISAMPLE_TYPE MultiSample;
    DWORD               MultisampleQuality;
    bool                Discard;
    bool                IsBackBuffer;
    bool                IsAttachmentOnly;
    bool                IsLockable;

    MtlTextureUsage     ImageUsage = 0;
  };

  struct D3D9ColorView {
    inline Rc<MtlTextureView>& Pick(bool Srgb) {
      return Srgb ? this->Srgb : this->Color;
    }

    inline const Rc<MtlTextureView>& Pick(bool Srgb) const {
      return Srgb ? this->Srgb : this->Color;
    }

    Rc<MtlTextureView> Color;
    Rc<MtlTextureView> Srgb;
  };

  template <typename T>
  using D3D9SubresourceArray = std::array<T, caps::MaxSubresources>;

  using D3D9SubresourceBitset = bit::bitset<caps::MaxSubresources>;

  class D3D9CommonTexture {

  public:

    static constexpr UINT AllLayers = std::numeric_limits<uint32_t>::max();

    D3D9CommonTexture(
            D3D9DeviceEx*             pDevice,
            IUnknown*                 pInterface,
      const D3D9_COMMON_TEXTURE_DESC* pDesc,
            D3DRESOURCETYPE           ResourceType,
            HANDLE*                   pSharedHandle);

    ~D3D9CommonTexture();

    D3D9DeviceEx* Device() const {
      return m_device;
    }

    const D3D9_COMMON_TEXTURE_DESC* Desc() const {
      return &m_desc;
    }

    const D3D9_MTL_FORMAT_MAPPING& GetFormatMapping() const {
      return m_mapping;
    }

    UINT CountSubresources() const {
      return m_desc.ArraySize * m_desc.MipLevels;
    }

    D3D9_COMMON_TEXTURE_MAP_MODE GetMapMode() const {
      return m_mapMode;
    }

    const Rc<MtlTexture>& GetImage() const {
      return m_image;
    }

    const Rc<MtlTexture>& GetResolveImage() {
      if (unlikely(m_resolveImage == nullptr))
        m_resolveImage = CreateResolveImage();

      return m_resolveImage;
    }

    void* GetData(UINT Subresource);

    const Rc<MtlBuffer>& GetBuffer();

    MtlBufferSlice GetBufferSlice(UINT Subresource);

    MtlSubresourceLayers GetSubresourceFromIndex(
            MtlAspectMask         Aspect,
            UINT                  Subresource) const;

    static HRESULT NormalizeTextureProperties(
            D3D9DeviceEx*              pDevice,
            D3DRESOURCETYPE            ResourceType,
            D3D9_COMMON_TEXTURE_DESC*  pDesc);

    static bool TextureUsesImage(const D3D9_COMMON_TEXTURE_DESC* pDesc) {
      return pDesc->Pool   != D3DPOOL_SYSTEMMEM
          && pDesc->Pool   != D3DPOOL_SCRATCH
          && pDesc->Format != D3D9Format::NULL_FORMAT;
    }

    bool IsShadow() const {
      return m_shadow;
    }

    bool IsCube() const {
      return m_type == D3DRTYPE_CUBETEXTURE;
    }

    bool IsUpgradedToD32f() const {
      return m_upgradedToD32f;
    }

    bool SupportsFetch4() const {
      return m_supportsFetch4;
    }

    bool IsNull() const {
      return m_desc.Format == D3D9Format::NULL_FORMAT;
    }

    UINT CalcSubresource(UINT Face, UINT MipLevel) const {
      return Face * m_desc.MipLevels + MipLevel;
    }

    void UnmapData() {
      m_data.unmap();
    }

    void DestroyBuffer() {
      m_buffer = nullptr;
      MarkAllNeedReadback();
    }

    bool IsDynamic() const {
      return m_desc.Usage & D3DUSAGE_DYNAMIC;
    }

    bool IsManaged() const {
      return IsPoolManaged(m_desc.Pool);
    }

    bool IsRenderTarget() const {
      return m_desc.Usage & D3DUSAGE_RENDERTARGET;
    }

    bool IsDepthStencil() const {
      return m_desc.Usage & D3DUSAGE_DEPTHSTENCIL;
    }

    bool IsAutomaticMip() const {
      return m_desc.Usage & D3DUSAGE_AUTOGENMIPMAP;
    }

    bool IsSrgbCompatible() const {
      return m_mapping.FormatSrgb != MtlFormat::Invalid;
    }

    void CreateSampleView(UINT Lod);

    MtlExtent3D GetExtent() const {
      return MtlExtent3D{ m_desc.Width, m_desc.Height, m_desc.Depth };
    }

    MtlExtent3D GetExtentMip(UINT Subresource) const {
      UINT MipLevel = Subresource % m_desc.MipLevels;
      MtlExtent3D e = GetExtent();
      return MtlExtent3D{
        std::max(1u, e.width  >> MipLevel),
        std::max(1u, e.height >> MipLevel),
        std::max(1u, e.depth  >> MipLevel) };
    }

    bool MarkTransitionedToHazardLayout() {
      return std::exchange(m_transitionedToHazardLayout, true);
    }

    bool HasBeenTransitionedToHazardLayout() const {
      return m_transitionedToHazardLayout;
    }

    D3DRESOURCETYPE GetType() const {
      return m_type;
    }

    uint32_t GetPlaneCount() const;

    D3DPOOL GetPool() const { return m_desc.Pool; }

    const D3D9_MTL_FORMAT_MAPPING& GetMapping() { return m_mapping; }

    void SetLocked(UINT Subresource, bool value) { m_locked.set(Subresource, value); }

    bool GetLocked(UINT Subresource) const { return m_locked.get(Subresource); }

    bool IsAnySubresourceLocked() const { return m_locked.any(); }

    void SetNeedsReadback(UINT Subresource, bool value) { m_needsReadback.set(Subresource, value); }

    bool NeedsReadback(UINT Subresource) const { return m_needsReadback.get(Subresource); }

    void MarkAllNeedReadback() { m_needsReadback.setAll(); }

    const Rc<MtlTextureView>& GetSampleView(bool srgb) const {
      return m_sampleView.Pick(srgb && IsSrgbCompatible());
    }

    Rc<MtlTextureView> CreateView(
            UINT                   Layer,
            UINT                   Lod,
            MtlTextureUsage        UsageFlags,
            bool                   Srgb);
    D3D9SubresourceBitset& GetUploadBitmask() { return m_needsUpload; }

    void SetAllNeedUpload() {
      if (likely(!IsAutomaticMip())) {
        m_needsUpload.setAll();
      } else {
        for (uint32_t a = 0; a < m_desc.ArraySize; a++) {
          for (uint32_t m = 0; m < ExposedMipLevels(); m++) {
            SetNeedsUpload(CalcSubresource(a, m), true);
          }
        }
      }
    }
    void SetNeedsUpload(UINT Subresource, bool upload) { m_needsUpload.set(Subresource, upload); }
    bool NeedsUpload(UINT Subresource) const { return m_needsUpload.get(Subresource); }
    bool NeedsAnyUpload() { return m_needsUpload.any(); }
    void ClearNeedsUpload() { return m_needsUpload.clearAll();  }

    void SetNeedsMipGen(bool value) { m_needsMipGen = value; }
    bool NeedsMipGen() const { return m_needsMipGen; }

    DWORD ExposedMipLevels() const { return m_exposedMipLevels; }

    void SetMipFilter(D3DTEXTUREFILTERTYPE filter) { m_mipFilter = filter; }
    D3DTEXTUREFILTERTYPE GetMipFilter() const { return m_mipFilter; }

    void PreLoadAll();
    void PreLoadSubresource(UINT Subresource);

    void AddDirtyBox(CONST D3DBOX* pDirtyBox, uint32_t layer) {
      if (pDirtyBox) {
        D3DBOX box = *pDirtyBox;
        if (box.Right <= box.Left
          || box.Bottom <= box.Top
          || box.Back <= box.Front)
          return;

        box.Right = std::min(box.Right, m_desc.Width);
        box.Bottom = std::min(box.Bottom, m_desc.Height);
        box.Back = std::min(box.Back, m_desc.Depth);

        D3DBOX& dirtyBox = m_dirtyBoxes[layer];
        if (dirtyBox.Left == dirtyBox.Right) {
          dirtyBox = box;
        } else {
          dirtyBox.Left    = std::min(dirtyBox.Left,   box.Left);
          dirtyBox.Right   = std::max(dirtyBox.Right,  box.Right);
          dirtyBox.Top     = std::min(dirtyBox.Top,    box.Top);
          dirtyBox.Bottom  = std::max(dirtyBox.Bottom, box.Bottom);
          dirtyBox.Front   = std::min(dirtyBox.Front,  box.Front);
          dirtyBox.Back    = std::max(dirtyBox.Back,   box.Back);
        }
      } else {
        m_dirtyBoxes[layer] = { 0, 0, m_desc.Width, m_desc.Height, 0, m_desc.Depth };
      }
    }

    void ClearDirtyBoxes() {
      for (uint32_t i = 0; i < m_dirtyBoxes.size(); i++) {
        m_dirtyBoxes[i] = { 0, 0, 0, 0, 0, 0 };
      }
    }

    const D3DBOX& GetDirtyBox(uint32_t layer) const {
      return m_dirtyBoxes[layer];
    }

    static MtlImageType GetImageTypeFromResourceType(
            D3DRESOURCETYPE  Dimension);

    static MtlTextureViewType GetImageViewTypeFromResourceType(
            D3DRESOURCETYPE  Dimension,
            UINT             Layer);

    void TrackMappingBufferSequenceNumber(UINT Subresource, uint64_t Seq) {
      if (Subresource < m_seqs.size())
        m_seqs[Subresource] = Seq;
    }

    uint64_t GetMappingBufferSequenceNumber(UINT Subresource) {
      return Subresource < m_seqs.size()
        ? m_seqs[Subresource]
        : 0ull;
    }

    MtlDeviceSize GetMipSize(UINT Subresource) const;

    uint32_t GetTotalSize() const {
      return m_totalSize;
    }

    void CreateBuffer(bool Initialize, uint32_t Size);

  private:

    D3D9DeviceEx*                 m_device;
    D3D9_COMMON_TEXTURE_DESC      m_desc;
    D3DRESOURCETYPE               m_type;
    D3D9_COMMON_TEXTURE_MAP_MODE  m_mapMode;

    Rc<MtlTexture>                m_image;
    Rc<MtlTexture>                m_resolveImage;
    Rc<MtlBuffer>                 m_buffer;
    MemoryFileRegion              m_data = { };

    D3D9SubresourceArray<
      uint64_t>                   m_seqs = { };

    D3D9SubresourceArray<
      uint32_t>                   m_memoryOffset = { };

    uint32_t                      m_totalSize = 0;

    D3D9_MTL_FORMAT_MAPPING       m_mapping;

    bool                          m_shadow;
    bool                          m_upgradedToD32f;
    bool                          m_supportsFetch4;

    int64_t                       m_size = 0;

    bool                          m_transitionedToHazardLayout = false;

    D3D9ColorView                 m_sampleView;

    D3D9SubresourceBitset         m_locked = { };

    D3D9SubresourceBitset         m_needsReadback = { };

    D3D9SubresourceBitset         m_needsUpload = { };

    DWORD                         m_exposedMipLevels = 0;

    bool                          m_needsMipGen = false;

    D3DTEXTUREFILTERTYPE          m_mipFilter = D3DTEXF_LINEAR;

    std::array<D3DBOX, 6>         m_dirtyBoxes;

    Rc<MtlTexture> CreatePrimaryImage(D3DRESOURCETYPE ResourceType, HANDLE* pSharedHandle) const;

    Rc<MtlTexture> CreateResolveImage() const;

    BOOL DetermineShadowState() const;

    BOOL DetermineFetch4Compatibility() const;

    BOOL CheckImageSupport(
      const MtlImageCreateInfo*  pImageInfo) const;

    D3D9_COMMON_TEXTURE_MAP_MODE DetermineMapMode() const;

  };

}
