#include "dxvk_presenter.h"
#include "dxvk_device.h"

// Presenter::acquireNextImage is defined out of line because it needs the full
// DxvkDevice interface (createImage) — the device shim mints the Metal-backed back
// buffer that stands in for the WSI swap chain image. The frontend's present lambda
// only reads info().format off this image and builds a view from it; the pixels are
// actually presented by the Metal backend, so a fresh render-target image is enough.

namespace dxvk {

  VkResult Presenter::acquireNextImage(
          PresenterSync&  sync,
          Rc<DxvkImage>&  image) {
    // No Vulkan semaphores/fences in the shim.
    sync = PresenterSync { };

    // Describe the conceptual swap chain back buffer at the requested extent/format.
    DxvkImageCreateInfo info = { };
    info.type        = VK_IMAGE_TYPE_2D;
    info.format      = m_format.format != VK_FORMAT_UNDEFINED
                       ? m_format.format : VK_FORMAT_B8G8R8A8_UNORM;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent      = { std::max(m_extent.width, 1u), std::max(m_extent.height, 1u), 1u };
    info.numLayers   = 1u;
    info.mipLevels   = 1u;
    info.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                     | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages      = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    info.access      = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    info.layout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    info.colorSpace  = m_format.colorSpace;

    image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (image == nullptr)
      return VK_NOT_READY;

    return VK_SUCCESS;
  }

}
