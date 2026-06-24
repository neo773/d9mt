#pragma once

#include "dxvk_include.h"
#include "dxvk_constant_state.h"
#include "dxvk_backend.h"

namespace dxvk {

  // Viewport + scissor pair, as the frontend binds them.
  struct DxvkViewport {
    VkViewport viewport = { };
    VkRect2D   scissor  = { };
  };

  /**
   * \brief Per-draw dynamic render state
   *
   * Owns the render state a draw needs beyond its pipeline: viewport/scissor,
   * primitive topology, rasterizer state (cull/winding/fill), depth bias, and
   * the stencil reference. Keeping it here lets DxvkContext coordinate the draw
   * rather than accumulate every state field itself. Setters record the
   * D3D9/Vulkan-side values; applyTo translates them into the backend's draw
   * descriptor (the Vulkan->Metal mapping lives in one place).
   */
  class DxvkDynamicState {

  public:

    void setViewport(const DxvkViewport& viewport) {
      m_viewport = viewport;
      m_hasViewport = true;
    }

    void setPrimitiveTopology(VkPrimitiveTopology topology) { m_topology = topology; }
    void setRasterizerState(const DxvkRasterizerState& state) { m_rasterizer = state; }
    void setDepthBias(const DxvkDepthBias& depthBias) { m_depthBias = depthBias; }
    void setStencilReference(uint32_t reference) { m_stencilReference = reference; }

    // Writes the translated viewport/scissor, topology, rasterizer state, depth
    // bias, and stencil reference into the draw descriptor.
    void applyTo(D9mtPipelineDraw& draw) const;

  private:

    DxvkViewport        m_viewport         = { };
    bool                m_hasViewport      = false;
    VkPrimitiveTopology m_topology         = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    DxvkRasterizerState m_rasterizer       = { };
    DxvkDepthBias       m_depthBias        = { };
    uint32_t            m_stencilReference = 0;

  };

}
