#pragma once

// Device info / features / capabilities shim.
//
// The D3D9 frontend reads device properties + features in two ways:
//   1. Through DxvkDevice::features()/properties() (the device module).
//   2. Through DxvkDeviceCapabilities, which D3D9Adapter holds DIRECTLY as
//      `m_caps` and constructs as `(*Instance, Adapter->handle(), nullptr)`
//      (see d3d9_adapter.cpp ctor). It reads m_caps.getProperties()/getFeatures()
//      /getMemoryInfo() for caps reporting (limits, vendorID, deviceName, UUID,
//      driverID, depthBounds, attachmentFeedbackLoopLayout, sample counts...).
//
// The real DxvkDeviceCapabilities queries Vulkan in its ctor. The shim CANNOT —
// there is no Vulkan device. So we keep the real POD structs (zero-initialised
// Vulkan structs already give safe "feature unsupported / limit 0" answers) and
// provide a DxvkDeviceCapabilities whose ctor ignores its arguments and whose
// getters return the canned, all-false/all-zero structs. Canned identity values
// (vendor/device/name) are filled so the frontend reports a sane "d9mt Metal"
// adapter instead of an empty string. See SHIM_SPEC.md.

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "dxvk_include.h"

#include "../util/util_version.h"

namespace dxvk {

  class DxvkInstance;

  // ---------------------------------------------------------------------------
  // Property / feature POD structs — kept from dxvk-ref. Zero/default init gives
  // safe answers everywhere; we only override a few identity fields in the caps
  // ctor so the frontend reports a recognisable adapter.
  // ---------------------------------------------------------------------------

  struct DxvkDeviceInfo {
    Version                            driverVersion = { };
    VkPhysicalDeviceProperties2        core          = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    VkPhysicalDeviceVulkan11Properties vk11          = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceVulkan12Properties vk12          = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceVulkan13Properties vk13          = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    // Robustness-2 alignment props: the frontend reads the two access-size
    // alignments to size constant buffers. Zero (the default) means "no extra
    // alignment", which is correct for the shim's plain Metal buffers.
    VkPhysicalDeviceRobustness2PropertiesEXT extRobustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT };
  };


  // Mirrors the real struct but only carries the members the frontend actually
  // reads. Every VkBool32 / nested feature defaults to VK_FALSE, which is what
  // the shim wants (no extension is "supported"). The two members the D3D9
  // device ctor reads by name are present: extDepthBiasControl and
  // extAttachmentFeedbackLoopLayout, plus core.features (for depthBounds etc).
  struct DxvkDeviceFeatures {
    VkPhysicalDeviceFeatures2                              core                            = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features                       vk11                            = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features                       vk12                            = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features                       vk13                            = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT extAttachmentFeedbackLoopLayout = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT };
    VkPhysicalDeviceDepthBiasControlFeaturesEXT            extDepthBiasControl             = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT            extDescriptorBuffer             = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };
    VkPhysicalDeviceDescriptorHeapFeaturesEXT              extDescriptorHeap               = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
    VkBool32                                               nvxImageViewHandle              = VK_FALSE;
  };


  struct DxvkDeviceMemoryInfo {
    VkPhysicalDeviceMemoryProperties2         core   = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
  };


  struct DxvkDeviceQueueInfo {
    VkQueueFamilyProperties2 core = { VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 };
  };


  struct DxvkDeviceQueueIndex {
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    uint32_t index  = 0u;
  };


  struct DxvkDeviceQueueMapping {
    DxvkDeviceQueueIndex graphics;
    DxvkDeviceQueueIndex transfer;
    DxvkDeviceQueueIndex sparse;
  };


  // ---------------------------------------------------------------------------
  // DxvkDeviceCapabilities — held directly by D3D9Adapter. The ctor signature
  // must match the frontend call `(*Instance, Adapter->handle(), nullptr)`; we
  // ignore all of it (no Vulkan to query) and just fill canned identity values.
  // ---------------------------------------------------------------------------
  class DxvkDeviceCapabilities {

  public:

    DxvkDeviceCapabilities(
      const DxvkInstance&       /*instance*/,
            VkPhysicalDevice    /*adapter*/,
      const VkDeviceCreateInfo* /*deviceInfo*/,
            bool                /*safeMode*/ = false) {
      // Report a recognisable, UMA-style adapter. Everything else stays at the
      // zero defaults, which read as "feature unsupported / limit 0".
      m_properties.core.properties.vendorID   = 0x106bu; // Apple
      m_properties.core.properties.deviceID   = 0x0001u;
      m_properties.core.properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
      std::strncpy(m_properties.core.properties.deviceName, "d9mt Metal",
                   VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);

      // Sensible limit defaults so caps math (e.g. point size range) is benign.
      m_properties.core.properties.limits.pointSizeRange[0] = 1.0f;
      m_properties.core.properties.limits.pointSizeRange[1] = 256.0f;

      // Single graphics queue family at index 0.
      m_queueMapping.graphics = { 0u, 0u };
      m_queuesAvailable.resize(1u);
    }

    DxvkDeviceCapabilities(const DxvkDeviceCapabilities&) = delete;
    DxvkDeviceCapabilities& operator = (const DxvkDeviceCapabilities&) = delete;

    const DxvkDeviceFeatures&   getFeatures()   const { return m_featuresEnabled; }
    const DxvkDeviceInfo&       getProperties() const { return m_properties; }
    const DxvkDeviceMemoryInfo& getMemoryInfo() const { return m_memory; }

    const DxvkDeviceQueueInfo& getQueueProperties(uint32_t family) const {
      return m_queuesAvailable[family];
    }

    DxvkDeviceQueueMapping getQueueMapping() const { return m_queueMapping; }

  private:

    DxvkDeviceInfo                   m_properties      = { };
    DxvkDeviceFeatures               m_featuresEnabled = { };
    DxvkDeviceMemoryInfo             m_memory          = { };
    DxvkDeviceQueueMapping           m_queueMapping    = { };
    std::vector<DxvkDeviceQueueInfo> m_queuesAvailable;

  };

}
