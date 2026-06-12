/* modeprobe.exe — pure win32 display-mode probe for the CX bottle.
 * Dumps EnumDisplaySettings mode list, current/registry settings, monitor
 * rects, GetSystemMetrics, ChangeDisplaySettingsExW behavior for a few
 * candidate modes, and client-rect rounding for a 1280x720 window.
 * Output: modeprobe_out.txt next to the exe. No d3d9 involved.
 */
#include <windows.h>
#include <stdio.h>

static FILE* out;
#define P(...) do { fprintf(out, __VA_ARGS__); fflush(out); } while (0)

static void tryMode(const WCHAR* dev, DWORD w, DWORD h, DWORD hz) {
  DEVMODEW dm;
  memset(&dm, 0, sizeof(dm));
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
  if (hz) { dm.dmFields |= DM_DISPLAYFREQUENCY; dm.dmDisplayFrequency = hz; }
  LONG st = ChangeDisplaySettingsExW(dev, &dm, NULL, CDS_TEST, NULL);
  LONG st2 = ChangeDisplaySettingsExW(dev, &dm, NULL, CDS_FULLSCREEN, NULL);
  P("CDS %lux%lu@%lu: TEST=%ld FULLSCREEN=%ld\n",
    (unsigned long)w, (unsigned long)h, (unsigned long)hz, st, st2);
  if (st2 == DISP_CHANGE_SUCCESSFUL) {
    DEVMODEW cur; memset(&cur, 0, sizeof(cur)); cur.dmSize = sizeof(cur);
    EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &cur);
    P("  -> now current: %lux%lu@%lu\n",
      (unsigned long)cur.dmPelsWidth, (unsigned long)cur.dmPelsHeight,
      (unsigned long)cur.dmDisplayFrequency);
    /* restore */
    ChangeDisplaySettingsExW(dev, NULL, NULL, 0, NULL);
  }
}

static BOOL CALLBACK monProc(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp) {
  MONITORINFOEXW mi;
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, (MONITORINFO*)&mi);
  P("monitor '%ls'%s rcMonitor=(%ld,%ld)-(%ld,%ld) %ldx%ld rcWork=(%ld,%ld)-(%ld,%ld)\n",
    mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " PRIMARY" : "",
    mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
    mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
  return TRUE;
}

int main(void) {
  out = fopen("modeprobe_out.txt", "w");
  if (!out) return 1;

  P("SM_CXSCREEN=%d SM_CYSCREEN=%d SM_CXVIRTUALSCREEN=%d SM_CYVIRTUALSCREEN=%d\n",
    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
    GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));

  EnumDisplayMonitors(NULL, NULL, monProc, 0);

  /* primary device name */
  MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
  POINT pt = { 0, 0 };
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  GetMonitorInfoW(mon, (MONITORINFO*)&mi);

  DEVMODEW dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
    P("ENUM_CURRENT_SETTINGS: %lux%lu@%lu %lubpp\n",
      (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
      (unsigned long)dm.dmDisplayFrequency, (unsigned long)dm.dmBitsPerPel);
  memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
    P("ENUM_REGISTRY_SETTINGS: %lux%lu@%lu %lubpp\n",
      (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
      (unsigned long)dm.dmDisplayFrequency, (unsigned long)dm.dmBitsPerPel);

  P("--- mode list ---\n");
  DWORD i = 0;
  for (;;) {
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, i, &dm)) break;
    P("mode %3lu: %lux%lu@%lu %lubpp%s\n", (unsigned long)i,
      (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
      (unsigned long)dm.dmDisplayFrequency, (unsigned long)dm.dmBitsPerPel,
      (dm.dmDisplayFlags & DM_INTERLACED) ? " interlaced" : "");
    i++;
    if (i > 512) { P("(stopping at 512)\n"); break; }
  }
  P("--- end mode list (%lu modes) ---\n", (unsigned long)i);

  P("--- ChangeDisplaySettingsExW probes ---\n");
  tryMode(mi.szDevice, 2992, 1934, 0);
  tryMode(mi.szDevice, 2056, 1285, 60);
  tryMode(mi.szDevice, 2056, 1286, 60);
  tryMode(mi.szDevice, 1280, 720, 60);
  tryMode(mi.szDevice, 640, 480, 60);

  P("--- window client-rect rounding ---\n");
  {
    RECT want = { 0, 0, 1280, 720 };
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    P("AdjustWindowRect(1280x720, OVERLAPPED): outer %ldx%ld\n",
      want.right - want.left, want.bottom - want.top);
    HWND wnd = CreateWindowExW(0, L"STATIC", L"modeprobe",
      WS_OVERLAPPEDWINDOW, 50, 50,
      want.right - want.left, want.bottom - want.top, NULL, NULL,
      GetModuleHandleW(NULL), NULL);
    if (wnd) {
      ShowWindow(wnd, SW_SHOW);
      RECT cr, wr; GetClientRect(wnd, &cr); GetWindowRect(wnd, &wr);
      P("window: outer %ldx%ld client %ldx%ld\n",
        wr.right - wr.left, wr.bottom - wr.top,
        cr.right - cr.left, cr.bottom - cr.top);
      /* fullscreen-style: pop styles, size to monitor like wsi does */
      SetWindowLongW(wnd, GWL_STYLE,
        (GetWindowLongW(wnd, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
      GetMonitorInfoW(mon, (MONITORINFO*)&mi);
      SetWindowPos(wnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      GetClientRect(wnd, &cr); GetWindowRect(wnd, &wr);
      P("borderless-fullscreen: outer %ldx%ld client %ldx%ld\n",
        wr.right - wr.left, wr.bottom - wr.top,
        cr.right - cr.left, cr.bottom - cr.top);
      DestroyWindow(wnd);
    } else {
      P("CreateWindow failed %lu\n", (unsigned long)GetLastError());
    }
  }

  P("PASS modeprobe done\n");
  fclose(out);
  return 0;
}
