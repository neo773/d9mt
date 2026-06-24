#include "dxvk_backend.h"

#include <cstdio>
#include <vector>

#include <windows.h>

// Metal is reached ONLY through the winemetal obj_handle_t ABI — never
// metal-cpp (that is host tooling). See SHIM_SPEC.md §4.
#include "winemetal.h"

#include "dxvk_trace.h"   // per-frame CPU draw-path profiler (D9MT_TRACE)
#include "d9mtmetal.h"    // D9MT_FUNC_CAPTURE: programmatic .gputrace capture

// Generated at build time from the embedded triangle MSL:
//   d9mt_triangle_metallib[] + d9mt_triangle_metallib_len.
// Built into build/triangle_metallib.h; the build adds that dir to -I.
#include "triangle_metallib.h"

namespace dxvk {

  namespace {
    // The shim has no logger wired this early; mirror metal_backend's file log
    // so initialize() failures are diagnosable from a fresh boot.
    void logLine(const char* message) {
      FILE* file = fopen("C:\\d9mt-test\\v2.log", "a");
      if (file) {
        fprintf(file, "[dxvk_backend] %s\n", message);
        fclose(file);
      }
    }
  }

  D9mtBackend* g_activeBackend = nullptr;

  D9mtBackend::D9mtBackend() = default;

  D9mtBackend::~D9mtBackend() {
    if (m_pipelineState) NSObject_release(m_pipelineState);
    if (m_library)       NSObject_release(m_library);
    if (m_queue)         NSObject_release(m_queue);
    // Device, layer, and view are owned by the windowing system / winemetal.
  }

  bool D9mtBackend::initialize() {
    // Pick the system's first Metal device.
    uint64_t devices = WMTCopyAllDevices();
    if (!devices || NSArray_count(devices) == 0) {
      logLine("initialize: no Metal devices");
      return false;
    }
    m_device = NSArray_object(devices, 0);
    NSObject_retain(m_device);
    NSObject_release(devices);

    m_queue = MTLDevice_newCommandQueue(m_device, 64);
    if (!m_queue) {
      logLine("initialize: newCommandQueue failed");
      return false;
    }

    // Build the one fixed-function triangle pipeline from the embedded
    // metallib. The MSL entry points are named triangleVertex/triangleFragment
    // (kept verbatim — the build-time metallib uses these names).
    uint64_t libraryData = DispatchData_alloc_init(
        reinterpret_cast<uint64_t>(d9mt_triangle_metallib),
        d9mt_triangle_metallib_len);
    uint64_t error = 0;
    m_library = MTLDevice_newLibrary(m_device, libraryData, &error);
    NSObject_release(libraryData);
    if (!m_library) {
      logLine("initialize: newLibrary failed");
      return false;
    }
    uint64_t vertexFunction = MTLLibrary_newFunction(m_library, "triangleVertex");
    uint64_t fragmentFunction = MTLLibrary_newFunction(m_library, "triangleFragment");
    if (!vertexFunction || !fragmentFunction) {
      logLine("initialize: newFunction failed");
      return false;
    }

    WMTRenderPipelineInfo pipelineInfo = {};
    pipelineInfo.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    pipelineInfo.colors[0].write_mask = WMTColorWriteMaskAll;
    pipelineInfo.colors[0].blending_enabled = false;
    pipelineInfo.rasterization_enabled = true;
    pipelineInfo.raster_sample_count = 1;
    pipelineInfo.vertex_function = vertexFunction;
    pipelineInfo.fragment_function = fragmentFunction;
    pipelineInfo.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    pipelineInfo.max_tessellation_factor = 16;
    error = 0;
    m_pipelineState = MTLDevice_newRenderPipelineState(m_device, &pipelineInfo, &error);
    NSObject_release(vertexFunction);
    NSObject_release(fragmentFunction);
    if (!m_pipelineState) {
      logLine("initialize: newRenderPipelineState failed");
      return false;
    }

    logLine("initialize: OK");
    return true;
  }

  bool D9mtBackend::attachWindow(void* windowHandle, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    // Attach a CAMetalLayer to the Wine window for presentation.
    m_view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(windowHandle),
                                     m_device, &m_layer);
    if (!m_view || !m_layer) {
      logLine("attachWindow: CreateMetalViewFromHWND failed");
      return false;
    }
    WMTLayerProps props = {};
    MetalLayer_getProps(m_layer, &props);
    props.device = m_device;
    props.contents_scale = 1.0;
    props.drawable_width = m_width;
    props.drawable_height = m_height;
    props.opaque = true;
    props.display_sync_enabled = true;
    props.framebuffer_only = true;
    props.pixel_format = WMTPixelFormatBGRA8Unorm;
    MetalLayer_setProps(m_layer, &props);

    logLine("attachWindow: OK");
    return true;
  }

  uint64_t D9mtBackend::createSharedBuffer(uint64_t byteCount, void** cpuPointerOut,
                                           uint64_t* gpuAddressOut) {
    if (cpuPointerOut)
      *cpuPointerOut = nullptr;
    if (gpuAddressOut)
      *gpuAddressOut = 0;

    // Page-aligned host storage the CPU writes and the GPU reads. Using
    // VirtualAlloc + StorageModeShared keeps writes coherent without an explicit
    // flush, matching the proven metal_backend vertex arena.
    void* memory = VirtualAlloc(nullptr, byteCount, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!memory) {
      logLine("createSharedBuffer: VirtualAlloc failed");
      return 0;
    }

    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = byteCount;
    bufferInfo.options = WMTResourceStorageModeShared;
    bufferInfo.memory.set(memory);
    uint64_t buffer = MTLDevice_newBuffer(m_device, &bufferInfo);
    if (!buffer) {
      logLine("createSharedBuffer: newBuffer failed");
      VirtualFree(memory, 0, MEM_RELEASE);
      return 0;
    }

    if (cpuPointerOut)
      *cpuPointerOut = memory;
    if (gpuAddressOut)
      *gpuAddressOut = bufferInfo.gpu_address;   // filled by MTLDevice_newBuffer
    return buffer;
  }

  uint64_t D9mtBackend::createSampledTexture(uint32_t width, uint32_t height,
                                             uint32_t mipLevels,
                                             uint32_t metalPixelFormat,
                                             uint32_t metalUsage,
                                             uint64_t* gpuResourceIdOut) {
    if (gpuResourceIdOut)
      *gpuResourceIdOut = 0;

    WMTTextureInfo info = {};
    info.pixel_format       = WMTPixelFormat(metalPixelFormat);
    info.width              = width;
    info.height             = height;
    info.depth              = 1u;
    info.array_length       = 1u;
    info.type               = WMTTextureType2D;
    info.mipmap_level_count = mipLevels ? mipLevels : 1u;
    info.sample_count       = 1u;
    info.usage              = WMTTextureUsage(metalUsage);
    // Render targets are GPU-only (Private); they are written by the GPU and
    // sampled by it, never CPU-uploaded. Plain sampled textures use Managed so
    // replaceRegion can stage CPU pixels into them.
    info.options            = (metalUsage & 4u)  // WMTTextureUsageRenderTarget
      ? WMTResourceStorageModePrivate
      : WMTResourceStorageModeManaged;

    uint64_t texture = MTLDevice_newTexture(m_device, &info);
    if (!texture) {
      logLine("createSampledTexture: newTexture failed");
      return 0;
    }
    if (gpuResourceIdOut)
      *gpuResourceIdOut = info.gpu_resource_id;   // filled by MTLDevice_newTexture
    return texture;
  }

  void D9mtBackend::uploadTexture(uint64_t texture, uint32_t mipLevel,
                                  uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  const void* data, uint32_t bytesPerRow) {
    if (!texture || !data)
      return;
    WMTOrigin origin = { x, y, 0u };
    WMTSize   size   = { width, height, 1u };
    WMTMemoryPointer ptr = {};
    ptr.set(const_cast<void*>(data));
    // bytes-per-image is 0 for a 2D (non-array) texture region.
    MTLTexture_replaceRegion(texture, origin, size, mipLevel, 0u, ptr, bytesPerRow, 0u);
  }

  uint64_t D9mtBackend::createSampler(uint32_t minFilter, uint32_t magFilter,
                                      uint32_t mipFilter, uint32_t addressU,
                                      uint32_t addressV, uint32_t addressW,
                                      uint32_t maxAnisotropy, float lodMinClamp,
                                      float lodMaxClamp,
                                      uint64_t* gpuResourceIdOut) {
    if (gpuResourceIdOut)
      *gpuResourceIdOut = 0;

    WMTSamplerInfo info = {};
    info.min_filter        = WMTSamplerMinMagFilter(minFilter);
    info.mag_filter        = WMTSamplerMinMagFilter(magFilter);
    info.mip_filter        = WMTSamplerMipFilter(mipFilter);
    info.s_address_mode    = WMTSamplerAddressMode(addressU);
    info.t_address_mode    = WMTSamplerAddressMode(addressV);
    info.r_address_mode    = WMTSamplerAddressMode(addressW);
    info.compare_function  = WMTCompareFunctionNever;
    info.lod_min_clamp     = lodMinClamp;
    info.lod_max_clamp     = lodMaxClamp;
    info.max_anisotroy     = maxAnisotropy ? maxAnisotropy : 1u;
    info.normalized_coords = true;
    // Samplers live in the bindless heap, so they must be argument-buffer-usable.
    info.support_argument_buffers = true;

    uint64_t sampler = MTLDevice_newSamplerState(m_device, &info);
    if (!sampler) {
      logLine("createSampler: newSamplerState failed");
      return 0;
    }
    if (gpuResourceIdOut)
      *gpuResourceIdOut = info.gpu_resource_id;
    return sampler;
  }

  uint64_t D9mtBackend::createDepthStencilState(uint32_t depthCompareFunction,
                                                bool depthWriteEnabled,
                                                bool stencilEnabled,
                                                const StencilFace& front,
                                                const StencilFace& back) {
    WMTDepthStencilInfo info = {};
    info.depth_compare_function = WMTCompareFunction(depthCompareFunction);
    info.depth_write_enabled    = depthWriteEnabled;

    auto fillFace = [stencilEnabled](WMTStencilInfo& dst, const StencilFace& src) {
      dst.enabled                  = stencilEnabled;
      dst.stencil_fail_op          = WMTStencilOperation(src.failOp);
      dst.depth_stencil_pass_op    = WMTStencilOperation(src.passOp);
      dst.depth_fail_op            = WMTStencilOperation(src.depthFailOp);
      dst.stencil_compare_function = WMTCompareFunction(src.compareFunction);
      dst.read_mask                = src.readMask;
      dst.write_mask               = src.writeMask;
    };
    fillFace(info.front_stencil, front);
    fillFace(info.back_stencil, back);
    return MTLDevice_newDepthStencilState(m_device, &info);
  }

  void D9mtBackend::renderAndPresent(const D9mtDraw& draw, bool clear, uint32_t clearColorArgb) {
    if (!isValid() || !m_layer)
      return;

    uint64_t pool = NSAutoreleasePool_alloc_init();

    uint64_t drawable = MetalLayer_nextDrawable(m_layer);
    if (!drawable) {
      NSObject_release(pool);
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(drawable);
    uint64_t commandBuffer = MTLCommandQueue_commandBuffer(m_queue);

    // Render pass: clear the drawable (when asked), then draw the triangle list.
    // The clear color is a raw D3DCOLOR (0xAARRGGBB).
    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    uint64_t encoder = MTLCommandBuffer_renderCommandEncoder(commandBuffer, &renderPass);
    if (encoder) {
      if (draw.vertexCount && draw.vertexBuffer) {
        // POD wmtcmd packet chain: set pipeline -> bind vertex buffer -> draw.
        wmtcmd_render_setpso setPipeline = {};
        wmtcmd_render_setbuffer setVertexBuffer = {};
        wmtcmd_render_draw drawCmd = {};

        setPipeline.type = WMTRenderCommandSetPSO;
        setPipeline.next.set(&setVertexBuffer);
        setPipeline.pso = m_pipelineState;

        setVertexBuffer.type = WMTRenderCommandSetVertexBuffer;
        setVertexBuffer.next.set(&drawCmd);
        setVertexBuffer.buffer = draw.vertexBuffer;
        setVertexBuffer.offset = draw.vertexOffset;
        setVertexBuffer.index = 0;

        drawCmd.type = WMTRenderCommandDraw;
        drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
        drawCmd.vertex_start = 0;
        drawCmd.vertex_count = draw.vertexCount;
        drawCmd.instance_count = 1;
        drawCmd.base_instance = 0;

        MTLRenderCommandEncoder_encodeCommands(
            encoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
      }
      MTLCommandEncoder_endEncoding(encoder);
    }

    MTLCommandBuffer_presentDrawable(commandBuffer, drawable);
    MTLCommandBuffer_commit(commandBuffer);
    MTLCommandBuffer_waitUntilCompleted(commandBuffer);  // milestone: block

    NSObject_release(pool);
  }

  void D9mtBackend::renderAndPresent(const D9mtPipelineDraw& draw) {
    if (!m_layer || !draw.pipelineState)
      return;

    uint64_t pool = NSAutoreleasePool_alloc_init();

    uint64_t drawable = MetalLayer_nextDrawable(m_layer);
    if (!drawable) {
      NSObject_release(pool);
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(drawable);
    uint64_t commandBuffer = MTLCommandQueue_commandBuffer(m_queue);

    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = draw.clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((draw.clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((draw.clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((draw.clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((draw.clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    uint64_t encoder = MTLCommandBuffer_renderCommandEncoder(commandBuffer, &renderPass);
    if (encoder) {
      // Build the POD command chain: set pipeline -> bind every buffer -> draw.
      // The buffer packets live in a vector with stable addresses (reserved up
      // front) so the `next` links stay valid until encodeCommands consumes them.
      wmtcmd_render_setpso setPipeline = {};
      wmtcmd_render_draw   drawCmd     = {};
      std::vector<wmtcmd_render_setbuffer>    bufferCommands(draw.bindingCount);
      std::vector<wmtcmd_render_useresource>  residencyCommands(draw.residentCount);

      setPipeline.type = WMTRenderCommandSetPSO;
      setPipeline.pso  = draw.pipelineState;

      wmtcmd_base* tail = reinterpret_cast<wmtcmd_base*>(&setPipeline);

      // Make every argument-buffer-referenced resource resident so the GPU can
      // follow the addresses written into the argument buffers.
      for (uint32_t i = 0; i < draw.residentCount; i++) {
        wmtcmd_render_useresource& cmd = residencyCommands[i];
        cmd.type     = WMTRenderCommandUseResource;
        cmd.resource = draw.residentBuffers[i];
        cmd.usage    = WMTResourceUsageRead;
        cmd.stages   = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
        tail->next.set(&cmd);
        tail = reinterpret_cast<wmtcmd_base*>(&cmd);
      }

      for (uint32_t i = 0; i < draw.bindingCount; i++) {
        const D9mtBufferBinding& binding = draw.bindings[i];
        wmtcmd_render_setbuffer& cmd = bufferCommands[i];
        cmd.type   = binding.fragmentStage ? WMTRenderCommandSetFragmentBuffer
                                            : WMTRenderCommandSetVertexBuffer;
        cmd.buffer = binding.buffer;
        cmd.offset = binding.offset;
        cmd.index  = uint8_t(binding.index);
        tail->next.set(&cmd);
        tail = reinterpret_cast<wmtcmd_base*>(&cmd);
      }

      drawCmd.type           = WMTRenderCommandDraw;
      drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
      drawCmd.vertex_start   = 0;
      drawCmd.vertex_count   = draw.vertexCount;
      drawCmd.instance_count = 1;
      drawCmd.base_instance  = 0;
      tail->next.set(&drawCmd);

      MTLRenderCommandEncoder_encodeCommands(
          encoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
      MTLCommandEncoder_endEncoding(encoder);
    }

    MTLCommandBuffer_presentDrawable(commandBuffer, drawable);
    MTLCommandBuffer_commit(commandBuffer);
    MTLCommandBuffer_waitUntilCompleted(commandBuffer);

    NSObject_release(pool);
  }

  // --- Frame lifecycle (Stage A) ---------------------------------------------
  // Split renderAndPresent's per-draw acquire/encode/present/stall into a
  // per-FRAME shape: beginFrame opens one drawable + command buffer + encoder,
  // appendDraw accumulates draws into that single encoder, and
  // endFrameAndPresent ends/presents/commits/waits once.

  void D9mtBackend::registerBackbufferTexture(uint64_t texture) {
    if (texture)
      m_backbufferTextures.insert(texture);
  }

  // Start a Metal frame capture when C:\d9mt-test\DO_CAPTURE exists (deleted on
  // trigger so exactly one frame is captured). Needs MTL_CAPTURE_ENABLED=1 in
  // the launch environment. Writes a .gputrace openable in Metal Debugger.
  void D9mtBackend::maybeBeginGpuCapture() {
    if (m_capturing)
      return;
    const char* trigger = "C:\\d9mt-test\\DO_CAPTURE";
    if (FILE* f = std::fopen(trigger, "rb")) {
      std::fclose(f);
      std::remove(trigger);
      static const char path[] = "C:\\d9mt-test\\frame.gputrace";
      d9mt_capture_params p = {};
      p.action   = 1u;
      p.queue    = m_queue;
      p.path_ptr = uint64_t(uintptr_t(path));
      p.path_len = sizeof(path) - 1u;
      D9MT_UnixCall(D9MT_FUNC_CAPTURE, &p);
      m_capturing = p.ret_ok != 0u;
    }
  }

  void D9mtBackend::endGpuCaptureIfActive() {
    if (!m_capturing)
      return;
    d9mt_capture_params p = {};
    p.action = 0u;
    D9MT_UnixCall(D9MT_FUNC_CAPTURE, &p);
    m_capturing = false;
  }

  void D9mtBackend::setColorTarget(uint64_t texture) {
    if (texture == m_targetTexture)
      return;
    // End the current render pass so the next draw opens a fresh pass against
    // the new target on the same command buffer.
    if (m_frameEncoder) {
      MTLCommandEncoder_endEncoding(m_frameEncoder);
      m_frameEncoder = 0;
      m_frameOpen = false;
    }
    m_targetTexture = texture;
  }

  void D9mtBackend::beginFrame(bool clear, uint32_t clearColorArgb) {
    // Remember the clear request so a draw-less (clear-only) frame can still
    // present a cleared image when endFrameAndPresent opens the frame itself.
    m_pendingClear = clear;
    m_pendingClearColor = clearColorArgb;

    // Idempotent within a render pass: only the first call opens the encoder;
    // later calls (e.g. one before every draw) are no-ops until a target switch.
    if (m_frameOpen)
      return;
    if (!m_layer)
      return;

    // The autorelease pool + command buffer span the whole frame (all render
    // passes); created once, on the first pass.
    if (!m_framePool)
      m_framePool = NSAutoreleasePool_alloc_init();
    if (!m_frameCommandBuffer) {
      maybeBeginGpuCapture();
      m_frameCommandBuffer = MTLCommandQueue_commandBuffer(m_queue);
    }

    // The backbuffer renders into the swapchain drawable (acquired once per
    // frame, presented at the end); an offscreen target renders into its own
    // texture and is never presented.
    uint64_t colorTexture;
    if (m_targetTexture && !m_backbufferTextures.count(m_targetTexture)) {
      colorTexture = m_targetTexture;  // offscreen render-to-texture target
    } else {
      if (!m_frameDrawable)
        m_frameDrawable = MetalLayer_nextDrawable(m_layer);
      if (!m_frameDrawable)
        return;  // no drawable available — leave the pass closed.
      colorTexture = MetalDrawable_texture(m_frameDrawable);
    }

    // Depth+stencil buffer (Depth32Float_Stencil8, drawable-sized, GPU-private),
    // created once and reused. Cleared at each pass so depth/stencil-tested draws
    // start fresh; draws that enable neither simply ignore it. A combined format
    // (not plain Depth32Float) so stencil-test state has a stencil plane to use.
    if (!m_depthTexture && m_width && m_height) {
      WMTTextureInfo depthInfo = {};
      depthInfo.pixel_format       = WMTPixelFormatDepth32Float_Stencil8;
      depthInfo.width              = m_width;
      depthInfo.height             = m_height;
      depthInfo.depth              = 1u;
      depthInfo.array_length       = 1u;
      depthInfo.type               = WMTTextureType2D;
      depthInfo.mipmap_level_count = 1u;
      depthInfo.sample_count       = 1u;
      depthInfo.usage              = WMTTextureUsageRenderTarget;
      depthInfo.options            = WMTResourceStorageModePrivate;
      m_depthTexture = MTLDevice_newTexture(m_device, &depthInfo);
    }

    // Render pass identical to renderAndPresent: clear the drawable (when
    // asked) then load into the open encoder. clearColorArgb is raw D3DCOLOR.
    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = colorTexture;
    renderPass.colors[0].load_action = clear ? WMTLoadActionClear : WMTLoadActionLoad;
    renderPass.colors[0].store_action = WMTStoreActionStore;
    renderPass.colors[0].clear_color.r = ((clearColorArgb >> 16) & 0xff) / 255.0;
    renderPass.colors[0].clear_color.g = ((clearColorArgb >> 8)  & 0xff) / 255.0;
    renderPass.colors[0].clear_color.b = ((clearColorArgb)       & 0xff) / 255.0;
    renderPass.colors[0].clear_color.a = ((clearColorArgb >> 24) & 0xff) / 255.0;
    renderPass.depth.texture      = m_depthTexture;
    renderPass.depth.load_action  = WMTLoadActionClear;
    renderPass.depth.store_action = WMTStoreActionDontCare;
    renderPass.depth.clear_depth  = 1.0f;
    renderPass.stencil.texture      = m_depthTexture;  // same combined attachment
    renderPass.stencil.load_action  = WMTLoadActionClear;
    renderPass.stencil.store_action = WMTStoreActionDontCare;
    renderPass.stencil.clear_stencil = 0u;
    renderPass.render_target_width = m_width;
    renderPass.render_target_height = m_height;
    renderPass.default_raster_sample_count = 1;

    m_frameEncoder = MTLCommandBuffer_renderCommandEncoder(m_frameCommandBuffer, &renderPass);
    m_frameOpen = true;
    m_residentThisFrame.clear();
  }

  void D9mtBackend::appendDraw(const D9mtPipelineDraw& draw) {
    if (!m_frameOpen || !m_frameEncoder || !draw.pipelineState)
      return;

    TraceScope appendScope(TraceZone::AppendDraw);

    // Same POD command chain as renderAndPresent(D9mtPipelineDraw): set pipeline
    // -> make resources resident -> bind every buffer -> draw. The buffer/
    // residency packets live in scratch with stable addresses (sized up front)
    // so the `next` links stay valid until encodeCommands consumes them.
    // encodeCommands appends onto the still-open encoder — no end/present here.
    //
    // The scratch is function-static (the shim replays draws on one thread): its
    // capacity persists across draws, so a steady draw stream allocates nothing.
    static std::vector<wmtcmd_render_setbuffer>   bufferCommands;
    static std::vector<wmtcmd_render_useresource> residencyCommands;
    bufferCommands.assign(draw.bindingCount, {});
    residencyCommands.assign(draw.residentCount, {});

    wmtcmd_render_setpso              setPipeline = {};
    wmtcmd_render_setdsso             setDepthState = {};
    wmtcmd_render_setrasterizerstate  setRaster = {};
    wmtcmd_render_setviewport         setViewport = {};
    wmtcmd_render_setscissorrect      setScissor = {};
    wmtcmd_render_draw                drawCmd     = {};

    setPipeline.type = WMTRenderCommandSetPSO;
    setPipeline.pso  = draw.pipelineState;

    wmtcmd_base* tail = reinterpret_cast<wmtcmd_base*>(&setPipeline);

    // Bind the depth-stencil state when the draw carries one; otherwise the
    // encoder keeps Metal's default always-pass / no-write depth behavior.
    if (draw.depthStencilState) {
      setDepthState.type        = WMTRenderCommandSetDSSO;
      setDepthState.dsso        = draw.depthStencilState;
      setDepthState.stencil_ref = draw.stencilReference;
      tail->next.set(&setDepthState);
      tail = reinterpret_cast<wmtcmd_base*>(&setDepthState);
    }

    // Rasterizer state: cull mode, winding, fill mode, depth bias.
    setRaster.type            = WMTRenderCommandSetRasterizerState;
    setRaster.fill_mode       = WMTTriangleFillMode(draw.fillMode);
    setRaster.cull_mode       = WMTCullMode(draw.cullMode);
    setRaster.depth_clip_mode = WMTDepthClipModeClip;
    setRaster.winding         = WMTWinding(draw.winding);
    setRaster.depth_bias      = draw.depthBias;
    setRaster.scole_scale     = draw.depthSlopeScale;
    setRaster.depth_bias_clamp = draw.depthBiasClamp;
    tail->next.set(&setRaster);
    tail = reinterpret_cast<wmtcmd_base*>(&setRaster);

    // Viewport (pixel rect + depth range) and scissor; absent leaves Metal's
    // default of the full render target.
    if (draw.hasViewport) {
      setViewport.type              = WMTRenderCommandSetViewport;
      setViewport.viewport.originX  = draw.viewportX;
      setViewport.viewport.originY  = draw.viewportY;
      setViewport.viewport.width    = draw.viewportWidth;
      setViewport.viewport.height   = draw.viewportHeight;
      setViewport.viewport.znear    = draw.viewportMinDepth;
      setViewport.viewport.zfar     = draw.viewportMaxDepth;
      tail->next.set(&setViewport);
      tail = reinterpret_cast<wmtcmd_base*>(&setViewport);
    }
    if (draw.hasScissor) {
      setScissor.type                = WMTRenderCommandSetScissorRect;
      setScissor.scissor_rect.x      = draw.scissorX;
      setScissor.scissor_rect.y      = draw.scissorY;
      setScissor.scissor_rect.width  = draw.scissorWidth;
      setScissor.scissor_rect.height = draw.scissorHeight;
      tail->next.set(&setScissor);
      tail = reinterpret_cast<wmtcmd_base*>(&setScissor);
    }

    // Make every argument-buffer-referenced resource resident so the GPU can
    // follow the addresses written into the argument buffers — but only the
    // first time each resource is seen this frame; residency persists for the
    // encoder, so re-emitting it for later draws is wasted work.
    uint32_t residencyCount = 0;
    for (uint32_t i = 0; i < draw.residentCount; i++) {
      if (!m_residentThisFrame.insert(draw.residentBuffers[i]).second)
        continue;
      wmtcmd_render_useresource& cmd = residencyCommands[residencyCount++];
      cmd.type     = WMTRenderCommandUseResource;
      cmd.resource = draw.residentBuffers[i];
      cmd.usage    = WMTResourceUsageRead;
      cmd.stages   = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
      tail->next.set(&cmd);
      tail = reinterpret_cast<wmtcmd_base*>(&cmd);
    }

    for (uint32_t i = 0; i < draw.bindingCount; i++) {
      const D9mtBufferBinding& binding = draw.bindings[i];
      wmtcmd_render_setbuffer& cmd = bufferCommands[i];
      cmd.type   = binding.fragmentStage ? WMTRenderCommandSetFragmentBuffer
                                          : WMTRenderCommandSetVertexBuffer;
      cmd.buffer = binding.buffer;
      cmd.offset = binding.offset;
      cmd.index  = uint8_t(binding.index);
      tail->next.set(&cmd);
      tail = reinterpret_cast<wmtcmd_base*>(&cmd);
    }

    // Indexed draws fetch from the bound index buffer; linear draws emit a
    // plain vertex range. Both append onto the same open encoder.
    wmtcmd_render_draw_indexed indexedCmd = {};
    if (draw.indexBuffer) {
      indexedCmd.type                = WMTRenderCommandDrawIndexed;
      indexedCmd.primitive_type      = WMTPrimitiveType(draw.primitiveType);
      indexedCmd.index_type          = WMTIndexType(draw.indexType);
      indexedCmd.index_count         = draw.indexCount;
      indexedCmd.index_buffer        = draw.indexBuffer;
      indexedCmd.index_buffer_offset = draw.indexBufferOffset;
      indexedCmd.instance_count      = 1;
      indexedCmd.base_vertex         = draw.baseVertex;
      indexedCmd.base_instance       = 0;
      tail->next.set(&indexedCmd);
    } else {
      drawCmd.type           = WMTRenderCommandDraw;
      drawCmd.primitive_type = WMTPrimitiveType(draw.primitiveType);
      drawCmd.vertex_start   = 0;
      drawCmd.vertex_count   = draw.vertexCount;
      drawCmd.instance_count = 1;
      drawCmd.base_instance  = 0;
      tail->next.set(&drawCmd);
    }

    MTLRenderCommandEncoder_encodeCommands(
        m_frameEncoder, reinterpret_cast<const wmtcmd_base*>(&setPipeline));
  }

  void D9mtBackend::endFrameAndPresent() {
    // Clear-only / no-draw frame: nothing opened a pass, so open a backbuffer
    // pass now using the last clear seen so the cleared image still presents.
    if (!m_frameCommandBuffer) {
      m_targetTexture = 0;
      beginFrame(m_pendingClear, m_pendingClearColor);
    }

    // End the last render pass of the frame.
    if (m_frameEncoder) {
      MTLCommandEncoder_endEncoding(m_frameEncoder);
      m_frameEncoder = 0;
      m_frameOpen = false;
    }

    if (m_frameCommandBuffer) {
      TraceScope presentScope(TraceZone::Present);
      // Present only if a backbuffer pass acquired the drawable.
      if (m_frameDrawable)
        MTLCommandBuffer_presentDrawable(m_frameCommandBuffer, m_frameDrawable);
      MTLCommandBuffer_commit(m_frameCommandBuffer);
      MTLCommandBuffer_waitUntilCompleted(m_frameCommandBuffer);  // Stage A: block
    }
    endGpuCaptureIfActive();

    if (m_framePool)
      NSObject_release(m_framePool);

    // Reset frame state — zero handles mean no frame is in flight. The target
    // returns to the backbuffer for the next frame.
    m_frameDrawable = m_frameCommandBuffer = m_frameEncoder = m_framePool = 0;
    m_frameOpen = false;
    m_targetTexture = 0;

    // Emit the per-frame CPU breakdown (no-op unless D9MT_TRACE is set).
    FrameTrace::endFrame();
  }

}
