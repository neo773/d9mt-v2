/* d9mt v2 per-draw-constant test (capture-inspected): TWO draws per frame with
 * DIFFERENT vertex-shader constants between them — the case GTA hits and no
 * other v2 test covers.
 *
 * Draw 1: identity mvp  -> big triangle covering screen center.
 * Draw 2: zero-scale mvp -> collapses to a point (covers nothing) but is the
 *         LAST constant set.
 *
 * Validated by GPU capture (CrossOver blocks readback): with correct per-draw
 * indirection the drawable shows the big triangle; if the per-draw argument/
 * push/spec buffers are shared+overwritten, draw 1 retroactively reads draw 2's
 * zero mvp and the drawable is empty (black).
 *
 * Presents to the backbuffer every frame so the in-driver capture trigger
 * (C:\d9mt-test\DO_CAPTURE) fires.  Reuses the shadertri vs/ps bytecode.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
};
static const unsigned char shadertri_ps_bytecode[] = {
#include "shadertri_ps_bytecode.inc"
};

struct Vertex { float x, y, z; DWORD color; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_twodraw";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("d9mt_twodraw", "d9mt twodraw",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9* dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
          D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
    return 1;

  IDirect3DVertexShader9* vs = NULL;
  IDirect3DPixelShader9* ps = NULL;
  IDirect3DDevice9_CreateVertexShader(dev, (const DWORD*)shadertri_vs_bytecode, &vs);
  IDirect3DDevice9_CreatePixelShader(dev, (const DWORD*)shadertri_ps_bytecode, &ps);

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9* decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl);

  static const struct Vertex verts[3] = {
      {0.0f, 0.6f, 0.5f, 0xFFFFFFFF},
      {0.6f, -0.6f, 0.5f, 0xFFFFFFFF},
      {-0.6f, -0.6f, 0.5f, 0xFFFFFFFF},
  };
  IDirect3DVertexBuffer9* vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  void* p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  static const float mvpFull[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  static const float mvpZero[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
  static const float tint[4] = {1,1,1,1};

  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));

  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg); DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0,0,0), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvpFull, 4);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);

    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvpZero, 4);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);

    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }

done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
