// d9mt: Metal backend — DxvkInstance / DxvkAdapter / DxvkDeviceCapabilities.
//
// Implements the adapter-level surface of docs/BACKEND-SURFACE.md §2.1/§2.2
// against winemetal. One Metal device = one adapter. All capability values
// follow the caps table in docs/METAL-BACKEND-NOTES.md.

#include <cstring>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_adapter.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_instance.h"
#include "../../vendor/dxvk/src/wsi/wsi_platform.h"

namespace dxvk::d9mt {

  // ---------------------------------------------------------------------
  // format capability table (see d9mt_backend.h for the rules)
  // ---------------------------------------------------------------------

  namespace {

    constexpr VkFormatFeatureFlags2 FMT_XFER =
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
      VK_FORMAT_FEATURE_2_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_2_BLIT_DST_BIT;

    constexpr VkFormatFeatureFlags2 FMT_SAMPLE =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;

    constexpr VkFormatFeatureFlags2 FMT_FILTER =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    constexpr VkFormatFeatureFlags2 FMT_RT =
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;

    constexpr VkFormatFeatureFlags2 FMT_BLEND =
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;

    constexpr VkFormatFeatureFlags2 FMT_DS =
      VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;

    constexpr VkFormatFeatureFlags2 FMT_STORAGE =
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;

    // typical renderable + filterable + blendable color format
    constexpr VkFormatFeatureFlags2 COLOR_STD =
      FMT_XFER | FMT_SAMPLE | FMT_FILTER | FMT_RT | FMT_BLEND;

    // 32-bit float formats: Apple GPUs (pre-M3) neither filter nor blend them
    constexpr VkFormatFeatureFlags2 COLOR_32F =
      FMT_XFER | FMT_SAMPLE | FMT_RT;

    // integer formats: renderable, storage-capable, no filtering/blending
    constexpr VkFormatFeatureFlags2 COLOR_UINT =
      FMT_XFER | FMT_SAMPLE | FMT_RT | FMT_STORAGE;

    // BC blocks: sampled only (Metal forbids BC render targets and blits)
    constexpr VkFormatFeatureFlags2 BC_STD =
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
      FMT_SAMPLE | FMT_FILTER;

    // depth: attachment + sampled (PCF/linear compare allowed)
    constexpr VkFormatFeatureFlags2 DEPTH_STD =
      FMT_XFER | FMT_SAMPLE | FMT_FILTER | FMT_DS;

    // texel buffer features for non-packed single/multi channel formats
    constexpr VkFormatFeatureFlags2 BUF_TEXEL =
      VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

    constexpr VkFormatFeatureFlags2 BUF_TEXEL_RW =
      VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT;

    constexpr WMTPixelFormat WMT_NONE = WMTPixelFormatInvalid;

    // {vkFormat, wmt, wmtSrgb, optimal, linear, buffer}
    // linear tiling: same as optimal minus attachment bits (the front-end
    // only ever ORs optimal|linear for support checks, and falls back to
    // LINEAR via getFormatLimits which honors these masks).
    constexpr FormatCaps g_formatCaps[] = {
      // 8-bit
      { VK_FORMAT_R8_UNORM,                 WMTPixelFormatR8Unorm,        WMTPixelFormatR8Unorm_sRGB,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R8_SRGB,                  WMTPixelFormatR8Unorm_sRGB,   WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_R8_UINT,                  WMTPixelFormatR8Uint,         WMT_NONE,
        COLOR_UINT, COLOR_UINT & ~(FMT_RT | FMT_STORAGE), BUF_TEXEL_RW },
      { VK_FORMAT_R8G8_UNORM,               WMTPixelFormatRG8Unorm,       WMTPixelFormatRG8Unorm_sRGB,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R8G8_SNORM,               WMTPixelFormatRG8Snorm,       WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R8G8B8A8_UNORM,           WMTPixelFormatRGBA8Unorm,     WMTPixelFormatRGBA8Unorm_sRGB,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R8G8B8A8_SRGB,            WMTPixelFormatRGBA8Unorm_sRGB, WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_R8G8B8A8_SNORM,           WMTPixelFormatRGBA8Snorm,     WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_B8G8R8A8_UNORM,           WMTPixelFormatBGRA8Unorm,     WMTPixelFormatBGRA8Unorm_sRGB,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_B8G8R8A8_SRGB,            WMTPixelFormatBGRA8Unorm_sRGB, WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },

      // packed 8/16-bit
      // R4G4 (D3DFMT_A4L4) has no Metal equivalent: unsupported.
      { VK_FORMAT_R4G4_UNORM_PACK8,         WMT_NONE, WMT_NONE, 0, 0, 0 },
      // A4R4G4B4 lands in ABGR4 bit order; the (G,B,A,R) component swizzle
      // is applied at view creation (same trick as the hand-rolled driver).
      { VK_FORMAT_A4R4G4B4_UNORM_PACK16,    WMTPixelFormatABGR4Unorm,     WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_A1R5G5B5_UNORM_PACK16,    WMTPixelFormatBGR5A1Unorm,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_B5G6R5_UNORM_PACK16,      WMTPixelFormatB5G6R5Unorm,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      // R5G6B5 (R/B-swapped sibling): same Metal format, swizzled view.
      { VK_FORMAT_R5G6B5_UNORM_PACK16,      WMTPixelFormatB5G6R5Unorm,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },

      // packed 32-bit
      { VK_FORMAT_A2B10G10R10_UNORM_PACK32, WMTPixelFormatRGB10A2Unorm,   WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_A2R10G10B10_UNORM_PACK32, WMTPixelFormatBGR10A2Unorm,   WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },
      { VK_FORMAT_B10G11R11_UFLOAT_PACK32,  WMTPixelFormatRG11B10Float,   WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), 0 },

      // 16-bit per channel
      { VK_FORMAT_R16_UNORM,                WMTPixelFormatR16Unorm,       WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16_UINT,                 WMTPixelFormatR16Uint,        WMT_NONE,
        COLOR_UINT, COLOR_UINT & ~(FMT_RT | FMT_STORAGE), BUF_TEXEL_RW },
      { VK_FORMAT_R16_SFLOAT,               WMTPixelFormatR16Float,       WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16_UNORM,             WMTPixelFormatRG16Unorm,      WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16_SNORM,             WMTPixelFormatRG16Snorm,      WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16_SFLOAT,            WMTPixelFormatRG16Float,      WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16B16A16_UNORM,       WMTPixelFormatRGBA16Unorm,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16B16A16_SNORM,       WMTPixelFormatRGBA16Snorm,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },
      { VK_FORMAT_R16G16B16A16_SFLOAT,      WMTPixelFormatRGBA16Float,    WMT_NONE,
        COLOR_STD, COLOR_STD & ~(FMT_RT | FMT_BLEND), BUF_TEXEL },

      // 32-bit per channel
      { VK_FORMAT_R32_UINT,                 WMTPixelFormatR32Uint,        WMT_NONE,
        COLOR_UINT, COLOR_UINT & ~(FMT_RT | FMT_STORAGE), BUF_TEXEL_RW },
      { VK_FORMAT_R32_SFLOAT,               WMTPixelFormatR32Float,       WMT_NONE,
        COLOR_32F, COLOR_32F & ~FMT_RT, BUF_TEXEL_RW },
      { VK_FORMAT_R32G32_SFLOAT,            WMTPixelFormatRG32Float,      WMT_NONE,
        COLOR_32F, COLOR_32F & ~FMT_RT, BUF_TEXEL },
      { VK_FORMAT_R32G32B32A32_SFLOAT,      WMTPixelFormatRGBA32Float,    WMT_NONE,
        COLOR_32F, COLOR_32F & ~FMT_RT, BUF_TEXEL },
      // UINT aliases used for BC-block clears (clearImageView storage views)
      { VK_FORMAT_R32G32_UINT,              WMTPixelFormatRG32Uint,       WMT_NONE,
        COLOR_UINT, COLOR_UINT & ~(FMT_RT | FMT_STORAGE), BUF_TEXEL_RW },
      { VK_FORMAT_R32G32B32A32_UINT,        WMTPixelFormatRGBA32Uint,     WMT_NONE,
        COLOR_UINT, COLOR_UINT & ~(FMT_RT | FMT_STORAGE), BUF_TEXEL_RW },

      // BC (DXT)
      { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     WMTPixelFormatBC1_RGBA,       WMTPixelFormatBC1_RGBA_sRGB,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC1_RGBA_SRGB_BLOCK,      WMTPixelFormatBC1_RGBA_sRGB,  WMT_NONE,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC2_UNORM_BLOCK,          WMTPixelFormatBC2_RGBA,       WMTPixelFormatBC2_RGBA_sRGB,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC2_SRGB_BLOCK,           WMTPixelFormatBC2_RGBA_sRGB,  WMT_NONE,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC3_UNORM_BLOCK,          WMTPixelFormatBC3_RGBA,       WMTPixelFormatBC3_RGBA_sRGB,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC3_SRGB_BLOCK,           WMTPixelFormatBC3_RGBA_sRGB,  WMT_NONE,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC4_UNORM_BLOCK,          WMTPixelFormatBC4_RUnorm,     WMT_NONE,
        BC_STD, BC_STD, 0 },
      { VK_FORMAT_BC5_UNORM_BLOCK,          WMTPixelFormatBC5_RGUnorm,    WMT_NONE,
        BC_STD, BC_STD, 0 },

      // depth/stencil — unified on Depth32Float_Stencil8 (METAL-BACKEND-
      // NOTES.md). D24S8 + D16S8 intentionally unsupported: the front-end's
      // D3D9VkFormatTable falls back to D32_SFLOAT_S8_UINT on its own.
      { VK_FORMAT_D16_UNORM,                WMTPixelFormatDepth32Float_Stencil8, WMT_NONE,
        DEPTH_STD, 0, 0 },
      { VK_FORMAT_D32_SFLOAT,               WMTPixelFormatDepth32Float_Stencil8, WMT_NONE,
        DEPTH_STD, 0, 0 },
      { VK_FORMAT_D32_SFLOAT_S8_UINT,       WMTPixelFormatDepth32Float_Stencil8, WMT_NONE,
        DEPTH_STD, 0, 0 },
      { VK_FORMAT_D24_UNORM_S8_UINT,        WMT_NONE, WMT_NONE, 0, 0, 0 },
      { VK_FORMAT_D16_UNORM_S8_UINT,        WMT_NONE, WMT_NONE, 0, 0, 0 },

      // video (YUY2/UYVY/NV12/YV12): unsupported until the D3D9FormatHelper
      // compute conversion path is implemented — CheckDeviceFormat reports
      // NOTAVAILABLE, apps fall back to RGB paths.
      { VK_FORMAT_G8B8G8R8_422_UNORM,       WMT_NONE, WMT_NONE, 0, 0, 0 },
      { VK_FORMAT_B8G8R8G8_422_UNORM,       WMT_NONE, WMT_NONE, 0, 0, 0 },
      { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, WMT_NONE, WMT_NONE, 0, 0, 0 },
      { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, WMT_NONE, WMT_NONE, 0, 0, 0 },
    };

  } // anonymous namespace

  const FormatCaps* lookupFormatCaps(VkFormat format) {
    for (const auto& entry : g_formatCaps) {
      if (entry.vkFormat == format)
        return &entry;
    }
    return nullptr;
  }


  // ---------------------------------------------------------------------
  // fake instance dispatch backing DxvkAdapter::vki().
  //
  // There is no Vulkan in this process; the ONLY instance-level entry point
  // anything on the d3d9 path ever calls is vkCreateWin32SurfaceKHR, via
  // the presenter surface proc -> wsi::createSurface (vendored win32 WSI).
  // We use it as the documented, verbatim-safe channel that carries the
  // HWND into our Presenter: VkSurfaceKHR := the HWND itself (u64-extended).
  // Everything else resolves to nullptr (wsi handles that gracefully).
  // ---------------------------------------------------------------------

  // dispatchable handle = pointer on i686; non-null, stable, never deref'd
  VkInstance fakeVkInstance() {
    static char s_dummy;
    return reinterpret_cast<VkInstance>(&s_dummy);
  }

  namespace {

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateWin32SurfaceKHR(
            VkInstance                        instance,
      const VkWin32SurfaceCreateInfoKHR*      pCreateInfo,
      const VkAllocationCallbacks*            pAllocator,
            VkSurfaceKHR*                     pSurface) {
      if (!pCreateInfo || !pCreateInfo->hwnd || !pSurface)
        return VK_ERROR_INITIALIZATION_FAILED;

      *pSurface = static_cast<VkSurfaceKHR>(
        reinterpret_cast<uintptr_t>(pCreateInfo->hwnd));
      return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroySurfaceKHR(
            VkInstance                        instance,
            VkSurfaceKHR                      surface,
      const VkAllocationCallbacks*            pAllocator) {
      // the surface handle is just the HWND; nothing to destroy
    }

  } // anonymous namespace

  PFN_vkVoidFunction VKAPI_CALL fakeGetInstanceProcAddr(
          VkInstance                          instance,
    const char*                               name) {
    if (!std::strcmp(name, "vkCreateWin32SurfaceKHR"))
      return reinterpret_cast<PFN_vkVoidFunction>(&fakeCreateWin32SurfaceKHR);
    if (!std::strcmp(name, "vkDestroySurfaceKHR"))
      return reinterpret_cast<PFN_vkVoidFunction>(&fakeDestroySurfaceKHR);

    logf("d9mt: fakeGetInstanceProcAddr: unsupported function '%s'", name);
    return nullptr;
  }

} // namespace dxvk::d9mt


namespace dxvk::vk {

  // d9mt: real (non-abort) loader pieces for the vki() fake dispatch.
  // LibraryLoader simply carries our fake loader proc; InstanceFn resolves
  // its function-pointer members through InstanceLoader::sym (the aborting
  // trampoline in d9mt_device.cpp), which is fine because nothing on the
  // d3d9 path calls them — getLoaderProc()/instance() are the only
  // consumers (presenter surface proc).

  LibraryLoader::LibraryLoader(PFN_vkGetInstanceProcAddr loaderProc)
  : m_library(nullptr), m_getInstanceProcAddr(loaderProc) {

  }


  InstanceFn::InstanceFn(
    const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : InstanceLoader(library, owned, instance) {

  }


  InstanceFn::~InstanceFn() {

  }

}


namespace dxvk {

  // =========================================================================
  // DxvkInstance
  // =========================================================================

  DxvkInstance::DxvkInstance(DxvkInstanceFlags flags)
  : DxvkInstance(DxvkInstanceImportInfo(), flags) {

  }


  DxvkInstance::DxvkInstance(
    const DxvkInstanceImportInfo& args,
          DxvkInstanceFlags       flags) {
    Logger::info(str::format("d9mt: DxvkInstance (Metal backend), game: ",
      env::getExeName()));
    d9mt::logf("DxvkInstance: init (exe %s)", env::getExeName().c_str());

    // Vulkan import is meaningless on the Metal backend; an import request
    // means an interop client we cannot serve. Fail loud.
    if (args.instance || args.loaderProc)
      throw DxvkError("d9mt: Vulkan instance import not supported by the Metal backend");

    // Win32 WSI driver (window sizing / monitor enumeration / fullscreen).
    // Upstream initializes it here too; without it every wsi::* call
    // null-derefs the driver pointer (s_driver) on first swapchain creation.
    wsi::init();

    // user config (dxvk.conf / env) takes precedence over app profiles,
    // matching upstream (Config::merge keeps existing keys)
    m_config = Config::getUserConfig();
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_config.logOptions();

    // m_options stays default-constructed: dxvk_options.cpp is not built
    // (its options tune the Vulkan backend); defaults are correct for us.

    if (!d9mt::mtlDevice())
      throw DxvkError("d9mt: no Metal device available");

    m_adapters.push_back(new DxvkAdapter(*this, d9mt::vkPhysicalDevice()));

    d9mt::logf("DxvkInstance: ready, 1 adapter");
  }


  DxvkInstance::~DxvkInstance() {
    d9mt::logf("DxvkInstance: destroyed");
    wsi::quit();
  }


  Rc<DxvkAdapter> DxvkInstance::enumAdapters(uint32_t index) const {
    return index < m_adapters.size()
      ? m_adapters[index]
      : nullptr;
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByLuid(const void* luid) const {
    for (const auto& adapter : m_adapters) {
      const auto& vk11 = adapter->deviceProperties().vk11;

      if (vk11.deviceLUIDValid && !std::memcmp(luid, vk11.deviceLUID, VK_LUID_SIZE))
        return adapter;
    }
    return nullptr;
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByDeviceId(
          uint16_t vendorId,
          uint16_t deviceId) const {
    for (const auto& adapter : m_adapters) {
      const auto& props = adapter->deviceProperties().core.properties;

      if (props.vendorID == vendorId && props.deviceID == deviceId)
        return adapter;
    }
    return nullptr;
  }


  // =========================================================================
  // DxvkAdapter
  // =========================================================================

  DxvkAdapter::DxvkAdapter(
          DxvkInstance&     instance,
          VkPhysicalDevice  handle)
  : m_instance(&instance), m_handle(handle),
    m_capabilities(instance, handle, nullptr) {
    d9mt::logf("DxvkAdapter: '%s'",
      m_capabilities.getProperties().core.properties.deviceName);
  }


  DxvkAdapter::~DxvkAdapter() {

  }


  Rc<vk::InstanceFn> DxvkAdapter::vki() const {
    // d9mt: serves EXACTLY ONE purpose — the presenter surface proc
    // (D3D9SwapChainEx::CreatePresenter lambda) routes the HWND through
    // vki()->getLoaderProc() -> wsi::createSurface ->
    // "vkCreateWin32SurfaceKHR". Our fake loader proc answers that with an
    // HWND-smuggling implementation (see d9mt::fakeGetInstanceProcAddr
    // above); every other instance function resolves to nullptr or an
    // aborting trampoline, so genuine Vulkan interop stays loudly
    // unsupported.
    static Rc<vk::InstanceFn> s_vki = [] {
      Rc<vk::LibraryLoader> lib =
        new vk::LibraryLoader(&d9mt::fakeGetInstanceProcAddr);
      return Rc<vk::InstanceFn>(new vk::InstanceFn(
        lib, false, d9mt::fakeVkInstance()));
    }();
    return s_vki;
  }


  bool DxvkAdapter::isCompatible(std::string& error) {
    error.clear();
    return true;
  }


  DxvkAdapterMemoryInfo DxvkAdapter::getMemoryHeapInfo() const {
    const auto& memInfo  = m_capabilities.getMemoryInfo();
    const auto& memProps = memInfo.core.memoryProperties;

    DxvkAdapterMemoryInfo info = { };
    info.heapCount = memProps.memoryHeapCount;

    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
      info.heaps[i].heapFlags       = memProps.memoryHeaps[i].flags;
      info.heaps[i].heapSize        = memProps.memoryHeaps[i].size;
      info.heaps[i].memoryBudget    = memInfo.budget.heapBudget[i]
        ? memInfo.budget.heapBudget[i]
        : memProps.memoryHeaps[i].size;
      info.heaps[i].memoryAllocated = m_memoryStats[i].allocated.load();
    }

    return info;
  }


  DxvkFormatFeatures DxvkAdapter::getFormatFeatures(VkFormat format) const {
    DxvkFormatFeatures result = { };

    auto caps = d9mt::lookupFormatCaps(format);

    if (caps && caps->wmtFormat != WMTPixelFormatInvalid) {
      result.optimal = caps->optimal;
      result.linear  = caps->linear;
      result.buffer  = caps->buffer;
    }

    return result;
  }


  std::optional<DxvkFormatLimits> DxvkAdapter::getFormatLimits(
    const DxvkFormatQuery& query) const {
    auto caps = d9mt::lookupFormatCaps(query.format);

    if (!caps || caps->wmtFormat == WMTPixelFormatInvalid)
      return std::nullopt;

    VkFormatFeatureFlags2 features = query.tiling == VK_IMAGE_TILING_LINEAR
      ? caps->linear
      : caps->optimal;

    if (!features)
      return std::nullopt;

    if (query.handleType) {
      Logger::err(str::format("d9mt: getFormatLimits: shared handles not "
        "supported (handleType ", uint32_t(query.handleType), ")"));
      return std::nullopt;
    }

    // map requested usage to required format features
    VkFormatFeatureFlags2 required = 0u;

    if (query.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
      required |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (query.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
      required |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    if (query.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      required |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (query.usage & VK_IMAGE_USAGE_STORAGE_BIT)
      required |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    if (query.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      required |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (query.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      required |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;

    if ((features & required) != required)
      return std::nullopt;

    DxvkFormatLimits limits = { };

    switch (query.type) {
      case VK_IMAGE_TYPE_1D:
        limits.maxExtent     = { 16384u, 1u, 1u };
        limits.maxMipLevels  = 15u;
        limits.maxArrayLayers = 2048u;
        break;

      case VK_IMAGE_TYPE_2D:
        limits.maxExtent     = { 16384u, 16384u, 1u };
        limits.maxMipLevels  = 15u;
        limits.maxArrayLayers = 2048u;
        break;

      case VK_IMAGE_TYPE_3D:
        limits.maxExtent     = { 2048u, 2048u, 2048u };
        limits.maxMipLevels  = 12u;
        limits.maxArrayLayers = 1u;
        break;

      default:
        Logger::err(str::format("d9mt: getFormatLimits: unhandled image type ",
          uint32_t(query.type)));
        return std::nullopt;
    }

    limits.sampleCounts = VK_SAMPLE_COUNT_1_BIT;

    if (query.type == VK_IMAGE_TYPE_2D
     && query.tiling == VK_IMAGE_TILING_OPTIMAL
     && !(query.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
     && (features & (VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT
                   | VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))) {
      limits.sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT
                          | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
    }

    limits.maxResourceSize  = VkDeviceSize(1u) << 31;
    limits.externalFeatures = 0u;
    return limits;
  }


  DxvkAdapterQueueIndices DxvkAdapter::findQueueFamilies() const {
    DxvkAdapterQueueIndices queues = { };
    queues.graphics = 0u;
    queues.transfer = 0u;
    queues.sparse   = VK_QUEUE_FAMILY_IGNORED;
    return queues;
  }


  bool DxvkAdapter::checkFeatureSupport(const DxvkDeviceFeatures& required) const {
    return true;
  }


  void DxvkAdapter::enableExtensions(const DxvkExtensionList& extensions) {
    // no Vulkan device underneath — extra extensions are meaningless
  }


  void DxvkAdapter::notifyMemoryStats(
          uint32_t  heap,
          int64_t   allocated,
          int64_t   used) {
    if (heap < m_memoryStats.size()) {
      m_memoryStats[heap].allocated += allocated;
      m_memoryStats[heap].used      += used;
    }
  }


  bool DxvkAdapter::matchesDriver(VkDriverIdKHR driver) const {
    return false;
  }


  bool DxvkAdapter::matchesDriver(
          VkDriverIdKHR driver,
          Version       minVer,
          Version       maxVer) const {
    return false;
  }


  bool DxvkAdapter::isUnifiedMemoryArchitecture() const {
    return true;
  }


  // =========================================================================
  // DxvkDeviceCapabilities — the single place all properties, features,
  // memory heaps and queue mappings are defined (caps table in
  // docs/METAL-BACKEND-NOTES.md).
  // =========================================================================

  DxvkDeviceCapabilities::DxvkDeviceCapabilities(
    const DxvkInstance&       instance,
          VkPhysicalDevice    adapter,
    const VkDeviceCreateInfo* deviceInfo) {
    // ------------------------------------------------------------ properties
    auto& core = m_properties.core.properties;

    core.apiVersion    = VK_MAKE_API_VERSION(0, 1, 3, 0);
    core.driverVersion = VK_MAKE_API_VERSION(0, 2, 7, 1);
    core.vendorID      = 0x106bu;   // Apple
    core.deviceID      = 0xa1u;
    core.deviceType    = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    {
      char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 8] = { };
      d9mt::mtlDeviceName(name, sizeof(name));
      std::snprintf(core.deviceName, sizeof(core.deviceName), "%s (d9mt)", name);
    }

    std::memcpy(core.pipelineCacheUUID, "d9mt-metal-be-01", VK_UUID_SIZE);

    auto& limits = core.limits;
    limits.maxImageDimension1D    = 16384u;
    limits.maxImageDimension2D    = 16384u;
    limits.maxImageDimension3D    = 2048u;
    limits.maxImageDimensionCube  = 16384u;
    limits.maxImageArrayLayers    = 2048u;
    limits.maxTexelBufferElements = 1u << 27;
    limits.maxUniformBufferRange  = 65536u;
    limits.maxStorageBufferRange  = 1u << 30;
    limits.maxPushConstantsSize   = 256u;
    limits.maxMemoryAllocationCount  = 1u << 16;
    limits.maxSamplerAllocationCount = 1u << 14;
    limits.bufferImageGranularity    = 16u;
    limits.maxBoundDescriptorSets    = 8u;
    limits.maxPerStageDescriptorSamplers         = 64u;
    limits.maxPerStageDescriptorUniformBuffers   = 32u;
    limits.maxPerStageDescriptorStorageBuffers   = 32u;
    limits.maxPerStageDescriptorSampledImages    = 128u;
    limits.maxPerStageDescriptorStorageImages    = 32u;
    limits.maxPerStageDescriptorInputAttachments = 8u;
    limits.maxPerStageResources                  = 256u;
    limits.maxDescriptorSetSamplers              = 256u;
    limits.maxDescriptorSetUniformBuffers        = 128u;
    limits.maxDescriptorSetUniformBuffersDynamic = 16u;
    limits.maxDescriptorSetStorageBuffers        = 128u;
    limits.maxDescriptorSetStorageBuffersDynamic = 16u;
    limits.maxDescriptorSetSampledImages         = 512u;
    limits.maxDescriptorSetStorageImages         = 128u;
    limits.maxDescriptorSetInputAttachments      = 8u;
    limits.maxVertexInputAttributes       = 32u;
    limits.maxVertexInputBindings         = 32u;
    limits.maxVertexInputAttributeOffset  = 2047u;
    limits.maxVertexInputBindingStride    = 16384u;
    limits.maxVertexOutputComponents      = 128u;
    limits.maxFragmentInputComponents     = 128u;
    limits.maxFragmentOutputAttachments   = 8u;
    limits.maxFragmentDualSrcAttachments  = 1u;
    limits.maxFragmentCombinedOutputResources = 16u;
    limits.maxComputeSharedMemorySize     = 32768u;
    limits.maxComputeWorkGroupCount[0]    = 65535u;
    limits.maxComputeWorkGroupCount[1]    = 65535u;
    limits.maxComputeWorkGroupCount[2]    = 65535u;
    limits.maxComputeWorkGroupInvocations = 1024u;
    limits.maxComputeWorkGroupSize[0]     = 1024u;
    limits.maxComputeWorkGroupSize[1]     = 1024u;
    limits.maxComputeWorkGroupSize[2]     = 64u;
    limits.subPixelPrecisionBits   = 8u;
    limits.subTexelPrecisionBits   = 8u;
    limits.mipmapPrecisionBits     = 8u;
    limits.maxDrawIndexedIndexValue = 0xffffffffu;
    limits.maxDrawIndirectCount     = 1u << 20;
    limits.maxSamplerLodBias        = 16.0f;
    limits.maxSamplerAnisotropy     = 16.0f;
    limits.maxViewports             = 16u;
    limits.maxViewportDimensions[0] = 16384u;
    limits.maxViewportDimensions[1] = 16384u;
    limits.viewportBoundsRange[0]   = -32768.0f;
    limits.viewportBoundsRange[1]   = 32767.0f;
    limits.viewportSubPixelBits     = 8u;
    limits.minMemoryMapAlignment            = 64u;
    limits.minTexelBufferOffsetAlignment    = 16u;
    limits.minUniformBufferOffsetAlignment  = 256u;
    limits.minStorageBufferOffsetAlignment  = 16u;
    limits.minTexelOffset    = -8;
    limits.maxTexelOffset    = 7u;
    limits.minTexelGatherOffset = -8;
    limits.maxTexelGatherOffset = 7u;
    limits.subPixelInterpolationOffsetBits = 4u;
    limits.maxFramebufferWidth  = 16384u;
    limits.maxFramebufferHeight = 16384u;
    limits.maxFramebufferLayers = 2048u;
    limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT
      | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
    limits.framebufferDepthSampleCounts   = limits.framebufferColorSampleCounts;
    limits.framebufferStencilSampleCounts = limits.framebufferColorSampleCounts;
    limits.framebufferNoAttachmentsSampleCounts = limits.framebufferColorSampleCounts;
    limits.maxColorAttachments = 8u;
    limits.sampledImageColorSampleCounts   = limits.framebufferColorSampleCounts;
    limits.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.sampledImageDepthSampleCounts   = limits.framebufferColorSampleCounts;
    limits.sampledImageStencilSampleCounts = limits.framebufferColorSampleCounts;
    limits.storageImageSampleCounts        = VK_SAMPLE_COUNT_1_BIT;
    limits.maxSampleMaskWords  = 1u;
    limits.timestampComputeAndGraphics = VK_TRUE;
    limits.timestampPeriod     = 1.0f;   // GPU timestamps reported in ns
    limits.maxClipDistances    = 8u;
    limits.maxCullDistances    = 0u;
    limits.maxCombinedClipAndCullDistances = 8u;
    limits.discreteQueuePriorities = 2u;
    limits.pointSizeRange[0]   = 1.0f;
    limits.pointSizeRange[1]   = 511.0f;
    limits.lineWidthRange[0]   = 1.0f;
    limits.lineWidthRange[1]   = 1.0f;
    limits.pointSizeGranularity = 0.125f;
    limits.lineWidthGranularity = 0.0f;
    limits.standardSampleLocations = VK_TRUE;
    limits.optimalBufferCopyOffsetAlignment   = 4u;
    limits.optimalBufferCopyRowPitchAlignment = 4u;
    limits.nonCoherentAtomSize = 64u;

    m_properties.vk11.deviceLUIDValid = VK_FALSE;
    m_properties.vk11.subgroupSize    = 32u;
    m_properties.vk11.maxMemoryAllocationSize = VkDeviceSize(2u) << 30;

    m_properties.vk12.driverID = VkDriverId(0);
    std::snprintf(m_properties.vk12.driverName,
      sizeof(m_properties.vk12.driverName), "d9mt Metal backend");
    std::snprintf(m_properties.vk12.driverInfo,
      sizeof(m_properties.vk12.driverInfo), "winemetal");

    // read by the memory allocator / dxso (BACKEND-SURFACE §2.3 properties)
    m_properties.extRobustness2.robustStorageBufferAccessSizeAlignment = 4u;
    m_properties.extRobustness2.robustUniformBufferAccessSizeAlignment = 16u;

    // -------------------------------------------------------------- features
    // Exactly the caps table in METAL-BACKEND-NOTES.md. Everything not
    // listed ON stays VK_FALSE from value-initialization, notably:
    // geometryShader, tessellationShader, depthBounds, logicOp, sparse*,
    // vertexPipelineStoresAndAtomics (+vk12.shaderInt8) => SWVP path off,
    // extGraphicsPipelineLibrary, extCustomBorderColor, extNonSeamlessCubeMap.
    auto& features = m_featuresEnabled;

    features.core.features.robustBufferAccess   = VK_TRUE;
    features.core.features.fullDrawIndexUint32  = VK_TRUE;
    features.core.features.imageCubeArray       = VK_TRUE;
    features.core.features.independentBlend     = VK_TRUE;
    features.core.features.multiViewport        = VK_TRUE;
    features.core.features.samplerAnisotropy    = VK_TRUE;
    features.core.features.textureCompressionBC = VK_TRUE;
    features.core.features.occlusionQueryPrecise = VK_TRUE;
    features.core.features.shaderClipDistance   = VK_TRUE;
    features.core.features.depthClamp           = VK_TRUE;
    features.core.features.depthBiasClamp       = VK_TRUE;
    features.core.features.fillModeNonSolid     = VK_TRUE;

    features.vk13.dynamicRendering  = VK_TRUE;
    features.vk13.synchronization2  = VK_TRUE;
    features.vk13.maintenance4      = VK_TRUE;

    features.extAttachmentFeedbackLoopLayout.attachmentFeedbackLoopLayout = VK_TRUE;

    features.extDepthClipEnable.depthClipEnable = VK_TRUE;

    features.extDepthBiasControl.depthBiasControl = VK_TRUE;
    features.extDepthBiasControl.depthBiasExact   = VK_TRUE;
    features.extDepthBiasControl.floatRepresentation = VK_TRUE;

    features.extRobustness2.robustBufferAccess2 = VK_TRUE;
    features.extRobustness2.nullDescriptor      = VK_TRUE;

    features.khrSwapchain = VK_TRUE;

    // supported == enabled on this backend
    m_featuresSupported = m_featuresEnabled;

    // ---------------------------------------------------------------- memory
    // one unified DEVICE_LOCAL heap, 3 GiB (matches the hand-rolled driver's
    // GetAvailableTextureMem budget for draw distance); three types so
    // memFlags() echo HOST_VISIBLE combos correctly (BACKEND-SURFACE §3.7).
    constexpr VkDeviceSize heapSize = VkDeviceSize(3u) << 30;

    auto& mem = m_memory.core.memoryProperties;
    mem.memoryHeapCount = 1u;
    mem.memoryHeaps[0].size  = heapSize;
    mem.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    mem.memoryTypeCount = 3u;
    mem.memoryTypes[0].heapIndex = 0u;
    mem.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    mem.memoryTypes[1].heapIndex = 0u;
    mem.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    mem.memoryTypes[2].heapIndex = 0u;
    mem.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    m_memory.budget.heapBudget[0] = heapSize;
    m_memory.budget.heapUsage[0]  = 0u;

    // ---------------------------------------------------------------- queues
    // single graphics+compute+transfer family; no sparse binding
    m_queueMapping.graphics = { 0u, 0u };
    m_queueMapping.transfer = { 0u, 0u };
    m_queueMapping.sparse   = { VK_QUEUE_FAMILY_IGNORED, 0u };

    VkQueueFamilyProperties2 family = { VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 };
    family.queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT
      | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    family.queueFamilyProperties.queueCount = 1u;
    family.queueFamilyProperties.timestampValidBits = 64u;
    family.queueFamilyProperties.minImageTransferGranularity = { 1u, 1u, 1u };
    m_queuesAvailable.push_back(family);

    m_queuePriorities.push_back(1.0f);

    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = 0u;
    queueInfo.queueCount       = 1u;
    queueInfo.pQueuePriorities = m_queuePriorities.data();
    m_queuesEnabled.push_back(queueInfo);
  }


  DxvkDeviceCapabilities::~DxvkDeviceCapabilities() {

  }

}
