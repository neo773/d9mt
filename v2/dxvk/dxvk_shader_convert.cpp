// SPIR-V -> MSL + reflection. See dxvk_shader_convert.h.
//
// The SPIRV-Cross invocation and the slot/AB/push/sampler reflection are copied
// from v1's proven path (d3d9fe/d9mt_shader.cpp, the SPIR-V->MSL section), with
// everything that was NOT shader conversion removed: no metallib cache, no
// runtime compile, no sampler-heap storage, no device handles, no Logger. The
// caller decides what to do with failures.

#include "dxvk_shader_convert.h"

#include <algorithm>

#include "../../vendor/spirv-cross/spirv_msl.hpp"

namespace dxvk {

  namespace {
    // Shared-block source offset into the D3D9 constant store for a given push
    // block index — the same formula DxvkContext::computePushDataBlockOffset uses
    // in the (kept) frontend. Block 0 is the shared block at offset 0; later
    // blocks are per-stage, packed after the shared region.
    uint32_t pushDataBlockSrcOffset(uint32_t index) {
      return index
        ? MaxSharedPushDataSize + MaxPerStagePushDataSize * (index - 1u)
        : 0u;
    }
  }

  MslShader convertSpirvToMsl(
    const SpirvCodeBuffer&            code,
    const DxvkPipelineLayoutBuilder&  layout,
          VkShaderStageFlagBits       stage,
          uint32_t                    samplerHeapSet) {
    MslShader out;
    out.reflection.stage = stage;


    // -------- SPIR-V -> MSL + SPIRV-Cross reflection ------------------------
    try {
      spirv_cross::CompilerMSL compiler(code.data(), code.dwords());

      spirv_cross::CompilerMSL::Options opts;
      opts.set_msl_version(3, 0, 0);
      opts.argument_buffers = true;
      opts.argument_buffers_tier =
        spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
      compiler.set_msl_options(opts);

      spirv_cross::ShaderResources res = compiler.get_shader_resources();

      // Fresh-upstream D3D9 shaders declare a bindless descriptor heap as an
      // unsized array (e.g. `uniform sampler sampler_heap[]`). SPIRV-Cross MSL
      // refuses to place a runtime-sized array in an argument buffer unless that
      // set is flagged device-address-space, so flag every set that holds one.
      auto markRuntimeArraySetsDevice = [&] (const spirv_cross::SmallVector<spirv_cross::Resource>& list) {
        for (const auto& r : list) {
          const auto& type = compiler.get_type(r.type_id);
          bool runtimeSized = !type.array.empty() && type.array.back() == 0u;
          if (runtimeSized) {
            uint32_t set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
            compiler.set_argument_buffer_device_address_space(set, true);
          }
        }
      };
      markRuntimeArraySetsDevice(res.separate_samplers);
      markRuntimeArraySetsDevice(res.separate_images);
      markRuntimeArraySetsDevice(res.sampled_images);
      markRuntimeArraySetsDevice(res.storage_buffers);
      markRuntimeArraySetsDevice(res.storage_images);

      out.source = compiler.compile();

      // SPIRV-Cross knows each resource's set, binding, AB slot, and kind
      // directly — build the resource refs straight from its reflection rather
      // than re-deriving them from the (push-only) pipeline layout. The FF
      // constant buffers live in argument-buffer sets 2/3, so we keep every set.
      auto reflectArgumentResource = [&] (const spirv_cross::Resource& r,
                                          VkDescriptorType type, bool isUniformBuffer) {
        uint32_t abId = compiler.get_automatic_msl_resource_binding(r.id);
        if (abId == uint32_t(-1))   // dead resource: never entered the argument buffer
          return;

        MslResourceRef ref;
        ref.set             = uint16_t(compiler.get_decoration(r.id, spv::DecorationDescriptorSet));
        ref.slot            = uint16_t(compiler.get_decoration(r.id, spv::DecorationBinding));
        ref.abId            = uint16_t(abId);
        ref.type            = type;
        ref.isUniformBuffer = isUniformBuffer;
        out.reflection.resources.push_back(ref);
      };

      for (const auto& r : res.uniform_buffers)
        reflectArgumentResource(r, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, true);
      for (const auto& r : res.storage_buffers)
        reflectArgumentResource(r, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, true);
      for (const auto& r : res.separate_images)
        reflectArgumentResource(r, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, false);
      for (const auto& r : res.sampled_images)
        reflectArgumentResource(r, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, false);

      for (const auto& r : res.push_constant_buffers)
        out.reflection.pushBufferIndex =
          int32_t(compiler.get_automatic_msl_resource_binding(r.id));

      for (const auto& r : res.separate_samplers) {
        if (compiler.get_decoration(r.id, spv::DecorationDescriptorSet) == samplerHeapSet)
          out.reflection.samplerHeapIndex =
            int32_t(compiler.get_automatic_msl_resource_binding(r.id));
      }

      for (const auto& sc : compiler.get_specialization_constants()) {
        const auto& cst  = compiler.get_constant(sc.id);
        const auto& type = compiler.get_type(cst.constant_type);

        MslSpecConstant info;
        info.id           = sc.constant_id;
        info.defaultValue = cst.scalar(0, 0);
        info.isBool       = type.basetype == spirv_cross::SPIRType::Boolean;
        out.reflection.specConstants.push_back(info);
      }
    } catch (const std::exception& e) {
      out.ok    = false;
      out.error = e.what();
      return out;
    }

    // -------- layout metadata: samplers + push blocks -----------------------
    // Resources came from SPIRV-Cross above. The pipeline layout supplies the
    // two things SPIR-V reflection can't: sampler descriptors (no Binding
    // decoration in the D3D9 path — matched by resourceIndex) and the push-data
    // block ranges.
    DxvkPipelineBindingRange bindings = layout.getBindings();

    for (size_t i = 0; i < bindings.bindingCount; i++) {
      const DxvkShaderDescriptor& binding = bindings.bindings[i];

      if (binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_SAMPLER) {
        MslSamplerRef ref;
        ref.slot        = uint16_t(binding.getResourceIndex());
        ref.blockOffset = uint16_t(binding.getBlockOffset());
        out.reflection.samplers.push_back(ref);
      }
    }

    uint32_t pushMask = layout.getPushDataMask();
    for (uint32_t index = 0; index < DxvkPushDataBlock::MaxBlockCount; index++) {
      if (!(pushMask & (1u << index)))
        continue;

      DxvkPushDataBlock block = layout.getPushDataBlock(index);

      MslPushBlock info;
      info.dstOffset    = block.getOffset();
      info.size         = block.getSize();
      // Source from the same stage region the frontend wrote into, so per-stage
      // blocks read their own data instead of a clobbered shared region. The
      // frontend writes each whole block at its region's local offset 0.
      info.srcOffset    = pushRegionBaseForStages(block.getStageMask());
      info.resourceMask = block.getResourceDwordMask();
      out.reflection.pushBlocks.push_back(info);

      out.reflection.pushDataSize = std::max(out.reflection.pushDataSize,
        info.dstOffset + info.size);
    }

    out.ok = true;
    return out;
  }

}
