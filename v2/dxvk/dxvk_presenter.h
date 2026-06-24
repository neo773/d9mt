#pragma once

// Shim of DXVK's Vulkan presenter. The frontend (d3d9_swapchain.cpp) holds an
// Rc<Presenter> per window and drives it: construct it with the device + a surface
// callback, then each frame call setSyncInterval / setSurfaceExtent / setSurfaceFormat /
// acquireNextImage, and finally DxvkDevice::presentImage. In real DXVK this wraps a
// VkSwapchainKHR; here presentation actually happens through the Metal backend
// (renderAndPresent), so the presenter is a thin bookkeeping stub.
//
// acquireNextImage() reports VK_SUCCESS and hands back a back-buffer DxvkImage so the
// frontend's present lambda has a valid destination view to create — the conceptual
// "next drawable". The real drawable is fetched inside the Metal backend at present time
// (SHIM_SPEC present module). Every signature matches dxvk-ref/dxvk_presenter.h verbatim
// so the unmodified frontend compiles and links.

#include <functional>

#include "dxvk_format.h"
#include "dxvk_image.h"

#include "../util/sync/sync_signal.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkLatencyTracker;

  // Surface creation callback. The frontend supplies one that calls wsi::createSurface;
  // the shim never invokes it (no Vulkan surface), but the type must match.
  using PresenterSurfaceProc = std::function<VkResult (VkSurfaceKHR*)>;

  /**
   * \brief Presenter description
   */
  struct PresenterDesc {
    bool deferSurfaceCreation = false;
  };

  /**
   * \brief Presenter semaphores
   *
   * Kept as a POD so the frontend's PresenterSync values copy through the present
   * lambda; the shim leaves all handles null (no Vulkan sync objects).
   */
  struct PresenterSync {
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkSemaphore present = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 fenceSignaled = VK_FALSE;
  };

  /**
   * \brief Presenter (Metal shim)
   *
   * Bookkeeping stub: tracks the requested extent/format/sync interval and reports a
   * back buffer on acquire. Actual presentation is done by the Metal backend.
   */
  class Presenter : public RcObject {

  public:

    Presenter(
      const Rc<DxvkDevice>&   device,
      const Rc<sync::Signal>& signal,
      const PresenterDesc&    desc,
            PresenterSurfaceProc&& proc)
    : m_device(device), m_signal(signal), m_surfaceProc(std::move(proc)) {
      // No Vulkan surface is created, but we invoke the callback once anyway: it
      // carries the HWND down to wsi::createSurface, which is where the shim
      // attaches the Metal presentation layer to the window. Without this, the
      // layer is never bound and the window stays transparent.
      VkSurfaceKHR ignored = VK_NULL_HANDLE;
      m_surfaceProc(&ignored);
    }

    ~Presenter() { }

    /**
     * \brief Tests swap chain status
     * \returns VK_SUCCESS — the Metal swap chain is always ready.
     */
    VkResult checkSwapChainStatus() {
      return VK_SUCCESS;
    }

    /**
     * \brief Acquires next image
     *
     * Reports success and hands back a back-buffer DxvkImage as the conceptual next
     * drawable. The real drawable is fetched in the Metal backend at present time.
     */
    VkResult acquireNextImage(
            PresenterSync&  sync,
            Rc<DxvkImage>&  image);

    /**
     * \brief Presents current image (no-op; the Metal backend presents)
     */
    VkResult presentImage(
            uint64_t                      frameId,
      const Rc<DxvkLatencyTracker>&       tracker) {
      return VK_SUCCESS;
    }

    /**
     * \brief Signals a given frame
     */
    void signalFrame(
            uint64_t                      frameId,
      const Rc<DxvkLatencyTracker>&       tracker) {
      if (m_signal != nullptr)
        m_signal->signal(frameId);
    }

    void setSyncInterval(uint32_t syncInterval) {
      m_syncInterval = syncInterval;
    }

    void setFrameRateLimit(double frameRate, uint32_t maxLatency) {
      m_frameRate = frameRate;
    }

    void setSurfaceFormat(VkSurfaceFormatKHR format) {
      m_format = format;
    }

    void setSurfaceExtent(VkExtent2D extent) {
      m_extent = extent;
    }

    void setHdrMetadata(VkHdrMetadataEXT hdrMetadata) {
    }

    bool supportsColorSpace(VkColorSpaceKHR colorspace) {
      // We only ever drive the standard sRGB non-linear space through Metal.
      return colorspace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    void invalidateSurface() {
    }

    void destroyResources() {
    }

  private:

    Rc<DxvkDevice>        m_device;
    Rc<sync::Signal>      m_signal;
    PresenterSurfaceProc  m_surfaceProc;

    uint32_t              m_syncInterval = 1;
    double                m_frameRate    = 0.0;
    VkExtent2D            m_extent       = { 0u, 0u };
    VkSurfaceFormatKHR    m_format       = { VK_FORMAT_B8G8R8A8_UNORM,
                                             VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };

  };

}
