/* d9mt MVP test: colored triangle through the real D3D9 API. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define FVF_TRI (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

struct Vertex {
  float x, y, z, rhw;
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
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = "d9mt_triangle";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_triangle", "d9mt triangle",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    fprintf(stderr, "CreateWindow failed\n");
    return 1;
  }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    fprintf(stderr, "Direct3DCreate9 failed\n");
    return 1;
  }
  printf("Direct3DCreate9 OK\n");

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
  if (FAILED(hr) || !dev) {
    fprintf(stderr, "CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
    return 1;
  }
  printf("CreateDevice OK\n");

  struct Vertex tri[3] = {
      {400.0f, 100.0f, 0.5f, 1.0f, 0xFFFF0000}, /* top: red */
      {700.0f, 500.0f, 0.5f, 1.0f, 0xFF00FF00}, /* right: green */
      {100.0f, 500.0f, 0.5f, 1.0f, 0xFF0000FF}, /* left: blue */
  };

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
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, FVF_TRI);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri,
                                     sizeof(struct Vertex));
    IDirect3DDevice9_EndScene(dev);
    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (FAILED(hr))
      fprintf(stderr, "Present failed: 0x%08lx\n", (unsigned long)hr);

    if (++frames == 3)
      printf("3 frames presented\n");
  }

done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  printf("exit after %d frames\n", frames);
  return 0;
}
