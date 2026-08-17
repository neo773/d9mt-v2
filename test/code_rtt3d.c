#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
};
static const unsigned char shadertri_ps_bytecode[] = {
#include "shadertri_ps_bytecode.inc"
};
static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

#define LOG(...)  do { printf("[rtt] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { \
    HRESULT _hr = (call); \
    printf("[HR ] %-52s -> 0x%08lx %s\n", #call, (unsigned long)_hr, \
           SUCCEEDED(_hr) ? "OK" : "**FAIL**"); \
    fflush(stdout); \
    if (FAILED(_hr)) LOG("!!! FAILED at %s:%d hr=0x%08lx", __FILE__, __LINE__, (unsigned long)_hr); \
  } while (0)

struct CVert { float x, y, z; DWORD color; };
struct TVert { float x, y, z; float u, v; };

typedef float mat4[16];
static void m_identity(mat4 m){ for(int i=0;i<16;i++)m[i]=0; m[0]=m[5]=m[10]=m[15]=1; }
static void m_mul(mat4 o,const mat4 a,const mat4 b){ mat4 t;
  for(int r=0;r<4;r++)for(int c=0;c<4;c++){float s=0;for(int k=0;k<4;k++)s+=a[r*4+k]*b[k*4+c];t[r*4+c]=s;}
  for(int i=0;i<16;i++)o[i]=t[i]; }
static void m_rot_y(mat4 m,float a){ m_identity(m); float c=cosf(a),s=sinf(a); m[0]=c;m[2]=-s;m[8]=s;m[10]=c; }
static void m_rot_x(mat4 m,float a){ m_identity(m); float c=cosf(a),s=sinf(a); m[5]=c;m[6]=s;m[9]=-s;m[10]=c; }
static void m_translate(mat4 m,float x,float y,float z){ m_identity(m); m[12]=x;m[13]=y;m[14]=z; }
static void m_persp(mat4 m,float fovy,float aspect,float zn,float zf){ for(int i=0;i<16;i++)m[i]=0;
  float ys=1.0f/tanf(fovy*0.5f), xs=ys/aspect; m[0]=xs;m[5]=ys;m[10]=zf/(zf-zn);m[11]=1;m[14]=-zn*zf/(zf-zn); }
static void transpose(mat4 m){ mat4 t; for(int r=0;r<4;r++)for(int c=0;c<4;c++)t[c*4+r]=m[r*4+c]; memcpy(m,t,sizeof(t)); }

static DWORD read_px(IDirect3DDevice9 *dev, UINT x, UINT y) {
  IDirect3DSurface9 *rt=NULL,*sys=NULL; D3DSURFACE_DESC d; D3DLOCKED_RECT lr; DWORD c=0xdeadbeef;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev,0,&rt))) return c;
  IDirect3DSurface9_GetDesc(rt,&d);
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev,d.Width,d.Height,d.Format,D3DPOOL_SYSTEMMEM,&sys,NULL))) {
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev,rt,sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys,&lr,NULL,D3DLOCK_READONLY))) {
      c = ((DWORD*)((BYTE*)lr.pBits + y*lr.Pitch))[x] & 0x00ffffff;
      IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
  }
  IDirect3DSurface9_Release(rt);
  return c;
}

enum { W = 1504, H = 924, RT = 512, FRAMES = 1000000 };

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_rtt3d"; RegisterClassA(&wc);
  RECT rc = {0,0,W,H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_rtt3d","d9mt rtt3d", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                            CW_USEDEFAULT,CW_USEDEFAULT, rc.right-rc.left, rc.bottom-rc.top, NULL,NULL,wc.hInstance,NULL);
  if (!hwnd) { LOG("FAIL CreateWindow"); return 1; }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { LOG("FAIL Direct3DCreate9"); return 1; }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth=W; pp.BackBufferHeight=H; pp.BackBufferFormat=D3DFMT_X8R8G8B8;
  pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=hwnd; pp.Windowed=TRUE;
  pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D24S8;
  pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
  if (!dev) { LOG("FAIL device"); return 1; }
  LOG("device %dx%d, RT texture %dx%d", W, H, RT, RT);

  IDirect3DSurface9 *backRT=NULL, *backDepth=NULL;
  CHECK(IDirect3DDevice9_GetRenderTarget(dev, 0, &backRT));
  CHECK(IDirect3DDevice9_GetDepthStencilSurface(dev, &backDepth));

  IDirect3DTexture9 *rtTex=NULL; IDirect3DSurface9 *rtSurf=NULL, *rtDepth=NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev, RT, RT, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex, NULL));
  CHECK(IDirect3DTexture9_GetSurfaceLevel(rtTex, 0, &rtSurf));
  CHECK(IDirect3DDevice9_CreateDepthStencilSurface(dev, RT, RT, D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &rtDepth, NULL));

  IDirect3DVertexShader9 *cvs=NULL,*qvs=NULL; IDirect3DPixelShader9 *cps=NULL,*qps=NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(dev,(const DWORD*)shadertri_vs_bytecode,&cvs));
  CHECK(IDirect3DDevice9_CreatePixelShader (dev,(const DWORD*)shadertri_ps_bytecode,&cps));
  CHECK(IDirect3DDevice9_CreateVertexShader(dev,(const DWORD*)texquad_vs_bytecode,&qvs));
  CHECK(IDirect3DDevice9_CreatePixelShader (dev,(const DWORD*)texquad_ps_bytecode,&qps));

  static const D3DVERTEXELEMENT9 cdsc[] = {
    {0,0, D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,0},
    {0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,   0}, D3DDECL_END()};
  static const D3DVERTEXELEMENT9 tdsc[] = {
    {0,0, D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,0},
    {0,12,D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END()};
  IDirect3DVertexDeclaration9 *cvd=NULL,*tvd=NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, cdsc, &cvd));
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, tdsc, &tvd));

  static const struct CVert cube[8] = {
    {-0.5f,-0.5f,-0.5f,0xFFFF0000},{ 0.5f,-0.5f,-0.5f,0xFF00FF00},
    { 0.5f, 0.5f,-0.5f,0xFF0000FF},{-0.5f, 0.5f,-0.5f,0xFFFFFF00},
    {-0.5f,-0.5f, 0.5f,0xFFFF00FF},{ 0.5f,-0.5f, 0.5f,0xFF00FFFF},
    { 0.5f, 0.5f, 0.5f,0xFFFFFFFF},{-0.5f, 0.5f, 0.5f,0xFF808080}};
  static const WORD cidx[36] = {0,1,2,0,2,3, 4,6,5,4,7,6, 0,4,5,0,5,1, 3,2,6,3,6,7, 1,5,6,1,6,2, 0,3,7,0,7,4};
  static const struct TVert quad[4] = {
    {-1,-1,0, 0,1},{ 1,-1,0, 1,1},{ 1, 1,0, 1,0},{-1, 1,0, 0,0}};
  static const WORD qidx[6] = {0,1,2, 0,2,3};

  IDirect3DVertexBuffer9 *cvb=NULL,*qvb=NULL; IDirect3DIndexBuffer9 *cib=NULL,*qib=NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev,sizeof(cube),0,0,D3DPOOL_DEFAULT,&cvb,NULL));
  CHECK(IDirect3DDevice9_CreateIndexBuffer (dev,sizeof(cidx),0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&cib,NULL));
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev,sizeof(quad),0,0,D3DPOOL_DEFAULT,&qvb,NULL));
  CHECK(IDirect3DDevice9_CreateIndexBuffer (dev,sizeof(qidx),0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&qib,NULL));
  #define FILL(buf,src,ty) { void*p=NULL; ty##_Lock(buf,0,sizeof(src),&p,0); memcpy(p,src,sizeof(src)); ty##_Unlock(buf); }
  FILL(cvb,cube,IDirect3DVertexBuffer9); FILL(cib,cidx,IDirect3DIndexBuffer9);
  FILL(qvb,quad,IDirect3DVertexBuffer9); FILL(qib,qidx,IDirect3DIndexBuffer9);

  mat4 proj,view; m_persp(proj,3.14159265f/3.0f,1.0f,0.5f,20.0f); m_translate(view,0,0,3);
  static const float white[4]={1,1,1,1};
  static const float ident[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  DWORD prev=0xdeadbeef; int pv=0,pc=0,pver=0; int nosample = getenv("NOSAMPLE")?1:0;

  for (int f=0; f<FRAMES; f++) {
    mat4 rx,ry,world,wv,mvp;
    float a=(getenv("FREEZE")?(float)atoi(getenv("FREEZE")):(float)f)*0.03f;
    m_rot_y(ry,a); m_rot_x(rx,a*0.5f); m_mul(world,rx,ry); m_mul(wv,world,view); m_mul(mvp,wv,proj);
    if (!getenv("NOTR")) transpose(mvp);

    if (!getenv("KEEPTEX"))
      IDirect3DDevice9_SetTexture(dev, 0, NULL); /* avoid RT read/write feedback loop */
    CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, rtSurf));
    CHECK(IDirect3DDevice9_SetDepthStencilSurface(dev, rtDepth));
    CHECK(IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,D3DCOLOR_XRGB(30,10,10),1.0f,0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    IDirect3DDevice9_SetVertexDeclaration(dev,cvd);
    IDirect3DDevice9_SetVertexShader(dev,cvs); IDirect3DDevice9_SetPixelShader(dev,cps);
    IDirect3DDevice9_SetPixelShaderConstantF(dev,0,white,1);
    IDirect3DDevice9_SetVertexShaderConstantF(dev,0,mvp,4);
    IDirect3DDevice9_SetStreamSource(dev,0,cvb,0,sizeof(struct CVert));
    IDirect3DDevice9_SetIndices(dev,cib);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);
    CHECK(IDirect3DDevice9_DrawIndexedPrimitive(dev,D3DPT_TRIANGLELIST,0,0,8,0,12));
    CHECK(IDirect3DDevice9_EndScene(dev));

    CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, backRT));
    CHECK(IDirect3DDevice9_SetDepthStencilSurface(dev, backDepth));

    CHECK(IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,D3DCOLOR_XRGB(16,16,40),1.0f,0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    if (!nosample) {
      IDirect3DDevice9_SetVertexDeclaration(dev,tvd);
      IDirect3DDevice9_SetVertexShader(dev,qvs); IDirect3DDevice9_SetPixelShader(dev,qps);
      IDirect3DDevice9_SetVertexShaderConstantF(dev,0,ident,4);
      IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9*)rtTex);
      IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
      IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
      IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
      IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,D3DZB_FALSE);
      IDirect3DDevice9_SetStreamSource(dev,0,qvb,0,sizeof(struct TVert));
      IDirect3DDevice9_SetIndices(dev,qib);
      CHECK(IDirect3DDevice9_DrawIndexedPrimitive(dev,D3DPT_TRIANGLELIST,0,0,4,0,2));
    }
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_Present(dev,NULL,NULL,NULL,NULL));

    if (getenv("NORB")) continue;

    DWORD bg = nosample ? 0x1e0a0a : 0x101028;
    DWORD c  = read_px(dev, W/2,     H/2);
    DWORD s1 = read_px(dev, W/2-160, H/2-160);
    DWORD s2 = read_px(dev, W/2+160, H/2+160);
    int visible=(c!=bg && c!=0xdeadbeef), changed=(f>0 && c!=prev);
    DWORD sm[3]={c,s1,s2}; int on=0,dist=0;
    for(int i=0;i<3;i++) if(sm[i]!=bg&&sm[i]!=0xdeadbeef) on++;
    for(int i=0;i<3;i++)for(int j=i+1;j<3;j++) if(sm[i]!=sm[j]&&sm[i]!=bg&&sm[j]!=bg) dist=1;
    if(visible)pv++; if(changed)pc++; if(dist)pver++;
    if (f<8 || (f%60)==0)
      LOG("f%-5d center=0x%06lx s=[%06lx %06lx] on=%d/3 varied=%d changed=%d",
          f,(unsigned long)c,(unsigned long)s1,(unsigned long)s2,on,dist,changed);
    prev=c;
    if (f==240) {
      LOG("======== SUMMARY (240 frames, %s) ========", nosample?"NOSAMPLE=read RT-composite-skipped":"full RTT");
      LOG("visible=%d varied=%d changed=%d", pv,pver,pc);
      LOG("RESULT: %s", (pv>200 && pver>100 && pc>100) ? "PASS (RTT renders + samples + moves)" : "FAIL");
    }
  }
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d); return 0;
}
