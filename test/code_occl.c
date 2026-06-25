/* d9mt v2 occlusion-query test. Draws a known-size quad (NDC -0.5..0.5, so
 * ~1/4 of an 800x600 target ≈ 120000 pixels) wrapped in an occlusion query,
 * then reads the visible-sample count back and writes it to a file. The count
 * is in physical drawable samples: on a 2x Retina drawable the 800x600 window's
 * center quarter is 1600x1200 * 1/4 = 480000. A count near that means real GPU
 * visibility counting works; ~0xFFFFFFFF means the conservative fallback.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char alpha_ps_bytecode[] = {
#include "alpha_ps_bytecode.inc"
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
  wc.lpszClassName = "d9mt_occl";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_occl", "d9mt occl", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs);
  IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)alpha_ps_bytecode, &ps);

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl);

  static const struct Vertex verts[6] = {
      {-0.5f,  0.5f, 0.5f, 0, 0}, { 0.5f,  0.5f, 0.5f, 1, 0}, { 0.5f, -0.5f, 0.5f, 1, 1},
      {-0.5f,  0.5f, 0.5f, 0, 0}, { 0.5f, -0.5f, 0.5f, 1, 1}, {-0.5f, -0.5f, 0.5f, 0, 1},
  };
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DQuery9 *query = NULL;
  if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &query)) || !query)
    return 1;

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);

  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);

  MSG msg;
  int frame = 0;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF101010, 1.0f, 0);
    IDirect3DQuery9_Issue(query, D3DISSUE_BEGIN);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DQuery9_Issue(query, D3DISSUE_END);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    DWORD samples = 0;
    HRESULT hr = IDirect3DQuery9_GetData(query, &samples, sizeof(samples), D3DGETDATA_FLUSH);
    frame++;
    if (hr == S_OK && (samples != 0xFFFFFFFFu || frame > 20)) {
      FILE *f = fopen("C:\\d9mt-test\\occl.txt", "w");
      if (f) {
        fprintf(f, "frame %d: occlusion samples = %lu (expect ~480000 on 2x drawable)\n", frame, (unsigned long) samples);
        fclose(f);
      }
      return 0;
    }
  }
  return 0;
}
