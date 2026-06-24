#pragma once

// DxvkInstance shim — the top of the device-creation chain.
//
// The D3D9 frontend creates exactly one instance via the global singleton
// `g_dxvkInstance.acquire(DxvkInstanceFlag::ClientApiIsD3D9)` (d3d9_interface.cpp)
// and then reads:
//   - config()        -> const Config&   (passed to D3D9Options)
//   - adapterCount()  -> single adapter
//   - enumAdapters(i) -> Rc<DxvkAdapter>
//   - vki()           -> Rc<vk::InstanceFn> (empty table)
//   - debugFlags()    -> DxvkDebugFlags (0)
//
// There is no Vulkan instance behind this — we hold one canned adapter and an
// empty instance-fn table. See SHIM_SPEC.md.

#include "../util/config/config.h"
#include "../util/util_singleton.h"

#include "dxvk_adapter.h"
#include "dxvk_extension_provider.h"
#include "dxvk_options.h"

namespace dxvk {

  // Debug flags. The shim never enables any, so debugFlags() reports 0; the
  // frontend tests DxvkDebugFlag::Markers in the device ctor and skips its
  // annotation path when unset.
  enum class DxvkDebugFlag : uint32_t {
    Validation = 0,
    Capture    = 1,
    Markers    = 2,
  };

  using DxvkDebugFlags = Flags<DxvkDebugFlag>;


  // Instance creation flags. The singleton forwards a single flag value to the
  // ctor; Flags<> has an implicit ctor from one enum value so this binds.
  enum class DxvkInstanceFlag : uint32_t {
    /** Enforce D3D9 behaviour for texture coordinate snapping */
    ClientApiIsD3D9,
  };

  using DxvkInstanceFlags = Flags<DxvkInstanceFlag>;


  class DxvkInstance : public RcObject {

  public:

    explicit DxvkInstance(DxvkInstanceFlags flags);

    ~DxvkInstance();

    // Empty Vulkan instance-fn table (RcObject so the frontend can hold it).
    Rc<vk::InstanceFn> vki() const { return m_vki; }

    // The shim always exposes exactly one adapter.
    size_t adapterCount() { return 1u; }

    // Returns the single canned adapter for any index (the frontend repeats the
    // first adapter when it runs out, so out-of-range simply reuses it).
    Rc<DxvkAdapter> enumAdapters(uint32_t index) const;

    // Real Config kept; default-constructed (no config file is read).
    const Config& config() const { return m_config; }

    const DxvkOptions& options() const { return m_options; }

    DxvkDebugFlags debugFlags() const { return m_debugFlags; }

    // Vulkan-interop accessors (ID3D9VkInteropInterface). No real Vulkan
    // instance exists, so handle() is null and the extension list is empty.
    VkInstance handle() const { return VK_NULL_HANDLE; }
    DxvkExtensionList getExtensionList() const { return DxvkExtensionList(); }

  private:

    Config             m_config;
    DxvkOptions        m_options;
    Rc<vk::InstanceFn> m_vki        = new vk::InstanceFn();
    DxvkDebugFlags     m_debugFlags = 0u;
    Rc<DxvkAdapter>    m_adapter;

  };

}
