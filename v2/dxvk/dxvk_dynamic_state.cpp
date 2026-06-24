#include "dxvk_dynamic_state.h"

namespace dxvk {

  // Maps a Vulkan primitive topology to the winemetal WMTPrimitiveType. Metal
  // has no triangle fan, so fans fall back to a triangle list (their geometry
  // needs index conversion — a later refinement); the pipeline's topology class
  // stays Unspecified, which can draw any of these primitive types.
  static uint32_t metalPrimitiveType(VkPrimitiveTopology topology) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     return 0;  // WMTPrimitiveTypePoint
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      return 1;  // WMTPrimitiveTypeLine
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return 2;  // WMTPrimitiveTypeLineStrip
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return 4;  // WMTPrimitiveTypeTriangleStrip
      default:                                   return 3;  // WMTPrimitiveTypeTriangle
    }
  }

  void DxvkDynamicState::applyTo(D9mtPipelineDraw& draw) const {
    draw.stencilReference = m_stencilReference;
    draw.primitiveType    = metalPrimitiveType(m_topology);

    // Rasterizer state. VkCullModeFlags and the Metal cull mode share their
    // none/front/back encoding. For winding, the frontend already accounts for
    // the render-target orientation, and the viewport normalization below keeps
    // screen-space winding D3D9-oriented, so map by meaning (Vulkan clockwise ->
    // Metal clockwise).
    VkCullModeFlags cull = m_rasterizer.cullMode();
    draw.cullMode = cull <= VK_CULL_MODE_BACK_BIT ? uint32_t(cull) : 2u;
    draw.winding  = m_rasterizer.frontFace() == VK_FRONT_FACE_CLOCKWISE
      ? 0u   // WMTWindingClockwise
      : 1u;  // WMTWindingCounterClockwise
    draw.fillMode = m_rasterizer.polygonMode() == VK_POLYGON_MODE_LINE
      ? 1u   // WMTTriangleFillModeLines
      : 0u;  // WMTTriangleFillModeFill
    draw.depthBias       = m_depthBias.depthBiasConstant;
    draw.depthSlopeScale = m_depthBias.depthBiasSlope;
    draw.depthBiasClamp  = m_depthBias.depthBiasClamp;

    if (!m_hasViewport)
      return;

    const VkViewport& vp = m_viewport.viewport;
    // DXVK emits a negative-height viewport (Vulkan's bottom-left y-flip
    // convention). Metal is top-left like D3D9 and the shader output already
    // matches it, so normalize to a positive-height top-left rect — applying
    // the flip verbatim would render upside-down.
    double y = vp.y, height = vp.height;
    if (height < 0) { y += height; height = -height; }
    draw.hasViewport      = true;
    draw.viewportX        = vp.x;
    draw.viewportY        = y;
    draw.viewportWidth    = vp.width;
    draw.viewportHeight   = height;
    draw.viewportMinDepth = vp.minDepth;
    draw.viewportMaxDepth = vp.maxDepth;

    const VkRect2D& sc = m_viewport.scissor;
    draw.hasScissor    = true;
    draw.scissorX      = sc.offset.x;
    draw.scissorY      = sc.offset.y;
    draw.scissorWidth  = sc.extent.width;
    draw.scissorHeight = sc.extent.height;
  }

}
