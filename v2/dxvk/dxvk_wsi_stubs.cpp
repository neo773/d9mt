// WSI stubs: the win32 backend (wsi/win32) isn't built — it pulls a Vulkan
// surface (vkCreateWin32SurfaceKHR) and the display-mode-switching machinery we
// don't run for a single windowed triangle. The frontend's adapter/swapchain/
// window code still references these symbols, so provide minimal win32-backed
// (or canned) implementations here.
//
// createSurface returns a null surface: the shim Presenter renders through a
// winemetal CAMetalLayer attached to the HWND, not a VkSurfaceKHR, so the
// frontend's surface handle is never dereferenced.

#include <windows.h>

#include "../wsi/wsi_window.h"
#include "../wsi/wsi_monitor.h"
#include "../wsi/wsi_edid.h"

#include "dxvk_backend.h"   // g_activeBackend — attach the Metal layer to the HWND here

namespace dxvk::wsi {

  // One canned desktop mode is enough for adapter/mode enumeration to succeed.
  static WsiMode cannedMode(HMONITOR hMonitor) {
    RECT rect = { 0, 0, 1920, 1080 };
    getDesktopCoordinates(hMonitor, &rect);
    WsiMode mode = { };
    mode.width        = uint32_t(rect.right - rect.left);
    mode.height       = uint32_t(rect.bottom - rect.top);
    mode.refreshRate  = WsiRational { 60u, 1u };
    mode.bitsPerPixel = 32u;
    mode.interlaced   = false;
    return mode;
  }

  // ---- monitors -----------------------------------------------------------
  HMONITOR getDefaultMonitor() {
    return ::MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
  }

  bool getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
    MONITORINFOEXW info = { };
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(hMonitor, &info))
      return false;
    for (size_t i = 0; i < 32; i++)
      Name[i] = info.szDevice[i];
    return true;
  }

  bool getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) {
    MONITORINFOEXW info = { };
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(hMonitor, &info))
      return false;
    *pRect = info.rcMonitor;
    return true;
  }

  bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) {
    if (modeNumber != 0)
      return false;            // single canned mode
    *pMode = cannedMode(hMonitor);
    return true;
  }

  bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    *pMode = cannedMode(hMonitor);
    return true;
  }

  bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    *pMode = cannedMode(hMonitor);
    return true;
  }

  WsiEdidData getMonitorEdid(HMONITOR) {
    return WsiEdidData();        // no EDID → caller falls back to canned metadata
  }

  // ---- windows ------------------------------------------------------------
  void getWindowSize(HWND hWindow, uint32_t* pWidth, uint32_t* pHeight) {
    RECT rect = { };
    ::GetClientRect(hWindow, &rect);
    if (pWidth)  *pWidth  = uint32_t(rect.right - rect.left);
    if (pHeight) *pHeight = uint32_t(rect.bottom - rect.top);
  }

  HMONITOR getWindowMonitor(HWND hWindow) {
    return ::MonitorFromWindow(hWindow, MONITOR_DEFAULTTOPRIMARY);
  }

  bool isWindow(HWND hWindow) {
    return ::IsWindow(hWindow);
  }

  // Fullscreen / mode-switch are no-ops: the triangle runs windowed.
  void saveWindowState(HWND, DxvkWindowState*, bool) { }
  void restoreWindowState(HWND, DxvkWindowState*, bool) { }
  bool setWindowMode(HMONITOR, HWND, DxvkWindowState*, const WsiMode&) { return true; }
  bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) { return true; }
  bool leaveFullscreenMode(HWND, DxvkWindowState*) { return true; }
  bool restoreDisplayMode() { return true; }
  void updateFullscreenWindow(HMONITOR, HWND, bool) { }

  VkResult createSurface(HWND hWindow, PFN_vkGetInstanceProcAddr, VkInstance, VkSurfaceKHR* pSurface) {
    // No Vulkan surface — the shim presents via a winemetal CAMetalLayer. This
    // callback is the only point the frontend hands us the HWND, so use it to
    // attach the backend's layer to the window (idempotent in the backend).
    if (g_activeBackend && hWindow) {
      RECT rc = { };
      ::GetClientRect(hWindow, &rc);
      g_activeBackend->attachWindow(hWindow,
        uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top));
    }
    if (pSurface)
      *pSurface = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }

  // ---- EDID colorimetry ---------------------------------------------------
  std::optional<WsiDisplayMetadata> parseColorimetryInfo(const WsiEdidData&) {
    return std::nullopt;         // no HDR metadata; caller normalizes to SDR defaults
  }

}
