/* verifytri: draws the fixed-function XYZRHW triangle and READS BACK the
 * render target (GetRenderTargetData -> LockRect) to verify pixels on the
 * CPU: background must be the clear color, the triangle centroid must be
 * the interpolated vertex-color mix. Prints PASS/FAIL; exits by itself. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
};
#define FVF_TRI (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  const UINT W = 640, H = 480;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "verifytri";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("verifytri", "verifytri", WS_OVERLAPPEDWINDOW,
                            64, 64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("FAIL: Direct3DCreate9\n");
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
  HRESULT hr = IDirect3D9_CreateDevice(
      d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    printf("FAIL: CreateDevice 0x%08lx\n", (unsigned long)hr);
    return 1;
  }

  /* large triangle covering the center of the target */
  struct Vertex tri[3] = {
      {320.0f, 40.0f, 0.5f, 1.0f, 0xFFFF0000},  /* top: red */
      {600.0f, 440.0f, 0.5f, 1.0f, 0xFF00FF00}, /* right: green */
      {40.0f, 440.0f, 0.5f, 1.0f, 0xFF0000FF},  /* left: blue */
  };

  IDirect3DSurface9 *sysmem = NULL;
  hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
  if (FAILED(hr)) {
    printf("FAIL: CreateOffscreenPlainSurface 0x%08lx\n", (unsigned long)hr);
    return 1;
  }

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
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, FVF_TRI);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri,
                                     sizeof(struct Vertex));
    IDirect3DDevice9_EndScene(dev);

    if (++frames == 5 && !verified) {
      verified = 1;

      IDirect3DSurface9 *bb = NULL;
      hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                          &bb);
      if (FAILED(hr)) {
        printf("FAIL: GetBackBuffer 0x%08lx\n", (unsigned long)hr);
        goto done;
      }
      hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem);
      IDirect3DSurface9_Release(bb);
      if (FAILED(hr)) {
        printf("FAIL: GetRenderTargetData 0x%08lx\n", (unsigned long)hr);
        goto done;
      }

      D3DLOCKED_RECT lr;
      hr = IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
      if (FAILED(hr)) {
        printf("FAIL: LockRect 0x%08lx\n", (unsigned long)hr);
        goto done;
      }

      const BYTE *base = (const BYTE *)lr.pBits;
      DWORD corner = *(const DWORD *)(base + 8 * lr.Pitch + 8 * 4) & 0xFFFFFF;
      /* centroid of the triangle: (320, 306) */
      DWORD center =
          *(const DWORD *)(base + 306 * lr.Pitch + 320 * 4) & 0xFFFFFF;
      IDirect3DSurface9_UnlockRect(sysmem);

      printf("corner=%06lx center=%06lx\n", (unsigned long)corner,
             (unsigned long)center);

      int cornerOk = corner == 0x101040; /* clear color 16,16,64 */
      int r = (center >> 16) & 0xFF, g = (center >> 8) & 0xFF,
          b = center & 0xFF;
      /* interpolated corner colors at the centroid: ~1/3 each (~85) */
      int centerOk = r > 50 && r < 120 && g > 50 && g < 120 && b > 50 &&
                     b < 120;

      ok = cornerOk && centerOk;
      printf(ok ? "PASS: triangle rendered and read back\n"
                : "FAIL: pixel mismatch (corner %s, center %s)\n",
             cornerOk ? "ok" : "bad", centerOk ? "ok" : "bad");
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (frames == 12)
      goto done;
  }

done:
  if (sysmem)
    IDirect3DSurface9_Release(sysmem);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  printf("exit after %d frames\n", frames);
  return ok ? 0 : 1;
}
