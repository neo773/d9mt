#pragma once

// D9mtBackend — the single Metal backend the shim is built on. Owns the
// winemetal device/queue, the swapchain layer for the window, the one hardcoded
// fixed-function MSL pipeline (triangleVertex/triangleFragment), and a shared
// vertex arena. DxvkDevice owns one; DxvkContext records draws into it; the
// swapchain presents through it. Metal is reached only via the winemetal ABI.
//
// This is the foundation every other shim module leans on (SHIM_SPEC.md).

#include <cstdint>
#include <unordered_set>

namespace dxvk {

  // A render target the backend can clear + draw into and present. For the
  // backbuffer this wraps a real Metal texture; the drawable is fetched per
  // present from the swapchain layer.
  struct D9mtRenderTarget {
    uint64_t texture = 0;  // obj_handle_t MTLTexture (0 = use next drawable)
    uint32_t width   = 0;
    uint32_t height  = 0;
  };

  // One recorded draw: a slice of the shared vertex arena + a vertex count.
  struct D9mtDraw {
    uint64_t vertexBuffer = 0;  // obj_handle_t MTLBuffer
    uint64_t vertexOffset = 0;
    uint32_t vertexCount  = 0;
  };

  // One Metal buffer bound to a shader stage at a given index (vertex stream,
  // push block, argument buffer, or sampler heap — the caller decides the index
  // and stage from the shader's reflection).
  struct D9mtBufferBinding {
    uint64_t buffer        = 0;
    uint64_t offset        = 0;
    uint32_t index         = 0;
    bool     fragmentStage = false;   // false = bind to the vertex stage
  };

  // A draw through a real (translated-shader) render pipeline: bind the pipeline
  // and the given buffers, then draw a triangle list. Replaces the hardcoded-MSL
  // path once shaders are translated; see DxvkContext::draw.
  struct D9mtPipelineDraw {
    uint64_t                  pipelineState  = 0;   // MTLRenderPipelineState
    const D9mtBufferBinding*  bindings       = nullptr;
    uint32_t                  bindingCount   = 0;
    // Buffers referenced by argument buffers (by GPU address) — made resident
    // via useResource so the GPU can follow those addresses.
    const uint64_t*           residentBuffers = nullptr;
    uint32_t                  residentCount   = 0;
    uint32_t                  vertexCount    = 0;
    // Primitive type for the draw (winemetal WMTPrimitiveType: point/line/
    // line-strip/triangle/triangle-strip). Defaults to triangle list.
    uint32_t                  primitiveType  = 3;  // WMTPrimitiveTypeTriangle
    // Indexed draw: when indexBuffer is non-zero, the draw fetches indexCount
    // indices of indexType (0 = uint16, 1 = uint32) from indexBuffer at
    // indexBufferOffset, adding baseVertex to each. Otherwise it is a linear
    // draw of vertexCount vertices.
    uint64_t                  indexBuffer       = 0;
    uint64_t                  indexBufferOffset = 0;
    uint32_t                  indexCount        = 0;
    uint32_t                  indexType         = 0;
    int32_t                   baseVertex        = 0;
    // Depth-stencil state object bound before the draw (0 = leave the Metal
    // default: depth test always passes, no depth write).
    uint64_t                  depthStencilState = 0;
    uint32_t                  stencilReference  = 0;
    // Viewport (pixel rect + [minZ,maxZ] depth range) and scissor rect bound
    // before the draw. hasViewport/hasScissor false leaves Metal's default
    // (full render target). Decoupled from winemetal types to keep this header
    // free of the winemetal include.
    bool                      hasViewport    = false;
    double                    viewportX = 0, viewportY = 0, viewportWidth = 0;
    double                    viewportHeight = 0, viewportMinDepth = 0, viewportMaxDepth = 1;
    bool                      hasScissor     = false;
    uint32_t                  scissorX = 0, scissorY = 0, scissorWidth = 0, scissorHeight = 0;
    // Rasterizer state (winemetal enum values): cull mode, front-face winding,
    // triangle fill mode, and depth bias. Defaults: no culling, fill.
    uint32_t                  cullMode = 0, winding = 0, fillMode = 0;
    float                     depthBias = 0, depthSlopeScale = 0, depthBiasClamp = 0;
    bool                      clear          = false;
    uint32_t                  clearColorArgb = 0;
  };

  class D9mtBackend {
  public:
    D9mtBackend();
    ~D9mtBackend();

    D9mtBackend(const D9mtBackend&) = delete;
    D9mtBackend& operator=(const D9mtBackend&) = delete;

    // Creates the winemetal device/queue and the FF pipeline. The swapchain
    // layer is attached later via attachWindow once the HWND is known.
    bool initialize();
    bool isValid() const { return m_pipelineState != 0; }

    uint64_t device() const { return m_device; }
    uint64_t queue()  const { return m_queue; }

    // Attaches a CAMetalLayer to the Wine window for presentation.
    bool attachWindow(void* windowHandle, uint32_t width, uint32_t height);

    // Allocates `byteCount` from a shared, CPU-writable, GPU-visible Metal
    // buffer. Returns the buffer handle + a CPU pointer; writes are coherent.
    // When gpuAddressOut is non-null, also returns the buffer's GPU address
    // (needed to reference it from an argument buffer).
    uint64_t createSharedBuffer(uint64_t byteCount, void** cpuPointerOut,
                                uint64_t* gpuAddressOut = nullptr);

    // Creates a 2D texture (mipLevels mips) in the given winemetal pixel format
    // with the given winemetal usage flags (e.g. ShaderRead, or ShaderRead |
    // RenderTarget for a render-to-texture target) and returns its MTLTexture
    // handle; writes the texture's gpuResourceID (for an argument-buffer
    // descriptor) to gpuResourceIdOut.
    uint64_t createSampledTexture(uint32_t width, uint32_t height, uint32_t mipLevels,
                                  uint32_t metalPixelFormat, uint32_t metalUsage,
                                  uint64_t* gpuResourceIdOut);

    // Uploads a tightly-packed region into one mip level of a texture.
    void uploadTexture(uint64_t texture, uint32_t mipLevel, uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height,
                       const void* data, uint32_t bytesPerRow);

    // Creates a sampler from already-translated Metal sampler parameters and
    // returns its handle; writes the sampler's gpuResourceID (for the sampler
    // heap) to gpuResourceIdOut. Filters/address modes are winemetal enums.
    uint64_t createSampler(uint32_t minFilter, uint32_t magFilter, uint32_t mipFilter,
                           uint32_t addressU, uint32_t addressV, uint32_t addressW,
                           uint32_t maxAnisotropy, float lodMinClamp, float lodMaxClamp,
                           uint64_t* gpuResourceIdOut);

    // Creates a depth-stencil state object (compare function is a WMT/Metal
    // enum value; stencil is left disabled). Returned handle is bound on the
    // render encoder before a draw.
    // Per-face stencil config. The op/compare values are Vulkan enums, which
    // share their numeric encoding with the winemetal/Metal equivalents.
    struct StencilFace {
      uint32_t failOp = 0, passOp = 0, depthFailOp = 0, compareFunction = 7; // Always
      uint8_t  readMask = 0xff, writeMask = 0xff;
    };
    uint64_t createDepthStencilState(uint32_t depthCompareFunction, bool depthWriteEnabled,
                                     bool stencilEnabled,
                                     const StencilFace& front, const StencilFace& back);

    // Selects the color render target for subsequent draws: a swapchain
    // backbuffer texture (see registerBackbufferTexture) renders into the
    // drawable, any other texture into that offscreen render-to-texture target.
    // Switching targets ends the current render pass; the next draw opens a new
    // pass against the new target.
    void setColorTarget(uint64_t texture);

    // Records a swapchain backbuffer's texture so setColorTarget can recognise
    // it and route its draws to the presentable drawable. Called when the
    // swapchain initializes its backbuffers.
    void registerBackbufferTexture(uint64_t texture);

    // Records one render pass into the next drawable: clear to clearColorArgb
    // (raw D3DCOLOR), draw the given list as a triangle list, present, wait.
    // Uses the hardcoded MSL pipeline (fallback / bring-up path).
    void renderAndPresent(const D9mtDraw& draw, bool clear, uint32_t clearColorArgb);

    // Same, but with a caller-built render pipeline + arbitrary buffer bindings
    // (the real translated-shader path).
    void renderAndPresent(const D9mtPipelineDraw& draw);

    // --- Frame lifecycle (Stage A) -------------------------------------------
    // One drawable + command buffer + render command encoder per PRESENTED
    // frame. Draws accumulate into the open encoder; the drawable is presented
    // and the command buffer committed once, at frame end — replacing the
    // per-draw acquire/present/stall of renderAndPresent.

    // Opens the frame (next drawable, command buffer, render encoder) if not
    // already open, clearing to clearColorArgb when `clear` is set. Idempotent
    // within a frame: only the first call opens; later calls are no-ops. Safe to
    // call before every draw. Remembers the clear so a draw-less (clear-only)
    // frame can still present a cleared image at endFrameAndPresent.
    void beginFrame(bool clear, uint32_t clearColorArgb);

    // Encodes one pipeline draw into the open frame encoder (no present, no
    // commit). beginFrame must have opened the frame first; the draw's own
    // clear fields are ignored (clearing is a frame-open concern).
    void appendDraw(const D9mtPipelineDraw& draw);

    // Ends the open encoder, presents the drawable, commits, waits, resets frame
    // state. If no frame is open (clear-only frame), opens + clears one first
    // using the last clear seen by beginFrame so the cleared frame still shows.
    void endFrameAndPresent();

  private:
    uint64_t m_device        = 0;
    uint64_t m_queue         = 0;
    uint64_t m_layer         = 0;
    uint64_t m_view          = 0;
    uint64_t m_library       = 0;
    uint64_t m_pipelineState = 0;
    uint32_t m_width         = 0;
    uint32_t m_height        = 0;

    // Depth attachment shared across frames (cleared each frame). Created lazily
    // at the drawable size so depth-tested draws have a depth buffer to compare
    // against; the color-only triangle path leaves the default always-pass state.
    uint64_t m_depthTexture  = 0;

    // Current color render target. The frontend binds a real texture for
    // color[0] even for the backbuffer, so backbuffer textures are registered
    // explicitly (registerBackbufferTexture); a bound backbuffer maps to the
    // drawable (which the shim renders into and presents), any other texture to
    // an offscreen render-to-texture target.
    uint64_t                     m_targetTexture = 0;
    std::unordered_set<uint64_t> m_backbufferTextures;

    // Programmatic GPU capture: when the DO_CAPTURE trigger file exists, capture
    // the next frame's command buffer to a .gputrace for Metal Debugger.
    bool m_capturing = false;
    void maybeBeginGpuCapture();
    void endGpuCaptureIfActive();

    // Resources already made resident in the current frame. Metal residency
    // persists for the encoder's lifetime, so a resource referenced by many
    // draws only needs one useResource per frame, not one per draw.
    std::unordered_set<uint64_t> m_residentThisFrame;

    // Open-frame state (Stage A). Non-zero handles mean a frame is in flight.
    uint64_t m_frameDrawable      = 0;
    uint64_t m_frameCommandBuffer = 0;
    uint64_t m_frameEncoder       = 0;
    uint64_t m_framePool          = 0;     // NSAutoreleasePool held across the frame
    bool     m_frameOpen          = false;
    bool     m_pendingClear       = false;  // last clear request seen by beginFrame
    uint32_t m_pendingClearColor  = 0;
  };

  // The one backend the active device created. Set in DxvkDevice::initialize so
  // the WSI surface stub (dxvk_wsi_stubs.cpp) can attach the Metal layer to the
  // HWND when the frontend's surface callback fires — that callback is the only
  // place the window handle is exposed to the shim.
  extern D9mtBackend* g_activeBackend;

}
