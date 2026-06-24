/* d9mt v2 stencil test: pass 1 draws a red triangle that writes stencil=1 in
 * its pixels; pass 2 draws a green full-screen quad that only passes where
 * stencil==1. Correct stencil masking shows a GREEN TRIANGLE (the quad clipped
 * to the triangle's stencil mask); if stencil state is ignored the whole quad
 * passes and the screen is fully green.
 *
 * Uses the vertex-colour shader (POSITION + D3DCOLOR, output = colour * tint)
 * so no textures are needed; depth test is disabled to isolate the stencil.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
};
static const unsigned char shadertri_ps_bytecode[] = {
#include "shadertri_ps_bytecode.inc"
};

struct Vertex { float x, y, z; DWORD color; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_stencil";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_stencil", "d9mt stencil", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)shadertri_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)shadertri_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  /* Mask triangle (red) and a full-screen quad (green). */
  static const struct Vertex tri[3] = {
      {0.0f, 0.8f, 0.5f, 0xFFFF0000}, {0.8f, -0.8f, 0.5f, 0xFFFF0000},
      {-0.8f, -0.8f, 0.5f, 0xFFFF0000}};
  static const struct Vertex quad[6] = {
      {-1, 1, 0.5f, 0xFF00FF00}, {1, 1, 0.5f, 0xFF00FF00}, {1, -1, 0.5f, 0xFF00FF00},
      {-1, 1, 0.5f, 0xFF00FF00}, {1, -1, 0.5f, 0xFF00FF00}, {-1, -1, 0.5f, 0xFF00FF00}};
  IDirect3DVertexBuffer9 *triVb = NULL, *quadVb = NULL;
  void *p = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(tri), 0, 0, D3DPOOL_DEFAULT, &triVb, NULL);
  IDirect3DVertexBuffer9_Lock(triVb, 0, sizeof(tri), &p, 0); memcpy(p, tri, sizeof(tri));
  IDirect3DVertexBuffer9_Unlock(triVb);
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(quad), 0, 0, D3DPOOL_DEFAULT, &quadVb, NULL);
  IDirect3DVertexBuffer9_Lock(quadVb, 0, sizeof(quad), &p, 0); memcpy(p, quad, sizeof(quad));
  IDirect3DVertexBuffer9_Unlock(quadVb);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, TRUE);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_STENCIL,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    /* Pass 1: write stencil=1 in the triangle. */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILREF, 1);
    IDirect3DDevice9_SetStreamSource(dev, 0, triVb, 0, sizeof(struct Vertex));
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);

    /* Pass 2: draw the green quad only where stencil==1. */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILREF, 1);
    IDirect3DDevice9_SetStreamSource(dev, 0, quadVb, 0, sizeof(struct Vertex));
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
