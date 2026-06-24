/* d9mt v2 cube-texture test: a quad sampling a cube map through an SM3 vs/ps
 * pair (passthrough position + 3D direction; texCUBE of s0).
 *
 * Exercises cube textures: D3D9 CreateCubeTexture + per-face LockRect upload to
 * the six Metal cube slices, and a samplerCUBE the translated shader samples
 * through. Each face is a distinct solid color; the quad's direction points at
 * +Z (face index 4 = cyan), so a cyan quad proves the +Z face uploaded to the
 * correct slice and cube sampling works (a face-0 color would mean a flat 2D
 * mis-handling).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char cube_vs_bytecode[] = {
#include "cube_vs_bytecode.inc"
};
static const unsigned char cube_ps_bytecode[] = {
#include "cube_ps_bytecode.inc"
};

struct Vertex {
  float x, y, z;
  float dx, dy, dz;
};

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
  wc.lpszClassName = "d9mt_cube";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_cube", "d9mt cube map",
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
          dev, (const DWORD *)cube_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(
          dev, (const DWORD *)cube_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  /* Quad with the cube direction pointing at +Z (z dominant); x/y nudge the
   * direction slightly so it stays on the +Z face across the whole quad. */
  static const struct Vertex verts[6] = {
      {-0.7f,  0.7f, 0.5f, -0.21f,  0.21f, 1.0f},
      { 0.7f,  0.7f, 0.5f,  0.21f,  0.21f, 1.0f},
      { 0.7f, -0.7f, 0.5f,  0.21f, -0.21f, 1.0f},
      {-0.7f,  0.7f, 0.5f, -0.21f,  0.21f, 1.0f},
      { 0.7f, -0.7f, 0.5f,  0.21f, -0.21f, 1.0f},
      {-0.7f, -0.7f, 0.5f, -0.21f, -0.21f, 1.0f},
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  /* One distinct solid color per cube face, indexed by D3DCUBEMAP_FACE_*:
   * +X red, -X green, +Y blue, -Y yellow, +Z cyan, -Z magenta. */
  enum { FACE = 8 };
  static const DWORD faceColor[6] = {
      0xFFE00000, 0xFF00E000, 0xFF0000E0,
      0xFFE0E000, 0xFF00E0E0, 0xFFE000E0,
  };
  IDirect3DCubeTexture9 *cube = NULL;
  if (FAILED(IDirect3DDevice9_CreateCubeTexture(
          dev, FACE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL)))
    return 1;
  for (int f = 0; f < 6; f++) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DCubeTexture9_LockRect(cube, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0))) {
      for (int y = 0; y < FACE; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < FACE; x++)
          row[x] = faceColor[f];
      }
      IDirect3DCubeTexture9_UnlockRect(cube, (D3DCUBEMAP_FACES)f, 0);
    }
  }

  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)cube);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF181828, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  return 0;
}
