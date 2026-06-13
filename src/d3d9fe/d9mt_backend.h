// d9mt: shared Metal-backend header for the DXVK v2.7.1 d3d9 front-end
// (d3d9fe.dll).  See docs/METAL-BACKEND-NOTES.md for the architecture and
// docs/BACKEND-SURFACE.md for the contract this backend implements.
//
//   app (32-bit PE) -> d3d9fe.dll (vendored DXVK front-end + this backend)
//     -> winemetal.dll (DXMT's wine-builtin bridge) -> winemetal.so -> Metal
//
// Everything here is process-global plumbing shared by all backend TUs:
//  - file logger (d3d9fe.log in the process cwd; winewrapper swallows stderr)
//  - lazy global Metal device + command queue (init pattern copied from the
//    working hand-rolled driver, src/d3d9/d3d9.cpp D9MTDevice::Init)
//  - VkFormat -> WMTPixelFormat capability table (drives DxvkAdapter::
//    getFormatFeatures / getFormatLimits and later texture creation)

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>

#include "../winemetal.h"
#include "../d9mtmetal/d9mtmetal.h"

// Vendored Vulkan type definitions, configured exactly like the rest of the
// front-end build (vulkan_loader.h self-defines VK_USE_PLATFORM_WIN32_KHR).
#include "../../vendor/dxvk/src/vulkan/vulkan_loader.h"

namespace dxvk::d9mt {

  // -------------------------------------------------------------------------
  // logging: d3d9fe.log in the process cwd (same pattern as d9mt.log in the
  // hand-rolled driver). stderr is swallowed by winewrapper, so the file is
  // the only reliable channel.
  // -------------------------------------------------------------------------

  inline void logf(const char* fmt, ...) {
#ifndef D9MT_NO_LOG
    static std::mutex s_mutex;
    static FILE*      s_file = nullptr;

    std::lock_guard<std::mutex> lock(s_mutex);

    if (!s_file) {
      s_file = std::fopen("d3d9fe.log", "w");
      if (!s_file)
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(s_file, fmt, ap);
    va_end(ap);
    std::fputc('\n', s_file);
    std::fflush(s_file);
#else
    (void) fmt;
#endif
  }

  // Logs and releases an NSError handle (winemetal convention: out-params
  // return retained NSError objects the caller must release).
  inline void logNSError(const char* what, obj_handle_t err) {
    if (!err) {
      logf("%s: failed with no NSError", what);
      return;
    }
    obj_handle_t desc = NSObject_description(err);
    char buf[512] = { };
    if (desc) {
      NSString_getCString(desc, buf, sizeof(buf), WMTUTF8StringEncoding);
      NSObject_release(desc);
    }
    logf("%s: %s", what, buf);
    NSObject_release(err);
  }

  // -------------------------------------------------------------------------
  // lazy global Metal objects. The whole process drives a single MTLDevice +
  // MTLCommandQueue (mirrors D9MTDevice::Init in src/d3d9/d3d9.cpp).
  // Returns 0 on failure; callers must fail loud (throw DxvkError / log).
  // -------------------------------------------------------------------------

  inline obj_handle_t mtlDevice() {
    static obj_handle_t s_device = []() -> obj_handle_t {
      obj_handle_t pool = NSAutoreleasePool_alloc_init();

      obj_handle_t devices = WMTCopyAllDevices();
      obj_handle_t device  = 0;

      if (devices && NSArray_count(devices) != 0) {
        device = NSArray_object(devices, 0);
        NSObject_retain(device);
      }
      if (devices)
        NSObject_release(devices);

      if (device) {
        char name[128] = { };
        obj_handle_t nameStr = MTLDevice_name(device);
        if (nameStr)
          NSString_getCString(nameStr, name, sizeof(name), WMTUTF8StringEncoding);
        logf("d9mt: Metal device '%s' (handle %llx)", name,
          (unsigned long long)device);
      } else {
        logf("d9mt: no Metal devices (WMTCopyAllDevices empty)");
      }

      NSObject_release(pool);
      return device;
    }();
    return s_device;
  }

  inline obj_handle_t mtlCommandQueue() {
    static obj_handle_t s_queue = []() -> obj_handle_t {
      obj_handle_t device = mtlDevice();
      if (!device)
        return 0;
      obj_handle_t queue = MTLDevice_newCommandQueue(device, 8);
      if (!queue)
        logf("d9mt: MTLDevice_newCommandQueue failed");
      return queue;
    }();
    return s_queue;
  }

  // Queries the Metal device name into buf (UTF-8), with fallback.
  inline void mtlDeviceName(char* buf, size_t size) {
    buf[0] = 0;
    obj_handle_t device = mtlDevice();
    if (device) {
      obj_handle_t pool = NSAutoreleasePool_alloc_init();
      obj_handle_t nameStr = MTLDevice_name(device);
      if (nameStr)
        NSString_getCString(nameStr, buf, size, WMTUTF8StringEncoding);
      NSObject_release(pool);
    }
    if (!buf[0])
      std::snprintf(buf, size, "Apple GPU");
  }

  // The fake VkPhysicalDevice handle backing our single adapter. Dispatchable
  // handle = pointer on i686; points at a private static so it is non-null
  // and stable, but must NEVER be dereferenced (no Vulkan exists here).
  inline VkPhysicalDevice vkPhysicalDevice() {
    static char s_dummy;
    return reinterpret_cast<VkPhysicalDevice>(&s_dummy);
  }

  // Repoints the vendored wsi::Win32WSI bootstrap factory at our
  // D9mtWsiDriver (CrossOver fullscreen reality: sane even-dimension mode
  // list, succeed-by-emulation display-mode sets — see d9mt_wsi.cpp).
  // Must run before wsi::init(); called from the DxvkInstance ctor.
  void installWsiDriver();

  // -------------------------------------------------------------------------
  // VkFormat -> WMTPixelFormat capability table.
  //
  // Covers every VkFormat d3d9_format.cpp ConvertFormatUnfixed can emit plus
  // the UINT alias formats used for BC-block clears. Depth is unified on
  // Depth32Float_Stencil8 (same decision as the hand-rolled driver: one DS
  // pixel format for all passes/PSOs; Apple silicon has no D24 anyway).
  // D24S8/D16S8 are reported unsupported so the front-end's own fallback
  // (D3D9VkFormatTable ctor) selects D32_SFLOAT_S8_UINT.
  //
  // wmtFormat == WMTPixelFormatInvalid means "format not supported" --
  // getFormatFeatures reports 0 and getFormatLimits returns nullopt, which
  // the front-end handles gracefully (CheckDeviceFormat -> NOTAVAILABLE).
  // -------------------------------------------------------------------------

  struct FormatCaps {
    VkFormat              vkFormat;
    WMTPixelFormat        wmtFormat;      // Invalid = unsupported
    WMTPixelFormat        wmtFormatSrgb;  // sRGB sibling, Invalid if none
    VkFormatFeatureFlags2 optimal;
    VkFormatFeatureFlags2 linear;
    VkFormatFeatureFlags2 buffer;
  };

  // nullptr for formats not in the table (treat as unsupported).
  const FormatCaps* lookupFormatCaps(VkFormat format);

  // Looks up the WMT pixel format backing a Vulkan view format
  // (Invalid = unsupported). Implemented in d9mt_presenter.cpp.
  WMTPixelFormat wmtFormatFor(VkFormat format);

  // -------------------------------------------------------------------------
  // fullscreen-triangle sample-blit pipeline (d9mt_presenter.cpp): one PSO
  // per destination WMTPixelFormat (+ filter), used by the swapchain blitter
  // and by DxvkContext::blitImageView / resolveImage. The fragment shader
  // samples texture(0) at uv_offset + uv * uv_scale ([[buffer(0)]] params);
  // the viewport does the scaling/placement.
  // -------------------------------------------------------------------------

  struct BlitParams {
    float uvOffset[2];
    float uvScale[2];
  };

  obj_handle_t getBlitPso(WMTPixelFormat dstFormat, bool pointFilter = false, bool useGamma = false);

  // Depth(+stencil) SAMPLE_ZERO resolve PSO (d9mt_presenter.cpp): fullscreen
  // triangle exporting [[depth(any)]] (+ [[stencil]]) read from sample 0 of
  // an MSAA Depth32Float_Stencil8 source. Used by DxvkContext::resolveImage.
  obj_handle_t getDepthResolvePso(bool withStencil);

  // Partial depth/stencil clear PSO (d9mt_presenter.cpp): fullscreen triangle
  // with z = 0 and a void fragment function; the clear depth value is encoded
  // as viewport znear == zfar, the stencil value as the DSSO stencil
  // reference (op Replace). One PSO per raster sample count. Used by
  // DxvkContext::clearImageView for scissored single-aspect DS clears.
  obj_handle_t getDepthStencilClearPso(uint32_t sampleCount);

  // -------------------------------------------------------------------------
  // built-in compute pipelines (DxvkDevice::createBuiltInComputePipeline,
  // d9mt_device.cpp): the D3D9FormatHelper YUV/mixed-format upload conversion
  // shaders. The VkPipeline handle returned to the front-end is a pointer to
  // this struct; ~D3D9FormatHelper releases it through the fake
  // vkDestroyPipeline dispatch entry.
  // -------------------------------------------------------------------------

  struct BuiltInComputePipeline {
    obj_handle_t pso     = 0;            // MTLComputePipelineState, retained
    obj_handle_t library = 0;            // MTLLibrary, retained
    WMTSize      threadgroupSize = { 1u, 1u, 1u }; // SPIR-V LocalSize
  };

  // -------------------------------------------------------------------------
  // completion watcher (d9mt_watcher.cpp) — the GPU-liveness backbone
  // (BACKEND-SURFACE §5.1). winemetal has no generic completion-callback
  // export, so a single background thread waits on submitted command buffers
  // in FIFO order (one queue ⇒ retirement order == submission order) and runs
  // the registered callbacks (sync::Signal signals, query resolves,
  // track-release of resources, presenter frame signals).
  //
  // watchCommandBuffer() RETAINS cmdbuf internally and releases it after the
  // callback ran; cmdbuf == 0 enqueues a pure callback that runs after all
  // previously watched command buffers retired (empty submissions still
  // signal — BACKEND-SURFACE §7 risk 6).
  // -------------------------------------------------------------------------

  void watchCommandBuffer(obj_handle_t cmdbuf, std::function<void()> callback);

  // Blocks until every watched command buffer has retired and all callbacks
  // have run. Backs DxvkDevice::waitForIdle.
  void watcherWaitIdle();

  // -------------------------------------------------------------------------
  // command-list encoder bridge (implemented in d9mt_context.cpp, welded to
  // the command-list side state there). Used by the swapchain blitter
  // (d9mt_presenter.cpp) to encode raw Metal work onto a DxvkCommandList
  // obtained via DxvkContext::beginExternalRendering. The key is the
  // DxvkCommandList pointer.
  // -------------------------------------------------------------------------

  // Returns the list's blit encoder, opening one (and ending any other
  // encoder) if needed. 0 on failure.
  obj_handle_t cmdListGetBlitEncoder(const void* list);

  // Ends any open encoder and begins a render pass on the list's command
  // buffer. The returned encoder is owned by the side state (kind = Render);
  // it is closed by cmdListEndEncoder or any subsequent encoder switch.
  // 0 on failure.
  obj_handle_t cmdListBeginRenderPass(const void* list, WMTRenderPassInfo& pass);

  // Ends whatever encoder is currently open on the list.
  void cmdListEndEncoder(const void* list);

  // Compute bridge for the D3D9FormatHelper path (vendored
  // DxvkCommandList::cmdBindPipeline / cmdDispatch route through the fake
  // Vulkan device dispatch in d9mt_device.cpp into these). Both open the
  // list's compute encoder (ending any other encoder) on demand.
  void cmdListBindComputePipeline(const void* list, obj_handle_t pso,
    const WMTSize& threadgroupSize);
  void cmdListDispatch(const void* list, uint32_t x, uint32_t y, uint32_t z);

  // -------------------------------------------------------------------------
  // sampler heap shadow: gpuResourceID of every live MTLSamplerState, indexed
  // by DxvkSampler heap index (== DxvkSamplerDescriptor::samplerIndex). The
  // Draw stage uploads this array as the set-15 sampler heap ([[buffer(2)]]).
  // Written by DxvkSampler ctor/dtor under the sampler pool mutex.
  // -------------------------------------------------------------------------

  constexpr uint32_t SamplerHeapSize = 2048u; // == DxvkSamplerPool::MaxSamplerCount

  // Implemented in d9mt_shader.cpp: the heap shadow lives inside a shared
  // MTLBuffer (VirtualAlloc + bytes-no-copy) so the Draw stage can bind it
  // directly as the set-15 sampler heap without per-draw uploads.
  // samplerHeapData() never returns nullptr (falls back to a plain array if
  // buffer creation fails; samplerHeapBuffer() is 0 in that case).
  uint64_t*    samplerHeapData();
  obj_handle_t samplerHeapBuffer();

}
