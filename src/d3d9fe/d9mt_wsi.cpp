// d9mt: Metal backend — WSI driver override (fullscreen Reset on CrossOver).
//
// CrossOver/Mac reality (measured with test/modeprobe.c in the bottle):
//   * EnumDisplaySettings exposes the Mac's scaled-resolution list ×2
//     (393 modes, e.g. 2056x1285 / 2056x1329 / 2992x1934), with NO classic
//     PC sizes at all — no 640x480, no 800x600, no 1280x720.
//   * ChangeDisplaySettingsExW "succeeds" for listed modes but never
//     actually switches the display (current mode stays the desktop mode),
//     and fails with DISP_CHANGE_BADMODE (-2) for anything off-list.
//   * Odd-height modes wobble ±1px through CX's physical↔logical (÷2/×2)
//     rounding: a game that sets 2056x1285 reads back 2056x1286, re-requests
//     that phantom mode, gets BADMODE → D3DERR_NOTAVAILABLE → Reset fails
//     ("Device reset failed: Device not reset" → GTA IV DD3D80 fatal).
//
// Fix: subclass the vendored Win32WsiDriver (all methods virtual) and patch
// the mutable WsiBootstrap Win32WSI.createDriver pointer before wsi::init()
// (installWsiDriver(), called from our DxvkInstance ctor). No vendored TU
// is modified. Three behaviors change:
//
//   1. getDisplayMode serves a sane, even-dimensioned mode list: the classic
//      ladder (640x480 .. 2560x1600, filtered to the desktop size) plus the
//      current desktop mode exactly, each at 60Hz (+ the desktop refresh
//      rate when different), 32 and 16 bpp. Consistent with
//      GetAdapterDisplayMode, and wobble-proof (even dims survive CX's ÷2/×2
//      round-trip). Games never see the phantom scaled modes again.
//   2. setWindowMode succeeds by emulation: real display-mode switching
//      does not exist under CX; fullscreen is "size the window/layer to the
//      screen" (the vendored enterFullscreenMode/updateFullscreenWindow
//      already do exactly that via getDesktopCoordinates, and our presenter
//      scales any backbuffer size to the layer). We still try the real
//      ChangeDisplaySettings first; failure is not an error.
//   3. getCurrentDisplayMode reports the emulated mode while one is active
//      (cleared by restoreDisplayMode, i.e. LeaveFullscreenMode), so
//      GetDisplayMode(Ex)/GetAdapterDisplayMode agree with what the game
//      asked for instead of leaking the unchanged desktop mode.

#include <algorithm>
#include <mutex>
#include <vector>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/wsi/win32/wsi_platform_win32.h"
#include "../../vendor/dxvk/src/util/log/log.h"
#include "../../vendor/dxvk/src/util/util_string.h"

namespace dxvk::d9mt {

  namespace {

    class D9mtWsiDriver final : public wsi::Win32WsiDriver {

    public:

      bool getDisplayMode(
              HMONITOR         hMonitor,
              uint32_t         modeNumber,
              wsi::WsiMode*    pMode) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Mode 0 starts a fresh enumeration: rebuild against the current
        // desktop so resolution changes between enumerations are honored.
        if (modeNumber == 0 || m_modes.empty())
          rebuildModeListLocked(hMonitor);

        if (modeNumber >= m_modes.size())
          return false;

        *pMode = m_modes[modeNumber];
        return true;
      }


      bool getCurrentDisplayMode(
              HMONITOR         hMonitor,
              wsi::WsiMode*    pMode) override {
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          if (m_emulatedModeActive) {
            *pMode = m_emulatedMode;
            return true;
          }
        }
        return Win32WsiDriver::getCurrentDisplayMode(hMonitor, pMode);
      }


      bool setWindowMode(
              HMONITOR         hMonitor,
              HWND             hWindow,
              wsi::DxvkWindowState* pState,
        const wsi::WsiMode&    mode) override {
        // Try the real mode set first (under CX it "succeeds" without
        // switching for listed modes); emulate success when it fails —
        // fullscreen on Mac is borderless: the vendored enterFullscreenMode/
        // updateFullscreenWindow size the window to the monitor and the
        // presenter scales the backbuffer into the layer.
        if (!Win32WsiDriver::setWindowMode(hMonitor, hWindow, pState, mode)) {
          Logger::info(str::format("d9mt: WSI: emulating display mode ",
            mode.width, "x", mode.height, " (mode set not available)"));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_emulatedMode = mode;

        // Reset()/ChangeDisplayMode divide by the refresh-rate denominator
        // and report this mode back through GetDisplayMode(Ex); normalize
        // "default" (0 / @0) requests to the real desktop refresh rate.
        if (!m_emulatedMode.refreshRate.numerator
         || !m_emulatedMode.refreshRate.denominator)
          m_emulatedMode.refreshRate = desktopRefreshRateLocked(hMonitor);

        if (!m_emulatedMode.bitsPerPixel)
          m_emulatedMode.bitsPerPixel = 32;

        m_emulatedMode.interlaced = false;
        m_emulatedModeActive = true;
        return true;
      }


      bool restoreDisplayMode() override {
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_emulatedModeActive = false;
        }
        // The real restore is a no-op when nothing was switched; its result
        // must not fail LeaveFullscreenMode under emulation.
        Win32WsiDriver::restoreDisplayMode();
        return true;
      }


    private:

      std::mutex                m_mutex;
      std::vector<wsi::WsiMode> m_modes;
      bool                      m_emulatedModeActive = false;
      wsi::WsiMode              m_emulatedMode = { };


      wsi::WsiRational desktopRefreshRateLocked(HMONITOR hMonitor) {
        wsi::WsiMode desktop = { };
        if (Win32WsiDriver::getCurrentDisplayMode(hMonitor, &desktop)
         && desktop.refreshRate.numerator
         && desktop.refreshRate.denominator)
          return desktop.refreshRate;
        return wsi::WsiRational{ 60, 1 };
      }


      void rebuildModeListLocked(HMONITOR hMonitor) {
        m_modes.clear();

        // Real current desktop mode (e.g. 2992x1934@120 on a retina panel);
        // never the emulated one — the list must stay anchored to reality.
        wsi::WsiMode desktop = { };
        if (!Win32WsiDriver::getCurrentDisplayMode(hMonitor, &desktop)
         || !desktop.width || !desktop.height) {
          Logger::err("d9mt: WSI: failed to query desktop mode for mode list");
          desktop.width  = 1920;
          desktop.height = 1080;
        }
        if (!desktop.refreshRate.numerator || !desktop.refreshRate.denominator)
          desktop.refreshRate = wsi::WsiRational{ 60, 1 };

        // Classic ladder; even dimensions only (odd sizes wobble ±1px through
        // CX's physical↔logical rounding). The exact desktop size is appended
        // so GetAdapterDisplayMode's mode is always in the list.
        static const struct { uint32_t w, h; } ladder[] = {
          {  640,  480 }, {  720,  480 }, {  720,  576 }, {  800,  600 },
          { 1024,  768 }, { 1152,  864 }, { 1280,  720 }, { 1280,  800 },
          { 1280,  960 }, { 1280, 1024 }, { 1360,  768 }, { 1440,  900 },
          { 1600,  900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 },
          { 1920, 1200 }, { 2048, 1152 }, { 2560, 1440 }, { 2560, 1600 },
        };

        const uint32_t desktopHz = desktop.refreshRate.numerator
                                 / desktop.refreshRate.denominator;

        auto addSize = [this, desktopHz] (uint32_t w, uint32_t h) {
          for (uint32_t bpp : { 32u, 16u }) {
            wsi::WsiMode mode = { };
            mode.width        = w;
            mode.height       = h;
            mode.bitsPerPixel = bpp;
            mode.interlaced   = false;

            mode.refreshRate  = wsi::WsiRational{ 60, 1 };
            m_modes.push_back(mode);

            if (desktopHz != 60) {
              mode.refreshRate = wsi::WsiRational{ desktopHz, 1 };
              m_modes.push_back(mode);
            }
          }
        };

        for (const auto& e : ladder) {
          if (e.w <= desktop.width && e.h <= desktop.height
           && (e.w != desktop.width || e.h != desktop.height))
            addSize(e.w, e.h);
        }

        addSize(desktop.width, desktop.height);

        logf("d9mt: WSI: mode list rebuilt: %u modes, desktop %ux%u@%u",
          unsigned(m_modes.size()), desktop.width, desktop.height, desktopHz);
      }
    };


    bool createD9mtWsiDriver(wsi::WsiDriver** driver) {
      *driver = new D9mtWsiDriver();
      return true;
    }

  } // anonymous namespace


  void installWsiDriver() {
    // The vendored bootstrap table entry is a mutable global; repointing its
    // factory makes wsi::init() construct OUR driver — no vendored edits.
    wsi::Win32WSI.createDriver = &createD9mtWsiDriver;
  }

} // namespace dxvk::d9mt
