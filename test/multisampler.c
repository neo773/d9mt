/* multisampler: proves PS sampler slots s0..s7 map to the right texture AND
 * the right sampler object (heap index) simultaneously.
 *
 * Each slot gets a 2x1 texture: left texel red+slotmark, right texel
 * blue+slotmark (mark = green channel encodes the slot). All vertices carry
 * UV (-0.25, 0.5); even slots use ADDRESSU=CLAMP (-0.25 -> left texel), odd
 * slots use WRAP (-0.25 -> 0.75 -> right texel). Eight column draws with
 * one-hot PS weights probe one slot each; readback verifies:
 *   green  == 16*slot+8  -> texture slot mapping correct
 *   red/blue selection   -> per-slot sampler object (address mode) correct
 *
 * CX winewrapper swallows console output: results go to multisampler_out.txt.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "multisampler_vs_bytecode.h"
#include "multisampler_ps_bytecode.h"

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
  float u, v;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static int near8(int got, int want, int tol) {
  int d = got - want;
  return d >= -tol && d <= tol;
}

int main(void) {
  const UINT W = 640, H = 400;

  g_out = fopen("multisampler_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "multisampler";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("multisampler", "d9mt multisampler",
                            WS_OVERLAPPEDWINDOW, 64, 64, W, H, NULL, NULL,
                            wc.hInstance, NULL);
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
      dev, (const DWORD *)multisampler_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(
      dev, (const DWORD *)multisampler_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,
       0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  /* 8 distinct 64x64 textures: [left half = red + mark, right = blue + mark] */
  IDirect3DTexture9 *tex[8];
  for (int i = 0; i < 8; i++) {
    CHECK(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &tex[i], NULL));
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DTexture9_LockRect(tex[i], 0, &lr, NULL, 0));
    DWORD mark = (DWORD)(16 * i + 8) << 8;
    for (int y = 0; y < 64; y++) {
      DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
      for (int x = 0; x < 64; x++)
        row[x] = (x < 32 ? 0xFFFF0000 : 0xFF0000FF) | mark;
    }
    CHECK(IDirect3DTexture9_UnlockRect(tex[i], 0));

    CHECK(IDirect3DDevice9_SetTexture(dev, i, (IDirect3DBaseTexture9 *)tex[i]));
    CHECK(IDirect3DDevice9_SetSamplerState(
        dev, i, D3DSAMP_ADDRESSU,
        (i & 1) ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
    CHECK(IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_ADDRESSV,
                                           D3DTADDRESS_CLAMP));
    CHECK(IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MAGFILTER,
                                           D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MINFILTER,
                                           D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MIPFILTER,
                                           D3DTEXF_NONE));
  }

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, decl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE));

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

    for (int d = 0; d < 8; d++) {
      /* one-hot weights: draw d probes slot d */
      float w[8] = {0};
      w[d] = 1.0f;
      IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, w, 2);

      float x0 = -1.0f + 2.0f * d / 8.0f;
      float x1 = -1.0f + 2.0f * (d + 1) / 8.0f;
      const float u = -0.25f, vv = 0.5f, z = 0.5f;
      struct Vertex v[6] = {
          {x0, 1.0f, z, u, vv},  {x1, 1.0f, z, u, vv}, {x1, -1.0f, z, u, vv},
          {x0, 1.0f, z, u, vv},  {x1, -1.0f, z, u, vv}, {x0, -1.0f, z, u, vv},
      };
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                       sizeof(struct Vertex));
    }

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

      ok = 1;
      for (int d = 0; d < 8; d++) {
        UINT x = (UINT)((2 * d + 1) * W / 16);
        DWORD p = *(const DWORD *)(base + (H / 2) * lr.Pitch + x * 4);
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;

        int wantMark = 16 * d + 8;
        int wantR = (d & 1) ? 0 : 255; /* even: CLAMP -> left (red)  */
        int wantB = (d & 1) ? 255 : 0; /* odd:  WRAP  -> right (blue) */

        int okSlot = near8(g, wantMark, 4);
        int okSamp = near8(r, wantR, 4) && near8(b, wantB, 4);
        if (!okSlot || !okSamp)
          ok = 0;

        LOG("slot %d: got %02x,%02x,%02x want %02x,%02x,%02x (texture %s, "
            "sampler %s)",
            d, r, g, b, wantR, wantMark, wantB, okSlot ? "ok" : "BAD",
            okSamp ? "ok" : "BAD");
      }
      IDirect3DSurface9_UnlockRect(sysmem);

      LOG(ok ? "PASS: all 8 sampler slots map to the right texture + sampler"
             : "FAIL: sampler slot mapping broken");
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (frames == 12)
      goto done;
  }

done:
  LOG("exit after %d frames", frames);
  return ok ? 0 : 1;
}
