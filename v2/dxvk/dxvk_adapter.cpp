#include "dxvk_adapter.h"
#include "dxvk_device.h"

namespace dxvk {

  DxvkAdapter::DxvkAdapter(DxvkInstance& instance)
    : m_instance(&instance) {
    // One device-local heap so the frontend's UMA / memory-budget reads are
    // benign. No real Vulkan heaps exist behind this.
    m_memoryProperties.memoryHeapCount = 1u;
    m_memoryProperties.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    m_memoryProperties.memoryTypeCount = 1u;
    m_memoryProperties.memoryTypes[0].heapIndex = 0u;
    m_memoryProperties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }


  DxvkAdapter::~DxvkAdapter() {
  }


  DxvkFormatFeatures DxvkAdapter::getFormatFeatures(VkFormat format) const {
    // Report broad support for ordinary color formats so the frontend's
    // CheckDeviceFormat (sampled / render-target / blend) passes. Depth/stencil
    // and the buffer path get the same generous flags; the triangle path never
    // needs precise validation.
    (void)format;

    DxvkFormatFeatures features = { };
    features.optimal =
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
      | VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT
      | VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT
      | VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT
      | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT
      | VK_FORMAT_FEATURE_2_BLIT_DST_BIT
      | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
      | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    features.linear = features.optimal;
    features.buffer =
        VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT
      | VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
    return features;
  }


  std::optional<DxvkFormatLimits> DxvkAdapter::getFormatLimits(
    const DxvkFormatQuery& query) const {
    (void)query;

    DxvkFormatLimits limits = { };
    limits.maxExtent       = { 16384u, 16384u, 1u };
    limits.maxMipLevels    = 15u;
    limits.maxArrayLayers  = 2048u;
    limits.sampleCounts    = VK_SAMPLE_COUNT_1_BIT;
    limits.maxResourceSize = VkDeviceSize(1u) << 31; // 2 GiB, plenty for a triangle
    return limits;
  }


  Rc<DxvkDevice> DxvkAdapter::createDevice() {
    Rc<DxvkDevice> device = new DxvkDevice(this);

    if (!device->initialize()) {
      Logger::err("DxvkAdapter::createDevice: Metal backend init failed");
      return nullptr;
    }

    return device;
  }

}
