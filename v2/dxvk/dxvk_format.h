#pragma once

// Shim of DXVK's format layer. The frontend (d3d9_format.cpp, d3d9_swapchain.cpp,
// the resource/image code) maps D3DFORMAT -> VkFormat -> DxvkFormatInfo, then reads
// elementSize / aspectMask / componentMask / flags off the returned struct. We keep
// the REAL Vulkan format enums (per SHIM_SPEC rule 2) and the exact struct layout the
// frontend expects from dxvk-ref/dxvk_format.h, but back lookupFormatInfo() with a
// canned table covering only the formats the one-triangle path can reach
// (BGRA8 / RGBA8 backbuffers + D24S8 depth). Unknown formats resolve to a zeroed entry,
// which is harmless because the triangle path never samples them.
//
// Signatures (struct fields, DxvkFormatCount, lookupFormatInfo / lookupFormatInfoSlow /
// getLinearFormat / getDefaultResolveMode) match dxvk-ref/dxvk_format.h verbatim so the
// unmodified frontend compiles and links against them.

#include "dxvk_include.h"

namespace dxvk {

  enum class DxvkFormatFlag {
    BlockCompressed = 0,  ///< Image format is block compressed
    SampledUInt     = 1,  ///< Sampled type is an unsigned integer type
    SampledSInt     = 2,  ///< Sampled type is a signed integer type
    ColorSpaceSrgb  = 3,  ///< Non-linear SRGB color format
    MultiPlane      = 4,  ///< Multi-plane format
  };

  using DxvkFormatFlags = Flags<DxvkFormatFlag>;

  /**
   * \brief Format support info
   */
  struct DxvkFormatFeatures {
    VkFormatFeatureFlags2 optimal;
    VkFormatFeatureFlags2 linear;
    VkFormatFeatureFlags2 buffer;
  };

  /**
   * \brief Format support limits for a given set of image usage flags
   */
  struct DxvkFormatLimits {
    VkExtent3D                  maxExtent;
    uint32_t                    maxMipLevels;
    uint32_t                    maxArrayLayers;
    VkSampleCountFlags          sampleCounts;
    VkDeviceSize                maxResourceSize;
    VkExternalMemoryFeatureFlags externalFeatures;
  };

  /**
   * \brief Format query info
   */
  struct DxvkFormatQuery {
    VkFormat                    format;
    VkImageType                 type;
    VkImageTiling               tiling;
    VkImageUsageFlags           usage;
    VkImageCreateFlags          flags;
    VkExternalMemoryHandleTypeFlagBits handleType;
  };

  /**
   * \brief Planar format info
   */
  struct DxvkPlaneFormatInfo {
    /// Byte size of a pixel in the current plane
    VkDeviceSize elementSize = 0;
    /// Number of image pixels covered by a
    /// single pixel in the current plane
    VkExtent2D blockSize = { 1, 1 };
  };

  /**
   * \brief Format info structure
   *
   * Provides some useful information
   * about a Vulkan image format.
   */
  struct DxvkFormatInfo {
    /// Size of an element in this format. For compressed
    /// formats, this is the size of a block, in bytes.
    VkDeviceSize elementSize = 0;

    /// Available component mask
    VkColorComponentFlags componentMask = 0;

    /// Available image aspect flags
    VkImageAspectFlags aspectMask = 0;

    /// Some other format info flags
    DxvkFormatFlags flags = 0;

    /// Size, in pixels, of a compressed block. For
    /// non-block formats, all these values are 1.
    VkExtent3D blockSize = { 1, 1, 1 };

    /// Plane info for multi-planar formats
    std::array<DxvkPlaneFormatInfo, 3> planes;
  };

  /// Number of formats defined in lookup table
  constexpr size_t DxvkFormatCount = 157;

  /// Format lookup table (defined in dxvk_format.cpp)
  extern const std::array<DxvkFormatInfo, DxvkFormatCount> g_formatInfos;

  /**
   * \brief Looks up format info (slow path / out-of-table formats)
   * \param [in] format Format to look up
   * \returns Info for the given format
   */
  const DxvkFormatInfo* lookupFormatInfoSlow(VkFormat format);

  /**
   * \brief Queries image format info
   *
   * Provides a fast path for the most common base formats.
   * \param [in] format Format to look up
   * \returns Info for the given format
   */
  inline const DxvkFormatInfo* lookupFormatInfo(VkFormat format) {
    if (likely(format <= VK_FORMAT_BC7_SRGB_BLOCK))
      return &g_formatInfos[uint32_t(format)];
    else
      return lookupFormatInfoSlow(format);
  }

  /**
   * \brief Queries default resolve mode for format
   * \param [in] format Format to look up
   * \returns Default resolve mode
   */
  inline VkResolveModeFlagBits getDefaultResolveMode(const DxvkFormatInfo* format) {
    if ((format->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
     || (format->flags.any(DxvkFormatFlag::SampledSInt, DxvkFormatFlag::SampledUInt)))
      return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

    return VK_RESOLVE_MODE_AVERAGE_BIT;
  }

  inline VkResolveModeFlagBits getDefaultResolveMode(VkFormat format) {
    return getDefaultResolveMode(lookupFormatInfo(format));
  }

  /**
   * \brief Queries non-sRGB format for a given format
   * \param [in] format Format
   * \returns Linear format that corresponds to \c format.
   */
  VkFormat getLinearFormat(VkFormat format);

}

// Pulled in AFTER lookupFormatInfo is declared above: dxvk_util.h's inline
// computeMipLevelExtent(format,aspect) depends on it. The frontend reaches the
// dxvk::util math helpers transitively through dxvk_format.h, matching real DXVK.
#include "dxvk_util.h"
