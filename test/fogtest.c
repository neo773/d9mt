/* fogtest: proves the D3D9 fog spec-constant + render_state push chain.
 * Three quads side by side, all drawn by a ps_2_0 shader returning solid
 * red, with fog color blue:
 *   left:   D3DRS_FOGENABLE = FALSE                  -> pure red
 *   middle: vertex fog (table NONE), oFog = 0.25     -> 0.25 red + 0.75 blue
 *   right:  table fog LINEAR start=0 end=1, z = 0.5  -> 0.50 red + 0.50 blue
 * Readback via GetRenderTargetData verifies all three regions, which fails
 * if the FogEnabled/PixelFogMode spec dwords don't reach the PSO or the
 * fogColor/fogScale/fogEnd push fields are mis-offset/stale.
 *
 * CX winewrapper swallows console output: results go to fogtest_out.txt. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "fogtest_vs_bytecode.h"
#include "fogtest_ps_bytecode.h"

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
  } while (0)

struct Vertex {
  float x, y, z;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static DWORD f2dw(float f) {
  union {
    float f;
    DWORD d;
  } u;
  u.f = f;
  return u.d;
}

/* quad covering x0..x1 in NDC, full height, z = 0.5 */
static void quad(struct Vertex *v, float x0, float x1) {
  const float z = 0.5f;
  v[0] = (struct Vertex){x0, 1.0f, z};
  v[1] = (struct Vertex){x1, 1.0f, z};
  v[2] = (struct Vertex){x1, -1.0f, z};
  v[3] = (struct Vertex){x0, 1.0f, z};
  v[4] = (struct Vertex){x1, -1.0f, z};
  v[5] = (struct Vertex){x0, -1.0f, z};
}

static int near8(int got, int want, int tol) {
  int d = got - want;
  return d >= -tol && d <= tol;
}

int main(void) {
  const UINT W = 600, H = 400;

  g_out = fopen("fogtest_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "fogtest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("fogtest", "d9mt fogtest", WS_OVERLAPPEDWINDOW,
                            64, 64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    LOG("FAIL: Direct3DCreate9");
    return 1;
  }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(
      dev, (const DWORD *)fogtest_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(
      dev, (const DWORD *)fogtest_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, decl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGCOLOR, 0x000000FF));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGSTART, f2dw(0.0f)));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGEND, f2dw(1.0f)));

  struct Vertex v[6];
  static const float fogFactor[4] = {0.25f, 0.0f, 0.0f, 0.0f};
  CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, fogFactor, 1));

  int frames = 0, verified = 0, ok = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(16, 16, 16), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    /* left third: fog disabled -> pure red */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    quad(v, -1.0f, -1.0f / 3.0f);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                     sizeof(struct Vertex));

    /* middle third: vertex fog (table NONE), oFog = 0.25 */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    quad(v, -1.0f / 3.0f, 1.0f / 3.0f);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                     sizeof(struct Vertex));

    /* right third: table fog LINEAR, depth 0.5 with start 0 end 1 */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    quad(v, 1.0f / 3.0f, 1.0f);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                     sizeof(struct Vertex));

    IDirect3DDevice9_EndScene(dev);

    if (++frames == 5 && !verified) {
      verified = 1;

      IDirect3DSurface9 *bb = NULL;
      CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                           &bb));
      HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem);
      IDirect3DSurface9_Release(bb);
      if (FAILED(hr)) {
        LOG("FAIL: GetRenderTargetData 0x%08lx", (unsigned long)hr);
        goto done;
      }

      D3DLOCKED_RECT lr;
      CHECK(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY));
      const BYTE *base = (const BYTE *)lr.pBits;

      /* sample the center of each third */
      DWORD pL = *(const DWORD *)(base + (H / 2) * lr.Pitch + (W / 6) * 4);
      DWORD pM = *(const DWORD *)(base + (H / 2) * lr.Pitch + (W / 2) * 4);
      DWORD pR = *(const DWORD *)(base + (H / 2) * lr.Pitch + (5 * W / 6) * 4);
      IDirect3DSurface9_UnlockRect(sysmem);

      LOG("left=%06lx middle=%06lx right=%06lx", (unsigned long)(pL & 0xFFFFFF),
          (unsigned long)(pM & 0xFFFFFF), (unsigned long)(pR & 0xFFFFFF));

      /* expected: left (255,0,0); middle mix(blue,red,0.25) = (64,0,191);
       * right mix(blue,red,0.5) = (128,0,128) */
      int okL = near8((pL >> 16) & 0xFF, 255, 4) && near8((pL >> 8) & 0xFF, 0, 4)
             && near8(pL & 0xFF, 0, 4);
      int okM = near8((pM >> 16) & 0xFF, 64, 12) && near8((pM >> 8) & 0xFF, 0, 4)
             && near8(pM & 0xFF, 191, 12);
      int okR = near8((pR >> 16) & 0xFF, 128, 12) && near8((pR >> 8) & 0xFF, 0, 4)
             && near8(pR & 0xFF, 128, 12);

      ok = okL && okM && okR;
      if (ok)
        LOG("PASS: fog disabled / vertex fog / table fog all correct");
      else
        LOG("FAIL: fog mismatch (disabled %s, vertex %s, table %s)",
            okL ? "ok" : "bad", okM ? "ok" : "bad", okR ? "ok" : "bad");
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (frames == 12)
      goto done;
  }

done:
  LOG("exit after %d frames", frames);
  return ok ? 0 : 1;
}
