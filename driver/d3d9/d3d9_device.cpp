#include "d3d9_device.h"

#include "d3d9_annotation.h"
#include "d3d9_common_texture.h"
#include "d3d9_interface.h"
#include "d3d9_swapchain.h"
#include "d3d9_caps.h"
#include "d3d9_util.h"
#include "d3d9_texture.h"
#include "d3d9_buffer.h"
#include "d3d9_vertex_declaration.h"
#include "d3d9_shader.h"
#include "d3d9_query.h"
#include "d3d9_stateblock.h"
#include "d3d9_monitor.h"
#include "d3d9_names.h"
#include "d3d9_format_helpers.h"

#include "../mtl/mtl_device.h"

#include "../util/util_bit.h"
#include "../util/util_math.h"
#include "../util/util_image_math.h"

#include "d3d9_initializer.h"

#include <algorithm>
#include <cfloat>
#ifdef MSC_VER
#pragma fenv_access (on)
#endif

namespace dxvk {

  D3D9DeviceEx::D3D9DeviceEx(
          D3D9InterfaceEx*       pParent,
          D3D9Adapter*           pAdapter,
          D3DDEVTYPE             DeviceType,
          HWND                   hFocusWindow,
          DWORD                  BehaviorFlags,
          Rc<MtlDevice>         dxvkDevice)
    : m_parent             ( pParent )
    , m_d3d9Options        ( dxvkDevice, pParent->GetInstance()->config() )
    , m_deviceType         ( DeviceType )
    , m_window             ( hFocusWindow )
    , m_behaviorFlags      ( BehaviorFlags )
    , m_adapter            ( pAdapter )
    , m_dxvkDevice         ( dxvkDevice )
    , m_memoryAllocator    ( )
    , m_shaderAllocator    ( )
    , m_ffModules          ( )
    , m_shaderModules      ( new D3D9ShaderModuleSet )
    , m_stagingBuffer      ( dxvkDevice, StagingBufferSize )
    , m_stagingBufferFence ( new sync::Fence() )
    , m_multithread        ( BehaviorFlags & D3DCREATE_MULTITHREADED )
    , m_isSWVP             ( (BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 )
    , m_isD3D8Compatible   ( pParent->IsD3D8Compatible() )
    , m_csThread           ( dxvkDevice, dxvkDevice->createContext() )
    , m_csChunk            ( AllocCsChunk() )
    , m_submissionFence    ( new sync::Fence() )
    , m_flushTracker       ( GetMaxFlushType() )
    , m_d3d9On12Args       ( pAdapter->Get9On12Args() )
    , m_d3d8Bridge         ( this ) {

    InitShaderOptions();

    m_ffModules.Initialize(m_dxvkDevice);

    bool canSWVP = CanSWVP();
    DetermineConstantLayouts(canSWVP);

    if (canSWVP)
      Logger::info("D3D9DeviceEx: Using extended constant set for software vertex processing.");

    m_annotation = new D3D9UserDefinedAnnotation(this);

    m_initializer      = new D3D9Initializer(this);
    m_converter        = new D3D9FormatHelper(m_dxvkDevice);

    EmitCs([] (MtlContext* ctx) {
      ctx->beginRecording();
    });

    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE))
      SetupFPU();


    CreateConstantBuffers();

    m_availableMemory = DetermineInitialTextureMemory();


    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer,
                D3D9DeviceDirtyFlag::ClipPlanes,
                D3D9DeviceDirtyFlag::DepthStencilState,
                D3D9DeviceDirtyFlag::BlendState,
                D3D9DeviceDirtyFlag::RasterizerState,
                D3D9DeviceDirtyFlag::DepthBias,
                D3D9DeviceDirtyFlag::AlphaTestState,
                D3D9DeviceDirtyFlag::InputLayout,
                D3D9DeviceDirtyFlag::ViewportScissor,
                D3D9DeviceDirtyFlag::MultiSampleState,
                D3D9DeviceDirtyFlag::Fog,
                D3D9DeviceDirtyFlag::FFVertexData,
                D3D9DeviceDirtyFlag::FFVertexBlend,
                D3D9DeviceDirtyFlag::FFPixelShader,
                D3D9DeviceDirtyFlag::FFViewport,
                D3D9DeviceDirtyFlag::SharedPixelShaderData,
                D3D9DeviceDirtyFlag::DepthBounds,
                D3D9DeviceDirtyFlag::PointScale,
                D3D9DeviceDirtyFlag::SpecializationEntries);

    m_specData.setDrefScale(m_d3d9Options.drefScaling);

    BindFFUbershader<D3D9ShaderType::VertexShader>();
    BindFFUbershader<D3D9ShaderType::PixelShader>();

    m_unlockAdditionalFormats = m_parent->HasFormatsUnlocked();
  }


  D3D9DeviceEx::~D3D9DeviceEx() {
    if (this_thread::isInModuleDetachment())
      return;

    ExecuteFlush(true);
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    if (m_annotation)
      delete m_annotation;

    delete m_initializer;
    delete m_converter;

    m_dxvkDevice->waitForIdle();
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    bool extended = m_parent->IsExtended()
                 && riid == __uuidof(IDirect3DDevice9Ex);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DDevice9)
     || extended) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDxvkD3D8Bridge)) {
      *ppvObject = ref(&m_d3d8Bridge);
      return S_OK;
    }


    if (riid == __uuidof(IDirect3DDevice9Ex))
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(IDirect3DDevice9), riid)) {
      Logger::warn("D3D9DeviceEx::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::TestCooperativeLevel() {
    D3D9DeviceLock lock = LockDevice();

    if (likely(m_deviceLostState == D3D9DeviceLostState::Ok)) {
      return D3D_OK;
    } else if (m_deviceLostState == D3D9DeviceLostState::NotReset) {
      return D3DERR_DEVICENOTRESET;
    } else {
      return D3DERR_DEVICELOST;
    }
  }


  UINT    STDMETHODCALLTYPE D3D9DeviceEx::GetAvailableTextureMem() {

    constexpr UINT range = 0xfff00000;

    int64_t memory = std::min(std::max<int64_t>(m_availableMemory.load(), 0), static_cast<int64_t>(range));

    return UINT(memory) & range;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EvictManagedResources() {
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDirect3D(IDirect3D9** ppD3D9) {
    if (ppD3D9 == nullptr)
      return D3DERR_INVALIDCALL;

    *ppD3D9 = m_parent.ref();
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDeviceCaps(D3DCAPS9* pCaps) {
    if (pCaps == nullptr)
      return D3DERR_INVALIDCALL;

    m_adapter->GetDeviceCaps(m_deviceType, pCaps);

    pCaps->MaxVertexBlendMatrixIndex = m_isSWVP ? 255 : 8;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    return m_implicitSwapchain->GetDisplayMode(pMode);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
    if (pParameters == nullptr)
      return D3DERR_INVALIDCALL;

    pParameters->AdapterOrdinal = m_adapter->GetOrdinal();
    pParameters->BehaviorFlags  = m_behaviorFlags;
    pParameters->DeviceType     = m_deviceType;
    pParameters->hFocusWindow   = m_window;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetCursorProperties(
          UINT               XHotSpot,
          UINT               YHotSpot,
          IDirect3DSurface9* pCursorBitmap) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pCursorBitmap == nullptr))
      return D3DERR_INVALIDCALL;

    auto* cursorTex = GetCommonTexture(pCursorBitmap);
    if (unlikely(cursorTex->Desc()->Format != D3D9Format::A8R8G8B8))
      return D3DERR_INVALIDCALL;

    const uint32_t inputWidth  = cursorTex->Desc()->Width;
    const uint32_t inputHeight = cursorTex->Desc()->Height;

    if (unlikely((inputWidth  && (inputWidth  & (inputWidth  - 1)))
              || (inputHeight && (inputHeight & (inputHeight - 1)))))
      return D3DERR_INVALIDCALL;

    if (unlikely((inputWidth  && (XHotSpot > inputWidth  - 1))
              || (inputHeight && (YHotSpot > inputHeight - 1))))
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEEX mode = { };
    mode.Size = sizeof(D3DDISPLAYMODEEX);
    m_adapter->GetAdapterDisplayModeEx(&mode, nullptr);

    if (unlikely(inputWidth  > mode.Width
              || inputHeight > mode.Height))
      return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS params;
    m_implicitSwapchain->GetPresentParameters(&params);

    const bool hwCursor = (inputWidth  == HardwareCursorWidth &&
                           inputHeight == HardwareCursorHeight)
                          || params.Windowed;

    D3DLOCKED_BOX lockedBox;
    HRESULT hr = LockImage(cursorTex, 0, 0, &lockedBox, nullptr, D3DLOCK_READONLY);
    if (unlikely(FAILED(hr)))
      return hr;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(lockedBox.pBits);

    if (hwCursor) {
      CursorBitmap bitmap = { 0 };
      const size_t copyPitch = std::min<size_t>(
        HardwareCursorPitch,
        inputWidth * HardwareCursorFormatSize);
      const size_t copyHeight = std::min<size_t>(
        HardwareCursorHeight,
        inputHeight);

      for (uint32_t h = 0; h < copyHeight; h++)
        std::memcpy(&bitmap[h * HardwareCursorPitch], &data[h * lockedBox.RowPitch], copyPitch);

      hr = UnlockImage(cursorTex, 0, 0);
      if (unlikely(FAILED(hr)))
        return hr;

      m_cursor.SetHardwareCursor(XHotSpot, YHotSpot, bitmap);
    } else {
      const size_t copyPitch = inputWidth * HardwareCursorFormatSize;
      std::vector<uint8_t> bitmap(inputHeight * copyPitch, 0);

      for (uint32_t h = 0; h < inputHeight; h++)
        std::memcpy(&bitmap[h * copyPitch], &data[h * lockedBox.RowPitch], copyPitch);

      hr = UnlockImage(cursorTex, 0, 0);
      if (unlikely(FAILED(hr)))
        return hr;

      m_implicitSwapchain->SetCursorTexture(inputWidth, inputHeight, &bitmap[0]);

      m_cursor.SetSoftwareCursor(XHotSpot, YHotSpot, inputWidth, inputHeight);
    }

    return D3D_OK;
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::SetCursorPosition(int X, int Y, DWORD Flags) {
    D3D9DeviceLock lock = LockDevice();

    m_cursor.UpdateCursor(X, Y);
  }


  BOOL    STDMETHODCALLTYPE D3D9DeviceEx::ShowCursor(BOOL bShow) {
    D3D9DeviceLock lock = LockDevice();

    return m_cursor.ShowCursor(bShow);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChain(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          IDirect3DSwapChain9**  ppSwapChain) {
    return CreateAdditionalSwapChainEx(pPresentationParameters, nullptr, ppSwapChain);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(pSwapChain);

    if (unlikely(pSwapChain == nullptr))
      return D3DERR_INVALIDCALL;


    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    *pSwapChain = static_cast<IDirect3DSwapChain9*>(m_implicitSwapchain.ref());

    return D3D_OK;
  }


  UINT    STDMETHODCALLTYPE D3D9DeviceEx::GetNumberOfSwapChains() {

    return 1;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    D3D9DeviceLock lock = LockDevice();

    Logger::info("Device reset");
    m_deviceLostState = D3D9DeviceLostState::Ok;

    HRESULT hr;
    if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {
      hr = m_parent->ValidatePresentationParameters(pPresentationParameters);

      if (unlikely(FAILED(hr)))
        return hr;
    }

    if (!IsExtended()) {
      ResetState(pPresentationParameters);
      m_implicitSwapchain->DestroyBackBuffers();
      m_autoDepthStencil = nullptr;

      EmitCs([] (MtlContext* ctx) {
        ctx->bindIndexBuffer(MtlBufferSlice(), MtlIndexType::Uint32);

        for (uint32_t i = 0; i < caps::MaxStreams; i++)
          ctx->bindVertexBuffer(i, MtlBufferSlice(), 0);
      });

      m_inScene = false;
    } else {
      const float MinZ = m_state.viewport.MinZ;
      const float MaxZ = m_state.viewport.MaxZ;

      for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++) {
        SetRenderTargetInternal(i, nullptr);
      }

      m_state.viewport.MinZ = MinZ;
      m_state.viewport.MaxZ = MaxZ;

      SetDepthStencilSurface(nullptr);
    }

    m_cursor.ResetCursor();

    if (unlikely(m_losableResourceCounter.load() != 0 && !IsExtended() && m_d3d9Options.countLosableResources)) {
      Logger::warn(str::format("Device reset failed because device still has alive losable resources: Device not reset. Remaining resources: ", m_losableResourceCounter.load()));
      m_deviceLostState = D3D9DeviceLostState::NotReset;
      return m_isD3D8Compatible ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
    }

    hr = ResetSwapChain(pPresentationParameters, nullptr);
    if (unlikely(FAILED(hr))) {
      if (!IsExtended()) {
        Logger::warn("Device reset failed: Device not reset");
        m_deviceLostState = D3D9DeviceLostState::NotReset;
      }
      return hr;
    }

    ExecuteFlush(true);
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    if (m_d3d9Options.deferSurfaceCreation)
      m_resetCtr++;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion) {
    return PresentEx(
      pSourceRect,
      pDestRect,
      hDestWindowOverride,
      pDirtyRegion,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetBackBuffer(
          UINT                iSwapChain,
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    InitReturnPtr(ppBackBuffer);

    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    return m_implicitSwapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    return m_implicitSwapchain->GetRasterStatus(pRasterStatus);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetDialogBoxMode(BOOL bEnableDialogs) {
    return m_implicitSwapchain->SetDialogBoxMode(bEnableDialogs);
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::SetGammaRamp(
          UINT          iSwapChain,
          DWORD         Flags,
    const D3DGAMMARAMP* pRamp) {
    if (unlikely(iSwapChain != 0))
      return;

    m_implicitSwapchain->SetGammaRamp(Flags, pRamp);
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
    if (unlikely(iSwapChain != 0))
      return;

    m_implicitSwapchain->GetGammaRamp(pRamp);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateTexture(
          UINT                Width,
          UINT                Height,
          UINT                Levels,
          DWORD               Usage,
          D3DFORMAT           Format,
          D3DPOOL             Pool,
          IDirect3DTexture9** ppTexture,
          HANDLE*             pSharedHandle) {
    InitReturnPtr(ppTexture);

    if (unlikely(ppTexture == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = FALSE;
    desc.IsLockable         = Pool != D3DPOOL_DEFAULT
                            || (Usage & D3DUSAGE_DYNAMIC)
                            || IsVendorFormat(EnumerateFormat(Format));

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_TEXTURE, &desc);
    if (FAILED(hr))
      return hr;

    try {
      void* initialData = nullptr;

      if (unlikely(pSharedHandle != nullptr && Pool == D3DPOOL_SYSTEMMEM && Levels == 1)) {
        initialData = *(reinterpret_cast<void**>(pSharedHandle));
        pSharedHandle = nullptr;
      }

      if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
        return D3DERR_INVALIDCALL;

      if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                   !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_TEXTURE, desc)))
        return E_INVALIDARG;

      const Com<D3D9Texture2D> texture = new D3D9Texture2D(this, &desc, IsExtended(), pSharedHandle);

      m_initializer->InitTexture(texture->GetCommonTexture(), initialData);
      *ppTexture = texture.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVolumeTexture(
          UINT                      Width,
          UINT                      Height,
          UINT                      Depth,
          UINT                      Levels,
          DWORD                     Usage,
          D3DFORMAT                 Format,
          D3DPOOL                   Pool,
          IDirect3DVolumeTexture9** ppVolumeTexture,
          HANDLE*                   pSharedHandle) {
    InitReturnPtr(ppVolumeTexture);

    if (unlikely(ppVolumeTexture == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle))
        Logger::err("CreateVolumeTexture: Shared volume textures not supported");

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = Depth;
    desc.ArraySize          = 1;
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = FALSE;
    desc.IsLockable         = Pool != D3DPOOL_DEFAULT
                            || (Usage & D3DUSAGE_DYNAMIC);

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_VOLUMETEXTURE, &desc);
    if (FAILED(hr))
      return hr;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_VOLUMETEXTURE, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9Texture3D> texture = new D3D9Texture3D(this, &desc, IsExtended());
      m_initializer->InitTexture(texture->GetCommonTexture());
      *ppVolumeTexture = texture.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateCubeTexture(
          UINT                    EdgeLength,
          UINT                    Levels,
          DWORD                   Usage,
          D3DFORMAT               Format,
          D3DPOOL                 Pool,
          IDirect3DCubeTexture9** ppCubeTexture,
          HANDLE*                 pSharedHandle) {
    InitReturnPtr(ppCubeTexture);

    if (unlikely(ppCubeTexture == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle))
        Logger::err("CreateCubeTexture: Shared cube textures not supported");

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = EdgeLength;
    desc.Height             = EdgeLength;
    desc.Depth              = 1;
    desc.ArraySize          = 6;
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = FALSE;
    desc.IsLockable         = Pool != D3DPOOL_DEFAULT
                            || (Usage & D3DUSAGE_DYNAMIC)
                            || IsVendorFormat(EnumerateFormat(Format));

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_CUBETEXTURE, &desc);
    if (FAILED(hr))
      return hr;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_CUBETEXTURE, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9TextureCube> texture = new D3D9TextureCube(this, &desc, IsExtended());
      m_initializer->InitTexture(texture->GetCommonTexture());
      *ppCubeTexture = texture.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexBuffer(
          UINT                     Length,
          DWORD                    Usage,
          DWORD                    FVF,
          D3DPOOL                  Pool,
          IDirect3DVertexBuffer9** ppVertexBuffer,
          HANDLE*                  pSharedHandle) {
    InitReturnPtr(ppVertexBuffer);

    if (unlikely(ppVertexBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_NOTAVAILABLE;

    if (unlikely(pSharedHandle))
        Logger::err("CreateVertexBuffer: Shared vertex buffers not supported");

    D3D9_BUFFER_DESC desc;
    desc.Format = D3D9Format::VERTEXDATA;
    desc.FVF    = FVF;
    desc.Pool   = Pool;
    desc.Size   = Length;
    desc.Type   = D3DRTYPE_VERTEXBUFFER;
    desc.Usage  = Usage;

    if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc, IsExtended())))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedBuffer(*pSharedHandle, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9VertexBuffer> buffer = new D3D9VertexBuffer(this, &desc, IsExtended());
      m_initializer->InitBuffer(buffer->GetCommonBuffer());
      *ppVertexBuffer = buffer.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateIndexBuffer(
          UINT                    Length,
          DWORD                   Usage,
          D3DFORMAT               Format,
          D3DPOOL                 Pool,
          IDirect3DIndexBuffer9** ppIndexBuffer,
          HANDLE*                 pSharedHandle) {
    InitReturnPtr(ppIndexBuffer);

    if (unlikely(ppIndexBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_NOTAVAILABLE;

    if (unlikely(pSharedHandle))
        Logger::err("CreateIndexBuffer: Shared index buffers not supported");

    D3D9_BUFFER_DESC desc;
    desc.Format = EnumerateFormat(Format);
    desc.Pool   = Pool;
    desc.Size   = Length;
    desc.Type   = D3DRTYPE_INDEXBUFFER;
    desc.Usage  = Usage;

    if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc, IsExtended())))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedBuffer(*pSharedHandle, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9IndexBuffer> buffer = new D3D9IndexBuffer(this, &desc, IsExtended());
      m_initializer->InitBuffer(buffer->GetCommonBuffer());
      *ppIndexBuffer = buffer.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTarget(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Lockable,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle) {
    return CreateRenderTargetEx(
      Width,
      Height,
      Format,
      MultiSample,
      MultisampleQuality,
      Lockable,
      ppSurface,
      pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurface(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Discard,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle) {
    return CreateDepthStencilSurfaceEx(
      Width,
      Height,
      Format,
      MultiSample,
      MultisampleQuality,
      Discard,
      ppSurface,
      pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateSurface(
          IDirect3DSurface9* pSourceSurface,
    const RECT*              pSourceRect,
          IDirect3DSurface9* pDestinationSurface,
    const POINT*             pDestPoint) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* src = static_cast<D3D9Surface*>(pSourceSurface);
    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestinationSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* srcTextureInfo = src->GetCommonTexture();
    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();

    if (unlikely(srcTextureInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM || dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcTextureInfo->Desc()->Format != dstTextureInfo->Desc()->Format))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcTextureInfo->Desc()->MultiSample != D3DMULTISAMPLE_NONE))
      return D3DERR_INVALIDCALL;

    if (unlikely(dstTextureInfo->Desc()->MultiSample != D3DMULTISAMPLE_NONE))
      return D3DERR_INVALIDCALL;

    const MtlFormatInfo* formatInfo = lookupFormatInfo(dstTextureInfo->GetFormatMapping().FormatColor);

    MtlOrigin srcOffset = { 0u, 0u, 0u };
    MtlOrigin dstOffset = { 0u, 0u, 0u };
    MtlExtent3D texLevelExtent = srcTextureInfo->GetExtentMip(src->GetSubresource());
    MtlExtent3D extent = texLevelExtent;

    if (pSourceRect != nullptr) {
      srcOffset = { pSourceRect->left,
                    pSourceRect->top,
                    0u };

      extent = { uint32_t(pSourceRect->right - pSourceRect->left), uint32_t(pSourceRect->bottom - pSourceRect->top), 1 };

      const bool extentAligned = extent.width % formatInfo->blockWidth == 0
        && extent.height % formatInfo->blockHeight == 0;

      if (pSourceRect->left < 0
        || pSourceRect->top < 0
        || pSourceRect->right <= pSourceRect->left
        || pSourceRect->bottom <= pSourceRect->top
        || pSourceRect->left % formatInfo->blockWidth != 0
        || pSourceRect->top % formatInfo->blockHeight != 0
        || (extent != texLevelExtent && !extentAligned))
        return D3DERR_INVALIDCALL;
    }

    if (pDestPoint != nullptr) {
      if (pDestPoint->x % formatInfo->blockWidth != 0
        || pDestPoint->y % formatInfo->blockHeight != 0
        || pDestPoint->x < 0
        || pDestPoint->y < 0)
        return D3DERR_INVALIDCALL;

      dstOffset = { pDestPoint->x,
                    pDestPoint->y,
                    0u };
    }

    UpdateTextureFromBuffer(dstTextureInfo, srcTextureInfo, dst->GetSubresource(), src->GetSubresource(), srcOffset, extent, dstOffset);

    dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

    if (dstTextureInfo->IsAutomaticMip())
      MarkTextureMipsDirty(dstTextureInfo);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateTexture(
          IDirect3DBaseTexture9* pSourceTexture,
          IDirect3DBaseTexture9* pDestinationTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (!pDestinationTexture || !pSourceTexture)
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTexInfo = GetCommonTexture(pDestinationTexture);
    D3D9CommonTexture* srcTexInfo = GetCommonTexture(pSourceTexture);

    if (unlikely(srcTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM || dstTexInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcTexInfo->Desc()->MipLevels < dstTexInfo->Desc()->MipLevels && !dstTexInfo->IsAutomaticMip()))
      return D3DERR_INVALIDCALL;

    if (unlikely(dstTexInfo->Desc()->Format != srcTexInfo->Desc()->Format))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcTexInfo->IsAutomaticMip() && !dstTexInfo->IsAutomaticMip()))
      return D3DERR_INVALIDCALL;

    const Rc<MtlTexture> dstImage  = dstTexInfo->GetImage();
    uint32_t srcMipLevels = srcTexInfo->IsAutomaticMip() ? 1 : srcTexInfo->Desc()->MipLevels;
    uint32_t dstMipLevels = dstTexInfo->IsAutomaticMip() ? 1 : dstTexInfo->Desc()->MipLevels;
    uint32_t arraySlices = std::min(srcTexInfo->Desc()->ArraySize, dstTexInfo->Desc()->ArraySize);

    uint32_t srcMipOffset = 0;
    MtlExtent3D srcFirstMipExtent = srcTexInfo->GetExtent();
    MtlExtent3D dstFirstMipExtent = dstTexInfo->GetExtent();

    if (srcMipLevels > 1 || dstMipLevels > 1) {
      if (srcFirstMipExtent != dstFirstMipExtent) {

        srcMipOffset = srcTexInfo->Desc()->MipLevels - dstMipLevels;
        srcFirstMipExtent = util::computeMipLevelExtent(srcTexInfo->GetExtent(), srcMipOffset);
        dstFirstMipExtent = dstTexInfo->GetExtent();
      }

      if (srcFirstMipExtent != dstFirstMipExtent)
        return D3DERR_INVALIDCALL;
    } else {
      if (unlikely(srcFirstMipExtent.width  > dstFirstMipExtent.width
                || srcFirstMipExtent.height > dstFirstMipExtent.height
                || srcFirstMipExtent.depth  > dstFirstMipExtent.depth))
        Logger::warn("D3D9DeviceEx::UpdateTexture: Source dimensions exceed the destination");
    }

    for (uint32_t a = 0; a < arraySlices; a++) {
      const D3DBOX& box = srcTexInfo->GetDirtyBox(a);
      { static int n=0; if (n++<8) Logger::info(str::format(
          "D9MT-UPDTEX box L", box.Left, " T", box.Top, " R", box.Right,
          " B", box.Bottom, " F", box.Front, " K", box.Back)); }
      if (box.Left >= box.Right || box.Top >= box.Bottom || box.Front >= box.Back)
        continue;

      MtlExtent3D mip0Extent = {
        uint32_t(box.Right - box.Left),
        uint32_t(box.Bottom - box.Top),
        uint32_t(box.Back - box.Front)
      };
      MtlOrigin mip0Offset = { int32_t(box.Left), int32_t(box.Top), int32_t(box.Front) };

      for (uint32_t dstMip = 0; dstMip < dstMipLevels; dstMip++) {
        uint32_t srcMip = dstMip + srcMipOffset;
        uint32_t srcSubresource = srcTexInfo->CalcSubresource(a, srcMip);
        uint32_t dstSubresource = dstTexInfo->CalcSubresource(a, dstMip);
        MtlExtent3D extent = util::computeMipLevelExtent(mip0Extent, srcMip);
        MtlOrigin offset = util::computeMipLevelOffset(mip0Offset, srcMip);

        UpdateTextureFromBuffer(dstTexInfo, srcTexInfo, dstSubresource, srcSubresource, offset, extent, offset);

        dstTexInfo->SetNeedsReadback(dstSubresource, true);
      }
    }

    srcTexInfo->ClearDirtyBoxes();
    if (dstTexInfo->IsAutomaticMip() && dstMipLevels != dstTexInfo->Desc()->MipLevels)
      MarkTextureMipsDirty(dstTexInfo);

    ConsiderFlush(GpuFlushType::ImplicitWeakHint);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTargetData(
          IDirect3DSurface9* pRenderTarget,
          IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(IsDeviceLost())) {
      return D3DERR_DEVICELOST;
    }

    D3D9Surface* src = static_cast<D3D9Surface*>(pRenderTarget);
    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    if (pRenderTarget == pDestSurface)
      return D3D_OK;

    D3D9CommonTexture* dstTexInfo = GetCommonTexture(dst);
    D3D9CommonTexture* srcTexInfo = GetCommonTexture(src);

    if (srcTexInfo->Desc()->Format != dstTexInfo->Desc()->Format)
      return D3DERR_INVALIDCALL;

    if (src->GetSurfaceExtent() != dst->GetSurfaceExtent())
      return D3DERR_INVALIDCALL;

    if (dstTexInfo->Desc()->Pool == D3DPOOL_DEFAULT)
      return this->StretchRect(pRenderTarget, nullptr, pDestSurface, nullptr, D3DTEXF_NONE);

    MtlExtent3D dstTexExtent = dstTexInfo->GetExtentMip(dst->GetMipLevel());
    MtlExtent3D srcTexExtent = srcTexInfo->GetExtentMip(src->GetMipLevel());

    const bool clearDst = dstTexInfo->Desc()->MipLevels > 1
                       || dstTexExtent.width > srcTexExtent.width
                       || dstTexExtent.height > srcTexExtent.height;

    dstTexInfo->CreateBuffer(clearDst, dstTexInfo->GetTotalSize());
    MtlBufferSlice dstBufferSlice      = dstTexInfo->GetBufferSlice(dst->GetSubresource());
    Rc<MtlTexture> srcImage              = srcTexInfo->GetImage();
    const MtlFormatInfo* srcFormatInfo = lookupFormatInfo(srcImage->info().format);

    const MtlSubresourceLayers srcSubresource = srcTexInfo->GetSubresourceFromIndex(srcFormatInfo->aspects, src->GetSubresource());
    MtlSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspects,
      srcSubresource.mipLevel,
      srcSubresource.baseLayer, 1 };

    EmitCs([
      cBufferSlice  = std::move(dstBufferSlice),
      cImage        = srcImage,
      cSubresources = srcSubresourceLayers,
      cLevelExtent  = srcTexExtent
    ] (MtlContext* ctx) {
      ctx->copyTextureToBuffer(cBufferSlice.buffer(), cBufferSlice.offset(),
        4, 0, MtlFormat::Invalid, cImage, cSubresources,
        MtlOrigin { 0, 0, 0 }, cLevelExtent);
    });

    dstTexInfo->SetNeedsReadback(dst->GetSubresource(), true);
    TrackTextureMappingBufferSequenceNumber(dstTexInfo, dst->GetSubresource());

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock lock = LockDevice();

    return m_mostRecentlyUsedSwapchain->GetFrontBufferData(pDestSurface);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::StretchRect(
          IDirect3DSurface9*   pSourceSurface,
    const RECT*                pSourceRect,
          IDirect3DSurface9*   pDestSurface,
    const RECT*                pDestRect,
          D3DTEXTUREFILTERTYPE Filter) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);
    D3D9Surface* src = static_cast<D3D9Surface*>(pSourceSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(src == dst))
      return D3DERR_INVALIDCALL;

    bool fastPath = true;

    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();
    D3D9CommonTexture* srcTextureInfo = src->GetCommonTexture();

    if (unlikely(dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT ||
                 srcTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    Rc<MtlTexture> dstImage = dstTextureInfo->GetImage();
    Rc<MtlTexture> srcImage = srcTextureInfo->GetImage();

    if (dstImage == nullptr || srcImage == nullptr)
        return D3DERR_INVALIDCALL;

    const MtlFormatInfo* dstFormatInfo = lookupFormatInfo(dstImage->info().format);
    const MtlFormatInfo* srcFormatInfo = lookupFormatInfo(srcImage->info().format);

    const MtlSubresourceLayers dstSubresource = dstTextureInfo->GetSubresourceFromIndex(dstFormatInfo->aspects, dst->GetSubresource());
    const MtlSubresourceLayers srcSubresource = srcTextureInfo->GetSubresourceFromIndex(srcFormatInfo->aspects, src->GetSubresource());

    if (unlikely(Filter != D3DTEXF_NONE && Filter != D3DTEXF_LINEAR && Filter != D3DTEXF_POINT))
      return D3DERR_INVALIDCALL;

    MtlExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);
    MtlExtent3D dstExtent = dstImage->mipLevelExtent(dstSubresource.mipLevel);

    D3D9Format srcFormat = srcTextureInfo->Desc()->Format;
    D3D9Format dstFormat = dstTextureInfo->Desc()->Format;

    bool similar = AreFormatsSimilar(srcFormat, dstFormat);

    fastPath &= similar;

    auto needsResolve = false;
    if (srcImage->info().sampleCount != dstImage->info().sampleCount) {
      needsResolve = srcImage->info().sampleCount != 1u;
      auto fbBlit = dstImage->info().sampleCount != 1u;
      fastPath &= !fbBlit;
    }

    if (pSourceRect != nullptr) {
      fastPath       &=  (pSourceRect->left   % srcFormatInfo->blockWidth  == 0);
      fastPath       &=  (pSourceRect->right  % srcFormatInfo->blockWidth  == 0);
      fastPath       &=  (pSourceRect->top    % srcFormatInfo->blockHeight == 0);
      fastPath       &=  (pSourceRect->bottom % srcFormatInfo->blockHeight == 0);
    }

    if (pDestRect != nullptr) {
      fastPath       &=  (pDestRect->left     % dstFormatInfo->blockWidth  == 0);
      fastPath       &=  (pDestRect->top      % dstFormatInfo->blockHeight == 0);
    }

    MtlSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspects,
      dstSubresource.mipLevel,
      dstSubresource.baseLayer, 1 };

    MtlSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspects,
      srcSubresource.mipLevel,
      srcSubresource.baseLayer, 1 };

    struct MtlBlitRegion {
      MtlSubresourceLayers dstSubresource;
      MtlSubresourceLayers srcSubresource;
      MtlOffset3D          dstOffsets[2];
      MtlOffset3D          srcOffsets[2];
    } blitInfo;
    blitInfo.dstSubresource = dstSubresourceLayers;
    blitInfo.srcSubresource = srcSubresourceLayers;

    blitInfo.dstOffsets[0] = pDestRect != nullptr
      ? MtlOffset3D{ int32_t(pDestRect->left), int32_t(pDestRect->top), 0 }
      : MtlOffset3D{ 0,                        0,                       0 };

    blitInfo.dstOffsets[1] = pDestRect != nullptr
      ? MtlOffset3D{ int32_t(pDestRect->right), int32_t(pDestRect->bottom), 1 }
      : MtlOffset3D{ int32_t(dstExtent.width),  int32_t(dstExtent.height),  1 };

    blitInfo.srcOffsets[0] = pSourceRect != nullptr
      ? MtlOffset3D{ int32_t(pSourceRect->left), int32_t(pSourceRect->top), 0 }
      : MtlOffset3D{ 0,                          0,                         0 };

    blitInfo.srcOffsets[1] = pSourceRect != nullptr
      ? MtlOffset3D{ int32_t(pSourceRect->right), int32_t(pSourceRect->bottom), 1 }
      : MtlOffset3D{ int32_t(srcExtent.width),    int32_t(srcExtent.height),    1 };

    if (unlikely(IsBlitRegionInvalid(blitInfo.srcOffsets, srcExtent)))
      return D3DERR_INVALIDCALL;

    if (unlikely(IsBlitRegionInvalid(blitInfo.dstOffsets, dstExtent)))
      return D3DERR_INVALIDCALL;

    MtlExtent3D srcCopyExtent =
    { uint32_t(blitInfo.srcOffsets[1].x - blitInfo.srcOffsets[0].x),
      uint32_t(blitInfo.srcOffsets[1].y - blitInfo.srcOffsets[0].y),
      uint32_t(blitInfo.srcOffsets[1].z - blitInfo.srcOffsets[0].z) };

    MtlExtent3D dstCopyExtent =
    { uint32_t(blitInfo.dstOffsets[1].x - blitInfo.dstOffsets[0].x),
      uint32_t(blitInfo.dstOffsets[1].y - blitInfo.dstOffsets[0].y),
      uint32_t(blitInfo.dstOffsets[1].z - blitInfo.dstOffsets[0].z) };

    bool srcIsDS = IsDepthStencilFormat(srcFormat);
    bool dstIsDS = IsDepthStencilFormat(dstFormat);
    if (unlikely(srcIsDS || dstIsDS)) {
      if (unlikely(!srcIsDS || !dstIsDS))
        return D3DERR_INVALIDCALL;

      if (unlikely(srcTextureInfo->Desc()->Discard || dstTextureInfo->Desc()->Discard))
        return D3DERR_INVALIDCALL;

      if (unlikely(srcCopyExtent.width != srcExtent.width || srcCopyExtent.height != srcExtent.height))
        return D3DERR_INVALIDCALL;

      if (unlikely(m_inScene))
        return D3DERR_INVALIDCALL;
    }

    bool stretch = srcCopyExtent != dstCopyExtent;

    bool dstHasAttachmentUsage = (dstTextureInfo->Desc()->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
    bool dstIsSurface = dstTextureInfo->GetType() == D3DRTYPE_SURFACE;
    if (stretch) {
      if (unlikely(pSourceSurface == pDestSurface))
        return D3DERR_INVALIDCALL;

      if (unlikely(dstIsDS))
        return D3DERR_INVALIDCALL;

      if (unlikely(!dstIsSurface && !dstHasAttachmentUsage))
        return D3DERR_INVALIDCALL;
    } else {
      bool srcIsSurface = srcTextureInfo->GetType() == D3DRTYPE_SURFACE;
      bool srcHasAttachmentUsage = (srcTextureInfo->Desc()->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;

      bool isCopy = IsExtended()
        && pSourceRect == nullptr && pDestRect == nullptr
        && srcTextureInfo->Desc()->Pool == D3DPOOL_DEFAULT
        && dstTextureInfo->Desc()->Pool == D3DPOOL_DEFAULT
        && srcTextureInfo->Desc()->Format == dstTextureInfo->Desc()->Format;

      if (unlikely((!dstHasAttachmentUsage && (!dstIsSurface || !srcIsSurface || srcHasAttachmentUsage)) && !m_isD3D8Compatible && !isCopy))
        return D3DERR_INVALIDCALL;
    }

    fastPath &= !stretch;

    if (!fastPath || needsResolve) {
      if (dstFormatInfo->flags & MtlFormatFlagCompressed)
        return D3DERR_INVALIDCALL;
    }

    if (fastPath) {
      if (needsResolve) {
        MtlResolveRegion region;
        region.srcSubresource = blitInfo.srcSubresource;
        region.srcOffset      = MtlOrigin{ blitInfo.srcOffsets[0].x, blitInfo.srcOffsets[0].y, blitInfo.srcOffsets[0].z };
        region.dstSubresource = blitInfo.dstSubresource;
        region.dstOffset      = MtlOrigin{ blitInfo.dstOffsets[0].x, blitInfo.dstOffsets[0].y, blitInfo.dstOffsets[0].z };
        region.extent         = srcCopyExtent;

        EmitCs([
          cDstImage    = dstImage,
          cSrcImage    = srcImage,
          cRegion      = region
        ] (MtlContext* ctx) {
          ctx->resolveTexture(cDstImage, cSrcImage, cRegion, cSrcImage->info().format,
            MtlResolveMode::Average, MtlResolveMode::SampleZero);
        });
      } else {
        EmitCs([
          cDstImage  = dstImage,
          cSrcImage  = srcImage,
          cDstLayers = blitInfo.dstSubresource,
          cSrcLayers = blitInfo.srcSubresource,
          cDstOffset = MtlOrigin{ blitInfo.dstOffsets[0].x, blitInfo.dstOffsets[0].y, blitInfo.dstOffsets[0].z },
          cSrcOffset = MtlOrigin{ blitInfo.srcOffsets[0].x, blitInfo.srcOffsets[0].y, blitInfo.srcOffsets[0].z },
          cExtent    = srcCopyExtent
        ] (MtlContext* ctx) {
          ctx->copyTexture(
            cDstImage, cDstLayers, cDstOffset,
            cSrcImage, cSrcLayers, cSrcOffset,
            cExtent);
        });
      }
    }
    else {
      MtlImageViewKey dstViewInfo;
      dstViewInfo.viewType = MtlTextureViewType::e2DArray;
      dstViewInfo.usage = MtlTextureUsageRenderTarget;
      dstViewInfo.format = dstImage->info().format;
      dstViewInfo.aspects = blitInfo.dstSubresource.aspects;
      dstViewInfo.baseMip = blitInfo.dstSubresource.mipLevel;
      dstViewInfo.mipCount = 1;
      dstViewInfo.baseLayer = blitInfo.dstSubresource.baseLayer;
      dstViewInfo.layerCount = blitInfo.dstSubresource.layerCount;
      dstViewInfo.packedSwizzle = MtlImageViewKey::packSwizzle(dstTextureInfo->GetMapping().Swizzle);

      MtlImageViewKey srcViewInfo;
      srcViewInfo.viewType = blitInfo.srcSubresource.layerCount > 1u
        ? MtlTextureViewType::e2DArray : MtlTextureViewType::e2D;
      srcViewInfo.usage = MtlTextureUsageShaderRead;
      srcViewInfo.format = srcImage->info().format;
      srcViewInfo.aspects = blitInfo.srcSubresource.aspects;
      srcViewInfo.baseMip = blitInfo.srcSubresource.mipLevel;
      srcViewInfo.mipCount = 1;
      srcViewInfo.baseLayer = blitInfo.srcSubresource.baseLayer;
      srcViewInfo.layerCount = blitInfo.srcSubresource.layerCount;
      srcViewInfo.packedSwizzle = MtlImageViewKey::packSwizzle(srcTextureInfo->GetMapping().Swizzle);

      EmitCs([
        cDstView  = dstImage->createView(dstViewInfo),
        cSrcView  = srcImage->createView(srcViewInfo),
        cBlitInfo = blitInfo,
        cFilter   = stretch ? DecodeFilter(Filter) : MtlSamplerFilter::Nearest
      ] (MtlContext* ctx) {
        ctx->blitTextureView(
          cDstView, cBlitInfo.dstOffsets,
          cSrcView, cBlitInfo.srcOffsets,
          cFilter);
      });
    }

    dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

    if (dstTextureInfo->IsAutomaticMip())
      MarkTextureMipsDirty(dstTextureInfo);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ColorFill(
          IDirect3DSurface9* pSurface,
    const RECT*              pRect,
          D3DCOLOR           Color) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* dst = static_cast<D3D9Surface*>(pSurface);

    if (unlikely(dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();

    if (dstTextureInfo->IsNull())
      return D3D_OK;

    if (unlikely(dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    MtlExtent3D mipExtent = dstTextureInfo->GetExtentMip(dst->GetSubresource());

    MtlOrigin offset = MtlOrigin{ 0u, 0u, 0u };
    MtlSize extent = mipExtent;

    if (pRect != nullptr)
      ConvertRect(*pRect, offset, extent);

    MtlClearValue clearValue = { };
    DecodeD3DCOLOR(Color, clearValue.color.float32);

    Rc<MtlTexture> image = dstTextureInfo->GetImage();

    if (image->formatInfo()->aspects != MtlAspectColor)
      return D3DERR_INVALIDCALL;

    MtlSubresourceLayers subresource = { };
    subresource.aspects = MtlAspectColor;
    subresource.mipLevel = dst->GetMipLevel();

    if (dst->GetFace() == D3D9CommonTexture::AllLayers) {
      subresource.baseLayer = 0u;
      subresource.layerCount = image->info().numLayers;
    } else {
      subresource.baseLayer = dst->GetFace();
      subresource.layerCount = 1u;
    }

    if (image->formatInfo()->flags & MtlFormatFlagCompressed) {
      EmitCs([
        cImage      = std::move(image),
        cSubresource = subresource,
        cOffset     = offset,
        cExtent     = extent,
        cClearValue = clearValue
      ] (MtlContext* ctx) {
        auto formatInfo = cImage->formatInfo();

        MtlFormat blockFormat = formatInfo->elementSize == 16u
          ? MtlFormat::RGBA32Float
          : MtlFormat::RG32Uint;

        MtlTextureUsage usage = MtlTextureUsageShaderWrite | MtlTextureUsagePixelView;

        ctx->ensureTextureUsage(cImage, usage);

        MtlImageViewKey viewKey = { };
        viewKey.viewType = MtlTextureViewType::e2DArray;
        viewKey.format = blockFormat;
        viewKey.usage = MtlTextureUsageShaderWrite;
        viewKey.aspects = cSubresource.aspects;
        viewKey.baseMip = cSubresource.mipLevel;
        viewKey.mipCount = 1u;
        viewKey.baseLayer = cSubresource.baseLayer;
        viewKey.layerCount = cSubresource.layerCount;

        Rc<MtlTextureView> view = cImage->createView(viewKey);

        MtlClearValue clearBlock = { };
        clearBlock.color = util::encodeClearBlockValue(cImage->info().format, cClearValue.color);

        MtlOrigin offset = util::computeBlockOffset(cOffset, util::formatBlockSize(formatInfo));
        MtlExtent3D extent = util::computeBlockExtent(cExtent, util::formatBlockSize(formatInfo));

        ctx->clearImageView(view, offset, extent,
          MtlAspectColor, clearBlock);
      });
    } else {
      EmitCs([
        cImage      = std::move(image),
        cSubresource = subresource,
        cOffset     = offset,
        cExtent     = extent,
        cClearValue = clearValue
      ] (MtlContext* ctx) {
        MtlTextureUsage usage = MtlTextureUsageRenderTarget;

        ctx->ensureTextureUsage(cImage, usage);

        MtlImageViewKey viewKey = { };
        viewKey.viewType = MtlTextureViewType::e2DArray;
        viewKey.format = cImage->info().format;
        viewKey.usage = MtlTextureUsageRenderTarget;
        viewKey.aspects = cSubresource.aspects;
        viewKey.baseMip = cSubresource.mipLevel;
        viewKey.mipCount = 1u;
        viewKey.baseLayer = cSubresource.baseLayer;
        viewKey.layerCount = cSubresource.layerCount;

        Rc<MtlTextureView> view = cImage->createView(viewKey);

        if (cOffset == MtlOrigin() && cExtent == cImage->mipLevelExtent(viewKey.baseMip)) {
          ctx->clearRenderTarget(view, cSubresource.aspects, cClearValue, 0u);
        } else {
          ctx->clearImageView(view, cOffset, cExtent,
            cSubresource.aspects, cClearValue);
        }
      });
    }

    dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

    if (dstTextureInfo->IsAutomaticMip())
      MarkTextureMipsDirty(dstTextureInfo);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurface(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle) {
    return CreateOffscreenPlainSurfaceEx(
      Width,     Height,
      Format,    Pool,
      ppSurface, pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderTarget(
          DWORD              RenderTargetIndex,
          IDirect3DSurface9* pRenderTarget) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pRenderTarget == nullptr && RenderTargetIndex == 0))
      return D3DERR_INVALIDCALL;

    D3D9Surface* rt = static_cast<D3D9Surface*>(pRenderTarget);
    if (unlikely(rt != nullptr && rt->GetDevice() != this))
      return D3DERR_INVALIDCALL;

    return SetRenderTargetInternal(RenderTargetIndex, pRenderTarget);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderTargetInternal(
          DWORD              RenderTargetIndex,
          IDirect3DSurface9* pRenderTarget) {
    if (unlikely(RenderTargetIndex >= caps::MaxSimultaneousRenderTargets))
      return D3DERR_INVALIDCALL;

    D3D9Surface* rt = static_cast<D3D9Surface*>(pRenderTarget);
    D3D9CommonTexture* texInfo = rt != nullptr
      ? rt->GetCommonTexture()
      : nullptr;

    if (unlikely(rt != nullptr && !(texInfo->Desc()->Usage & D3DUSAGE_RENDERTARGET)))
      return D3DERR_INVALIDCALL;

    if (RenderTargetIndex == 0) {
      D3DVIEWPORT9 viewport;
      viewport.X       = 0;
      viewport.Y       = 0;
      viewport.MinZ    = 0.0f;
      viewport.MaxZ    = 1.0f;

      RECT scissorRect;
      scissorRect.left    = 0;
      scissorRect.top     = 0;

      if (likely(rt != nullptr)) {
        auto rtSize = rt->GetSurfaceExtent();
        viewport.Width  = rtSize.width;
        viewport.Height = rtSize.height;
        scissorRect.right  = rtSize.width;
        scissorRect.bottom = rtSize.height;
      } else {
        viewport.Width  = 0;
        viewport.Height = 0;
        scissorRect.right  = 0;
        scissorRect.bottom = 0;
      }

      if (m_state.viewport != viewport) {
        m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
        m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
        m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
        m_state.viewport = viewport;
      }

      if (m_state.scissorRect != scissorRect) {
        m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
        m_state.scissorRect = scissorRect;
      }

      if (m_specData.setAlphaPrecision(GetAlphaTestPrecision(rt)))
        m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }

    if (m_state.renderTargets[RenderTargetIndex] == rt)
      return D3D_OK;

    m_state.renderTargets[RenderTargetIndex] = rt;

    ConsiderFlush(RenderTargetIndex == 0
      ? GpuFlushType::ImplicitStrongHint
      : GpuFlushType::ImplicitWeakHint);

    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);

    uint32_t oldAlphaSwizzleRTs = m_rtSlotTracking.hasAlphaSwizzle;
    const uint32_t bit = 1u << RenderTargetIndex;
    m_rtSlotTracking.canBeSampled    &= ~bit;
    m_rtSlotTracking.hasAlphaSwizzle &= ~bit;

    if (texInfo != nullptr) {
      m_rtSlotTracking.canBeSampled |= uint8_t(!texInfo->IsNull() &&
        rt->GetBaseTexture() != nullptr) << RenderTargetIndex;

      m_rtSlotTracking.hasAlphaSwizzle |= uint8_t(texInfo->GetMapping().Swizzle.a == MtlSwizzle::One) << RenderTargetIndex;

      if (texInfo->IsAutomaticMip())
        texInfo->SetNeedsMipGen(true);
    }


    UpdateActiveHazardsRT(std::numeric_limits<uint32_t>::max());

    if (oldAlphaSwizzleRTs != m_rtSlotTracking.hasAlphaSwizzle)
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);

    if (RenderTargetIndex == 0) {
      UpdateAlphaToCoverangeAndAlphaTest();

      if (likely(texInfo != nullptr)) {
        if (m_alphaTestEnabled)
          m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);

        bool oldValidSampleMask = m_validSampleMask;
        m_validSampleMask = texInfo->Desc()->MultiSample > D3DMULTISAMPLE_NONMASKABLE;
        if (m_validSampleMask != oldValidSampleMask)
          m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
      } else {
        m_validSampleMask = false;
        m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
        m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
      }

      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTarget(
          DWORD               RenderTargetIndex,
          IDirect3DSurface9** ppRenderTarget) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppRenderTarget);

    if (unlikely(ppRenderTarget == nullptr || RenderTargetIndex > caps::MaxSimultaneousRenderTargets))
      return D3DERR_INVALIDCALL;

    if (m_state.renderTargets[RenderTargetIndex] == nullptr)
      return D3DERR_NOTFOUND;

    *ppRenderTarget = m_state.renderTargets[RenderTargetIndex].ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* ds = static_cast<D3D9Surface*>(pNewZStencil);

    if (unlikely(ds && !(ds->GetCommonTexture()->Desc()->Usage & D3DUSAGE_DEPTHSTENCIL)))
      return D3DERR_INVALIDCALL;

    if (m_state.depthStencil == ds)
      return D3D_OK;

    ConsiderFlush(GpuFlushType::ImplicitWeakHint);
    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);


    m_state.depthStencil = ds;

    UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppZStencilSurface);

    if (unlikely(ppZStencilSurface == nullptr))
      return D3DERR_INVALIDCALL;

    if (m_state.depthStencil == nullptr)
      return D3DERR_NOTFOUND;

    *ppZStencilSurface = m_state.depthStencil.ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginScene() {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(m_inScene))
      return D3DERR_INVALIDCALL;

    m_inScene = true;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EndScene() {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(!m_inScene))
      return D3DERR_INVALIDCALL;

    ConsiderFlush(GpuFlushType::ImplicitStrongHint);

    m_inScene = false;

    if (m_state.indices == nullptr) {
      EmitCs([](MtlContext* ctx) {
        ctx->bindIndexBuffer(MtlBufferSlice(), MtlIndexType::Uint32);
      });
    }

    for (uint32_t i : bit::BitMask(~static_cast<uint32_t>(m_vbSlotTracking.bound) & ((1 << 16) - 1))) {
      if (m_state.vertexBuffers[i].vertexBuffer == nullptr) {
        EmitCs([cIndex = i](MtlContext* ctx) {
          ctx->bindVertexBuffer(cIndex, MtlBufferSlice(), 0);
        });
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Clear(
          DWORD    Count,
    const D3DRECT* pRects,
          DWORD    Flags,
          D3DCOLOR Color,
          float    Z,
          DWORD    Stencil) {
    if (unlikely(!Count && pRects))
      return D3D_OK;

    if (d9mtTrace())
      Logger::info(str::format("D3D9::Clear Flags=0x", std::hex, Flags, std::dec,
        " (TARGET=", (Flags & D3DCLEAR_TARGET) ? 1 : 0,
        " ZB=", (Flags & D3DCLEAR_ZBUFFER) ? 1 : 0,
        ") Count=", Count, " Color=0x", std::hex, Color, std::dec));

    D3D9DeviceLock lock = LockDevice();
    BindFramebuffer();

    if (unlikely(m_state.depthStencil == nullptr
            && (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))))
      return D3DERR_INVALIDCALL;

    const auto& vp = m_state.viewport;
    const auto& sc = m_state.scissorRect;

    bool srgb      = m_state.renderStates[D3DRS_SRGBWRITEENABLE];
    bool scissor   = m_state.renderStates[D3DRS_SCISSORTESTENABLE];

    MtlOrigin offset = { int32_t(vp.X),    int32_t(vp.Y),      0  };
    MtlExtent3D extent = {         vp.Width,         vp.Height,  1u };

    if (scissor) {
      offset.x = std::max<int32_t> (offset.x, sc.left);
      offset.y = std::max<int32_t> (offset.y, sc.top);

      extent.width  = std::min<uint32_t>(extent.width,  sc.right  - offset.x);
      extent.height = std::min<uint32_t>(extent.height, sc.bottom - offset.y);
    }

    if (Count) {
      if (!pRects)
        Count = 0;
      else if (pRects[0].x1 <= offset.x                         && pRects[0].y1 <= offset.y
            && pRects[0].x2 >= offset.x + int32_t(extent.width) && pRects[0].y2 >= offset.y + int32_t(extent.height))
        Count = 0;
    }

    MtlClearValue clearValueDepth;
    clearValueDepth.depth   = Z;
    clearValueDepth.stencil = Stencil;

    MtlClearValue clearValueColor;
    DecodeD3DCOLOR(Color, clearValueColor.color.float32);

    MtlAspectMask depthAspectMask = 0;
    if (m_state.depthStencil != nullptr) {
      if (Flags & D3DCLEAR_ZBUFFER)
        depthAspectMask |= MtlAspectDepth;

      if (Flags & D3DCLEAR_STENCIL)
        depthAspectMask |= MtlAspectStencil;

      depthAspectMask &= lookupFormatInfo(m_state.depthStencil->GetCommonTexture()->GetFormatMapping().FormatColor)->aspects;
    }

    auto ClearImageView = [this](
      uint32_t                 alignment,
      MtlOrigin               offset,
      MtlExtent3D               extent,
      const Rc<MtlTextureView>& imageView,
      MtlAspectMask       aspectMask,
      MtlClearValue             clearValue) {

      MtlExtent3D imageExtent = imageView->image->mipLevelExtent(imageView->key.baseMip);
      extent.width = std::min(imageExtent.width, extent.width);
      extent.height = std::min(imageExtent.height, extent.height);

      if (unlikely(uint32_t(offset.x) >= imageExtent.width || uint32_t(offset.y) >= imageExtent.height))
        return;

      const bool fullClear = align(extent.width, alignment) == align(imageExtent.width, alignment)
        && align(extent.height, alignment) == align(imageExtent.height, alignment)
        && offset.x == 0
        && offset.y == 0;

      if (fullClear) {
        EmitCs([
          cClearValue = clearValue,
          cAspectMask = aspectMask,
          cImageView  = imageView
        ] (MtlContext* ctx) {
          ctx->clearRenderTarget(cImageView,
            cAspectMask, cClearValue, 0u);
        });
      }
      else {
        EmitCs([
          cClearValue = clearValue,
          cAspectMask = aspectMask,
          cImageView  = imageView,
          cOffset     = offset,
          cExtent     = extent
        ] (MtlContext* ctx) {
          ctx->clearImageView(
            cImageView,
            cOffset, cExtent,
            cAspectMask,
            cClearValue);
        });
      }
    };

    auto ClearViewRect = [&](
      uint32_t           alignment,
      MtlOrigin         offset,
      MtlExtent3D         extent) {
      if (depthAspectMask != 0)
        ClearImageView(alignment, offset, extent, m_state.depthStencil->GetDepthStencilView(true), depthAspectMask, clearValueDepth);

      if (Flags & D3DCLEAR_TARGET) {
        for (uint32_t rt = 0u; rt < m_state.renderTargets.size(); rt++) {
          if (!HasRenderTargetBound(rt))
            continue;
          const auto& rts = m_state.renderTargets[rt];
          const auto& rtv = rts->GetRenderTargetView(srgb);

          if (likely(rtv != nullptr)) {
            ClearImageView(alignment, offset, extent, rtv, MtlAspectColor, clearValueColor);

            D3D9CommonTexture* dstTexture = rts->GetCommonTexture();

            if (dstTexture->IsAutomaticMip())
              MarkTextureMipsDirty(dstTexture);
          }
        }
      }
    };

    uint32_t alignment = m_d3d9Options.lenientClear ? 8 : 1;

    if (extent.width == 0 || extent.height == 0) {
      return D3D_OK;
    }

    if (!Count) {
      ClearViewRect(alignment, offset, extent);
    }
    else {
      for (uint32_t i = 0; i < Count; i++) {
        MtlOrigin rectOffset = {
          std::max<int32_t>(pRects[i].x1, offset.x),
          std::max<int32_t>(pRects[i].y1, offset.y),
          0
        };

        if (std::min<int32_t>(pRects[i].x2, offset.x + extent.width) <= rectOffset.x
          || std::min<int32_t>(pRects[i].y2, offset.y + extent.height) <= rectOffset.y) {
          continue;
        }

        MtlExtent3D rectExtent = {
          std::min<uint32_t>(pRects[i].x2, offset.x + extent.width)  - rectOffset.x,
          std::min<uint32_t>(pRects[i].y2, offset.y + extent.height) - rectOffset.y,
          1u
        };

        ClearViewRect(alignment, rectOffset, rectExtent);
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    return SetStateTransform(GetTransformIndex(State), pMatrix);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMatrix == nullptr))
      return D3DERR_INVALIDCALL;

    *pMatrix = bit::cast<D3DMATRIX>(m_state.transforms[GetTransformIndex(State)]);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::MultiplyTransform(D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    const uint32_t idx = GetTransformIndex(TransformState);

    m_state.transforms[idx] = m_state.transforms[idx] * ConvertMatrix(pMatrix);

    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    if (idx == GetTransformIndex(D3DTS_VIEW) || idx >= GetTransformIndex(D3DTS_WORLD))
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetViewport(const D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pViewport == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetViewport(pViewport);

    if (m_state.viewport == *pViewport)
      return D3D_OK;

    m_state.viewport.X = pViewport->X;
    m_state.viewport.Y = pViewport->Y;
    m_state.viewport.Width = pViewport->Width;
    m_state.viewport.Height = pViewport->Height;
    m_state.viewport.MinZ = pViewport->MinZ;
    m_state.viewport.MaxZ = pViewport->MinZ < pViewport->MaxZ ? pViewport->MaxZ : pViewport->MinZ + 0.001f;

    m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
    m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
    m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetViewport(D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pViewport == nullptr))
      return D3DERR_INVALIDCALL;

    *pViewport = m_state.viewport;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetMaterial(const D3DMATERIAL9* pMaterial) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaterial == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetMaterial(pMaterial);

    m_state.material = *pMaterial;
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaterial(D3DMATERIAL9* pMaterial) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaterial == nullptr))
      return D3DERR_INVALIDCALL;

    *pMaterial = m_state.material;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pLight == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord())) {
      m_recorder->SetLight(Index, pLight);
      return D3D_OK;
    }

    if (Index >= m_state.lights.size())
      m_state.lights.resize(Index + 1);

    auto& light = m_state.lights[Index];
    light.isValid = true;
    light.light = *pLight;

    if (light.isEnabled)
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLight(DWORD Index, D3DLIGHT9* pLight) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pLight == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= m_state.lights.size()))
      return D3DERR_INVALIDCALL;

    auto& light = m_state.lights[Index];

    if (unlikely(!light.isValid))
      return D3DERR_INVALIDCALL;

    *pLight = m_state.lights[Index].light;
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::LightEnable(DWORD Index, BOOL Enable) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord())) {
      m_recorder->LightEnable(Index, Enable);
      return D3D_OK;
    }

    if (unlikely(Index >= m_state.lights.size()))
      m_state.lights.resize(Index + 1);

    auto& light = m_state.lights[Index];

    if (light.isEnabled == bool(Enable))
      return D3D_OK;

    light.isValid = true;
    light.isEnabled = bool(Enable);

    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLightEnable(DWORD Index, BOOL* pEnable) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pEnable == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= m_state.lights.size()))
      return D3DERR_INVALIDCALL;

    auto& light = m_state.lights[Index];

    if (unlikely(!light.isValid))
      return D3DERR_INVALIDCALL;

    *pEnable = light.isEnabled ? 128 : 0;
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetClipPlane(DWORD Index, const float* pPlane) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(!pPlane))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= caps::MaxClipPlanes))
      Index = caps::MaxClipPlanes - 1;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetClipPlane(Index, pPlane);

    bool dirty = false;

    for (uint32_t i = 0; i < 4; i++) {
      dirty |= m_state.clipPlanes[Index].coeff[i] != pPlane[i];
      m_state.clipPlanes[Index].coeff[i] = pPlane[i];
    }

    bool enabled = m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1u << Index);
    dirty &= enabled;

    if (dirty)
      m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetClipPlane(DWORD Index, float* pPlane) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(!pPlane))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= caps::MaxClipPlanes))
      Index = caps::MaxClipPlanes - 1;

    for (uint32_t i = 0; i < 4; i++)
      pPlane[i] = m_state.clipPlanes[Index].coeff[i];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
      return D3D_OK;
    }

    if (unlikely(ShouldRecord()))
      return m_recorder->SetRenderState(State, Value);

    auto& states         = m_state.renderStates;
    const DWORD oldValue = states[State];

    if (likely(Value != oldValue)) {
      constexpr uint32_t nvidiaVendorId = uint32_t(MtlGpuVendor::Nvidia);
      constexpr uint32_t amdVendorId    = uint32_t(MtlGpuVendor::Amd);
      constexpr uint32_t intelVendorId  = uint32_t(MtlGpuVendor::Intel);

      states[State] = Value;

      switch (State) {
        case D3DRS_SEPARATEALPHABLENDENABLE:
        case D3DRS_ALPHABLENDENABLE:
        case D3DRS_BLENDOP:
        case D3DRS_BLENDOPALPHA:
        case D3DRS_DESTBLEND:
        case D3DRS_DESTBLENDALPHA:
        case D3DRS_SRCBLEND:
        case D3DRS_SRCBLENDALPHA:
          m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
          break;

        case D3DRS_COLORWRITEENABLE:
          if (likely(!Value != !oldValue))
            UpdateAnyColorWrites<0>();
          m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
          break;
        case D3DRS_COLORWRITEENABLE1:
          if (likely(!Value != !oldValue))
            UpdateAnyColorWrites<1>();
          m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
          break;
        case D3DRS_COLORWRITEENABLE2:
          if (likely(!Value != !oldValue))
            UpdateAnyColorWrites<2>();
          m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
          break;
        case D3DRS_COLORWRITEENABLE3:
          if (likely(!Value != !oldValue))
            UpdateAnyColorWrites<3>();
          m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
          break;

        case D3DRS_ALPHATESTENABLE: {
          UpdateAlphaToCoverangeAndAlphaTest();
          break;
        }

        case D3DRS_ALPHAFUNC:
          m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
          break;

        case D3DRS_BLENDFACTOR:
          BindBlendFactor();
          break;

        case D3DRS_MULTISAMPLEMASK:
          if (m_validSampleMask)
            m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
          break;

        case D3DRS_ZWRITEENABLE:
          if (likely(!Value != !oldValue)) {
            if (likely(m_state.depthStencil != nullptr
              && m_state.renderStates[D3DRS_ZENABLE])) {
              UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());
            }

            m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
          }
          break;

        case D3DRS_STENCILENABLE:
          if (likely(!Value != !oldValue)) {
            m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
          }
          break;

        case D3DRS_ZENABLE:
          if (likely(!Value != !oldValue)) {
            if (likely(m_state.depthStencil != nullptr)) {
              UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());
            }

            m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
          }
          break;

        case D3DRS_ZFUNC:
        case D3DRS_TWOSIDEDSTENCILMODE:
        case D3DRS_STENCILFAIL:
        case D3DRS_STENCILZFAIL:
        case D3DRS_STENCILPASS:
        case D3DRS_STENCILFUNC:
        case D3DRS_CCW_STENCILFAIL:
        case D3DRS_CCW_STENCILZFAIL:
        case D3DRS_CCW_STENCILPASS:
        case D3DRS_CCW_STENCILFUNC:
        case D3DRS_STENCILMASK:
        case D3DRS_STENCILWRITEMASK:
          m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
          break;

        case D3DRS_STENCILREF:
          BindDepthStencilReference();
          break;

        case D3DRS_SCISSORTESTENABLE:
          m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
          break;

        case D3DRS_SRGBWRITEENABLE:
          m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
          break;

        case D3DRS_DEPTHBIAS:
        case D3DRS_SLOPESCALEDEPTHBIAS:
          m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
          break;

        case D3DRS_CULLMODE:
        case D3DRS_FILLMODE:
        case D3DRS_MULTISAMPLEANTIALIAS:
          m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
          break;

        case D3DRS_CLIPPLANEENABLE:
          if (!Value != !oldValue)
            m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

          m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);
          break;

        case D3DRS_ALPHAREF:
          m_pushData.shared.alphaRef = Value;
          m_dirty.set(D3D9DeviceDirtyFlag::PushDataShared);
          break;

        case D3DRS_TEXTUREFACTOR:
          m_pushData.ffps.textureFactor = Value;
          m_dirty.set(D3D9DeviceDirtyFlag::PushDataFfps);
          break;

        case D3DRS_DIFFUSEMATERIALSOURCE:
        case D3DRS_AMBIENTMATERIALSOURCE:
        case D3DRS_SPECULARMATERIALSOURCE:
        case D3DRS_EMISSIVEMATERIALSOURCE:
        case D3DRS_COLORVERTEX:
        case D3DRS_LIGHTING:
        case D3DRS_NORMALIZENORMALS:
        case D3DRS_LOCALVIEWER:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_AMBIENT:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_SPECULARENABLE:
          if (m_specData.setGlobalSpecular(bool(Value)))
            m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
          break;

        case D3DRS_FOGENABLE:
        case D3DRS_FOGVERTEXMODE:
        case D3DRS_FOGTABLEMODE:
        case D3DRS_FOGCOLOR:
        case D3DRS_FOGSTART:
        case D3DRS_FOGEND:
        case D3DRS_FOGDENSITY:
          m_dirty.set(D3D9DeviceDirtyFlag::Fog);
          break;

        case D3DRS_RANGEFOGENABLE:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_POINTSIZE: {
          // AMD/Nvidia expose ATOC, RESZ and related driver hacks as magic values
          // written through D3DRS_POINTSIZE; real point-size values never collide with them.
          const uint32_t vendorId = m_adapter->GetVendorId();

          if (likely(vendorId != intelVendorId)) {
            constexpr uint32_t AlphaToCoverageEnable  = uint32_t(D3D9Format::A2M1);
            constexpr uint32_t AlphaToCoverageDisable = uint32_t(D3D9Format::A2M0);

            if ((Value == AlphaToCoverageEnable
              || Value == AlphaToCoverageDisable) &&
                 vendorId == amdVendorId &&
                 !m_isD3D8Compatible) {
              UpdateAlphaToCoverangeAndAlphaTest();
              break;
            }

            constexpr uint32_t RESZ = 0x7fa05000;
            if (unlikely(Value == RESZ && vendorId == amdVendorId)) {
              ResolveZ();
              break;
            }

            if (unlikely(Value == uint32_t(D3D9Format::INST) &&
                         vendorId == amdVendorId &&
                         !m_isD3D8Compatible)) {
              break;
            }

            if (unlikely(Value == uint32_t(D3D9Format::CENT) &&
                         !m_isD3D8Compatible)) {
              break;
            }
          }

          m_pushData.vs.pointSize = EncodePointSize(Value);
          m_dirty.set(D3D9DeviceDirtyFlag::PushDataVs);
          break;
        }

        case D3DRS_POINTSIZE_MIN:
          m_pushData.vs.pointSizeMin = EncodePointSize(Value);
          m_dirty.set(D3D9DeviceDirtyFlag::PushDataVs);
          break;

        case D3DRS_POINTSIZE_MAX:
          m_pushData.vs.pointSizeMax = EncodePointSize(Value);
          m_dirty.set(D3D9DeviceDirtyFlag::PushDataVs);
          break;

        case D3DRS_POINTSCALE_A:
        case D3DRS_POINTSCALE_B:
        case D3DRS_POINTSCALE_C:
          m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
          break;

        case D3DRS_POINTSCALEENABLE:
        case D3DRS_POINTSPRITEENABLE:
          break;

        case D3DRS_SHADEMODE:
          m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
          break;

        case D3DRS_TWEENFACTOR:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_VERTEXBLEND:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_INDEXEDVERTEXBLENDENABLE:
          if (CanSWVP() && Value)
            m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case D3DRS_ADAPTIVETESS_Y: {
          const uint32_t vendorId = m_adapter->GetVendorId();

          if (likely(vendorId != amdVendorId && !m_isD3D8Compatible)) {
            constexpr uint32_t AlphaToCoverageEnable  = uint32_t(D3D9Format::ATOC);
            constexpr uint32_t AlphaToCoverageDisable = 0;

            if (Value == AlphaToCoverageEnable
             || Value == AlphaToCoverageDisable) {
              UpdateAlphaToCoverangeAndAlphaTest();
              break;
            }

            if (unlikely(Value == uint32_t(D3D9Format::COPM) &&
                         vendorId == nvidiaVendorId)) {
              static bool s_copmErrorShown;

              if (!std::exchange(s_copmErrorShown, true))
                Logger::info("D3D9DeviceEx::SetRenderState: MinimalNVIDIADriverShaderOptimization is unsupported");

              break;
            }

            if (unlikely(Value == uint32_t(D3D9Format::SSAA) &&
                         vendorId == nvidiaVendorId)) {
              static bool s_ssaaErrorShown;

              if (!std::exchange(s_ssaaErrorShown, true))
                Logger::warn("D3D9DeviceEx::SetRenderState: Transparency supersampling (SSAA) is unsupported");

              break;
            }
          }
          break;
        }

        case D3DRS_ADAPTIVETESS_X:
        case D3DRS_ADAPTIVETESS_Z:
        case D3DRS_ADAPTIVETESS_W: {
          const uint32_t vendorId = m_adapter->GetVendorId();

          const bool nvdbEnabled = vendorId == nvidiaVendorId && IsNVDepthBoundsTestEnabled();

          if (nvdbEnabled || m_nvdbEnabled) {
            m_dirty.set(D3D9DeviceDirtyFlag::DepthBounds);

            if (likely(IsZTestEnabled()))
              m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);

            if (State == D3DRS_ADAPTIVETESS_X && nvdbEnabled != m_nvdbEnabled)
              m_nvdbEnabled = nvdbEnabled;
          }
          break;
        }

        default:
          static bool s_errorShown[256];

          if (!std::exchange(s_errorShown[State], true))
            Logger::warn(str::format("D3D9DeviceEx::SetRenderState: Unhandled render state ", State));
          break;
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
      return D3DERR_INVALIDCALL;
    }

    if (State < D3DRS_ZENABLE || State > D3DRS_BLENDOPALPHA)
      *pValue = 0;
    else
      *pValue = m_state.renderStates[State];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateStateBlock(
          D3DSTATEBLOCKTYPE      Type,
          IDirect3DStateBlock9** ppSB) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return D3DERR_INVALIDCALL;

    InitReturnPtr(ppSB);

    if (unlikely(ppSB == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9StateBlockType stateBlockType = ConvertStateBlockType(Type);

    if (unlikely(stateBlockType == D3D9StateBlockType::Unknown)) {
      Logger::warn(str::format("D3D9DeviceEx::CreateStateBlock: Invalid state block type: ", Type));
      return D3DERR_INVALIDCALL;
    }

    try {
      const Com<D3D9StateBlock> sb = new D3D9StateBlock(this, stateBlockType);
      *ppSB = sb.ref();
      if (!m_isD3D8Compatible)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginStateBlock() {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return D3DERR_INVALIDCALL;

    m_recorder = new D3D9StateBlock(this, D3D9StateBlockType::None);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EndStateBlock(IDirect3DStateBlock9** ppSB) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ppSB == nullptr || !ShouldRecord()))
      return D3DERR_INVALIDCALL;

    InitReturnPtr(ppSB);

    *ppSB = m_recorder.ref();
    if (!m_isD3D8Compatible)
      m_losableResourceCounter++;
    m_recorder = nullptr;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pClipStatus == nullptr))
      return D3DERR_INVALIDCALL;

    m_state.clipStatus = *pClipStatus;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pClipStatus == nullptr))
      return D3DERR_INVALIDCALL;

    *pClipStatus = m_state.clipStatus;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (ppTexture == nullptr)
      return D3DERR_INVALIDCALL;

    *ppTexture = nullptr;

    if (unlikely(InvalidSampler(Stage)))
      return D3D_OK;

    DWORD stateSampler = RemapSamplerState(Stage);

    *ppTexture = ref(m_state.textures[stateSampler]);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    if (unlikely(InvalidSampler(Stage)))
      return D3D_OK;

    DWORD stateSampler = RemapSamplerState(Stage);

    return SetStateTexture(stateSampler, pTexture);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTextureStageState(
          DWORD                    Stage,
          D3DTEXTURESTAGESTATETYPE Type,
          DWORD*                   pValue) {
    auto dxvkType = RemapTextureStageStateType(Type);

    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    Stage = std::min(Stage, DWORD(caps::TextureStageCount - 1));
    dxvkType = std::min(dxvkType, D3D9TextureStageStateTypes(DXVK_TSS_COUNT - 1));

    *pValue = m_state.textureStages[Stage][dxvkType];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTextureStageState(
          DWORD                    Stage,
          D3DTEXTURESTAGESTATETYPE Type,
          DWORD                    Value) {
    return SetStateTextureStageState(Stage, RemapTextureStageStateType(Type), Value);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetSamplerState(
          DWORD               Sampler,
          D3DSAMPLERSTATETYPE Type,
          DWORD*              pValue) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    *pValue = 0;

    if (unlikely(InvalidSampler(Sampler)))
      return D3D_OK;

    Sampler = RemapSamplerState(Sampler);

    *pValue = m_state.samplerStates[Sampler][Type];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetSamplerState(
          DWORD               Sampler,
          D3DSAMPLERSTATETYPE Type,
          DWORD               Value) {
    if (unlikely(InvalidSampler(Sampler)))
      return D3D_OK;

    uint32_t stateSampler = RemapSamplerState(Sampler);

    return SetStateSamplerState(stateSampler, Type, Value);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ValidateDevice(DWORD* pNumPasses) {
    D3D9DeviceLock lock = LockDevice();

    if (pNumPasses != nullptr)
      *pNumPasses = 1;

    return IsDeviceLost() ? D3DERR_DEVICELOST : D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pEntries == nullptr))
      return D3DERR_INVALIDCALL;

    auto texturePalettesIter = m_state.texturePalettes.find(PaletteNumber);

    if (likely(texturePalettesIter == m_state.texturePalettes.end())) {
      std::array<PALETTEENTRY, PaletteEntryCount> paletteEntry;
      memcpy(&paletteEntry[0], pEntries, sizeof(PALETTEENTRY) * PaletteEntryCount);

      m_state.texturePalettes.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(PaletteNumber),
                                      std::forward_as_tuple(paletteEntry));
    } else {
      memcpy(&texturePalettesIter->second[0], pEntries, sizeof(PALETTEENTRY) * PaletteEntryCount);
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pEntries == nullptr))
      return D3DERR_INVALIDCALL;

    auto texturePalettesIter = m_state.texturePalettes.find(PaletteNumber);

    if (unlikely(texturePalettesIter == m_state.texturePalettes.end())) {
      Logger::warn(str::format("D3D9DeviceEx::GetPaletteEntries: Invalid PaletteNumber: ", PaletteNumber));
      return D3DERR_INVALIDCALL;
    }

    memcpy(pEntries, &texturePalettesIter->second[0], sizeof(PALETTEENTRY) * PaletteEntryCount);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetCurrentTexturePalette(UINT PaletteNumber) {
    D3D9DeviceLock lock = LockDevice();

    m_state.texturePaletteNumber = PaletteNumber;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetCurrentTexturePalette(UINT *PaletteNumber) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(PaletteNumber == nullptr))
      return D3DERR_INVALIDCALL;

    *PaletteNumber = m_state.texturePaletteNumber;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetScissorRect(const RECT* pRect) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pRect == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetScissorRect(pRect);

    if (m_state.scissorRect == *pRect)
      return D3D_OK;

    m_state.scissorRect = *pRect;

    m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetScissorRect(RECT* pRect) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pRect == nullptr))
      return D3DERR_INVALIDCALL;

    *pRect = m_state.scissorRect;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetSoftwareVertexProcessing(BOOL bSoftware) {
    D3D9DeviceLock lock = LockDevice();

    if (bSoftware && !CanSWVP())
      return D3DERR_INVALIDCALL;

    if (!bSoftware && (m_behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
      return D3DERR_INVALIDCALL;

    m_isSWVP = bSoftware;

    return D3D_OK;
  }


  BOOL    STDMETHODCALLTYPE D3D9DeviceEx::GetSoftwareVertexProcessing() {
    D3D9DeviceLock lock = LockDevice();

    return m_isSWVP;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetNPatchMode(float nSegments) {
    D3D9DeviceLock lock = LockDevice();

    m_state.nPatchSegments = nSegments;

    return D3D_OK;
  }


  float   STDMETHODCALLTYPE D3D9DeviceEx::GetNPatchMode() {
    D3D9DeviceLock lock = LockDevice();

    return m_state.nPatchSegments;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitive(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             StartVertex,
          UINT             PrimitiveCount) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(m_state.vertexDecl == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!PrimitiveCount))
      return D3D_OK;

    bool dynamicSysmemVBOs = false;

    uint32_t firstIndex     = 0;
    int32_t baseVertexIndex = 0;
    uint32_t vertexCount    = GetVertexCount(PrimitiveType, PrimitiveCount);

    UploadPerDrawData(StartVertex, vertexCount, firstIndex, 0,
      baseVertexIndex, &dynamicSysmemVBOs, nullptr);

    PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, false);

    MtlDrawArgs draw = {};
    draw.vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
    draw.instanceCount = 1u;
    draw.firstVertex = StartVertex;

    if (m_csDataType == D3D9CmdType::Draw) {
      auto* drawArgs = m_csChunk->pushData(m_csData, 1u);

      if (likely(drawArgs)) {
        new (drawArgs) MtlDrawArgs(draw);
        return D3D_OK;
      }
    }

    EmitCsCmd<MtlDrawArgs>(D3D9CmdType::Draw, 1u,
      [this] (MtlContext* ctx, const MtlDrawArgs* drawArgs, uint32_t drawCount) {
        ctx->draw(drawCount, drawArgs);
      });

    new (m_csData->first()) MtlDrawArgs(draw);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitive(
          D3DPRIMITIVETYPE PrimitiveType,
          INT              BaseVertexIndex,
          UINT             MinVertexIndex,
          UINT             NumVertices,
          UINT             StartIndex,
          UINT             PrimitiveCount) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(m_state.vertexDecl == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!PrimitiveCount || !NumVertices))
      return D3D_OK;

    bool dynamicSysmemVBOs = false;
    bool dynamicSysmemIBO = false;

    uint32_t indexCount = GetVertexCount(PrimitiveType, PrimitiveCount);

    UploadPerDrawData(MinVertexIndex, NumVertices, StartIndex, indexCount,
      BaseVertexIndex, &dynamicSysmemVBOs, &dynamicSysmemIBO);

    PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, !dynamicSysmemIBO);

    MtlDrawIndexedArgs draw = {};
    draw.indexCount = indexCount;
    draw.instanceCount = GetInstanceCount();
    draw.firstIndex = StartIndex;
    draw.vertexOffset = BaseVertexIndex;

    if (m_csDataType == D3D9CmdType::DrawIndexed) {
      auto* drawArgs = m_csChunk->pushData(m_csData, 1u);

      if (likely(drawArgs)) {
        new (drawArgs) MtlDrawIndexedArgs(draw);
        return D3D_OK;
      }
    }

    EmitCsCmd<MtlDrawIndexedArgs>(D3D9CmdType::DrawIndexed, 1u,
      [this] (MtlContext* ctx, MtlDrawIndexedArgs* drawArgs, uint32_t drawCount) {
      if (unlikely(m_iaState.streamsInstanced && !(m_iaState.streamsInstanced & m_iaState.streamsUsed))) {
        for (uint32_t i = 0u; i < drawCount; i++)
          drawArgs[i].instanceCount = 1u;
      }

      ctx->drawIndexed(drawCount, drawArgs);
    });

    new (m_csData->first()) MtlDrawIndexedArgs(draw);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitiveUP(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             PrimitiveCount,
    const void*            pVertexStreamZeroData,
          UINT             VertexStreamZeroStride) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(!VertexStreamZeroStride))
      return D3DERR_INVALIDCALL;

    if (unlikely(m_state.vertexDecl == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!PrimitiveCount))
      return D3D_OK;

    PrepareDraw(PrimitiveType, false, false);

    uint32_t vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);

    const uint32_t dataSize = GetUPDataSize(vertexCount, VertexStreamZeroStride);
    const uint32_t bufferSize = GetUPBufferSize(vertexCount, VertexStreamZeroStride);

    auto upSlice = AllocUPBuffer(bufferSize);
    FillUPVertexBuffer(upSlice.mapPtr, pVertexStreamZeroData, dataSize, bufferSize);

    EmitCs([this,
      cBufferSlice  = std::move(upSlice.slice),
      cPrimType     = PrimitiveType,
      cStride       = VertexStreamZeroStride,
      cVertexCount  = vertexCount
    ](MtlContext* ctx) mutable {
      MtlDrawArgs draw = { };
      draw.vertexCount = cVertexCount;
      draw.instanceCount = 1u;

      ctx->bindVertexBuffer(0, std::move(cBufferSlice), cStride);
      ctx->draw(1u, &draw);
      ctx->bindVertexBuffer(0, MtlBufferSlice(), 0);
    });

    m_state.vertexBuffers[0].vertexBuffer = nullptr;
    m_state.vertexBuffers[0].offset       = 0;
    m_state.vertexBuffers[0].length       = 0;
    m_state.vertexBuffers[0].stride       = 0;
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitiveUP(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             MinVertexIndex,
          UINT             NumVertices,
          UINT             PrimitiveCount,
    const void*            pIndexData,
          D3DFORMAT        IndexDataFormat,
    const void*            pVertexStreamZeroData,
          UINT             VertexStreamZeroStride) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(!VertexStreamZeroStride))
      return D3DERR_INVALIDCALL;

    if (unlikely(m_state.vertexDecl == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!PrimitiveCount || !NumVertices))
      return D3D_OK;

    PrepareDraw(PrimitiveType, false, false);

    uint32_t vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);

    const uint32_t vertexDataSize = GetUPDataSize(MinVertexIndex + NumVertices, VertexStreamZeroStride);
    const uint32_t vertexBufferSize = GetUPBufferSize(MinVertexIndex + NumVertices, VertexStreamZeroStride);

    const uint32_t indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;
    const uint32_t indicesSize = vertexCount * indexSize;

    const uint32_t upSize = vertexBufferSize + indicesSize;

    auto upSlice = AllocUPBuffer(upSize);
    uint8_t* data = reinterpret_cast<uint8_t*>(upSlice.mapPtr);
    FillUPVertexBuffer(data, pVertexStreamZeroData, vertexDataSize, vertexBufferSize);
    std::memcpy(data + vertexBufferSize, pIndexData, indicesSize);

    EmitCs([this,
      cVertexSize   = vertexBufferSize,
      cBufferSlice  = std::move(upSlice.slice),
      cPrimType     = PrimitiveType,
      cPrimCount    = PrimitiveCount,
      cStride       = VertexStreamZeroStride,
      cInstanceCount = GetInstanceCount(),
      cIndexType    = DecodeIndexType(
                        static_cast<D3D9Format>(IndexDataFormat))
    ](MtlContext* ctx) {
      auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

      MtlDrawIndexedArgs draw = { };
      draw.indexCount    = drawInfo.vertexCount;
      draw.instanceCount = drawInfo.instanceCount;

      ctx->bindVertexBuffer(0, cBufferSlice.subSlice(0, cVertexSize), cStride);
      ctx->bindIndexBuffer(cBufferSlice.subSlice(cVertexSize, cBufferSlice.length() - cVertexSize), cIndexType);
      ctx->drawIndexed(1u, &draw);
      ctx->bindVertexBuffer(0, MtlBufferSlice(), 0);
      ctx->bindIndexBuffer(MtlBufferSlice(), MtlIndexType::Uint32);
    });

    m_state.vertexBuffers[0].vertexBuffer = nullptr;
    m_state.vertexBuffers[0].offset       = 0;
    m_state.vertexBuffers[0].length       = 0;
    m_state.vertexBuffers[0].stride       = 0;

    m_state.indices = nullptr;
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ProcessVertices(
          UINT                         SrcStartIndex,
          UINT                         DestIndex,
          UINT                         VertexCount,
          IDirect3DVertexBuffer9*      pDestBuffer,
          IDirect3DVertexDeclaration9* pVertexDecl,
          DWORD                        Flags) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pDestBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (UseProgrammableVS()) {
      const auto& programInfo = GetCommonShader(m_state.vertexShader)->GetInfo();

      if (unlikely(programInfo.getVersion().first >= 3) && (pVertexDecl == nullptr))
        return D3DERR_INVALIDCALL;
    }

    if (!SupportsSWVP()) {
      static bool s_errorShown = false;

      if (!std::exchange(s_errorShown, true))
        Logger::err("D3D9DeviceEx::ProcessVertices: SWVP emu unsupported (vertexPipelineStoresAndAtomics)");

      return D3D_OK;
    }

    if (unlikely(!VertexCount))
      return D3D_OK;

    D3D9CommonBuffer* dst  = static_cast<D3D9VertexBuffer*>(pDestBuffer)->GetCommonBuffer();
    D3D9VertexDecl*   decl = static_cast<D3D9VertexDecl*>  (pVertexDecl);

    bool dynamicSysmemVBOs;
    uint32_t firstIndex     = 0;
    int32_t baseVertexIndex = 0;
    UploadPerDrawData(
      SrcStartIndex,
      VertexCount,
      firstIndex,
      0,
      baseVertexIndex,
      &dynamicSysmemVBOs,
      nullptr
    );

    PrepareDraw(D3DPT_FORCE_DWORD, !dynamicSysmemVBOs, false);

    if (decl == nullptr) {
      DWORD FVF = dst->Desc()->FVF;

      auto iter = m_fvfTable.find(FVF);

      if (iter == m_fvfTable.end()) {
        decl = new D3D9VertexDecl(this, FVF);
        m_fvfTable.insert(std::make_pair(FVF, decl));
      }
      else
        decl = iter->second.ptr();
    }

    D3D9CompactVertexElements elements;
    for (const D3DVERTEXELEMENT9& element : decl->GetElements()) {
      elements.emplace_back(element);
    }

    uint32_t dstSliceOffset = DestIndex * decl->GetSize(0);
    uint32_t dstSliceLength = VertexCount * decl->GetSize(0);

    EmitCs([this,
      cVertexElements = std::move(elements),
      cVertexCount    = VertexCount,
      cStartIndex     = SrcStartIndex,
      cInstanceCount  = GetInstanceCount(),
      cBufferSlice    = dst->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(dstSliceOffset, dstSliceLength)
    ](MtlContext* ctx) mutable {
      (void) ctx;
      (void) cVertexElements; (void) cVertexCount; (void) cStartIndex;
      (void) cInstanceCount;  (void) cBufferSlice;
      Logger::warn("D3D9DeviceEx::ProcessVertices: SWVP stream-out unimplemented on Metal (PHASE-E)");
    });

    if (m_state.pixelShader != nullptr) {
      BindShader<D3D9ShaderType::PixelShader>(
        GetCommonShader(m_state.pixelShader));
    } else {
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
      BindFFUbershader<D3D9ShaderType::PixelShader>();
    }

    if (dst->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER) {
      EmitCs([
        cSrcBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>(),
        cDstBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>(),
        cOffset    = dstSliceOffset,
        cCopySize  = dstSliceLength
      ](MtlContext* ctx) {
        ctx->copyBuffer(cDstBuffer, cOffset, cSrcBuffer, cOffset, cCopySize);
      });
    }

    dst->SetNeedsReadback(true);
    TrackBufferMappingBufferSequenceNumber(dst);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9*            pVertexElements,
          IDirect3DVertexDeclaration9** ppDecl) {
    InitReturnPtr(ppDecl);

    if (unlikely(ppDecl == nullptr || pVertexElements == nullptr))
      return D3DERR_INVALIDCALL;

    const D3DVERTEXELEMENT9* counter = pVertexElements;
    while (counter->Stream != 0xFF)
      counter++;

    const uint32_t declCount = uint32_t(counter - pVertexElements);

    try {
      const Com<D3D9VertexDecl> decl = new D3D9VertexDecl(this, pVertexElements, declCount);
      *ppDecl = decl.ref();
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
    D3D9DeviceLock lock = LockDevice();

    D3D9VertexDecl* decl = static_cast<D3D9VertexDecl*>(pDecl);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetVertexDeclaration(decl);

    if (decl == m_state.vertexDecl.ptr())
      return D3D_OK;

    bool dirtyFFShader = decl == nullptr || m_state.vertexDecl == nullptr;
    if (!dirtyFFShader)
      dirtyFFShader |= decl->GetFlags()        != m_state.vertexDecl->GetFlags()
                    || decl->GetTexcoordMask() != m_state.vertexDecl->GetTexcoordMask();

    if (dirtyFFShader)
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    const bool wasUsingProgrammableVS = UseProgrammableVS();

    m_state.vertexDecl = decl;

    const bool usesProgrammableVS = UseProgrammableVS();

    if (unlikely(usesProgrammableVS != wasUsingProgrammableVS)) {
      if (usesProgrammableVS) {
        BindShader<D3D9ShaderType::VertexShader>(GetCommonShader(m_state.vertexShader));
      } else {
        m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
        BindFFUbershader<D3D9ShaderType::VertexShader>();
      }
    }

    m_dirty.set(D3D9DeviceDirtyFlag::InputLayout, D3D9DeviceDirtyFlag::Fog);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppDecl);

    if (unlikely(ppDecl == nullptr))
      return D3DERR_INVALIDCALL;

    if (m_state.vertexDecl == nullptr)
      return D3D_OK;

    *ppDecl = m_state.vertexDecl.ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetFVF(DWORD FVF) {
    D3D9DeviceLock lock = LockDevice();

    if (FVF == 0)
      return D3D_OK;

    D3D9VertexDecl* decl = nullptr;

    auto iter = m_fvfTable.find(FVF);

    if (iter == m_fvfTable.end()) {
      decl = new D3D9VertexDecl(this, FVF);
      m_fvfTable.insert(std::make_pair(FVF, decl));
    }
    else
      decl = iter->second.ptr();

    return this->SetVertexDeclaration(decl);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFVF(DWORD* pFVF) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pFVF == nullptr))
      return D3DERR_INVALIDCALL;

    *pFVF = m_state.vertexDecl != nullptr
      ? m_state.vertexDecl->GetFVF()
      : 0;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexShader(
    const DWORD*                   pFunction,
          IDirect3DVertexShader9** ppShader) {

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonShader module;
    size_t bytecodeLength;

    if (FAILED(this->CreateShaderModule(&module,
      &bytecodeLength,
      D3D9ShaderType::VertexShader,
      pFunction)))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(new D3D9VertexShader(this,
      &m_shaderAllocator,
      module,
      pFunction,
      uint32_t(bytecodeLength)));

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShader(IDirect3DVertexShader9* pShader) {
    D3D9DeviceLock lock = LockDevice();

    D3D9VertexShader* shader = static_cast<D3D9VertexShader*>(pShader);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetVertexShader(shader);

    if (shader == m_state.vertexShader.ptr())
      return D3D_OK;

    auto* oldShader = GetCommonShader(m_state.vertexShader);
    auto* newShader = GetCommonShader(shader);

    auto oldLayout = oldShader ? oldShader->GetConstantLayout() : nullptr;
    auto newLayout = newShader ? newShader->GetConstantLayout() : nullptr;

    auto& vsConstants = m_consts[uint32_t(D3D9ShaderType::VertexShader)];
    vsConstants.dirty |= oldLayout != newLayout;
    vsConstants.shaderConstantsInfo = newShader ? newShader->GetConstantsInfo() : D3D9ShaderConstantsInfo();

    const bool wasUsingProgrammableVS = UseProgrammableVS();

    m_state.vertexShader = shader;

    m_boundPsLinkageHash = ~0ull;

    const bool usesProgrammableVS = UseProgrammableVS();

    if (usesProgrammableVS) {
      BindShader<D3D9ShaderType::VertexShader>(GetCommonShader(shader));
    } else if (wasUsingProgrammableVS) {
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
      BindFFUbershader<D3D9ShaderType::VertexShader>();
    }

    m_dirty.set(D3D9DeviceDirtyFlag::InputLayout,
                D3D9DeviceDirtyFlag::Fog);
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    *ppShader = m_state.vertexShader.ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantF(
          UINT   StartRegister,
    const float* pConstantData,
          UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantF(
          UINT   StartRegister,
          float* pConstantData,
          UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantI(
          UINT StartRegister,
    const int* pConstantData,
          UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantI(
          UINT StartRegister,
          int* pConstantData,
          UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantB(
          UINT  StartRegister,
    const BOOL* pConstantData,
          UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantB(
          UINT  StartRegister,
          BOOL* pConstantData,
          UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSource(
          UINT                    StreamNumber,
          IDirect3DVertexBuffer9* pStreamData,
          UINT                    OffsetInBytes,
          UINT                    Stride) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    D3D9VertexBuffer* buffer = static_cast<D3D9VertexBuffer*>(pStreamData);

    if (unlikely(ShouldRecord())) {
      return m_recorder->SetStreamSource(
        StreamNumber, buffer, OffsetInBytes, Stride);
    }

    auto& vbo = m_state.vertexBuffers[StreamNumber];

    if (vbo.vertexBuffer != buffer) {
      const uint32_t bit = 1u << StreamNumber;
      m_vbSlotTracking.uploadPerDraw &= ~bit;
      m_vbSlotTracking.needsUpload &= ~bit;

      if (likely(buffer)) {
        const D3D9CommonBuffer* commonBuffer = GetCommonBuffer(buffer);
        m_vbSlotTracking.bound |= bit;

        if (commonBuffer->DoPerDrawUpload() || CanOnlySWVP())
          m_vbSlotTracking.uploadPerDraw |= bit;

        if (commonBuffer->NeedsUpload())
          m_vbSlotTracking.needsUpload |= bit;

        vbo.vertexBuffer = buffer;
        vbo.length = commonBuffer->Desc()->Size;
        vbo.offset = OffsetInBytes;
        vbo.stride = Stride;

        BindVertexBuffer(StreamNumber, buffer,
          vbo.offset, vbo.length, vbo.stride);
      } else {
        m_vbSlotTracking.bound &= ~bit;

        vbo.vertexBuffer = nullptr;
        vbo.offset = 0u;
        vbo.length = 0u;
        vbo.stride = 0u;
      }
    } else if (likely(buffer && (vbo.offset != OffsetInBytes || vbo.stride != Stride))) {
      vbo.offset = OffsetInBytes;
      vbo.stride = Stride;

      BindVertexBufferRange(StreamNumber,
        vbo.offset, vbo.length, vbo.stride);
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSource(
          UINT                     StreamNumber,
          IDirect3DVertexBuffer9** ppStreamData,
          UINT*                    pOffsetInBytes,
          UINT*                    pStride) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppStreamData);

    if (likely(pOffsetInBytes != nullptr))
      *pOffsetInBytes = 0;

    if (likely(pStride != nullptr))
      *pStride = 0;

    if (unlikely(ppStreamData == nullptr || pOffsetInBytes == nullptr || pStride == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    const auto& vbo = m_state.vertexBuffers[StreamNumber];

    *ppStreamData   = vbo.vertexBuffer.ref();
    *pOffsetInBytes = vbo.offset;
    *pStride        = vbo.stride;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    const bool indexed  = Setting & D3DSTREAMSOURCE_INDEXEDDATA;
    const bool instanced = Setting & D3DSTREAMSOURCE_INSTANCEDATA;

    if (unlikely(StreamNumber == 0 && instanced))
      return D3DERR_INVALIDCALL;

    if (unlikely(instanced && indexed))
      return D3DERR_INVALIDCALL;

    if (unlikely(Setting == 0))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStreamSourceFreq(StreamNumber, Setting);

    UINT oldSetting = m_state.streamFreq[StreamNumber];

    if (oldSetting == Setting)
      return D3D_OK;

    m_state.streamFreq[StreamNumber] = Setting;

    if (instanced)
      m_vbSlotTracking.instanced |=   1u << StreamNumber;
    else
      m_vbSlotTracking.instanced &= ~(1u << StreamNumber);

    if ((oldSetting | Setting) & D3DSTREAMSOURCE_INSTANCEDATA)
      m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSetting == nullptr))
      return D3DERR_INVALIDCALL;

    *pSetting = m_state.streamFreq[StreamNumber];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
    D3D9DeviceLock lock = LockDevice();

    D3D9IndexBuffer* buffer = static_cast<D3D9IndexBuffer*>(pIndexData);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetIndices(buffer);

    if (buffer == m_state.indices.ptr())
      return D3D_OK;

    m_state.indices = buffer;

    if (buffer != nullptr)
      BindIndices();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
    D3D9DeviceLock lock = LockDevice();
    InitReturnPtr(ppIndexData);

    if (unlikely(ppIndexData == nullptr))
      return D3DERR_INVALIDCALL;

    *ppIndexData = m_state.indices.ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreatePixelShader(
    const DWORD*                  pFunction,
          IDirect3DPixelShader9** ppShader) {
    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonShader module;
    size_t bytecodeLength;

    if (FAILED(this->CreateShaderModule(&module,
      &bytecodeLength,
      D3D9ShaderType::PixelShader,
      pFunction)))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(new D3D9PixelShader(this,
      &m_shaderAllocator,
      module,
      pFunction,
      uint32_t(bytecodeLength)));

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShader(IDirect3DPixelShader9* pShader) {
    D3D9DeviceLock lock = LockDevice();

    D3D9PixelShader* shader = static_cast<D3D9PixelShader*>(pShader);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetPixelShader(shader);

    if (shader == m_state.pixelShader.ptr())
      return D3D_OK;

    auto* oldShader = GetCommonShader(m_state.pixelShader);
    auto* newShader = GetCommonShader(shader);

    auto oldLayout = oldShader ? oldShader->GetConstantLayout() : nullptr;
    auto newLayout = newShader ? newShader->GetConstantLayout() : nullptr;

    auto& psConstants = m_consts[uint32_t(D3D9ShaderType::PixelShader)];
    psConstants.dirty |= oldLayout != newLayout;
    psConstants.shaderConstantsInfo = newShader ? newShader->GetConstantsInfo() : D3D9ShaderConstantsInfo();

    const D3D9ShaderMasks oldShaderMasks = PSShaderMasks();
    m_state.pixelShader = shader;
    const D3D9ShaderMasks newShaderMasks = PSShaderMasks();

    m_boundPsLinkageHash = ~0ull;

    if (shader != nullptr) {
      BindShader<D3D9ShaderType::PixelShader>(newShader);

      const auto& programInfo = GetCommonShader(m_state.pixelShader)->GetInfo();

      if (m_specData.setPsShaderModel(programInfo.getVersion().first))
        m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }
    else {
      m_specData.setPsShaderModel(0u);

      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader,
                  D3D9DeviceDirtyFlag::SpecializationEntries);

      BindFFUbershader<D3D9ShaderType::PixelShader>();
    }

    if (likely(oldShaderMasks.samplerMask != newShaderMasks.samplerMask ||
      oldShaderMasks.rtMask != newShaderMasks.rtMask))
      UpdateActiveHazardsRT(oldShaderMasks.samplerMask | newShaderMasks.samplerMask);

    if (likely(oldShaderMasks.samplerMask != newShaderMasks.samplerMask))
      UpdateActiveHazardsDS(oldShaderMasks.samplerMask | newShaderMasks.samplerMask);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    *ppShader = m_state.pixelShader.ref();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantF(
    UINT   StartRegister,
    const float* pConstantData,
    UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants <
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantF(
    UINT   StartRegister,
    float* pConstantData,
    UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantI(
    UINT StartRegister,
    const int* pConstantData,
    UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantI(
    UINT StartRegister,
    int* pConstantData,
    UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantB(
    UINT  StartRegister,
    const BOOL* pConstantData,
    UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantB(
    UINT  StartRegister,
    BOOL* pConstantData,
    UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawRectPatch(
          UINT               Handle,
    const float*             pNumSegs,
    const D3DRECTPATCH_INFO* pRectPatchInfo) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DrawRectPatch: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawTriPatch(
          UINT              Handle,
    const float*            pNumSegs,
    const D3DTRIPATCH_INFO* pTriPatchInfo) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DrawTriPatch: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DeletePatch(UINT Handle) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DeletePatch: Stub");

    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    HRESULT hr = D3D9Query::QuerySupported(this, Type);

    if (ppQuery == nullptr || hr != D3D_OK)
      return hr;

    try {
      *ppQuery = ref(new D3D9Query(this, Type));
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_NOTAVAILABLE;
    }
  }

  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetConvolutionMonoKernel(
          UINT   width,
          UINT   height,
          float* rows,
          float* columns) {
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ComposeRects(
          IDirect3DSurface9*      pSrc,
          IDirect3DSurface9*      pDst,
          IDirect3DVertexBuffer9* pSrcRectDescs,
          UINT                    NumRects,
          IDirect3DVertexBuffer9* pDstRectDescs,
          D3DCOMPOSERECTSOP       Operation,
          int                     Xoffset,
          int                     Yoffset) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::ComposeRects: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetGPUThreadPriority(INT* pPriority) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::GetGPUThreadPriority: Stub");

    if (unlikely(pPriority == nullptr))
      return D3DERR_INVALIDCALL;

    *pPriority = 0;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetGPUThreadPriority(INT Priority) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::SetGPUThreadPriority: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::WaitForVBlank(UINT iSwapChain) {
    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    return m_implicitSwapchain->WaitForVBlank();
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::CheckResourceResidency: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetMaximumFrameLatency(UINT MaxLatency) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(MaxLatency > 30))
      return D3DERR_INVALIDCALL;

    if (MaxLatency == 0)
      MaxLatency = DefaultFrameLatency;

    if (MaxLatency > MaxFrameLatency)
      MaxLatency = MaxFrameLatency;

    m_frameLatency = MaxLatency;

    m_implicitSwapchain->SyncFrameLatency();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaximumFrameLatency(UINT* pMaxLatency) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaxLatency == nullptr))
      return D3DERR_INVALIDCALL;

    *pMaxLatency = m_frameLatency;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CheckDeviceState(HWND hDestinationWindow) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::CheckDeviceState: Stub");

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(
    const RECT* pSourceRect,
    const RECT* pDestRect,
          HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD dwFlags) {

    if (m_cursor.IsSoftwareCursor()) {
      D3D9_SOFTWARE_CURSOR* pSoftwareCursor = m_cursor.GetSoftwareCursor();

      UINT cursorWidth  = pSoftwareCursor->DrawCursor ? pSoftwareCursor->Width  : 0;
      UINT cursorHeight = pSoftwareCursor->DrawCursor ? pSoftwareCursor->Height : 0;

      m_implicitSwapchain->SetCursorPosition(pSoftwareCursor->X - pSoftwareCursor->XHotSpot,
                                             pSoftwareCursor->Y - pSoftwareCursor->YHotSpot,
                                             cursorWidth, cursorHeight);

      if (unlikely(pSoftwareCursor->ClearCursor)) {
        pSoftwareCursor->Width       = 0;
        pSoftwareCursor->Height      = 0;
        pSoftwareCursor->XHotSpot    = 0;
        pSoftwareCursor->YHotSpot    = 0;
        pSoftwareCursor->ClearCursor = false;
      }
    }

    return m_implicitSwapchain->Present(
      pSourceRect,
      pDestRect,
      hDestWindowOverride,
      pDirtyRegion,
      dwFlags);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Lockable,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
      return D3DERR_INVALIDCALL;

    if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0
      && pSharedHandle == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage | D3DUSAGE_RENDERTARGET;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Discard            = FALSE;
    desc.MultiSample        = MultiSample;
    desc.MultisampleQuality = MultisampleQuality;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = TRUE;
    desc.IsLockable         = Lockable;

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc);
    if (FAILED(hr))
      return hr;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurfaceEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DPOOL             Pool,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
      return D3DERR_INVALIDCALL;

    if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0
      && pSharedHandle == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = TRUE;
    desc.IsLockable         = TRUE;

    if (unlikely(IsDepthStencilFormat(desc.Format) && !IsLockableDepthStencilFormat(desc.Format)))
      return D3DERR_INVALIDCALL;

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc);
    if (FAILED(hr))
      return hr;

    try {
      void* initialData = nullptr;

      if (unlikely(pSharedHandle != nullptr && Pool == D3DPOOL_SYSTEMMEM)) {
        initialData = *(reinterpret_cast<void**>(pSharedHandle));
        pSharedHandle = nullptr;
      }

      if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
        return D3DERR_INVALIDCALL;

      if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                   !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
        return E_INVALIDARG;

      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
      m_initializer->InitTexture(surface->GetCommonTexture(), initialData);
      *ppSurface = surface.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurfaceEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Discard,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
      return D3DERR_INVALIDCALL;

    if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0
      && pSharedHandle == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage | D3DUSAGE_DEPTHSTENCIL;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Discard            = Discard;
    desc.MultiSample        = MultiSample;
    desc.MultisampleQuality = MultisampleQuality;
    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = TRUE;
    desc.IsLockable         = IsLockableDepthStencilFormat(desc.Format);

    HRESULT hr = D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc);
    if (FAILED(hr))
      return hr;

    if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
                 !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
      return E_INVALIDARG;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = LockDevice();

    HRESULT hr;
    if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {
      hr = m_parent->ValidatePresentationParametersEx(pPresentationParameters, pFullscreenDisplayMode);

      if (unlikely(FAILED(hr)))
        return hr;
    }

    hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
    if (FAILED(hr))
      return hr;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayModeEx(
          UINT                iSwapChain,
          D3DDISPLAYMODEEX*   pMode,
          D3DDISPLAYROTATION* pRotation) {
    if (unlikely(iSwapChain != 0))
      return D3DERR_INVALIDCALL;

    return m_implicitSwapchain->GetDisplayModeEx(pMode, pRotation);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChainEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          IDirect3DSwapChain9**  ppSwapChain) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppSwapChain);

    if (ppSwapChain == nullptr || pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    if (!pPresentationParameters->Windowed)
      return D3DERR_INVALIDCALL;

    if (!m_implicitSwapchain->GetPresentParams()->Windowed)
      return D3DERR_INVALIDCALL;

    if (unlikely(IsDeviceLost())) {
      return D3DERR_DEVICELOST;
    }

    m_implicitSwapchain->Invalidate(pPresentationParameters->hDeviceWindow);

    try {
      auto* swapchain = new D3D9SwapChainEx(this, pPresentationParameters, pFullscreenDisplayMode, false);
      *ppSwapChain = ref(swapchain);
      m_losableResourceCounter++;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateSamplerState(
    DWORD               StateSampler,
    D3DSAMPLERSTATETYPE Type,
    DWORD               Value) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateSamplerState(StateSampler, Type, Value);

    auto& state = m_state.samplerStates;

    if (state[StateSampler][Type] == Value)
      return D3D_OK;

    state[StateSampler][Type] = Value;

    const uint32_t samplerBit = 1u << StateSampler;

    m_textureSlotTracking.samplerStateDirty |= samplerBit;

    if (Type == D3DSAMP_SRGBTEXTURE && (m_textureSlotTracking.bound & samplerBit))
      m_textureSlotTracking.textureDirty |= samplerBit;

    constexpr DWORD Fetch4Enabled  = MAKEFOURCC('G', 'E', 'T', '4');
    constexpr DWORD Fetch4Disabled = MAKEFOURCC('G', 'E', 'T', '1');

    if (unlikely(Type == D3DSAMP_MIPMAPLODBIAS)) {
      if (unlikely(Value == Fetch4Enabled))
        m_textureSlotTracking.fetch4SamplerState |= samplerBit;
      else if (unlikely(Value == Fetch4Disabled))
        m_textureSlotTracking.fetch4SamplerState &= ~samplerBit;

      UpdateActiveFetch4(StateSampler);
    }

    if (unlikely(Type == D3DSAMP_MAGFILTER && (m_textureSlotTracking.fetch4SamplerState & samplerBit)))
      UpdateActiveFetch4(StateSampler);

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9* pTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateTexture(StateSampler, pTexture);

    if (m_state.textures[StateSampler] == pTexture)
      return D3D_OK;

    auto oldTexture = GetCommonTexture(m_state.textures[StateSampler]);
    auto newTexture = GetCommonTexture(pTexture);

    const bool isPSSampler = StateSampler < caps::MaxTexturesPS;
    if (isPSSampler) {
      if (newTexture == nullptr || oldTexture == nullptr)
        m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
    }

    bool oldTextureIsCube = oldTexture != nullptr && oldTexture->IsCube();
    bool newTextureIsCube = newTexture != nullptr && newTexture->IsCube();
    if (unlikely(oldTextureIsCube != newTextureIsCube))
      m_textureSlotTracking.samplerStateDirty |= 1u << StateSampler;

    bool oldTextureHasMultipleMips = oldTexture != nullptr && oldTexture->Desc()->MipLevels > 1u;
    bool newTextureHasMultipleMips = newTexture != nullptr && newTexture->Desc()->MipLevels > 1u;
    if (unlikely(oldTextureHasMultipleMips != newTextureHasMultipleMips))
      m_textureSlotTracking.samplerStateDirty |= 1u << StateSampler;

    DWORD oldUsage = oldTexture != nullptr ? oldTexture->Desc()->Usage : 0;
    DWORD newUsage = newTexture != nullptr ? newTexture->Desc()->Usage : 0;
    DWORD combinedUsage = oldUsage | newUsage;
    TextureChangePrivate(m_state.textures[StateSampler], pTexture);
    m_textureSlotTracking.textureDirty |= 1u << StateSampler;
    UpdateTextureBitmasks(StateSampler, combinedUsage);

    if ((!oldTexture || !newTexture || oldTexture->Desc()->Format != newTexture->Desc()->Format)
      && SamplerUsesBorderColor(StateSampler))
        m_textureSlotTracking.samplerStateDirty |= 1u << StateSampler;

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateTransform(uint32_t idx, const D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateTransform(idx, pMatrix);

    m_state.transforms[idx] = ConvertMatrix(pMatrix);

    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    if (idx == GetTransformIndex(D3DTS_VIEW)
     || idx >= GetTransformIndex(D3DTS_WORLD))
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

    if (idx == GetTransformIndex(D3DTS_PROJECTION))
      m_dirty.set(D3D9DeviceDirtyFlag::Fog);

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateTextureStageState(
          DWORD                      Stage,
          D3D9TextureStageStateTypes Type,
          DWORD                      Value) {

    Stage = std::min(Stage, DWORD(caps::TextureStageCount - 1));
    Type = std::min(Type, D3D9TextureStageStateTypes(DXVK_TSS_COUNT - 1));

    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateTextureStageState(Stage, Type, Value);

    if (likely(m_state.textureStages[Stage][Type] != Value)) {
      m_state.textureStages[Stage][Type] = Value;

      switch (Type) {
        case DXVK_TSS_COLOROP:
        case DXVK_TSS_COLORARG0:
        case DXVK_TSS_COLORARG1:
        case DXVK_TSS_COLORARG2:
        case DXVK_TSS_ALPHAOP:
        case DXVK_TSS_ALPHAARG0:
        case DXVK_TSS_ALPHAARG1:
        case DXVK_TSS_ALPHAARG2:
        case DXVK_TSS_RESULTARG:
          m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
          break;

        case DXVK_TSS_TEXCOORDINDEX:
          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          break;

        case DXVK_TSS_TEXTURETRANSFORMFLAGS:
          m_textureSlotTracking.projected &= ~(1 << Stage);
          if (Value & D3DTTFF_PROJECTED)
            m_textureSlotTracking.projected |= 1 << Stage;

          m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
          m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
          break;

        case DXVK_TSS_BUMPENVMAT00:
        case DXVK_TSS_BUMPENVMAT01:
        case DXVK_TSS_BUMPENVMAT10:
        case DXVK_TSS_BUMPENVMAT11:
        case DXVK_TSS_BUMPENVLSCALE:
        case DXVK_TSS_BUMPENVLOFFSET:
        case DXVK_TSS_CONSTANT:
          m_dirty.set(D3D9DeviceDirtyFlag::SharedPixelShaderData);
          break;

        default: break;
      }
    }

    return D3D_OK;
  }


  bool D3D9DeviceEx::IsExtended() {
    return m_parent->IsExtended();
  }


  bool D3D9DeviceEx::SupportsSWVP() {
    return true;
  }


  bool D3D9DeviceEx::SupportsVCacheQuery() const {
    return m_adapter->GetVendorId() == uint32_t(MtlGpuVendor::Nvidia);
  }


  HWND D3D9DeviceEx::GetWindow() {
    return m_window;
  }


  void D3D9DeviceEx::DetermineConstantLayouts(bool canSWVP) {
    D3D9ConstantSets& vsConstSet    = m_consts[uint32_t(D3D9ShaderType::VertexShader)];
    vsConstSet.layout.floatCount    = canSWVP ? caps::MaxFloatConstantsSoftware : caps::MaxFloatConstantsVS;
    vsConstSet.layout.intCount      = canSWVP ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    vsConstSet.layout.boolCount     = canSWVP ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    vsConstSet.layout.bitmaskCount  = align(vsConstSet.layout.boolCount, 32) / 32;

    D3D9ConstantSets& psConstSet   = m_consts[uint32_t(D3D9ShaderType::PixelShader)];
    psConstSet.layout.floatCount   = caps::MaxSM3FloatConstantsPS;
    psConstSet.layout.intCount     = caps::MaxOtherConstants;
    psConstSet.layout.boolCount    = caps::MaxOtherConstants;
    psConstSet.layout.bitmaskCount = align(psConstSet.layout.boolCount, 32) / 32;
  }


  D3D9BufferSlice D3D9DeviceEx::AllocUPBuffer(MtlDeviceSize size) {
    constexpr MtlDeviceSize UPBufferSize = 1 << 20;

    if (unlikely(m_upBuffer == nullptr || size > UPBufferSize)) {
      MtlBufferCreateInfo info;
      info.size      = std::max(UPBufferSize, size);
      info.usage     = MtlUsage::VertexBuffer | MtlUsage::IndexBuffer;
      info.cpuAccess = MtlCpuAccess::Write;

      Rc<MtlBuffer> buffer = m_dxvkDevice->createBuffer(info);
      void* mapPtr = buffer->mapPtr(0);

      if (size <= UPBufferSize) {
        m_upBuffer = std::move(buffer);
        m_upBufferAllocation = m_upBuffer->storage();
        m_upBufferMapPtr = mapPtr;
      } else {
        D3D9BufferSlice result;
        result.slice = MtlBufferSlice(std::move(buffer), 0, size);
        result.mapPtr = mapPtr;
        return result;
      }
    }

    MtlDeviceSize alignedSize = align(size, CACHE_LINE_SIZE);

    if (unlikely(m_upBufferOffset + alignedSize > UPBufferSize)) {
      auto slice = m_upBuffer->allocateStorage();

      m_upBufferOffset = 0;
      m_upBufferAllocation = slice;
      m_upBufferMapPtr = slice->mapPtr;

      EmitCs([
        cBuffer = m_upBuffer,
        cSlice  = std::move(slice)
      ] (MtlContext* ctx) mutable {
        Rc<MtlAllocation> oldSlice =
          cBuffer->assignStorage(std::move(cSlice));
        ctx->invalidateBuffer(cBuffer, std::move(oldSlice));
      });
    }

    D3D9BufferSlice result;
    result.slice = MtlBufferSlice(m_upBuffer, m_upBufferAllocation, m_upBufferOffset, size);
    result.mapPtr = reinterpret_cast<char*>(m_upBufferMapPtr) + m_upBufferOffset;

    m_upBufferOffset += alignedSize;
    return result;
  }


  D3D9BufferSlice D3D9DeviceEx::AllocStagingBuffer(MtlDeviceSize size) {
    D3D9BufferSlice result;
    result.slice = m_stagingBuffer.alloc(size);
    if (unlikely(!result.slice.defined())) {
      m_stagingAllocatedBase += m_stagingBuffer.stats().allocated;

      MtlDeviceSize newSize = std::max<MtlDeviceSize>(size, StagingBufferSize);
      m_stagingBuffer.reinit(GetDXVKDevice(), newSize);
      result.slice = m_stagingBuffer.alloc(size);
    }

    result.mapPtr = result.slice.mapPtr(0);
    return result;
  }


  void D3D9DeviceEx::ThrottleAllocation() {
    constexpr MtlDeviceSize MaxMemoryPerSubmission = (env::is32BitHostPlatform() ? 10u : 30u) << 20u;

    constexpr MtlDeviceSize MaxMemoryInFlight = MaxMemoryPerSubmission * 3u;

    MtlStagingBufferStats stats = GetStagingMemoryStatistics();

    MtlDeviceSize stagingBufferAllocated = stats.allocated;

    if (stagingBufferAllocated > m_stagingMemorySignaled + MaxMemoryPerSubmission) {
      GpuFlushType flushType = stagingBufferAllocated <= m_stagingMemorySignaled + MaxMemoryInFlight
        ? GpuFlushType::ImplicitSynchronization
        : GpuFlushType::ExplicitFlush;

      ConsiderFlush(flushType);
    }

    if (stagingBufferAllocated > MaxMemoryInFlight)
      m_stagingBufferFence->wait(stagingBufferAllocated - MaxMemoryInFlight);
  }


  MtlStagingBufferStats D3D9DeviceEx::GetStagingMemoryStatistics() const {
    MtlStagingBufferStats stats = m_stagingBuffer.stats();
    stats.allocated += m_stagingAllocatedBase;
    stats.allocated += m_discardMemoryCounter;
    return stats;
  }


  D3D9_MTL_FORMAT_MAPPING D3D9DeviceEx::LookupFormat(
    D3D9Format            Format) const {
    return m_adapter->GetFormatMapping(Format);
  }


  const MtlFormatInfo* D3D9DeviceEx::UnsupportedFormatInfo(
    D3D9Format            Format) const {
    return m_adapter->GetUnsupportedFormatInfo(Format);
  }


  bool D3D9DeviceEx::WaitForResource(
    const MtlResource&                Resource,
          uint64_t                          SequenceNumber,
          DWORD                             MapFlags) {
    if (!m_dxvkDevice->isResourceInUse(Resource))
      SynchronizeCsThread(SequenceNumber);

    if (m_dxvkDevice->isResourceInUse(Resource)) {
      if (MapFlags & D3DLOCK_DONOTWAIT) {
        ConsiderFlush(GpuFlushType::ImplicitWeakHint);
        return false;
      }

      ExecuteFlush(false);
      SynchronizeCsThread(SequenceNumber);
      m_dxvkDevice->waitForResource(Resource);
    }

    return true;
  }


  uint32_t D3D9DeviceEx::CalcImageLockOffset(
            uint32_t                SlicePitch,
            uint32_t                RowPitch,
      const MtlFormatInfo*         FormatInfo,
      const D3DBOX*                 pBox) {
    if (pBox == nullptr)
      return 0;

    std::array<uint32_t, 3> offsets = { pBox->Front, pBox->Top, pBox->Left };

    uint32_t elementSize = 1;

    if (FormatInfo != nullptr) {
      elementSize = FormatInfo->elementSize;
      MtlExtent3D blockSize = util::formatBlockSize(FormatInfo);
      if (unlikely(FormatInfo->flags & MtlFormatFlagMultiPlane)) {
        elementSize = FormatInfo->planes[0].elementSize;
        blockSize = { FormatInfo->planes[0].blockWidth, FormatInfo->planes[0].blockHeight, 1u };
      }

      offsets[0] = offsets[0] / blockSize.depth;
      offsets[1] = offsets[1] / blockSize.height;
      offsets[2] = offsets[2] / blockSize.width;
    }

    return offsets[0] * SlicePitch +
           offsets[1] * RowPitch   +
           offsets[2] * elementSize;
  }


  HRESULT D3D9DeviceEx::LockImage(
            D3D9CommonTexture*      pResource,
            UINT                    Face,
            UINT                    MipLevel,
            D3DLOCKED_BOX*          pLockedBox,
      const D3DBOX*                 pBox,
            DWORD                   Flags) {
    D3D9DeviceLock lock = LockDevice();

    UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

    if (unlikely(pResource->GetLocked(Subresource)))
      return D3DERR_INVALIDCALL;

    auto& desc = *(pResource->Desc());

    if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY)
               && desc.Pool == D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    Flags &= ~D3DLOCK_DONOTWAIT;

    if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)))
      Flags &= ~D3DLOCK_DISCARD;

    if (unlikely(m_deviceLostState != D3D9DeviceLostState::Ok))
      Flags &= ~D3DLOCK_DISCARD;

    if (unlikely(!desc.IsLockable))
      return D3DERR_INVALIDCALL;

    if (unlikely(pBox != nullptr)) {
      D3DRESOURCETYPE type = pResource->GetType();
      D3D9_FORMAT_BLOCK_SIZE blockSize = GetFormatAlignedBlockSize(desc.Format);

      bool isBlockAlignedFormat = blockSize.Width > 0 && blockSize.Height > 0;
      bool isNotLeftAligned   = pBox->Left   && (pBox->Left   & (blockSize.Width  - 1));
      bool isNotTopAligned    = pBox->Top    && (pBox->Top    & (blockSize.Height - 1));
      bool isNotRightAligned  = pBox->Right  && (pBox->Right  & (blockSize.Width  - 1));
      bool isNotBottomAligned = pBox->Bottom && (pBox->Bottom & (blockSize.Height - 1));

      if (MipLevel == 0 && isBlockAlignedFormat
        && (type == D3DRTYPE_VOLUMETEXTURE ||
            desc.Pool == D3DPOOL_DEFAULT)
        && (isNotLeftAligned  || isNotTopAligned ||
            isNotRightAligned || isNotBottomAligned))
        return D3DERR_INVALIDCALL;
    }

    auto& formatMapping = pResource->GetFormatMapping();
    const MtlFormatInfo* formatInfo = formatMapping.IsValid()
      ? lookupFormatInfo(formatMapping.FormatColor) : UnsupportedFormatInfo(pResource->Desc()->Format);

    auto subresource = pResource->GetSubresourceFromIndex(
        formatInfo->aspects, Subresource);

    MtlExtent3D levelExtent = pResource->GetExtentMip(MipLevel);
    MtlExtent3D blockCount  = util::computeBlockCount(levelExtent, util::formatBlockSize(formatInfo));

    bool fullResource = pBox == nullptr;
    if (unlikely(!fullResource)) {
      MtlOrigin lockOffset;
      MtlSize lockExtent;

      ConvertBox(*pBox, lockOffset, lockExtent);

      fullResource = lockOffset == MtlOrigin{ 0, 0, 0 }
                  && lockExtent.width  >= levelExtent.width
                  && lockExtent.height >= levelExtent.height
                  && lockExtent.depth  >= levelExtent.depth;
    }

    if (!fullResource || desc.Pool != D3DPOOL_DEFAULT)
      Flags &= ~D3DLOCK_DISCARD;

    if (desc.Usage & D3DUSAGE_WRITEONLY)
      Flags &= ~D3DLOCK_READONLY;

    bool renderable = desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL);
    bool needsReadback = pResource->NeedsReadback(Subresource) || renderable;

    needsReadback &= pResource->GetImage() != nullptr || !(Flags & D3DLOCK_DISCARD);
    pResource->SetNeedsReadback(Subresource, false);

    if (unlikely(pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED || needsReadback)) {
      pResource->CreateBuffer(!needsReadback, pResource->GetTotalSize());
    }

    void* mapPtr = pResource->GetData(Subresource);

    if (unlikely(needsReadback)) {

      MtlBufferSlice mappedBufferSlice = pResource->GetBufferSlice(Subresource);
      const Rc<MtlBuffer> mappedBuffer = pResource->GetBuffer();

      if (unlikely(pResource->GetFormatMapping().ConversionFormatInfo.FormatType != D3D9ConversionFormat_None)) {
        Logger::err(str::format("Reading back format", pResource->Desc()->Format, " is not supported. It is uploaded using the fomrat converter."));
      }

      if (pResource->GetImage() != nullptr) {
        Rc<MtlTexture> resourceImage = pResource->GetImage();

        Rc<MtlTexture> mappedImage;
        if (resourceImage->info().sampleCount != 1) {
            mappedImage = pResource->GetResolveImage();
        } else {
            mappedImage = std::move(resourceImage);
        }

        auto subresourceLayers = subresource;

        if (resourceImage != nullptr) {
          EmitCs([
            cMainImage    = resourceImage,
            cResolveImage = mappedImage,
            cSubresource  = subresourceLayers
          ] (MtlContext* ctx) {
            MtlFormat format = cMainImage->info().format;

            MtlResolveRegion region;
            region.srcSubresource = cSubresource;
            region.srcOffset      = MtlOrigin { 0, 0, 0 };
            region.dstSubresource = cSubresource;
            region.dstOffset      = MtlOrigin { 0, 0, 0 };
            region.extent         = cMainImage->mipLevelExtent(cSubresource.mipLevel);

            ctx->resolveTexture(cResolveImage, cMainImage, region, format,
              MtlResolveMode::Average, MtlResolveMode::SampleZero);
          });
        }

        MtlFormat packedFormat = GetPackedDepthStencilFormat(desc.Format);

        EmitCs([
          cImageBufferSlice = std::move(mappedBufferSlice),
          cImage            = std::move(mappedImage),
          cSubresources     = subresourceLayers,
          cLevelExtent      = levelExtent,
          cPackedFormat     = packedFormat
        ] (MtlContext* ctx) {
          ctx->copyTextureToBuffer(cImageBufferSlice.buffer(),
            cImageBufferSlice.offset(), 4, 0, cPackedFormat,
            cImage, cSubresources, MtlOrigin { 0, 0, 0 },
            cLevelExtent);
        });
        TrackTextureMappingBufferSequenceNumber(pResource, Subresource);
      }

      if (!WaitForResource(*mappedBuffer, pResource->GetMappingBufferSequenceNumber(Subresource), Flags))
        return D3DERR_WASSTILLDRAWING;
    }

    const bool atiHack = desc.Format == D3D9Format::ATI1 || desc.Format == D3D9Format::ATI2;
    if (atiHack) {
      pLockedBox->RowPitch   = align(std::max(desc.Width >> MipLevel, 1u), 4);
      pLockedBox->SlicePitch = pLockedBox->RowPitch * std::max(desc.Height >> MipLevel, 1u);
    }
    else if (likely(!(formatInfo->flags & MtlFormatFlagMultiPlane))) {
      pLockedBox->RowPitch   = align(formatInfo->elementSize * blockCount.width, 4);
      pLockedBox->SlicePitch = pLockedBox->RowPitch * blockCount.height;
    } else {
      auto plane = &formatInfo->planes[0];
      uint32_t planeElementSize = plane->elementSize;
      MtlExtent3D planeBlockSize = { plane->blockWidth, plane->blockHeight, 1u };
      MtlExtent3D blockCount  = util::computeBlockCount(levelExtent, planeBlockSize);
      pLockedBox->RowPitch   = align(planeElementSize * blockCount.width, 4);
      pLockedBox->SlicePitch = pLockedBox->RowPitch * blockCount.height;
    }

    pResource->SetLocked(Subresource, true);

    UnmapTextures();

    const bool readOnly = Flags & D3DLOCK_READONLY;
    const bool noDirtyUpdate = Flags & D3DLOCK_NO_DIRTY_UPDATE;
    if ((desc.Pool == D3DPOOL_DEFAULT || !noDirtyUpdate) && !readOnly) {
      if (pBox && MipLevel != 0) {
        D3DBOX scaledBox = *pBox;
        scaledBox.Left   <<= MipLevel;
        scaledBox.Right    = std::min(scaledBox.Right << MipLevel, pResource->Desc()->Width);
        scaledBox.Top    <<= MipLevel;
        scaledBox.Bottom   = std::min(scaledBox.Bottom << MipLevel, pResource->Desc()->Height);
        scaledBox.Back   <<= MipLevel;
        scaledBox.Front    = std::min(scaledBox.Front << MipLevel, pResource->Desc()->Depth);
        pResource->AddDirtyBox(&scaledBox, Face);
      } else {
        pResource->AddDirtyBox(pBox, Face);
      }
    }

    if (IsPoolManaged(desc.Pool) && !readOnly) {
      pResource->SetNeedsUpload(Subresource, true);

      for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
        auto texInfo = GetCommonTexture(m_state.textures[i]);

        if (texInfo == pResource) {
          m_textureSlotTracking.needsUpload |= 1 << i;
        }
      }
    }

    const uint32_t offset = CalcImageLockOffset(
      pLockedBox->SlicePitch,
      pLockedBox->RowPitch,
      (!atiHack) ? formatInfo : nullptr,
      pBox);

    uint8_t* data = reinterpret_cast<uint8_t*>(mapPtr);
    data += offset;
    pLockedBox->pBits = data;
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::UnlockImage(
        D3D9CommonTexture*      pResource,
        UINT                    Face,
        UINT                    MipLevel) {
    D3D9DeviceLock lock = LockDevice();

    UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

    if (unlikely(!pResource->GetLocked(Subresource))) {
      if (pResource->GetType() == D3DRTYPE_TEXTURE)
        return D3D_OK;
      else
        return D3DERR_INVALIDCALL;
    }

    MapTexture(pResource, Subresource);
    pResource->SetLocked(Subresource, false);

    const D3DBOX& box = pResource->GetDirtyBox(Face);
    bool shouldFlush  = pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED;
         shouldFlush &= box.Left < box.Right && box.Top < box.Bottom && box.Front < box.Back;
         shouldFlush &= !pResource->IsManaged();

    if (shouldFlush) {
        this->FlushImage(pResource, Subresource);
        if (!pResource->IsAnySubresourceLocked())
          pResource->ClearDirtyBoxes();
    }

    bool shouldToss  = pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED;
         shouldToss &= !pResource->IsDynamic();
         shouldToss &= !pResource->IsManaged();
         shouldToss &= !pResource->IsAnySubresourceLocked();

    shouldToss &= pResource->GetFormatMapping().ConversionFormatInfo.FormatType == D3D9ConversionFormat_None;

    if (shouldToss)
      pResource->DestroyBuffer();

    UnmapTextures();
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::FlushImage(
        D3D9CommonTexture*      pResource,
        UINT                    Subresource) {

    const Rc<MtlTexture> image = pResource->GetImage();
    auto formatInfo  = lookupFormatInfo(image->info().format);
    auto subresource = pResource->GetSubresourceFromIndex(
      formatInfo->aspects, Subresource);

    const D3DBOX& box = pResource->GetDirtyBox(subresource.baseLayer);

    MtlExtent3D mip0Extent = { box.Right - box.Left, box.Bottom - box.Top, box.Back - box.Front };
    MtlExtent3D extent = util::computeMipLevelExtent(mip0Extent, subresource.mipLevel);
    MtlOrigin mip0Offset = { int32_t(box.Left), int32_t(box.Top), int32_t(box.Front) };
    MtlOrigin offset = util::computeMipLevelOffset(mip0Offset, subresource.mipLevel);

    UpdateTextureFromBuffer(pResource, pResource, Subresource, Subresource, offset, extent, offset);

    if (pResource->IsAutomaticMip())
      MarkTextureMipsDirty(pResource);

    return D3D_OK;
  }

  void D3D9DeviceEx::UpdateTextureFromBuffer(
    D3D9CommonTexture* pDestTexture,
    D3D9CommonTexture* pSrcTexture,
    UINT DestSubresource,
    UINT SrcSubresource,
    MtlOrigin SrcOffset,
    MtlExtent3D SrcExtent,
    MtlOrigin DestOffset) {
    ThrottleAllocation();

    const Rc<MtlTexture> image = pDestTexture->GetImage();


    auto formatInfo = lookupFormatInfo(pDestTexture->GetFormatMapping().FormatColor);
    auto srcSubresource = pSrcTexture->GetSubresourceFromIndex(
      formatInfo->aspects, SrcSubresource);

    auto dstSubresource = pDestTexture->GetSubresourceFromIndex(
      formatInfo->aspects, DestSubresource);
    MtlSubresourceLayers dstLayers = { dstSubresource.aspects, dstSubresource.mipLevel, dstSubresource.baseLayer, 1 };

    MtlExtent3D dstTexLevelExtent = image->mipLevelExtent(dstSubresource.mipLevel);
    MtlExtent3D srcTexLevelExtent = util::computeMipLevelExtent(pSrcTexture->GetExtent(), srcSubresource.mipLevel);

    auto convertFormat = pDestTexture->GetFormatMapping().ConversionFormatInfo;

    if (unlikely(pSrcTexture->NeedsReadback(SrcSubresource))) {
      const Rc<MtlBuffer>& buffer = pSrcTexture->GetBuffer();
      WaitForResource(*buffer, pSrcTexture->GetMappingBufferSequenceNumber(SrcSubresource), 0);
      pSrcTexture->SetNeedsReadback(SrcSubresource, false);
    }

    if (likely(convertFormat.FormatType == D3D9ConversionFormat_None)) {
      MtlOrigin alignedDestOffset = {
        int32_t(alignDown(DestOffset.x, formatInfo->blockWidth)),
        int32_t(alignDown(DestOffset.y, formatInfo->blockHeight)),
        int32_t(alignDown(DestOffset.z, 1u))
      };
      MtlOrigin alignedSrcOffset = {
        int32_t(alignDown(SrcOffset.x, formatInfo->blockWidth)),
        int32_t(alignDown(SrcOffset.y, formatInfo->blockHeight)),
        int32_t(alignDown(SrcOffset.z, 1u))
      };
      SrcExtent.width += SrcOffset.x - alignedSrcOffset.x;
      SrcExtent.height += SrcOffset.y - alignedSrcOffset.y;
      SrcExtent.depth += SrcOffset.z - alignedSrcOffset.z;
      MtlExtent3D extentBlockCount = util::computeBlockCount(SrcExtent, util::formatBlockSize(formatInfo));
      MtlExtent3D alignedExtent = util::computeBlockExtent(extentBlockCount, util::formatBlockSize(formatInfo));

      alignedExtent = util::snapExtent3D(alignedDestOffset, alignedExtent, dstTexLevelExtent);
      alignedExtent = util::snapExtent3D(alignedSrcOffset, alignedExtent, srcTexLevelExtent);

      MtlOrigin srcOffsetBlockCount = util::computeBlockOffset(alignedSrcOffset, util::formatBlockSize(formatInfo));
      MtlExtent3D srcTexLevelExtentBlockCount = util::computeBlockCount(srcTexLevelExtent, util::formatBlockSize(formatInfo));
      MtlDeviceSize pitch = align(srcTexLevelExtentBlockCount.width * formatInfo->elementSize, 4);
      MtlDeviceSize copySrcOffset = srcOffsetBlockCount.z * srcTexLevelExtentBlockCount.height * pitch
          + srcOffsetBlockCount.y * pitch
          + srcOffsetBlockCount.x * formatInfo->elementSize;

      const void* mapPtr = MapTexture(pSrcTexture, SrcSubresource);
      MtlDeviceSize dirtySize = extentBlockCount.width * extentBlockCount.height * extentBlockCount.depth * formatInfo->elementSize;
      D3D9BufferSlice slice = AllocStagingBuffer(dirtySize);
      const void* srcData = reinterpret_cast<const uint8_t*>(mapPtr) + copySrcOffset;
      util::packImageData(
        slice.mapPtr, srcData, extentBlockCount, formatInfo->elementSize,
        pitch, pitch * srcTexLevelExtentBlockCount.height);

      MtlFormat packedDSFormat = GetPackedDepthStencilFormat(pDestTexture->Desc()->Format);

      EmitCs([
        cSrcSlice       = slice.slice,
        cDstImage       = image,
        cDstLayers      = dstLayers,
        cDstLevelExtent = alignedExtent,
        cOffset         = alignedDestOffset,
        cPackedDSFormat = packedDSFormat
      ] (MtlContext* ctx) {
        ctx->copyBufferToTexture(
          cDstImage,  cDstLayers,
          cOffset, cDstLevelExtent,
          cSrcSlice.buffer(), cSrcSlice.offset(),
          0, 0, cPackedDSFormat);
      });

      TrackTextureMappingBufferSequenceNumber(pSrcTexture, SrcSubresource);
    }
    else {
      const void* mapPtr = MapTexture(pSrcTexture, SrcSubresource);

      if (unlikely(SrcOffset.x != 0 || SrcOffset.y != 0 || SrcOffset.z != 0
        || DestOffset.x != 0 || DestOffset.y != 0 || DestOffset.z != 0
        || SrcExtent != srcTexLevelExtent)) {
        Logger::warn("Offset and rect not supported with the texture converter.");
      }

      if (unlikely(srcTexLevelExtent != dstTexLevelExtent)) {
        Logger::err("Different extents are not supported with the texture converter.");
        return;
      }

      uint32_t formatElementSize = formatInfo->elementSize;
      MtlExtent3D srcBlockSize = util::formatBlockSize(formatInfo);
      if (formatInfo->flags & MtlFormatFlagMultiPlane) {
        formatElementSize = formatInfo->planes[0].elementSize;
        srcBlockSize = { formatInfo->planes[0].blockWidth, formatInfo->planes[0].blockHeight, 1u };
      }
      MtlExtent3D srcBlockCount = util::computeBlockCount(srcTexLevelExtent, srcBlockSize);
      srcBlockCount.height *= std::min(pSrcTexture->GetPlaneCount(), 2u);

      D3D9BufferSlice slice = AllocStagingBuffer(pSrcTexture->GetMipSize(SrcSubresource));
      MtlDeviceSize pitch = align(srcBlockCount.width * formatElementSize, 4);

      const MtlFormatInfo* convertedFormatInfo = lookupFormatInfo(convertFormat.FormatColor);
      MtlSubresourceLayers convertedDstLayers = { convertedFormatInfo->aspects, dstSubresource.mipLevel, dstSubresource.baseLayer, 1 };

      util::packImageData(
        slice.mapPtr, mapPtr, srcBlockCount, formatElementSize,
        pitch, std::min(pSrcTexture->GetPlaneCount(), 2u) * pitch * srcBlockCount.height);

      EmitCs([this,
        cConvertFormat    = convertFormat,
        cDstImage         = std::move(image),
        cDstLayers        = convertedDstLayers,
        cSrcSlice         = std::move(slice.slice)
      ] (MtlContext* ctx) {
        auto contextObjects = ctx->beginExternalRendering();

        m_converter->ConvertFormat(contextObjects,
          cConvertFormat, cDstImage, cDstLayers, cSrcSlice);
      });
    }
    UnmapTextures();
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);
  }


  void D3D9DeviceEx::EmitGenerateMips(
    D3D9CommonTexture* pResource) {
    if (pResource->IsManaged())
      UploadManagedTexture(pResource);

    EmitCs([
      cImageView = pResource->GetSampleView(false),
      cFilter    = pResource->GetMipFilter()
    ] (MtlContext* ctx) {
      ctx->generateMipmaps(cImageView, DecodeFilter(cFilter));
    });
  }


  HRESULT D3D9DeviceEx::LockBuffer(
          D3D9CommonBuffer*       pResource,
          UINT                    OffsetToLock,
          UINT                    SizeToLock,
          void**                  ppbData,
          DWORD                   Flags) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ppbData == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(!m_d3d9Options.allowDiscard))
      Flags &= ~D3DLOCK_DISCARD;

    auto& desc = *pResource->Desc();

    if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) != D3DLOCK_DISCARD))
      Flags &= ~D3DLOCK_DISCARD;

    if (desc.Pool != D3DPOOL_DEFAULT || CanOnlySWVP())
      Flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);

    if (desc.Usage & D3DUSAGE_DYNAMIC)
      Flags &= ~D3DLOCK_DONOTWAIT;

    if (unlikely(m_deviceLostState != D3D9DeviceLostState::Ok))
      Flags &= ~D3DLOCK_DISCARD;

    if (unlikely(CanOnlySWVP() && !pResource->NeedsReadback()))
      Flags |= D3DLOCK_NOOVERWRITE;

    if ((Flags & D3DLOCK_READONLY) && !IsPoolManaged(desc.Pool))
      Flags &= ~D3DLOCK_READONLY;

    const bool respectUserBounds = !(Flags & D3DLOCK_DISCARD) &&
                                    SizeToLock != 0 &&
                                    !m_d3d9Options.ignoreDefaultBufferLockRange;

    uint32_t offset = respectUserBounds ? OffsetToLock : 0;
    uint32_t size   = respectUserBounds ? std::min(SizeToLock, desc.Size - offset) : desc.Size;
    D3D9Range lockRange = D3D9Range(offset, offset + size);

    bool updateDirtyRange = (desc.Pool == D3DPOOL_DEFAULT || !(Flags & D3DLOCK_NO_DIRTY_UPDATE)) && !(Flags & D3DLOCK_READONLY);
    if (updateDirtyRange) {
      pResource->DirtyRange().Conjoin(lockRange);

      for (uint32_t i : bit::BitMask(static_cast<uint32_t>(m_vbSlotTracking.bound))) {
        auto commonBuffer = GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
        if (commonBuffer == pResource) {
          m_vbSlotTracking.needsUpload |= 1 << i;
        }
      }
    }

    const bool directMapping = pResource->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
    const bool needsReadback = pResource->NeedsReadback();

    uint8_t* data = nullptr;

    if ((Flags & D3DLOCK_DISCARD) && (directMapping || needsReadback)) {

      m_discardMemoryCounter += desc.Size;
      ThrottleAllocation();

      Rc<MtlBuffer> mappingBuffer = pResource->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
      auto bufferSlice = pResource->DiscardMapSlice();
      data = reinterpret_cast<uint8_t*>(bufferSlice->mapPtr);

      EmitCs([
        cBuffer      = std::move(mappingBuffer),
        cBufferSlice = std::move(bufferSlice)
      ] (MtlContext* ctx) mutable {
        Rc<MtlAllocation> oldSlice =
          cBuffer->assignStorage(std::move(cBufferSlice));
        ctx->invalidateBuffer(cBuffer, std::move(oldSlice));
      });

      pResource->SetNeedsReadback(false);
    }
    else {

      data = reinterpret_cast<uint8_t*>(pResource->GetMappedSlice()->mapPtr);

      const bool readOnly = Flags & D3DLOCK_READONLY;
      const bool noOverwrite = Flags & D3DLOCK_NOOVERWRITE;
      const bool directMapping = pResource->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;

      const bool skipWait = (!needsReadback && (readOnly || !directMapping)) || noOverwrite;

      if (!skipWait) {
        const Rc<MtlBuffer> mappingBuffer = pResource->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
        if (!WaitForResource(*mappingBuffer, pResource->GetMappingBufferSequenceNumber(), Flags))
          return D3DERR_WASSTILLDRAWING;

        pResource->SetNeedsReadback(false);
      }
    }

    data += OffsetToLock;
    *ppbData = reinterpret_cast<void*>(data);

    DWORD oldFlags = pResource->GetMapFlags();

    if (!(Flags & D3DLOCK_READONLY))
      oldFlags &= ~D3DLOCK_READONLY;

    pResource->SetMapFlags(Flags | oldFlags);
    pResource->IncrementLockCount();

    UnmapTextures();

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::FlushBuffer(
        D3D9CommonBuffer*       pResource) {
    ThrottleAllocation();

    auto dstBuffer = pResource->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>();
    auto srcSlice = pResource->GetMappedSlice();

    D3D9Range& range = pResource->DirtyRange();

    D3D9BufferSlice slice = AllocStagingBuffer(range.max - range.min);
    if (unlikely(slice.mapPtr == nullptr || srcSlice->mapPtr == nullptr)) {
      Logger::err("D3D9DeviceEx::FlushBuffer: staging allocation failed (address space?)");
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    void* srcData = reinterpret_cast<uint8_t*>(srcSlice->mapPtr) + range.min;
    memcpy(slice.mapPtr, srcData, range.max - range.min);

    EmitCs([
      cDstSlice  = dstBuffer,
      cSrcSlice  = slice.slice,
      cDstOffset = range.min,
      cLength    = range.max - range.min
    ] (MtlContext* ctx) {
      ctx->copyBuffer(
        cDstSlice.buffer(),
        cDstSlice.offset() + cDstOffset,
        cSrcSlice.buffer(),
        cSrcSlice.offset(),
        cLength);
    });

    pResource->DirtyRange().Clear();
    TrackBufferMappingBufferSequenceNumber(pResource);

    UnmapTextures();
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::UnlockBuffer(
        D3D9CommonBuffer*       pResource) {
    D3D9DeviceLock lock = LockDevice();

    if (pResource->DecrementLockCount() != 0)
      return D3D_OK;

    if (pResource->GetMapMode() != D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      return D3D_OK;

    if (pResource->DirtyRange().IsDegenerate())
      return D3D_OK;

    pResource->SetMapFlags(0);

    if (pResource->Desc()->Pool != D3DPOOL_DEFAULT || m_d3d9Options.forceDrawTimeBufferUpload)
      return D3D_OK;

    FlushBuffer(pResource);

    return D3D_OK;
  }


  void D3D9DeviceEx::UploadPerDrawData(
          UINT&                   FirstVertexIndex,
          UINT                    NumVertices,
          UINT&                   FirstIndex,
          UINT                    NumIndices,
          INT&                    BaseVertexIndex,
          bool*                   pDynamicVBOs,
          bool*                   pDynamicIBO
  ) {
    const uint32_t usedBuffersMask = (m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetStreamMask() : ~0u) & static_cast<uint32_t>(m_vbSlotTracking.bound);
    bool dynamicSysmemVBOs = usedBuffersMask == m_vbSlotTracking.uploadPerDraw;

    D3D9CommonBuffer* ibo = GetCommonBuffer(m_state.indices);
    bool dynamicSysmemIBO = NumIndices != 0 && ibo != nullptr && (ibo->DoPerDrawUpload() || CanOnlySWVP());

    *pDynamicVBOs = dynamicSysmemVBOs;

    if (unlikely(pDynamicIBO))
      *pDynamicIBO = dynamicSysmemIBO;

    if (likely(!dynamicSysmemVBOs && !dynamicSysmemIBO))
      return;

    uint32_t vertexBuffersToUpload;
    if (likely(dynamicSysmemVBOs))
      vertexBuffersToUpload = m_vbSlotTracking.uploadPerDraw & usedBuffersMask;
    else
      vertexBuffersToUpload = 0;


    struct VBOCopy {
      uint32_t srcOffset;
      uint32_t dstOffset;
      uint32_t copyBufferLength;
      uint32_t copyElementCount;
      uint32_t copyElementSize;
      uint32_t copyElementStride;
    };
    uint32_t totalUpBufferSize = 0;
    std::array<VBOCopy, caps::MaxStreams> vboCopies = {};

    for (uint32_t i : bit::BitMask(vertexBuffersToUpload)) {
      auto* vbo = GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
      if (likely(vbo == nullptr)) {
        continue;
      }

      if (unlikely(vbo->NeedsReadback())) {
        WaitForResource(*vbo->GetBuffer<D3D9_COMMON_BUFFER_TYPE_STAGING>(), vbo->GetMappingBufferSequenceNumber(), D3DLOCK_READONLY);
      }

      const uint32_t vertexSize = m_state.vertexDecl->GetSize(i);
      const uint32_t vertexStride = m_state.vertexBuffers[i].stride;
      const uint32_t srcStride = vertexStride;
      const uint32_t dstStride = std::min(vertexStride, vertexSize);

      uint32_t elementCount = NumVertices;
      if (m_state.streamFreq[i] & D3DSTREAMSOURCE_INSTANCEDATA) {
        elementCount = GetInstanceCount();
      }
      const uint32_t vboOffset = m_state.vertexBuffers[i].offset;
      const uint32_t vertexOffset = (FirstVertexIndex + BaseVertexIndex) * srcStride;
      const uint32_t vertexBufferSize = vbo->Desc()->Size;
      const uint32_t srcOffset = vboOffset + vertexOffset;

      if (unlikely(srcOffset > vertexBufferSize)) {
        vboCopies[i].copyBufferLength = 0;
      } else if (unlikely(srcOffset + elementCount * srcStride > vertexBufferSize)) {
        uint32_t boundVertexBufferRange = vertexBufferSize - vboOffset;
        elementCount = boundVertexBufferRange / srcStride;
        vboCopies[i].copyBufferLength = elementCount * dstStride;
        vboCopies[i].copyBufferLength += std::min(dstStride, boundVertexBufferRange % srcStride);
      } else {
        vboCopies[i].copyBufferLength = elementCount * dstStride;
      }

      vboCopies[i].copyElementCount = elementCount;
      vboCopies[i].copyElementStride = srcStride;
      vboCopies[i].copyElementSize = dstStride;
      vboCopies[i].srcOffset = srcOffset;
      vboCopies[i].dstOffset = totalUpBufferSize;
      totalUpBufferSize += vboCopies[i].copyBufferLength;
    }

    uint32_t iboUPBufferSize = 0;
    uint32_t iboUPBufferOffset = 0;
    if (dynamicSysmemIBO) {
      auto* ibo = GetCommonBuffer(m_state.indices);
      if (likely(ibo != nullptr)) {
        uint32_t indexStride = ibo->Desc()->Format == D3D9Format::INDEX16 ? 2 : 4;
        uint32_t offset = indexStride * FirstIndex;
        uint32_t indexBufferSize = ibo->Desc()->Size;
        if (offset < indexBufferSize) {
          iboUPBufferSize = std::min(NumIndices * indexStride, indexBufferSize - offset);
          iboUPBufferOffset = totalUpBufferSize;
          totalUpBufferSize += iboUPBufferSize;
        }
      }
    }

    if (unlikely(totalUpBufferSize == 0)) {
      *pDynamicVBOs = false;
      if (pDynamicIBO)
        *pDynamicIBO = false;

      return;
    }

    auto upSlice = AllocUPBuffer(totalUpBufferSize);

    if (dynamicSysmemVBOs) {
      for (uint32_t i : bit::BitMask(vertexBuffersToUpload)) {
        const VBOCopy& copy = vboCopies[i];

        if (likely(copy.copyBufferLength != 0)) {
          const auto* vbo = GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
          uint8_t* data = reinterpret_cast<uint8_t*>(upSlice.mapPtr) + copy.dstOffset;
          const uint8_t* src = reinterpret_cast<uint8_t*>(vbo->GetMappedSlice()->mapPtr) + copy.srcOffset;

          if (likely(copy.copyElementStride == copy.copyElementSize)) {
            std::memcpy(data, src, copy.copyBufferLength);
          } else {
            for (uint32_t j = 0; j < copy.copyElementCount; j++) {
              std::memcpy(data + j * copy.copyElementSize, src + j * copy.copyElementStride, copy.copyElementSize);
            }
            if (unlikely(copy.copyBufferLength > copy.copyElementCount * copy.copyElementSize)) {
              std::memcpy(
                data + copy.copyElementCount * copy.copyElementSize,
                src + copy.copyElementCount * copy.copyElementStride,
                copy.copyBufferLength - copy.copyElementCount * copy.copyElementSize);
            }
          }
        }

        auto vboSlice = upSlice.slice.subSlice(copy.dstOffset, copy.copyBufferLength);
        EmitCs([
          cStream      = i,
          cBufferSlice = std::move(vboSlice),
          cStride      = copy.copyElementSize
        ](MtlContext* ctx) mutable {
          ctx->bindVertexBuffer(cStream, std::move(cBufferSlice), cStride);
        });
        m_dirty.set(D3D9DeviceDirtyFlag::VertexBuffers);
      }

      if (NumIndices != 0) {
        BaseVertexIndex = -FirstVertexIndex;
      } else {
        FirstVertexIndex = 0;
      }
    }

    if (dynamicSysmemIBO) {
      if (unlikely(iboUPBufferSize == 0)) {
        EmitCs([](MtlContext* ctx) {
          ctx->bindIndexBuffer(MtlBufferSlice(), MtlIndexType::Uint32);
        });
        m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
      } else {
        auto* ibo = GetCommonBuffer(m_state.indices);
        uint32_t indexStride = ibo->Desc()->Format == D3D9Format::INDEX16 ? 2 : 4;
        MtlIndexType indexType = DecodeIndexType(ibo->Desc()->Format);
        uint32_t offset = indexStride * FirstIndex;
        uint8_t* data = reinterpret_cast<uint8_t*>(upSlice.mapPtr) + iboUPBufferOffset;
        uint8_t* src = reinterpret_cast<uint8_t*>(ibo->GetMappedSlice()->mapPtr) + offset;
        std::memcpy(data, src, iboUPBufferSize);

        auto iboSlice = upSlice.slice.subSlice(iboUPBufferOffset, iboUPBufferSize);
        EmitCs([
          cBufferSlice = std::move(iboSlice),
          cIndexType = indexType
        ](MtlContext* ctx) mutable {
          ctx->bindIndexBuffer(std::move(cBufferSlice), cIndexType);
        });
        m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
      }

      FirstIndex = 0;
    }
  }


  const D3D9ConstantBufferCopy* D3D9DeviceEx::GetOrCreateConstantLayout(
          D3D9ConstantBufferLayout  FloatLayout,
          D3D9ConstantBufferLayout  IntLayout,
          D3D9ConstantBufferLayout  BoolLayout) {
    std::lock_guard lock(m_constantLayoutMutex);

    auto entry = m_constantLayouts.emplace(
      std::move(FloatLayout),
      std::move(IntLayout),
      std::move(BoolLayout));

    return &(*entry.first);
  }


  void D3D9DeviceEx::InjectCsChunk(
          DxvkCsChunkRef&&            Chunk,
          bool                        Synchronize) {
    m_csThread.injectChunk(DxvkCsQueue::HighPriority, std::move(Chunk), Synchronize);
  }


  void D3D9DeviceEx::EmitCsChunk(DxvkCsChunkRef&& chunk) {
    m_initializer->FlushCsChunk();

    m_csDataType = D3D9CmdType::None;
    m_csData = nullptr;

    for (auto& cbv : m_constantBuffers)
      cbv.ResetStreamCommand();

    m_csSeqNum = m_csThread.dispatchChunk(std::move(chunk));
  }


  void D3D9DeviceEx::ConsiderFlush(GpuFlushType FlushType) {
    uint64_t chunkId = GetCurrentSequenceNumber();
    uint64_t submissionId = m_submissionFence->value();

    if (m_flushTracker.considerFlush(FlushType, chunkId, submissionId, 0u))
      ExecuteFlush(false);
  }


  void D3D9DeviceEx::SynchronizeCsThread(uint64_t SequenceNumber) {
    D3D9DeviceLock lock = LockDevice();

    if (SequenceNumber > m_csSeqNum)
      FlushCsChunk();

    m_csThread.synchronize(SequenceNumber);
  }


  void D3D9DeviceEx::SetupFPU() {

#if defined(_MSC_VER)

    _clearfp();

    _controlfp(_MCW_EM, _MCW_EM);

#ifndef _WIN64
    _controlfp(_PC_24, _MCW_PC);
#endif

    _controlfp(_RC_NEAR, _MCW_RC);
#elif (defined(__GNUC__) || defined(__MINGW32__)) && (defined(__i386__) || (defined(__x86_64__) && !defined(__arm64ec__)) || defined(__ia64))

    uint16_t control;

    __asm__ __volatile__("fnstcw %0" : "=m" (*&control));

    control &= 0xF0C0;

    control |= 0x003F;

    __asm__ __volatile__("fldcw %0" : : "m" (*&control));
#else
    Logger::warn("D3D9DeviceEx::SetupFPU: not supported on this arch.");
#endif
  }


  int64_t D3D9DeviceEx::DetermineInitialTextureMemory() {
    const MtlDeviceInfo& adapterInfo = m_dxvkDevice->deviceInfo();
    MtlDeviceSize availableTextureMemory = adapterInfo.maxBufferLength != 0u
      ? adapterInfo.maxBufferLength
      : (MtlDeviceSize(4096) * 1024 * 1024);

    constexpr MtlDeviceSize Megabytes = 1024 * 1024;
    constexpr MtlDeviceSize ReservedMemory = 8 * Megabytes;

    MtlDeviceSize maxMemory = (MtlDeviceSize(m_d3d9Options.maxAvailableMemory) * Megabytes) - 1;
    availableTextureMemory = std::min(availableTextureMemory, maxMemory) - ReservedMemory;

    return int64_t(availableTextureMemory);
  }


  void D3D9DeviceEx::CreateConstantBuffers() {
    for (uint32_t i = 0u; i < m_constantBuffers.size(); i++)
      m_constantBuffers[i] = D3D9ConstantBuffer(this, D3D9ShaderResourceMapping::CbvIndex(i));
  }


  template<D3D9ShaderType ShaderType>
  void D3D9DeviceEx::UpdateShaderConstants() {
    auto& constants = m_consts[uint32_t(ShaderType)];

    if (!constants.dirty)
      return;

    auto shader = ShaderType == D3D9ShaderType::VertexShader
      ? GetCommonShader(m_state.vertexShader)
      : GetCommonShader(m_state.pixelShader);

    const auto* layout = shader->GetConstantLayout();

    uint32_t dynamicFloatCount = 0u;

    if (ShaderType == D3D9ShaderType::VertexShader)
      dynamicFloatCount = layout->getLayout(D3D9ConstantType::Float).computeConstantCount(constants.changedFloatCount);

    bool isDynamicallyIndexed = ShaderType == D3D9ShaderType::VertexShader
      && layout->getLayout(D3D9ConstantType::Float).isDynamicallyIndexed();

    auto dataSize = layout->getAllocationSizes(dynamicFloatCount);

    if (ShaderType == D3D9ShaderType::PixelShader)
      dataSize.boolBufferSize = 0u;

    auto staticSize = dataSize.boolBufferSize + dataSize.intBufferSize;
    auto dynamicSize = dataSize.floatBufferSize;

    if (!isDynamicallyIndexed) {
      staticSize += dynamicSize;
      dynamicSize = 0u;
    }

    if (staticSize) {
      D3D9ConstantBufferCopyArgs copyArgs = {};
      copyArgs.flushNan = m_d3d9Options.d3d9FloatEmulation == D3D9FloatEmulation::Enabled;

      auto& buffer = GetConstantBuffer(ShaderType == D3D9ShaderType::VertexShader
        ? CbvIndex::VSStaticConstants
        : CbvIndex::PSStaticConstants);
      staticSize = align(staticSize, buffer.GetAlignment());

      auto staticData = buffer.Alloc(staticSize);
      auto staticOffset = 0u;

      if (!dynamicSize) {
        copyArgs.floatBuffer = reinterpret_cast<char*>(staticData) + staticOffset;
        copyArgs.floatBufferSize = dataSize.floatBufferSize;
        staticOffset += dataSize.floatBufferSize;
      }

      copyArgs.intBuffer = reinterpret_cast<char*>(staticData) + staticOffset;
      copyArgs.intBufferSize = dataSize.intBufferSize;
      staticOffset += dataSize.intBufferSize;

      copyArgs.boolBuffer = reinterpret_cast<char*>(staticData) + staticOffset;
      copyArgs.boolBufferSize = staticSize - staticOffset;

      if (ShaderType == D3D9ShaderType::VertexShader) {
        copyArgs.constFloatApi = m_state.vsConsts->fConsts;
        copyArgs.constIntApi = m_state.vsConsts->iConsts;
        copyArgs.constBoolApi = m_state.vsConsts->bConsts;
      } else {
        copyArgs.constFloatApi = m_state.psConsts->fConsts;
        copyArgs.constIntApi = m_state.psConsts->iConsts;
      }

      layout->copyConstantData(copyArgs);
    }

    if (dynamicSize) {
      D3D9ConstantBufferCopyArgs copyArgs = {};
      copyArgs.floatConstantCount = dynamicFloatCount;
      copyArgs.flushNan = m_d3d9Options.d3d9FloatEmulation == D3D9FloatEmulation::Enabled;

      dynamicSize += sizeof(Vector4);

      auto& buffer = GetConstantBuffer(CbvIndex::VSDynamicConstants);
      dynamicSize = align(dynamicSize, buffer.GetAlignment());

      copyArgs.floatBuffer = buffer.Alloc(dynamicSize);
      copyArgs.floatBufferSize = dynamicSize;
      copyArgs.constFloatApi = m_state.vsConsts->fConsts;

      layout->copyConstantData(copyArgs);

      if (m_pushData.vs.floatCount != dynamicFloatCount) {
        m_pushData.vs.floatCount = dynamicFloatCount;
        m_dirty.set(D3D9DeviceDirtyFlag::PushDataVs);
      }
    }

    constants.dirty = false;
  }


  void D3D9DeviceEx::UpdateClipPlanes() {
    m_dirty.clr(D3D9DeviceDirtyFlag::ClipPlanes);

    auto dst = GetConstantBuffer(CbvIndex::VSClipPlanes).AllocTyped<D3D9ClipPlane>(caps::MaxClipPlanes);

    uint32_t clipPlaneCount = 0u;
    for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
      D3D9ClipPlane clipPlane = (m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1 << i))
        ? m_state.clipPlanes[i]
        : D3D9ClipPlane();

      if (clipPlane != D3D9ClipPlane())
        dst[clipPlaneCount++] = clipPlane;
    }

    for (uint32_t i = clipPlaneCount; i < caps::MaxClipPlanes; i++)
      dst[i] = D3D9ClipPlane();

    if (m_specData.setClipPlaneCount(clipPlaneCount))
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
  }


  void D3D9DeviceEx::ExecuteFlush(bool Synchronize9On12) {
    D3D9DeviceLock lock = LockDevice();


    m_stagingMemorySignaled = GetStagingMemoryStatistics().allocated;

    uint64_t submissionId = ++m_submissionId;

    EmitCs<false>([
      cSubmissionFence  = m_submissionFence,
      cSubmissionId     = submissionId,
      cSubmissionStatus = Synchronize9On12 ? &m_submitStatus : nullptr,
      cStagingBufferFence = m_stagingBufferFence,
      cStagingBufferAllocated = m_stagingMemorySignaled
    ] (MtlContext* ctx) {
      ctx->signal(cSubmissionFence, cSubmissionId);
      ctx->signal(cStagingBufferFence, cStagingBufferAllocated);
      ctx->flushCommandList(nullptr, cSubmissionStatus);
    });

    FlushCsChunk();

    m_flushSeqNum = m_csSeqNum;
    m_flushTracker.notifyFlush(m_flushSeqNum, submissionId);

    (void) Synchronize9On12;

    m_initializer->NotifyContextFlush();
  }


  void D3D9DeviceEx::Flush() {
    D3D9DeviceLock lock = LockDevice();
    ExecuteFlush(false);
  }


  void D3D9DeviceEx::FlushAndSync9On12() {
    D3D9DeviceLock lock = LockDevice();
    ExecuteFlush(true);
  }


  void D3D9DeviceEx::BeginFrame(Rc<MtlLatencyTracker> LatencyTracker, uint64_t FrameId) {
    D3D9DeviceLock lock = LockDevice();

    EmitCs<false>([
      cTracker = std::move(LatencyTracker),
      cFrameId = FrameId
    ] (MtlContext* ctx) {
      if (cTracker && cTracker->needsAutoMarkers())
        ctx->beginLatencyTracking(cTracker, cFrameId);
    });
  }


  void D3D9DeviceEx::EndFrame(Rc<MtlLatencyTracker> LatencyTracker) {
    D3D9DeviceLock lock = LockDevice();

    EmitCs<false>([
      cTracker = std::move(LatencyTracker)
    ] (MtlContext* ctx) {
      ctx->endFrame();

      if (cTracker && cTracker->needsAutoMarkers())
        ctx->endLatencyTracking(cTracker);
    });
  }


  template <uint32_t Index>
  inline void D3D9DeviceEx::UpdateAnyColorWrites() {
    UpdateActiveHazardsRT(std::numeric_limits<uint32_t>::max());

    if (Index == 0 && m_state.depthStencil != nullptr)
      m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  }


  inline void D3D9DeviceEx::UpdateTextureBitmasks(uint32_t index, DWORD combinedUsage) {
    const uint32_t bit = 1 << index;

    m_textureSlotTracking.rtUsage                &= ~bit;
    m_textureSlotTracking.dsUsage                &= ~bit;
    m_textureSlotTracking.bound                  &= ~bit;
    m_textureSlotTracking.needsUpload            &= ~bit;
    m_textureSlotTracking.needsMipGen            &= ~bit;

    auto tex = GetCommonTexture(m_state.textures[index]);

    if (likely(IsPSSampler(index))) {
      const uint32_t textureType = tex != nullptr
        ? uint32_t(tex->GetType() - D3DRTYPE_TEXTURE)
        : 0;
      const uint32_t offset = index * 2;
      const uint32_t textureBitMask = 0b11u       << offset;
      const uint32_t textureBits    = textureType << offset;

      m_textureSlotTracking.textureType &= ~textureBitMask;
      m_textureSlotTracking.textureType |=  textureBits;
    }

    if (likely(tex != nullptr)) {
      m_textureSlotTracking.bound |= bit;

      if (unlikely(tex->IsRenderTarget()))
        m_textureSlotTracking.rtUsage |= bit;

      if (unlikely(tex->IsDepthStencil()))
        m_textureSlotTracking.dsUsage |= bit;

      if (unlikely(tex->NeedsAnyUpload()))
        m_textureSlotTracking.needsUpload |= bit;

      if (unlikely(tex->NeedsMipGen()))
        m_textureSlotTracking.needsMipGen |= bit;

      const bool oldDepth = m_textureSlotTracking.depth & bit;
      const bool newDepth = tex->IsShadow();

      if (oldDepth != newDepth) {
        m_textureSlotTracking.depth ^= bit;
        m_textureSlotTracking.samplerStateDirty |= bit;
      }

      m_textureSlotTracking.drefClamp &= ~bit;
      m_textureSlotTracking.drefClamp |= uint32_t(tex->IsUpgradedToD32f()) << index;

      if (unlikely(m_textureSlotTracking.fetch4SamplerState & bit))
        UpdateActiveFetch4(index);
    } else {
      if (unlikely(m_textureSlotTracking.fetch4 & bit))
        UpdateActiveFetch4(index);
    }

    if (unlikely(combinedUsage & D3DUSAGE_RENDERTARGET)) {
      UpdateActiveHazardsRT(bit);
    } else {
      m_textureSlotTracking.hazardRT             &= ~bit;
      m_textureSlotTracking.unresolvableHazardRT &= ~bit;
    }

    if (unlikely(combinedUsage & D3DUSAGE_DEPTHSTENCIL)) {
      UpdateActiveHazardsDS(bit);
    } else {
      m_textureSlotTracking.hazardDS             &= ~bit;
      m_textureSlotTracking.unresolvableHazardDS &= ~bit;
    }
  }


  inline void D3D9DeviceEx::UpdateActiveHazardsRT(uint32_t texMask) {
    uint32_t oldHazardMask             = m_textureSlotTracking.hazardRT;
    uint32_t oldUnresolvableHazardMask = m_textureSlotTracking.unresolvableHazardRT;
    m_textureSlotTracking.hazardRT             &= ~texMask;
    m_textureSlotTracking.unresolvableHazardRT &= ~texMask;

    auto psMasks = PSShaderMasks();
    uint32_t rtMask = m_rtSlotTracking.canBeSampled;
    texMask &= m_textureSlotTracking.rtUsage;

    for (uint32_t rtIdx : bit::BitMask(rtMask)) {
      bool anyColorWrite = m_state.renderStates[ColorWriteIndex(rtIdx)] != 0;
      bool shaderWritesToRt = (psMasks.rtMask & (1 << rtIdx)) != 0;
      for (uint32_t samplerIdx : bit::BitMask(texMask)) {
        D3D9Surface* rtSurf = m_state.renderTargets[rtIdx].ptr();

        IDirect3DBaseTexture9* rtBase  = rtSurf->GetBaseTexture();
        IDirect3DBaseTexture9* texBase = m_state.textures[samplerIdx];

        if (likely(rtSurf->GetMipLevel() != 0 || rtBase != texBase))
          continue;

        const bool sampledInShader = !!(psMasks.samplerMask & (1 << samplerIdx));
        const bool wasHazard = !!(oldHazardMask & (1 << samplerIdx));

        if (unlikely(!sampledInShader && ((anyColorWrite && shaderWritesToRt) || !wasHazard)))
          continue;

        m_textureSlotTracking.hazardRT |= 1 << samplerIdx;

        if (unlikely(!sampledInShader))
          continue;

        if (likely(!anyColorWrite || !shaderWritesToRt))
          continue;

        m_textureSlotTracking.unresolvableHazardRT |= 1 << samplerIdx;
      }
    }

    if (unlikely(m_textureSlotTracking.hazardRT != oldHazardMask
      || m_textureSlotTracking.unresolvableHazardRT != oldUnresolvableHazardMask)) {
      m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
    }
  }


  inline void D3D9DeviceEx::UpdateActiveHazardsDS(uint32_t texMask) {
    uint32_t oldHazardMask             = m_textureSlotTracking.hazardDS;
    uint32_t oldUnresolvableHazardMask = m_textureSlotTracking.unresolvableHazardDS;
    m_textureSlotTracking.hazardDS             &= ~texMask;
    m_textureSlotTracking.unresolvableHazardDS &= ~texMask;

    if (m_state.depthStencil == nullptr ||
        m_state.depthStencil->GetBaseTexture() == nullptr)
      return;

    auto psMasks = PSShaderMasks();
    texMask &= m_textureSlotTracking.dsUsage;

    const bool depthWrite = m_state.renderStates[D3DRS_ZENABLE] && m_state.renderStates[D3DRS_ZWRITEENABLE];

    for (uint32_t samplerIdx : bit::BitMask(texMask)) {
      IDirect3DBaseTexture9* dsBase  = m_state.depthStencil->GetBaseTexture();
      IDirect3DBaseTexture9* texBase = m_state.textures[samplerIdx];

      if (likely(dsBase != texBase))
        continue;

      const bool sampledInShader = !!(psMasks.samplerMask & (1 << samplerIdx));
      const bool wasHazard = !!(oldHazardMask & (1 << samplerIdx));

      if (unlikely(!sampledInShader && (depthWrite || !wasHazard)))
        continue;

      m_textureSlotTracking.hazardDS |= 1 << samplerIdx;

      if (unlikely(!sampledInShader))
        continue;

      if (unlikely(!depthWrite))
        continue;

      m_textureSlotTracking.unresolvableHazardDS |= 1 << samplerIdx;
    }

    if (unlikely(m_textureSlotTracking.hazardDS != oldHazardMask
      || m_textureSlotTracking.unresolvableHazardDS != oldUnresolvableHazardMask)) {
      m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
    }
  }


  void D3D9DeviceEx::EmitFeedbackLoopBarriers() {
    struct {
      uint8_t RT : 1;
      uint8_t DS : 1;
    } hazardState;
    hazardState.RT = m_textureSlotTracking.unresolvableHazardRT != 0;
    hazardState.DS = m_textureSlotTracking.unresolvableHazardDS != 0;

    EmitCs([
      cHazardState = hazardState
    ](MtlContext* ctx) {
      MtlStageMask   srcStages = 0u;
      MtlBarrierScope srcScope = 0u;

      if (cHazardState.RT != 0) {
        srcStages |= MtlStageFragment;
        srcScope  |= MtlScopeRenderTgt;
      }
      if (cHazardState.DS != 0) {
        srcStages |= MtlStageFragment;
        srcScope  |= MtlScopeRenderTgt;
      }

      ctx->emitGraphicsBarrier(
        srcStages, srcScope,
        MtlStageFragment, MtlScopeTextures);
    });

    for (uint32_t samplerIdx : bit::BitMask(m_textureSlotTracking.unresolvableHazardRT | m_textureSlotTracking.unresolvableHazardDS)) {
      auto tex = GetCommonTexture(m_state.textures[samplerIdx]);

      if (unlikely(!tex->MarkTransitionedToHazardLayout())) {
        EmitCs([
          cImage  = tex->GetImage()
        ] (MtlContext* ctx) mutable {
          MtlTextureUsage usage = MtlTextureUsageRenderTarget | MtlTextureUsageShaderRead;
          ctx->ensureTextureUsage(std::move(cImage), usage);
        });

        m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
      }
    }
  }


  void D3D9DeviceEx::UpdateActiveFetch4(uint32_t stateSampler) {
    auto& state = m_state.samplerStates;

    const uint32_t samplerBit = 1u << stateSampler;

    auto texture = GetCommonTexture(m_state.textures[stateSampler]);
    const bool textureSupportsFetch4 = texture != nullptr && texture->SupportsFetch4();

    const bool fetch4Enabled = m_textureSlotTracking.fetch4SamplerState & samplerBit;
    const bool pointSampled  = state[stateSampler][D3DSAMP_MAGFILTER] == D3DTEXF_POINT;
    const bool shouldFetch4  = fetch4Enabled && textureSupportsFetch4 && pointSampled;

    if (unlikely(shouldFetch4 != !!(m_textureSlotTracking.fetch4 & samplerBit))) {
      if (shouldFetch4)
        m_textureSlotTracking.fetch4 |= samplerBit;
      else
        m_textureSlotTracking.fetch4 &= ~samplerBit;
    }
  }


  void D3D9DeviceEx::UploadManagedTexture(D3D9CommonTexture* pResource) {
    for (uint32_t subresource = 0; subresource < pResource->CountSubresources(); subresource++) {
      if (!pResource->NeedsUpload(subresource))
        continue;

      this->FlushImage(pResource, subresource);
    }

    pResource->ClearDirtyBoxes();
    pResource->ClearNeedsUpload();
  }


  void D3D9DeviceEx::UploadManagedTextures(uint32_t mask) {
    for (uint32_t texIdx : bit::BitMask(mask))
      UploadManagedTexture(GetCommonTexture(m_state.textures[texIdx]));

    m_textureSlotTracking.needsUpload &= ~mask;
  }


  void D3D9DeviceEx::GenerateTextureMips(uint32_t mask) {
    for (uint32_t texIdx : bit::BitMask(mask)) {
      auto texInfo = GetCommonTexture(m_state.textures[texIdx]);

      if (likely(texInfo->NeedsMipGen())) {
        this->EmitGenerateMips(texInfo);
        if (likely(!IsTextureBoundAsAttachment(texInfo))) {
          texInfo->SetNeedsMipGen(false);
        }
      }
    }

    m_textureSlotTracking.needsMipGen &= ~mask;
  }


  void D3D9DeviceEx::MarkTextureMipsDirty(D3D9CommonTexture* pResource) {
    pResource->SetNeedsMipGen(true);

    for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
      auto texInfo = GetCommonTexture(m_state.textures[i]);

      if (texInfo == pResource) {
        m_textureSlotTracking.needsMipGen |= 1 << i;
        break;
      }
    }
  }


  void D3D9DeviceEx::MarkTextureMipsUnDirty(D3D9CommonTexture* pResource) {
    if (likely(!IsTextureBoundAsAttachment(pResource))) {
      pResource->SetNeedsMipGen(false);

      for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
        auto texInfo = GetCommonTexture(m_state.textures[i]);

        if (unlikely(texInfo == pResource)) {
          m_textureSlotTracking.needsMipGen &= ~(1 << i);
        }
      }
    }
  }


  void D3D9DeviceEx::MarkTextureUploaded(D3D9CommonTexture* pResource) {
    for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
      auto texInfo = GetCommonTexture(m_state.textures[i]);

      if (texInfo == pResource)
        m_textureSlotTracking.needsUpload &= ~(1 << i);
    }
  }


  void D3D9DeviceEx::UpdatePointMode(bool pointList) {
    if (likely(!pointList)) {
      bool dirty = false;
      dirty |= m_specData.setPointSprite(false);
      dirty |= m_specData.setPointScale(false);

      if (unlikely(dirty)) {
        m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries,
                    D3D9DeviceDirtyFlag::PointScale);
      }
    } else {
      auto& rs = m_state.renderStates;

      bool scale = rs[D3DRS_POINTSCALEENABLE] && !UseProgrammableVS();
      bool sprite = rs[D3DRS_POINTSPRITEENABLE];

      if (scale && m_dirty.test(D3D9DeviceDirtyFlag::PointScale)) {
        m_dirty.clr(D3D9DeviceDirtyFlag::PointScale);
        m_dirty.set(D3D9DeviceDirtyFlag::PushDataFfvs);

        float scale = 1.0f / float(m_state.viewport.Height * m_state.viewport.Height);
        m_pushData.ffvs.pointScaleA = scale * bit::cast<float>(rs[D3DRS_POINTSCALE_A]);
        m_pushData.ffvs.pointScaleB = scale * bit::cast<float>(rs[D3DRS_POINTSCALE_B]);
        m_pushData.ffvs.pointScaleC = scale * bit::cast<float>(rs[D3DRS_POINTSCALE_C]);
      }

      bool dirty = false;
      dirty |= m_specData.setPointScale(scale);
      dirty |= m_specData.setPointSprite(sprite);

      if (dirty)
        m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }
  }


  void D3D9DeviceEx::UpdateFog() {
    m_dirty.clr(D3D9DeviceDirtyFlag::Fog);

    auto& rs = m_state.renderStates;

    bool fogEnabled = bool(rs[D3DRS_FOGENABLE]);
    bool fogUseZ = false;

    if (fogEnabled && !UseProgrammableVS()) {
      if (!m_state.vertexDecl || !m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT)) {
        auto [wNear, wFar] = ComputeWNearFar();
        fogUseZ = wNear == 1.0f && wFar == 1.0f;
      }
    }

    if (m_specData.setFogMode(fogEnabled, fogUseZ,
        D3DFOGMODE(rs[D3DRS_FOGVERTEXMODE]),
        D3DFOGMODE(rs[D3DRS_FOGTABLEMODE])))
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);

    if (fogEnabled) {
      uint32_t fogColor = rs[D3DRS_FOGCOLOR];
      float fogDensity = bit::cast<float>(rs[D3DRS_FOGDENSITY]);
      float fogEnd   = bit::cast<float>(rs[D3DRS_FOGEND]);
      float fogStart = bit::cast<float>(rs[D3DRS_FOGSTART]);

      m_pushData.shared.fogColor[0] = uint8_t(fogColor >>  0u);
      m_pushData.shared.fogColor[1] = uint8_t(fogColor >>  8u);
      m_pushData.shared.fogColor[2] = uint8_t(fogColor >> 16u);
      m_pushData.shared.fogDensity = fogDensity;
      m_pushData.shared.fogDistanceEnd = fogEnd;
      m_pushData.shared.fogDistanceScale = (fogEnd != fogStart) ? 1.0f / (fogEnd - fogStart) : 0.0f;

      m_dirty.set(D3D9DeviceDirtyFlag::PushDataShared);
    }
  }


  std::pair<float, float> D3D9DeviceEx::ComputeWNearFar() const {
    const auto& m = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];

    if (m[2][2] == 0.0f || m[2][2] == m[2][3])
      return std::make_pair(1.0f, 1.0f);

    float wNear = m[3][3] - m[3][2] / m[2][2] * m[2][3];
    float wFar = (m[3][3] - m[3][2]) / (m[2][2] - m[2][3]) * m[2][3] + m[3][3];

    return std::make_pair(wNear, wFar);
  }


  template<typename T>
  void D3D9DeviceEx::UpdatePushDataBlock(const T& Block) {
    EmitCs([cBlock = Block] (MtlContext* ctx) {
      ctx->pushData(T::Stages, T::Offset, sizeof(T), &cBlock);
    });
  }


  void D3D9DeviceEx::UpdatePushData() {
    if (m_dirty.test(D3D9DeviceDirtyFlag::PushDataShared))
      UpdatePushDataBlock(m_pushData.shared);
    if (m_dirty.test(D3D9DeviceDirtyFlag::PushDataVs))
      UpdatePushDataBlock(m_pushData.vs);
    if (m_dirty.test(D3D9DeviceDirtyFlag::PushDataFfvs))
      UpdatePushDataBlock(m_pushData.ffvs);
    if (m_dirty.test(D3D9DeviceDirtyFlag::PushDataFfps))
      UpdatePushDataBlock(m_pushData.ffps);

    m_dirty.clr(D3D9DeviceDirtyFlag::PushDataShared,
                D3D9DeviceDirtyFlag::PushDataVs,
                D3D9DeviceDirtyFlag::PushDataFfvs,
                D3D9DeviceDirtyFlag::PushDataFfps);
  }


  void D3D9DeviceEx::BindFramebuffer() {
    m_dirty.clr(D3D9DeviceDirtyFlag::Framebuffer);

    MtlRenderTargets attachments;

    bool srgb = m_state.renderStates[D3DRS_SRGBWRITEENABLE];

    const auto& psMasks = PSShaderMasks();

    uint32_t sampleCount = 1u;
    MtlExtent2D renderArea = { 0u, 0u };
    const D3D9CommonTexture* rt0 = GetCommonTexture(m_state.renderTargets[0].ptr());
    if (likely(rt0 != nullptr && !rt0->IsNull())) {
      const MtlImageInfo& rt0Info = rt0->GetImage()->info();
      sampleCount = rt0Info.sampleCount;
      renderArea = { rt0Info.extent.width, rt0Info.extent.height };
    }

    if (m_state.depthStencil != nullptr) {
      const D3D9CommonTexture* ds = m_state.depthStencil->GetCommonTexture();
      const MtlImageInfo& dsInfo = ds->GetImage()->info();

      uint32_t boundRTCount = 0;
      for (uint32_t i = 0; i < m_state.renderTargets.size(); i++) {
        if (m_state.renderTargets[i] == nullptr)
          continue;

        boundRTCount++;
      }

      const bool rt0WrittenTo = (psMasks.rtMask & 1u) != 0
          && m_state.renderStates[D3DRS_COLORWRITEENABLE] != 0;

      const bool noRT0Bound = rt0 == nullptr || rt0->IsNull();
      const bool ignoreRT0 = boundRTCount == 1
          && renderArea.width == 1
          && renderArea.height == 1
          && !rt0WrittenTo;

      const bool mismatch = dsInfo.extent.width < renderArea.width
        || dsInfo.extent.height < renderArea.height
        || dsInfo.sampleCount != sampleCount;
      const bool bindDS = !mismatch || noRT0Bound || ignoreRT0;

      if (likely(bindDS)) {
        if (unlikely(noRT0Bound || ignoreRT0)) {
          renderArea = { dsInfo.extent.width, dsInfo.extent.height };
          sampleCount = dsInfo.sampleCount;
        }

        bool readOnly = m_textureSlotTracking.hazardDS != 0;
        readOnly &= !m_state.renderStates[D3DRS_ZENABLE]
          || !m_state.renderStates[D3DRS_ZWRITEENABLE];

        readOnly &= !m_state.depthStencil->GetCommonTexture()->HasBeenTransitionedToHazardLayout();

        attachments.depthStencil = m_state.depthStencil->GetDepthStencilView(!readOnly);
      }
    }

    for (uint32_t i = 0u; i < m_state.renderTargets.size(); i++) {
      if (!HasRenderTargetBound(i))
        continue;

      const D3D9CommonTexture* rt = m_state.renderTargets[i]->GetCommonTexture();
      const MtlImageInfo& rtInfo = rt->GetImage()->info();
      if (unlikely(rtInfo.extent.width != renderArea.width
          || rtInfo.extent.height != renderArea.height
          || rtInfo.sampleCount != sampleCount))
        continue;

      bool hasHazard = false;
      for (uint32_t samplerIdx : bit::BitMask(m_textureSlotTracking.hazardRT)) {
        D3D9Surface* rtSurf = m_state.renderTargets[i].ptr();

        IDirect3DBaseTexture9* rtBase  = rtSurf->GetBaseTexture();
        IDirect3DBaseTexture9* texBase = m_state.textures[samplerIdx];

        if (likely(rtSurf->GetMipLevel() == 0 && rtBase == texBase)) {
          hasHazard = true;
          break;
        }
      }

      const uint32_t rtBit = 1u << i;
      const bool writtenTo = (psMasks.rtMask & rtBit) != 0
          && m_state.renderStates[ColorWriteIndex(i)] != 0;

      if (hasHazard && !writtenTo)
        continue;

      attachments.color[i] = m_state.renderTargets[i]->GetRenderTargetView(srgb);
    }

    MtlAspectMask feedbackLoopAspects = 0u;

    if (m_textureSlotTracking.unresolvableHazardRT != 0)
      feedbackLoopAspects |= MtlAspectColor;
    if (m_textureSlotTracking.unresolvableHazardDS != 0)
      feedbackLoopAspects |= MtlAspectDepth;

    EmitCs([
      cAttachments         = std::move(attachments),
      cFeedbackLoopAspects = feedbackLoopAspects
    ] (MtlContext* ctx) mutable {
      ctx->bindRenderTargets(std::move(cAttachments), cFeedbackLoopAspects);
    });
  }


  void D3D9DeviceEx::BindViewportAndScissor() {
    m_dirty.clr(D3D9DeviceDirtyFlag::ViewportScissor);

    const D3DVIEWPORT9& vp = m_state.viewport;

    constexpr float cf = 0.5f;

    float zBias;
    if (vp.MinZ >= 0.5f) {
      zBias = 0.0f;
    } else {
      zBias = 0.001f;
    }

    MtlViewport state = { };
    state.originX = float(vp.X) + cf;
    state.originY = float(vp.Y) + cf;
    state.width   = float(vp.Width);
    state.height  = float(vp.Height);
    state.znear   = std::clamp(vp.MinZ,                            0.0f, 1.0f);
    state.zfar    = std::clamp(std::max(vp.MaxZ, vp.MinZ + zBias), 0.0f, 1.0f);

    bool enableScissorTest = m_state.renderStates[D3DRS_SCISSORTESTENABLE];

    if (enableScissorTest) {
      RECT sr = m_state.scissorRect;

      MtlOffset2D srPosA;
      srPosA.x = std::max<int32_t>(0, sr.left);
      srPosA.x = std::max<int32_t>(vp.X, srPosA.x);
      srPosA.y = std::max<int32_t>(0, sr.top);
      srPosA.y = std::max<int32_t>(vp.Y, srPosA.y);

      MtlOffset2D srPosB;
      srPosB.x = std::max<int32_t>(srPosA.x, sr.right);
      srPosB.x = std::min<int32_t>(vp.X + vp.Width, srPosB.x);
      srPosB.y = std::max<int32_t>(srPosA.y, sr.bottom);
      srPosB.y = std::min<int32_t>(vp.Y + vp.Height, srPosB.y);

      MtlExtent2D srSize;
      srSize.width  = uint32_t(srPosB.x - srPosA.x);
      srSize.height = uint32_t(srPosB.y - srPosA.y);

      state.scissor = MtlRect{ uint32_t(srPosA.x), uint32_t(srPosA.y), srSize.width, srSize.height };
    }
    else {
      state.scissor = MtlRect{ uint32_t(vp.X), uint32_t(vp.Y), vp.Width, vp.Height };
    }

    EmitCs([
      cViewport = state
    ] (MtlContext* ctx) {
      ctx->setViewports(1, &cViewport);
    });
  }


  void D3D9DeviceEx::UpdateAlphaToCoverangeAndAlphaTest() {
    if (likely(!m_isD3D8Compatible)) {
      bool alphaToCoverageEnabled = true;

      const bool isAMDATOCEnabled = m_state.renderStates[D3DRS_POINTSIZE]      == uint32_t(D3D9Format::A2M1);
      const bool isNVATOCEnabled  = m_state.renderStates[D3DRS_ADAPTIVETESS_Y] == uint32_t(D3D9Format::ATOC)
                                      && m_state.renderStates[D3DRS_ALPHATESTENABLE] != 0;
      const bool isAMD = m_adapter->GetVendorId() == uint32_t(MtlGpuVendor::Amd);
      alphaToCoverageEnabled &= (isAMD && isAMDATOCEnabled) || (!isAMD && isNVATOCEnabled);

      const D3D9CommonTexture* rt0 = GetCommonTexture(m_state.renderTargets[0].ptr());
      const bool isMultisampled = rt0 != nullptr && (
        rt0->Desc()->MultiSample >= D3DMULTISAMPLE_2_SAMPLES
        || (rt0->Desc()->MultiSample == D3DMULTISAMPLE_NONMASKABLE && rt0->Desc()->MultisampleQuality > 0)
      );
      alphaToCoverageEnabled &= isMultisampled;

      if (m_atocEnabled != alphaToCoverageEnabled) {
        m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
        m_atocEnabled = alphaToCoverageEnabled;
      }
    }

    bool alphaTestEnabled = m_state.renderStates[D3DRS_ALPHATESTENABLE] && !m_atocEnabled;
    if (m_alphaTestEnabled != alphaTestEnabled) {
      m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
      m_alphaTestEnabled = alphaTestEnabled;
    }
  }


  void D3D9DeviceEx::BindMultiSampleState() {
    m_dirty.clr(D3D9DeviceDirtyFlag::MultiSampleState);

    MtlMultisampleState msState = { };
    msState.setSampleMask(m_validSampleMask
      ? uint16_t(m_state.renderStates[D3DRS_MULTISAMPLEMASK])
      : uint16_t(0xffffu));
    msState.setAlphaToCoverage(m_atocEnabled);

    EmitCs([
      cState = msState
    ] (MtlContext* ctx) {
      ctx->setMultisampleState(cState);
    });
  }


  void D3D9DeviceEx::BindBlendState() {
    m_dirty.clr(D3D9DeviceDirtyFlag::BlendState);

    auto& state = m_state.renderStates;

    MtlBlendMode mode = { };
    mode.setBlendEnable(state[D3DRS_ALPHABLENDENABLE]);

    D3D9BlendState color = { };

    color.Src = D3DBLEND(state[D3DRS_SRCBLEND]);
    color.Dst = D3DBLEND(state[D3DRS_DESTBLEND]);
    color.Op  = D3DBLENDOP(state[D3DRS_BLENDOP]);
    FixupBlendState(color);

    D3D9BlendState alpha = color;

    if (state[D3DRS_SEPARATEALPHABLENDENABLE]) {
      alpha.Src = D3DBLEND(state[D3DRS_SRCBLENDALPHA]);
      alpha.Dst = D3DBLEND(state[D3DRS_DESTBLENDALPHA]);
      alpha.Op  = D3DBLENDOP(state[D3DRS_BLENDOPALPHA]);
      FixupBlendState(alpha);
    }

    mode.setColorOp(DecodeBlendFactor(color.Src, false),
                    DecodeBlendFactor(color.Dst, false),
                    DecodeBlendOp(color.Op));

    mode.setAlphaOp(DecodeBlendFactor(alpha.Src, true),
                    DecodeBlendFactor(alpha.Dst, true),
                    DecodeBlendOp(alpha.Op));

    uint16_t writeMasks = 0;

    for (uint32_t i = 0; i < 4; i++)
      writeMasks |= (state[ColorWriteIndex(i)] & 0xfu) << (4u * i);

    EmitCs([
      cMode       = mode,
      cWriteMasks = writeMasks,
      cAlphaMasks = m_rtSlotTracking.hasAlphaSwizzle
    ](MtlContext* ctx) {
      for (uint32_t i = 0; i < 4; i++) {
        MtlBlendMode mode = cMode;
        mode.setWriteMask(cWriteMasks >> (4u * i));

        if (cAlphaMasks & (1 << i)) {
          auto NormalizeFactor = [] (MtlBlendFactor Factor) {
            if (Factor == MtlBlendFactor::DestAlpha)
              return MtlBlendFactor::One;
            else if (Factor == MtlBlendFactor::OneMinusDestAlpha)
              return MtlBlendFactor::Zero;
            return Factor;
          };

          mode.setColorOp(NormalizeFactor(mode.colorSrcFactor()),
                          NormalizeFactor(mode.colorDstFactor()), mode.colorBlendOp());
          mode.setAlphaOp(NormalizeFactor(mode.alphaSrcFactor()),
                          NormalizeFactor(mode.alphaDstFactor()), mode.alphaBlendOp());
        }

        mode.normalize();

        ctx->setBlendMode(i, mode);
      }
    });
  }


  void D3D9DeviceEx::BindBlendFactor() {
    MtlColor4 blendConstants;
    DecodeD3DCOLOR(
      D3DCOLOR(m_state.renderStates[D3DRS_BLENDFACTOR]),
      reinterpret_cast<float*>(&blendConstants));

    EmitCs([
      cBlendConstants = blendConstants
    ](MtlContext* ctx) {
      ctx->setBlendColor(cBlendConstants);
    });
  }


  void D3D9DeviceEx::BindDepthStencilState() {
    m_dirty.clr(D3D9DeviceDirtyFlag::DepthStencilState);

    auto& rs = m_state.renderStates;

    bool stencil            = rs[D3DRS_STENCILENABLE];
    bool twoSidedStencil    = stencil && rs[D3DRS_TWOSIDEDSTENCILMODE];

    MtlDepthStencilState state = { };
    state.setDepthTest(rs[D3DRS_ZENABLE]);
    state.setDepthWrite(rs[D3DRS_ZWRITEENABLE]);
    state.setStencilTest(stencil);
    state.setDepthCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ZFUNC])));

    MtlStencilState frontOp = { };

    if (stencil) {
      frontOp.setFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILFAIL])));
      frontOp.setPassOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILPASS])));
      frontOp.setDepthFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILZFAIL])));
      frontOp.setCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_STENCILFUNC])));
      frontOp.setCompareMask(rs[D3DRS_STENCILMASK]);
      frontOp.setWriteMask(rs[D3DRS_STENCILWRITEMASK]);
    }

    MtlStencilState backOp = frontOp;

    if (twoSidedStencil) {
      backOp.setFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILFAIL])));
      backOp.setPassOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILPASS])));
      backOp.setDepthFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILZFAIL])));
      backOp.setCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_CCW_STENCILFUNC])));
      backOp.setCompareMask(rs[D3DRS_STENCILMASK]);
      backOp.setWriteMask(rs[D3DRS_STENCILWRITEMASK]);
    }

    state.setStencilOpFront(frontOp);
    state.setStencilOpBack(backOp);

    EmitCs([
      cState = state
    ] (MtlContext* ctx) mutable {
      cState.normalize();

      ctx->setDepthStencilState(cState);
    });
  }


  void D3D9DeviceEx::BindRasterizerState() {
    m_dirty.clr(D3D9DeviceDirtyFlag::RasterizerState);

    auto& rs = m_state.renderStates;

    MtlRasterizerState state = { };
    state.setCullMode(DecodeCullMode(D3DCULL(rs[D3DRS_CULLMODE])));
    state.setDepthClip(true);
    state.setFrontFace(MtlWinding::Clockwise);
    state.setPolygonMode(DecodeFillMode(D3DFILLMODE(rs[D3DRS_FILLMODE])));
    state.setFlatShading(m_state.renderStates[D3DRS_SHADEMODE] == D3DSHADE_FLAT);
    state.setSampleCount(m_state.renderStates[D3DRS_MULTISAMPLEANTIALIAS]
      ? uint32_t(0u)
      : uint32_t(1u));

    EmitCs([
      cState  = state
    ](MtlContext* ctx) {
      ctx->setRasterizerState(cState);
    });
  }


  void D3D9DeviceEx::BindDepthBias() {
    m_dirty.clr(D3D9DeviceDirtyFlag::DepthBias);

    auto& rs = m_state.renderStates;

    float depthBias            = bit::cast<float>(rs[D3DRS_DEPTHBIAS]) * m_depthBiasScale;
    float slopeScaledDepthBias = bit::cast<float>(rs[D3DRS_SLOPESCALEDEPTHBIAS]);

    MtlDepthBias biases;
    biases.constantFactor = depthBias;
    biases.slopeFactor    = slopeScaledDepthBias;
    biases.clamp    = 0.0f;

    EmitCs([
      cBiases = biases
    ](MtlContext* ctx) {
      ctx->setDepthBias(cBiases);
    });
  }


  uint32_t D3D9DeviceEx::GetAlphaTestPrecision(D3D9Surface* rt) {
    if (unlikely(!rt))
      return 0;

    D3D9Format format = rt->GetCommonTexture()->Desc()->Format;

    switch (format) {
      case D3D9Format::A2B10G10R10:
      case D3D9Format::A2R10G10B10:
      case D3D9Format::A2W10V10U10:
      case D3D9Format::A2B10G10R10_XR_BIAS:
        return 0x2;

      case D3D9Format::R16F:
      case D3D9Format::G16R16F:
      case D3D9Format::A16B16G16R16F:
        return 0x7;

      case D3D9Format::G16R16:
      case D3D9Format::A16B16G16R16:
      case D3D9Format::V16U16:
      case D3D9Format::L16:
      case D3D9Format::Q16W16V16U16:
        return 0x8;

      case D3D9Format::R32F:
      case D3D9Format::G32R32F:
      case D3D9Format::A32B32G32R32F:
        return 0xF;

      default:
        return 0x0;
    }
  }


  void D3D9DeviceEx::BindAlphaTestState() {
    m_dirty.clr(D3D9DeviceDirtyFlag::AlphaTestState);

    auto& rs = m_state.renderStates;

    MtlCompareFunction alphaOp = MtlCompareFunction::Always;

    if (m_alphaTestEnabled)
      alphaOp = DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ALPHAFUNC]));

    if (m_specData.setAlphaCompareOp(alphaOp))
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
  }


  void D3D9DeviceEx::BindDepthStencilReference() {
    auto& rs = m_state.renderStates;

    uint32_t ref = uint32_t(rs[D3DRS_STENCILREF]) & 0xff;

    EmitCs([cRef = ref] (MtlContext* ctx) {
      ctx->setStencilReference(cRef);
    });
  }


  void D3D9DeviceEx::BindSampler(DWORD Sampler) {
    m_samplerBindCount++;

    const D3D9CommonTexture* tex = GetCommonTexture(m_state.textures[Sampler]);
    const bool srgb = m_state.samplerStates[Sampler][D3DSAMP_SRGBTEXTURE] & 0x1;

    Rc<MtlTextureView> imageView;

    if (tex && SamplerUsesBorderColor(Sampler))
      imageView = tex->GetSampleView(srgb);

    EmitCs([this,
      cSlot       = Sampler,
      cState      = D3D9SamplerInfo(m_state.samplerStates[Sampler]),
      cIsCube     = tex && tex->IsCube(),
      cIsMultiMip = tex && (tex->Desc()->MipLevels > 1u),
      cIsDepth    = bool(m_textureSlotTracking.depth & (1u << Sampler)),
      cView       = std::move(imageView),
      cBindId     = m_samplerBindCount
    ] (MtlContext* ctx) {
      MtlSamplerKey key = { };

      key.minFilter = DecodeFilter(cState.minFilter);
      key.magFilter = DecodeFilter(cState.magFilter);
      key.mipMode   = MtlSamplerMipMode(DecodeMipFilter(cState.mipFilter));

      if (cIsCube) {
        key.addressU = key.addressV = key.addressW = MtlSamplerAddressMode::ClampToEdge;
      } else {
        key.addressU = DecodeAddressMode(cState.addressU);
        key.addressV = DecodeAddressMode(cState.addressV);
        key.addressW = DecodeAddressMode(cState.addressW);
      }

      key.compareEnable = cIsDepth;
      key.compareOp     = cIsDepth ? MtlCompareFunction::LessEqual : MtlCompareFunction::Never;

      if (cState.mipFilter) {
        uint32_t anisotropy = cState.maxAnisotropy;

        if (cState.minFilter != D3DTEXF_ANISOTROPIC || !cIsMultiMip)
          anisotropy = 0u;

        if (m_d3d9Options.samplerAnisotropy != -1 && cIsMultiMip && cState.minFilter > D3DTEXF_POINT)
          anisotropy = m_d3d9Options.samplerAnisotropy;

        key.maxAnisotropy = std::max(anisotropy, 1u);

        float lodBias = cState.mipLodBias;
        lodBias += m_d3d9Options.samplerLodBias;

        if (m_d3d9Options.clampNegativeLodBias)
          lodBias = std::max(lodBias, 0.0f);

        key.mipLodBias = lodBias;
        key.minLod     = float(cState.maxMipLevel);
        key.maxLod     = 16.0f;
      }


      auto [stage, slot] = D3D9ShaderResourceMapping::getTextureSlotInfo(cSlot);
      ctx->bindSampler(stage, slot, m_dxvkDevice->createSampler(key));

      (void) cBindId;
    });
  }


  void D3D9DeviceEx::BindTexture(DWORD StateSampler) {
    bool srgb = m_state.samplerStates[StateSampler][D3DSAMP_SRGBTEXTURE] & 0x1;
    D3D9CommonTexture* commonTex = GetCommonTexture(m_state.textures[StateSampler]);

    EmitCs([
      cSlot       = StateSampler,
      cImageView  = commonTex->GetSampleView(srgb)
    ](MtlContext* ctx) mutable {
      auto [stage, slot] = D3D9ShaderResourceMapping::getTextureSlotInfo(cSlot);
      ctx->bindTexture(stage, slot, std::move(cImageView));
    });
  }


  void D3D9DeviceEx::UnbindTextures(uint32_t mask) {
    EmitCs([cMask = mask] (MtlContext* ctx) {
      for (uint32_t i : bit::BitMask(cMask)) {
        auto [stage, slot] = D3D9ShaderResourceMapping::getTextureSlotInfo(i);
        ctx->bindTexture(stage, slot, nullptr);
      }
    });
  }


  void D3D9DeviceEx::UndirtySamplers(uint32_t mask) {
    EnsureSamplerLimit();

    for (uint32_t i : bit::BitMask(mask))
      BindSampler(i);

    m_textureSlotTracking.samplerStateDirty &= ~mask;
  }


  void D3D9DeviceEx::UndirtyTextures(uint32_t usedMask) {
    const uint32_t activeMask   = usedMask & m_textureSlotTracking.bound;
    const uint32_t inactiveMask = usedMask & ~m_textureSlotTracking.bound;

    for (uint32_t i : bit::BitMask(activeMask))
      BindTexture(i);

    if (inactiveMask)
      UnbindTextures(inactiveMask);

    m_textureSlotTracking.textureDirty &= ~usedMask;
  }

  void D3D9DeviceEx::MarkTextureBindingDirty(IDirect3DBaseTexture9* texture) {
    D3D9DeviceLock lock = LockDevice();

    for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
      if (m_state.textures[i] == texture)
        m_textureSlotTracking.textureDirty |= 1u << i;
    }
  }


  bool D3D9DeviceEx::SamplerUsesBorderColor(DWORD Sampler) const {
    const auto& sampler = m_state.samplerStates[Sampler];

    return sampler[D3DSAMP_ADDRESSU] == D3DTADDRESS_BORDER
        || sampler[D3DSAMP_ADDRESSV] == D3DTADDRESS_BORDER
        || sampler[D3DSAMP_ADDRESSW] == D3DTADDRESS_BORDER;
  }


  D3D9DrawInfo D3D9DeviceEx::GenerateDrawInfo(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             PrimitiveCount,
          UINT             InstanceCount) {
    D3D9DrawInfo drawInfo;
    drawInfo.vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
    drawInfo.instanceCount = (m_iaState.streamsInstanced & m_iaState.streamsUsed)
      ? InstanceCount
      : 1u;
    return drawInfo;
  }


  uint32_t D3D9DeviceEx::GetInstanceCount() const {
    uint32_t instanceCount = std::max(m_state.streamFreq[0] & 0x7FFFFFu, 1u);
    return m_vbSlotTracking.instanced ? instanceCount : 1u;
  }


  void D3D9DeviceEx::PrepareDraw(D3DPRIMITIVETYPE PrimitiveType, bool UploadVBOs, bool UploadIBO) {
    if (unlikely(!UseProgrammablePS()) && m_dirty.test(D3D9DeviceDirtyFlag::FFPixelShader))
      UpdateFixedFunctionPS();

    if (unlikely(m_textureSlotTracking.unresolvableHazardRT != 0 || m_textureSlotTracking.unresolvableHazardDS != 0))
      EmitFeedbackLoopBarriers();

    if (likely(UploadVBOs)) {
      const uint32_t usedBuffersMask = m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetStreamMask() : ~0u;
      const uint32_t buffersToUpload = m_vbSlotTracking.needsUpload & usedBuffersMask;
      for (uint32_t bufferIdx : bit::BitMask(buffersToUpload)) {
        auto* vbo = GetCommonBuffer(m_state.vertexBuffers[bufferIdx].vertexBuffer);
        if (likely(vbo != nullptr && vbo->NeedsUpload()))
          FlushBuffer(vbo);
      }
      m_vbSlotTracking.needsUpload &= ~buffersToUpload;
    }

    const uint32_t usedSamplerMask = PSShaderMasks().samplerMask | VSShaderMasks().samplerMask;
    const uint32_t usedTextureMask = m_textureSlotTracking.bound & usedSamplerMask;

    const uint32_t texturesToUpload = m_textureSlotTracking.needsUpload & usedTextureMask;
    if (unlikely(texturesToUpload != 0))
      UploadManagedTextures(texturesToUpload);

    const uint32_t texturesToGen = m_textureSlotTracking.needsMipGen & usedTextureMask;
    if (unlikely(texturesToGen != 0))
      GenerateTextureMips(texturesToGen);

    auto* ibo = GetCommonBuffer(m_state.indices);
    if (unlikely(UploadIBO && ibo != nullptr && ibo->NeedsUpload()))
      FlushBuffer(ibo);

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::Fog)))
      UpdateFog();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::Framebuffer)))
      BindFramebuffer();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::ViewportScissor)))
      BindViewportAndScissor();

    const uint32_t activeDirtySamplers = m_textureSlotTracking.samplerStateDirty & usedTextureMask;
    if (unlikely(activeDirtySamplers))
      UndirtySamplers(activeDirtySamplers);

    const uint32_t usedDirtyTextures = m_textureSlotTracking.textureDirty & usedSamplerMask;
    if (likely(usedDirtyTextures))
      UndirtyTextures(usedDirtyTextures);

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::BlendState)))
      BindBlendState();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthStencilState)))
      BindDepthStencilState();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::RasterizerState)))
      BindRasterizerState();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthBias)))
      BindDepthBias();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::MultiSampleState)))
      BindMultiSampleState();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::AlphaTestState)))
      BindAlphaTestState();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::ClipPlanes)))
      UpdateClipPlanes();

    UpdatePointMode(PrimitiveType == D3DPT_POINTLIST);

    if (likely(UseProgrammableVS())) {
      UpdateShaderConstants<D3D9ShaderType::VertexShader>();

      if (likely(!CanSWVP())) {
        uint32_t boolConstants = m_state.vsConsts->bConsts[0]
          & m_consts[uint32_t(D3D9ShaderType::VertexShader)].shaderConstantsInfo.boolMask;

        if (m_specData.setVsBoolConstants(boolConstants))
          m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
      }
    } else {
      UpdateFixedFunctionVS();
    }

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::InputLayout)))
      BindInputLayout();

    if (likely(UseProgrammablePS())) {
      UpdateShaderConstants<D3D9ShaderType::PixelShader>();

      uint32_t boolConstants = m_state.psConsts->bConsts[0]
        & m_consts[uint32_t(D3D9ShaderType::PixelShader)].shaderConstantsInfo.boolMask;

      if (m_specData.setPsBoolConstants(boolConstants))
        m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }

    BindProgrammablePixelShaderLinkage();

    uint32_t nullOrUnusedMask = ~usedSamplerMask | ~usedTextureMask;
    uint32_t depthTextureMask = m_textureSlotTracking.depth;
    uint32_t drefClampMask = m_textureSlotTracking.drefClamp;
    uint32_t fetch4TextureMask = UseProgrammablePS() ? m_textureSlotTracking.fetch4 : 0u;

    bool specDirty = false;
    specDirty |= m_specData.setSamplerProjectionMask(m_textureSlotTracking.projected & usedTextureMask);
    specDirty |= m_specData.setPsSamplers(m_textureSlotTracking.textureType,
      nullOrUnusedMask, fetch4TextureMask, depthTextureMask, drefClampMask);
    specDirty |= m_specData.setVsSamplers(nullOrUnusedMask, depthTextureMask, drefClampMask);

    if (specDirty)
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::SharedPixelShaderData))) {
      m_dirty.clr(D3D9DeviceDirtyFlag::SharedPixelShaderData);

      auto data = GetConstantBuffer(CbvIndex::PSShared).AllocTyped<D3D9SharedPS>(1u);

      for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
        data->Stages[i].Constant = m_state.textureStages[i][DXVK_TSS_CONSTANT];
        data->Stages[i].Padding = 0u;

        data->Stages[i].BumpEnvMat[0][0] = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT00]);
        data->Stages[i].BumpEnvMat[1][0] = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT01]);
        data->Stages[i].BumpEnvMat[0][1] = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT10]);
        data->Stages[i].BumpEnvMat[1][1] = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT11]);

        data->Stages[i].BumpEnvLScale    = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVLSCALE]);
        data->Stages[i].BumpEnvLOffset   = bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVLOFFSET]);
      }
    }

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthBounds))) {
      m_dirty.clr(D3D9DeviceDirtyFlag::DepthBounds);

      MtlDepthBounds db = { };
      db.minDepth = 0.0f;
      db.maxDepth = 1.0f;

      if (m_nvdbEnabled) {
        db.minDepth = std::clamp(bit::cast<float>(m_state.renderStates[D3DRS_ADAPTIVETESS_Z]), 0.0f, 1.0f);
        db.maxDepth = std::clamp(bit::cast<float>(m_state.renderStates[D3DRS_ADAPTIVETESS_W]), 0.0f, 1.0f);

        if (db.maxDepth < db.minDepth) {
          db.minDepth = 0.0f;
          db.maxDepth = 1.0f;
        }
      }

      EmitCs([
        cDepthBounds = db
      ] (MtlContext* ctx) {
        ctx->setDepthBounds(cDepthBounds);
      });
    }

    if (m_dirty.test(D3D9DeviceDirtyFlag::SpecializationEntries))
      BindSpecConstants();

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::VertexBuffers) && UploadVBOs)) {
      for (uint32_t i = 0; i < caps::MaxStreams; i++) {
        const D3D9VBO& vbo = m_state.vertexBuffers[i];
        BindVertexBuffer(i, vbo.vertexBuffer.ptr(),
          vbo.offset, vbo.length, vbo.stride);
      }
      m_dirty.clr(D3D9DeviceDirtyFlag::VertexBuffers);
    }

    if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::IndexBuffer) && UploadIBO)) {
      BindIndices();
      m_dirty.clr(D3D9DeviceDirtyFlag::IndexBuffer);
    }

    if (m_dirty.any(D3D9DeviceDirtyFlag::PushDataShared,
                    D3D9DeviceDirtyFlag::PushDataVs,
                    D3D9DeviceDirtyFlag::PushDataFfvs,
                    D3D9DeviceDirtyFlag::PushDataFfps))
      UpdatePushData();

    ApplyPrimitiveType(PrimitiveType);
  }


  void D3D9DeviceEx::EnsureSamplerLimit() {
  }


  template <D3D9ShaderType ShaderStage>
  void D3D9DeviceEx::BindShader(
  const D3D9CommonShader*                 pShaderModule) {
    auto shader = pShaderModule->GetShader();


    EmitCs([
      cShader = std::move(shader)
    ] (MtlContext* ctx) mutable {
      constexpr MtlShaderStage stage = ShaderStage == D3D9ShaderType::VertexShader
        ? MtlShaderStage::Vertex : MtlShaderStage::Fragment;
      ctx->bindShader(stage, std::move(cShader));
    });
  }


  template <D3D9ShaderType ShaderStage>
  void D3D9DeviceEx::BindFFUbershader() {
    constexpr MtlShaderStage Stage = ShaderStage == D3D9ShaderType::VertexShader
      ? MtlShaderStage::Vertex
      : MtlShaderStage::Fragment;

    EmitCs([
      cShader = m_ffModules.GetShader<ShaderStage>()
    ](MtlContext* ctx) mutable {
      ctx->bindShader(Stage, std::move(cShader));
    });
  }


  void D3D9DeviceEx::BindProgrammablePixelShaderLinkage() {
    if (!UseProgrammablePS() || !UseProgrammableVS())
      return;

    const D3D9CommonShader* vs = GetCommonShader(m_state.vertexShader);
    const D3D9CommonShader* ps = GetCommonShader(m_state.pixelShader);

    if (vs == nullptr || ps == nullptr)
      return;

    uint64_t linkageHash = vs->GetOutputIoHash();
    linkageHash = linkageHash * 1099511628211ull ^ uint64_t(uintptr_t(vs));
    linkageHash = linkageHash * 1099511628211ull ^ uint64_t(uintptr_t(ps));

    if (linkageHash == m_boundPsLinkageHash)
      return;

    m_boundPsLinkageHash = linkageHash;

    Rc<MtlShaderFunction> shader = ps->GetShaderForLinkage(vs);

    EmitCs([
      cShader = std::move(shader)
    ] (MtlContext* ctx) mutable {
      ctx->bindShader(MtlShaderStage::Fragment, std::move(cShader));
    });
  }


  void D3D9DeviceEx::BindInputLayout() {
    m_dirty.clr(D3D9DeviceDirtyFlag::InputLayout);

    if (likely(m_state.vertexDecl)) {
      std::array<uint32_t, caps::MaxStreams> streamFreq;

      for (uint32_t i = 0; i < caps::MaxStreams; i++)
        streamFreq[i] = m_state.streamFreq[i];

      const auto& vertexElements = m_state.vertexDecl->GetElements();

      const D3D9InputSignature& inputSignature = UseProgrammableVS()
        ? GetCommonShader(m_state.vertexShader)->GetInputSignature()
        : GetFixedFunctionIsgn();

      auto elementCount = vertexElements.size();
      auto elementBlock = EmitCsCmd<D3DVERTEXELEMENT9>(D3D9CmdType::None, elementCount, [
        &cIaState         = m_iaState,
        cInputSignature   = inputSignature,
        cStreamsInstanced = m_vbSlotTracking.instanced,
        cStreamFreq       = streamFreq
      ] (MtlContext* ctx, const D3DVERTEXELEMENT9* elements, uint32_t elementCount) {
        uint32_t attrCount = cInputSignature.size();

        std::array<uint8_t, caps::InputRegisterCount * 2u> attrMap;
        std::fill(attrMap.begin(), attrMap.end(), 0xffu);

        for (uint32_t i = 0u; i < elementCount; i++) {
          dxbc_spv::sm3::Semantic elementSemantic = {};
          elementSemantic.usage = dxbc_spv::sm3::SemanticUsage(elements[i].Usage);
          elementSemantic.index = elements[i].UsageIndex;

          if (elementSemantic.usage == dxbc_spv::sm3::SemanticUsage::ePositionT)
            elementSemantic.usage = dxbc_spv::sm3::SemanticUsage::ePosition;

          uint32_t index = cInputSignature.find(elementSemantic);

          if (index < attrCount)
            attrMap[index] = uint8_t(i);
        }

        std::array<MtlVertexAttr,   caps::InputRegisterCount * 2u> attrList = {};
        std::array<MtlVertexLayout, caps::MaxStreams + 1u> bindList = {};
        std::array<uint16_t,        caps::MaxStreams + 1u> vertexSizes = {};

        uint32_t bindMask = 0;

        for (uint32_t i = 0; i < attrCount; i++) {
          MtlVertexAttr attrib = {};
          attrib.location = i;

          if (likely(attrMap[i] < elementCount)) {
            const auto& element = elements[attrMap[i]];
            attrib.binding = uint32_t(element.Stream);
            attrib.format = DecodeDecltype(D3DDECLTYPE(element.Type));
            attrib.offset = element.Offset;
          } else {
            attrib.binding = NullStreamIdx;
            attrib.format = MtlFormat::RGBA32Float;
            attrib.offset = 0;
          }

          attrList[i] = attrib;

          vertexSizes[attrib.binding] = std::max(vertexSizes[attrib.binding],
            uint16_t(attrib.offset + lookupFormatInfo(attrib.format)->elementSize));

          bindMask |= 1u << attrib.binding;
        }

        uint32_t bindCount = 0u;

        for (auto i : bit::BitMask(bindMask)) {
          MtlVertexLayout binding = { };
          binding.binding = i;
          binding.stride  = vertexSizes[i];

          if (likely(i < NullStreamIdx)) {
            uint32_t instanceData = cStreamFreq[binding.binding % caps::MaxStreams];

            if (instanceData & D3DSTREAMSOURCE_INSTANCEDATA) {
              binding.divisor = instanceData & 0x7fffffu;
              binding.perInstance = true;
            } else {
              binding.divisor = 0u;
              binding.perInstance = false;
            }
          } else {
            binding.divisor = 0u;
            binding.perInstance = true;
          }

          bindList[bindCount++] = binding;
        }

        ctx->setVertexLayout(
          attrCount, attrList.data(),
          bindCount, bindList.data());

        cIaState.streamsInstanced = cStreamsInstanced;
        cIaState.streamsUsed = bindMask;
      });

      for (uint32_t i = 0u; i < elementCount; i++)
        new (elementBlock->at(i)) D3DVERTEXELEMENT9(vertexElements[i]);
    } else {
      EmitCs([&cIaState = m_iaState] (MtlContext* ctx) {
        cIaState.streamsUsed = 0;
        ctx->setVertexLayout(0, nullptr, 0, nullptr);
      });
    }
  }


  void D3D9DeviceEx::BindVertexBuffer(
        UINT                              Slot,
        D3D9VertexBuffer*                 pBuffer,
        UINT                              Offset,
        UINT                              Length,
        UINT                              Stride) {
    if (likely(pBuffer)) {
      Offset = std::min(Offset, Length);

      EmitCs([
        cSlotId       = Slot,
        cBuffer       = pBuffer->GetCommonBuffer()->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>(),
        cOffset       = Offset,
        cLength       = Length - Offset,
        cStride       = Stride
      ] (MtlContext* ctx) mutable {
        MtlBufferSlice slice(std::move(cBuffer), cOffset, cLength);
        ctx->bindVertexBuffer(cSlotId, std::move(slice), cStride);
      });
    } else {
      EmitCs([cSlotId = Slot] (MtlContext* ctx) mutable {
        ctx->bindVertexBuffer(cSlotId, MtlBufferSlice(), 0);
      });
    }
  }


  void D3D9DeviceEx::BindVertexBufferRange(
          UINT                              Slot,
          UINT                              Offset,
          UINT                              Length,
          UINT                              Stride) {
    Offset = std::min(Offset, Length);

    EmitCs([
      cSlotId       = Slot,
      cOffset       = Offset,
      cLength       = Length - Offset,
      cStride       = Stride
    ] (MtlContext* ctx) {
      ctx->bindVertexBufferRange(cSlotId, cOffset, cLength, cStride);
    });
  }


  void D3D9DeviceEx::BindIndices() {
    if (likely(m_state.indices)) {
      D3D9CommonBuffer* buffer = m_state.indices->GetCommonBuffer();
      MtlIndexType indexType = DecodeIndexType(buffer->Desc()->Format);

      EmitCs([
        cBufferSlice = buffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(),
        cIndexType   = indexType
      ](MtlContext* ctx) mutable {
        ctx->bindIndexBuffer(std::move(cBufferSlice), cIndexType);
      });
    } else {
      EmitCs([] (MtlContext* ctx) {
        ctx->bindIndexBuffer(MtlBufferSlice(), MtlIndexType::Uint32);
      });
    }
  }


  void D3D9DeviceEx::Begin(D3D9Query* pQuery) {
    D3D9DeviceLock lock = LockDevice();

    EmitCs([cQuery = Com<D3D9Query, false>(pQuery)](MtlContext* ctx) {
      cQuery->Begin(ctx);
    });
  }


  void D3D9DeviceEx::End(D3D9Query* pQuery) {
    D3D9DeviceLock lock = LockDevice();

    EmitCs([cQuery = Com<D3D9Query, false>(pQuery)](MtlContext* ctx) {
      cQuery->End(ctx);
    });

    pQuery->NotifyEnd();
    if (unlikely(pQuery->IsEvent())) {
      pQuery->IsStalling()
        ? ExecuteFlush(false)
        : ConsiderFlush(GpuFlushType::ImplicitStrongHint);
    } else if (pQuery->IsStalling()) {
      ConsiderFlush(GpuFlushType::ImplicitWeakHint);
    }
  }


  void D3D9DeviceEx::SetVertexBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits) {
    m_state.vsConsts->bConsts[idx] &= ~mask;
    m_state.vsConsts->bConsts[idx] |= bits & mask;

    m_consts[uint32_t(D3D9ShaderType::VertexShader)].dirty |= CanSWVP();
  }


  void D3D9DeviceEx::SetPixelBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits) {
    m_state.psConsts->bConsts[idx] &= ~mask;
    m_state.psConsts->bConsts[idx] |= bits & mask;
  }


  void D3D9DeviceEx::InitShaderOptions() {
    m_shaderOptions.d3d9FloatEmulation = m_d3d9Options.d3d9FloatEmulation;
    m_shaderOptions.isSWVP = CanSWVP();
    m_shaderOptions.forceSamplerTypeSpecConstants = m_d3d9Options.forceSamplerTypeSpecConstants;
  }


  HRESULT D3D9DeviceEx::CreateShaderModule(
          D3D9CommonShader*       pShaderModule,
          size_t*                 pBytecodeLength,
          D3D9ShaderType          ShaderType,
    const DWORD*                  pShaderBytecode) {

    if (!pShaderBytecode)
      return D3DERR_INVALIDCALL;

    dxbc_spv::util::ByteReader reader(pShaderBytecode, std::numeric_limits<size_t>::max());

    D3D9ShaderAnalysis analysis(reader, CanSWVP());

    if (!analysis) {
      Logger::err("CreateShaderModule: Shader analysis prepass failed");
      return D3DERR_INVALIDCALL;
    }

    auto info = analysis.GetShaderInfo();

    size_t bytecodeLength = analysis.GetLength();

    if (unlikely(ShaderType != D3D9ShaderType(info.getType()))) {
      Logger::err("CreateShaderModule: Bytecode does not match shader stage");
      return D3DERR_INVALIDCALL;
    }

    if (ShaderType == D3D9ShaderType::VertexShader) {
      const uint32_t shaderModelVS = IsD3D8Compatible() ? 1u : std::max(1u, m_d3d9Options.shaderModel);

      if (unlikely(info.getVersion().first > shaderModelVS
               || (info.getVersion().first == 1 && info.getVersion().second > 1)
               || (info.getVersion().first == 3 && info.getVersion().second != 0))) {
        Logger::err(str::format("CreateShaderModule: Unsupported VS version ", info.getVersion().first, ".", info.getVersion().second));
        return D3DERR_INVALIDCALL;
      }
    } else if (ShaderType == D3D9ShaderType::PixelShader) {
      const uint32_t shaderModelPS = IsD3D8Compatible() ? std::min(1u, m_d3d9Options.shaderModel) : m_d3d9Options.shaderModel;

      if (unlikely(info.getVersion().first > shaderModelPS
               || (info.getVersion().first == 1 && info.getVersion().second > 4)
               || (info.getVersion().first == 3 && info.getVersion().second != 0))) {
        Logger::err(str::format("CreateShaderModule: Unsupported PS version ", info.getVersion().first, ".", info.getVersion().second));
        return D3DERR_INVALIDCALL;
      }
    } else {
      Logger::err("CreateShaderModule: Unsupported shader stage");
      return D3DERR_INVALIDCALL;
    }

    const uint32_t floatConstantCount = analysis.GetImmediateConstants().floatCount;
    const uint32_t intConstantCount   = analysis.GetImmediateConstants().intCount;
    const uint32_t boolConstantCount  = analysis.GetImmediateConstants().boolCount;

    if (!CanSWVP() && ShaderType == D3D9ShaderType::VertexShader) {

      if (unlikely(floatConstantCount > caps::MaxFloatConstantsVS)) {
        Logger::err(str::format("CreateShaderModule: Invalid VS float constant index ", floatConstantCount));
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(intConstantCount > caps::MaxOtherConstants)) {
        Logger::err(str::format("CreateShaderModule: Invalid VS int constant index ", intConstantCount));
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(boolConstantCount > caps::MaxOtherConstants)) {
        Logger::err(str::format("CreateShaderModule: Invalid VS bool constant index ", boolConstantCount));
        return D3DERR_INVALIDCALL;
      }

    } else if (ShaderType == D3D9ShaderType::PixelShader) {

      uint32_t majorVersion = analysis.GetShaderInfo().getVersion().first;
      uint32_t minorVersion = analysis.GetShaderInfo().getVersion().second;

      const bool isSM2XOrNewer = majorVersion == 3 || (majorVersion == 2 && minorVersion != 0);
      const uint32_t maxFloatConstantsPS = majorVersion == 3 ? caps::MaxSM3FloatConstantsPS :
                                           majorVersion == 2 ? caps::MaxSM2FloatConstantsPS :
                                           caps::MaxSM1FloatConstantsPS;

      if (unlikely(!CanSWVP() && floatConstantCount > maxFloatConstantsPS)) {
        Logger::err(str::format("CreateShaderModule: Invalid PS float constant index ", floatConstantCount));
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(!isSM2XOrNewer && intConstantCount != 0u)) {
        Logger::err(str::format("CreateShaderModule: Invalid use of PS int constant: ", intConstantCount));
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(isSM2XOrNewer && !CanSWVP() && intConstantCount > caps::MaxOtherConstants)) {
        Logger::err(str::format("CreateShaderModule: Invalid PS int constant index ", intConstantCount));
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(!isSM2XOrNewer && boolConstantCount != 0u)) {
        Logger::err("CreateShaderModule: Invalid use of PS bool constant");
        return D3DERR_INVALIDCALL;
      }

      if (unlikely(isSM2XOrNewer && !CanSWVP() && boolConstantCount > caps::MaxOtherConstants)) {
        Logger::err(str::format("CreateShaderModule: Invalid PS bool constant index ", boolConstantCount));
        return D3DERR_INVALIDCALL;
      }
    }

    std::array<Sha1Data, 2> hashChunks = {{
      { pShaderBytecode,  bytecodeLength },
      { &m_shaderOptions, sizeof(m_shaderOptions) },
    }};
    Sha1Hash sha = Sha1Hash::compute(hashChunks.size(), hashChunks.data());

    MtlShaderStage stage = ShaderType == D3D9ShaderType::PixelShader
      ? MtlShaderStage::Fragment : MtlShaderStage::Vertex;
    D3D9ShaderModuleKey key(stage, sha);

    D3D9CommonShader commonShader = { };

    D3D9ShaderCreateInfo moduleInfo;
    moduleInfo.shaderOptions                  = m_shaderOptions;

    HRESULT hr = m_shaderModules->GetShaderModule(this,
      key, std::move(analysis), moduleInfo, pShaderBytecode, &commonShader);

    if (FAILED(hr))
      return hr;

    *pShaderModule   = std::move(commonShader);
    *pBytecodeLength = bytecodeLength;
    return D3D_OK;
  }


  template<D3D9ShaderType ShaderType, D3D9ConstantType ConstantType, typename T>
  HRESULT D3D9DeviceEx::SetShaderConstants(
          UINT  StartRegister,
    const T*    pConstantData,
          UINT  Count) {
    uint32_t maxRegCount = DetermineSoftwareRegCount<ShaderType, ConstantType>();

    if (unlikely(StartRegister > maxRegCount) || unlikely(Count > maxRegCount - StartRegister))
      return D3DERR_INVALIDCALL;

    if (unlikely(!Count))
      return D3D_OK;

    if (ShaderType == D3D9ShaderType::VertexShader && likely(!CanSWVP())) {
      maxRegCount = DetermineHardwareRegCount<ShaderType, ConstantType>();

      if (unlikely(StartRegister >= maxRegCount))
        return D3D_OK;

      Count = std::min(maxRegCount - StartRegister, Count);
    }

    if (unlikely(!pConstantData))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord())) {
      return m_recorder->SetShaderConstants<ShaderType, ConstantType, T>(
        StartRegister, pConstantData, Count);
    }

    bool anyConstantDirty = UpdateStateConstants<ShaderType, ConstantType, T>(
      &m_state, StartRegister, pConstantData, Count);

    if (unlikely(!anyConstantDirty))
      return D3D_OK;

    D3D9ConstantSets& constSet = m_consts[uint32_t(ShaderType)];

    if (ConstantType == D3D9ConstantType::Float) {
      constSet.dirty |= StartRegister < constSet.shaderConstantsInfo.floatCount;

      if (ShaderType == D3D9ShaderType::VertexShader)
        constSet.changedFloatCount = std::max(constSet.changedFloatCount, StartRegister + Count);
    } else if (ConstantType == D3D9ConstantType::Int) {
      constSet.dirty |= StartRegister < constSet.shaderConstantsInfo.intCount;
    } else if (ConstantType == D3D9ConstantType::Bool && ShaderType == D3D9ShaderType::VertexShader) {
      constSet.dirty |= StartRegister < constSet.shaderConstantsInfo.boolCount && CanSWVP();
    }

    return D3D_OK;
  }


  void D3D9DeviceEx::UpdateFixedFunctionVS() {
    bool hasPositionT    = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
    bool hasBlendWeight  = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasBlendWeight);
    bool hasBlendIndices = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasBlendIndices);

    bool indexedVertexBlend = hasBlendIndices && m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE];
    D3D9FF_VertexBlendMode vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;

    if (m_state.renderStates[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE && !hasPositionT) {
      vertexBlendMode = m_state.renderStates[D3DRS_VERTEXBLEND] == D3DVBF_TWEENING
        ? D3D9FF_VertexBlendMode_Tween
        : D3D9FF_VertexBlendMode_Normal;

      if (m_state.renderStates[D3DRS_VERTEXBLEND] != D3DVBF_0WEIGHTS) {
        if (!hasBlendWeight)
          vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;
      }
      else if (!indexedVertexBlend)
        vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;
    }

    if (hasPositionT && (m_dirty.test(D3D9DeviceDirtyFlag::FFViewport) || m_ffZTest != IsZTestEnabled())) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FFViewport);
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

      const auto& vp = m_state.viewport;

      m_ffZTest = IsZTestEnabled();

      m_viewportInfo.inverseExtent = Vector4(
         2.0f / float(vp.Width),
        -2.0f / float(vp.Height),
        m_ffZTest ? 1.0f : 0.0f,
        1.0f);

      m_viewportInfo.inverseOffset = Vector4(
        -float(vp.X), -float(vp.Y),
         0.0f,         0.0f);

      m_viewportInfo.inverseOffset = m_viewportInfo.inverseOffset * m_viewportInfo.inverseExtent;

      m_viewportInfo.inverseOffset = m_viewportInfo.inverseOffset + Vector4(-1.0f, 1.0f, 0.0f, 0.0f);
    }

    if (m_dirty.test(D3D9DeviceDirtyFlag::FFVertexData)) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexData);

      auto WorldView    = m_state.transforms[GetTransformIndex(D3DTS_VIEW)] * m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
      auto NormalMatrix = tryInverse(WorldView).value_or(Matrix4(1.0f));
      auto Projection   = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];

      auto data = GetConstantBuffer(CbvIndex::VSFixedFunction).AllocTyped<D3D9FixedFunctionVS>(1u);
      data->WorldView    = WorldView;
      data->NormalMatrix = NormalMatrix;
      data->InverseView  = transpose(inverse(m_state.transforms[GetTransformIndex(D3DTS_VIEW)]));
      data->Projection   = Projection;
      data->WorldViewProj = Projection * WorldView;

      for (uint32_t i = 0; i < data->TexcoordMatrices.size(); i++)
        data->TexcoordMatrices[i] = m_state.transforms[GetTransformIndex(D3DTS_TEXTURE0) + i];

      data->ViewportInfo = m_viewportInfo;

      uint32_t lightIdx = 0;

      for (auto& light : m_state.lights) {
        if (!light.isEnabled)
          continue;

        if (unlikely(!light.light.Type || light.light.Type > D3DLIGHT_DIRECTIONAL))
          continue;

        data->Lights[lightIdx++] = D3D9Light(light.light, m_state.transforms[GetTransformIndex(D3DTS_VIEW)]);

        if (lightIdx == caps::MaxEnabledLights)
          break;
      }

      data->Material = m_state.material;
      data->GlobalAmbient = m_state.renderStates[D3DRS_AMBIENT];
      data->TweenFactor = bit::cast<float>(m_state.renderStates[D3DRS_TWEENFACTOR]);

      bool vertexHasPositionT = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
      data->VertexHasPositionT = vertexHasPositionT;
      data->VertexHasColor0    = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0);
      data->VertexHasColor1    = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1);
      data->VertexHasPointSize = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPointSize);
      data->VertexHasFog       = m_state.vertexDecl != nullptr && m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasFog);

      bool lighting    = m_state.renderStates[D3DRS_LIGHTING] != 0 && !vertexHasPositionT;
      bool colorVertex = m_state.renderStates[D3DRS_COLORVERTEX] != 0;
      uint32_t mask    = (lighting && colorVertex)
                       ? (data->VertexHasColor0 ? D3DMCS_COLOR1 : D3DMCS_MATERIAL)
                       | (data->VertexHasColor1 ? D3DMCS_COLOR2 : D3DMCS_MATERIAL)
                       : 0;

      data->UseLighting      = lighting;
      data->NormalizeNormals = m_state.renderStates[D3DRS_NORMALIZENORMALS];
      data->LocalViewer      = m_state.renderStates[D3DRS_LOCALVIEWER] && lighting;

      data->RangeFog         = m_state.renderStates[D3DRS_RANGEFOGENABLE];

      data->DiffuseSource    = m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE]  & mask;
      data->AmbientSource    = m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE]  & mask;
      data->SpecularSource   = m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] & mask;
      data->EmissiveSource   = m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] & mask;

      uint32_t lightCount = lightIdx;
      data->LightCount       = lighting ? lightCount : 0;

      for (uint32_t i = 0; i < caps::MaxTextureBlendStages; i++) {
        uint32_t transformFlags = m_state.textureStages[i][DXVK_TSS_TEXTURETRANSFORMFLAGS] & ~(D3DTTFF_PROJECTED);
        uint32_t index          = m_state.textureStages[i][DXVK_TSS_TEXCOORDINDEX];
        uint32_t indexFlags     = (index & TCIMask) >> TCIOffset;

        transformFlags &= 0b111;
        index          &= 0b111;

        data->TexcoordTransformFlags[i]  = transformFlags;
        data->TexcoordFlags[i]   = indexFlags;
        data->TexcoordIndices[i] = index;
      }

      data->VertexTexcoordDeclMask = m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetTexcoordMask() : 0;

      data->VertexBlendMode = uint8_t(vertexBlendMode);

      if (vertexBlendMode == D3D9FF_VertexBlendMode_Normal) {
        data->VertexBlendIndexed = indexedVertexBlend;
        data->VertexBlendCount   = m_state.renderStates[D3DRS_VERTEXBLEND] & 0xff;
      }

      data->VertexClipping = m_state.renderStates[D3DRS_CLIPPLANEENABLE] != 0;
    }

    if (m_dirty.test(D3D9DeviceDirtyFlag::FFVertexBlend) && vertexBlendMode == D3D9FF_VertexBlendMode_Normal) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexBlend);

      uint32_t matrixCount = m_isSWVP && indexedVertexBlend
        ? D3D9MaxVertexBlendTransformsSw
        : D3D9MaxVertexBlendTransformsHw;

      auto data = GetConstantBuffer(CbvIndex::VSVertexBlendData).AllocTyped<Matrix4>(matrixCount);

      for (uint32_t i = 0; i < matrixCount; i++) {
        data[i] = m_state.transforms[GetTransformIndex(D3DTS_VIEW)] *
          m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(i))];
      }
    }
  }


  uint32_t D3D9DeviceEx::GetTextureStageArgMask(
          D3DTEXTUREOP          Op) {
    switch (Op) {
      case D3DTOP_DISABLE:
      case D3DTOP_BUMPENVMAP:
      case D3DTOP_BUMPENVMAPLUMINANCE:
        return 0b000u;

      case D3DTOP_SELECTARG1:
      case D3DTOP_PREMODULATE:
        return 0b010u;

      case D3DTOP_SELECTARG2:
        return 0b100u;

      case D3DTOP_MULTIPLYADD:
      case D3DTOP_LERP:
        return 0b111u;

      default:
        return 0b110u;
    }
  }


  D3D9TextureStageStateFlags D3D9DeviceEx::GetTextureStageStateFlags(
          D3DTEXTUREOP          Op,
          UINT                  Arg0,
          UINT                  Arg1,
          UINT                  Arg2,
          bool                  Premodulate) {
    uint32_t usedArgMask = GetTextureStageArgMask(Op);

    D3D9TextureStageStateFlags result = {};

    if (Op == D3DTOP_BLENDTEXTUREALPHA
     || Op == D3DTOP_BLENDTEXTUREALPHAPM
     || Op == D3DTOP_BUMPENVMAP
     || Op == D3DTOP_BUMPENVMAPLUMINANCE)
      result.set(D3D9TextureStageStateFlag::UsesTexture);

    if (Op == D3DTOP_BLENDCURRENTALPHA)
      result.set(D3D9TextureStageStateFlag::UsesCurrent);

    std::array<UINT, 3u> args = { Arg0, Arg1, Arg2 };

    for (uint32_t i = 0u; i < args.size(); i++) {
      if (usedArgMask & (1u << i)) {
        auto arg = args[i] & D3DTA_SELECTMASK;

        if (arg == D3DTA_TEXTURE)
          result.set(D3D9TextureStageStateFlag::UsesTexture);
        else if (arg == D3DTA_CURRENT)
          result.set(D3D9TextureStageStateFlag::UsesCurrent);
        else if (arg == D3DTA_TEMP)
          result.set(D3D9TextureStageStateFlag::UsesTemp);
      }
    }

    if (Premodulate && result.test(D3D9TextureStageStateFlag::UsesCurrent))
      result.set(D3D9TextureStageStateFlag::UsesTexture);

    return result;
  }


  void D3D9DeviceEx::UpdateFixedFunctionPS() {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFPixelShader);

    bool dirty = false;

    uint32_t activeTextureStageCount = 0u;
    uint32_t preserveColorStageMask = 0u;

    for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
      auto& data = m_state.textureStages[i];

      auto colorOp = D3DTEXTUREOP(data[DXVK_TSS_COLOROP]);

      if (colorOp == D3DTOP_DISABLE)
        break;

      if (!m_state.textures[i]) {
        uint32_t colorArgMask = GetTextureStageArgMask(colorOp);

        DWORD colorArg0 = ((colorArgMask & 0b001u) ? data[DXVK_TSS_COLORARG0] : D3DTA_CURRENT) & D3DTA_SELECTMASK;
        DWORD colorArg1 = ((colorArgMask & 0b010u) ? data[DXVK_TSS_COLORARG1] : D3DTA_CURRENT) & D3DTA_SELECTMASK;
        DWORD colorArg2 = ((colorArgMask & 0b100u) ? data[DXVK_TSS_COLORARG2] : D3DTA_CURRENT) & D3DTA_SELECTMASK;

        if (colorArg0 == D3DTA_TEXTURE || colorArg1 == D3DTA_TEXTURE || colorArg2 == D3DTA_TEXTURE)
          break;
      }

      if (colorOp == D3DTOP_BUMPENVMAP || colorOp == D3DTOP_BUMPENVMAPLUMINANCE)
        preserveColorStageMask |= 1u << i;

      activeTextureStageCount += 1u;
    }

    bool hasColorRt = m_state.renderTargets[0u] && !m_state.renderTargets[0u]->IsNull();

    uint32_t currTextures = 0u;
    uint32_t tempTextures = 0u;
    uint32_t bumpTextures = 0u;

    bool premodulateColor = false;
    bool premodulateAlpha = false;

    for (uint32_t i = 0u; i < activeTextureStageCount; i++) {
      auto& data = m_state.textureStages[i];

      DWORD resultArg = data[DXVK_TSS_RESULTARG];
      bool resultIsTemp = resultArg == D3DTA_TEMP;

      D3DTEXTUREOP colorOp = D3DTEXTUREOP(data[DXVK_TSS_COLOROP]);
      D3DTEXTUREOP alphaOp = D3DTEXTUREOP(data[DXVK_TSS_ALPHAOP]);

      uint32_t colorArgMask = GetTextureStageArgMask(colorOp);
      uint32_t alphaArgMask = GetTextureStageArgMask(alphaOp);

      DWORD colorArg0 = (colorArgMask & 0b001u) ? data[DXVK_TSS_COLORARG0] : 0u;
      DWORD colorArg1 = (colorArgMask & 0b010u) ? data[DXVK_TSS_COLORARG1] : 0u;
      DWORD colorArg2 = (colorArgMask & 0b100u) ? data[DXVK_TSS_COLORARG2] : 0u;

      DWORD alphaArg0 = (alphaArgMask & 0b001u) ? data[DXVK_TSS_ALPHAARG0] : 0u;
      DWORD alphaArg1 = (alphaArgMask & 0b010u) ? data[DXVK_TSS_ALPHAARG1] : 0u;
      DWORD alphaArg2 = (alphaArgMask & 0b100u) ? data[DXVK_TSS_ALPHAARG2] : 0u;

      if (!i && resultIsTemp && colorOp != D3DTOP_DISABLE && alphaOp == D3DTOP_DISABLE) {
        alphaOp   = D3DTOP_SELECTARG1;
        alphaArg0 = 0u;
        alphaArg1 = D3DTA_DIFFUSE;
        alphaArg2 = 0u;
      }

      if (!hasColorRt && !(preserveColorStageMask & (1u << i))) {
        colorOp = D3DTOP_SELECTARG1;
        colorArg0 = 0u;
        colorArg1 = D3DTA_TFACTOR;
        colorArg2 = 0u;
      }

      if (i + 1u == activeTextureStageCount)
        resultArg = D3DTA_CURRENT;

      dirty |= m_specData.setTextureStage(i, colorOp, alphaOp, resultArg,
        colorArg0, colorArg1, colorArg2, alphaArg0, alphaArg1, alphaArg2);

      uint32_t stageTextures = std::exchange(bumpTextures, 0u);

      D3D9TextureStageStateFlags flags = {};
      flags.set(GetTextureStageStateFlags(colorOp, colorArg0, colorArg1, colorArg2, premodulateColor));
      flags.set(GetTextureStageStateFlags(alphaOp, alphaArg0, alphaArg1, alphaArg2, premodulateAlpha));

      if (flags.test(D3D9TextureStageStateFlag::UsesTexture))
        stageTextures |= 1u << i;
      if (flags.test(D3D9TextureStageStateFlag::UsesCurrent))
        stageTextures |= currTextures;
      if (flags.test(D3D9TextureStageStateFlag::UsesTemp))
        stageTextures |= tempTextures;

      auto& dstMask = resultIsTemp ? tempTextures : currTextures;

      if (colorOp == D3DTOP_BUMPENVMAP || colorOp == D3DTOP_BUMPENVMAPLUMINANCE) {
        bumpTextures = stageTextures;
        stageTextures |= dstMask;
      }

      dstMask = stageTextures;

      premodulateColor = colorOp == D3DTOP_PREMODULATE;
      premodulateAlpha = alphaOp == D3DTOP_PREMODULATE;
    }

    for (uint32_t i = activeTextureStageCount; i < caps::TextureStageCount; i++)
      dirty |= m_specData.disableTextureStage(i);

    if (dirty)
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);

    auto prevTextures = m_textureSlotTracking.ffpsTextures;

    if (prevTextures != currTextures) {
      m_textureSlotTracking.ffpsTextures = currTextures;

      UpdateActiveHazardsRT(prevTextures | currTextures);
      UpdateActiveHazardsDS(prevTextures | currTextures);
    }
  }


  bool D3D9DeviceEx::UseProgrammableVS() {
    return m_state.vertexShader != nullptr
      && m_state.vertexDecl != nullptr
      && !m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  }


  bool D3D9DeviceEx::UseProgrammablePS() {
    return m_state.pixelShader != nullptr;
  }


  void D3D9DeviceEx::ApplyPrimitiveType(D3DPRIMITIVETYPE PrimType) {
    if (unlikely(PrimType == D3DPT_FORCE_DWORD))
      PrimType = D3DPT_POINTLIST;

    if (m_iaState.primitiveType != PrimType) {
      m_iaState.primitiveType = PrimType;

      EmitCs([
        cPrimType = PrimType
      ] (MtlContext* ctx) {
        auto iaState = DecodeInputAssemblyState(cPrimType);
        ctx->setPrimitiveTopology(iaState.topology);
      });
    }
  }


  void D3D9DeviceEx::ResolveZ() {
    D3D9Surface*           src = m_state.depthStencil.ptr();
    IDirect3DBaseTexture9* dst = m_state.textures[0];

    if (unlikely(!src || !dst))
      return;

    D3D9CommonTexture* srcTextureInfo = GetCommonTexture(src);
    D3D9CommonTexture* dstTextureInfo = GetCommonTexture(dst);

    const D3D9_COMMON_TEXTURE_DESC* srcDesc = srcTextureInfo->Desc();
    const D3D9_COMMON_TEXTURE_DESC* dstDesc = dstTextureInfo->Desc();

    uint32_t dstSampleCount;
    DecodeMultiSampleType(dstDesc->MultiSample, dstDesc->MultisampleQuality, &dstSampleCount);

    if (unlikely(dstSampleCount != 1u)) {
      Logger::warn("D3D9DeviceEx::ResolveZ: dstSampleCount != 1. Discarding.");
      return;
    }

    const D3D9_MTL_FORMAT_MAPPING srcFormatInfo = LookupFormat(srcDesc->Format);
    const D3D9_MTL_FORMAT_MAPPING dstFormatInfo = LookupFormat(dstDesc->Format);

    MtlSubresourceLayers dstSubresource =
      dstTextureInfo->GetSubresourceFromIndex(
        dstFormatInfo.Aspect, 0);

    MtlSubresourceLayers srcSubresource =
      srcTextureInfo->GetSubresourceFromIndex(
        srcFormatInfo.Aspect, src->GetSubresource());

    if ((dstSubresource.aspects & srcSubresource.aspects) != 0) {
      dstSubresource.aspects = dstSubresource.aspects & srcSubresource.aspects;
      srcSubresource.aspects = dstSubresource.aspects & srcSubresource.aspects;
    } else if (unlikely(dstSubresource.aspects != MtlAspectColor && srcSubresource.aspects != MtlAspectColor)) {
      Logger::err(str::format("D3D9DeviceEx::ResolveZ: Trying to blit from ",
        uint32_t(srcFormatInfo.FormatColor), " (aspect ", srcSubresource.aspects, ")", " to ",
        uint32_t(dstFormatInfo.FormatColor), " (aspect ", dstSubresource.aspects, ")"
      ));
      return;
    }

    const MtlSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspects,
      dstSubresource.mipLevel,
      dstSubresource.baseLayer, 1 };

    const MtlSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspects,
      srcSubresource.mipLevel,
      srcSubresource.baseLayer, 1 };

    uint32_t srcSampleCount;
    DecodeMultiSampleType(srcDesc->MultiSample, srcDesc->MultisampleQuality, &srcSampleCount);

    if (srcSampleCount == 1u) {
      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstLayers = dstSubresourceLayers,
        cSrcLayers = srcSubresourceLayers
      ] (MtlContext* ctx) {
        ctx->copyTexture(
          cDstImage, cDstLayers, MtlOrigin { 0, 0, 0 },
          cSrcImage, cSrcLayers, MtlOrigin { 0, 0, 0 },
          cDstImage->mipLevelExtent(cDstLayers.mipLevel));
      });
    } else {
      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstSubres = dstSubresourceLayers,
        cSrcSubres = srcSubresourceLayers
      ] (MtlContext* ctx) {
        MtlResolveRegion region;
        region.srcSubresource = cSrcSubres;
        region.srcOffset      = MtlOrigin { 0, 0, 0 };
        region.dstSubresource = cDstSubres;
        region.dstOffset      = MtlOrigin { 0, 0, 0 };
        region.extent         = cDstImage->mipLevelExtent(cDstSubres.mipLevel);

        ctx->resolveTexture(cDstImage, cSrcImage, region, cSrcImage->info().format,
          MtlResolveMode::SampleZero, MtlResolveMode::SampleZero);
      });
    }

    dstTextureInfo->MarkAllNeedReadback();
  }


  void D3D9DeviceEx::TransformImage(
          D3D9CommonTexture*       pResource,
    const MtlSubresourceRange*     pSubresources) {
    EmitCs([
      cImage        = pResource->GetImage(),
      cSubresources = *pSubresources
    ] (MtlContext* ctx) {
      ctx->transformImage(cImage, cSubresources, MtlScopeTextures);
    });
  }


  void D3D9DeviceEx::ResetState(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    SetDepthStencilSurface(nullptr);

    for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++)
      SetRenderTargetInternal(i, nullptr);

    auto& rs = m_state.renderStates;

    rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
    rs[D3DRS_ALPHABLENDENABLE]         = FALSE;
    rs[D3DRS_BLENDOP]                  = D3DBLENDOP_ADD;
    rs[D3DRS_BLENDOPALPHA]             = D3DBLENDOP_ADD;
    rs[D3DRS_DESTBLEND]                = D3DBLEND_ZERO;
    rs[D3DRS_DESTBLENDALPHA]           = D3DBLEND_ZERO;
    rs[D3DRS_COLORWRITEENABLE]         = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE1]        = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE2]        = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE3]        = 0x0000000f;
    rs[D3DRS_SRCBLEND]                 = D3DBLEND_ONE;
    rs[D3DRS_SRCBLENDALPHA]            = D3DBLEND_ONE;
    BindBlendState();

    rs[D3DRS_BLENDFACTOR]              = 0xffffffff;
    BindBlendFactor();

    rs[D3DRS_ZENABLE]                  = pPresentationParameters->EnableAutoDepthStencil
                                       ? D3DZB_TRUE
                                       : D3DZB_FALSE;
    rs[D3DRS_ZFUNC]                    = D3DCMP_LESSEQUAL;
    rs[D3DRS_TWOSIDEDSTENCILMODE]      = FALSE;
    rs[D3DRS_ZWRITEENABLE]             = TRUE;
    rs[D3DRS_STENCILENABLE]            = FALSE;
    rs[D3DRS_STENCILFAIL]              = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL]             = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILPASS]              = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC]              = D3DCMP_ALWAYS;
    rs[D3DRS_CCW_STENCILFAIL]          = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILZFAIL]         = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILPASS]          = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC]          = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK]              = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK]         = 0xFFFFFFFF;
    BindDepthStencilState();

    rs[D3DRS_STENCILREF] = 0;
    BindDepthStencilReference();

    rs[D3DRS_FILLMODE]            = D3DFILL_SOLID;
    rs[D3DRS_CULLMODE]            = D3DCULL_CCW;
    rs[D3DRS_DEPTHBIAS]           = bit::cast<DWORD>(0.0f);
    rs[D3DRS_SLOPESCALEDEPTHBIAS] = bit::cast<DWORD>(0.0f);
    BindRasterizerState();
    BindDepthBias();

    rs[D3DRS_SCISSORTESTENABLE]   = FALSE;

    rs[D3DRS_ALPHATESTENABLE]     = FALSE;
    rs[D3DRS_ALPHAFUNC]           = D3DCMP_ALWAYS;
    BindAlphaTestState();
    rs[D3DRS_ALPHAREF]            = 0;
    m_pushData.shared.alphaRef    = rs[D3DRS_ALPHAREF];

    rs[D3DRS_MULTISAMPLEMASK]     = 0xffffffff;
    BindMultiSampleState();

    rs[D3DRS_TEXTUREFACTOR]       = 0xffffffff;
    m_pushData.ffps.textureFactor = rs[D3DRS_TEXTUREFACTOR];

    rs[D3DRS_DIFFUSEMATERIALSOURCE]  = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_AMBIENTMATERIALSOURCE]  = D3DMCS_MATERIAL;
    rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    rs[D3DRS_LIGHTING]               = TRUE;
    rs[D3DRS_COLORVERTEX]            = TRUE;
    rs[D3DRS_LOCALVIEWER]            = TRUE;
    rs[D3DRS_RANGEFOGENABLE]         = FALSE;
    rs[D3DRS_NORMALIZENORMALS]       = FALSE;
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    rs[D3DRS_SPECULARENABLE] = FALSE;

    rs[D3DRS_AMBIENT]                = 0;
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    rs[D3DRS_FOGENABLE]                  = FALSE;
    rs[D3DRS_FOGCOLOR]                   = 0;
    rs[D3DRS_FOGTABLEMODE]               = D3DFOG_NONE;
    rs[D3DRS_FOGSTART]                   = bit::cast<DWORD>(0.0f);
    rs[D3DRS_FOGEND]                     = bit::cast<DWORD>(1.0f);
    rs[D3DRS_FOGDENSITY]                 = bit::cast<DWORD>(1.0f);
    rs[D3DRS_FOGVERTEXMODE]              = D3DFOG_NONE;
    m_dirty.set(D3D9DeviceDirtyFlag::Fog);

    rs[D3DRS_CLIPPLANEENABLE] = 0;
    m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);

    const float maxPointSize = m_dxvkDevice->deviceInfo().maxPointSize;

    rs[D3DRS_POINTSPRITEENABLE]          = FALSE;
    rs[D3DRS_POINTSCALEENABLE]           = FALSE;
    rs[D3DRS_POINTSCALE_A]               = bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSCALE_B]               = bit::cast<DWORD>(0.0f);
    rs[D3DRS_POINTSCALE_C]               = bit::cast<DWORD>(0.0f);
    rs[D3DRS_POINTSIZE]                  = bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSIZE_MIN]              = m_isD3D8Compatible ? bit::cast<DWORD>(0.0f) : bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSIZE_MAX]              = bit::cast<DWORD>(maxPointSize);
    m_pushData.vs.pointSize = EncodePointSize(rs[D3DRS_POINTSIZE]);
    m_pushData.vs.pointSizeMin = EncodePointSize(rs[D3DRS_POINTSIZE_MIN]);
    m_pushData.vs.pointSizeMax = EncodePointSize(rs[D3DRS_POINTSIZE_MAX]);
    m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
    UpdatePointMode(false);

    rs[D3DRS_SRGBWRITEENABLE]            = 0;

    rs[D3DRS_SHADEMODE]                  = D3DSHADE_GOURAUD;

    rs[D3DRS_VERTEXBLEND]                = D3DVBF_DISABLE;
    rs[D3DRS_INDEXEDVERTEXBLENDENABLE]   = FALSE;
    rs[D3DRS_TWEENFACTOR]                = bit::cast<DWORD>(0.0f);
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

    rs[D3DRS_LASTPIXEL]                  = TRUE;
    rs[D3DRS_DITHERENABLE]               = FALSE;
    rs[D3DRS_WRAP0]                      = 0;
    rs[D3DRS_WRAP1]                      = 0;
    rs[D3DRS_WRAP2]                      = 0;
    rs[D3DRS_WRAP3]                      = 0;
    rs[D3DRS_WRAP4]                      = 0;
    rs[D3DRS_WRAP5]                      = 0;
    rs[D3DRS_WRAP6]                      = 0;
    rs[D3DRS_WRAP7]                      = 0;
    rs[D3DRS_CLIPPING]                   = TRUE;
    rs[D3DRS_MULTISAMPLEANTIALIAS]       = TRUE;
    rs[D3DRS_PATCHEDGESTYLE]             = D3DPATCHEDGE_DISCRETE;
    rs[D3DRS_DEBUGMONITORTOKEN]          = D3DDMT_ENABLE;
    rs[D3DRS_POSITIONDEGREE]             = D3DDEGREE_CUBIC;
    rs[D3DRS_NORMALDEGREE]               = D3DDEGREE_LINEAR;
    rs[D3DRS_ANTIALIASEDLINEENABLE]      = FALSE;
    rs[D3DRS_MINTESSELLATIONLEVEL]       = bit::cast<DWORD>(1.0f);
    rs[D3DRS_MAXTESSELLATIONLEVEL]       = bit::cast<DWORD>(1.0f);
    rs[D3DRS_ADAPTIVETESS_X]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ADAPTIVETESS_Y]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ADAPTIVETESS_Z]             = bit::cast<DWORD>(1.0f);
    rs[D3DRS_ADAPTIVETESS_W]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
    rs[D3DRS_WRAP8]                      = 0;
    rs[D3DRS_WRAP9]                      = 0;
    rs[D3DRS_WRAP10]                     = 0;
    rs[D3DRS_WRAP11]                     = 0;
    rs[D3DRS_WRAP12]                     = 0;
    rs[D3DRS_WRAP13]                     = 0;
    rs[D3DRS_WRAP14]                     = 0;
    rs[D3DRS_WRAP15]                     = 0;

    for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
      auto& stage = m_state.textureStages[i];

      stage[DXVK_TSS_COLOROP]               = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
      stage[DXVK_TSS_COLORARG1]             = D3DTA_TEXTURE;
      stage[DXVK_TSS_COLORARG2]             = D3DTA_CURRENT;
      stage[DXVK_TSS_ALPHAOP]               = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
      stage[DXVK_TSS_ALPHAARG1]             = D3DTA_TEXTURE;
      stage[DXVK_TSS_ALPHAARG2]             = D3DTA_CURRENT;
      stage[DXVK_TSS_BUMPENVMAT00]          = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_BUMPENVMAT01]          = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_BUMPENVMAT10]          = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_BUMPENVMAT11]          = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_TEXCOORDINDEX]         = i;
      stage[DXVK_TSS_BUMPENVLSCALE]         = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_BUMPENVLOFFSET]        = bit::cast<DWORD>(0.0f);
      stage[DXVK_TSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
      stage[DXVK_TSS_COLORARG0]             = D3DTA_CURRENT;
      stage[DXVK_TSS_ALPHAARG0]             = D3DTA_CURRENT;
      stage[DXVK_TSS_RESULTARG]             = D3DTA_CURRENT;
      stage[DXVK_TSS_CONSTANT]              = 0x00000000;
    }

    m_textureSlotTracking.projected = 0;

    m_dirty.set(D3D9DeviceDirtyFlag::SharedPixelShaderData);
    m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);

    for (uint32_t i = 0; i < caps::MaxStreams; i++)
      m_state.streamFreq[i] = 1;

    for (uint32_t i = 0; i < m_state.textures->size(); i++) {
      SetStateTexture(i, nullptr);
    }

    EmitCs([
      cSize = m_state.textures->size()
    ] (MtlContext* ctx) {
      for (uint32_t i = 0; i < cSize; i++) {
        auto [stage, slot] = D3D9ShaderResourceMapping::getTextureSlotInfo(i);
        ctx->bindTexture(stage, slot, nullptr);
      }
    });

    m_textureSlotTracking.textureDirty = 0;
    m_textureSlotTracking.depth = 0;

    auto& ss = m_state.samplerStates.get();
    for (uint32_t i = 0; i < ss.size(); i++) {
      auto& state = ss[i];
      state[D3DSAMP_ADDRESSU]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_ADDRESSV]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_ADDRESSW]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_BORDERCOLOR]   = 0x00000000;
      state[D3DSAMP_MAGFILTER]     = D3DTEXF_POINT;
      state[D3DSAMP_MINFILTER]     = D3DTEXF_POINT;
      state[D3DSAMP_MIPFILTER]     = D3DTEXF_NONE;
      state[D3DSAMP_MIPMAPLODBIAS] = bit::cast<DWORD>(0.0f);
      state[D3DSAMP_MAXMIPLEVEL]   = 0;
      state[D3DSAMP_MAXANISOTROPY] = 1;
      state[D3DSAMP_SRGBTEXTURE]   = 0;
      state[D3DSAMP_ELEMENTINDEX]  = 0;
      state[D3DSAMP_DMAPOFFSET]    = 0;

      BindSampler(i);
    }

    m_textureSlotTracking.samplerStateDirty = 0;

    for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
      float plane[4] = { 0, 0, 0, 0 };
      SetClipPlane(i, plane);
    }

    m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

    UpdateAnyColorWrites<0>();
    UpdateAnyColorWrites<1>();
    UpdateAnyColorWrites<2>();
    UpdateAnyColorWrites<3>();

    SetIndices(nullptr);
    for (uint32_t i = 0; i < caps::MaxStreams; i++) {
      SetStreamSource(i, nullptr, 0, 0);
    }

    if (m_isD3D8Compatible)
      m_state.nPatchSegments = 1.0f;

    m_alphaTestEnabled = false;
    m_atocEnabled      = false;
    m_nvdbEnabled      = false;

    m_dirty.set(D3D9DeviceDirtyFlag::PushDataShared,
                D3D9DeviceDirtyFlag::PushDataVs,
                D3D9DeviceDirtyFlag::PushDataFfvs,
                D3D9DeviceDirtyFlag::PushDataFfps);
  }


  HRESULT D3D9DeviceEx::ResetSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    D3D9Format backBufferFmt = EnumerateFormat(pPresentationParameters->BackBufferFormat);
    bool unlockedFormats = m_parent->HasFormatsUnlocked();

    Logger::info(str::format(
      "D3D9DeviceEx::ResetSwapChain:\n",
      "  Requested Presentation Parameters\n",
      "    - Width:              ", pPresentationParameters->BackBufferWidth, "\n",
      "    - Height:             ", pPresentationParameters->BackBufferHeight, "\n",
      "    - Format:             ", backBufferFmt, "\n"
      "    - Auto Depth Stencil: ", pPresentationParameters->EnableAutoDepthStencil ? "true" : "false", "\n",
      "                ^ Format: ", EnumerateFormat(pPresentationParameters->AutoDepthStencilFormat), "\n",
      "    - Windowed:           ", pPresentationParameters->Windowed ? "true" : "false", "\n",
      "    - Swap effect:        ", pPresentationParameters->SwapEffect, "\n"));

    if (likely(m_deviceType != D3DDEVTYPE_NULLREF) &&
        unlikely(!pPresentationParameters->Windowed &&
                 (pPresentationParameters->BackBufferWidth  == 0
               || pPresentationParameters->BackBufferHeight == 0))) {
      return D3DERR_INVALIDCALL;
    }

    if (backBufferFmt != D3D9Format::Unknown && !unlockedFormats) {
      if (!IsSupportedBackBufferFormat(backBufferFmt)) {
        Logger::err(str::format("D3D9DeviceEx::ResetSwapChain: Unsupported backbuffer format: ",
          EnumerateFormat(pPresentationParameters->BackBufferFormat)));
        return D3DERR_INVALIDCALL;
      }
    }

    if (m_implicitSwapchain != nullptr) {
      HRESULT hr = m_implicitSwapchain->Reset(pPresentationParameters, pFullscreenDisplayMode);
      if (FAILED(hr))
        return hr;
    }
    else {
      m_implicitSwapchain = new D3D9SwapChainEx(this, pPresentationParameters, pFullscreenDisplayMode, true);
      m_mostRecentlyUsedSwapchain = m_implicitSwapchain.ptr();
    }

    if (pPresentationParameters->EnableAutoDepthStencil) {
      D3D9_COMMON_TEXTURE_DESC desc;
      desc.Width              = pPresentationParameters->BackBufferWidth;
      desc.Height             = pPresentationParameters->BackBufferHeight;
      desc.Depth              = 1;
      desc.ArraySize          = 1;
      desc.MipLevels          = 1;
      desc.Usage              = D3DUSAGE_DEPTHSTENCIL;
      desc.Format             = EnumerateFormat(pPresentationParameters->AutoDepthStencilFormat);
      desc.Pool               = D3DPOOL_DEFAULT;
      desc.Discard            = (pPresentationParameters->Flags & D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL) != 0;
      desc.MultiSample        = pPresentationParameters->MultiSampleType;
      desc.MultisampleQuality = pPresentationParameters->MultiSampleQuality;
      desc.IsBackBuffer       = FALSE;
      desc.IsAttachmentOnly   = TRUE;
      desc.IsLockable         = IsLockableDepthStencilFormat(desc.Format);

      if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc)))
        return D3DERR_NOTAVAILABLE;

      m_autoDepthStencil = new D3D9Surface(this, &desc, IsExtended(), nullptr, nullptr);
      m_initializer->InitTexture(m_autoDepthStencil->GetCommonTexture());
      SetDepthStencilSurface(m_autoDepthStencil.ptr());
      m_losableResourceCounter++;
    }

    if (!IsExtended()) {
      SetRenderTarget(0, m_implicitSwapchain->GetBackBuffer(0));
    } else {
      const float MinZ = m_state.viewport.MinZ;
      const float MaxZ = m_state.viewport.MaxZ;

      SetRenderTarget(0, m_implicitSwapchain->GetBackBuffer(0));

      m_state.viewport.MinZ = MinZ;
      m_state.viewport.MaxZ = MaxZ;
    }

    BindViewportAndScissor();

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::InitialReset(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    ResetState(pPresentationParameters);

    HRESULT hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
    if (FAILED(hr))
      return hr;

    ExecuteFlush(false);
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    return D3D_OK;
  }


  void D3D9DeviceEx::TrackBufferMappingBufferSequenceNumber(
        D3D9CommonBuffer* pResource) {
    uint64_t sequenceNumber = GetCurrentSequenceNumber();
    pResource->TrackMappingBufferSequenceNumber(sequenceNumber);
  }


  void D3D9DeviceEx::TrackTextureMappingBufferSequenceNumber(
      D3D9CommonTexture* pResource,
      UINT Subresource) {
    uint64_t sequenceNumber = GetCurrentSequenceNumber();
    pResource->TrackMappingBufferSequenceNumber(Subresource, sequenceNumber);
  }


  uint64_t D3D9DeviceEx::GetCurrentSequenceNumber() {
    return m_csChunk->empty() ? m_csSeqNum : m_csSeqNum + 1;
  }


  void* D3D9DeviceEx::MapTexture(D3D9CommonTexture* pTexture, UINT Subresource) {
    void *ptr = pTexture->GetData(Subresource);

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    if (likely(pTexture->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)) {
      m_mappedTextures.insert(pTexture);
    }
#endif

    return ptr;
  }


  void D3D9DeviceEx::TouchMappedTexture(D3D9CommonTexture* pTexture) {
#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    if (pTexture->GetMapMode() != D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)
      return;

    D3D9DeviceLock lock = LockDevice();
    m_mappedTextures.touch(pTexture);
#endif
  }


  void D3D9DeviceEx::RemoveMappedTexture(D3D9CommonTexture* pTexture) {
#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    if (pTexture->GetMapMode() != D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)
      return;

    D3D9DeviceLock lock = LockDevice();
    m_mappedTextures.remove(pTexture);
#endif
  }


  void D3D9DeviceEx::UnmapTextures() {

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
    uint32_t mappedMemory = m_memoryAllocator.getStats().memoryMapped;

    if (likely(mappedMemory < uint32_t(m_d3d9Options.textureMemory)))
      return;

    uint32_t threshold = (m_d3d9Options.textureMemory / 4) * 3;

    auto iter = m_mappedTextures.leastRecentlyUsedIter();
    while (m_memoryAllocator.getStats().memoryMapped >= threshold && iter != m_mappedTextures.leastRecentlyUsedEndIter()) {
      if (unlikely((*iter)->IsAnySubresourceLocked() != 0)) {
        iter++;
        continue;
      }
      (*iter)->UnmapData();

      iter = m_mappedTextures.remove(iter);
    }
#endif
  }


  void D3D9DeviceEx::NotifyFullscreen(HWND window, bool fullscreen) {
    D3D9DeviceLock lock = LockDevice();

    if (fullscreen) {
      if (unlikely(window != m_fullscreenWindow && m_fullscreenWindow != NULL)) {
        Logger::warn("Multiple fullscreen windows detected.");
      }
      m_fullscreenWindow = window;
    } else {
      if (unlikely(m_fullscreenWindow != window)) {
        Logger::warn("Window was not fullscreen in the first place.");
      } else {
        m_fullscreenWindow = 0;
      }
    }
  }


  void D3D9DeviceEx::NotifyWindowActivated(HWND window, bool activated) {
    D3D9DeviceLock lock = LockDevice();

    if (likely(!m_d3d9Options.deviceLossOnFocusLoss || IsExtended()))
      return;

    if (activated && m_deviceLostState == D3D9DeviceLostState::Lost) {
      Logger::info("Device not reset");
      m_deviceLostState = D3D9DeviceLostState::NotReset;
    } else if (!activated && m_deviceLostState != D3D9DeviceLostState::Lost && m_fullscreenWindow == window) {
      Logger::info("Device lost");
      m_deviceLostState = D3D9DeviceLostState::Lost;
      m_fullscreenWindow = NULL;
    }
  }


  void D3D9DeviceEx::BindSpecConstants() {
    m_dirty.clr(D3D9DeviceDirtyFlag::SpecializationEntries);

    EmitCs([
      cSpecData = m_specData
    ] (MtlContext* ctx) {
      ctx->setFunctionConstants(MtlPipeline::Graphics,
        0u, sizeof(cSpecData) / sizeof(uint32_t), &cSpecData);
    });

    auto specCbv = GetConstantBuffer(CbvIndex::SpecData).AllocTyped<D3D9SpecData>(1u);
    *specCbv = m_specData;
  }


  GpuFlushType D3D9DeviceEx::GetMaxFlushType() const {
    if (m_d3d9Options.reproducibleCommandStream)
      return GpuFlushType::ExplicitFlush;
    else
      return GpuFlushType::ImplicitWeakHint;
  }


  bool D3D9DeviceEx::ValidateSharedTexture(
    HANDLE                          handle,
    D3DRESOURCETYPE                 type,
    const D3D9_COMMON_TEXTURE_DESC& textureDesc) const {
    if (!(reinterpret_cast<uintptr_t>(handle) & 0xc0000000)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: not a D3DKMT handle: ", handle));
      return false;
    }

    union d3dkmt_desc desc;

    D3DKMT_QUERYRESOURCEINFO query = { };
    query.hDevice = 0u ;
    query.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
    query.pPrivateRuntimeData = &desc;
    query.PrivateRuntimeDataSize = sizeof(desc);

    if (D3DKMTQueryResourceInfo(&query)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Failed to query resource: ", handle));
    } else if (query.PrivateRuntimeDataSize < sizeof(desc.dxgi) || query.PrivateRuntimeDataSize > sizeof(desc)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize));
    } else {
      D3DDDI_OPENALLOCATIONINFO2 alloc = { };
      D3DKMT_OPENRESOURCE open = { };
      open.hDevice = 0u ;
      open.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
      open.NumAllocations = 1;
      open.pOpenAllocationInfo2 = &alloc;
      open.pPrivateRuntimeData = &desc;
      open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

      if (D3DKMTOpenResource2(&open)) {
        Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Failed to open resource: ", handle));
      } else {
        D3DKMT_DESTROYALLOCATION destroy = { };
        destroy.hDevice = 0u ;
        destroy.hResource = open.hResource;
        D3DKMTDestroyAllocation(&destroy);

        if (desc.dxgi.size != sizeof(desc.d3d9) || desc.dxgi.version != 1) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid size: ",
                                   desc.dxgi.size, " or version: ", desc.dxgi.version));
          return false;
        }
        if (desc.d3d9.type != type) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid type: ", desc.d3d9.type));
          return false;
        }
        if (desc.d3d9.dxgi.width != textureDesc.Width || desc.d3d9.dxgi.height != textureDesc.Height) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid dimensions: ", desc.d3d9.dxgi.width, "x", desc.d3d9.dxgi.height));
          return false;
        }
        if (desc.d3d9.format != static_cast<D3DFORMAT>(textureDesc.Format)) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid format: ", desc.d3d9.format));
          return false;
        }
        if (textureDesc.Usage & ~desc.d3d9.usage) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid usage: ", desc.d3d9.usage));
          return false;
        }
        if (type == D3DRTYPE_TEXTURE && desc.d3d9.texture.levels != textureDesc.MipLevels) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid mip levels: ", desc.d3d9.texture.levels));
          return false;
        }

        Logger::debug(str::format("Found D3D9 desc: ", desc.d3d9.type));
        Logger::debug(str::format("  dxgi.width: ", desc.d3d9.dxgi.width));
        Logger::debug(str::format("  dxgi.height: ", desc.d3d9.dxgi.height));
        Logger::debug(str::format("  format: ", desc.d3d9.format));
        Logger::debug(str::format("  usage: ", desc.d3d9.usage));
        if (desc.d3d9.type == D3DRTYPE_TEXTURE) {
          Logger::debug(str::format("  texture.width: ", desc.d3d9.texture.width));
          Logger::debug(str::format("  texture.height: ", desc.d3d9.texture.height));
          Logger::debug(str::format("  texture.depth: ", desc.d3d9.texture.depth));
          Logger::debug(str::format("  texture.levels: ", desc.d3d9.texture.levels));
        } else if (desc.d3d9.type == D3DRTYPE_SURFACE) {
          Logger::debug(str::format("  surface.width: ", desc.d3d9.surface.width));
          Logger::debug(str::format("  surface.height: ", desc.d3d9.surface.height));
        }
        return true;
      }
    }

    return true;
  }


  bool D3D9DeviceEx::ValidateSharedBuffer(
      HANDLE                        handle,
      const dxvk::D3D9_BUFFER_DESC& bufferDesc) const {
    if (!(reinterpret_cast<uintptr_t>(handle) & 0xc0000000)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: not a D3DKMT handle: ", handle));
      return false;
    }

    union d3dkmt_desc desc;

    D3DKMT_QUERYRESOURCEINFO query = { };
    query.hDevice = 0u ;
    query.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
    query.pPrivateRuntimeData = &desc;
    query.PrivateRuntimeDataSize = sizeof(desc);

    if (D3DKMTQueryResourceInfo(&query)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Failed to query resource: ", handle));
    } else if (query.PrivateRuntimeDataSize < sizeof(desc.dxgi) || query.PrivateRuntimeDataSize > sizeof(desc)) {
      Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Unexpected size: ", query.PrivateRuntimeDataSize));
    } else {
      D3DDDI_OPENALLOCATIONINFO2 alloc = { };
      D3DKMT_OPENRESOURCE open = { };
      open.hDevice = 0u ;
      open.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
      open.NumAllocations = 1;
      open.pOpenAllocationInfo2 = &alloc;
      open.pPrivateRuntimeData = &desc;
      open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

      if (D3DKMTOpenResource2(&open)) {
        Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Failed to open resource: ", handle));
      } else {
        D3DKMT_DESTROYALLOCATION destroy = { };
        destroy.hDevice = 0u ;
        destroy.hResource = open.hResource;
        D3DKMTDestroyAllocation(&destroy);

        if (desc.dxgi.size != sizeof(desc.d3d9) || desc.dxgi.version != 1) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid size: ",
                                   desc.dxgi.size, " or version: ", desc.dxgi.version));
          return false;
        }
        if (desc.d3d9.type != bufferDesc.Type) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid type: ", desc.d3d9.type));
          return false;
        }
        if (desc.d3d9.dxgi.width != bufferDesc.Size || desc.d3d9.buffer.width != bufferDesc.Size) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid dimensions: ", desc.d3d9.dxgi.width, "x", desc.d3d9.dxgi.height));
          return false;
        }
        if (desc.d3d9.buffer.format != static_cast<UINT>(bufferDesc.Format)) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid format: ", desc.d3d9.format));
          return false;
        }
        if (bufferDesc.Usage & ~desc.d3d9.usage) {
          Logger::warn(str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid usage: ", desc.d3d9.usage));
          return false;
        }

        Logger::debug(str::format("Found D3D9 desc: ", desc.d3d9.type));
        Logger::debug(str::format("  dxgi.width: ", desc.d3d9.buffer.width));
        Logger::debug(str::format("  format: ", desc.d3d9.buffer.format));
        Logger::debug(str::format("  usage: ", desc.d3d9.usage));
        return true;
      }
    }

    return true;
  }

}
