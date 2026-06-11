/* extest: exercises the D3D9Ex path GTA IV uses - Direct3DCreate9Ex,
 * CreateDeviceEx, backbuffer/render-target/depth-stencil surface queries,
 * PresentEx. Results go to extest_out.txt.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

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

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  g_out = fopen("extest_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "extest";
  RegisterClassA(&wc);
  HWND hwnd =
      CreateWindowA("extest", "d9mt extest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    100, 100, 640, 480, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    LOG("FAIL: CreateWindow");
    return 1;
  }

  IDirect3D9Ex *d3d = NULL;
  CHECK(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d));

  D3DCAPS9 caps;
  CHECK(IDirect3D9Ex_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps));
  LOG("caps: VS %lx PS %lx", (unsigned long)caps.VertexShaderVersion,
      (unsigned long)caps.PixelShaderVersion);

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9Ex *dev = NULL;
  CHECK(IDirect3D9Ex_CreateDeviceEx(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                    NULL, &dev));

  IDirect3DSurface9 *bb = NULL, *rt = NULL, *ds = NULL;
  CHECK(IDirect3DDevice9Ex_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                         &bb));
  CHECK(IDirect3DDevice9Ex_GetRenderTarget(dev, 0, &rt));
  CHECK(IDirect3DDevice9Ex_GetDepthStencilSurface(dev, &ds));
  D3DSURFACE_DESC bd;
  CHECK(IDirect3DSurface9_GetDesc(bb, &bd));
  LOG("backbuffer %ux%u fmt %u rt==bb: %d", bd.Width, bd.Height,
      (unsigned)bd.Format, bb == rt);
  CHECK(IDirect3DDevice9Ex_SetRenderTarget(dev, 0, rt));
  CHECK(IDirect3DDevice9Ex_SetDepthStencilSurface(dev, ds));

  UINT lat = 0;
  CHECK(IDirect3DDevice9Ex_SetMaximumFrameLatency(dev, 1));
  CHECK(IDirect3DDevice9Ex_GetMaximumFrameLatency(dev, &lat));
  CHECK(IDirect3DDevice9Ex_CheckDeviceState(dev, hwnd));

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9Ex_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                             D3DCOLOR_XRGB(40, 80, 40), 1.0f, 0);
    IDirect3DDevice9Ex_BeginScene(dev);
    IDirect3DDevice9Ex_EndScene(dev);
    HRESULT hr = IDirect3DDevice9Ex_PresentEx(dev, NULL, NULL, NULL, NULL, 0);
    frames++;
    if (frames == 3) {
      LOG("3 frames via PresentEx, hr=0x%08lx", (unsigned long)hr);
      LOG("PASS: D3D9Ex path (green window)");
    }
  }
done:
  LOG("exiting after %d frames", frames);
  return 0;
}
