#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD read_pixel(IDirect3DDevice9 *dev, UINT x, UINT y, HRESULT *out_hr) {
  IDirect3DSurface9 *rt = NULL, *sys = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT lr;
  DWORD color = 0xdeadbeef;
  HRESULT hr;

  hr = IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
  if (FAILED(hr)) { *out_hr = hr; return color; }
  IDirect3DSurface9_GetDesc(rt, &desc);

  hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
  if (FAILED(hr)) { *out_hr = hr; IDirect3DSurface9_Release(rt); return color; }

  /* canary value distinguishes "GPU never wrote" (survives) from
   * "Lock reads different memory" (reads back as 0) */
  hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, 0);
  if (SUCCEEDED(hr)) {
    ((DWORD *)lr.pBits)[y * lr.Pitch / 4 + x] = 0xAABBCCDD;
    IDirect3DSurface9_UnlockRect(sys);
  }

  hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
  *out_hr = hr;
  if (SUCCEEDED(hr)) {
    hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
      color = ((DWORD *)lr.pBits)[y * lr.Pitch / 4 + x];
      IDirect3DSurface9_UnlockRect(sys);
    } else {
      *out_hr = hr;
    }
  }
  IDirect3DSurface9_Release(sys);
  IDirect3DSurface9_Release(rt);
  return color;
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_readback";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_readback", "d9mt readback",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) { printf("FAIL window\n"); return 1; }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { printf("FAIL d3d9\n"); return 1; }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(
      d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) { printf("FAIL device %08lx\n", hr); return 1; }

  HRESULT rhr;
  DWORD c;

  if (GetEnvironmentVariableA("D9MT_CAPTURE_WAIT", NULL, 0)) {
    printf("WAITING 8s for gpucapture attach...\n");
    fflush(stdout);
    Sleep(8000);
  }

  hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
  c = read_pixel(dev, 1, 1, &rhr);
  printf("CASE-A clear=ffff0000 hr=%08lx rhr=%08lx pixel=%08lx %s\n",
         hr, rhr, c, (c & 0xffffff) == 0xff0000 ? "PASS" : "FAIL");

  hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ddee, 0.0f, 0);
  c = read_pixel(dev, 639, 479, &rhr);
  printf("CASE-B clear=ff00ddee hr=%08lx rhr=%08lx pixel=%08lx %s\n",
         hr, rhr, c, (c & 0xffffff) == 0x00ddee ? "PASS" : "FAIL");

  hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
  IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  c = read_pixel(dev, 320, 240, &rhr);
  printf("CASE-C clear=ff00ff00+present hr=%08lx rhr=%08lx pixel=%08lx %s\n",
         hr, rhr, c, (c & 0xffffff) == 0x00ff00 ? "PASS-frontish" : "note");

  fflush(stdout);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
