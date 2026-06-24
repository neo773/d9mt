#pragma once

#include <string>

#include "../dxvk_include.h"

#include "../../util/util_time.h"

// HUD item shim. The Metal shim renders no HUD overlay (Hud::createHud returns
// null), so these types exist only so the frontend's HUD item classes
// (d3d9_hud.h) and the swapchain's addItem<T> calls compile. Nothing here is
// ever constructed or rendered. Shapes mirror dxvk-ref's hud item/renderer
// headers (Vulkan TYPES only). See SHIM_SPEC.md (present module).

namespace dxvk {
  class DxvkCommandList;
  // DxvkLatencyStats comes from dxvk_include.h (shared with the latency tracker).
}

namespace dxvk::hud {

  struct HudOptions {
    float scale = 1.0f;
    float opacity = 1.0f;
  };

  struct HudPos {
    int32_t x = 0;
    int32_t y = 0;
  };

  struct HudPipelineKey {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    size_t hash() const { return size_t(format) ^ (size_t(colorSpace) << 8); }
  };

  // Text renderer — entirely inert in the shim (no glyph atlas, no draws).
  class HudRenderer { };

  // Base HUD item. render() is pure virtual to match dxvk-ref; the shim never
  // instantiates any concrete item (addItem<T> returns a null Rc<T>).
  class HudItem : public RcObject {

  public:

    virtual ~HudItem() = default;

    virtual void update(dxvk::high_resolution_clock::time_point) { }

    virtual HudPos render(
      const Rc<DxvkCommandList>& ctx,
      const HudPipelineKey&      key,
      const HudOptions&          options,
            HudRenderer&         renderer,
            HudPos               position) = 0;

  };

  // Manages a set of HUD items. The shim adds nothing (Hud::addItem short-
  // circuits), so this is an empty stand-in with a no-op templated add().
  class HudItemSet {

  public:

    template<typename T, typename... Args>
    Rc<T> add(const char*, int32_t, Args&&...) { return Rc<T>(); }

    bool empty() const { return true; }

  };

  // Items the swapchain references directly (not defined in d3d9_hud.h). They are
  // never constructed; only their type + the methods the swapchain names exist.
  class HudClientApiItem : public HudItem {
  public:
    HudClientApiItem(std::string) { }
    HudPos render(const Rc<DxvkCommandList>&, const HudPipelineKey&,
                  const HudOptions&, HudRenderer&, HudPos position) override { return position; }
  };

  class HudLatencyItem : public HudItem {
  public:
    void accumulateStats(const DxvkLatencyStats&) { }
    HudPos render(const Rc<DxvkCommandList>&, const HudPipelineKey&,
                  const HudOptions&, HudRenderer&, HudPos position) override { return position; }
  };

}
