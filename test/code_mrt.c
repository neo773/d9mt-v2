/* d9mt v2 multiple-render-target test. Binds two offscreen RTs and draws a
 * full quad with a pixel shader writing oC0=red and oC1=green. The DUMP_RT
 * readback then writes rt_dump.bin (RT0, expect red) and rt_dump1.bin (RT1,
 * expect green) — each RT must receive its own distinct output.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char mrt_ps_bytecode[] = {
#include "mrt_ps_bytecode.inc"
};

struct Vertex { float x, y, z, u, v; };

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_mrt";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_mrt", "d9mt mrt", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
          D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
    return 1;

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)mrt_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl);

  static const struct Vertex verts[6] = {
      {-1.0f,  1.0f, 0.5f, 0, 0}, { 1.0f,  1.0f, 0.5f, 1, 0}, { 1.0f, -1.0f, 0.5f, 1, 1},
      {-1.0f,  1.0f, 0.5f, 0, 0}, { 1.0f, -1.0f, 0.5f, 1, 1}, {-1.0f, -1.0f, 0.5f, 0, 1},
  };
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DTexture9 *tex0 = NULL, *tex1 = NULL;
  IDirect3DSurface9 *rt0 = NULL, *rt1 = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 800, 600, 1, D3DUSAGE_RENDERTARGET,
          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex0, NULL)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 800, 600, 1, D3DUSAGE_RENDERTARGET,
          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex1, NULL)))
    return 1;
  IDirect3DTexture9_GetSurfaceLevel(tex0, 0, &rt0);
  IDirect3DTexture9_GetSurfaceLevel(tex1, 0, &rt1);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt0);
    IDirect3DDevice9_SetRenderTarget(dev, 1, rt1);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF101010, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  return 0;
}
