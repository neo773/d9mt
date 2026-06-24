#pragma once

// Metal-backed shim for DXVK's image layer (replaces dxvk-ref/dxvk_image.h).
//
// App textures are STUBBED: sampling is not on the fixed-function triangle
// path, so DxvkImage/DxvkImageView for ordinary textures are opaque handles
// that satisfy the frontend's compile-time and lifetime contracts but own no
// Metal texture. The exception is the backbuffer / render target: it must hold
// a REAL winemetal MTLTexture so the presenter can draw the triangle into it
// and blit it to the drawable. DxvkImage therefore carries a settable
// `metalTexture` handle (0 for stub textures).
//
// Frontend surface actually used (verified in ../d3d9):
//   DxvkDevice::createImage(DxvkImageCreateInfo, memFlags) -> Rc<DxvkImage>
//   DxvkImage:     info(), createView(key), handle(), mapPtr(off), storage()
//   DxvkImageView: info(), image(), handle()
//   DxvkImageViewKey fields set by the frontend (viewType, format, usage,
//     aspects, layout, mip/layer ranges, packedSwizzle, allowTypeMismatch)
// Everything else (layout tracking, sparse, keyed mutex, relocation) is gone.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "dxvk_hash.h"     // DxvkHashState (DxvkImageViewKey::hash)
#include "dxvk_include.h"
#include "dxvk_backend.h"  // D9mtBackend (real Metal texture creation)
#include "dxvk_buffer.h"   // DxvkResourceAllocation (reused for image storage)
#include "dxvk_format.h"   // DxvkFormatInfo / lookupFormatInfo (formatInfo())

namespace dxvk {

  class DxvkDevice;
  class DxvkImage;
  class DxvkImageView;

  // Win32 shared-handle import/export mode (dxvk-ref/dxvk_image.h). The Metal
  // shim never shares images cross-API; the frontend still writes these fields
  // for D3D9Ex shared surfaces, so we keep the type and the None default.
  enum class DxvkSharedHandleMode {
    None,
    Import,
    Export,
  };

  struct DxvkSharedHandleInfo {
    DxvkSharedHandleMode mode = DxvkSharedHandleMode::None;
    VkExternalMemoryHandleTypeFlagBits type = VkExternalMemoryHandleTypeFlagBits(0);
    HANDLE handle = INVALID_HANDLE_VALUE;
  };

  /**
   * \brief Image create info
   *
   * Full field set the D3D9 frontend writes (d3d9_common_texture.cpp,
   * d3d9_swapchain.cpp). The shim only acts on extent/format for the render
   * target; the rest are recorded so the frontend compiles and so debugging /
   * format queries see consistent values.
   */
  struct DxvkImageCreateInfo {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags flags = 0u;
    VkSampleCountFlagBits sampleCount = VkSampleCountFlagBits(0u);
    VkExtent3D extent = { };
    uint32_t numLayers = 0u;
    uint32_t mipLevels = 0u;
    VkImageUsageFlags usage = 0u;
    VkPipelineStageFlags stages = 0u;
    VkAccessFlags access = 0u;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    VkBool32 shared = VK_FALSE;
    VkBool32 transient = VK_FALSE;
    uint32_t viewFormatCount = 0u;
    const VkFormat* viewFormats = nullptr;
    const char* debugName = nullptr;
    // Cross-API shared-handle config (D3D9Ex shared surfaces). Default = not shared.
    DxvkSharedHandleInfo sharing = { };
  };


  /**
   * \brief Image view key
   *
   * The subset of dxvk-ref's key (dxvk_memory.h) the frontend writes when
   * creating a view. Kept here so the shim's dxvk_image.h is self-contained.
   * Provides hash()/eq() so it works as an unordered_map key like the original,
   * and packSwizzle() because the frontend calls DxvkImageViewKey::packSwizzle.
   */
  struct DxvkImageViewKey {
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkImageUsageFlags usage = VkImageUsageFlags(0u);
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspects = 0u;
    VkBool32 allowTypeMismatch = VK_TRUE;
    uint8_t mipIndex = 0u;
    uint8_t mipCount = 0u;
    uint16_t layerIndex = 0u;
    uint16_t layerCount = 0u;
    uint16_t packedSwizzle = 0u;

    size_t hash() const {
      DxvkHashState h;
      h.add(uint32_t(viewType));
      h.add(uint32_t(usage));
      h.add(uint32_t(format));
      h.add(uint32_t(layout));
      h.add(uint32_t(aspects));
      h.add(uint32_t(allowTypeMismatch));
      h.add(uint32_t(mipIndex) | (uint32_t(mipCount) << 16));
      h.add(uint32_t(layerIndex) | (uint32_t(layerCount) << 16));
      h.add(uint32_t(packedSwizzle));
      return h;
    }

    bool eq(const DxvkImageViewKey& o) const {
      return viewType == o.viewType && usage == o.usage && format == o.format
          && layout == o.layout && aspects == o.aspects
          && allowTypeMismatch == o.allowTypeMismatch
          && mipIndex == o.mipIndex && mipCount == o.mipCount
          && layerIndex == o.layerIndex && layerCount == o.layerCount
          && packedSwizzle == o.packedSwizzle;
    }

    VkComponentMapping unpackSwizzle() const {
      return VkComponentMapping {
        VkComponentSwizzle((packedSwizzle >>  0) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  4) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  8) & 0xf),
        VkComponentSwizzle((packedSwizzle >> 12) & 0xf) };
    }

    static uint16_t packSwizzle(VkComponentMapping m) {
      return (uint16_t(m.r) << 0) | (uint16_t(m.g) << 4)
           | (uint16_t(m.b) << 8) | (uint16_t(m.a) << 12);
    }
  };


  /**
   * \brief Image resource (stub, except render targets)
   *
   * Holds the create info and, for render targets only, a real winemetal
   * MTLTexture handle. Created through \ref DxvkDevice::createImage.
   */
  class DxvkImage : public DxvkPagedResource {

  public:

    /**
     * \brief Creates an image
     * \param [in] createInfo Image properties from the frontend
     * \param [in] metalTexture winemetal MTLTexture handle (0 = stub texture)
     */
    DxvkImage(
      const DxvkImageCreateInfo& createInfo,
            uint64_t             metalTexture = 0u)
    : m_info(createInfo),
      m_metalTexture(metalTexture),
      m_storage(new DxvkResourceAllocation(nullptr)) {
      if (createInfo.debugName)
        m_debugName = createInfo.debugName;
    }

    // Form used by DxvkDevice::createImage(info, backend&). Sampled 2D textures
    // get a real Metal texture so the GPU can sample them; the backbuffer / RT
    // path leaves the texture null here and sets it later via setMetalTexture().
    DxvkImage(const DxvkImageCreateInfo& createInfo, D9mtBackend& backend)
    : DxvkImage(createInfo, 0u) {
      uint32_t metalFormat = metalPixelFormatFromVulkan(createInfo.format);
      bool isSampled2D = (createInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
                      && createInfo.type == VK_IMAGE_TYPE_2D
                      && createInfo.extent.width && createInfo.extent.height;
      if (isSampled2D && metalFormat) {
        // ShaderRead, plus RenderTarget when this is also a color attachment
        // (a render-to-texture target). The backbuffer is a color attachment
        // but NOT sampled, so it stays texture-less here and uses the drawable.
        uint32_t metalUsage = 1u;  // WMTTextureUsageShaderRead
        if (createInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
          metalUsage |= 4u;        // WMTTextureUsageRenderTarget
        m_metalTexture = backend.createSampledTexture(
          createInfo.extent.width, createInfo.extent.height,
          createInfo.mipLevels, metalFormat, metalUsage, &m_gpuResourceId);
      }
    }

    // Maps the D3D9-frontend VkFormat to a winemetal pixel format for sampled
    // textures. Only the formats app textures commonly use are mapped; an
    // unmapped format yields 0 (the image stays a stub, sampling reads zero).
    static uint32_t metalPixelFormatFromVulkan(VkFormat format) {
      switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:      return 80u;   // WMTPixelFormatBGRA8Unorm
        case VK_FORMAT_B8G8R8A8_SRGB:       return 81u;   // WMTPixelFormatBGRA8Unorm_sRGB
        case VK_FORMAT_R8G8B8A8_UNORM:      return 70u;   // WMTPixelFormatRGBA8Unorm
        case VK_FORMAT_R8G8B8A8_SRGB:       return 71u;   // WMTPixelFormatRGBA8Unorm_sRGB
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 130u; // WMTPixelFormatBC1_RGBA
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:  return 131u; // WMTPixelFormatBC1_RGBA_sRGB
        case VK_FORMAT_BC2_UNORM_BLOCK:      return 132u; // WMTPixelFormatBC2_RGBA
        case VK_FORMAT_BC2_SRGB_BLOCK:       return 133u; // WMTPixelFormatBC2_RGBA_sRGB
        case VK_FORMAT_BC3_UNORM_BLOCK:      return 134u; // WMTPixelFormatBC3_RGBA
        case VK_FORMAT_BC3_SRGB_BLOCK:       return 135u; // WMTPixelFormatBC3_RGBA_sRGB
        default:                             return 0u;
      }
    }

    /**
     * \brief Image properties
     * \returns Image create info
     */
    const DxvkImageCreateInfo& info() const {
      return m_info;
    }

    /**
     * \brief Vulkan image handle
     *
     * The shim has no VkImage; returns VK_NULL_HANDLE. Only read by
     * d3d9_interop, which is not on the triangle path.
     * \returns VK_NULL_HANDLE
     */
    VkImage handle() const {
      return VK_NULL_HANDLE;
    }

    /**
     * \brief Underlying Metal texture handle
     *
     * Non-zero only for render targets / the backbuffer; the presenter draws
     * into / blits from this.
     * \returns winemetal obj_handle_t MTLTexture (0 if stub)
     */
    uint64_t metalTexture() const {
      return m_metalTexture;
    }

    /**
     * \brief Assigns a Metal texture (for render targets)
     * \param [in] texture winemetal MTLTexture handle
     */
    void setMetalTexture(uint64_t texture) {
      m_metalTexture = texture;
    }

    // Texture's argument-buffer resource ID (0 for stubs / render targets).
    // Written into a set's argument buffer so a shader can sample this texture.
    uint64_t gpuResourceId() const {
      return m_gpuResourceId;
    }

    /**
     * \brief Map pointer
     *
     * App textures are not host-mapped in the shim; returns nullptr. Linear /
     * mappable textures are not exercised by the triangle.
     * \param [in] offset Byte offset (ignored)
     * \returns nullptr
     */
    void* mapPtr(VkDeviceSize offset) const {
      (void)offset;
      return nullptr;
    }

    /**
     * \brief Mip level extent
     *
     * Standard halving per level, clamped to 1. Inlined here to keep the shim
     * independent of dxvk-ref/dxvk_util.h (which the shim does not keep).
     * \param [in] level Mip level
     * \returns Extent of that level
     */
    VkExtent3D mipLevelExtent(uint32_t level) const {
      return VkExtent3D {
        std::max(m_info.extent.width  >> level, 1u),
        std::max(m_info.extent.height >> level, 1u),
        std::max(m_info.extent.depth  >> level, 1u) };
    }

    /**
     * \brief Retrieves current backing storage
     * \returns Backing storage (carries no mapping for stub textures)
     */
    Rc<DxvkResourceAllocation> storage() const {
      return m_storage;
    }

    // Exported Win32 shared handle — the shim never shares images cross-API.
    HANDLE sharedHandle() const { return INVALID_HANDLE_VALUE; }

    // Backing-memory info (only .size is read, for D3D9 surface accounting).
    struct MemoryInfo { VkDeviceSize size = 0u; };
    MemoryInfo getMemoryInfo() const {
      // Rough size estimate from extent*layers*elementSize; good enough for the
      // frontend's bookkeeping (the shim allocates no real image memory).
      MemoryInfo info;
      info.size = VkDeviceSize(m_info.extent.width) * m_info.extent.height
                * std::max(m_info.extent.depth, 1u) * std::max(m_info.numLayers, 1u) * 4u;
      return info;
    }

    // Format info for this image's format (canned table in dxvk_format.cpp).
    const DxvkFormatInfo* formatInfo() const { return lookupFormatInfo(m_info.format); }

    /**
     * \brief Creates or retrieves an image view
     *
     * Defined out-of-line below (needs the complete DxvkImageView type).
     * \param [in] key View properties
     * \returns Image view
     */
    Rc<DxvkImageView> createView(const DxvkImageViewKey& key);

    /**
     * \brief Sets debug name
     */
    void setDebugName(const char* name) {
      m_debugName = name ? name : "";
    }

    const char* getDebugName() const {
      return m_debugName.c_str();
    }

  private:

    DxvkImageCreateInfo        m_info         = { };
    uint64_t                   m_metalTexture = 0u;
    uint64_t                   m_gpuResourceId = 0u;
    Rc<DxvkResourceAllocation> m_storage;
    std::string                m_debugName;

  };


  /**
   * \brief Image view (stub)
   *
   * Holds the parent image + key. \c image() reaches the image's Metal texture
   * for render-target views (the presenter binds backbuffer views this way);
   * sampling views are inert. \c handle() returns VK_NULL_HANDLE since there is
   * no VkImageView.
   */
  class DxvkImageView : public RcObject {

  public:

    DxvkImageView(
            Rc<DxvkImage>      image,
      const DxvkImageViewKey&  key)
    : m_image(std::move(image)), m_key(key) { }

    /**
     * \brief Image view properties
     */
    DxvkImageViewKey info() const {
      return m_key;
    }

    /**
     * \brief View type
     */
    VkImageViewType type() const {
      return m_key.viewType;
    }

    /**
     * \brief Underlying image
     */
    DxvkImage* image() const {
      return m_image.ptr();
    }

    /**
     * \brief Vulkan view handle (none in the shim)
     * \returns VK_NULL_HANDLE
     */
    VkImageView handle() const {
      return VK_NULL_HANDLE;
    }

    // Opaque descriptor — only used by the dead format-conversion compute path.
    // Returns a static empty descriptor; never dereferenced by the shim.
    const DxvkDescriptor* getDescriptor() const { static DxvkDescriptor d; return &d; }
    const DxvkDescriptor* getDescriptor(VkImageViewType) const { return getDescriptor(); }

    /**
     * \brief Mip level extent relative to the view's base mip
     * \param [in] level Mip level within the view
     * \returns Extent of that level
     */
    VkExtent3D mipLevelExtent(uint32_t level) const {
      return m_image != nullptr
        ? m_image->mipLevelExtent(level + m_key.mipIndex)
        : VkExtent3D { };
    }

    /**
     * \brief Metal texture this view refers to
     *
     * Convenience for the presenter: render-target views forward to the
     * backbuffer's Metal texture.
     * \returns winemetal MTLTexture handle (0 if none)
     */
    uint64_t metalTexture() const {
      return m_image != nullptr ? m_image->metalTexture() : 0u;
    }

    // Argument-buffer resource ID of the underlying texture (for sampling).
    uint64_t gpuResourceId() const {
      return m_image != nullptr ? m_image->gpuResourceId() : 0u;
    }

  private:

    Rc<DxvkImage>    m_image;
    DxvkImageViewKey m_key = { };

  };


  // Out-of-line: DxvkImageView is a complete type only here.
  inline Rc<DxvkImageView> DxvkImage::createView(const DxvkImageViewKey& key) {
    return new DxvkImageView(this, key);
  }

}
