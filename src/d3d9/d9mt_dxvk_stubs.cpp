// Link stubs for DXVK runtime symbols referenced only from DEAD code in the
// vendored translation units (DxvkShaderPipelineLibrary, D3D9FFShader,
// pipeline-manager paths). The host translate-test links the same sources
// with -dead_strip and runs the full translation chain without ever
// reaching these, proving they are unreachable from
// DxsoModule::compile()/DxvkShader::getRawCode(). PE ld resolves symbols
// before --gc-sections can discard the dead sections, so the references
// must be satisfied; any call into a stub aborts loudly.
#include <cstdlib>

#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_pipemanager.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_sampler.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_util.h"
#include "../../vendor/dxvk/src/vulkan/vulkan_loader.h"
#include "../../vendor/dxvk/src/vulkan/vulkan_names.h"

#define D9MT_STUB_BODY                                                         \
  { std::abort(); }

namespace dxvk {

DxvkDevice::~DxvkDevice() D9MT_STUB_BODY

bool DxvkDevice::canUseGraphicsPipelineLibrary() const D9MT_STUB_BODY

bool DxvkDevice::mustTrackPipelineLifetime() const D9MT_STUB_BODY

VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const D9MT_STUB_BODY

void DxvkDevice::registerShader(const Rc<DxvkShader> &shader) D9MT_STUB_BODY

DxvkDescriptorUpdateList::DxvkDescriptorUpdateList(
    DxvkDevice *device, uint32_t setSize, uint32_t descriptorCount,
    const DxvkDescriptorUpdateInfo *descriptorInfos) D9MT_STUB_BODY

DxvkDescriptorUpdateList::~DxvkDescriptorUpdateList() D9MT_STUB_BODY

DxvkSamplerDescriptorSet
DxvkSamplerDescriptorHeap::getDescriptorSetInfo() const D9MT_STUB_BODY

const DxvkDescriptorSetLayout *DxvkPipelineManager::createDescriptorSetLayout(
    const DxvkDescriptorSetLayoutKey &key) D9MT_STUB_BODY

const DxvkPipelineLayout *DxvkPipelineManager::createPipelineLayout(
    const DxvkPipelineLayoutKey &key) D9MT_STUB_BODY

namespace vk {
DeviceFn::~DeviceFn() D9MT_STUB_BODY
} // namespace vk

namespace util {
uint32_t getComponentIndex(VkComponentSwizzle component,
                           uint32_t identity) D9MT_STUB_BODY

bool isIdentityMapping(VkComponentMapping mapping) D9MT_STUB_BODY
} // namespace util

} // namespace dxvk

std::ostream &operator<<(std::ostream &os, VkResult e) D9MT_STUB_BODY
