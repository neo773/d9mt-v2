/* d9mt v2 single-RT offscreen-DRAW test (isolates the offscreen-draw bug from
 * multi-RT). Pass 1 draws a constant-green quad into ONE offscreen render target
 * (not just clearing it). Pass 2 samples that RT onto the backbuffer. If the
 * backbuffer shows green, offscreen geometry draws work; if blank, offscreen
 * draws are broken in general (not MRT-specific).
 */
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
  wc.lpszClassName = "d9mt_offdraw";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_offdraw", "d9mt offdraw", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  IDirect3DPixelShader9 *drawPs = NULL, *samplePs = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)alpha_ps_bytecode, &drawPs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &samplePs)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  static const struct Vertex verts[6] = {
      {-0.9f,  0.9f, 0.5f, 0, 0}, { 0.9f,  0.9f, 0.5f, 1, 0}, { 0.9f, -0.9f, 0.5f, 1, 1},
      {-0.9f,  0.9f, 0.5f, 0, 0}, { 0.9f, -0.9f, 0.5f, 1, 1}, {-0.9f, -0.9f, 0.5f, 0, 1},
  };
  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DTexture9 *rttex = NULL;
  IDirect3DSurface9 *rt = NULL, *backbuffer = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 800, 600, 1, D3DUSAGE_RENDERTARGET,
          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rttex, NULL)))
    return 1;
  IDirect3DTexture9_GetSurfaceLevel(rttex, 0, &rt);
  IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);

  IDirect3DQuery9 *query = NULL;
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &query);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);

  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  int frame = 0;

  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    /* Pass 1: DRAW a green quad into the offscreen RT, counting visible samples. */
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetPixelShader(dev, drawPs);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF101010, 1.0f, 0);
    if (query) IDirect3DQuery9_Issue(query, D3DISSUE_BEGIN);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    if (query) IDirect3DQuery9_Issue(query, D3DISSUE_END);

    /* Pass 2: sample the RT onto the backbuffer. */
    IDirect3DDevice9_SetRenderTarget(dev, 0, backbuffer);
    IDirect3DDevice9_SetPixelShader(dev, samplePs);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)rttex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF181828, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    if (query && ++frame >= 3) {
      DWORD samples = 0xFFFFFFFFu;
      IDirect3DQuery9_GetData(query, &samples, sizeof(samples), D3DGETDATA_FLUSH);
      FILE *f = fopen("C:\\d9mt-test\\offdraw_occl.txt", "w");
      if (f) { fprintf(f, "offscreen draw visible samples = %lu\n", (unsigned long) samples); fclose(f); }
    }
  }
  return 0;
}
