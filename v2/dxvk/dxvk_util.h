#pragma once

#include "dxvk_include.h"
#include "dxvk_limits.h"

#include "../vulkan/vulkan_util.h"  // vk::getPlaneIndex / getNextAspect / getPlaneCount

// dxvk::util — pure image/format math helpers (mip extents, block counts, data
// packing, component-mask remapping, blend-factor helpers). Copied verbatim from
// dxvk-ref/dxvk_util.h: every function operates on Vulkan TYPES (VkExtent3D,
// VkFormat, …) which the shim keeps (SHIM_SPEC rule 2) — there is NO Vulkan
// runtime here. The D3D9 frontend calls these directly (texture sizing, image
// uploads, format conversion). Out-of-line bodies live in dxvk_util.cpp.
//
// Requires lookupFormatInfo / vk::getPlaneIndex from dxvk_format.h, which is why
// dxvk_format.h includes this header at the bottom (after those are declared).

namespace dxvk::util {

  enum class DxvkDebugLabelType : uint32_t {
    External,
    InternalRenderPass,
    InternalBarrierControl,
  };

  class DxvkDebugLabel {

  public:

    DxvkDebugLabel(const VkDebugUtilsLabelEXT& label, DxvkDebugLabelType type)
    : m_text(label.pLabelName ? label.pLabelName : ""), m_type(type) {
      for (uint32_t i = 0; i < m_color.size(); i++)
        m_color[i] = label.color[i];
    }

    DxvkDebugLabelType type() const { return m_type; }

    VkDebugUtilsLabelEXT get() const {
      VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
      label.pLabelName = m_text.c_str();
      for (uint32_t i = 0; i < m_color.size(); i++)
        label.color[i] = m_color[i];
      return label;
    }

  private:

    std::string           m_text;
    std::array<float, 4>  m_color = { };
    DxvkDebugLabelType    m_type;

  };


  // Built-in shader stage — code pointer + size. The shim never builds these
  // pipelines, but the type appears in createBuiltIn* signatures.
  struct DxvkBuiltInShaderStage {
    DxvkBuiltInShaderStage() = default;

    template<size_t N>
    DxvkBuiltInShaderStage(const uint32_t (&dwords)[N], const VkSpecializationInfo* s)
    : size(N * sizeof(uint32_t)), code(&dwords[0]), spec(s) { }

    DxvkBuiltInShaderStage(const std::vector<uint32_t>& dwords)
    : size(dwords.size() * sizeof(uint32_t)), code(dwords.data()) { }

    size_t                      size = 0u;
    const uint32_t*             code = nullptr;
    const VkSpecializationInfo* spec = nullptr;
  };


  struct DxvkBuiltInGraphicsState {
    DxvkBuiltInShaderStage vs;
    DxvkBuiltInShaderStage gs;
    DxvkBuiltInShaderStage fs;
    std::array<VkFormat, MaxNumRenderTargets> colorFormats = { };
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineVertexInputStateCreateInfo* viState = nullptr;
    const VkPipelineInputAssemblyStateCreateInfo* iaState = nullptr;
    const VkPipelineRasterizationStateCreateInfo* rsState = nullptr;
    const VkPipelineDepthStencilStateCreateInfo* dsState = nullptr;
    const VkPipelineColorBlendAttachmentState* cbAttachment = nullptr;
    uint32_t dynamicStateCount = 0u;
    const VkDynamicState* dynamicStates = nullptr;
  };


  inline VkPipelineStageFlags pipelineStages(VkShaderStageFlags shaderStages) {
    return (shaderStages & VK_SHADER_STAGE_ALL_GRAPHICS) << 3
         | (shaderStages & VK_SHADER_STAGE_COMPUTE_BIT) << 6;
  }

  inline VkShaderStageFlags shaderStages(VkPipelineStageFlags pipelineStages) {
    return ((pipelineStages >> 3) & VK_SHADER_STAGE_ALL_GRAPHICS)
         | ((pipelineStages >> 6) & VK_SHADER_STAGE_COMPUTE_BIT);
  }

  uint32_t computeMipLevelCount(VkExtent3D imageSize);

  void packImageData(
          void*             dstBytes,
    const void*             srcBytes,
          VkExtent3D        blockCount,
          VkDeviceSize      blockSize,
          VkDeviceSize      pitchPerRow,
          VkDeviceSize      pitchPerLayer);

  void packImageData(
          void*             dstBytes,
    const void*             srcBytes,
          VkDeviceSize      srcRowPitch,
          VkDeviceSize      srcSlicePitch,
          VkDeviceSize      dstRowPitchIn,
          VkDeviceSize      dstSlicePitchIn,
          VkImageType       imageType,
          VkExtent3D        imageExtent,
          uint32_t          imageLayers,
    const DxvkFormatInfo*   formatInfo,
          VkImageAspectFlags aspectMask);

  inline VkExtent3D minExtent3D(VkExtent3D a, VkExtent3D b) {
    return VkExtent3D {
      std::min(a.width,  b.width),
      std::min(a.height, b.height),
      std::min(a.depth,  b.depth) };
  }

  inline bool isBlockAligned(VkOffset3D offset, VkExtent3D blockSize) {
    return (offset.x % blockSize.width  == 0)
        && (offset.y % blockSize.height == 0)
        && (offset.z % blockSize.depth  == 0);
  }

  inline bool isBlockAligned(VkOffset3D offset, VkExtent3D extent, VkExtent3D blockSize, VkExtent3D imageSize) {
    return ((extent.width  % blockSize.width  == 0) || (uint32_t(offset.x + extent.width)  == imageSize.width))
        && ((extent.height % blockSize.height == 0) || (uint32_t(offset.y + extent.height) == imageSize.height))
        && ((extent.depth  % blockSize.depth  == 0) || (uint32_t(offset.z + extent.depth)  == imageSize.depth))
        && isBlockAligned(offset, blockSize);
  }

  inline VkExtent3D computeMipLevelExtent(VkExtent3D size, uint32_t level) {
    size.width  = std::max(1u, size.width  >> level);
    size.height = std::max(1u, size.height >> level);
    size.depth  = std::max(1u, size.depth  >> level);
    return size;
  }

  inline VkOffset3D computeMipLevelOffset(VkOffset3D offset, uint32_t level) {
    offset.x  = offset.x >> level;
    offset.y  = offset.y >> level;
    offset.z  = offset.z >> level;
    return offset;
  }

  inline VkExtent3D computeMipLevelExtent(VkExtent3D size, uint32_t level, VkFormat format, VkImageAspectFlags aspect) {
    if (unlikely(!(aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))) {
      auto plane = &lookupFormatInfo(format)->planes[vk::getPlaneIndex(aspect)];
      size.width  /= plane->blockSize.width;
      size.height /= plane->blockSize.height;
    }

    size.width  = std::max(1u, size.width  >> level);
    size.height = std::max(1u, size.height >> level);
    size.depth  = std::max(1u, size.depth  >> level);
    return size;
  }

  inline VkOffset3D computeBlockOffset(VkOffset3D offset, VkExtent3D blockSize) {
    return VkOffset3D {
      offset.x / int32_t(blockSize.width),
      offset.y / int32_t(blockSize.height),
      offset.z / int32_t(blockSize.depth) };
  }

  inline VkExtent3D computeBlockCount(VkExtent3D extent, VkExtent3D blockSize) {
    return VkExtent3D {
      (extent.width  + blockSize.width  - 1) / blockSize.width,
      (extent.height + blockSize.height - 1) / blockSize.height,
      (extent.depth  + blockSize.depth  - 1) / blockSize.depth };
  }

  inline VkExtent3D computeMaxBlockCount(VkOffset3D offset, VkExtent3D extent, VkExtent3D blockSize) {
    return VkExtent3D {
      (extent.width  + blockSize.width  - offset.x - 1) / blockSize.width,
      (extent.height + blockSize.height - offset.y - 1) / blockSize.height,
      (extent.depth  + blockSize.depth  - offset.z - 1) / blockSize.depth };
  }

  inline VkExtent3D snapExtent3D(VkOffset3D offset, VkExtent3D extent, VkExtent3D imageExtent) {
    return VkExtent3D {
      std::min(extent.width,  imageExtent.width  - uint32_t(offset.x)),
      std::min(extent.height, imageExtent.height - uint32_t(offset.y)),
      std::min(extent.depth,  imageExtent.depth  - uint32_t(offset.z)) };
  }

  inline VkExtent3D computeBlockExtent(VkExtent3D blockCount, VkExtent3D blockSize) {
    return VkExtent3D {
      blockCount.width  * blockSize.width,
      blockCount.height * blockSize.height,
      blockCount.depth  * blockSize.depth };
  }

  inline uint32_t flattenImageExtent(VkExtent3D extent) {
    return extent.width * extent.height * extent.depth;
  }

  inline bool isDepthReadOnlyLayout(VkImageLayout layout) {
    return layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        || layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  }

  VkDeviceSize computeImageDataSize(VkFormat format, VkExtent3D extent);
  VkDeviceSize computeImageDataSize(VkFormat format, VkExtent3D extent, VkImageAspectFlags aspects);

  VkColorComponentFlags remapComponentMask(
          VkColorComponentFlags       mask,
          VkComponentMapping          mapping);

  VkComponentMapping invertComponentMapping(VkComponentMapping mapping);

  VkComponentMapping resolveSrcComponentMapping(
          VkComponentMapping          dstMapping,
          VkComponentMapping          srcMapping);

  VkBlendFactor remapAlphaToColorBlendFactor(VkBlendFactor factor);

  bool isIdentityMapping(VkComponentMapping mapping);

  uint32_t getComponentIndex(VkComponentSwizzle component, uint32_t identity);

  VkClearColorValue swizzleClearColor(VkClearColorValue color, VkComponentMapping mapping);

  bool isBlendConstantBlendFactor(VkBlendFactor factor);
  bool isDualSourceBlendFactor(VkBlendFactor factor);

  // setupSampleLocations is omitted: its real body lives in a Vulkan-runtime cpp
  // the shim drops, and the FF-triangle frontend never calls it.

  inline uint32_t computeUnorm(float f, uint32_t bits) {
    f = std::max(f, 0.0f);
    f = std::min(f, 1.0f);
    return uint32_t((f * float((1u << bits) - 1u)) + 0.5f);
  }

  inline uint32_t computeSnorm(float f, uint32_t bits) {
    f = std::max(f, -1.0f);
    f = std::min(f,  1.0f);
    return int32_t((f * float((1u << (bits - 1u)) - 1u)) + (f < 0.0f ? -0.5f : 0.5f));
  }

  inline VkClearColorValue encodeClearBlockValue(VkFormat format, const VkClearColorValue& color) {
    VkClearColorValue result = { };

    switch (format) {
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: {
        result.uint32[0] = (computeUnorm(color.float32[2], 5) <<  0)
                         | (computeUnorm(color.float32[1], 6) <<  5)
                         | (computeUnorm(color.float32[0], 5) << 11);
      } return result;

      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK: {
        uint32_t alpha = 0x11111111u * computeUnorm(color.float32[3], 4);
        result.uint32[0] = alpha;
        result.uint32[1] = alpha;
        result.uint32[2] = (computeUnorm(color.float32[2], 5) <<  0)
                         | (computeUnorm(color.float32[1], 6) <<  5)
                         | (computeUnorm(color.float32[0], 5) << 11);
      } return result;

      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK: {
        result.uint32[0] = computeUnorm(color.float32[3], 8);
        result.uint32[2] = (computeUnorm(color.float32[2], 5) <<  0)
                         | (computeUnorm(color.float32[1], 6) <<  5)
                         | (computeUnorm(color.float32[0], 5) << 11);
      } return result;

      case VK_FORMAT_BC4_SNORM_BLOCK: {
        result.uint32[0] = computeSnorm(color.float32[0], 8);
      } return result;

      case VK_FORMAT_BC4_UNORM_BLOCK: {
        result.uint32[0] = computeUnorm(color.float32[0], 8);
      } return result;

      case VK_FORMAT_BC5_SNORM_BLOCK: {
        result.uint32[0] = computeSnorm(color.float32[0], 8);
        result.uint32[2] = computeSnorm(color.float32[1], 8);
      } return result;

      case VK_FORMAT_BC5_UNORM_BLOCK: {
        result.uint32[0] = computeUnorm(color.float32[0], 8);
        result.uint32[2] = computeUnorm(color.float32[1], 8);
      } return result;

      default:
        return color;
    }
  }

}
