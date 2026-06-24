// Metal shader pipeline assembly. See dxvk_metal_shader_pipeline.h.

#include "dxvk_metal_shader_pipeline.h"

#include <cstring>

#include "winemetal.h"      // function specialization + handle lifetime
#include "d9mtmetal.h"      // D9MT_UnixCall: MSL compile + vertex-descriptor PSO

#include "dxvk_limits.h"    // MaxNumSpecConstants

namespace dxvk {

  uint32_t metalVertexFormatFromVulkan(uint32_t vkFormat) {
    // MTLVertexFormat raw values. Mirrors v1's vkVertexFormatToMtl — the D3D9
    // vertex-declaration formats DXVK actually emits.
    switch (vkFormat) {
      case VK_FORMAT_R32_SFLOAT:               return 28; // Float
      case VK_FORMAT_R32G32_SFLOAT:            return 29; // Float2
      case VK_FORMAT_R32G32B32_SFLOAT:         return 30; // Float3
      case VK_FORMAT_R32G32B32A32_SFLOAT:      return 31; // Float4
      case VK_FORMAT_B8G8R8A8_UNORM:           return 42; // UChar4Normalized_BGRA
      case VK_FORMAT_R8G8B8A8_UNORM:           return 9;  // UChar4Normalized
      case VK_FORMAT_R8G8B8A8_UINT:            return 3;  // UChar4
      case VK_FORMAT_R8G8B8A8_USCALED:         return 3;  // UChar4
      case VK_FORMAT_R8G8B8A8_SSCALED:         return 6;  // Char4
      case VK_FORMAT_R16G16_SINT:              return 16; // Short2
      case VK_FORMAT_R16G16B16A16_SINT:        return 18; // Short4
      case VK_FORMAT_R16G16_SSCALED:           return 16; // Short2
      case VK_FORMAT_R16G16B16A16_SSCALED:     return 18; // Short4
      case VK_FORMAT_R16G16_SNORM:             return 22; // Short2Normalized
      case VK_FORMAT_R16G16B16A16_SNORM:       return 24; // Short4Normalized
      case VK_FORMAT_R16G16_UNORM:             return 19; // UShort2Normalized
      case VK_FORMAT_R16G16B16A16_UNORM:       return 21; // UShort4Normalized
      case VK_FORMAT_R16G16_SFLOAT:            return 25; // Half2
      case VK_FORMAT_R16G16B16A16_SFLOAT:      return 27; // Half4
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 41; // UInt1010102Normalized
      case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return 40; // Int1010102Normalized
      default:                                 return 0;  // unsupported
    }
  }

  uint64_t metalCompileLibrary(uint64_t device, const std::string& mslSource) {
    d9mt_newlibrary_params params;
    std::memset(&params, 0, sizeof(params));
    params.device     = device;
    params.source_ptr = uint64_t(uintptr_t(mslSource.data()));
    params.source_len = mslSource.size();
    params.fast_math  = 0u;   // math mode is fixed native-side; flag unused

    int status = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &params);
    if (params.ret_error)
      NSObject_release(params.ret_error);   // warnings only; library may still be valid
    return (status == 0) ? params.ret_library : 0u;
  }

  uint64_t metalResolveFunction(
          uint64_t                          library,
    const std::vector<MslSpecConstant>&     specConstants,
    const uint32_t*                         specData,
          uint32_t                          specDataCount) {
    if (!library)
      return 0u;

    if (specConstants.empty())
      return MTLLibrary_newFunction(library, "main0");

    // Supply ALL declared function constants — omitting any one makes Metal
    // reject the function (hard-won lesson carried over from v1). ids below
    // MaxNumSpecConstants come from the live spec data; the gate id is forced
    // true; anything else falls back to the shader's default.
    std::vector<WMTFunctionConstant> constants(specConstants.size());
    std::vector<uint32_t>            values(specConstants.size());

    for (size_t i = 0; i < specConstants.size(); i++) {
      const MslSpecConstant& sc = specConstants[i];
      if (specData && sc.id < specDataCount && sc.id < MaxNumSpecConstants)
        values[i] = specData[sc.id];
      else if (sc.id == MaxNumSpecConstants)
        values[i] = 1u;
      else
        values[i] = sc.defaultValue;

      std::memset(&constants[i], 0, sizeof(constants[i]));
      constants[i].data.set(&values[i]);
      constants[i].type  = sc.isBool ? WMTDataTypeBool : WMTDataTypeUInt;
      constants[i].index = uint16_t(sc.id);
    }

    uint64_t error = 0;
    uint64_t function = MTLLibrary_newFunctionWithConstants(
      library, "main0", constants.data(), uint32_t(constants.size()), &error);
    if (error)
      NSObject_release(error);
    return function;
  }

  uint64_t metalBuildRenderPipeline(
          uint64_t                  device,
          uint64_t                  vertexFunction,
          uint64_t                  fragmentFunction,
    const MetalVertexLayout&        vertexLayout,
          uint32_t                  colorPixelFormat,
    const MetalColorBlend&          blend) {
    if (!device || !vertexFunction)
      return 0u;

    d9mt_pso_info info;
    std::memset(&info, 0, sizeof(info));
    info.vertex_function   = vertexFunction;
    info.fragment_function = fragmentFunction;

    info.colors[0].pixel_format           = colorPixelFormat;
    info.colors[0].blending_enabled       = blend.enabled ? 1u : 0u;
    info.colors[0].rgb_blend_op           = blend.rgbOp;
    info.colors[0].alpha_blend_op         = blend.alphaOp;
    info.colors[0].src_rgb_blend_factor   = blend.srcRgb;
    info.colors[0].dst_rgb_blend_factor   = blend.dstRgb;
    info.colors[0].src_alpha_blend_factor = blend.srcAlpha;
    info.colors[0].dst_alpha_blend_factor = blend.dstAlpha;
    info.colors[0].write_mask             = blend.writeMask;
    // The frame always binds a Depth32Float_Stencil8 attachment, so every
    // pipeline must declare the matching depth and stencil formats even when
    // depth/stencil testing is disabled.
    info.depth_pixel_format               = 260u;  // WMTPixelFormatDepth32Float_Stencil8
    info.stencil_pixel_format             = 260u;
    info.raster_sample_count              = 1u;

    uint32_t attributeCount = uint32_t(vertexLayout.attributes.size());
    uint32_t streamCount    = uint32_t(vertexLayout.streams.size());
    if (attributeCount > 18u || streamCount > 16u)
      return 0u;

    info.num_attributes = attributeCount;
    for (uint32_t i = 0; i < attributeCount; i++) {
      const MetalVertexAttribute& a = vertexLayout.attributes[i];
      info.attributes[i].format       = a.metalFormat;
      info.attributes[i].offset       = a.offset;
      info.attributes[i].buffer_index = a.bufferIndex;
      info.attributes[i].location     = a.location;
    }

    info.num_layouts = streamCount;
    for (uint32_t i = 0; i < streamCount; i++) {
      const MetalVertexStream& s = vertexLayout.streams[i];
      info.layouts[i].buffer_index  = s.bufferIndex;
      info.layouts[i].stride        = s.stride;
      info.layouts[i].step_function = s.stepFunction;
      info.layouts[i].step_rate     = s.stepRate;
    }

    d9mt_newpso_params params;
    std::memset(&params, 0, sizeof(params));
    params.device   = device;
    params.info_ptr = uint64_t(uintptr_t(&info));

    int status = D9MT_UnixCall(D9MT_FUNC_NEW_RENDER_PSO, &params);
    if (params.ret_error)
      NSObject_release(params.ret_error);
    return (status == 0) ? params.ret_pso : 0u;
  }

}
