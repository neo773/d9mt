// d9mt: Draw-stage shared declarations — DxvkShader -> MSL compile cache
// (d9mt_shader.cpp) consumed by the draw path in d9mt_context.cpp.
//
// Binding contract (METAL-BACKEND-NOTES.md "Stage decisions: draw"):
//   set-0 argument buffer  -> [[buffer(0)]], one 8-byte slot per [[id]]
//                             (tier-2 ABs; ids are SPIRV-Cross automatic,
//                             mapped back to DXVK slot ids via the SPIR-V
//                             Binding decoration)
//   push data block        -> [[buffer(pushBufferIndex)]] (reflected)
//   set-15 sampler heap    -> [[buffer(samplerHeapIndex)]] (reflected),
//                             contents = d9mt::samplerHeapBuffer()
//   vertex streams         -> [[buffer(VertexBufferBase + binding)]]
//                             (14 + b: bindings 0..16 fit Metal's 31-slot
//                             buffer table; §4.2's "16+binding" does not)

#pragma once

#include <cstdint>
#include <vector>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_shader.h"

namespace dxvk::d9mt {

  // First Metal vertex-buffer slot used for vertex streams.
  constexpr uint32_t VertexBufferBase = 14u;

  // One set-0 resource the shader references: where to read the bound
  // object from (slot) and which AB dword to write (abId).
  struct ShaderResourceRef {
    uint16_t          slot;
    uint16_t          abId;
    VkDescriptorType  type;
    bool              isUniformBuffer; // raw buffer (VA word), not a view
  };

  // One sampler binding: heap index goes into the push block at blockOffset
  // (shader push space, absolute).
  struct ShaderSamplerRef {
    uint16_t slot;
    uint16_t blockOffset;
  };

  // One push data block: copy non-resource dwords from
  // m_state.pc.constantData[srcOffset..] to push space [dstOffset..].
  struct ShaderPushBlock {
    uint32_t dstOffset;
    uint32_t size;
    uint32_t srcOffset;
    uint64_t resourceMask;
  };

  // One MSL function constant the shader declares.
  struct ShaderSpecConstant {
    uint32_t id;
    uint32_t defaultValue;
    bool     isBool;
  };

  struct CompiledShader {
    obj_handle_t library = 0;  // retained MTLLibrary, entry point "main0"
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;

    uint32_t abEntryCount     = 0;   // set-0 AB size in 8-byte slots
    int32_t  abBufferIndex    = -1;  // [[buffer(N)]] of the set-0 AB
    int32_t  pushBufferIndex  = -1;  // [[buffer(N)]] of the push block
    int32_t  samplerHeapIndex = -1;  // [[buffer(N)]] of the set-15 heap
    uint32_t pushDataSize     = 0;   // bytes to upload (max member end)

    std::vector<ShaderResourceRef>  resources;
    std::vector<ShaderSamplerRef>   samplers;
    std::vector<ShaderPushBlock>    pushBlocks;
    std::vector<ShaderSpecConstant> specConstants;

    // function cache keyed by the spec-constant values (d9mt_shader.cpp)
    struct FunctionCache;
    FunctionCache* functions = nullptr;
  };

  // Translates (cached, process-global; keeps the shader alive) a DxvkShader
  // to MSL with the given module fixups (undefined-input elimination, RT
  // swizzles, flat shading). Returns nullptr on failure (logged).
  const CompiledShader* getCompiledShader(
    const Rc<DxvkShader>&             shader,
    const DxvkShaderModuleCreateInfo& moduleInfo);

  // Returns a retained-by-cache MTLFunction specialized with ALL function
  // constants the shader declares (ids < MaxNumSpecConstants from specData,
  // the gate id MaxNumSpecConstants = true, anything else = default).
  // 0 on failure (logged).
  obj_handle_t getShaderFunction(
    const CompiledShader*             shader,
    const uint32_t*                   specData /* MaxNumSpecConstants dwords */);

}
