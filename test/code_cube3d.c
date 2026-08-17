#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <math.h>

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
};
static const unsigned char shadertri_ps_bytecode[] = {
#include "shadertri_ps_bytecode.inc"
};

#define LOG(...)  do { printf("[cube] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { \
    HRESULT _hr = (call); \
    printf("[HR ] %-46s -> 0x%08lx %s\n", #call, (unsigned long)_hr, \
           SUCCEEDED(_hr) ? "OK" : "**FAIL**"); \
    fflush(stdout); \
    if (FAILED(_hr)) LOG("!!! FAILED at %s:%d hr=0x%08lx", __FILE__, __LINE__, (unsigned long)_hr); \
  } while (0)

struct Vertex { float x, y, z; DWORD color; };

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

enum { W = 1504, H = 924, FRAMES = 1000000 };

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_cube3d";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_cube3d", "d9mt cube3d", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  CHECK(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)shadertri_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)shadertri_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 decl[] = {
      {0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *vdecl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, decl, &vdecl));

  static const struct Vertex verts[8] = {
      {-0.5f,-0.5f,-0.5f, 0xFFFF0000}, { 0.5f,-0.5f,-0.5f, 0xFF00FF00},
      { 0.5f, 0.5f,-0.5f, 0xFF0000FF}, {-0.5f, 0.5f,-0.5f, 0xFFFFFF00},
      {-0.5f,-0.5f, 0.5f, 0xFFFF00FF}, { 0.5f,-0.5f, 0.5f, 0xFF00FFFF},
      { 0.5f, 0.5f, 0.5f, 0xFFFFFFFF}, {-0.5f, 0.5f, 0.5f, 0xFF808080}};
  static const WORD idx[36] = {
      0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
      3,2,6, 3,6,7,  1,5,6, 1,6,2,  0,3,7, 0,7,4};

  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9  *ib = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL));
  CHECK(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL));
  { void *p = NULL; CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0));
    memcpy(p, verts, sizeof(verts)); IDirect3DVertexBuffer9_Unlock(vb); }
  { void *p = NULL; CHECK(IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(idx), &p, 0));
    memcpy(p, idx, sizeof(idx)); IDirect3DIndexBuffer9_Unlock(ib); }

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, vdecl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));
  CHECK(IDirect3DDevice9_SetIndices(dev, ib));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, getenv("NOZ") ? D3DZB_FALSE : D3DZB_TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, getenv("NOZ") ? FALSE : TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));

  { static const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1)); }

  mat4 proj, view;
  m_persp(proj, 3.14159265f / 3.0f, (float)W / (float)H, 0.5f, 20.0f);
  m_translate(view, 0.0f, 0.0f, 3.0f);

  DWORD prev_center = 0xdeadbeef;
  int pass_visible = 0, pass_changed = 0;

  for (int f = 0; f < FRAMES; f++) {
    LOG("======== FRAME %d ========", f);

    if (getenv("STRESS")) {
      IDirect3DVertexBuffer9 *scratch = NULL;
      CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, 4096, D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &scratch, NULL));
      if (scratch) IDirect3DVertexBuffer9_Release(scratch);
    }

    mat4 world, wv, mvp;
    float ang = (getenv("FREEZE") ? (float)atoi(getenv("FREEZE")) : (float)f) * 0.03f;
    m_rot_y(world, ang);
    m_mul(wv, world, view);
    m_mul(mvp, wv, proj);
    LOG("mvp row0 = %.3f %.3f %.3f %.3f", mvp[0], mvp[1], mvp[2], mvp[3]);
    LOG("mvp row3 = %.3f %.3f %.3f %.3f", mvp[12], mvp[13], mvp[14], mvp[15]);
    /* HLSL's default column-major layout reads c0..c3 as matrix columns
     * (mul(pos, M) via dp4), so D3D row-vector apps must upload M^T. */
    if (!getenv("NOTR")) {
      mat4 mvpT;
      for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) mvpT[c*4+r] = mvp[r*4+c];
      memcpy(mvp, mvpT, sizeof(mvp));
    }
    static mat4 mvp0;
    if (f == 0) memcpy(mvp0, mvp, sizeof(mvp0));
    const float *upload = getenv("SAMEVAL") ? mvp0 : mvp;
    if (!getenv("STATICMVP") || f == 0)
      CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, upload, 4));

    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 D3DCOLOR_XRGB(16, 16, 40), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 8, 0, 12));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));

    if (getenv("RARERB") && f >= 2 && f != FRAMES - 1) { continue; }
    if (getenv("NORB")) { continue; }

    DWORD center = read_px(dev, W / 2, H / 2);
    DWORD bg = 0x101028;
    LOG("readback center(%d,%d) = 0x%06lx  (bg=0x%06lx)", W/2, H/2,
        (unsigned long)center, (unsigned long)bg);
    for (int gy = 0; gy < 3; gy++) {
      DWORD row[3];
      for (int gx = 0; gx < 3; gx++)
        row[gx] = read_px(dev, W/4 + gx*(W/4), H/4 + gy*(H/4));
      LOG("  grid[%d] = %06lx %06lx %06lx", gy,
          (unsigned long)row[0], (unsigned long)row[1], (unsigned long)row[2]);
    }
    int visible = (center != bg && center != 0xdeadbeef);
    int changed = (f > 0 && center != prev_center);
    LOG("ASSERT visible=%d (center!=bg)   changed=%d (center!=prev)", visible, changed);
    if (visible) pass_visible++;
    if (changed) pass_changed++;
    prev_center = center;
  }

  LOG("======== SUMMARY ========");
  LOG("frames=%d  visible_frames=%d/%d  changed_frames=%d/%d",
      FRAMES, pass_visible, FRAMES, pass_changed, FRAMES - 1);
  LOG("RESULT: %s", (pass_visible >= FRAMES - 1 && pass_changed >= 1) ? "PASS (cube renders + moves)"
                                                                      : "FAIL (black / static)");
  fflush(stdout);

  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
