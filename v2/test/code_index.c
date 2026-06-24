/* d9mt v2 indexed-draw test: the same SM3 programmable triangle as
 * code_shader.c, but drawn through an index buffer with DrawIndexedPrimitive.
 *
 * Exercises Phase 1 item 3 (index buffers + drawIndexed) on top of the
 * programmable-shader path. Geometry, shaders, declaration and constants are
 * identical to code_shader.c so any difference in output isolates the indexed
 * path. The index buffer is a trivial {0,1,2} so a correct drawIndexed must
 * produce the exact same triangle as the non-indexed DrawPrimitive.
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

struct Vertex {
  float x, y, z;
  DWORD color;
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_index";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_index", "d9mt indexed triangle",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd)
    return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800;
  pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(
      d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev)
    return 1;

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(
          dev, (const DWORD *)shadertri_vs_bytecode, &vs)))
    return 1;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(
          dev, (const DWORD *)shadertri_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  static const struct Vertex verts[3] = {
      {0.0f, 0.6f, 0.5f, 0xFFFF0000},   /* top, red */
      {0.6f, -0.6f, 0.5f, 0xFF00FF00},  /* right, green */
      {-0.6f, -0.6f, 0.5f, 0xFF0000FF}, /* left, blue */
  };
  static const WORD indices[3] = {0, 1, 2};

  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DIndexBuffer9 *ib = NULL;
  if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(indices), 0,
                                                D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                                &ib, NULL)))
    return 1;
  IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(indices), &p, 0);
  memcpy(p, indices, sizeof(indices));
  IDirect3DIndexBuffer9_Unlock(ib);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  static const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1);

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetIndices(dev, ib);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    IDirect3DDevice9_EndScene(dev);
    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    if (++frames == 3)
      printf("3 frames presented\n");
  }

done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
