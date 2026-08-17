#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

#define LOG(...)  do { printf("[tex] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { \
    HRESULT _hr = (call); \
    printf("[HR ] %-46s -> 0x%08lx %s\n", #call, (unsigned long)_hr, \
           SUCCEEDED(_hr) ? "OK" : "**FAIL**"); \
    fflush(stdout); \
    if (FAILED(_hr)) LOG("!!! FAILED at %s:%d hr=0x%08lx", __FILE__, __LINE__, (unsigned long)_hr); \
  } while (0)

struct Vertex { float x, y, z; float u, v; };

typedef float mat4[16];
static void m_identity(mat4 m) {
  for (int i = 0; i < 16; i++) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void m_mul(mat4 out, const mat4 a, const mat4 b) {
  mat4 t;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++) s += a[r*4+k] * b[k*4+c];
      t[r*4+c] = s;
    }
  for (int i = 0; i < 16; i++) out[i] = t[i];
}
static void m_rot_y(mat4 m, float a) {
  m_identity(m);
  float c = cosf(a), s = sinf(a);
  m[0] = c;  m[2] = -s;
  m[8] = s;  m[10] = c;
}
static void m_rot_x(mat4 m, float a) {
  m_identity(m);
  float c = cosf(a), s = sinf(a);
  m[5] = c;  m[6] = s;
  m[9] = -s; m[10] = c;
}
static void m_translate(mat4 m, float x, float y, float z) {
  m_identity(m);
  m[12] = x; m[13] = y; m[14] = z;
}
static void m_persp(mat4 m, float fovy, float aspect, float zn, float zf) {
  for (int i = 0; i < 16; i++) m[i] = 0.0f;
  float ys = 1.0f / tanf(fovy * 0.5f);
  float xs = ys / aspect;
  m[0] = xs;
  m[5] = ys;
  m[10] = zf / (zf - zn);
  m[11] = 1.0f;
  m[14] = -zn * zf / (zf - zn);
}

static DWORD read_px(IDirect3DDevice9 *dev, UINT x, UINT y) {
  IDirect3DSurface9 *rt = NULL, *sys = NULL;
  D3DSURFACE_DESC d;
  D3DLOCKED_RECT lr;
  DWORD c = 0xdeadbeef;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt))) return c;
  IDirect3DSurface9_GetDesc(rt, &d);
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(
          dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
      DWORD raw = ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x];
      c = raw & 0x00ffffff;
      IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
  }
  IDirect3DSurface9_Release(rt);
  return c;
}

enum { W = 1504, H = 924, FRAMES = 1000000, TEX = 256 };

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_texcube3d";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_texcube3d", "d9mt texcube3d", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
  if (!dev) { LOG("FAIL device"); return 1; }
  LOG("device created %dx%d X8R8G8B8 + D24S8 depth", W, H);

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 decl[] = {
      {0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *vdecl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, decl, &vdecl));

  static const struct Vertex verts[24] = {
    {-0.5f,-0.5f,-0.5f, 0,1},{ 0.5f,-0.5f,-0.5f, 1,1},{ 0.5f, 0.5f,-0.5f, 1,0},{-0.5f, 0.5f,-0.5f, 0,0},
    { 0.5f,-0.5f, 0.5f, 0,1},{-0.5f,-0.5f, 0.5f, 1,1},{-0.5f, 0.5f, 0.5f, 1,0},{ 0.5f, 0.5f, 0.5f, 0,0},
    {-0.5f,-0.5f, 0.5f, 0,1},{-0.5f,-0.5f,-0.5f, 1,1},{-0.5f, 0.5f,-0.5f, 1,0},{-0.5f, 0.5f, 0.5f, 0,0},
    { 0.5f,-0.5f,-0.5f, 0,1},{ 0.5f,-0.5f, 0.5f, 1,1},{ 0.5f, 0.5f, 0.5f, 1,0},{ 0.5f, 0.5f,-0.5f, 0,0},
    {-0.5f, 0.5f,-0.5f, 0,1},{ 0.5f, 0.5f,-0.5f, 1,1},{ 0.5f, 0.5f, 0.5f, 1,0},{-0.5f, 0.5f, 0.5f, 0,0},
    {-0.5f,-0.5f, 0.5f, 0,1},{ 0.5f,-0.5f, 0.5f, 1,1},{ 0.5f,-0.5f,-0.5f, 1,0},{-0.5f,-0.5f,-0.5f, 0,0},
  };
  WORD idx[36];
  for (int f = 0; f < 6; f++) {
    WORD b = (WORD)(f * 4);
    WORD *o = &idx[f * 6];
    o[0]=b; o[1]=(WORD)(b+1); o[2]=(WORD)(b+2);
    o[3]=b; o[4]=(WORD)(b+2); o[5]=(WORD)(b+3);
  }

  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9  *ib = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL));
  CHECK(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL));
  { void *p = NULL; CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0));
    memcpy(p, verts, sizeof(verts)); IDirect3DVertexBuffer9_Unlock(vb); }
  { void *p = NULL; CHECK(IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(idx), &p, 0));
    memcpy(p, idx, sizeof(idx)); IDirect3DIndexBuffer9_Unlock(ib); }

  IDirect3DTexture9 *tex = NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &tex, NULL));
  { D3DLOCKED_RECT lr;
    CHECK(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0));
    for (int ty = 0; ty < TEX; ty++) {
      DWORD *row = (DWORD *)((BYTE *)lr.pBits + ty * lr.Pitch);
      for (int tx = 0; tx < TEX; tx++) {
        BYTE r = (BYTE)tx;
        BYTE g = (BYTE)ty;
        BYTE b = (((tx >> 5) ^ (ty >> 5)) & 1) ? 240 : 40;
        row[tx] = 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
      }
    }
    IDirect3DTexture9_UnlockRect(tex, 0);
    LOG("texture %dx%d A8R8G8B8 filled (R=u, G=v, B=checker)", TEX, TEX);
  }

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, vdecl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));
  CHECK(IDirect3DDevice9_SetIndices(dev, ib));
  CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER,
        getenv("POINT") ? D3DTEXF_POINT : D3DTEXF_LINEAR));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER,
        getenv("POINT") ? D3DTEXF_POINT : D3DTEXF_LINEAR));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, getenv("NOZ") ? D3DZB_FALSE : D3DZB_TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, getenv("NOZ") ? FALSE : TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));

  mat4 proj, view;
  m_persp(proj, 3.14159265f / 3.0f, (float)W / (float)H, 0.5f, 20.0f);
  m_translate(view, 0.0f, 0.0f, 3.0f);

  DWORD prev_center = 0xdeadbeef;
  int pass_visible = 0, pass_changed = 0, pass_varied = 0;

  for (int f = 0; f < FRAMES; f++) {
    mat4 rx, ry, world, wv, mvp;
    float ang = (getenv("FREEZE") ? (float)atoi(getenv("FREEZE")) : (float)f) * 0.03f;
    m_rot_y(ry, ang);
    m_rot_x(rx, ang * 0.5f);
    m_mul(world, rx, ry);
    m_mul(wv, world, view);
    m_mul(mvp, wv, proj);

    /* HLSL constant registers are read column-major; transpose to match. */
    if (!getenv("NOTR")) {
      mat4 t;
      for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) t[c*4+r] = mvp[r*4+c];
      memcpy(mvp, t, sizeof(mvp));
    }
    CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4));

    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 D3DCOLOR_XRGB(16, 16, 40), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));

    if (getenv("NORB")) continue;

    DWORD bg = 0x101028;
    DWORD c  = read_px(dev, W/2,     H/2);
    DWORD s1 = read_px(dev, W/2 - 120, H/2 - 120);
    DWORD s2 = read_px(dev, W/2 + 120, H/2 + 120);
    DWORD s3 = read_px(dev, W/2 + 120, H/2 - 120);
    int visible = (c != bg && c != 0xdeadbeef);
    int changed = (f > 0 && c != prev_center);
    int on = 0, distinct = 0;
    DWORD samp[4] = {c, s1, s2, s3};
    for (int i = 0; i < 4; i++) if (samp[i] != bg && samp[i] != 0xdeadbeef) on++;
    for (int i = 0; i < 4; i++) for (int j = i+1; j < 4; j++)
      if (samp[i] != samp[j] && samp[i] != bg && samp[j] != bg) { distinct = 1; }
    if (visible) pass_visible++;
    if (changed) pass_changed++;
    if (distinct) pass_varied++;
    if (f < 8 || (f % 60) == 0)
      LOG("f%-5d center=0x%06lx s=[%06lx %06lx %06lx] on=%d/4 varied=%d changed=%d",
          f, (unsigned long)c, (unsigned long)s1, (unsigned long)s2,
          (unsigned long)s3, on, distinct, changed);
    prev_center = c;

    if (f == 240) {
      LOG("======== SUMMARY (first 240 frames) ========");
      LOG("visible=%d varied=%d changed=%d", pass_visible, pass_varied, pass_changed);
      LOG("RESULT: %s", (pass_visible > 200 && pass_varied > 100 && pass_changed > 100)
            ? "PASS (textured + sampled + moving)" : "FAIL (see samples above)");
    }
  }

  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
