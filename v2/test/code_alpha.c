/* d9mt v2 alpha-test test: a quad textured with a 2x1 texture — left texel
 * opaque (alpha 255), right texel transparent (alpha 0) — drawn with
 * ALPHATESTENABLE + ALPHAFUNC GREATER and ALPHAREF=128. Correct alpha test
 * keeps the left half (alpha>0.5) and discards the right; if alpha test is
 * ignored the whole quad shows, and if it over-discards nothing shows.
 *
 * Uses a sampled texture (not vertex alpha) so the output alpha is well-defined
 * regardless of the vertex-colour shader's alpha handling.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
/* Explicit ps_3_0 that samples s0 and outputs the full sampled RGBA, so the
 * output alpha is exactly the texture alpha (assembled from test/shaders). */
static const unsigned char texquad_ps_bytecode[] = {
#include "alpha_ps_bytecode.inc"
};

struct Vertex { float x, y, z, u, v; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_alpha";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_alpha", "d9mt alpha test",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  static const struct Vertex verts[6] = {
      {-0.7f, 0.7f, 0.5f, 0.0f, 0.0f},  {0.7f, 0.7f, 0.5f, 1.0f, 0.0f},
      {0.7f, -0.7f, 0.5f, 1.0f, 1.0f},  {-0.7f, 0.7f, 0.5f, 0.0f, 0.0f},
      {0.7f, -0.7f, 0.5f, 1.0f, 1.0f},  {-0.7f, -0.7f, 0.5f, 0.0f, 1.0f}};
  IDirect3DVertexBuffer9 *vb = NULL;
  void *p = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0); memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  /* 2x1 texture: left opaque white, right transparent white (A8R8G8B8). */
  IDirect3DTexture9 *tex = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 2, 1, 1, 0, D3DFMT_A8R8G8B8,
                                            D3DPOOL_MANAGED, &tex, NULL)))
    return 1;
  D3DLOCKED_RECT lr;
  if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
    DWORD *px = (DWORD *)lr.pBits;
    px[0] = 0xFFFFFFFF; /* opaque white */
    px[1] = 0x00FFFFFF; /* transparent white */
    IDirect3DTexture9_UnlockRect(tex, 0);
  }

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, 128);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
