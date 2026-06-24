#pragma once

#include "../dxvk_include.h"

#include "dxvk_hud_item.h"

// HUD shim. The real DXVK HUD renders perf/driver overlays via its own Vulkan
// pipeline + glyph renderer. The Metal shim draws only the FF triangle and never
// shows a HUD, so this is a no-op stand-in: Hud::createHud() returns null (as the
// real one does when DXVK_HUD is unset), addItem<T>() returns a null Rc<T>
// without constructing anything. The item types live in dxvk_hud_item.h (and
// the D3D9-specific ones in d3d9_hud.h). See SHIM_SPEC.md (present module).

namespace dxvk {
  class DxvkDevice;
  class DxvkCommandList;
  class DxvkImageView;
}

namespace dxvk::hud {

  class Hud : public dxvk::RcObject {

  public:

    Hud(const Rc<DxvkDevice>& device) : m_device(device) { }
    ~Hud() = default;

    void update() { }

    void render(const Rc<DxvkCommandList>&, const Rc<DxvkImageView>&) { }

    bool empty() const { return true; }

    // Returns a null item — the shim renders no HUD. Args are ignored, so T is
    // only used as a type (no real construction happens).
    template<typename T, typename... Args>
    Rc<T> addItem(const char*, int32_t, Args&&...) {
      return Rc<T>();
    }

    // Mirrors real behaviour when DXVK_HUD is unset: no HUD is created.
    static Rc<Hud> createHud(const Rc<DxvkDevice>&) {
      return nullptr;
    }

  private:

    Rc<DxvkDevice> m_device;

  };

}
