#pragma once

#include <cstdint>
#include <string>

#include "dxvk_shader.h"

#include "../spirv/spirv_code_buffer.h"

// SPIR-V shader (shim). The D3D9 fixed-function path constructs one of these
// from a create-info plus an embedded SPIR-V array, then holds it as
// Rc<DxvkShader>. The triangle the shim renders comes from a hardcoded MSL
// pipeline in D9mtBackend, so the SPIR-V is stored but never used: every method
// returns an empty/trivial result. Matches the real DxvkSpirvShader signatures
// so the frontend compiles unmodified (SHIM_SPEC.md).

namespace dxvk {

  // Mirrors the fields the frontend sets on d3d9_fixed_function.cpp's info
  // (bindingCount, bindings, flatShadingInputs, sharedPushData, localPushData,
  // samplerHeap, specDataBuffer, debugName). xfbRasterizedStream/patchVertexCount
  // are kept to match the real struct even though the FF path leaves them default.
  struct DxvkSpirvShaderCreateInfo {
    uint32_t              bindingCount = 0u;
    const DxvkBindingInfo* bindings = nullptr;
    uint32_t              flatShadingInputs = 0u;
    DxvkPushDataBlock     sharedPushData;
    DxvkPushDataBlock     localPushData;
    DxvkShaderBinding     samplerHeap;
    DxvkShaderBinding     specDataBuffer;
    int32_t               xfbRasterizedStream = 0;
    uint32_t              patchVertexCount = 0u;
    std::string           debugName;
  };


  class DxvkSpirvShader : public DxvkShader {

  public:

    // Primary ctor: takes ownership of the SPIR-V buffer. We keep the code and
    // debug name only for identity/logging; nothing executes it.
    DxvkSpirvShader(
      const DxvkSpirvShaderCreateInfo&  info,
            SpirvCodeBuffer&&           spirv)
    : m_info(info), m_code(std::move(spirv)), m_debugName(info.debugName) {
      m_metadata.stage = stageFromPushData(info);
      m_metadata.flatShadingInputs = info.flatShadingInputs;
    }

    // Convenience ctor used by the FF path: `DxvkSpirvShader(info, codeArray)`.
    template<size_t N>
    DxvkSpirvShader(
      const DxvkSpirvShaderCreateInfo&  info,
      const uint32_t(&code)[N])
    : DxvkSpirvShader(info, SpirvCodeBuffer(N, code)) { }

    ~DxvkSpirvShader() = default;

    DxvkShaderMetadata getShaderMetadata() {
      return m_metadata;
    }

    void compile() {
      // No real compilation in the shim — the triangle pipeline is prebuilt.
    }

    // Hand back the stored SPIR-V. The shader-conversion module translates it to
    // MSL; we do NOT remap bindings (no Vulkan pipeline), so the binding map and
    // linkage are ignored — the converter reflects raw decorations + getLayout().
    SpirvCodeBuffer getCode(
      const DxvkShaderBindingMap*       /*bindings*/,
      const DxvkShaderLinkage*          /*linkage*/) {
      return SpirvCodeBuffer(m_code.dwords(), m_code.data());
    }

    // Reconstruct the descriptor/push layout from the create-info so the
    // converter can reflect resource slots and push blocks. Block 0 is the
    // shared push block; block 1 is this stage's local push block (matching the
    // srcOffset formula in dxvk_shader_convert.cpp).
    DxvkPipelineLayoutBuilder getLayout() {
      DxvkPipelineLayoutBuilder builder(m_metadata.stage);
      if (m_info.bindingCount && m_info.bindings)
        builder.addBindings(m_info.bindingCount, m_info.bindings);
      builder.addPushData(0u, m_info.sharedPushData);
      builder.addPushData(1u, m_info.localPushData);
      return builder;
    }

    void dump(std::ostream& outputStream) {
      // Emit the raw words for tooling parity; harmless and rarely called.
      m_code.store(outputStream);
    }

    std::string debugName() {
      return m_debugName;
    }

  private:

    DxvkSpirvShaderCreateInfo m_info;
    SpirvCodeBuffer           m_code;
    std::string               m_debugName;
    DxvkShaderMetadata        m_metadata = { };

    // The FF path encodes the owning stage in localPushData's stage flags;
    // recover it so metadata().stage is meaningful for the pipeline grouping.
    static VkShaderStageFlagBits stageFromPushData(const DxvkSpirvShaderCreateInfo& info) {
      VkShaderStageFlags stage = info.localPushData.getStageMask();
      if (stage & VK_SHADER_STAGE_VERTEX_BIT)   return VK_SHADER_STAGE_VERTEX_BIT;
      if (stage & VK_SHADER_STAGE_FRAGMENT_BIT) return VK_SHADER_STAGE_FRAGMENT_BIT;
      return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    }

  };

}
