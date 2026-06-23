#include "metal_backend.h"

#include <cstdio>

#include <windows.h>

#include "winemetal.h"

#include "triangle_metallib.h"  // generated: d9mt_triangle_metallib[] + _len

namespace d9mt::metal {

  namespace {
    void logLine(const char* message) {
      FILE* file = fopen("C:\\d9mt-test\\v2.log", "a");
      if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
      }
    }
  }

  MetalBackend::~MetalBackend() {
    if (m_pipelineState) NSObject_release(m_pipelineState);
    if (m_library)       NSObject_release(m_library);
    if (m_vertexBuffer)  NSObject_release(m_vertexBuffer);
    if (m_queue)         NSObject_release(m_queue);
    // Device, layer, and view are owned by the windowing system / winemetal.
    if (m_vertexMemory)  VirtualFree(m_vertexMemory, 0, MEM_RELEASE);
  }

  bool MetalBackend::initialize(void* windowHandle, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

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

    // Attach a CAMetalLayer to the Wine window.
    m_view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(windowHandle),
                                     m_device, &m_layer);
    if (!m_view || !m_layer) {
      logLine("initialize: CreateMetalViewFromHWND failed");
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

    // Build the triangle pipeline from the embedded metallib.
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

    // Shared, page-aligned vertex storage the CPU writes and the GPU reads.
    const uint64_t bufferBytes = kMaxVertices * sizeof(TriangleVertex);
    m_vertexMemory = VirtualAlloc(nullptr, bufferBytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!m_vertexMemory) {
      logLine("initialize: VirtualAlloc failed");
      return false;
    }
    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = bufferBytes;
    bufferInfo.options = WMTResourceStorageModeShared;
    bufferInfo.memory.set(m_vertexMemory);
    m_vertexBuffer = MTLDevice_newBuffer(m_device, &bufferInfo);
    if (!m_vertexBuffer) {
      logLine("initialize: newBuffer failed");
      return false;
    }

    logLine("initialize: OK");
    return true;
  }

  void MetalBackend::renderAndPresent(uint32_t vertexCount, uint32_t clearColorArgb) {
    if (!isValid())
      return;

    uint64_t pool = NSAutoreleasePool_alloc_init();

    uint64_t drawable = MetalLayer_nextDrawable(m_layer);
    if (!drawable) {
      NSObject_release(pool);
      return;
    }
    uint64_t drawableTexture = MetalDrawable_texture(drawable);
    uint64_t commandBuffer = MTLCommandQueue_commandBuffer(m_queue);

    // Render pass: clear the drawable, then draw the triangle list into it.
    WMTRenderPassInfo renderPass = {};
    renderPass.colors[0].texture = drawableTexture;
    renderPass.colors[0].load_action = WMTLoadActionClear;
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
      if (vertexCount) {
        // POD packet chain: set pipeline -> bind vertex buffer -> draw.
        wmtcmd_render_setpso setPipeline = {};
        wmtcmd_render_setbuffer setVertexBuffer = {};
        wmtcmd_render_draw draw = {};

        setPipeline.type = WMTRenderCommandSetPSO;
        setPipeline.next.set(&setVertexBuffer);
        setPipeline.pso = m_pipelineState;

        setVertexBuffer.type = WMTRenderCommandSetVertexBuffer;
        setVertexBuffer.next.set(&draw);
        setVertexBuffer.buffer = m_vertexBuffer;
        setVertexBuffer.offset = 0;
        setVertexBuffer.index = 0;

        draw.type = WMTRenderCommandDraw;
        draw.primitive_type = WMTPrimitiveTypeTriangle;
        draw.vertex_start = 0;
        draw.vertex_count = vertexCount;
        draw.instance_count = 1;
        draw.base_instance = 0;

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

}
