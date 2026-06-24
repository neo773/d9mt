#pragma once

#include <cstddef>

// DXVK hardware/pipeline limits — pure integer constants, copied verbatim from
// dxvk-ref/dxvk_limits.h. No Vulkan runtime; the frontend references several of
// these directly (MaxNumSpecConstants in d3d9_state.h, MaxSharedPushDataSize in
// d3d9_fixed_function.h, MaxNumRenderTargets in render-target loops). Included
// from dxvk_include.h so every TU sees them. See SHIM_SPEC.md.

namespace dxvk {

  enum DxvkLimits : size_t {
    MaxNumRenderTargets         =     8,
    MaxNumVertexAttributes      =    32,
    MaxNumVertexBindings        =    32,
    MaxNumXfbBuffers            =     4,
    MaxNumXfbStreams            =     4,
    MaxNumViewports             =    16,
    MaxNumUniformBufferSlots    =   128,
    MaxNumSamplerSlots          =   128,
    MaxNumResourceSlots         =  1024,
    MaxNumQueuedCommandBuffers  =    32,
    MaxNumQueryCountPerPool     =   128,
    // The D3D9 frontend packs spec IDs 0..19 into D3D9SpecData (80 bytes), so
    // this must be >= 20 — newer than the stale 12 in old dxvk-ref headers.
    // static_assert(sizeof(D3D9SpecData) <= 4 * MaxNumSpecConstants) in
    // d3d9_state.h enforces it.
    MaxNumSpecConstants         =    20,
    MaxUniformBufferSize        = 65536,
    MaxVertexBindingStride      =  2048,
    MaxTotalPushDataSize        =   256,
    MaxSharedPushDataSize       =    64,
    MaxPerStagePushDataSize     =    32,
    MaxReservedPushDataSize     =    32,
  };

}
