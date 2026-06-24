#pragma once

// DxvkAdapter shim — the single "physical device" the frontend sees.
//
// D3D9Adapter (frontend) holds an Rc<DxvkAdapter> and:
//   - constructs DxvkDeviceCapabilities from `Adapter->handle()` (VK_NULL_HANDLE
//     here — the caps shim ignores it).
//   - calls getFormatFeatures(VkFormat) / getFormatLimits(query) for
//     CheckDeviceFormat / CheckDeviceMultiSampleType.
//   - reads info() for identity, and createDevice() to make the device.
//
// There is no Vulkan physical device. handle() returns VK_NULL_HANDLE, format
// queries return generous canned support so ordinary RT/sampled formats pass,
// and createDevice() builds the Metal-backed DxvkDevice. See SHIM_SPEC.md.

#include <optional>

#include "dxvk_device_info.h"
#include "dxvk_extension_provider.h"
#include "dxvk_format.h"
#include "dxvk_include.h"
#include "dxvk_options.h"

#include "../util/util_gdi.h"  // D3DKMT_HANDLE

namespace dxvk {

  class DxvkDevice;
  class DxvkInstance;

  // GPU vendors (kept from ref — the frontend compares vendor IDs against these).
  enum class DxvkGpuVendor : uint16_t {
    Amd    = 0x1002,
    Nvidia = 0x10de,
    Intel  = 0x8086,
  };


  // Limited adapter identity. Canned with "d9mt Metal" values. Mirrors the real
  // struct's field set so any frontend read compiles.
  struct DxvkAdapterInfo {
    uint32_t             vendorId   = 0x106bu; // Apple
    uint32_t             deviceId   = 0x0001u;
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    char                 deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "d9mt Metal";
    uint8_t              deviceUuid[VK_UUID_SIZE]   = { };
    uint8_t              deviceLuid[VK_LUID_SIZE]   = { };
    bool                 luidIsValid   = false;
    VkDriverId           driverId      = VK_DRIVER_ID_MESA_HONEYKRISP; // any non-NV/AMD id
    char                 driverName[VK_MAX_DRIVER_NAME_SIZE] = "d9mt";
    char                 driverInfo[VK_MAX_DRIVER_INFO_SIZE] = "Metal";
    uint32_t             driverVersion = 1u;
    VkDeviceSize         deviceMemory  = 0u;
    VkDeviceSize         systemMemory  = 0u;
  };


  class DxvkAdapter : public RcObject {

  public:

    explicit DxvkAdapter(DxvkInstance& instance);
    ~DxvkAdapter();

    // No Vulkan instance functions; empty table so callers can hold an Rc.
    Rc<vk::InstanceFn> vki() const { return m_vki; }

    // No Vulkan physical device exists. The caps/device shims ignore this.
    VkPhysicalDevice handle() const { return VK_NULL_HANDLE; }

    // No D3DKMT adapter.
    D3DKMT_HANDLE kmtLocal() const { return 0; }

    DxvkAdapterInfo info() const { return DxvkAdapterInfo(); }

    // Driver-ID matching (dxvk-ref). The shim is not any real Vulkan driver, so
    // it never matches a queried driver — the frontend only uses this to gate
    // driver-specific workarounds, all of which stay disabled here.
    bool matchesDriver(VkDriverId, Version = Version(), Version = Version()) const { return false; }

    const VkPhysicalDeviceMemoryProperties& memoryProperties() const {
      return m_memoryProperties;
    }

    // Generous canned format support so the frontend's RT/sampled/blend checks
    // for ordinary color formats succeed. We are not actually validating Metal
    // format support here — the triangle path only needs BGRA8 to pass.
    DxvkFormatFeatures getFormatFeatures(VkFormat format) const;

    // Canned limits: 2D up to 16384, 1 sample. Returned for any queried format.
    std::optional<DxvkFormatLimits> getFormatLimits(
      const DxvkFormatQuery& query) const;

    // Builds the Metal-backed device (constructs DxvkDevice + its D9mtBackend
    // and initializes the backend). Returns nullptr if backend init fails.
    Rc<DxvkDevice> createDevice();

    // Memory accounting is a no-op (no real heaps to track).
    void notifyMemoryStats(uint32_t heap, int64_t allocated, int64_t used) {
      (void)heap; (void)allocated; (void)used;
    }

    // Owning instance (Vulkan-interop accessor; device->instance() forwards here).
    Rc<DxvkInstance> instance() const { return m_instance; }

  private:

    DxvkInstance*      m_instance = nullptr;
    Rc<vk::InstanceFn> m_vki      = new vk::InstanceFn();

    VkPhysicalDeviceMemoryProperties m_memoryProperties = { };

  };

}
