/* resettest: fullscreen Reset chain under CrossOver (d9mt_wsi.cpp).
 *
 * 1. Enumerates X8R8G8B8 adapter modes: expects the d9mt ladder (640x480@60,
 *    1280x720@60 present; the current adapter mode present exactly; no odd
 *    width/height — odd modes wobble +-1px through CX scaling).
 * 2. Creates a windowed 640x480 device, draws + presents.
 * 3. Reset() to FULLSCREEN 1280x720@60 (NOT a real CX mode — exercises
 *    succeed-by-emulation), checks TestCooperativeLevel + GetDisplayMode
 *    reports 1280x720, draws + presents.
 * 4. Reset() to FULLSCREEN at the desktop resolution, draws + presents.
 * 5. Reset() back to windowed 640x480, draws + presents.
 * Results go to resettest_out.txt.
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

static void pump(void) {
  MSG msg;
  while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
}

/* draw a triangle scaled to the backbuffer + present a few frames */
static int draw_frames(IDirect3DDevice9 *dev, UINT w, UINT h, const char *tag) {
  struct Vertex tri[3] = {
      {w * 0.50f, h * 0.15f, 0.5f, 1.0f, 0xFFFF0000},
      {w * 0.85f, h * 0.85f, 0.5f, 1.0f, 0xFF00FF00},
      {w * 0.15f, h * 0.85f, 0.5f, 1.0f, 0xFF0000FF},
  };
  int i;
  for (i = 0; i < 3; i++) {
    pump();
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, FVF_TRI);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri,
                                     sizeof(struct Vertex));
    IDirect3DDevice9_EndScene(dev);
    HRESULT hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (FAILED(hr)) {
      LOG("FAIL: Present(%s, frame %d) -> 0x%08lx", tag, i, (unsigned long)hr);
      return 1;
    }
  }
  LOG("ok: 3 frames drawn+presented (%s)", tag);
  return 0;
}

static int do_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp,
                    const char *tag) {
  HRESULT hr = IDirect3DDevice9_Reset(dev, pp);
  if (FAILED(hr)) {
    LOG("FAIL: Reset(%s) -> 0x%08lx", tag, (unsigned long)hr);
    return 1;
  }
  LOG("ok: Reset(%s)", tag);
  hr = IDirect3DDevice9_TestCooperativeLevel(dev);
  if (hr != D3D_OK) {
    LOG("FAIL: TestCooperativeLevel after Reset(%s) -> 0x%08lx", tag,
        (unsigned long)hr);
    return 1;
  }
  LOG("ok: TestCooperativeLevel(%s) == D3D_OK", tag);
  return 0;
}

int main(void) {
  g_out = fopen("resettest_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = "d9mt_resettest";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_resettest", "d9mt resettest",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60,
                            rc.right - rc.left, rc.bottom - rc.top, NULL,
                            NULL, wc.hInstance, NULL);
  if (!hwnd) {
    LOG("FAIL: CreateWindow");
    return 1;
  }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    LOG("FAIL: Direct3DCreate9");
    return 1;
  }

  /* ---- mode enumeration sanity ---- */
  D3DDISPLAYMODE cur = {0};
  CHECK(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &cur));
  LOG("adapter display mode: %ux%u@%u", (unsigned)cur.Width,
      (unsigned)cur.Height, (unsigned)cur.RefreshRate);

  UINT count = IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT,
                                              D3DFMT_X8R8G8B8);
  LOG("X8R8G8B8 mode count: %u", (unsigned)count);
  if (count == 0) {
    LOG("FAIL: no modes enumerated");
    return 1;
  }

  int have640 = 0, have720p = 0, haveDesktop = 0, oddModes = 0;
  UINT i;
  for (i = 0; i < count; i++) {
    D3DDISPLAYMODE m = {0};
    if (FAILED(IDirect3D9_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT,
                                           D3DFMT_X8R8G8B8, i, &m)))
      break;
    if (m.Width == 640 && m.Height == 480 && m.RefreshRate == 60)
      have640 = 1;
    if (m.Width == 1280 && m.Height == 720 && m.RefreshRate == 60)
      have720p = 1;
    if (m.Width == cur.Width && m.Height == cur.Height)
      haveDesktop = 1;
    if ((m.Width & 1) || (m.Height & 1)) {
      oddModes++;
      LOG("odd mode in list: %ux%u", (unsigned)m.Width, (unsigned)m.Height);
    }
  }
  if (!have640 || !have720p || !haveDesktop || oddModes) {
    LOG("FAIL: mode list: 640x480@60=%d 1280x720@60=%d desktop=%d odd=%d",
        have640, have720p, haveDesktop, oddModes);
    return 1;
  }
  LOG("ok: mode list has 640x480@60, 1280x720@60, desktop %ux%u, no odd modes",
      (unsigned)cur.Width, (unsigned)cur.Height);

  /* ---- windowed device ---- */
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));
  if (draw_frames(dev, 640, 480, "windowed 640x480"))
    return 1;

  /* ---- Reset to fullscreen 1280x720@60 (phantom mode for CX: emulated) -- */
  pp.Windowed = FALSE;
  pp.BackBufferWidth = 1280;
  pp.BackBufferHeight = 720;
  pp.FullScreen_RefreshRateInHz = 60;
  if (do_reset(dev, &pp, "fullscreen 1280x720@60"))
    return 1;

  D3DDISPLAYMODE dm = {0};
  CHECK(IDirect3DDevice9_GetDisplayMode(dev, 0, &dm));
  if (dm.Width != 1280 || dm.Height != 720) {
    LOG("FAIL: GetDisplayMode after fullscreen Reset: %ux%u (want 1280x720)",
        (unsigned)dm.Width, (unsigned)dm.Height);
    return 1;
  }
  LOG("ok: GetDisplayMode reports 1280x720@%u", (unsigned)dm.RefreshRate);
  if (draw_frames(dev, 1280, 720, "fullscreen 1280x720"))
    return 1;

  /* ---- Reset to fullscreen at desktop resolution (mode change while
   *      already fullscreen: ChangeDisplayMode + updateFullscreenWindow) -- */
  pp.BackBufferWidth = cur.Width;
  pp.BackBufferHeight = cur.Height;
  pp.FullScreen_RefreshRateInHz = 0; /* "default" — must not div-by-zero */
  if (do_reset(dev, &pp, "fullscreen desktop"))
    return 1;
  if (draw_frames(dev, cur.Width, cur.Height, "fullscreen desktop"))
    return 1;

  /* ---- back to windowed ---- */
  pp.Windowed = TRUE;
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.FullScreen_RefreshRateInHz = 0;
  if (do_reset(dev, &pp, "windowed again"))
    return 1;

  /* after LeaveFullscreenMode the emulated mode must be cleared */
  CHECK(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &dm));
  if (dm.Width != cur.Width || dm.Height != cur.Height) {
    LOG("FAIL: adapter mode after windowed Reset: %ux%u (want %ux%u)",
        (unsigned)dm.Width, (unsigned)dm.Height, (unsigned)cur.Width,
        (unsigned)cur.Height);
    return 1;
  }
  LOG("ok: adapter mode restored to desktop %ux%u", (unsigned)dm.Width,
      (unsigned)dm.Height);
  if (draw_frames(dev, 640, 480, "windowed again"))
    return 1;

  LOG("PASS: fullscreen Reset chain (emulated mode set + ladder modes)");

  /* keep rendering until the suite runner kills us (no more logging) */
  for (;;) {
    pump();
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL)))
      break;
  }
  return 0;
}
