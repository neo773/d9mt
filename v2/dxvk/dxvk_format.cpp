#include "dxvk_format.h"

// Canned format table for the shim. DXVK's real table has 157 entries describing
// every Vulkan format it understands; here we only need the handful the one-triangle
// D3D9 path actually looks up: the BGRA8/RGBA8 backbuffer formats and the D24S8 depth
// format. Everything else resolves to a zeroed DxvkFormatInfo, which is safe because the
// triangle path never reads element sizes / aspects off those formats.
//
// We populate by VkFormat index (real Vulkan enum values) via a builder so the table
// stays correct regardless of the exact numeric value of each enum. lookupFormatInfo()
// (header) indexes g_formatInfos directly for format <= VK_FORMAT_BC7_SRGB_BLOCK.

namespace dxvk {

  static std::array<DxvkFormatInfo, DxvkFormatCount> buildFormatInfos() {
    std::array<DxvkFormatInfo, DxvkFormatCount> table;
    // Fill with default-constructed entries so unset formats keep a valid
    // blockSize {1,1,1} (and plane blockSize {1,1}) — a zero block size would
    // divide-by-zero in the frontend's extent math. = {} would zero those out.
    table.fill(DxvkFormatInfo { });

    auto set = [&] (VkFormat format, const DxvkFormatInfo& info) {
      if (uint32_t(format) < DxvkFormatCount)
        table[uint32_t(format)] = info;
    };

    // 8-bit single channel (used by some D3D9 luminance/alpha mappings).
    set(VK_FORMAT_R8_UNORM, DxvkFormatInfo {
      1, VK_COLOR_COMPONENT_R_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT });

    set(VK_FORMAT_R8G8_UNORM, DxvkFormatInfo {
      2, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT });

    const VkColorComponentFlags rgba =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // BGRA8 backbuffer formats (D3DFMT_A8R8G8B8 / X8R8G8B8 -> these).
    set(VK_FORMAT_B8G8R8A8_UNORM, DxvkFormatInfo {
      4, rgba, VK_IMAGE_ASPECT_COLOR_BIT });
    set(VK_FORMAT_B8G8R8A8_SRGB, DxvkFormatInfo {
      4, rgba, VK_IMAGE_ASPECT_COLOR_BIT, DxvkFormatFlag::ColorSpaceSrgb });

    // RGBA8 (D3DFMT_A8B8G8R8 / X8B8G8R8 -> these).
    set(VK_FORMAT_R8G8B8A8_UNORM, DxvkFormatInfo {
      4, rgba, VK_IMAGE_ASPECT_COLOR_BIT });
    set(VK_FORMAT_R8G8B8A8_SRGB, DxvkFormatInfo {
      4, rgba, VK_IMAGE_ASPECT_COLOR_BIT, DxvkFormatFlag::ColorSpaceSrgb });

    // Block-compressed formats (D3DFMT_DXT1/2/3/4/5 -> BC1/2/3). The element
    // size is the bytes per 4x4 block: 8 for BC1, 16 for BC2/BC3.
    set(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, DxvkFormatInfo {
      8, rgba, VK_IMAGE_ASPECT_COLOR_BIT, 0, { 4, 4, 1 } });
    set(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, DxvkFormatInfo {
      8, rgba, VK_IMAGE_ASPECT_COLOR_BIT, DxvkFormatFlag::ColorSpaceSrgb, { 4, 4, 1 } });
    set(VK_FORMAT_BC2_UNORM_BLOCK, DxvkFormatInfo {
      16, rgba, VK_IMAGE_ASPECT_COLOR_BIT, 0, { 4, 4, 1 } });
    set(VK_FORMAT_BC2_SRGB_BLOCK, DxvkFormatInfo {
      16, rgba, VK_IMAGE_ASPECT_COLOR_BIT, DxvkFormatFlag::ColorSpaceSrgb, { 4, 4, 1 } });
    set(VK_FORMAT_BC3_UNORM_BLOCK, DxvkFormatInfo {
      16, rgba, VK_IMAGE_ASPECT_COLOR_BIT, 0, { 4, 4, 1 } });
    set(VK_FORMAT_BC3_SRGB_BLOCK, DxvkFormatInfo {
      16, rgba, VK_IMAGE_ASPECT_COLOR_BIT, DxvkFormatFlag::ColorSpaceSrgb, { 4, 4, 1 } });

    // Packed 16-bit color formats the swapchain may select.
    set(VK_FORMAT_R5G6B5_UNORM_PACK16, DxvkFormatInfo {
      2, rgba, VK_IMAGE_ASPECT_COLOR_BIT });
    set(VK_FORMAT_A1R5G5B5_UNORM_PACK16, DxvkFormatInfo {
      2, rgba, VK_IMAGE_ASPECT_COLOR_BIT });

    // Depth-stencil formats (D3DFMT_D24S8 / D24X8 / INTZ -> D24_UNORM_S8_UINT).
    set(VK_FORMAT_D16_UNORM, DxvkFormatInfo {
      2, 0, VK_IMAGE_ASPECT_DEPTH_BIT });
    set(VK_FORMAT_D32_SFLOAT, DxvkFormatInfo {
      4, 0, VK_IMAGE_ASPECT_DEPTH_BIT });
    set(VK_FORMAT_D24_UNORM_S8_UINT, DxvkFormatInfo {
      4, 0, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT });
    set(VK_FORMAT_D32_SFLOAT_S8_UINT, DxvkFormatInfo {
      8, 0, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT });

    return table;
  }

  const std::array<DxvkFormatInfo, DxvkFormatCount> g_formatInfos = buildFormatInfos();


  const DxvkFormatInfo* lookupFormatInfoSlow(VkFormat format) {
    // Out-of-table formats are not on the triangle path; return a zeroed entry.
    static const DxvkFormatInfo s_unknown = { };
    return &s_unknown;
  }


  VkFormat getLinearFormat(VkFormat format) {
    // Map the sRGB variants we care about back to their UNORM counterparts; all other
    // formats are already linear for our purposes.
    switch (format) {
      case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
      case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
      default:                      return format;
    }
  }

}
