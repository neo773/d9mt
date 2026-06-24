#include "dxvk_instance.h"

namespace dxvk {

  DxvkInstance::DxvkInstance(DxvkInstanceFlags flags) {
    // Flags are irrelevant to the Metal shim (they only tune Vulkan driver
    // quirks); accept and ignore them. The single canned adapter is created
    // eagerly so enumAdapters() can hand out the same Rc to every query.
    (void)flags;
    m_adapter = new DxvkAdapter(*this);
  }


  DxvkInstance::~DxvkInstance() {
  }


  Rc<DxvkAdapter> DxvkInstance::enumAdapters(uint32_t index) const {
    // Only one adapter exists; the frontend already repeats adapter 0 when it
    // runs past the count, so any index resolves to the same adapter.
    (void)index;
    return m_adapter;
  }

}
