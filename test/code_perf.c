/* d9mt v2 — the absolute minimum render: ONE static red triangle.
 *
 * Pre-transformed vertices (D3DFVF_XYZRHW + DIFFUSE): no vertex shader, no
 * constants, no matrices, no depth test, no textures, one draw per frame over
 * a dark-blue clear. A STATIC scene: frame 1 state stays correct forever, so
 * any per-frame state-update path is deliberately out of the picture.
 *
 * Purpose: the smallest possible end-to-end probe of clear + one FF draw +
 * present. If this shows on screen, the whole pipeline minus per-frame state
 * updates is proven.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define LOG(...)  do { printf("[tri] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { \
    HRESULT _hr = (call); \
    printf("[HR ] %-46s -> 0x%08lx %s\n", #call, (unsigned long)_hr, \
           SUCCEEDED(_hr) ? "OK" : "**FAIL**"); \
    fflush(stdout); \
    if (FAILED(_hr)) LOG("!!! FAILED at %s:%d hr=0x%08lx", __FILE__, __LINE__, (unsigned long)_hr); \
  } while (0)

struct Vertex { float x, y, z, rhw; DWORD color; };

int main(void) {
  const int W = 800, H = 600;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_perf";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_perf", "d9mt perf", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) { LOG("FAIL CreateWindow"); return 1; }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { LOG("FAIL Direct3DCreate9"); return 1; }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = FALSE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
  if (!dev) { LOG("FAIL device"); return 1; }
  LOG("device created %dx%d X8R8G8B8, no depth", W, H);

  /* One big centered red triangle in screen coordinates. */
  static const struct Vertex tri[3] = {
    { 400.0f, 100.0f, 0.5f, 1.0f, 0xFFFF0000 },
    { 650.0f, 500.0f, 0.5f, 1.0f, 0xFFFF0000 },
    { 150.0f, 500.0f, 0.5f, 1.0f, 0xFFFF0000 },
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(tri),
      0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb, NULL));
  void *p = NULL;
  CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(tri), &p, 0));
  memcpy(p, tri, sizeof(tri));
  CHECK(IDirect3DVertexBuffer9_Unlock(vb));

  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));

  for (int frame = 0; frame < 20000; frame++) {  /* UNTHROTTLED perf run */
    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                                 D3DCOLOR_XRGB(16, 16, 40), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }

  LOG("60 frames presented — leaving window up briefly");
  Sleep(3000);

  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  LOG("clean exit");
  return 0;
}
