/* d9mt v2 BC subresource-box gate: a live partial (dirty-box) update of a
 * block-compressed texture — the streamed-texture UPDATE shape. A 128x128
 * DXT1 MANAGED texture is first filled solid BLUE; after 60 presented
 * frames LockRect with RECT{32,32,96,96} rewrites the centre 64x64 to
 * solid RED (block-aligned box -> FlushImage ships a SUB-RECT
 * upload_texture record: origin 32,32 + block-row pitch of the box, not
 * the whole level).
 *
 * The fullscreen quad's centre samples texel (64,64) — inside the box.
 * Gate: EXPECT="255,0,0" centre pixel late in the run; a transport that
 * drops the partial upload (or mis-applies its origin/pitch) shows blue,
 * black, or displaced red instead.
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

static void writeSolidBlock(BYTE *dst, WORD rgb565) {
  dst[0] = (BYTE)(rgb565 & 0xFF);
  dst[1] = (BYTE)(rgb565 >> 8);
  dst[2] = 0; dst[3] = 0;
  dst[4] = 0; dst[5] = 0; dst[6] = 0; dst[7] = 0;
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
  wc.lpszClassName = "d9mt_dxtbox";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_dxtbox", "d9mt dxt1 dirty-box update",
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

  static const struct Vertex verts[6] = {
      {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},  {1.0f, 1.0f, 0.5f, 1.0f, 0.0f},
      {1.0f, -1.0f, 0.5f, 1.0f, 1.0f},  {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},
      {1.0f, -1.0f, 0.5f, 1.0f, 1.0f},  {-1.0f, -1.0f, 0.5f, 0.0f, 1.0f},
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  /* 128x128 DXT1 = 32x32 blocks, single level, all BLUE. */
  IDirect3DTexture9 *tex = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 128, 128, 1, 0, D3DFMT_DXT1,
                                            D3DPOOL_MANAGED, &tex, NULL)))
    return 1;
  D3DLOCKED_RECT lr;
  if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
    for (UINT by = 0; by < 32; by++)
      for (UINT bx = 0; bx < 32; bx++)
        writeSolidBlock((BYTE *)lr.pBits + by * (UINT)lr.Pitch + bx * 8, 0x001F);
    IDirect3DTexture9_UnlockRect(tex, 0);
  }

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  int frames = 0;
  int boxDone = 0;
  DWORD t0 = GetTickCount();
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    /* 8 s of solid-blue frames first (pre-update dumps see the initial
     * whole-level upload), then the live dirty-box rewrite. */
    if (!boxDone && GetTickCount() - t0 > 8000) {
      /* live dirty-box update: centre 64x64 (block-aligned) -> RED.
       * pBits points at the RECT's first block; Pitch is the FULL row. */
      RECT box = {32, 32, 96, 96};
      if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, &box, 0))) {
        for (UINT by = 0; by < 16; by++)
          for (UINT bx = 0; bx < 16; bx++)
            writeSolidBlock((BYTE *)lr.pBits + by * (UINT)lr.Pitch + bx * 8,
                            0xF800);
        IDirect3DTexture9_UnlockRect(tex, 0);
        printf("box update applied\n");
      }
      boxDone = 1;
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
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
