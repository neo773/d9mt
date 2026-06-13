// d9mt: Metal backend — Presenter (CAMetalLayer) + DxvkSwapchainBlitter.
//
// Present flow (BACKEND-SURFACE §5.1):
//   app thread:  presenter->acquireNextImage(sync, image)
//                  -> returns the PROXY image (B8G8R8A8 private texture,
//                     recreated on extent/sync-interval change; the layer's
//                     drawable size always equals the proxy extent)
//   CS thread:   blitter->present(cmdList, dstView(proxy), dstRect,
//                                 srcView(backbuffer), srcRect)
//                  -> fullscreen-triangle sample pass into the proxy
//                     (or blit-encoder copy when 1:1 and same format)
//                ctx->flushCommandList   (commits the frame's work)
//                device->presentImage -> presenter->presentImage(frameId)
//                  -> nextDrawable, blit proxy -> drawable, presentDrawable,
//                     commit, watcher callback -> signalFrame(frameId)
//   watcher:     signalFrame: fps limit + m_signal->signal(frameId)
//
// LIVENESS: signalFrame(frameId) fires on EVERY presentImage path — failure
// paths signal through the watcher's pure-callback entry (cmdbuf == 0),
// which runs after all previously watched command buffers retired, so the
// signal still happens "after the GPU caught up" and stays monotonic.
//
// HWND channel: the Presenter learns its window through the surface proc:
// the CreatePresenter lambda calls wsi::createSurface with our fake
// vki()->getLoaderProc(), whose vkCreateWin32SurfaceKHR smuggles the HWND
// back as the VkSurfaceKHR value (see d9mt_instance.cpp). m_surface == HWND.

#include <cstring>
#include <unordered_map>
#include <vector>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_presenter.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_swapchain_blitter.h"
#include "../../vendor/dxvk/src/dxvk/hud/dxvk_hud.h"
#include "../../vendor/dxvk/src/wsi/wsi_window.h"

namespace dxvk::d9mt {

  // ==========================================================================
  // Presenter side state: Metal window objects + the proxy image. The
  // vendored Presenter members are Vulkan-shaped; everything Metal lives
  // here, keyed by the Presenter pointer (same pattern as cmdListState).
  //
  // Locking: state.mutex guards layer/proxy against the CS thread
  // (presentImage) racing the app thread (acquireNextImage/destroyResources).
  // The vendored preferred-extent/format/dirty members are app-thread-only.
  // ==========================================================================

  struct PresenterState {
    std::mutex   mutex;
    HWND         hwnd  = nullptr;
    obj_handle_t view  = 0;   // CreateMetalViewFromHWND; ReleaseMetalView to free
    obj_handle_t layer = 0;   // owned by the view, not separately retained
    Rc<DxvkImage> proxy;
    VkExtent2D   proxyExtent = { };
  };

  namespace {
    std::mutex s_presenterMutex;
    std::unordered_map<const void*, std::unique_ptr<PresenterState>> s_presenterStates;

    PresenterState& presenterState(const void* presenter) {
      std::lock_guard<std::mutex> lock(s_presenterMutex);
      auto& slot = s_presenterStates[presenter];
      if (!slot)
        slot = std::make_unique<PresenterState>();
      return *slot;
    }

    void erasePresenterState(const void* presenter) {
      std::lock_guard<std::mutex> lock(s_presenterMutex);
      s_presenterStates.erase(presenter);
    }
  }


  // ==========================================================================
  // blit pipeline: MSL compiled at runtime through d9mtmetal's
  // newLibraryWithSource (winemetal has no MSL-source entry point), one PSO
  // per destination pixel format. Process-global: one MTLDevice per process,
  // cache intentionally lives for the process lifetime.
  // ==========================================================================

  namespace {

    const char g_blitShaderMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct d9mt_blit_params {
  float2 uv_offset;
  float2 uv_scale;
};

struct d9mt_blit_vout {
  float4 pos [[position]];
  float2 uv;
};

struct DxvkGammaCp {
  ushort r, g, b, a;
};

vertex d9mt_blit_vout d9mt_blit_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  d9mt_blit_vout o;
  o.uv  = uv;
  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

fragment float4 d9mt_blit_ps(d9mt_blit_vout in [[stage_in]],
                             constant d9mt_blit_params& p [[buffer(0)]],
                             texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return src.sample(s, p.uv_offset + in.uv * p.uv_scale);
}

fragment float4 d9mt_blit_ps_point(d9mt_blit_vout in [[stage_in]],
                                   constant d9mt_blit_params& p [[buffer(0)]],
                                   texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::nearest, address::clamp_to_edge);
  return src.sample(s, p.uv_offset + in.uv * p.uv_scale);
}

float3 apply_gamma(float3 color, constant DxvkGammaCp* ramp) {
  float3 index = color * 255.0f;
  int3 i0 = clamp(int3(index), 0, 255);
  int3 i1 = min(i0 + 1, 255);
  float3 t = index - float3(i0);
  
  float3 v0 = float3(ramp[i0.x].r, ramp[i0.y].g, ramp[i0.z].b) / 65535.0f;
  float3 v1 = float3(ramp[i1.x].r, ramp[i1.y].g, ramp[i1.z].b) / 65535.0f;
  return mix(v0, v1, t);
}

fragment float4 d9mt_blit_ps_gamma(d9mt_blit_vout in [[stage_in]],
                                   constant d9mt_blit_params& p [[buffer(0)]],
                                   texture2d<float> src [[texture(0)]],
                                   constant DxvkGammaCp* ramp [[buffer(1)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 color = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  color.rgb = apply_gamma(color.rgb, ramp);
  return color;
}

fragment float4 d9mt_blit_ps_point_gamma(d9mt_blit_vout in [[stage_in]],
                                         constant d9mt_blit_params& p [[buffer(0)]],
                                         texture2d<float> src [[texture(0)]],
                                         constant DxvkGammaCp* ramp [[buffer(1)]]) {
  constexpr sampler s(filter::nearest, address::clamp_to_edge);
  float4 color = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  color.rgb = apply_gamma(color.rgb, ramp);
  return color;
}
)";

    std::mutex   s_blitMutex;
    bool         s_blitInitFailed = false;
    obj_handle_t s_blitLibrary = 0;
    obj_handle_t s_blitVs = 0;
    obj_handle_t s_blitPs = 0;
    obj_handle_t s_blitPsPoint = 0;
    obj_handle_t s_blitPsGamma = 0;
    obj_handle_t s_blitPsPointGamma = 0;
    std::vector<std::pair<uint32_t, obj_handle_t>> s_blitPsoCache;

    bool ensureBlitFunctionsLocked() {
      if (s_blitVs && s_blitPs && s_blitPsGamma && s_blitPsPointGamma)
        return true;
      if (s_blitInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_blitInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_blitShaderMsl));
      lp.source_len = sizeof(g_blitShaderMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: blitter: newLibraryWithSource failed");
        logf("d9mt: blitter: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: blitter MSL compile", lp.ret_error);
        s_blitInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_blitLibrary = lp.ret_library;
      s_blitVs = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_vs");
      s_blitPs = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps");
      s_blitPsPoint = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_point");
      s_blitPsGamma = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_gamma");
      s_blitPsPointGamma = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_point_gamma");

      if (!s_blitVs || !s_blitPs || !s_blitPsPoint || !s_blitPsGamma || !s_blitPsPointGamma) {
        Logger::err("d9mt: blitter: blit functions missing from compiled library");
        s_blitInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: also used by DxvkContext::blitImageView / copyImage /
  // resolveImage (declared in d9mt_backend.h)
  obj_handle_t getBlitPso(WMTPixelFormat dstFormat, bool pointFilter, bool useGamma) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    uint32_t key = uint32_t(dstFormat) | (pointFilter ? 0x80000000u : 0u) | (useGamma ? 0x40000000u : 0u);

    for (const auto& e : s_blitPsoCache) {
      if (e.first == key)
        return e.second;
    }

    if (!ensureBlitFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.colors[0].pixel_format = dstFormat;
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.colors[0].blending_enabled = false;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.depth_pixel_format = WMTPixelFormatInvalid;
    info.stencil_pixel_format = WMTPixelFormatInvalid;
    info.vertex_function = s_blitVs;
    info.fragment_function = useGamma ? (pointFilter ? s_blitPsPointGamma : s_blitPsGamma)
                                      : (pointFilter ? s_blitPsPoint : s_blitPs);
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: blitter: newRenderPipelineState failed");
      logNSError("d9mt: blitter PSO", err);
      return 0;
    }

    s_blitPsoCache.push_back({ key, pso });
    return pso;
  }


  // Looks up the WMT pixel format backing a Vulkan view format.
  WMTPixelFormat wmtFormatFor(VkFormat format) {
    const FormatCaps* caps = lookupFormatCaps(format);
    return caps ? caps->wmtFormat : WMTPixelFormatInvalid;
  }


  // ==========================================================================
  // depth(+stencil) SAMPLE_ZERO resolve pipeline (DxvkContext::resolveImage):
  // fullscreen triangle reading sample 0 of the MSAA source and exporting
  // [[depth(any)]] (+ [[stencil]], shader stencil export — supported on all
  // Apple-silicon Metal GPUs) into the 1x destination's depth/stencil
  // attachments. Kept in its OWN MTLLibrary so a hypothetical stencil-export
  // compile failure cannot take down the color blit pipeline above.
  // ==========================================================================

  namespace {

    const char g_depthResolveMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 d9mt_resolve_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

struct d9mt_resolve_d_out {
  float depth [[depth(any)]];
};

fragment d9mt_resolve_d_out d9mt_resolve_d_ps(
    float4 pos [[position]],
    depth2d_ms<float> src [[texture(0)]]) {
  d9mt_resolve_d_out o;
  o.depth = src.read(uint2(pos.xy), 0);
  return o;
}

struct d9mt_resolve_ds_out {
  float depth [[depth(any)]];
  uint stencil [[stencil]];
};

fragment d9mt_resolve_ds_out d9mt_resolve_ds_ps(
    float4 pos [[position]],
    depth2d_ms<float> src [[texture(0)]],
    texture2d_ms<uint> stc [[texture(1)]]) {
  d9mt_resolve_ds_out o;
  o.depth   = src.read(uint2(pos.xy), 0);
  o.stencil = stc.read(uint2(pos.xy), 0).x;
  return o;
}
)";

    bool         s_resolveInitFailed = false;
    obj_handle_t s_resolveLibrary = 0;
    obj_handle_t s_resolveVs = 0;
    obj_handle_t s_resolveDPs = 0;
    obj_handle_t s_resolveDsPs = 0;
    obj_handle_t s_resolveDPso = 0;
    obj_handle_t s_resolveDsPso = 0;

    bool ensureResolveFunctionsLocked() {
      if (s_resolveVs)
        return true;
      if (s_resolveInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_resolveInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_depthResolveMsl));
      lp.source_len = sizeof(g_depthResolveMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: depth resolve: newLibraryWithSource failed");
        logf("d9mt: depth resolve: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: depth resolve MSL compile", lp.ret_error);
        s_resolveInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_resolveLibrary = lp.ret_library;
      s_resolveVs   = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_vs");
      s_resolveDPs  = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_d_ps");
      s_resolveDsPs = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_ds_ps");

      if (!s_resolveVs || !s_resolveDPs || !s_resolveDsPs) {
        Logger::err("d9mt: depth resolve: functions missing from compiled library");
        s_resolveInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: used by DxvkContext::resolveImage (declared in d9mt_backend.h).
  // All backend depth formats unify on Depth32Float_Stencil8, so there is
  // exactly one PSO per (with/without stencil export) variant.
  obj_handle_t getDepthResolvePso(bool withStencil) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    obj_handle_t& cached = withStencil ? s_resolveDsPso : s_resolveDPso;
    if (cached)
      return cached;

    if (!ensureResolveFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.depth_pixel_format   = WMTPixelFormatDepth32Float_Stencil8;
    info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
    info.vertex_function = s_resolveVs;
    info.fragment_function = withStencil ? s_resolveDsPs : s_resolveDPs;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: depth resolve: newRenderPipelineState failed");
      logNSError("d9mt: depth resolve PSO", err);
      return 0;
    }

    cached = pso;
    return pso;
  }


  // ==========================================================================
  // partial depth/stencil clear pipeline (DxvkContext::clearImageView):
  // fullscreen triangle at z = 0 with a void fragment function. The clear
  // depth value is encoded as viewport znear == zfar (depth written =
  // znear + z_ndc * (zfar - znear) = znear), the stencil clear value as the
  // DSSO stencil reference with op Replace; the scissor rect restricts the
  // clear to the requested region. Own MTLLibrary: a compile failure here
  // must not take down the blit / resolve pipelines above.
  // ==========================================================================

  namespace {

    const char g_dsClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 d9mt_dsclear_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fragment void d9mt_dsclear_fs() {}
)";

    bool         s_dsClearInitFailed = false;
    obj_handle_t s_dsClearLibrary = 0;
    obj_handle_t s_dsClearVs = 0;
    obj_handle_t s_dsClearFs = 0;
    std::vector<std::pair<uint32_t, obj_handle_t>> s_dsClearPsoCache;

    bool ensureDsClearFunctionsLocked() {
      if (s_dsClearVs)
        return true;
      if (s_dsClearInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_dsClearInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_dsClearMsl));
      lp.source_len = sizeof(g_dsClearMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: ds clear: newLibraryWithSource failed");
        logf("d9mt: ds clear: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: ds clear MSL compile", lp.ret_error);
        s_dsClearInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_dsClearLibrary = lp.ret_library;
      s_dsClearVs = MTLLibrary_newFunction(s_dsClearLibrary, "d9mt_dsclear_vs");
      s_dsClearFs = MTLLibrary_newFunction(s_dsClearLibrary, "d9mt_dsclear_fs");

      if (!s_dsClearVs || !s_dsClearFs) {
        Logger::err("d9mt: ds clear: functions missing from compiled library");
        s_dsClearInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: used by DxvkContext::clearImageView (declared in
  // d9mt_backend.h). All backend depth formats unify on
  // Depth32Float_Stencil8, so the PSO is keyed by sample count alone.
  obj_handle_t getDepthStencilClearPso(uint32_t sampleCount) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    for (const auto& e : s_dsClearPsoCache) {
      if (e.first == sampleCount)
        return e.second;
    }

    if (!ensureDsClearFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.rasterization_enabled = true;
    info.raster_sample_count = uint8_t(sampleCount ? sampleCount : 1u);
    info.depth_pixel_format   = WMTPixelFormatDepth32Float_Stencil8;
    info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
    info.vertex_function = s_dsClearVs;
    info.fragment_function = s_dsClearFs;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: ds clear: newRenderPipelineState failed");
      logNSError("d9mt: ds clear PSO", err);
      return 0;
    }

    s_dsClearPsoCache.push_back({ sampleCount, pso });
    return pso;
  }


  // ==========================================================================
  // Blitter side state (gamma/cursor metadata + log-once flags). The
  // vendored members are Vulkan-resource-shaped; we only need a few bools.
  // Guarded by the blitter's own vendored m_mutex (per BACKEND-SURFACE §5.2).
  // ==========================================================================

  struct BlitterState {
    bool cursorTextureSet = false;
    bool warnedGamma  = false;
    bool warnedCursor = false;
    bool warnedMsaa   = false;
    std::vector<DxvkGammaCp> gammaRamp;
  };

  namespace {
    std::mutex s_blitterMutex;
    std::unordered_map<const void*, std::unique_ptr<BlitterState>> s_blitterStates;

    BlitterState& blitterState(const void* blitter) {
      std::lock_guard<std::mutex> lock(s_blitterMutex);
      auto& slot = s_blitterStates[blitter];
      if (!slot)
        slot = std::make_unique<BlitterState>();
      return *slot;
    }

    void eraseBlitterState(const void* blitter) {
      std::lock_guard<std::mutex> lock(s_blitterMutex);
      s_blitterStates.erase(blitter);
    }
  }

} // namespace dxvk::d9mt


namespace dxvk {

  // ==========================================================================
  // Presenter
  // ==========================================================================

  Presenter::Presenter(
    const Rc<DxvkDevice>&   device,
    const Rc<sync::Signal>& signal,
    const PresenterDesc&    desc,
          PresenterSurfaceProc&& proc)
  : m_device(device), m_signal(signal), m_surfaceProc(std::move(proc)) {
    auto& state = d9mt::presenterState(this);

    if (!desc.deferSurfaceCreation) {
      std::lock_guard<std::mutex> lock(state.mutex);
      VkResult vr = createSurface();
      if (vr != VK_SUCCESS)
        Logger::err(str::format("d9mt: Presenter: deferred surface creation after error ", vr));
    }

    d9mt::logf("Presenter: created (deferSurfaceCreation=%d)",
      desc.deferSurfaceCreation ? 1 : 0);
  }


  Presenter::~Presenter() {
    destroyResources();
    d9mt::erasePresenterState(this);
    d9mt::logf("Presenter: destroyed");
  }


  VkResult Presenter::acquireNextImage(
          PresenterSync&  sync,
          Rc<DxvkImage>&  image) {
    sync = PresenterSync();

    auto& state = d9mt::presenterState(this);
    std::lock_guard<std::mutex> lock(state.mutex);

    // Recreate the surface if it was invalidated (multi-surface hack)
    if (m_dirtySurface) {
      destroySwapchain();
      destroySurface();
      m_dirtySurface = false;
    }

    if (!state.layer) {
      VkResult vr = createSurface();
      if (vr != VK_SUCCESS)
        return vr;
    }

    VkResult vr = recreateSwapChain();
    if (vr != VK_SUCCESS)
      return vr;

    image = state.proxy;
    m_acquireStatus = VK_SUCCESS;
    return VK_SUCCESS;
  }


  VkResult Presenter::presentImage(
          uint64_t                frameId,
    const Rc<DxvkLatencyTracker>& tracker) {
    auto& state = d9mt::presenterState(this);

    VkResult status = VK_SUCCESS;
    bool signalQueued = false;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    {
      std::lock_guard<std::mutex> lock(state.mutex);

      obj_handle_t queue = d9mt::mtlCommandQueue();

      if (!state.layer || state.proxy == nullptr || !queue) {
        status = VK_ERROR_OUT_OF_DATE_KHR;
      } else {
        obj_handle_t drawable = MetalLayer_nextDrawable(state.layer);

        if (!drawable) {
          d9mt::logf("Presenter: nextDrawable returned null");
          status = VK_ERROR_OUT_OF_DATE_KHR;
        } else {
          obj_handle_t drawTex = MetalDrawable_texture(drawable);
          obj_handle_t cmdbuf  = MTLCommandQueue_commandBuffer(queue);

          if (!drawTex || !cmdbuf) {
            d9mt::logf("Presenter: drawable texture / command buffer unavailable");
            status = VK_ERROR_DEVICE_LOST;
          } else {
            // proxy -> drawable; the layer's drawable size always matches
            // the proxy extent (both set under this lock in recreateSwapChain)
            wmtcmd_blit_copy_from_texture_to_texture cp = { };
            cp.type = WMTBlitCommandCopyFromTextureToTexture;
            cp.src = obj_handle_t(state.proxy->handle());
            cp.src_size = { state.proxyExtent.width, state.proxyExtent.height, 1u };
            cp.dst = drawTex;

            obj_handle_t benc = MTLCommandBuffer_blitCommandEncoder(cmdbuf);
            if (benc) {
              MTLBlitCommandEncoder_encodeCommands(benc,
                reinterpret_cast<const wmtcmd_base*>(&cp));
              MTLCommandEncoder_endEncoding(benc);
            }

            MTLCommandBuffer_presentDrawable(cmdbuf, drawable);
            MTLCommandBuffer_commit(cmdbuf);

            // Frame signal fires when the present command buffer retires.
            // Keep presenter + proxy alive until then.
            Rc<Presenter>  self  = this;
            Rc<DxvkImage>  proxy = state.proxy;

            d9mt::watchCommandBuffer(cmdbuf, [self, proxy, frameId, tracker] {
              self->signalFrame(frameId, tracker);
            });

            signalQueued = true;
          }
        }
      }
    }
    NSObject_release(pool);

    if (!signalQueued) {
      // LIVENESS: failed presents still signal, ordered behind all
      // previously submitted GPU work (watcher pure-callback path)
      Rc<Presenter> self = this;
      d9mt::watchCommandBuffer(0, [self, frameId, tracker] {
        self->signalFrame(frameId, tracker);
      });
    }

    return status;
  }


  void Presenter::signalFrame(
          uint64_t                frameId,
    const Rc<DxvkLatencyTracker>& tracker) {
    // Runs on the watcher thread, after the present command buffer (and all
    // prior GPU work) retired — satisfies the "GPU work has completed"
    // precondition. Presents retire in submission order, so frameId stays
    // monotonic per presenter.
    m_fpsLimiter.delay();

    if (tracker != nullptr)
      tracker->notifyGpuPresentEnd(frameId);

    m_signal->signal(frameId);
    m_lastSignaled = frameId;
  }


  void Presenter::setSyncInterval(uint32_t syncInterval) {
    // CAMetalLayer only knows vsync on/off; clamp like upstream FIFO/IMMEDIATE
    if (syncInterval > 1u)
      syncInterval = 1u;

    if (syncInterval != m_preferredSyncInterval) {
      m_preferredSyncInterval = syncInterval;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setFrameRateLimit(double frameRate, uint32_t maxLatency) {
    m_fpsLimiter.setTargetFrameRate(frameRate, maxLatency);
  }


  void Presenter::setSurfaceFormat(VkSurfaceFormatKHR format) {
    // The proxy is always B8G8R8A8_UNORM (BGRA8 layer); other formats are
    // converted by the blitter's sample pass. Warn once about precision
    // loss for wide formats.
    if (format.format != m_preferredFormat.format) {
      if (format.format != VK_FORMAT_UNDEFINED
       && format.format != VK_FORMAT_B8G8R8A8_UNORM
       && format.format != VK_FORMAT_R8G8B8A8_UNORM
       && format.format != VK_FORMAT_B8G8R8A8_SRGB
       && format.format != VK_FORMAT_R8G8B8A8_SRGB) {
        Logger::warn(str::format("d9mt: Presenter: surface format ", format.format,
          " presented through a B8G8R8A8 proxy (precision loss possible)"));
      }
    }

    m_preferredFormat = format;
  }


  void Presenter::setSurfaceExtent(VkExtent2D extent) {
    if (extent.width  != m_preferredExtent.width
     || extent.height != m_preferredExtent.height) {
      m_preferredExtent = extent;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setHdrMetadata(VkHdrMetadataEXT hdrMetadata) {
    // HDR color spaces are not supported (supportsColorSpace), so the
    // metadata can never become active; store it for completeness
    m_hdrMetadata = hdrMetadata;
    m_hdrMetadataDirty = false;
  }


  bool Presenter::supportsColorSpace(VkColorSpaceKHR colorspace) {
    return colorspace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  }


  void Presenter::invalidateSurface() {
    // applied on the next acquireNextImage (app thread, like this call)
    m_dirtySurface = true;
  }


  void Presenter::destroyResources() {
    // block until pending swapchain operations (presents, proxy blits)
    // have completed, then tear down the Metal window objects
    d9mt::watcherWaitIdle();

    auto& state = d9mt::presenterState(this);
    std::lock_guard<std::mutex> lock(state.mutex);

    destroySwapchain();
    destroySurface();
  }


  // --------------------------------------------------------------------------
  // private helpers — all assume the side-state mutex is held by the caller
  // --------------------------------------------------------------------------

  VkResult Presenter::createSurface() {
    auto& state = d9mt::presenterState(this);

    if (state.layer)
      return VK_SUCCESS;

    if (!m_surface) {
      // The proc routes through wsi::createSurface over our fake instance
      // dispatch; the resulting VkSurfaceKHR value IS the HWND.
      VkResult vr = m_surfaceProc(&m_surface);

      if (vr != VK_SUCCESS || !m_surface) {
        Logger::err(str::format("d9mt: Presenter: surface proc failed: ", vr));
        m_surface = VK_NULL_HANDLE;
        return vr != VK_SUCCESS ? vr : VK_ERROR_SURFACE_LOST_KHR;
      }
    }

    state.hwnd = reinterpret_cast<HWND>(uintptr_t(m_surface));

    obj_handle_t device = d9mt::mtlDevice();
    if (!device)
      return VK_ERROR_SURFACE_LOST_KHR;

    obj_handle_t layer = 0;
    obj_handle_t view = CreateMetalViewFromHWND(
      intptr_t(state.hwnd), device, &layer);

    if (!view || !layer) {
      Logger::err("d9mt: Presenter: CreateMetalViewFromHWND failed");
      d9mt::logf("Presenter: CreateMetalViewFromHWND failed (hwnd=%p)",
        reinterpret_cast<void*>(state.hwnd));
      if (view)
        ReleaseMetalView(view);
      return VK_ERROR_SURFACE_LOST_KHR;
    }

    state.view  = view;
    state.layer = layer;

    // swapchain (proxy + layer drawable size) is created on first acquire
    m_dirtySwapchain = true;

    d9mt::logf("Presenter: surface created (hwnd=%p layer=%llx)",
      reinterpret_cast<void*>(state.hwnd), (unsigned long long)layer);
    return VK_SUCCESS;
  }


  void Presenter::destroySurface() {
    auto& state = d9mt::presenterState(this);

    if (state.view)
      ReleaseMetalView(state.view);

    state.view  = 0;
    state.layer = 0;
    state.hwnd  = nullptr;

    m_surface = VK_NULL_HANDLE;
  }


  void Presenter::destroySwapchain() {
    auto& state = d9mt::presenterState(this);

    state.proxy = nullptr;
    state.proxyExtent = { };
  }


  VkResult Presenter::recreateSwapChain() {
    auto& state = d9mt::presenterState(this);

    VkExtent2D extent = m_preferredExtent;
    if (!extent.width || !extent.height)
      wsi::getWindowSize(state.hwnd, &extent.width, &extent.height);

    if (!extent.width || !extent.height)
      return VK_NOT_READY;

    if (state.proxy != nullptr
     && state.proxyExtent.width  == extent.width
     && state.proxyExtent.height == extent.height
     && !m_dirtySwapchain)
      return VK_SUCCESS;

    // resize the layer's drawables; framebuffer_only=false is REQUIRED
    // (the drawable is a blit destination, not a render target)
    WMTLayerProps props = { };
    MetalLayer_getProps(state.layer, &props);
    props.device = d9mt::mtlDevice();
    props.contents_scale = 1.0;
    props.drawable_width  = extent.width;
    props.drawable_height = extent.height;
    props.opaque = true;
    props.display_sync_enabled = m_preferredSyncInterval != 0u;
    props.framebuffer_only = false;
    props.pixel_format = WMTPixelFormatBGRA8Unorm;
    MetalLayer_setProps(state.layer, &props);

    if (state.proxy == nullptr
     || state.proxyExtent.width  != extent.width
     || state.proxyExtent.height != extent.height) {
      DxvkImageCreateInfo info;
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = VK_FORMAT_B8G8R8A8_UNORM;
      info.flags       = 0u;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { extent.width, extent.height, 1u };
      info.numLayers   = 1u;
      info.mipLevels   = 1u;
      info.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      info.stages      = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                       | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                       | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access      = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                       | VK_ACCESS_SHADER_READ_BIT
                       | VK_ACCESS_TRANSFER_READ_BIT
                       | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;
      info.colorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      info.debugName   = "d9mt presenter proxy";

      try {
        state.proxy = m_device->createImage(info,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      } catch (const DxvkError& e) {
        Logger::err(str::format("d9mt: Presenter: proxy creation failed: ", e.message()));
        state.proxy = nullptr;
        return VK_NOT_READY;
      }

      state.proxyExtent = extent;

      // Zero-init the fresh proxy so partial first presents (letterboxed
      // dstRect) read defined black borders. Watched (empty callback) so
      // destroyResources' watcherWaitIdle accounts for it.
      obj_handle_t queue = d9mt::mtlCommandQueue();
      obj_handle_t pool  = NSAutoreleasePool_alloc_init();
      obj_handle_t cmdbuf = queue ? MTLCommandQueue_commandBuffer(queue) : 0;

      if (cmdbuf) {
        WMTRenderPassInfo pass = { };
        pass.render_target_width  = extent.width;
        pass.render_target_height = extent.height;
        pass.colors[0].texture = obj_handle_t(state.proxy->handle());
        pass.colors[0].load_action = WMTLoadActionClear;
        pass.colors[0].store_action = WMTStoreActionStore;
        pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };

        obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(cmdbuf, &pass);
        if (enc)
          MTLCommandEncoder_endEncoding(enc);

        MTLCommandBuffer_commit(cmdbuf);
        d9mt::watchCommandBuffer(cmdbuf, [] { });
      }
      NSObject_release(pool);

      d9mt::logf("Presenter: proxy %ux%u created (vsync=%d)",
        extent.width, extent.height, m_preferredSyncInterval != 0u ? 1 : 0);
    }

    m_dirtySwapchain = false;
    return VK_SUCCESS;
  }


  // ==========================================================================
  // DxvkSwapchainBlitter
  // ==========================================================================

  DxvkSwapchainBlitter::DxvkSwapchainBlitter(
    const Rc<DxvkDevice>& device,
    const Rc<hud::Hud>&   hud)
  : m_device(device), m_hud(hud) {
    d9mt::blitterState(this);
  }


  DxvkSwapchainBlitter::~DxvkSwapchainBlitter() {
    // command lists track every image the blitter touched; the global blit
    // PSO cache outlives all blitters by design — nothing to release here
    d9mt::eraseBlitterState(this);
  }


  void DxvkSwapchainBlitter::present(
    const Rc<DxvkCommandList>&ctx,
    const Rc<DxvkImageView>&  dstView,
          VkRect2D            dstRect,
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    auto& bs = d9mt::blitterState(this);

    if (srcView == nullptr || dstView == nullptr) {
      Logger::err("d9mt: blitter: null view");
      return;
    }

    obj_handle_t srcHandle = obj_handle_t(srcView->handle());
    obj_handle_t dstHandle = obj_handle_t(dstView->handle());

    if (!srcHandle || !dstHandle) {
      Logger::err("d9mt: blitter: view has no Metal texture");
      return;
    }

    bool useGamma = (m_gammaCpCount != 0 && !bs.gammaRamp.empty());

    // unimplemented composition features — fail loud, keep presenting
    if (bs.cursorTextureSet && m_cursorRect.extent.width && !bs.warnedCursor) {
      bs.warnedCursor = true;
      Logger::err("d9mt: blitter: software cursor composition not implemented — ignored");
    }

    if (srcView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      if (!bs.warnedMsaa) {
        bs.warnedMsaa = true;
        Logger::err("d9mt: blitter: multisampled present source not implemented — skipping blit");
      }
      return;
    }

    VkExtent3D dstExtent = dstView->mipLevelExtent(0);
    VkExtent3D srcExtent = srcView->mipLevelExtent(0);

    bool fullDst = dstRect.offset.x == 0 && dstRect.offset.y == 0
                && dstRect.extent.width  == dstExtent.width
                && dstRect.extent.height == dstExtent.height;

    WMTPixelFormat srcFormat = d9mt::wmtFormatFor(srcView->info().format);
    WMTPixelFormat dstFormat = d9mt::wmtFormatFor(dstView->info().format);

    // lifetime + waitForResource tracking (BACKEND-SURFACE §1.8)
    ctx->track(srcView->image(), DxvkAccess::Read);
    ctx->track(dstView->image(), DxvkAccess::Write);

    // fast path: 1:1 same-format copy through the blit encoder. Swizzled
    // views can't be blitted on Metal (the draw path samples them instead).
    if (srcFormat != WMTPixelFormatInvalid
     && srcFormat == dstFormat
     && srcRect.extent.width  == dstRect.extent.width
     && srcRect.extent.height == dstRect.extent.height
     && !srcView->info().packedSwizzle
     && !dstView->info().packedSwizzle
     && !useGamma) {
      wmtcmd_blit_copy_from_texture_to_texture cp = { };
      cp.type = WMTBlitCommandCopyFromTextureToTexture;
      cp.src = srcHandle;
      cp.src_origin = { uint32_t(srcRect.offset.x), uint32_t(srcRect.offset.y), 0u };
      cp.src_size = { dstRect.extent.width, dstRect.extent.height, 1u };
      cp.dst = dstHandle;
      cp.dst_origin = { uint32_t(dstRect.offset.x), uint32_t(dstRect.offset.y), 0u };

      obj_handle_t enc = d9mt::cmdListGetBlitEncoder(ctx.ptr());
      if (enc)
        MTLBlitCommandEncoder_encodeCommands(enc,
          reinterpret_cast<const wmtcmd_base*>(&cp));
      return;
    }

    // general path: fullscreen-triangle sample pass (scaling + format
    // conversion + view swizzles, linear filtering)
    if (dstFormat == WMTPixelFormatInvalid) {
      Logger::err(str::format("d9mt: blitter: unsupported destination format ",
        dstView->info().format));
      return;
    }

    obj_handle_t pso = d9mt::getBlitPso(dstFormat, false, useGamma);
    if (!pso)
      return;

    WMTRenderPassInfo pass = { };
    pass.render_target_width  = dstExtent.width;
    pass.render_target_height = dstExtent.height;
    pass.colors[0].texture = dstHandle;
    pass.colors[0].store_action = WMTStoreActionStore;
    if (fullDst) {
      pass.colors[0].load_action = WMTLoadActionDontCare;
    } else {
      // letterbox: clear the uncovered border to opaque black
      pass.colors[0].load_action = WMTLoadActionClear;
      pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };
    }

    obj_handle_t enc = d9mt::cmdListBeginRenderPass(ctx.ptr(), pass);
    if (!enc)
      return;

    d9mt::BlitParams params = { };
    params.uvOffset[0] = float(srcRect.offset.x) / float(srcExtent.width);
    params.uvOffset[1] = float(srcRect.offset.y) / float(srcExtent.height);
    params.uvScale[0]  = float(srcRect.extent.width)  / float(srcExtent.width);
    params.uvScale[1]  = float(srcRect.extent.height) / float(srcExtent.height);

    wmtcmd_render_setpso setPso = { };
    wmtcmd_render_setviewport setVp = { };
    wmtcmd_render_setscissorrect setSc = { };
    wmtcmd_render_useresource use = { };
    wmtcmd_render_settexture setTex = { };
    wmtcmd_render_setbytes setBytes = { };
    wmtcmd_render_setbytes setGammaBytes = { };
    wmtcmd_render_draw draw = { };

    setPso.type = WMTRenderCommandSetPSO;
    setPso.next.set(&setVp);
    setPso.pso = pso;

    setVp.type = WMTRenderCommandSetViewport;
    setVp.next.set(&setSc);
    setVp.viewport = { double(dstRect.offset.x), double(dstRect.offset.y),
                       double(dstRect.extent.width), double(dstRect.extent.height),
                       0.0, 1.0 };

    setSc.type = WMTRenderCommandSetScissorRect;
    setSc.next.set(&use);
    setSc.scissor_rect = {
      uint64_t(std::max(dstRect.offset.x, 0)),
      uint64_t(std::max(dstRect.offset.y, 0)),
      std::min(uint64_t(dstRect.extent.width),  uint64_t(dstExtent.width)),
      std::min(uint64_t(dstRect.extent.height), uint64_t(dstExtent.height)) };

    use.type = WMTRenderCommandUseResource;
    use.next.set(&setTex);
    use.resource = srcHandle;
    use.usage = WMTResourceUsageRead;
    use.stages = WMTRenderStages(WMTRenderStageFragment);

    setTex.type = WMTRenderCommandSetFragmentTexture;
    setTex.next.set(&setBytes);
    setTex.texture = srcHandle;
    setTex.index = 0;

    setBytes.type = WMTRenderCommandSetFragmentBytes;
    if (useGamma) {
      setBytes.next.set(&setGammaBytes);
      setGammaBytes.type = WMTRenderCommandSetFragmentBytes;
      setGammaBytes.next.set(&draw);
      setGammaBytes.bytes.set(bs.gammaRamp.data());
      setGammaBytes.length = bs.gammaRamp.size() * sizeof(DxvkGammaCp);
      setGammaBytes.index = 1;
    } else {
      setBytes.next.set(&draw);
    }
    setBytes.bytes.set(&params);
    setBytes.length = sizeof(params);
    setBytes.index = 0;

    draw.type = WMTRenderCommandDraw;
    draw.primitive_type = WMTPrimitiveTypeTriangle;
    draw.vertex_start = 0;
    draw.vertex_count = 3;
    draw.instance_count = 1;
    draw.base_instance = 0;

    MTLRenderCommandEncoder_encodeCommands(enc,
      reinterpret_cast<const wmtcmd_base*>(&setPso));

    // close the pass; the front-end flushes the command list right after
    d9mt::cmdListEndEncoder(ctx.ptr());
  }


  void DxvkSwapchainBlitter::setGammaRamp(
          uint32_t            cpCount,
    const DxvkGammaCp*        cpData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    // identity ramps (windowed path, default ramps) are equivalent to
    // "disabled"; only true LUTs are unimplemented (warned at present time)
    bool identity = true;

    if (cpCount && cpData) {
      for (uint32_t i = 0; i < cpCount && identity; i++) {
        uint32_t expected = (uint64_t(i) * 65535u) / std::max(cpCount - 1u, 1u);

        auto close = [expected] (uint16_t v) {
          int32_t d = int32_t(v) - int32_t(expected);
          return d >= -256 && d <= 256;
        };

        identity = close(cpData[i].r) && close(cpData[i].g) && close(cpData[i].b);
      }
    } else {
      cpCount = 0;
    }

    auto& bs = d9mt::blitterState(this);
    m_gammaCpCount = identity ? 0u : cpCount;

    if (m_gammaCpCount) {
      bs.gammaRamp.assign(cpData, cpData + cpCount);
      bs.warnedGamma = false; // re-warn on new ramps
    } else {
      bs.gammaRamp.clear();
    }
  }


  void DxvkSwapchainBlitter::setCursorTexture(
          VkExtent2D          extent,
          VkFormat            format,
    const void*               data) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    auto& bs = d9mt::blitterState(this);

    bs.cursorTextureSet = extent.width && extent.height && data;
    if (bs.cursorTextureSet)
      bs.warnedCursor = false;
  }


  void DxvkSwapchainBlitter::setCursorPos(
          VkRect2D            rect) {
    // called on the CS thread (BACKEND-SURFACE §5.2) — hence the mutex
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_cursorRect = rect;
  }


  // ==========================================================================
  // hud — disabled on this backend; a null Hud makes every HudItem/
  // HudRenderer path unreachable (upstream returns nullptr when no HUD
  // elements are enabled, and all front-end uses are null-guarded)
  // ==========================================================================

  namespace hud {

    Rc<Hud> Hud::createHud(const Rc<DxvkDevice>& device) {
      return nullptr;
    }

  }

}
