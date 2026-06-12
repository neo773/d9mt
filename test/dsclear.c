/* dsclear: proves partial single-aspect depth-stencil clears (the GTA IV
 * black-world case). Reuses the shadertri SM3 shaders (pos+color).
 *
 * Per frame, on a D24S8 auto depth-stencil:
 *   1. full Clear(TARGET | ZBUFFER, z = 1.0)
 *   2. draw a big NEAR red triangle (z = 0.3) -> depth buffer holds 0.3
 *   3. Clear(D3DCLEAR_ZBUFFER only, z = 1.0) with a sub-rect
 *      -> partial DEPTH-aspect-only clearImageView in the backend
 *   4. draw the SAME triangle FAR and green (z = 0.7)
 *
 * With LESSEQUAL depth testing, green (0.7) only passes where the rect
 * reset depth back to 1.0; everywhere else red (0.3) keeps it out. Pixel
 * readback verifies: inside the rect -> green, outside the rect -> red.
 *
 * Results go to dsclear_out.txt. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "shadertri_vs_bytecode.h"
#include "shadertri_ps_bytecode.h"

static FILE *g_out;
#define LOG(...)                                                               \
  do {                                                                         \
    fprintf(g_out, __VA_ARGS__);                                               \
    fputc('\n', g_out);                                                        \
    fflush(g_out);                                                             \
  } while (0)
#define CHECK(expr)                                                            \
  do {                                                                         \
    HRESULT hr_ = (expr);                                                      \
    if (FAILED(hr_)) {                                                         \
      LOG("FAIL: %s -> 0x%08lx", #expr, (unsigned long)hr_);                   \
      return 1;                                                                \
    }                                                                          \
    LOG("ok: %s", #expr);                                                      \
  } while (0)

struct Vertex {
  float x, y, z;
  DWORD color;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  g_out = fopen("dsclear_out.txt", "w");
  if (!g_out)
    return 1;

  const UINT W = 800, H = 600;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "dsclear";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("dsclear", "d9mt dsclear",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, W, H,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    LOG("FAIL: CreateWindow");
    return 1;
  }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    LOG("FAIL: Direct3DCreate9");
    return 1;
  }
  LOG("ok: Direct3DCreate9");

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(
      dev, (const DWORD *)shadertri_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(
      dev, (const DWORD *)shadertri_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,
       0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  /* the SAME big triangle twice: near red (z=0.3), then far green (z=0.7).
   * screen-space vertices: (160,120) (640,120) (400,480) */
  static const struct Vertex verts[6] = {
      {-0.6f, 0.6f, 0.3f, 0xFFFF0000},
      {0.6f, 0.6f, 0.3f, 0xFFFF0000},
      {0.0f, -0.6f, 0.3f, 0xFFFF0000},
      {-0.6f, 0.6f, 0.7f, 0xFF00FF00},
      {0.6f, 0.6f, 0.7f, 0xFF00FF00},
      {0.0f, -0.6f, 0.7f, 0xFF00FF00},
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                            D3DPOOL_DEFAULT, &vb, NULL));
  void *p = NULL;
  CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0));
  memcpy(p, verts, sizeof(verts));
  CHECK(IDirect3DVertexBuffer9_Unlock(vb));

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  static const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4));
  CHECK(IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1));

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, decl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));

  /* depth-only clear rect (left, top, right, bottom) */
  static const D3DRECT clearRect = {350, 150, 450, 350};

  LOG("entering render loop");
  int frames = 0, ok = 0, verified = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    /* near red */
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
    /* partial DEPTH-ONLY mid-frame clear back to 1.0 (the path under test:
     * D24S8 has a stencil aspect, so this is a single-aspect partial clear) */
    IDirect3DDevice9_Clear(dev, 1, &clearRect, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    /* far green: passes only inside the cleared rect */
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 3, 1);
    IDirect3DDevice9_EndScene(dev);

    if (++frames == 5 && !verified) {
      verified = 1;

      IDirect3DSurface9 *bb = NULL;
      HRESULT hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                                  D3DBACKBUFFER_TYPE_MONO, &bb);
      if (FAILED(hr)) {
        LOG("FAIL: GetBackBuffer 0x%08lx", (unsigned long)hr);
        goto done;
      }
      hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem);
      IDirect3DSurface9_Release(bb);
      if (FAILED(hr)) {
        LOG("FAIL: GetRenderTargetData 0x%08lx", (unsigned long)hr);
        goto done;
      }

      D3DLOCKED_RECT lr;
      hr = IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
      if (FAILED(hr)) {
        LOG("FAIL: LockRect 0x%08lx", (unsigned long)hr);
        goto done;
      }

      const BYTE *base = (const BYTE *)lr.pBits;
      /* (400,250): inside triangle AND inside rect -> green wins */
      DWORD inside =
          *(const DWORD *)(base + 250 * lr.Pitch + 400 * 4) & 0xFFFFFF;
      /* (300,250): inside triangle, OUTSIDE rect -> red stays */
      DWORD outside =
          *(const DWORD *)(base + 250 * lr.Pitch + 300 * 4) & 0xFFFFFF;
      /* (8,8): background -> clear color */
      DWORD corner = *(const DWORD *)(base + 8 * lr.Pitch + 8 * 4) & 0xFFFFFF;
      IDirect3DSurface9_UnlockRect(sysmem);

      LOG("inside=%06lx outside=%06lx corner=%06lx", (unsigned long)inside,
          (unsigned long)outside, (unsigned long)corner);

      int insideOk = inside == 0x00FF00;
      int outsideOk = outside == 0xFF0000;
      int cornerOk = corner == 0x101040;
      ok = insideOk && outsideOk && cornerOk;
      LOG(ok ? "PASS: partial depth-only clear verified by readback"
             : "FAIL: pixel mismatch (inside %s, outside %s, corner %s)",
          insideOk ? "ok" : "bad", outsideOk ? "ok" : "bad",
          cornerOk ? "ok" : "bad");
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
done:
  LOG("exiting after %d frames", frames);
  return ok ? 0 : 1;
}
