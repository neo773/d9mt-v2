/* d9mt v2 — minimal FIXED-FUNCTION test (no shaders at all).
 *
 * Exercises the FF pipeline the driver just gained: FVF XYZRHW|DIFFUSE|TEX1,
 * a pre-transformed (POSITIONT) screen-space quad, sampled from a procedural
 * texture and modulated by the diffuse color — exactly the shape GTA-style 2D
 * UI uses. No SetVertexShader/SetPixelShader => the driver's FF ubershaders run.
 *
 * Draws one full-window quad at known screen coords; reads back the center and
 * a few points; asserts it's textured (non-bg, spatially varied).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define LOG(...)  do { printf("[ff] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { HRESULT _hr=(call); \
    printf("[HR ] %-46s -> 0x%08lx %s\n", #call,(unsigned long)_hr, SUCCEEDED(_hr)?"OK":"**FAIL**"); \
    fflush(stdout); } while (0)

enum { W = 640, H = 480, TEX = 256 };

/* D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 */
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
struct V { float x, y, z, rhw; DWORD color; float u, v; };

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

static void ident(D3DMATRIX *m){ memset(m,0,sizeof(*m)); m->m[0][0]=m->m[1][1]=m->m[2][2]=m->m[3][3]=1.f; }

/* Case B: LIT, non-POSITIONT (XYZ|NORMAL|DIFFUSE|TEX1). Exercises the FF VS
 * world/view/proj transform + directional lighting path (the branches the
 * POSITIONT quad skips), plus texture*litColor modulation. */
static int run_lit(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex) {
#define LFVF (D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1)
  struct LV { float x,y,z, nx,ny,nz; DWORD color; float u,v; };
  float e=0.8f, z=0.5f;
  struct LV q[4]={
    {-e, e, z, 0,0,-1, 0xFFFFFFFF, 0,0},
    { e, e, z, 0,0,-1, 0xFFFFFFFF, 1,0},
    {-e,-e, z, 0,0,-1, 0xFFFFFFFF, 0,1},
    { e,-e, z, 0,0,-1, 0xFFFFFFFF, 1,1},
  };
  D3DMATRIX I; ident(&I);
  IDirect3DDevice9_SetTransform(dev,D3DTS_WORLD,&I);
  IDirect3DDevice9_SetTransform(dev,D3DTS_VIEW,&I);
  IDirect3DDevice9_SetTransform(dev,D3DTS_PROJECTION,&I);   /* identity WVP: xyz already in NDC */

  D3DMATERIAL9 mat; memset(&mat,0,sizeof mat);
  mat.Diffuse.r=mat.Diffuse.g=mat.Diffuse.b=mat.Diffuse.a=1.f;
  mat.Ambient.r=mat.Ambient.g=mat.Ambient.b=mat.Ambient.a=1.f;
  IDirect3DDevice9_SetMaterial(dev,&mat);

  D3DLIGHT9 light; memset(&light,0,sizeof light);
  light.Type=D3DLIGHT_DIRECTIONAL;
  light.Diffuse.r=light.Diffuse.g=light.Diffuse.b=1.f;
  light.Direction.x=0; light.Direction.y=0; light.Direction.z=1.f; /* travels +z; N=(0,0,-1) faces it */
  IDirect3DDevice9_SetLight(dev,0,&light);
  IDirect3DDevice9_LightEnable(dev,0,TRUE);

  IDirect3DDevice9_SetFVF(dev,LFVF);
  IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9*)tex);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,TRUE);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_AMBIENT,0x00303030);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);

  int pass=0; DWORD bg=0x101028;
  for(int f=0; f<4; f++){
    IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,16,40),1.f,0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,q,sizeof(struct LV));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev,NULL,NULL,NULL,NULL);
    DWORD c =read_px(dev,W/2,H/2);
    DWORD tl=read_px(dev,W/2-180,H/2-160);
    DWORD br=read_px(dev,W/2+180,H/2+160);
    DWORD out=read_px(dev,6,6);
    int textured=(c!=bg&&c!=0xdeadbeef), varied=(tl!=br&&tl!=bg&&br!=bg), bgok=(out==bg);
    LOG("B f%d center=%06lx tl=%06lx br=%06lx out=%06lx textured=%d varied=%d bgok=%d",
        f,(unsigned long)c,(unsigned long)tl,(unsigned long)br,(unsigned long)out,textured,varied,bgok);
    if(textured&&varied&&bgok) pass++;
    Sleep(60);
  }
  IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);
  int ok = pass>=3;
  LOG("RESULT-B: %s (%d/4) lit non-POSITIONT quad", ok?"PASS":"FAIL", pass);
  return ok;
}

/* Case C: multi-stage texturing (2 stages). Stage0 = tex*diffuse, stage1 =
 * tex2*current. Exercises the FS texture-stage-op spec constants + a second
 * bound texture/sampler + a second texcoord set. */
static int run_multistage(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex) {
#define MFVF (D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX2)
  struct MV { float x,y,z,rhw; DWORD color; float u0,v0, u1,v1; };
  /* second texture: horizontal red gradient so stage1 visibly modulates */
  IDirect3DTexture9 *tex2=NULL;
  if(FAILED(IDirect3DDevice9_CreateTexture(dev,TEX,TEX,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex2,NULL)))
    { LOG("RESULT-C: FAIL (CreateTexture)"); return 0; }
  { D3DLOCKED_RECT lr; IDirect3DTexture9_LockRect(tex2,0,&lr,NULL,0);
    for(int ty=0;ty<TEX;ty++){ DWORD*row=(DWORD*)((BYTE*)lr.pBits+ty*lr.Pitch);
      for(int tx=0;tx<TEX;tx++){ BYTE g=(BYTE)tx; row[tx]=0xFF000000u|((DWORD)g<<8)|0x40; } }
    IDirect3DTexture9_UnlockRect(tex2,0); }

  float x0=80.f,y0=60.f,x1=560.f,y1=420.f;
  struct MV q[4]={
    {x0,y0,0,1, 0xFFFFFFFF, 0,0, 0,0},
    {x1,y0,0,1, 0xFFFFFFFF, 1,0, 1,0},
    {x0,y1,0,1, 0xFFFFFFFF, 0,1, 0,1},
    {x1,y1,0,1, 0xFFFFFFFF, 1,1, 1,1},
  };
  IDirect3DDevice9_SetFVF(dev,MFVF);
  IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9*)tex);
  IDirect3DDevice9_SetTexture(dev,1,(IDirect3DBaseTexture9*)tex2);
  /* stage 0: tex0 * diffuse */
  IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLOROP,D3DTOP_MODULATE);
  IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
  IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_TEXCOORDINDEX,0);
  /* stage 1: tex1 * current */
  IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLOROP,D3DTOP_MODULATE);
  IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLORARG1,D3DTA_TEXTURE);
  IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLORARG2,D3DTA_CURRENT);
  IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_TEXCOORDINDEX,1);
  IDirect3DDevice9_SetTextureStageState(dev,2,D3DTSS_COLOROP,D3DTOP_DISABLE);
  IDirect3DDevice9_SetSamplerState(dev,1,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
  IDirect3DDevice9_SetSamplerState(dev,1,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);

  int pass=0; DWORD bg=0x101028;
  for(int f=0; f<4; f++){
    IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,16,40),1.f,0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,q,sizeof(struct MV));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev,NULL,NULL,NULL,NULL);
    DWORD tl=read_px(dev,x0+20,y0+20);   /* u≈0: tex2 red≈0 -> dark */
    DWORD br=read_px(dev,x1-20,y1-20);   /* u≈1: tex2 red≈255 -> bright red */
    DWORD c =read_px(dev,W/2,H/2);
    DWORD out=read_px(dev,6,6);
    int textured=(c!=bg&&c!=0xdeadbeef), varied=(tl!=br&&tl!=bg&&br!=bg), bgok=(out==bg);
    /* stage1's green gradient (tex2.G = u*255) makes the right edge greener */
    int grnGrad = ((br&0xff00) > (tl&0xff00));
    LOG("C f%d center=%06lx tl=%06lx br=%06lx out=%06lx textured=%d varied=%d bgok=%d grnGrad=%d",
        f,(unsigned long)c,(unsigned long)tl,(unsigned long)br,(unsigned long)out,textured,varied,bgok,grnGrad);
    if(textured&&varied&&bgok&&grnGrad) pass++;
    Sleep(60);
  }
  IDirect3DDevice9_SetTexture(dev,1,NULL);
  IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLOROP,D3DTOP_DISABLE);
  IDirect3DTexture9_Release(tex2);
  int ok = pass>=3;
  LOG("RESULT-C: %s (%d/4) multi-stage texturing", ok?"PASS":"FAIL", pass);
  return ok;
}

int main(void) {
  WNDCLASSA wc = {0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL);
  wc.lpszClassName="d9mt_ff"; RegisterClassA(&wc);
  RECT rc={0,0,W,H}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,FALSE);
  HWND hwnd=CreateWindowA("d9mt_ff","d9mt ff",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
    CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,NULL,NULL,wc.hInstance,NULL);
  IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp={0};
  pp.BackBufferWidth=W; pp.BackBufferHeight=H; pp.BackBufferFormat=D3DFMT_X8R8G8B8;
  pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=hwnd; pp.Windowed=TRUE;
  pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev=NULL;
  CHECK(IDirect3D9_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev));
  if(!dev){LOG("no device");return 1;}
  LOG("device %dx%d, FIXED FUNCTION (no shaders)", W, H);

  /* procedural texture: R=u, G=v, B=checker */
  IDirect3DTexture9 *tex=NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev,TEX,TEX,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,NULL));
  { D3DLOCKED_RECT lr; IDirect3DTexture9_LockRect(tex,0,&lr,NULL,0);
    for(int ty=0;ty<TEX;ty++){ DWORD*row=(DWORD*)((BYTE*)lr.pBits+ty*lr.Pitch);
      for(int tx=0;tx<TEX;tx++){ BYTE b=(((tx>>5)^(ty>>5))&1)?240:40;
        row[tx]=0xFF000000u|((DWORD)tx<<16)|((DWORD)ty<<8)|b; } }
    IDirect3DTexture9_UnlockRect(tex,0); }

  /* XYZRHW quad covering most of the window; rhw=1; white diffuse; uv 0..1 */
  float x0=80.f,y0=60.f,x1=560.f,y1=420.f;
  struct V q[4]={
    {x0,y0,0,1, 0xFFFFFFFF, 0,0},
    {x1,y0,0,1, 0xFFFFFFFF, 1,0},
    {x0,y1,0,1, 0xFFFFFFFF, 0,1},
    {x1,y1,0,1, 0xFFFFFFFF, 1,1},
  };

  CHECK(IDirect3DDevice9_SetFVF(dev, FVF));
  CHECK(IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9*)tex));
  IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
  IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);

  int pass=0;
  for(int f=0; f<6; f++){
    CHECK(IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,16,40),1.f,0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,q,sizeof(struct V)));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_Present(dev,NULL,NULL,NULL,NULL));

    DWORD bg=0x101028;
    DWORD c =read_px(dev,W/2,H/2);           /* center of quad: mid texture */
    DWORD tl=read_px(dev,x0+20,y0+20);       /* near uv(0,0): low R,G */
    DWORD br=read_px(dev,x1-20,y1-20);       /* near uv(1,1): high R,G */
    DWORD out=read_px(dev,10,10);            /* outside quad: bg */
    int textured = (c!=bg && c!=0xdeadbeef);
    int varied   = (tl!=br && tl!=bg && br!=bg);
    int bgok     = (out==bg);
    LOG("A f%d center=%06lx tl=%06lx br=%06lx out=%06lx  textured=%d varied=%d bgok=%d",
        f,(unsigned long)c,(unsigned long)tl,(unsigned long)br,(unsigned long)out,textured,varied,bgok);
    if(textured&&varied&&bgok) pass++;
    Sleep(60);
  }
  int passA = pass>=5;
  LOG("RESULT-A: %s (%d/6) POSITIONT textured quad", passA?"PASS":"FAIL", pass);

  int passB = run_lit(dev, tex);
  int passC = run_multistage(dev, tex);

  int overall = passA && passB && passC;
  LOG("RESULT: %s  (A=%d B=%d C=%d)", overall?"PASS":"FAIL", passA, passB, passC);
  IDirect3DTexture9_Release(tex);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
