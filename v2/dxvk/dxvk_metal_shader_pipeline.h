#pragma once

// Metal shader pipeline assembly — and only that.
//
// Turns translated MSL (from the shader-conversion module) into the Metal
// objects a draw binds: an MTLLibrary, a specialized "main0" MTLFunction, and an
// MTLRenderPipelineState with a vertex descriptor. MSL compilation and the
// vertex-descriptor PSO are done through the d9mtmetal unixlib (winemetal lacks
// both: no newLibraryWithSource, no MTLVertexDescriptor in its pipeline info);
// function specialization uses winemetal's newFunctionWithConstants.
//
// This module holds NO frontend/DXVK types beyond the conversion module's
// reflection — it is the Metal-side counterpart of the (pure) converter, kept
// separate from command recording and resource binding.

#include <cstdint>
#include <string>
#include <vector>

#include "dxvk_shader_convert.h"   // MslSpecConstant (function-constant metadata)

namespace dxvk {

  // One vertex attribute as Metal wants it: a typed field at `offset` in the
  // buffer bound at `bufferIndex`, feeding shader [[attribute(location)]].
  struct MetalVertexAttribute {
    uint32_t metalFormat  = 0;   // MTLVertexFormat raw value
    uint32_t offset       = 0;
    uint32_t bufferIndex  = 0;
    uint32_t location     = 0;
  };

  // One vertex buffer's stepping: stride + per-vertex/per-instance rate.
  struct MetalVertexStream {
    uint32_t bufferIndex  = 0;
    uint32_t stride       = 0;
    uint32_t stepFunction = 1;   // MTLVertexStepFunction: 1=per-vertex, 2=per-instance, 0=constant
    uint32_t stepRate     = 1;
  };

  // The vertex interface for a pipeline: where each attribute reads from and how
  // each bound buffer steps.
  struct MetalVertexLayout {
    std::vector<MetalVertexAttribute> attributes;
    std::vector<MetalVertexStream>    streams;
  };

  // MTLVertexFormat for a Vulkan vertex-attribute format (0 = unsupported).
  uint32_t metalVertexFormatFromVulkan(uint32_t vkFormat);

  // Compile MSL text to a retained MTLLibrary (entry point "main0") via the
  // d9mtmetal unixlib. Returns 0 on failure.
  uint64_t metalCompileLibrary(uint64_t device, const std::string& mslSource);

  // Resolve the library's "main0" specialized with every function constant the
  // shader declares: ids < MaxNumSpecConstants take values from `specData`, the
  // gate id == MaxNumSpecConstants is forced true, anything else uses its
  // default. `specData` may be null (all defaults). Returns 0 on failure.
  uint64_t metalResolveFunction(
          uint64_t                          library,
    const std::vector<MslSpecConstant>&     specConstants,
    const uint32_t*                         specData,
          uint32_t                          specDataCount);

  // Color-attachment blend state for a render pipeline, in Metal/WMT enum
  // values (the caller maps from Vulkan). The defaults describe an opaque draw:
  // blending off, all channels written — i.e. source replaces destination.
  struct MetalColorBlend {
    bool     enabled   = false;
    uint32_t rgbOp     = 0;    // MTLBlendOperationAdd
    uint32_t alphaOp   = 0;
    uint32_t srcRgb    = 1;    // MTLBlendFactorOne
    uint32_t dstRgb    = 0;    // MTLBlendFactorZero
    uint32_t srcAlpha  = 1;
    uint32_t dstAlpha  = 0;
    uint32_t writeMask = 0xFu; // all channels
  };

  // Build a retained MTLRenderPipelineState from the given functions, vertex
  // layout, single color attachment format (MTLPixelFormat raw value), and
  // color blend state via the d9mtmetal unixlib. Returns 0 on failure.
  uint64_t metalBuildRenderPipeline(
          uint64_t                  device,
          uint64_t                  vertexFunction,
          uint64_t                  fragmentFunction,
    const MetalVertexLayout&        vertexLayout,
          uint32_t                  colorPixelFormat,
    const MetalColorBlend&          blend);

}
