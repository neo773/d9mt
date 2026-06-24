#pragma once

// Shader conversion — and ONLY shader conversion.
//
// Pure transform: D3D9-frontend SPIR-V  ->  Metal Shading Language text + the
// reflection a draw needs to bind the shader. No Metal objects, no runtime
// compile, no caches, no device, no logging side effects. v1 folded all of that
// into one 550-line file (d3d9fe/d9mt_shader.cpp); here the translation stands
// alone so the compile/PSO/binding concerns live in their own modules.
//
// Input is the SPIR-V from DxvkShader::getCode plus the shader's
// DxvkPipelineLayoutBuilder (getLayout). Output is MSL + a flat reflection
// struct keyed by DXVK resource slots, which the command/backend module turns
// into Metal argument-buffer / push / sampler bindings.

#include <cstdint>
#include <string>
#include <vector>

#include "../spirv/spirv_code_buffer.h"

// DxvkPipelineLayoutBuilder / DxvkShaderDescriptor / DxvkPushDataBlock — the
// frontend's (kept) layout types describing the shader's resource interface.
#include "dxvk_pipelayout.h"

namespace dxvk {

  // Base offset of a push-data region in the D3D9 constant store. The frontend
  // writes every push block at its own local offset 0, distinguishing them only
  // by stage, so the shared block (no stage flags, or all graphics stages) and
  // each single-stage block must occupy disjoint regions or they clobber each
  // other. The shared block starts the store; each single-stage block gets its
  // own slot after it, keyed by stage bit. Both the writer (DxvkContext::pushData)
  // and the reader (the reflection's srcOffset) use this so they agree.
  inline uint32_t pushRegionBaseForStages(VkShaderStageFlags stages) {
    if (stages == 0u || (stages & (stages - 1u)))
      return 0u;  // shared region (all stages)
    uint32_t bit = 0u;
    while (!(stages & (1u << bit)))
      bit++;
    return MaxSharedPushDataSize + MaxPerStagePushDataSize * bit;
  }

  // One argument-buffer-resident resource (UBO / SSBO / sampled image). `abId`
  // is SPIRV-Cross's automatic MSL binding inside the set-0 argument buffer;
  // `slot` is the DXVK resource slot the draw binds from.
  struct MslResourceRef {
    uint16_t        set             = 0;   // descriptor set == the argument buffer's [[buffer]] index
    uint16_t        slot            = 0;   // DXVK resource slot the draw binds from
    uint16_t        abId            = 0;   // 8-byte slot within that set's argument buffer
    VkDescriptorType type           = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    bool            isUniformBuffer = false;
  };

  // A sampler the shader references. Samplers carry no SPIR-V Binding decoration
  // in the D3D9 (dxso + FF) path, so they are matched by DXVK resource index and
  // the push-block dword that holds the sampler's heap index.
  struct MslSamplerRef {
    uint16_t slot        = 0;
    uint16_t blockOffset = 0;
  };

  // A contiguous push-data block the shader reads: copy `size` bytes from
  // `srcOffset` in the D3D9 constant store to `dstOffset` in the Metal push
  // buffer. `resourceMask` marks dwords that are resource handles (sampler heap
  // indices), not raw constants.
  struct MslPushBlock {
    uint32_t dstOffset    = 0;
    uint32_t size         = 0;
    uint32_t srcOffset    = 0;
    uint64_t resourceMask = 0;
  };

  // A Metal function constant (SPIR-V specialization constant) the function must
  // be specialized with before a pipeline can be built.
  struct MslSpecConstant {
    uint32_t id           = 0;
    uint32_t defaultValue = 0;
    bool     isBool       = false;
  };

  // Everything the draw path needs to bind one translated shader stage.
  struct MslReflection {
    VkShaderStageFlagBits        stage           = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;

    int32_t                      abBufferIndex   = -1;   // [[buffer(n)]] of the set-0 argument buffer
    uint32_t                     abEntryCount    = 0;    // argument-buffer entry count
    int32_t                      pushBufferIndex = -1;   // [[buffer(n)]] of the push-constant buffer
    int32_t                      samplerHeapIndex= -1;   // [[buffer(n)]] of the sampler heap
    uint32_t                     pushDataSize    = 0;    // bytes of push data the shader reads

    std::vector<MslResourceRef>  resources;
    std::vector<MslSamplerRef>   samplers;
    std::vector<MslPushBlock>    pushBlocks;
    std::vector<MslSpecConstant> specConstants;
  };

  // Result of translating one shader stage.
  struct MslShader {
    bool          ok = false;
    std::string   source;       // MSL text; entry point is always "main0"
    std::string   error;        // populated when ok == false
    MslReflection reflection;
  };

  // Translate one SPIR-V shader stage to MSL + reflection.
  //
  // \param code         SPIR-V from DxvkShader::getCode (raw, no binding remap)
  // \param layout       the shader's DxvkPipelineLayoutBuilder (getLayout)
  // \param stage        the shader stage (vertex / fragment)
  // \param samplerHeapSet the descriptor set the sampler heap lives in
  //                       (shader->info().samplerHeap.getSet())
  MslShader convertSpirvToMsl(
    const SpirvCodeBuffer&            code,
    const DxvkPipelineLayoutBuilder&  layout,
          VkShaderStageFlagBits       stage,
          uint32_t                    samplerHeapSet);

}
