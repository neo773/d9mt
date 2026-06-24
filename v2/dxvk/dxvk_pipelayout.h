#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

#include "dxvk_hash.h"
#include "dxvk_include.h"

// Minimal pipeline-layout shim. The real DXVK header (../dxvk-ref/) is a 1800+
// line descriptor/Vulkan-pipeline-layout engine; the Metal shim never builds a
// Vulkan pipeline (the one triangle uses a hardcoded MSL pipeline in
// D9mtBackend). We expose ONLY the value types the unmodified frontend and the
// shim's own shader headers construct/hold:
//   DxvkDescriptorFlag(s), DxvkBindingInfo  (d3d9_fixed_function.cpp)
//   DxvkPushDataBlock, DxvkShaderBinding    (create-info fields)
//   DxvkShaderBindingMap, DxvkPipelineLayoutBuilder (opaque, getCode/getLayout)
// All bodies are trivial — nothing downstream consumes them on the render path.
// Signatures mirror dxvk-ref so the frontend compiles unmodified (SHIM_SPEC.md).

namespace dxvk {

  // --- Descriptor binding metadata -----------------------------------------

  enum class DxvkDescriptorFlag : uint8_t {
    UniformBuffer   = 0u,
    Multisampled    = 1u,
    PushData        = 2u,
  };

  using DxvkDescriptorFlags = Flags<DxvkDescriptorFlag>;

  // Order-invariant store descriptor (real DXVK uses this to relax barriers).
  // The Metal shim issues no barriers, but the IR binding-model pass tags UAV
  // stores with one, so the value type must exist and round-trip bit-exactly.
  struct DxvkAccessOp {
    static constexpr uint32_t StoreValueBits = 12u;

    enum OpType : uint16_t {
      None    = 0x0u, Or = 0x1u, And = 0x2u, Xor = 0x3u, Add = 0x4u,
      IMin    = 0x5u, IMax = 0x6u, UMin = 0x7u, UMax = 0x8u, Load = 0x9u,
      StoreF  = 0xdu, StoreUi = 0xeu, StoreSi = 0xfu,
    };

    DxvkAccessOp() = default;
    DxvkAccessOp(OpType t) : op(uint16_t(t)) { }
    DxvkAccessOp(OpType t, uint16_t constant) : op(uint16_t(t) | (constant << 4u)) { }

    uint16_t op = 0u;

    bool operator == (const DxvkAccessOp& t) const { return op == t.op; }
    bool operator != (const DxvkAccessOp& t) const { return op != t.op; }

    template<typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
    explicit operator T() const { return op; }
  };

  // Mirrors the real struct's public fields. The FF path sets a subset
  // (set/binding/resourceIndex/descriptorType/access/flags/descriptorCount/
  // blockOffset); the rest stay default so the layout is value-copyable.
  struct DxvkBindingInfo {
    uint32_t            set             = 0u;
    uint32_t            binding         = 0u;
    uint32_t            resourceIndex   = 0u;
    VkDescriptorType    descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t            descriptorCount = 1u;
    VkImageViewType     viewType        = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkAccessFlags       access          = 0u;
    DxvkDescriptorFlags flags           = 0u;
    DxvkAccessOp        accessOp        = DxvkAccessOp::None;
    uint32_t            blockOffset     = 0u;
  };

  // --- Push data block -------------------------------------------------------

  // Maps to a shader push-constant range. The shim only needs the two ctors the
  // FF path calls plus getStageMask() (read by DxvkSpirvShader to recover the
  // owning stage). Layout is bit-compatible with the real class.
  class DxvkPushDataBlock {

  public:

    constexpr static uint32_t MaxBlockCount = 6u;

    DxvkPushDataBlock() = default;

    DxvkPushDataBlock(
            VkShaderStageFlags stages,
            uint32_t           offset,
            uint32_t           size,
            uint32_t           alignment,
            uint64_t           resourceMask)
    : m_stageMask(uint16_t(stages)), m_alignment(uint16_t(alignment)),
      m_offset(uint16_t(offset)), m_size(uint16_t(size)),
      m_resourceMask(resourceMask) { }

    // Shared-block form: no stage flags (available to all stages).
    DxvkPushDataBlock(
            uint32_t           offset,
            uint32_t           size,
            uint32_t           alignment,
            uint64_t           resourceMask)
    : DxvkPushDataBlock(0u, offset, size, alignment, resourceMask) { }

    VkShaderStageFlags getStageMask() const { return m_stageMask; }
    uint32_t getOffset() const { return m_offset; }
    uint32_t getSize() const { return m_size; }
    uint64_t getResourceDwordMask() const { return m_resourceMask; }

    // Push-data block index for a stage: 0 = shared (multi-stage / compute),
    // else one block per graphics stage. Mirrors real DXVK so the IR pass's
    // offsets line up with what the shader-conversion module reflects.
    static uint32_t computeIndex(VkShaderStageFlags stageMask) {
      if (stageMask & VK_SHADER_STAGE_COMPUTE_BIT)
        return 0u;
      uint32_t remainder = stageMask & (stageMask - 1u);
      return remainder ? 0u : (bit::tzcnt(uint32_t(stageMask)) + 1u);
    }

    static uint32_t computeBlockOffsetForIndex(uint32_t index) {
      return index ? MaxSharedPushDataSize + MaxPerStagePushDataSize * (index - 1u) : 0u;
    }

    static uint32_t computeBlockOffsetForStage(VkShaderStageFlags stageMask) {
      return computeBlockOffsetForIndex(computeIndex(stageMask));
    }

    static uint32_t computeBlockSizeForStage(VkShaderStageFlags stageMask) {
      if (stageMask & VK_SHADER_STAGE_COMPUTE_BIT)
        return MaxTotalPushDataSize - MaxReservedPushDataSize;
      if (stageMask & (stageMask - 1u))
        return MaxSharedPushDataSize;
      if (stageMask == VK_SHADER_STAGE_FRAGMENT_BIT)
        return (MaxTotalPushDataSize - MaxReservedPushDataSize)
             - computeBlockOffsetForStage(VK_SHADER_STAGE_FRAGMENT_BIT);
      return MaxPerStagePushDataSize;
    }

    bool eq(const DxvkPushDataBlock& o) const {
      return m_stageMask == o.m_stageMask && m_offset == o.m_offset
          && m_size == o.m_size && m_resourceMask == o.m_resourceMask;
    }

    size_t hash() const {
      return size_t(m_stageMask) ^ (size_t(m_offset) << 8)
           ^ (size_t(m_size) << 16) ^ size_t(m_resourceMask);
    }

  private:

    uint16_t m_stageMask    = 0u;
    uint16_t m_alignment    = 0u;
    uint16_t m_offset       = 0u;
    uint16_t m_size         = 0u;
    uint64_t m_resourceMask = 0u;

  };

  // --- Shader binding (set/binding identity) --------------------------------

  class DxvkShaderBinding {

  public:

    DxvkShaderBinding() = default;

    DxvkShaderBinding(
            VkShaderStageFlags stages,
            uint32_t           set,
            uint32_t           binding)
    : m_stages(uint8_t(stages)), m_set(uint8_t(set)),
      m_binding(uint16_t(binding)) { }

    VkShaderStageFlags getStageMask() const { return VkShaderStageFlags(m_stages); }
    uint32_t getSet() const { return m_set; }
    uint32_t getBinding() const { return m_binding; }

    bool eq(const DxvkShaderBinding& o) const {
      return m_stages == o.m_stages && m_set == o.m_set && m_binding == o.m_binding;
    }

    size_t hash() const {
      return size_t(m_stages) | (size_t(m_set) << 8) | (size_t(m_binding) << 16);
    }

  private:

    uint8_t  m_stages  = 0u;
    uint8_t  m_set     = 0u;
    uint16_t m_binding = 0u;

  };

  // --- Shader descriptor + binding range (read side for reflection) ---------

  // One resource binding the shader declares, in the form the shader-conversion
  // module (dxvk_shader_convert.cpp) reflects against. Built from a
  // DxvkBindingInfo; exposes the same accessors as the real DxvkShaderDescriptor.
  class DxvkShaderDescriptor {

  public:

    DxvkShaderDescriptor() = default;

    explicit DxvkShaderDescriptor(const DxvkBindingInfo& info)
    : m_info(info) { }

    // Stage-carrying form the IR binding-model pass builds. The shim's read
    // side (the shader-conversion module) only consults set/binding/resource/
    // type/flags, so the stage mask is recorded but otherwise unused here.
    DxvkShaderDescriptor(const DxvkBindingInfo& info, VkShaderStageFlags stages)
    : m_info(info), m_stages(stages) { }

    VkShaderStageFlags getStageMask() const { return m_stages; }

    VkDescriptorType getDescriptorType() const { return m_info.descriptorType; }
    uint32_t getSet() const { return m_info.set; }
    uint32_t getBinding() const { return m_info.binding; }
    uint32_t getResourceIndex() const { return m_info.resourceIndex; }
    uint32_t getBlockOffset() const { return m_info.blockOffset; }

    bool isUniformBuffer() const {
      return m_info.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
          || m_info.flags.test(DxvkDescriptorFlag::UniformBuffer);
    }

  private:

    DxvkBindingInfo    m_info   = { };
    VkShaderStageFlags m_stages = 0u;

  };

  // A view over a shader's descriptor bindings (matches the real struct's two
  // fields). Returned by DxvkPipelineLayoutBuilder::getBindings().
  struct DxvkPipelineBindingRange {
    size_t                      bindingCount = 0u;
    const DxvkShaderDescriptor* bindings     = nullptr;
  };

  // --- Binding map / layout builder (opaque on the shim render path) --------

  // The shim never remaps bindings (no Vulkan pipeline). getCode() takes a
  // pointer to one but ignores it; we provide an empty, copyable container.
  class DxvkShaderBindingMap {

  public:

    void addBinding(DxvkShaderBinding src, DxvkShaderBinding dst) {
      m_bindings.insert({ src, dst });
    }

    const DxvkShaderBinding* mapBinding(DxvkShaderBinding src) const {
      auto it = m_bindings.find(src);
      return it != m_bindings.end() ? &it->second : nullptr;
    }

    // The shim never relocates push data (it has no merged Vulkan pipeline
    // layout), so a push-data offset maps to itself. Present so the IR shader's
    // ResourceMapping compiles; only reached when a non-null map is passed.
    uint32_t mapPushData(VkShaderStageFlags stage, uint32_t offset) const {
      (void) stage;
      return offset;
    }

  private:

    std::unordered_map<DxvkShaderBinding, DxvkShaderBinding, DxvkHash, DxvkEq> m_bindings;

  };

  // ---- Built-in compute path types (format conversion, dead on FF path) ----
  // d3d9_format_helpers.cpp builds a compute pipeline to convert video formats.
  // The shim never dispatches it, but the class is constructed by the device, so
  // these opaque stand-ins must exist. None carry real GPU state.

  // Which internal command buffer a recorded command targets. The shim records
  // nothing; only the ExecBuffer enumerator is referenced.
  enum class DxvkCmdBuffer : uint32_t {
    ExecBuffer = 0,
    InitBuffer = 1,
    SdmaBuffer = 2,
  };

  // Opaque descriptor payload (a Vulkan descriptor in real DXVK). The shim's
  // views hand one out so DxvkDescriptorWrite can hold it; never dereferenced.
  struct DxvkDescriptor { };

  // One descriptor write for a built-in pipeline bind. The shim ignores it.
  struct DxvkDescriptorWrite {
    VkDescriptorType      descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    const DxvkDescriptor* descriptor     = nullptr;
  };

  // One descriptor-set binding for a built-in pipeline layout. Aggregate-
  // initialized by d3d9_format_helpers.cpp as { type, count, stages }.
  struct DxvkDescriptorSetLayoutBinding {
    VkDescriptorType   descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t           descriptorCount = 0u;
    VkShaderStageFlags stageMask       = 0u;
  };

  // Opaque pipeline layout (real DXVK manages a VkPipelineLayout). The format
  // helper holds a const pointer to one; the shim never builds a real layout.
  class DxvkPipelineLayout { };

  // Returned by every shim shader's getLayout(). The shim shaders populate one
  // from their create-info (addBindings/addPushData) so the shader-conversion
  // module can reflect resource slots + push blocks off it. It does NOT build a
  // Vulkan pipeline layout — it is just the descriptor/push bookkeeping the
  // converter reads.
  class DxvkPipelineLayoutBuilder {

  public:

    DxvkPipelineLayoutBuilder() = default;
    explicit DxvkPipelineLayoutBuilder(VkShaderStageFlags stageMask)
    : m_stageMask(stageMask) { }

    VkShaderStageFlags getStageMask() const { return m_stageMask; }

    // Append `count` bindings (the FF shim passes its create-info bindings).
    void addBindings(uint32_t count, const DxvkBindingInfo* bindings) {
      for (uint32_t i = 0; i < count; i++)
        m_bindings.emplace_back(bindings[i]);
    }

    // Pre-wrapped form the IR binding-model pass emits (carries stage mask).
    void addBindings(uint32_t count, const DxvkShaderDescriptor* bindings) {
      for (uint32_t i = 0; i < count; i++)
        m_bindings.push_back(bindings[i]);
    }

    // The sampler heap / spec-data buffer are declared by the IR pass so the
    // backend can remap them; the shim only needs to remember the bindings.
    void addSamplerHeap(const DxvkShaderBinding& binding) {
      m_samplerHeaps.push_back(binding);
    }

    void addSpecDataBuffer(const DxvkShaderBinding& binding) {
      m_specDataBuffers.push_back(binding);
    }

    // Record a push-data block at the given block index (0 = shared, 1.. per
    // stage); marks it present in the push mask.
    void addPushData(uint32_t index, const DxvkPushDataBlock& block) {
      if (index < DxvkPushDataBlock::MaxBlockCount) {
        m_pushData[index] = block;
        m_pushMask |= (1u << index);
      }
    }

    // Stage-derived form the IR pass emits; the block index follows from its
    // stage mask (shared block 0 for multi-stage data, else one per stage).
    void addPushData(const DxvkPushDataBlock& block) {
      addPushData(DxvkPushDataBlock::computeIndex(block.getStageMask()), block);
    }

    DxvkPipelineBindingRange getBindings() const {
      DxvkPipelineBindingRange range;
      range.bindingCount = m_bindings.size();
      range.bindings     = m_bindings.data();
      return range;
    }

    uint32_t getPushDataMask() const { return m_pushMask; }
    DxvkPushDataBlock getPushDataBlock(uint32_t index) const { return m_pushData[index]; }

    // Descriptor set the bindless sampler heap was declared in (addSamplerHeap),
    // or ~0u if the shader uses no samplers. The shader-conversion module needs
    // this to locate the heap's [[buffer]] index from SPIRV-Cross.
    uint32_t getSamplerHeapSet() const {
      return m_samplerHeaps.empty() ? ~0u : m_samplerHeaps.front().getSet();
    }

  private:

    VkShaderStageFlags m_stageMask = 0u;
    uint32_t           m_pushMask  = 0u;
    std::vector<DxvkShaderDescriptor> m_bindings;
    std::array<DxvkPushDataBlock, DxvkPushDataBlock::MaxBlockCount> m_pushData = { };
    std::vector<DxvkShaderBinding> m_samplerHeaps;
    std::vector<DxvkShaderBinding> m_specDataBuffers;

  };

}
