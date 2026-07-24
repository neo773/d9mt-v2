/* d9mt v2 world-texture-shaped BC gate: the exact shape streamed game art
 * ships in — block-compressed (DXT1 + DXT5), FULL mip chain, MANAGED pool,
 * every level filled through LockRect/UnlockRect (the FlushImage ->
 * copyBufferToTexture -> wire upload_texture path, one subresource at a
 * time, block-row pitch math per level down to the 1x1 tail).
 *
 * The fullscreen quad samples the DXT1 texture with MAXMIPLEVEL=2 +
 * MIPFILTER=POINT, so the frame's centre shows MIP 2's colour (solid
 * green). A transport that ships only mip 0 (or drops streamed content)
 * renders black/red there instead. Gate: EXPECT="0,255,0" centre pixel.
 * A corner quad samples a full-chain DXT5 texture (alpha-block layout)
 * for coverage of the 16-byte block path.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

struct Vertex {
  float x, y, z;
  float u, v;
};

/* DXT1 block: color0 (RGB565), color1=0 (color0>color1 = 4-colour mode),
 * all-zero indices select color0 everywhere — a solid block. */
static void writeSolidBlock(BYTE *dst, WORD rgb565) {
  dst[0] = (BYTE)(rgb565 & 0xFF);
  dst[1] = (BYTE)(rgb565 >> 8);
  dst[2] = 0; dst[3] = 0;
  dst[4] = 0; dst[5] = 0; dst[6] = 0; dst[7] = 0;
}

/* DXT5 block: 8-byte alpha block (alpha0=255, indices 0 -> opaque) + the
 * same solid colour block as DXT1. */
static void writeSolidBlockDXT5(BYTE *dst, WORD rgb565) {
  dst[0] = 0xFF; dst[1] = 0xFF;
  memset(dst + 2, 0, 6);
  writeSolidBlock(dst + 8, rgb565);
}

static void fillMipChainDXT1(IDirect3DTexture9 *tex, UINT topDim,
                             const WORD *colors, UINT nColors) {
  UINT levels = IDirect3DTexture9_GetLevelCount(tex);
  for (UINT lvl = 0; lvl < levels; lvl++) {
    UINT dim = topDim >> lvl;
    if (!dim) dim = 1;
    UINT blocks = (dim + 3) / 4;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(tex, lvl, &lr, NULL, 0)))
      continue;
    WORD c = colors[lvl < nColors ? lvl : nColors - 1];
    for (UINT by = 0; by < blocks; by++)
      for (UINT bx = 0; bx < blocks; bx++)
        writeSolidBlock((BYTE *)lr.pBits + by * (UINT)lr.Pitch + bx * 8, c);
    IDirect3DTexture9_UnlockRect(tex, lvl);
  }
}

static void fillMipChainDXT5(IDirect3DTexture9 *tex, UINT topDim, WORD c) {
  UINT levels = IDirect3DTexture9_GetLevelCount(tex);
  for (UINT lvl = 0; lvl < levels; lvl++) {
    UINT dim = topDim >> lvl;
    if (!dim) dim = 1;
    UINT blocks = (dim + 3) / 4;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(tex, lvl, &lr, NULL, 0)))
      continue;
    for (UINT by = 0; by < blocks; by++)
      for (UINT bx = 0; bx < blocks; bx++)
        writeSolidBlockDXT5((BYTE *)lr.pBits + by * (UINT)lr.Pitch + bx * 16, c);
    IDirect3DTexture9_UnlockRect(tex, lvl);
  }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_dxtworld";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_dxtworld", "d9mt dxt mip-chain world shape",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd)
    return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800;
  pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(
      d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev)
    return 1;

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(
          dev, (const DWORD *)texquad_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(
          dev, (const DWORD *)texquad_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  /* verts 0-5: fullscreen quad (DXT1, forced mip 2);
   * verts 6-11: top-left corner quad (DXT5 chain). */
  static const struct Vertex verts[12] = {
      {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},   {1.0f, 1.0f, 0.5f, 1.0f, 0.0f},
      {1.0f, -1.0f, 0.5f, 1.0f, 1.0f},   {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},
      {1.0f, -1.0f, 0.5f, 1.0f, 1.0f},   {-1.0f, -1.0f, 0.5f, 0.0f, 1.0f},

      {-0.95f, 0.95f, 0.5f, 0.0f, 0.0f}, {-0.55f, 0.95f, 0.5f, 1.0f, 0.0f},
      {-0.55f, 0.55f, 0.5f, 1.0f, 1.0f}, {-0.95f, 0.95f, 0.5f, 0.0f, 0.0f},
      {-0.55f, 0.55f, 0.5f, 1.0f, 1.0f}, {-0.95f, 0.55f, 0.5f, 0.0f, 1.0f},
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  /* DXT1 256x256, FULL mip chain (Levels=0 -> 9 levels). Per-mip solid
   * colour: mip0 red, mip1 blue, mip2 GREEN (the asserted one), rest
   * yellow — a wrong-mip fetch is visible as the wrong colour, a missing
   * upload as black. */
  IDirect3DTexture9 *tex1 = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 256, 256, 0, 0, D3DFMT_DXT1,
                                            D3DPOOL_MANAGED, &tex1, NULL)))
    return 1;
  static const WORD mipColors[] = {0xF800, 0x001F, 0x07E0, 0xFFE0};
  fillMipChainDXT1(tex1, 256, mipColors, 4);

  /* DXT5 128x128 full chain, solid cyan — 16-byte block coverage. */
  IDirect3DTexture9 *tex5 = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 128, 128, 0, 0, D3DFMT_DXT5,
                                            D3DPOOL_MANAGED, &tex5, NULL)))
    return 1;
  fillMipChainDXT5(tex5, 128, 0x07FF);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    /* fullscreen quad: DXT1 clamped to mip 2 (green) */
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex1);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 2);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);

    /* corner quad: DXT5 chain at mip 0 (cyan) */
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex5);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 6, 2);

    IDirect3DDevice9_EndScene(dev);
    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    if (++frames == 3)
      printf("3 frames presented\n");
  }

done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
