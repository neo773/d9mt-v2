#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

struct Vertex { float x, y, z, u, v; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static DWORD read_center(IDirect3DDevice9 *dev) {
  IDirect3DSurface9 *rt = NULL, *sys = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT lr;
  DWORD c = 0xdeadbeef;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt))) return c;
  IDirect3DSurface9_GetDesc(rt, &desc);
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(
          dev, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
      c = ((DWORD *)lr.pBits)[(desc.Height / 2) * lr.Pitch / 4 + desc.Width / 2];
      IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
  }
  IDirect3DSurface9_Release(rt);
  return c;
}

/* one solid-color DXT5 block: both endpoints = c565, alpha = 0xFF, all indices 0. */
static void fill_dxt5_block(BYTE *b, WORD c565) {
  b[0] = 0xFF; b[1] = 0xFF;
  b[2] = b[3] = b[4] = b[5] = b[6] = b[7] = 0;
  b[8] = (BYTE)(c565 & 0xFF); b[9] = (BYTE)(c565 >> 8);
  b[10] = (BYTE)(c565 & 0xFF); b[11] = (BYTE)(c565 >> 8);
  b[12] = b[13] = b[14] = b[15] = 0;
}

static void draw_and_report(IDirect3DDevice9 *dev, const char *name, DWORD expect_mask) {
  IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 16), 1.0f, 0);
  IDirect3DDevice9_BeginScene(dev);
  IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
  IDirect3DDevice9_EndScene(dev);
  DWORD c = read_center(dev);
  DWORD r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  int pass = expect_mask == 0x00FF00 ? (g > 0xC0 && r < 0x40 && b < 0x40)
                                     : (r > 0xC0 && g < 0x40 && b < 0x40);
  printf("CASE-%s pixel=%08lx %s\n", name, c, pass ? "PASS" : "FAIL");
  fflush(stdout);
  IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_updatetex";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_updatetex", "d9mt updatetex",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) { printf("FAIL window\n"); return 1; }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
    printf("FAIL device\n"); return 1;
  }

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs);
  IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &ps);
  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl);
  static const struct Vertex verts[6] = {
      {-0.9f,  0.9f, 0.5f, 0, 0}, { 0.9f,  0.9f, 0.5f, 1, 0},
      { 0.9f, -0.9f, 0.5f, 1, 1}, {-0.9f,  0.9f, 0.5f, 0, 0},
      { 0.9f, -0.9f, 0.5f, 1, 1}, {-0.9f, -0.9f, 0.5f, 0, 1}};
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  static const float mvp[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);

  enum { TEX = 64 };
  D3DLOCKED_RECT lr;
  HRESULT hr;

  {
    IDirect3DTexture9 *sysTex = NULL, *defTex = NULL;
    IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_A8R8G8B8,
                                   D3DPOOL_SYSTEMMEM, &sysTex, NULL);
    IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_A8R8G8B8,
                                   D3DPOOL_DEFAULT, &defTex, NULL);
    if (SUCCEEDED(IDirect3DTexture9_LockRect(sysTex, 0, &lr, NULL, 0))) {
      for (int y = 0; y < TEX; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < TEX; x++) row[x] = 0xFF00FF00;
      }
      IDirect3DTexture9_UnlockRect(sysTex, 0);
    }
    hr = IDirect3DDevice9_UpdateTexture(
        dev, (IDirect3DBaseTexture9 *)sysTex, (IDirect3DBaseTexture9 *)defTex);
    printf("UpdateTexture(A8R8G8B8) hr=%08lx\n", hr);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)defTex);
    draw_and_report(dev, "A-argb-green", 0x00FF00);
    IDirect3DTexture9_Release(sysTex);
    IDirect3DTexture9_Release(defTex);
  }

  {
    IDirect3DTexture9 *sysTex = NULL, *defTex = NULL;
    IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_DXT5,
                                   D3DPOOL_SYSTEMMEM, &sysTex, NULL);
    IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_DXT5,
                                   D3DPOOL_DEFAULT, &defTex, NULL);
    if (SUCCEEDED(IDirect3DTexture9_LockRect(sysTex, 0, &lr, NULL, 0))) {
      for (int by = 0; by < TEX / 4; by++) {
        BYTE *row = (BYTE *)lr.pBits + by * lr.Pitch;
        for (int bx = 0; bx < TEX / 4; bx++)
          fill_dxt5_block(row + bx * 16, 0xF800);
      }
      IDirect3DTexture9_UnlockRect(sysTex, 0);
    }
    hr = IDirect3DDevice9_UpdateTexture(
        dev, (IDirect3DBaseTexture9 *)sysTex, (IDirect3DBaseTexture9 *)defTex);
    printf("UpdateTexture(DXT5) hr=%08lx\n", hr);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)defTex);
    draw_and_report(dev, "B-dxt5-red", 0xFF0000);
    IDirect3DTexture9_Release(sysTex);
    IDirect3DTexture9_Release(defTex);
  }

  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
