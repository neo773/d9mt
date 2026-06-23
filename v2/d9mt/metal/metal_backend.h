// metal_backend — the v2 Metal backend for the Wine driver.
//
// Reaches Metal exclusively through the winemetal ABI (vendored as-is from
// DXMT) plus the d9mtmetal companion. No Vulkan, no metal-cpp (that is host
// tooling). Owns the device, queue, swapchain layer, triangle pipeline, and a
// shared vertex buffer. The D3D9 layer hands it NDC vertices and a clear color;
// it records a wmtcmd render packet chain and presents.

#pragma once

#include <cstdint>

namespace d9mt::metal {

  // Matches the Metal `TriangleVertex` (packed_float4 position + uint color),
  // 20 bytes, no padding. The D3D9 layer fills `position` already in NDC.
  struct TriangleVertex {
    float    position[4];  // x, y, z, w in normalized device coordinates
    uint32_t color;        // raw D3DCOLOR (0xAARRGGBB)
  };

  class MetalBackend {
  public:
    MetalBackend() = default;
    ~MetalBackend();

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

    // Creates the device/queue, attaches a CAMetalLayer to the Wine HWND, and
    // builds the triangle pipeline + shared vertex buffer. Returns false on any
    // failure (reason logged).
    bool initialize(void* windowHandle, uint32_t width, uint32_t height);

    bool isValid() const { return m_pipelineState != 0; }

    // CPU-writable, GPU-shared vertex storage. The D3D9 layer writes NDC
    // vertices here directly, then calls renderAndPresent with the count.
    TriangleVertex* vertexUploadBuffer() const {
      return static_cast<TriangleVertex*>(m_vertexMemory);
    }
    uint32_t vertexCapacity() const { return kMaxVertices; }

    // Records one render pass into the next drawable: clears to clearColorArgb
    // (raw D3DCOLOR), draws `vertexCount` vertices from the shared buffer as a
    // triangle list, presents, and waits for completion.
    void renderAndPresent(uint32_t vertexCount, uint32_t clearColorArgb);

  private:
    static constexpr uint32_t kMaxVertices = 4096;

    uint64_t m_device        = 0;
    uint64_t m_queue         = 0;
    uint64_t m_layer         = 0;
    uint64_t m_view          = 0;
    uint64_t m_library       = 0;
    uint64_t m_pipelineState = 0;
    uint64_t m_vertexBuffer  = 0;
    void*    m_vertexMemory  = nullptr;
    uint32_t m_width         = 0;
    uint32_t m_height        = 0;
  };

}
