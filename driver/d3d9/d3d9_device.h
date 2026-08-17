#pragma once

#include "../mtl/mtl_device.h"
#include "../cs/dxvk_cs.h"

#include "d3d9_include.h"
#include "d3d9_cursor.h"
#include "d3d9_format.h"
#include "d3d9_multithread.h"
#include "d3d9_adapter.h"
#include "d3d9_constant_buffer.h"
#include "d3d9_constant_copy.h"
#include "d3d9_constant_set.h"

#include "d3d9_state.h"

#include "d3d9_options.h"

#include "d3d9_fixed_function.h"
#include "d3d9_swvp_emu.h"


#include <cstdint>
#include <unordered_set>
#include "d3d9_bridge.h"

#include <vector>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "../util/util_flush.h"
#include "../util/util_lru.h"
#include "../util/util_unmap.h"
#include "../util/sync/sync_signal.h"

namespace dxvk {

  class D3D9InterfaceEx;
  class D3D9SwapChainEx;
  class D3D9CommonTexture;
  class D3D9CommonBuffer;
  class D3D9CommonShader;
  class D3D9ShaderModuleSet;
  class D3D9Initializer;
  class D3D9Query;
  class D3D9StateBlock;
  class D3D9FormatHelper;
  class D3D9UserDefinedAnnotation;

  enum class D3D9CmdType : uint32_t {
    None,
    Draw,
    DrawIndexed,
  };

  enum class D3D9DeviceDirtyFlag : uint32_t {
    Framebuffer,
    ClipPlanes,
    DepthStencilState,
    BlendState,
    RasterizerState,
    DepthBias,
    AlphaTestState,
    InputLayout,
    ViewportScissor,
    MultiSampleState,
    VertexBuffers,
    IndexBuffer,
    Fog,
    FFVertexData,
    FFVertexBlend,
    FFVertexShader,
    FFPixelShader,
    FFViewport,
    FFPixelData,
    SharedPixelShaderData,
    DepthBounds,
    PointScale,

    PushDataShared,
    PushDataVs,
    PushDataFfvs,
    PushDataFfps,

    SpecializationEntries,
  };

  using D3D9DeviceDirtyFlags = Flags<D3D9DeviceDirtyFlag>;

  enum class D3D9DeviceLostState {
    Ok = 0,
    Lost = 1,
    NotReset = 2,
  };

  struct D3D9DrawInfo {
    uint32_t vertexCount;
    uint32_t instanceCount;
  };

  struct D3D9BufferSlice {
    MtlBufferSlice slice = {};
    void*          mapPtr = nullptr;
  };

  struct D3D9TextureSlotTracking {

    uint32_t depth = 0;

    uint32_t drefClamp = 0;

    uint32_t textureType = 0;

    uint32_t projected = 0;

    uint32_t samplerStateDirty = 0;

    uint32_t fetch4SamplerState = 0;

    uint32_t fetch4 = 0;

    uint32_t textureDirty = 0;

    uint32_t rtUsage = 0;

    uint32_t dsUsage = 0;

    uint32_t unresolvableHazardRT = 0;

    uint32_t unresolvableHazardDS = 0;

    uint32_t hazardRT = 0;

    uint32_t hazardDS = 0;

    uint32_t bound = 0;

    uint32_t needsUpload = 0;

    uint32_t needsMipGen = 0;

    uint32_t ffpsTextures = 0u;
  };

  struct D3D9RTSlotTracking {

    uint8_t canBeSampled = 0;

    uint8_t hasAlphaSwizzle = 0;
  };

  struct D3D9VBSlotTracking {

    uint16_t bound = 0;

    uint16_t needsUpload = 0;

    uint16_t uploadPerDraw = 0;

    uint16_t instanced = 0;
  };

  class D3D9DeviceEx final : public ComObjectClamp<IDirect3DDevice9Ex> {
    constexpr static uint32_t DefaultFrameLatency = 3;
    constexpr static uint32_t MaxFrameLatency     = 20;

    constexpr static uint32_t MinFlushIntervalUs = 750;
    constexpr static uint32_t IncFlushIntervalUs = 250;
    constexpr static uint32_t MaxPendingSubmits = 6;

    constexpr static uint32_t NullStreamIdx = caps::MaxStreams;

    constexpr static MtlDeviceSize StagingBufferSize = 4ull << 20;

    friend class D3D9SwapChainEx;
    friend struct D3D9WindowContext;
    friend class D3D9ConstantBuffer;
    friend class D3D9UserDefinedAnnotation;
    friend class DxvkD3D8Bridge;

    using CbvIndex = D3D9ShaderResourceMapping::CbvIndex;
  public:

    D3D9DeviceEx(
            D3D9InterfaceEx*       pParent,
            D3D9Adapter*           pAdapter,
            D3DDEVTYPE             DeviceType,
            HWND                   hFocusWindow,
            DWORD                  BehaviorFlags,
            Rc<MtlDevice>          dxvkDevice);

    ~D3D9DeviceEx();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel();

    UINT    STDMETHODCALLTYPE GetAvailableTextureMem();

    HRESULT STDMETHODCALLTYPE EvictManagedResources();

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9);

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps);

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode);

    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters);

    HRESULT STDMETHODCALLTYPE SetCursorProperties(
            UINT               XHotSpot,
            UINT               YHotSpot,
            IDirect3DSurface9* pCursorBitmap);

    void    STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags);

    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL bShow);

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPresentationParameters,
            IDirect3DSwapChain9**  ppSwapChain);

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain);

    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains();

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters);

    HRESULT STDMETHODCALLTYPE Present(
      const RECT* pSourceRect,
      const RECT* pDestRect, HWND hDestWindowOverride,
      const RGNDATA* pDirtyRegion);

    HRESULT STDMETHODCALLTYPE GetBackBuffer(
      UINT iSwapChain,
      UINT iBackBuffer,
      D3DBACKBUFFER_TYPE Type,
      IDirect3DSurface9** ppBackBuffer);

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus);

    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs);

    void    STDMETHODCALLTYPE SetGammaRamp(
      UINT iSwapChain,
      DWORD Flags,
      const D3DGAMMARAMP* pRamp);

    void    STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp);

    HRESULT STDMETHODCALLTYPE CreateTexture(
            UINT                Width,
            UINT                Height,
            UINT                Levels,
            DWORD               Usage,
            D3DFORMAT           Format,
            D3DPOOL             Pool,
            IDirect3DTexture9** ppTexture,
            HANDLE*             pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
            UINT                      Width,
            UINT                      Height,
            UINT                      Depth,
            UINT                      Levels,
            DWORD                     Usage,
            D3DFORMAT                 Format,
            D3DPOOL                   Pool,
            IDirect3DVolumeTexture9** ppVolumeTexture,
            HANDLE*                   pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(
          UINT                      EdgeLength,
            UINT                    Levels,
            DWORD                   Usage,
            D3DFORMAT               Format,
            D3DPOOL                 Pool,
            IDirect3DCubeTexture9** ppCubeTexture,
            HANDLE*                 pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
            UINT                     Length,
            DWORD                    Usage,
            DWORD                    FVF,
            D3DPOOL                  Pool,
            IDirect3DVertexBuffer9** ppVertexBuffer,
            HANDLE*                  pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
            UINT                    Length,
            DWORD                   Usage,
            D3DFORMAT               Format,
            D3DPOOL                 Pool,
            IDirect3DIndexBuffer9** ppIndexBuffer,
            HANDLE*                 pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(
            UINT                Width,
            UINT                Height,
            D3DFORMAT           Format,
            D3DMULTISAMPLE_TYPE MultiSample,
            DWORD               MultisampleQuality,
            BOOL                Lockable,
            IDirect3DSurface9** ppSurface,
            HANDLE*             pSharedHandle);

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
            UINT                Width,
            UINT                Height,
            D3DFORMAT           Format,
            D3DMULTISAMPLE_TYPE MultiSample,
            DWORD               MultisampleQuality,
            BOOL                Discard,
            IDirect3DSurface9** ppSurface,
            HANDLE*             pSharedHandle);

    HRESULT STDMETHODCALLTYPE UpdateSurface(
            IDirect3DSurface9* pSourceSurface,
      const RECT*              pSourceRect,
            IDirect3DSurface9* pDestinationSurface,
      const POINT*             pDestPoint);

    HRESULT STDMETHODCALLTYPE UpdateTexture(
            IDirect3DBaseTexture9* pSourceTexture,
            IDirect3DBaseTexture9* pDestinationTexture);

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(
            IDirect3DSurface9* pRenderTarget,
            IDirect3DSurface9* pDestSurface);

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface);

    HRESULT STDMETHODCALLTYPE StretchRect(
            IDirect3DSurface9*   pSourceSurface,
      const RECT*                pSourceRect,
            IDirect3DSurface9*   pDestSurface,
      const RECT*                pDestRect,
            D3DTEXTUREFILTERTYPE Filter);

    HRESULT STDMETHODCALLTYPE ColorFill(
            IDirect3DSurface9* pSurface,
      const RECT*              pRect,
            D3DCOLOR           Color);

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(
      UINT Width,
      UINT Height,
      D3DFORMAT Format,
      D3DPOOL Pool,
      IDirect3DSurface9** ppSurface,
      HANDLE* pSharedHandle);

    HRESULT STDMETHODCALLTYPE SetRenderTarget(
            DWORD              RenderTargetIndex,
            IDirect3DSurface9* pRenderTarget);

    HRESULT STDMETHODCALLTYPE GetRenderTarget(
            DWORD               RenderTargetIndex,
            IDirect3DSurface9** ppRenderTarget);

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil);

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface);

    HRESULT STDMETHODCALLTYPE BeginScene();

    HRESULT STDMETHODCALLTYPE EndScene();

    HRESULT STDMETHODCALLTYPE Clear(
            DWORD    Count,
      const D3DRECT* pRects,
            DWORD    Flags,
            D3DCOLOR Color,
            float    Z,
            DWORD    Stencil);

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix);

    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix);

    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX* pMatrix);

    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pViewport);

    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport);

    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pMaterial);

    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial);

    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9* pLight);

    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9* pLight);

    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable);

    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable);

    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float* pPlane);

    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane);

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value);

    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue);

    HRESULT STDMETHODCALLTYPE CreateStateBlock(
            D3DSTATEBLOCKTYPE      Type,
            IDirect3DStateBlock9** ppSB);

    HRESULT STDMETHODCALLTYPE BeginStateBlock();

    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB);

    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* pClipStatus);

    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus);

    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture);

    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture);

    HRESULT STDMETHODCALLTYPE GetTextureStageState(
            DWORD                    Stage,
            D3DTEXTURESTAGESTATETYPE Type,
            DWORD*                   pValue);

    HRESULT STDMETHODCALLTYPE SetTextureStageState(
            DWORD                    Stage,
            D3DTEXTURESTAGESTATETYPE Type,
            DWORD                    Value);

    HRESULT STDMETHODCALLTYPE GetSamplerState(
            DWORD               Sampler,
            D3DSAMPLERSTATETYPE Type,
            DWORD*              pValue);

    HRESULT STDMETHODCALLTYPE SetSamplerState(
            DWORD               Sampler,
            D3DSAMPLERSTATETYPE Type,
            DWORD               Value);

    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses);

    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries);

    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries);

    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber);

    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber);

    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pRect);

    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect);

    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware);

    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing();

    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments);

    float   STDMETHODCALLTYPE GetNPatchMode();

    HRESULT STDMETHODCALLTYPE DrawPrimitive(
            D3DPRIMITIVETYPE PrimitiveType,
            UINT             StartVertex,
            UINT             PrimitiveCount);

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
            D3DPRIMITIVETYPE PrimitiveType,
            INT              BaseVertexIndex,
            UINT             MinVertexIndex,
            UINT             NumVertices,
            UINT             StartIndex,
            UINT             PrimitiveCount);

    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
            D3DPRIMITIVETYPE PrimitiveType,
            UINT             PrimitiveCount,
      const void*            pVertexStreamZeroData,
            UINT             VertexStreamZeroStride);

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
            D3DPRIMITIVETYPE PrimitiveType,
            UINT             MinVertexIndex,
            UINT             NumVertices,
            UINT             PrimitiveCount,
      const void*            pIndexData,
            D3DFORMAT        IndexDataFormat,
      const void*            pVertexStreamZeroData,
            UINT             VertexStreamZeroStride);

    HRESULT STDMETHODCALLTYPE ProcessVertices(
            UINT                         SrcStartIndex,
            UINT                         DestIndex,
            UINT                         VertexCount,
            IDirect3DVertexBuffer9*      pDestBuffer,
            IDirect3DVertexDeclaration9* pVertexDecl,
            DWORD                        Flags);

    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
      const D3DVERTEXELEMENT9*            pVertexElements,
            IDirect3DVertexDeclaration9** ppDecl);

    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl);

    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl);

    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF);

    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF);

    HRESULT STDMETHODCALLTYPE CreateVertexShader(
      const DWORD*                   pFunction,
            IDirect3DVertexShader9** ppShader);

    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader);

    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader);

    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
            UINT   StartRegister,
      const float* pConstantData,
            UINT   Vector4fCount);

    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(
            UINT   StartRegister,
            float* pConstantData,
            UINT   Vector4fCount);

    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
            UINT StartRegister,
      const int* pConstantData,
            UINT Vector4iCount);

    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(
            UINT StartRegister,
            int* pConstantData,
            UINT Vector4iCount);

    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(
            UINT  StartRegister,
      const BOOL* pConstantData,
            UINT  BoolCount);

    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(
            UINT  StartRegister,
            BOOL* pConstantData,
            UINT  BoolCount);

    HRESULT STDMETHODCALLTYPE SetStreamSource(
            UINT                    StreamNumber,
            IDirect3DVertexBuffer9* pStreamData,
            UINT                    OffsetInBytes,
            UINT                    Stride);

    HRESULT STDMETHODCALLTYPE GetStreamSource(
            UINT                     StreamNumber,
            IDirect3DVertexBuffer9** ppStreamData,
            UINT*                    pOffsetInBytes,
            UINT*                    pStride);

    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting);

    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting);

    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData);

    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData);

    HRESULT STDMETHODCALLTYPE CreatePixelShader(
      const DWORD*                  pFunction,
            IDirect3DPixelShader9** ppShader);

    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader);

    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader);

    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
            UINT   StartRegister,
      const float* pConstantData,
            UINT   Vector4fCount);

    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(
            UINT   StartRegister,
            float* pConstantData,
            UINT   Vector4fCount);

    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
            UINT StartRegister,
      const int* pConstantData,
            UINT Vector4iCount);

    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(
            UINT StartRegister,
            int* pConstantData,
            UINT Vector4iCount);

    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(
            UINT  StartRegister,
      const BOOL* pConstantData,
            UINT  BoolCount);

    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(
            UINT  StartRegister,
            BOOL* pConstantData,
            UINT  BoolCount);

    HRESULT STDMETHODCALLTYPE DrawRectPatch(
            UINT               Handle,
      const float*             pNumSegs,
      const D3DRECTPATCH_INFO* pRectPatchInfo);

    HRESULT STDMETHODCALLTYPE DrawTriPatch(
            UINT              Handle,
      const float*            pNumSegs,
      const D3DTRIPATCH_INFO* pTriPatchInfo);

    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle);

    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery);


    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(
            UINT   width,
            UINT   height,
            float* rows,
            float* columns);

    HRESULT STDMETHODCALLTYPE ComposeRects(
            IDirect3DSurface9*      pSrc,
            IDirect3DSurface9*      pDst,
            IDirect3DVertexBuffer9* pSrcRectDescs,
            UINT                    NumRects,
            IDirect3DVertexBuffer9* pDstRectDescs,
            D3DCOMPOSERECTSOP       Operation,
            int                     Xoffset,
            int                     Yoffset);

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority);

    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority);

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain);

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources);

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency);

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency);

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow);

    HRESULT STDMETHODCALLTYPE PresentEx(
      const RECT*    pSourceRect,
      const RECT*    pDestRect,
            HWND     hDestWindowOverride,
      const RGNDATA* pDirtyRegion,
            DWORD    dwFlags);

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(
            UINT                Width,
            UINT                Height,
            D3DFORMAT           Format,
            D3DMULTISAMPLE_TYPE MultiSample,
            DWORD               MultisampleQuality,
            BOOL                Lockable,
            IDirect3DSurface9** ppSurface,
            HANDLE*             pSharedHandle,
            DWORD               Usage);

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(
            UINT                Width,
            UINT                Height,
            D3DFORMAT           Format,
            D3DPOOL             Pool,
            IDirect3DSurface9** ppSurface,
            HANDLE*             pSharedHandle,
            DWORD               Usage);

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(
            UINT                Width,
            UINT                Height,
            D3DFORMAT           Format,
            D3DMULTISAMPLE_TYPE MultiSample,
            DWORD               MultisampleQuality,
            BOOL                Discard,
            IDirect3DSurface9** ppSurface,
            HANDLE*             pSharedHandle,
            DWORD               Usage);

    HRESULT STDMETHODCALLTYPE ResetEx(
            D3DPRESENT_PARAMETERS* pPresentationParameters,
            D3DDISPLAYMODEEX*      pFullscreenDisplayMode);

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(
            UINT                iSwapChain,
            D3DDISPLAYMODEEX*   pMode,
            D3DDISPLAYROTATION* pRotation);

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChainEx(
            D3DPRESENT_PARAMETERS* pPresentationParameters,
      const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
            IDirect3DSwapChain9**  ppSwapChain);

    HRESULT SetStateSamplerState(
        DWORD               StateSampler,
        D3DSAMPLERSTATETYPE Type,
        DWORD               Value);

    HRESULT SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9* pTexture);

    HRESULT SetStateTransform(uint32_t idx, const D3DMATRIX* pMatrix);

    HRESULT SetStateTextureStageState(
            DWORD                      Stage,
            D3D9TextureStageStateTypes Type,
            DWORD                      Value);

    MtlStageMask GetEnabledShaderStages() const {
      return m_dxvkDevice->getShaderStages();
    }

    bool SupportsSWVP();

    bool SupportsVCacheQuery() const;

    bool IsExtended();

    HWND GetWindow();

    const Rc<MtlDevice>& GetDXVKDevice() {
      return m_dxvkDevice;
    }

    D3D9_MTL_FORMAT_MAPPING LookupFormat(
      D3D9Format            Format) const;

    const MtlFormatInfo* UnsupportedFormatInfo(
      D3D9Format            Format) const;

    bool WaitForResource(
      const MtlResource&            Resource,
            uint64_t                SequenceNumber,
            DWORD                   MapFlags);

    HRESULT LockImage(
            D3D9CommonTexture* pResource,
            UINT                    Face,
            UINT                    Mip,
            D3DLOCKED_BOX*          pLockedBox,
      const D3DBOX*                 pBox,
            DWORD                   Flags);

    uint32_t CalcImageLockOffset(
            uint32_t                SlicePitch,
            uint32_t                RowPitch,
      const MtlFormatInfo*          FormatInfo,
      const D3DBOX*                 pBox);

    HRESULT UnlockImage(
            D3D9CommonTexture*      pResource,
            UINT                    Face,
            UINT                    MipLevel);

    HRESULT FlushImage(
            D3D9CommonTexture*      pResource,
            UINT                    Subresource);

    void UpdateTextureFromBuffer(
            D3D9CommonTexture*      pDestTexture,
            D3D9CommonTexture*      pSrcTexture,
            UINT                    DestSubresource,
            UINT                    SrcSubresource,
            MtlOrigin               SrcOffset,
            MtlExtent3D             SrcExtent,
            MtlOrigin               DestOffset);

    void EmitGenerateMips(
            D3D9CommonTexture* pResource);

    HRESULT LockBuffer(
            D3D9CommonBuffer*       pResource,
            UINT                    OffsetToLock,
            UINT                    SizeToLock,
            void**                  ppbData,
            DWORD                   Flags);

    HRESULT FlushBuffer(
            D3D9CommonBuffer*       pResource);

    HRESULT UnlockBuffer(
            D3D9CommonBuffer*       pResource);

    void UploadPerDrawData(
            UINT&                   FirstVertexIndex,
            UINT                    NumVertices,
            UINT&                   FirstIndex,
            UINT                    NumIndices,
            INT&                    BaseVertexIndex,
            bool*                   pDynamicVBOs,
            bool*                   pDynamicIBO);


    void SetupFPU();

    int64_t DetermineInitialTextureMemory();

    void CreateConstantBuffers();

    void SynchronizeCsThread(uint64_t SequenceNumber);

    void Flush();
    void FlushAndSync9On12();

    void BeginFrame(Rc<MtlLatencyTracker> LatencyTracker, uint64_t FrameId);
    void EndFrame(Rc<MtlLatencyTracker> LatencyTracker);

    template <uint32_t Index>
    void UpdateAnyColorWrites();

    void UpdateTextureBitmasks(uint32_t index, DWORD combinedUsage);

    void UpdateActiveHazardsRT(uint32_t texMask);

    void UpdateActiveHazardsDS(uint32_t texMask);

    void EmitFeedbackLoopBarriers();

    void UpdateActiveFetch4(uint32_t stateSampler);

    void UploadManagedTexture(D3D9CommonTexture* pResource);

    void UploadManagedTextures(uint32_t mask);

    void GenerateTextureMips(uint32_t mask);

    void MarkTextureMipsDirty(D3D9CommonTexture* pResource);

    void MarkTextureMipsUnDirty(D3D9CommonTexture* pResource);

    void MarkTextureUploaded(D3D9CommonTexture* pResource);

    void UpdatePointMode(bool pointList);

    void UpdateFog();

    std::pair<float, float> ComputeWNearFar() const;

    void BindFramebuffer();

    void BindViewportAndScissor();

    inline bool IsNVDepthBoundsTestEnabled () {
      if (unlikely(m_isD3D8Compatible))
        return false;

      return m_state.renderStates[D3DRS_ADAPTIVETESS_X] == uint32_t(D3D9Format::NVDB);
    }

    void UpdateAlphaToCoverangeAndAlphaTest();

    inline bool IsZTestEnabled() {
      return m_state.renderStates[D3DRS_ZENABLE] && m_state.depthStencil != nullptr;
    }

    void BindMultiSampleState();

    void BindBlendState();

    void BindBlendFactor();

    void BindDepthStencilState();

    void BindDepthStencilReference();

    void BindRasterizerState();

    void BindDepthBias();

    template<D3D9ShaderType ShaderType>
    void UpdateShaderConstants();

    void UpdateClipPlanes();

    template<typename T>
    void UpdatePushDataBlock(const T& Block);

    void UpdatePushData();

    void BindSampler(DWORD Sampler);

    void BindTexture(DWORD SamplerSampler);

    void UnbindTextures(uint32_t mask);

    void UndirtySamplers(uint32_t mask);

    void UndirtyTextures(uint32_t usedMask);

    void MarkTextureBindingDirty(IDirect3DBaseTexture9* texture);

    bool SamplerUsesBorderColor(DWORD Sampler) const;

    HRESULT STDMETHODCALLTYPE SetRenderTargetInternal(
            DWORD              RenderTargetIndex,
            IDirect3DSurface9* pRenderTarget);

    D3D9DrawInfo GenerateDrawInfo(
      D3DPRIMITIVETYPE PrimitiveType,
      UINT             PrimitiveCount,
      UINT             InstanceCount);

    uint32_t GetInstanceCount() const;

    void PrepareDraw(D3DPRIMITIVETYPE PrimitiveType, bool UploadVBOs, bool UploadIBOs);

    void EnsureSamplerLimit();

    template <D3D9ShaderType ShaderStage>
    void BindShader(
    const D3D9CommonShader*                 pShaderModule);

    template <D3D9ShaderType ShaderStage>
    void BindFFUbershader();

    void BindProgrammablePixelShaderLinkage();

    void BindInputLayout();

    void BindVertexBuffer(
            UINT                              Slot,
            D3D9VertexBuffer*                 pBuffer,
            UINT                              Offset,
            UINT                              Length,
            UINT                              Stride);

    void BindVertexBufferRange(
            UINT                              Slot,
            UINT                              Offset,
            UINT                              Length,
            UINT                              Stride);

    void BindIndices();

    D3D9DeviceLock LockDevice() {
      return m_multithread.AcquireLock();
    }

    const D3D9Options* GetOptions() const {
      return &m_d3d9Options;
    }

    Direct3DState9* GetRawState() {
      return &m_state;
    }

    void Begin(D3D9Query* pQuery);
    void End(D3D9Query* pQuery);

    void SetVertexBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits);
    void SetPixelBoolBitfield (uint32_t idx, uint32_t mask, uint32_t bits);

    void ConsiderFlush(GpuFlushType FlushType);

    bool ChangeReportedMemory(int64_t delta) {
      if (IsExtended())
        return true;

      int64_t availableMemory = m_availableMemory.fetch_add(delta);

      return !m_d3d9Options.memoryTrackTest || availableMemory >= delta;
    }

    void ResolveZ();

    void TransformImage(
            D3D9CommonTexture*       pResource,
      const MtlSubresourceRange*     pSubresources);

    const D3D9ConstantLayout& GetVertexConstantLayout() { return m_consts[uint32_t(D3D9ShaderType::VertexShader)].layout; }
    const D3D9ConstantLayout& GetPixelConstantLayout()  { return m_consts[uint32_t(D3D9ShaderType::PixelShader)].layout; }

    void ResetState(D3DPRESENT_PARAMETERS* pPresentationParameters);
    HRESULT ResetSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode);

    HRESULT InitialReset(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode);

    MemoryFilePool* GetAllocator() {
      return &m_memoryAllocator;
    }

    void* MapTexture(D3D9CommonTexture* pTexture, UINT Subresource);

    void TouchMappedTexture(D3D9CommonTexture* pTexture);

    void RemoveMappedTexture(D3D9CommonTexture* pTexture);

    bool ShouldRecord() const {
      return m_recorder != nullptr;
    }

    bool IsD3D8Compatible() const {
      return m_isD3D8Compatible;
    }

    bool IsDeviceLost() const {
      return m_deviceLostState != D3D9DeviceLostState::Ok;
    }

    void NotifyFullscreen(HWND window, bool fullscreen);
    void NotifyWindowActivated(HWND window, bool activated);

    void IncrementLosableCounter() {
      m_losableResourceCounter++;
    }

    void DecrementLosableCounter() {
      m_losableResourceCounter--;
    }

    bool CanOnlySWVP() const {
      return m_behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }

    bool CanSWVP() const {
      return m_behaviorFlags & (D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING);
    }

    bool IsSWVP() const {
      return m_isSWVP;
    }

    UINT GetSWVPShaderCount() const {
      return m_swvpEmulator.GetShaderCount();
    }

    void InjectCsChunk(
            DxvkCsChunkRef&&            Chunk,
            bool                        Synchronize);

    template<typename Fn>
    void InjectCs(
            Fn&&                        Command) {
      auto chunk = AllocCsChunk();
      chunk->push(std::move(Command));

      InjectCsChunk(std::move(chunk), false);
    }

    DxvkCsChunkRef AllocCsChunk() {
      DxvkCsChunk* chunk = m_csChunkPool.allocChunk(DxvkCsChunkFlag::SingleUse);
      return DxvkCsChunkRef(chunk, &m_csChunkPool);
    }

    bool Is9On12Device() const {
      return m_d3d9On12Args.Enable9On12;
    }

    D3D9Adapter* GetAdapter() const {
      return m_adapter;
    }


    const D3D9ConstantBufferCopy* GetOrCreateConstantLayout(
            D3D9ConstantBufferLayout  FloatLayout,
            D3D9ConstantBufferLayout  IntLayout,
            D3D9ConstantBufferLayout  BoolLayout);

  private:

    template<bool AllowFlush = true, typename Cmd>
    void EmitCs(Cmd&& command) {
      if (unlikely(m_csDataType != D3D9CmdType::None)) {
        m_csData = nullptr;
        m_csDataType = D3D9CmdType::None;
      }

      if (unlikely(!m_csChunk->push(command))) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (AllowFlush)
          ConsiderFlush(GpuFlushType::ImplicitWeakHint);

        m_csChunk->push(command);
      }
    }

    template<typename M, bool AllowFlush = true, typename Cmd>
    DxvkCsDataBlock* EmitCsCmd(D3D9CmdType type, size_t count, Cmd&& command) {
      m_csDataType = type;
      m_csData = m_csChunk->pushCmd<M, Cmd>(command, count);

      if (unlikely(!m_csData)) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (AllowFlush)
          ConsiderFlush(GpuFlushType::ImplicitWeakHint);

        m_csData = m_csChunk->pushCmd<M, Cmd>(command, count);
      }

      return m_csData;
    }

    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    void FlushCsChunk() {
      if (likely(!m_csChunk->empty())) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();
      }
    }

    uint32_t GetResetCounter() {
      return m_resetCtr;
    }

    void ExecuteFlush(bool Synchronize9On12);

    void DetermineConstantLayouts(bool canSWVP);

    D3D9BufferSlice AllocUPBuffer(MtlDeviceSize size);

    D3D9BufferSlice AllocStagingBuffer(MtlDeviceSize size);

    void ThrottleAllocation();

    MtlStagingBufferStats GetStagingMemoryStatistics() const;

    HRESULT CreateShaderModule(
            D3D9CommonShader*     pShaderModule,
            size_t*               pLength,
            D3D9ShaderType        ShaderType,
      const DWORD*                pShaderBytecode);

    inline uint32_t GetUPDataSize(uint32_t vertexCount, uint32_t stride) {
      return vertexCount * stride;
    }

    inline uint32_t GetUPBufferSize(uint32_t vertexCount, uint32_t stride) {
      return (vertexCount - 1) * stride + std::max(m_state.vertexDecl->GetSize(0), stride);
    }

    inline void FillUPVertexBuffer(void* buffer, const void* userData, uint32_t dataSize, uint32_t bufferSize) {
      uint8_t* data = reinterpret_cast<uint8_t*>(buffer);
      dataSize = std::min(dataSize, bufferSize);
      std::memcpy(data, userData, dataSize);
      if (dataSize < bufferSize)
        std::memset(data + dataSize, 0, bufferSize - dataSize);
    }

    template <D3D9ShaderType ShaderType, D3D9ConstantType ConstantType>
    inline static constexpr uint32_t DetermineSoftwareRegCount() {
      constexpr bool isVS = ShaderType == D3D9ShaderType::VertexShader;

      switch (ConstantType) {
        default:
        case D3D9ConstantType::Float:  return isVS ? caps::MaxFloatConstantsSoftware : caps::MaxSM3FloatConstantsPS;
        case D3D9ConstantType::Int:    return isVS ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
        case D3D9ConstantType::Bool:   return isVS ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
      }
    }

    template<D3D9ShaderType ShaderType, D3D9ConstantType ConstantType>
    inline uint32_t DetermineHardwareRegCount() const {
      constexpr bool isVS = ShaderType == D3D9ShaderType::VertexShader;

      switch (ConstantType) {
        default:
        case D3D9ConstantType::Float:  return isVS ? caps::MaxFloatConstantsVS : caps::MaxSM3FloatConstantsPS;
        case D3D9ConstantType::Int:    return caps::MaxOtherConstants;
        case D3D9ConstantType::Bool:   return caps::MaxOtherConstants;
      }
    }

    inline uint32_t GetFrameLatency() {
      return m_frameLatency;
    }

    template <
      D3D9ShaderType   ShaderType,
      D3D9ConstantType ConstantType,
      typename         T>
      HRESULT SetShaderConstants(
              UINT  StartRegister,
        const T*    pConstantData,
              UINT  Count);

    template <
      D3D9ShaderType   ShaderType,
      D3D9ConstantType ConstantType,
      typename         T>
    HRESULT GetShaderConstants(
            UINT StartRegister,
            T*   pConstantData,
            UINT Count) {
      auto GetHelper = [&] (const auto& set) {
        const     uint32_t regCountHardware = DetermineHardwareRegCount<ShaderType, ConstantType>();
        constexpr uint32_t regCountSoftware = DetermineSoftwareRegCount<ShaderType, ConstantType>();

        if (StartRegister + Count > regCountSoftware)
          return D3DERR_INVALIDCALL;

        Count = UINT(
          std::max<INT>(
            std::clamp<INT>(Count + StartRegister, 0, regCountHardware) - INT(StartRegister),
            0));

        if (Count == 0)
          return D3D_OK;

        if (pConstantData == nullptr)
          return D3DERR_INVALIDCALL;

        if constexpr (ConstantType == D3D9ConstantType::Float) {
          const float* source = set->fConsts[StartRegister].data;
          const size_t size   = Count * sizeof(Vector4);

          std::memcpy(pConstantData, source, size);
        }
        else if constexpr (ConstantType == D3D9ConstantType::Int) {
          const int*  source = set->iConsts[StartRegister].data;
          const size_t size  = Count * sizeof(Vector4i);

          std::memcpy(pConstantData, source, size);
        }
        else {
          for (uint32_t i = 0; i < Count; i++) {
            const uint32_t constantIdx = StartRegister + i;
            const uint32_t arrayIdx    = constantIdx / 32;
            const uint32_t bitIdx      = constantIdx % 32;

            const uint32_t bit         = (1u << bitIdx);

            bool constValue = set->bConsts[arrayIdx] & bit;
            pConstantData[i] = constValue ? TRUE : FALSE;
          }
        }

        return D3D_OK;
      };

      return ShaderType == D3D9ShaderType::VertexShader
        ? GetHelper(m_state.vsConsts)
        : GetHelper(m_state.psConsts);
    }

    void UpdateFixedFunctionVS();

    void UpdateFixedFunctionPS();

    void ApplyPrimitiveType(D3DPRIMITIVETYPE PrimType);

    bool UseProgrammableVS();

    bool UseProgrammablePS();

    uint32_t GetAlphaTestPrecision(D3D9Surface* rt);

    void BindAlphaTestState();

    void BindSpecConstants();

    void TrackBufferMappingBufferSequenceNumber(
      D3D9CommonBuffer* pResource);

    void TrackTextureMappingBufferSequenceNumber(
      D3D9CommonTexture* pResource,
      UINT Subresource);

    uint64_t GetCurrentSequenceNumber();

    void UnmapTextures();

    D3D9SwapChainEx* GetMostRecentlyUsedSwapchain() {
      return m_mostRecentlyUsedSwapchain;
    }

    void SetMostRecentlyUsedSwapchain(D3D9SwapChainEx* swapchain) {
      m_mostRecentlyUsedSwapchain = swapchain;
    }

    void ResetMostRecentlyUsedSwapchain() {
      m_mostRecentlyUsedSwapchain = m_implicitSwapchain.ptr();
    }

    bool IsTextureBoundAsAttachment(const D3D9CommonTexture* pTexture) const {
      if (unlikely(pTexture->IsRenderTarget())) {
        for (uint32_t i = 0u; i < m_state.renderTargets.size(); i++) {
          if (m_state.renderTargets[i] == nullptr)
            continue;

          auto texInfo = m_state.renderTargets[i]->GetCommonTexture();
          if (unlikely(texInfo == pTexture)) {
            return true;
          }
        }
      } else if (unlikely(pTexture->IsDepthStencil() && m_state.depthStencil != nullptr)) {
        auto texInfo = m_state.depthStencil->GetCommonTexture();
        if (unlikely(texInfo == pTexture)) {
          return true;
        }
      }
      return false;
    }

    inline bool HasRenderTargetBound(uint32_t Index) const {
      return m_state.renderTargets[Index] != nullptr
        && !m_state.renderTargets[Index]->IsNull();
    }

    inline D3D9ShaderMasks VSShaderMasks() const {
      return m_state.vertexShader != nullptr
        ? m_state.vertexShader->GetCommonShader()->GetShaderMask()
        : D3D9ShaderMasks();
    }

    inline D3D9ShaderMasks PSShaderMasks() const {
      return m_state.pixelShader != nullptr
        ? m_state.pixelShader->GetCommonShader()->GetShaderMask()
        : D3D9ShaderMasks { m_textureSlotTracking.ffpsTextures, 0x1u };
    }

    inline static uint16_t EncodePointSize(DWORD Value) {
      return uint16_t(std::clamp(bit::cast<float>(Value) * 8.0f, 0.0f, 65535.0f));
    }

    D3D9ConstantBuffer& GetConstantBuffer(CbvIndex Index) {
      return m_constantBuffers[uint32_t(Index)];
    }

    GpuFlushType GetMaxFlushType() const;

    bool ValidateSharedTexture(
      HANDLE                          handle,
      D3DRESOURCETYPE                 type,
      const D3D9_COMMON_TEXTURE_DESC& textureDesc) const;

    bool ValidateSharedBuffer(
      HANDLE                        handle,
      const dxvk::D3D9_BUFFER_DESC& bufferDesc) const;

    bool HasFormatsUnlocked() const { return m_unlockAdditionalFormats; }

    void InitShaderOptions();

    static uint32_t GetTextureStageArgMask(
            D3DTEXTUREOP          Op);

    static D3D9TextureStageStateFlags GetTextureStageStateFlags(
            D3DTEXTUREOP          Op,
            UINT                  Arg0,
            UINT                  Arg1,
            UINT                  Arg2,
            bool                  Premodulate);

    Com<D3D9InterfaceEx>            m_parent;
    D3D9Options                     m_d3d9Options;
    D3DDEVTYPE                      m_deviceType;
    HWND                            m_window;
    WORD                            m_behaviorFlags;

    D3D9Adapter*                    m_adapter;
    Rc<MtlDevice>                   m_dxvkDevice;

    MemoryFilePool                  m_memoryAllocator;

    MemoryFilePool                  m_shaderAllocator;

    uint32_t                        m_frameLatency = DefaultFrameLatency;

    D3D9Initializer*                m_initializer = nullptr;
    D3D9FormatHelper*               m_converter   = nullptr;

    D3D9FFShaderModuleSet           m_ffModules;
    D3D9SWVPEmulator                m_swvpEmulator;

    uint64_t                        m_boundPsLinkageHash = ~0ull;

    Com<D3D9StateBlock, false>      m_recorder;

    Rc<D3D9ShaderModuleSet>         m_shaderModules;

    std::array<D3D9ConstantBuffer, CbvIndex::Count> m_constantBuffers;

    Rc<MtlBuffer>                   m_upBuffer;
    Rc<MtlAllocation>               m_upBufferAllocation;
    MtlDeviceSize                   m_upBufferOffset  = 0ull;
    void*                           m_upBufferMapPtr  = nullptr;

    MtlStagingBuffer                m_stagingBuffer;
    Rc<sync::Fence>                 m_stagingBufferFence;
    MtlDeviceSize                   m_stagingMemorySignaled = 0ull;
    MtlDeviceSize                   m_stagingAllocatedBase = 0ull;

    MtlDeviceSize                   m_discardMemoryCounter = 0u;

    D3D9Cursor                      m_cursor;

    Com<D3D9Surface, false>         m_autoDepthStencil;

    Com<D3D9SwapChainEx, false>     m_implicitSwapchain;

    D3D9ShaderOptions               m_shaderOptions;

    std::unordered_map<
      DWORD,
      Com<D3D9VertexDecl,
      false>>                       m_fvfTable;

    D3D9Multithread                 m_multithread;
    D3D9InputAssemblyState          m_iaState;

    D3D9DeviceDirtyFlags            m_dirty;

    D3D9TextureSlotTracking         m_textureSlotTracking;

    D3D9RTSlotTracking              m_rtSlotTracking;

    D3D9VBSlotTracking              m_vbSlotTracking;

    bool                            m_isSWVP;
    bool                            m_isD3D8Compatible;
    bool                            m_ffZTest          = false;

    bool                            m_alphaTestEnabled = false;
    bool                            m_atocEnabled      = false;
    bool                            m_nvdbEnabled      = false;

    bool                            m_inScene          = false;
    bool                            m_validSampleMask  = false;


    uint32_t                        m_resetCtr = 0u;

    float                           m_depthBiasScale  = 0.0f;

    uint32_t                        m_robustSSBOAlignment     = 1;
    uint32_t                        m_robustUBOAlignment      = 1;

    D3D9ConstantSets                m_consts[uint32_t(D3D9ShaderType::PixelShader) + 1];

    D3D9UserDefinedAnnotation*      m_annotation = nullptr;

    D3D9ViewportInfo                m_viewportInfo;

    DxvkCsChunkPool                 m_csChunkPool;
    DxvkCsThread                    m_csThread;
    DxvkCsChunkRef                  m_csChunk;
    uint64_t                        m_csSeqNum = 0ull;

    D3D9CmdType                     m_csDataType = D3D9CmdType::None;
    DxvkCsDataBlock*                m_csData = nullptr;

    Rc<sync::Fence>                 m_submissionFence;
    uint64_t                        m_submissionId = 0ull;
    MtlSubmitStatus                 m_submitStatus;

    uint64_t                        m_flushSeqNum = 0ull;
    GpuFlushTracker                 m_flushTracker;

    std::atomic<int64_t>            m_availableMemory = { 0 };

    D3D9DeviceLostState             m_deviceLostState          = D3D9DeviceLostState::Ok;
    HWND                            m_fullscreenWindow         = NULL;
    std::atomic<uint32_t>           m_losableResourceCounter   = { 0 };

    D3D9SwapChainEx*                m_mostRecentlyUsedSwapchain = nullptr;

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    lru_list<D3D9CommonTexture*>    m_mappedTextures;
#endif

    dxvk::mutex                     m_constantLayoutMutex;
    std::unordered_set<D3D9ConstantBufferCopy,
      DxvkHash, DxvkEq>             m_constantLayouts;

    // Members below are destroyed first; their objects can call back into the device on teardown.
    Direct3DState9                  m_state;
    D3D9PushData                    m_pushData = {};
    D3D9SpecData                    m_specData = {};

    D3D9ON12_ARGS                   m_d3d9On12Args = { };
    DxvkD3D8Bridge                  m_d3d8Bridge;

    constexpr static uint32_t       SamplerCountBits = 12u;
    constexpr static uint64_t       SamplerCountMask = (1u << SamplerCountBits) - 1u;

    uint64_t                        m_samplerBindCount = 0u;

    uint64_t                        m_lastSamplerLiveCount = 0u;
    uint64_t                        m_lastSamplerBindCount = 0u;

    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t>           m_lastSamplerStats = { 0u };

    bool m_unlockAdditionalFormats = false;
  };

}
