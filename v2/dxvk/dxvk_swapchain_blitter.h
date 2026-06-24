#pragma once

// Shim of DXVK's swap chain blitter. In real DXVK this object owns a pile of Vulkan
// pipelines/samplers/images and blits the rendered back buffer onto the WSI swap chain
// image, compositing the HUD and software cursor. For the one-triangle milestone the
// actual pixels already reach the drawable through the Metal backend's
// renderAndPresent (the command module draws straight into the next drawable), so the
// blitter's present() is a deliberate no-op here. The other entry points the frontend
// calls (constructor, setGammaRamp, setCursorTexture, setCursorPos) are kept as harmless
// stubs.
//
// Only the methods/members the unmodified frontend (d3d9_swapchain.cpp) actually
// references are kept; their signatures match dxvk-ref/dxvk_swapchain_blitter.h verbatim:
//   - ctor(const Rc<DxvkDevice>&, const Rc<hud::Hud>&)
//   - present(const Rc<DxvkCommandList>&, const Rc<DxvkImageView>&, VkRect2D,
//             const Rc<DxvkImageView>&, VkRect2D)
//   - setGammaRamp(uint32_t, const DxvkGammaCp*)
//   - setCursorTexture(VkExtent2D, VkFormat, const void*)
//   - setCursorPos(VkRect2D)
// The cursor/gamma pipeline-key structs are preserved so anything that includes this
// header still sees the same public types.

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_context.h"
#include "../dxvk/dxvk_presenter.h"   // Presenter / PresenterDesc (full def for swapchain)

#include "./hud/dxvk_hud.h"

namespace dxvk {

  /**
   * \brief Gamma control point
   */
  struct DxvkGammaCp {
    uint16_t r, g, b, a;
  };


  /**
   * \brief Swap chain blitter pipeline key
   */
  struct DxvkSwapchainPipelineKey {
    VkColorSpaceKHR srcSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    VkSampleCountFlagBits srcSamples = VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
    VkBool32 srcIsSrgb = VK_FALSE;
    VkColorSpaceKHR dstSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    VkFormat dstFormat = VK_FORMAT_UNDEFINED;
    VkBool32 needsBlit = VK_FALSE;
    VkBool32 needsGamma = VK_FALSE;
    VkBool32 compositeHud = VK_FALSE;
    VkBool32 compositeCursor = VK_FALSE;

    size_t hash() const {
      DxvkHashState hash;
      hash.add(uint32_t(srcSpace));
      hash.add(uint32_t(srcSamples));
      hash.add(uint32_t(srcIsSrgb));
      hash.add(uint32_t(dstSpace));
      hash.add(uint32_t(dstFormat));
      hash.add(uint32_t(needsBlit));
      hash.add(uint32_t(needsGamma));
      hash.add(uint32_t(compositeHud));
      hash.add(uint32_t(compositeCursor));
      return hash;
    }

    bool eq(const DxvkSwapchainPipelineKey& other) const {
      return srcSpace == other.srcSpace
          && srcSamples == other.srcSamples
          && srcIsSrgb == other.srcIsSrgb
          && dstSpace == other.dstSpace
          && dstFormat == other.dstFormat
          && needsBlit == other.needsBlit
          && needsGamma == other.needsGamma
          && compositeHud == other.compositeHud
          && compositeCursor == other.compositeCursor;
    }
  };


  /**
   * \brief Swap chain cursor pipeline key
   */
  struct DxvkCursorPipelineKey {
    VkColorSpaceKHR dstSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    VkFormat dstFormat = VK_FORMAT_UNDEFINED;

    size_t hash() const {
      DxvkHashState hash;
      hash.add(uint32_t(dstSpace));
      hash.add(uint32_t(dstFormat));
      return hash;
    }

    bool eq(const DxvkCursorPipelineKey& other) const {
      return dstSpace == other.dstSpace
          && dstFormat == other.dstFormat;
    }
  };


  /**
   * \brief Swap chain blitter (Metal shim)
   *
   * The real implementation blits the source image onto the swap chain image. The shim
   * relies on the Metal backend having already presented the frame, so present() is a
   * no-op and the other methods are stubs. See header comment.
   */
  class DxvkSwapchainBlitter : public RcObject {

  public:

    DxvkSwapchainBlitter(
      const Rc<DxvkDevice>& device,
      const Rc<hud::Hud>&   hud)
    : m_device(device), m_hud(hud) { }

    ~DxvkSwapchainBlitter() { }

    /**
     * \brief Begins recording presentation commands
     *
     * No-op in the shim: the Metal backend draws straight into the drawable and presents
     * it, so there is nothing to blit here. Signature matches the frontend call site
     * (d3d9_swapchain.cpp), which passes the result of ctx->beginExternalRendering().
     */
    void present(
      const Rc<DxvkCommandList>& ctx,
      const Rc<DxvkImageView>&   dstView,
            VkRect2D             dstRect,
      const Rc<DxvkImageView>&   srcView,
            VkRect2D             srcRect) {
      // Intentionally empty — present already happened in the Metal backend.
    }

    /**
     * \brief Sets gamma ramp (stub)
     */
    void setGammaRamp(
            uint32_t      cpCount,
      const DxvkGammaCp*  cpData) {
      m_gammaCpCount = cpCount;
    }

    /**
     * \brief Sets software cursor texture (stub)
     */
    void setCursorTexture(
            VkExtent2D    extent,
            VkFormat      format,
      const void*         data) {
    }

    /**
     * \brief Sets cursor position (stub)
     */
    void setCursorPos(
            VkRect2D      rect) {
      m_cursorRect = rect;
    }

  private:

    Rc<DxvkDevice> m_device;
    Rc<hud::Hud>   m_hud;

    uint32_t       m_gammaCpCount = 0;
    VkRect2D       m_cursorRect   = { };

  };

}
