// d9mt: Metal backend — DxvkDevice + DxvkObjects member classes.
//
// Implements the device-level surface of docs/BACKEND-SURFACE.md §2.3/§2.4:
// DxvkAdapter::createDevice, the DxvkDevice constructor member cascade
// (DescriptorProperties, MemoryAllocator, SamplerPool, PipelineManager,
// GpuEventPool, GpuQueryPool, UnboundResources), a THREADLESS
// DxvkSubmissionQueue, device factories and waits, and a fake Vulkan device
// dispatch table (vk::DeviceFn) that services the few vkCreate*/vkDestroy*
// calls made by vendored CPU-side code (dxvk_pipelayout.cpp).
//
// See docs/METAL-BACKEND-NOTES.md "Stage decisions: device" for rationale.

#include <algorithm>
#include <chrono>
#include <cstring>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_queue.h"

#include "../../vendor/spirv-cross/spirv_msl.hpp"

namespace dxvk::d9mt {

  namespace {

    // Fake dispatchable Vulkan handles (pointers on i686): non-null, stable,
    // never dereferenced — they only have to satisfy null checks.
    template<typename T>
    T fakeHandle() {
      static char s_dummy;
      return reinterpret_cast<T>(&s_dummy);
    }

    // Fake non-dispatchable handles (u64 on i686): unique non-zero cookies.
    std::atomic<uint64_t> g_fakeCookie = { 0u };

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateDescriptorSetLayout(
            VkDevice, const VkDescriptorSetLayoutCreateInfo*,
      const VkAllocationCallbacks*, VkDescriptorSetLayout* pSetLayout) {
      *pSetLayout = ++g_fakeCookie;
      return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyDescriptorSetLayout(
            VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {
    }

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateDescriptorUpdateTemplate(
            VkDevice, const VkDescriptorUpdateTemplateCreateInfo*,
      const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* pTemplate) {
      *pTemplate = ++g_fakeCookie;
      return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyDescriptorUpdateTemplate(
            VkDevice, VkDescriptorUpdateTemplate, const VkAllocationCallbacks*) {
    }

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreatePipelineLayout(
            VkDevice, const VkPipelineLayoutCreateInfo*,
      const VkAllocationCallbacks*, VkPipelineLayout* pPipelineLayout) {
      *pPipelineLayout = ++g_fakeCookie;
      return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyPipelineLayout(
            VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyPipeline(
            VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*) {
      // ~D3D9FormatHelper destroys its built-in compute pipelines through
      // vkd(); the handle is a BuiltInComputePipeline* minted by
      // DxvkDevice::createBuiltInComputePipeline below
      if (!pipeline)
        return;

      auto* p = reinterpret_cast<BuiltInComputePipeline*>(uintptr_t(pipeline));
      if (p->pso)
        NSObject_release(p->pso);
      if (p->library)
        NSObject_release(p->library);
      delete p;
    }

    // The vendored DxvkCommandList inline methods route cmdBindPipeline /
    // cmdDispatch / cmdPipelineBarrier through this dispatch table for the
    // D3D9FormatHelper conversion path (BACKEND-SURFACE §1.8). The
    // VkCommandBuffer is the DxvkCommandList pointer, smuggled in by
    // DxvkCommandList::init (d9mt_context.cpp).
    VKAPI_ATTR void VKAPI_CALL fakeCmdBindPipeline(
            VkCommandBuffer commandBuffer,
            VkPipelineBindPoint pipelineBindPoint,
            VkPipeline pipeline) {
      if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_COMPUTE || !pipeline) {
        Logger::err("d9mt: vkCmdBindPipeline: only built-in compute pipelines supported");
        return;
      }
      auto* p = reinterpret_cast<BuiltInComputePipeline*>(uintptr_t(pipeline));
      cmdListBindComputePipeline(reinterpret_cast<const void*>(commandBuffer),
        p->pso, p->threadgroupSize);
    }

    VKAPI_ATTR void VKAPI_CALL fakeCmdDispatch(
            VkCommandBuffer commandBuffer,
            uint32_t x, uint32_t y, uint32_t z) {
      cmdListDispatch(reinterpret_cast<const void*>(commandBuffer), x, y, z);
    }

    VKAPI_ATTR void VKAPI_CALL fakeCmdPipelineBarrier2(
            VkCommandBuffer commandBuffer,
      const VkDependencyInfo*) {
      // compute-write → consumer barrier: Metal encoder boundaries provide
      // cross-encoder ordering, so ending the compute encoder suffices
      cmdListEndEncoder(reinterpret_cast<const void*>(commandBuffer));
    }

    // Trampoline for every Vulkan device function we do not service. Calling
    // it through an arbitrary signature is fine because it never returns.
    VKAPI_ATTR void VKAPI_CALL fakeUnimplemented() {
      Logger::err("d9mt: unimplemented Vulkan device function called via fake dispatch");
      logf("FATAL: unimplemented Vulkan device function called via fake dispatch");
      std::abort();
    }

    struct FakeVkEntry {
      const char*        name;
      PFN_vkVoidFunction fn;
    };

    const FakeVkEntry g_fakeVkFunctions[] = {
      { "vkCreateDescriptorSetLayout",      reinterpret_cast<PFN_vkVoidFunction>(&fakeCreateDescriptorSetLayout) },
      { "vkDestroyDescriptorSetLayout",     reinterpret_cast<PFN_vkVoidFunction>(&fakeDestroyDescriptorSetLayout) },
      { "vkCreateDescriptorUpdateTemplate", reinterpret_cast<PFN_vkVoidFunction>(&fakeCreateDescriptorUpdateTemplate) },
      { "vkDestroyDescriptorUpdateTemplate",reinterpret_cast<PFN_vkVoidFunction>(&fakeDestroyDescriptorUpdateTemplate) },
      { "vkCreatePipelineLayout",           reinterpret_cast<PFN_vkVoidFunction>(&fakeCreatePipelineLayout) },
      { "vkDestroyPipelineLayout",          reinterpret_cast<PFN_vkVoidFunction>(&fakeDestroyPipelineLayout) },
      { "vkDestroyPipeline",                reinterpret_cast<PFN_vkVoidFunction>(&fakeDestroyPipeline) },
      { "vkCmdBindPipeline",                reinterpret_cast<PFN_vkVoidFunction>(&fakeCmdBindPipeline) },
      { "vkCmdDispatch",                    reinterpret_cast<PFN_vkVoidFunction>(&fakeCmdDispatch) },
      { "vkCmdPipelineBarrier2",            reinterpret_cast<PFN_vkVoidFunction>(&fakeCmdPipelineBarrier2) },
    };

  } // anonymous namespace

} // namespace dxvk::d9mt


namespace dxvk::vk {

  // ==========================================================================
  // Fake Vulkan dispatch (d9mt). There is no Vulkan ICD in this process; the
  // loader classes are constructed once with fake handles so that vendored
  // CPU-side code (dxvk_pipelayout.cpp set/pipeline layout objects) works
  // unmodified. Unknown functions resolve to a loud abort trampoline.
  // ==========================================================================

  LibraryLoader::~LibraryLoader() {
    // constructed only by DxvkAdapter::vki() (d9mt_instance.cpp) with our
    // fake loader proc; owns no real library handle, nothing to release
  }


  InstanceLoader::InstanceLoader(
    const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : m_library(library), m_instance(instance), m_owned(owned) {

  }


  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return reinterpret_cast<PFN_vkVoidFunction>(&d9mt::fakeUnimplemented);
  }


  DeviceLoader::DeviceLoader(
    const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : m_library(library), m_getDeviceProcAddr(nullptr),
    m_device(device), m_owned(owned) {

  }


  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    for (const auto& entry : d9mt::g_fakeVkFunctions) {
      if (!std::strcmp(entry.name, name))
        return entry.fn;
    }
    return reinterpret_cast<PFN_vkVoidFunction>(&d9mt::fakeUnimplemented);
  }


  DeviceFn::DeviceFn(
    const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : DeviceLoader(library, owned, device) {
    // members are filled by the VULKAN_FN default member initializers,
    // which route through DeviceLoader::sym above
  }


  DeviceFn::~DeviceFn() {

  }

}


namespace dxvk {

  // ==========================================================================
  // DxvkAdapter::createDevice
  // ==========================================================================

  Rc<DxvkDevice> DxvkAdapter::createDevice() {
    d9mt::logf("DxvkAdapter::createDevice");

    if (!d9mt::mtlDevice() || !d9mt::mtlCommandQueue())
      throw DxvkError("d9mt: createDevice: no Metal device/queue available");

    // Fake Vulkan device dispatch (see above); the VkDevice handle is a
    // stable dummy pointer that must never be dereferenced.
    Rc<vk::InstanceLoader> instanceLoader = new vk::InstanceLoader(
      nullptr, false, d9mt::fakeHandle<VkInstance>());

    Rc<vk::DeviceFn> vkd = new vk::DeviceFn(
      instanceLoader, false, d9mt::fakeHandle<VkDevice>());

    // Single graphics+compute+transfer queue, family 0 (caps table). The
    // VkQueue handle is a dummy; the real queue is d9mt::mtlCommandQueue().
    DxvkDeviceQueueSet queues = { };
    queues.graphics = { d9mt::fakeHandle<VkQueue>(), 0u, 0u };
    queues.transfer = queues.graphics;

    return new DxvkDevice(
      Rc<DxvkInstance>(m_instance), this, vkd,
      m_capabilities.getFeatures(), queues, DxvkQueueCallback());
  }


  // ==========================================================================
  // DxvkDevice
  // ==========================================================================

  DxvkDevice::DxvkDevice(
    const Rc<DxvkInstance>&   instance,
    const Rc<DxvkAdapter>&    adapter,
    const Rc<vk::DeviceFn>&   vkd,
    const DxvkDeviceFeatures& features,
    const DxvkDeviceQueueSet& queues,
    const DxvkQueueCallback&  queueCallback)
  : m_options         (instance->options()),
    m_instance        (instance),
    m_adapter         (adapter),
    m_vkd             (vkd),
    m_debugFlags      (0u),
    m_queues          (queues),
    m_features        (features),
    m_properties      (adapter->deviceProperties()),
    m_perfHints       (getPerfHints()),
    m_objects         (this),
    m_submissionQueue (this, queueCallback) {
    d9mt::logf("DxvkDevice: created");
  }


  DxvkDevice::~DxvkDevice() {
    // drain all in-flight GPU work so tracked resources are safe to destroy
    this->waitForIdle();
    d9mt::logf("DxvkDevice: destroyed");
  }


  DxvkDevicePerfHints DxvkDevice::getPerfHints() {
    DxvkDevicePerfHints hints = { };
    // deferred clears suit the Metal pass manager (BACKEND-SURFACE §2.3)
    hints.preferRenderPassOps = VK_TRUE;
    return hints;
  }


  bool DxvkDevice::isUnifiedMemoryArchitecture() const {
    return m_adapter->isUnifiedMemoryArchitecture();
  }


  bool DxvkDevice::canUseGraphicsPipelineLibrary() const {
    return false;
  }


  bool DxvkDevice::canUsePipelineCacheControl() const {
    return false;
  }


  bool DxvkDevice::mustTrackPipelineLifetime() const {
    return false;
  }


  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
         | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
         | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }


  Rc<DxvkCommandList> DxvkDevice::createCommandList() {
    Rc<DxvkCommandList> cmdList = m_recycledCommandLists.retrieveObject();

    if (cmdList == nullptr)
      cmdList = new DxvkCommandList(this);

    cmdList->init();
    return cmdList;
  }


  void DxvkDevice::recycleCommandList(const Rc<DxvkCommandList>& cmdList) {
    cmdList->reset();
    m_recycledCommandLists.returnObject(cmdList);
  }


  Rc<DxvkContext> DxvkDevice::createContext() {
    return new DxvkContext(this);
  }


  Rc<DxvkEvent> DxvkDevice::createGpuEvent() {
    return new DxvkEvent(this);
  }


  Rc<DxvkQuery> DxvkDevice::createGpuQuery(
          VkQueryType         type,
          VkQueryControlFlags flags,
          uint32_t            index) {
    return new DxvkQuery(this, type, flags, index);
  }


  Rc<DxvkGpuQuery> DxvkDevice::createRawQuery(VkQueryType type) {
    return m_objects.queryPool().allocQuery(type);
  }


  Rc<DxvkSampler> DxvkDevice::createSampler(const DxvkSamplerKey& createInfo) {
    return m_objects.samplerPool().createSampler(createInfo);
  }


  DxvkLocalAllocationCache DxvkDevice::createAllocationCache(
          VkBufferUsageFlags    bufferUsage,
          VkMemoryPropertyFlags propertyFlags) {
    return m_objects.memoryManager().createAllocationCache(
      bufferUsage, propertyFlags);
  }


  const DxvkPipelineLayout* DxvkDevice::createBuiltInPipelineLayout(
          DxvkPipelineLayoutFlags         flags,
          VkShaderStageFlags              pushDataStages,
          VkDeviceSize                    pushDataSize,
          uint32_t                        bindingCount,
    const DxvkDescriptorSetLayoutBinding* bindings) {
    DxvkDescriptorSetLayoutKey setLayoutKey;
    VkShaderStageFlags stageMask = pushDataStages;

    for (uint32_t i = 0; i < bindingCount; i++) {
      setLayoutKey.add(bindings[i]);
      stageMask |= bindings[i].getStageMask();
    }

    const DxvkDescriptorSetLayout* setLayout =
      m_objects.pipelineManager().createDescriptorSetLayout(setLayoutKey);

    DxvkPipelineLayoutKey key(DxvkPipelineLayoutType::Merged, flags);
    key.addStages(stageMask);
    key.setDescriptorSetLayouts(1u, &setLayout);

    if (pushDataSize) {
      key.addPushData(DxvkPushDataBlock(pushDataStages,
        0u, uint32_t(pushDataSize), sizeof(uint32_t), 0u));
    }

    return m_objects.pipelineManager().createPipelineLayout(key);
  }


  VkPipeline DxvkDevice::createBuiltInComputePipeline(
    const DxvkPipelineLayout*           layout,
    const util::DxvkBuiltInShaderStage& stage) {
    // SPIR-V -> MSL for the 7 embedded D3D9FormatHelper conversion shaders
    // (BACKEND-SURFACE §4.8). Unlike the Draw stage (argument buffers), the
    // built-in pipelines use DIRECT bindings matching the convention in
    // DxvkCommandList::bindResources (METAL-BACKEND-NOTES context item 12):
    // descriptor i of image/texel-buffer type -> [[texture(i)]], push
    // constants -> [[buffer(30)]]. Spec constants (id 0 = YUY2/UYVY select)
    // become Metal function constants resolved at newFunction time.
    std::string msl;
    WMTSize threadgroupSize = { 1u, 1u, 1u };

    struct SpecConstant {
      uint32_t id;
      uint32_t value;
      bool     isBool;
    };
    std::vector<SpecConstant> specConstants;

    try {
      spirv_cross::CompilerMSL compiler(stage.code, stage.size / sizeof(uint32_t));

      spirv_cross::CompilerMSL::Options opts;
      opts.set_msl_version(3, 0, 0);
      opts.texture_buffer_native = true; // usamplerBuffer -> texture_buffer
      compiler.set_msl_options(opts);

      // direct bindings per the bindResources slot convention
      for (uint32_t binding = 0; binding < 2u; binding++) {
        spirv_cross::MSLResourceBinding b = { };
        b.stage = spv::ExecutionModelGLCompute;
        b.desc_set = 0u;
        b.binding = binding;
        b.count = 1u;
        b.msl_texture = binding;
        b.msl_buffer  = binding;
        b.msl_sampler = binding;
        compiler.add_msl_resource_binding(b);
      }

      spirv_cross::MSLResourceBinding push = { };
      push.stage = spv::ExecutionModelGLCompute;
      push.desc_set = spirv_cross::kPushConstDescSet;
      push.binding  = spirv_cross::kPushConstBinding;
      push.count = 1u;
      push.msl_buffer = 30u;
      compiler.add_msl_resource_binding(push);

      msl = compiler.compile();

      threadgroupSize.width = std::max(1u,
        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0));
      threadgroupSize.height = std::max(1u,
        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1));
      threadgroupSize.depth = std::max(1u,
        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2));

      for (const auto& sc : compiler.get_specialization_constants()) {
        const auto& cst  = compiler.get_constant(sc.id);
        const auto& type = compiler.get_type(cst.constant_type);

        SpecConstant info = { };
        info.id     = sc.constant_id;
        info.value  = cst.scalar(0, 0);
        info.isBool = type.basetype == spirv_cross::SPIRType::Boolean;

        // override with the supplied specialization data (VkSpecializationInfo)
        if (stage.spec) {
          for (uint32_t i = 0; i < stage.spec->mapEntryCount; i++) {
            const auto& entry = stage.spec->pMapEntries[i];
            if (entry.constantID != sc.constant_id)
              continue;
            uint32_t value = 0u;
            std::memcpy(&value, reinterpret_cast<const char*>(stage.spec->pData)
              + entry.offset, std::min(sizeof(value), entry.size));
            info.value = value;
          }
        }

        specConstants.push_back(info);
      }
    } catch (const std::exception& e) {
      Logger::err(str::format("d9mt: createBuiltInComputePipeline: "
        "SPIR-V -> MSL translation failed: ", e.what()));
      return VK_NULL_HANDLE;
    }

    obj_handle_t device = d9mt::mtlDevice();
    if (!device)
      return VK_NULL_HANDLE;

    // MSL -> MTLLibrary (d9mtmetal runtime compile)
    d9mt_newlibrary_params lp;
    std::memset(&lp, 0, sizeof(lp));
    lp.device     = device;
    lp.source_ptr = uint64_t(uintptr_t(msl.data()));
    lp.source_len = msl.size();
    lp.fast_math  = 1;

    int status = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
    if (status != 0 || !lp.ret_library) {
      Logger::err(str::format("d9mt: createBuiltInComputePipeline: "
        "MSL compile failed, status ", status));
      if (lp.ret_error)
        d9mt::logNSError("d9mt: built-in compute MSL compile", lp.ret_error);
      return VK_NULL_HANDLE;
    }
    if (lp.ret_error)
      NSObject_release(lp.ret_error); // warnings only

    obj_handle_t library = lp.ret_library;

    // MTLLibrary -> MTLFunction (supplying ALL declared function constants)
    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    obj_handle_t function = 0;

    if (specConstants.empty()) {
      function = MTLLibrary_newFunction(library, "main0");
      if (!function)
        Logger::err("d9mt: createBuiltInComputePipeline: main0 not found");
    } else {
      std::vector<uint32_t> values(specConstants.size());
      std::vector<WMTFunctionConstant> consts(specConstants.size());

      for (size_t i = 0; i < specConstants.size(); i++) {
        values[i] = specConstants[i].value;
        std::memset(&consts[i], 0, sizeof(consts[i]));
        consts[i].data.set(&values[i]);
        consts[i].type = specConstants[i].isBool
          ? WMTDataTypeBool
          : WMTDataTypeUInt;
        consts[i].index = uint16_t(specConstants[i].id);
      }

      obj_handle_t err = 0;
      function = MTLLibrary_newFunctionWithConstants(library, "main0",
        consts.data(), uint32_t(consts.size()), &err);

      if (!function)
        d9mt::logNSError("d9mt: built-in compute newFunctionWithConstants", err);
      else if (err)
        NSObject_release(err);
    }

    if (!function) {
      NSObject_release(pool);
      NSObject_release(library);
      return VK_NULL_HANDLE;
    }

    // MTLFunction -> MTLComputePipelineState
    WMTComputePipelineInfo info = { };
    info.compute_function = function;

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newComputePipelineState(device, &info, &err);

    NSObject_release(function);
    NSObject_release(pool);

    if (!pso) {
      Logger::err("d9mt: createBuiltInComputePipeline: newComputePipelineState failed");
      if (err)
        d9mt::logNSError("d9mt: built-in compute PSO", err);
      NSObject_release(library);
      return VK_NULL_HANDLE;
    }
    if (err)
      NSObject_release(err);

    auto* result = new d9mt::BuiltInComputePipeline();
    result->pso = pso;
    result->library = library;
    result->threadgroupSize = threadgroupSize;

    d9mt::logf("d9mt: built-in compute pipeline created (%zu bytes MSL, "
      "tg %ux%ux%u, spec=%zu)", msl.size(),
      uint32_t(threadgroupSize.width), uint32_t(threadgroupSize.height),
      uint32_t(threadgroupSize.depth), specConstants.size());

    return VkPipeline(uintptr_t(result));
  }


  void DxvkDevice::registerShader(const Rc<DxvkShader>& shader) {
    // no precompilation on the Metal backend: pipelines are
    // compiled on first use, keyed by the full PSO state
  }


  void DxvkDevice::requestCompileShader(const Rc<DxvkShader>& shader) {
    // see registerShader — nothing to precompile
  }


  Rc<DxvkLatencyTracker> DxvkDevice::createLatencyTracker(
    const Rc<Presenter>& presenter) {
    // all front-end uses are null-guarded (BACKEND-SURFACE §2.3)
    return nullptr;
  }


  void DxvkDevice::presentImage(
    const Rc<Presenter>&          presenter,
    const Rc<DxvkLatencyTracker>& tracker,
          uint64_t                frameId,
          DxvkSubmitStatus*       status) {
    // Threadless submission: present synchronously on the calling (CS)
    // thread. Presenter::presentImage guarantees that signalFrame(frameId)
    // eventually fires on EVERY path (success: completion-watcher callback
    // after the present command buffer retires; failure: watcher pure-
    // callback ordered behind all in-flight work) — BACKEND-SURFACE §5.1,
    // §7 risk 6. Do NOT signal here: that would release SyncFrameLatency
    // before the GPU finished the frame.
    VkResult vr = presenter->presentImage(frameId, tracker);

    if (status)
      status->result.store(vr);

    std::lock_guard<sync::Spinlock> lock(m_statLock);
    m_statCounters.addCtr(DxvkStatCounter::QueuePresentCount, 1u);
  }


  VkResult DxvkDevice::waitForSubmission(DxvkSubmitStatus* status) {
    // submissions complete synchronously or via the completion watcher;
    // spin-yield until the status atomic leaves VK_NOT_READY
    VkResult result = status->result.load();

    while (result == VK_NOT_READY) {
      dxvk::this_thread::yield();
      result = status->result.load();
    }

    return result;
  }


  void DxvkDevice::waitForFence(sync::Fence& fence, uint64_t value) {
    auto t0 = dxvk::high_resolution_clock::now();
    fence.wait(value);
    auto t1 = dxvk::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

    std::lock_guard<sync::Spinlock> lock(m_statLock);
    m_statCounters.addCtr(DxvkStatCounter::GpuSyncCount, 1u);
    m_statCounters.addCtr(DxvkStatCounter::GpuSyncTicks, us.count());
  }


  void DxvkDevice::waitForResource(
    const DxvkPagedResource& resource,
          DxvkAccess         access) {
    if (!resource.isInUse(access))
      return;

    // Use counts are dropped by the completion-watcher thread when the
    // tracking command list retires, so polling always makes progress.
    auto t0 = dxvk::high_resolution_clock::now();

    while (resource.isInUse(access))
      dxvk::this_thread::yield();

    auto t1 = dxvk::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

    std::lock_guard<sync::Spinlock> lock(m_statLock);
    m_statCounters.addCtr(DxvkStatCounter::GpuSyncCount, 1u);
    m_statCounters.addCtr(DxvkStatCounter::GpuSyncTicks, us.count());
  }


  void DxvkDevice::waitForIdle() {
    d9mt::watcherWaitIdle();
  }


  VkSubresourceLayout DxvkDevice::queryImageSubresourceLayout(
    const DxvkImageCreateInfo& createInfo,
    const VkImageSubresource&  subresource) {
    // Linear tiling layout, tightly packed: mip levels of one array layer
    // are laid out consecutively, then the next layer follows (arrayPitch).
    VkSubresourceLayout result = { };

    const DxvkFormatInfo* formatInfo = lookupFormatInfo(createInfo.format);

    if (!formatInfo) {
      Logger::err(str::format("d9mt: queryImageSubresourceLayout: unknown format ",
        uint32_t(createInfo.format)));
      return result;
    }

    if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
      Logger::err("d9mt: queryImageSubresourceLayout: multi-planar formats not supported");
      return result;
    }

    VkDeviceSize layerSize = 0u;

    for (uint32_t i = 0; i < createInfo.mipLevels; i++) {
      VkExtent3D mipExtent  = util::computeMipLevelExtent(createInfo.extent, i);
      VkExtent3D blockCount = util::computeBlockCount(mipExtent, formatInfo->blockSize);

      VkDeviceSize rowPitch   = VkDeviceSize(blockCount.width) * formatInfo->elementSize;
      VkDeviceSize depthPitch = rowPitch * blockCount.height;
      VkDeviceSize mipSize    = depthPitch * blockCount.depth;

      if (i == subresource.mipLevel) {
        result.offset     = layerSize;
        result.size       = mipSize;
        result.rowPitch   = rowPitch;
        result.depthPitch = depthPitch;
      }

      layerSize += mipSize;
    }

    result.arrayPitch = layerSize;
    result.offset    += VkDeviceSize(subresource.arrayLayer) * layerSize;
    return result;
  }


  uint32_t DxvkDevice::getCurrentFrameId() const {
    return uint32_t(m_statCounters.getCtr(DxvkStatCounter::QueuePresentCount));
  }


  DxvkStatCounters DxvkDevice::getStatCounters() {
    DxvkPipelineCount pipe = m_objects.pipelineManager().getPipelineCount();

    DxvkStatCounters result;
    {
      std::lock_guard<sync::Spinlock> lock(m_statLock);
      result = m_statCounters;
    }

    result.setCtr(DxvkStatCounter::PipeCountGraphics, pipe.numGraphicsPipelines);
    result.setCtr(DxvkStatCounter::PipeCountLibrary,  pipe.numGraphicsLibraries);
    result.setCtr(DxvkStatCounter::PipeCountCompute,  pipe.numComputePipelines);
    result.setCtr(DxvkStatCounter::GpuIdleTicks,      m_submissionQueue.gpuIdleTicks());
    return result;
  }


  DxvkMemoryStats DxvkDevice::getMemoryStats(uint32_t heap) {
    return m_objects.memoryManager().getMemoryStats(heap);
  }


  DxvkSharedAllocationCacheStats DxvkDevice::getMemoryAllocationStats(
    DxvkMemoryAllocationStats& stats) {
    m_objects.memoryManager().getAllocationStats(stats);
    return m_objects.memoryManager().getAllocationCacheStats();
  }


  // ==========================================================================
  // DxvkSubmissionQueue — threadless variant. The Metal backend submits and
  // presents synchronously on the calling thread (completion is tracked by
  // the d9mt completion watcher), so the upstream submit/finish worker
  // threads are never spawned and the queues stay empty by construction.
  // ==========================================================================

  DxvkSubmissionQueue::DxvkSubmissionQueue(
          DxvkDevice*        device,
    const DxvkQueueCallback& callback)
  : m_device(device), m_callback(callback) {
    // no worker threads on purpose (METAL-BACKEND-NOTES.md)
  }


  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    m_stopped.store(true);
  }


  void DxvkSubmissionQueue::synchronize() {
    // nothing is ever queued asynchronously — submissions
    // happen synchronously on the calling thread
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  // ==========================================================================
  // DxvkDescriptorProperties — descriptor-buffer metadata. The Metal backend
  // uses 8-byte argument-buffer slots (u64 gpuResourceID / gpuAddress) for
  // every view/buffer descriptor type; the legacy Vulkan descriptor model is
  // never exercised, but the sizes keep dependent size math meaningful.
  // ==========================================================================

  DxvkDescriptorProperties::DxvkDescriptorProperties(DxvkDevice* device) {
    m_setAlignment = 16u;

    for (auto& info : m_descriptorTypes)
      info = { 8u, 8u };
  }


  DxvkDescriptorProperties::~DxvkDescriptorProperties() {

  }


  // ==========================================================================
  // DxvkMemoryAllocator — construction + statistics. The actual allocation
  // paths (createBufferResource / createImageResource / freeAllocation) land
  // with the Resources stage; this stage wires the memory-type/heap tables
  // (from the adapter caps) that those paths and the HUD statistics need.
  // ==========================================================================

  DxvkMemoryAllocator::DxvkMemoryAllocator(DxvkDevice* device)
  : m_device(device),
    m_sharingModeInfo(device->getSharingMode()) {
    const auto& memProps = device->adapter()->memoryProperties();

    m_memTypeCount = memProps.memoryTypeCount;
    m_memHeapCount = memProps.memoryHeapCount;

    for (uint32_t i = 0; i < m_memHeapCount; i++) {
      m_memHeaps[i].index        = i;
      m_memHeaps[i].properties   = memProps.memoryHeaps[i];
      m_memHeaps[i].memoryBudget = memProps.memoryHeaps[i].size;
    }

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      m_memTypes[i].index      = i;
      m_memTypes[i].properties = memProps.memoryTypes[i];
      m_memTypes[i].heap       = &m_memHeaps[memProps.memoryTypes[i].heapIndex];
      m_memHeaps[memProps.memoryTypes[i].heapIndex].memoryTypes |= 1u << i;
    }
  }


  DxvkMemoryAllocator::~DxvkMemoryAllocator() {
    // chunk teardown lands with the Resources stage; nothing
    // is allocated through this object until then
  }


  uint32_t DxvkMemoryAllocator::getMemoryTypeMask(
          VkMemoryPropertyFlags properties) const {
    uint32_t mask = 0u;

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if ((m_memTypes[i].properties.propertyFlags & properties) == properties)
        mask |= 1u << i;
    }

    return mask;
  }


  DxvkLocalAllocationCache DxvkMemoryAllocator::createAllocationCache(
          VkBufferUsageFlags    bufferUsage,
          VkMemoryPropertyFlags properties) {
    return DxvkLocalAllocationCache(this, getMemoryTypeMask(properties));
  }


  DxvkMemoryStats DxvkMemoryAllocator::getMemoryStats(uint32_t heap) const {
    DxvkMemoryStats result = { };

    if (heap >= m_memHeapCount)
      return result;

    result.memoryBudget = m_memHeaps[heap].memoryBudget;

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if (m_memTypes[i].heap == &m_memHeaps[heap]) {
        result.memoryAllocated += m_memTypes[i].stats.memoryAllocated;
        result.memoryUsed      += m_memTypes[i].stats.memoryUsed;
      }
    }

    return result;
  }


  void DxvkMemoryAllocator::getAllocationStats(DxvkMemoryAllocationStats& stats) {
    stats.memoryTypes = { };
    stats.chunks.clear();
    stats.pageMasks.clear();

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      auto& typeStats = stats.memoryTypes[i];
      typeStats.properties = m_memTypes[i].properties;
      typeStats.allocated  = m_memTypes[i].stats.memoryAllocated;
      typeStats.used       = m_memTypes[i].stats.memoryUsed;
    }
  }


  DxvkSharedAllocationCacheStats DxvkMemoryAllocator::getAllocationCacheStats() const {
    // no shared caches until the Resources stage
    return DxvkSharedAllocationCacheStats();
  }


  void DxvkLocalAllocationCache::freeCache() {
    if (!m_allocator)
      return;

    // The cache can only hold allocations once the Resources-stage allocator
    // exists; replace this with freeCachedAllocations then. Stay loud if the
    // invariant ever breaks.
    for (auto& pool : m_pools) {
      if (pool) {
        Logger::err("d9mt: DxvkLocalAllocationCache::freeCache: leaking cached "
          "allocations (allocator free path not implemented yet)");
        pool = nullptr;
      }
    }
  }


  // CPU-side suballocator helpers: construction only for now; the allocation
  // algorithms land with the Resources stage (upstream dxvk_allocator.cpp is
  // not vendored — every method is ours).

  DxvkPageAllocator::DxvkPageAllocator() {

  }


  DxvkPageAllocator::~DxvkPageAllocator() {

  }


  DxvkPoolAllocator::DxvkPoolAllocator(DxvkPageAllocator& pageAllocator)
  : m_pageAllocator(&pageAllocator) {

  }


  DxvkPoolAllocator::~DxvkPoolAllocator() {

  }


  DxvkResourceAllocationPool::DxvkResourceAllocationPool() {

  }


  DxvkResourceAllocationPool::~DxvkResourceAllocationPool() {

  }


  DxvkRelocationList::DxvkRelocationList() {

  }


  DxvkRelocationList::~DxvkRelocationList() {

  }


  // ==========================================================================
  // Sampler pool. Real LRU-recycling pool (upstream semantics): samplers are
  // deduped by key, kept alive while referenced, and parked in an LRU list at
  // refcount zero for reuse. The Metal MTLSamplerState is created directly in
  // the DxvkSampler constructor; its gpuResourceID is published to the d9mt
  // sampler-heap shadow array under the pool mutex.
  // ==========================================================================

  namespace {

    WMTSamplerAddressMode d9mtSamplerAddressMode(uint32_t vkMode) {
      switch (VkSamplerAddressMode(vkMode)) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT:               return WMTSamplerAddressModeRepeat;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:      return WMTSamplerAddressModeMirrorRepeat;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:        return WMTSamplerAddressModeClampToEdge;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:      return WMTSamplerAddressModeClampToBorderColor;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return WMTSamplerAddressModeMirrorClampToEdge;
        default:
          Logger::err(str::format("d9mt: unhandled sampler address mode ", vkMode));
          return WMTSamplerAddressModeClampToEdge;
      }
    }

  }


  DxvkSampler::DxvkSampler(
          DxvkSamplerPool* pool,
    const DxvkSamplerKey&  key,
          uint16_t         index)
  : m_pool(pool), m_key(key) {
    WMTSamplerInfo info = { };

    info.min_filter = key.u.p.minFilter
      ? WMTSamplerMinMagFilterLinear
      : WMTSamplerMinMagFilterNearest;
    info.mag_filter = key.u.p.magFilter
      ? WMTSamplerMinMagFilterLinear
      : WMTSamplerMinMagFilterNearest;
    info.mip_filter = key.u.p.mipMode
      ? WMTSamplerMipFilterLinear
      : WMTSamplerMipFilterNearest;

    info.s_address_mode = d9mtSamplerAddressMode(key.u.p.addressU);
    info.t_address_mode = d9mtSamplerAddressMode(key.u.p.addressV);
    info.r_address_mode = d9mtSamplerAddressMode(key.u.p.addressW);

    // Metal supports only the fixed border palette — snap (BACKEND-SURFACE §3.8)
    switch (determineBorderColorType()) {
      case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
        info.border_color = WMTSamplerBorderColorOpaqueWhite;
        break;
      case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
        info.border_color = WMTSamplerBorderColorOpaqueBlack;
        break;
      default:
        info.border_color = WMTSamplerBorderColorTransparentBlack;
        break;
    }

    info.compare_function = key.u.p.compareEnable
      ? WMTCompareFunction(key.u.p.compareOp)
      : WMTCompareFunctionNever;

    info.lod_min_clamp = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.minLod);
    info.lod_max_clamp = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.maxLod);
    // NB Metal samplers have no LOD bias (key.u.p.lodBias is dropped) —
    // known visual-deviation class, recorded in METAL-BACKEND-NOTES.md.

    info.max_anisotroy = std::max(1u, uint32_t(key.u.p.anisotropy));
    info.normalized_coords = !key.u.p.pixelCoord;
    info.support_argument_buffers = true;

    obj_handle_t handle = MTLDevice_newSamplerState(d9mt::mtlDevice(), &info);

    if (!handle) {
      Logger::err("d9mt: MTLDevice_newSamplerState failed");
      d9mt::logf("DxvkSampler: newSamplerState FAILED (key %08x %08x)",
        key.u.properties[0], key.u.properties[1]);
    }

    m_descriptor.samplerObject = VkSampler(handle);
    m_descriptor.samplerIndex  = index;

    if (index < d9mt::SamplerHeapSize)
      d9mt::samplerHeapData()[index] = info.gpu_resource_id;
  }


  DxvkSampler::~DxvkSampler() {
    if (m_descriptor.samplerIndex < d9mt::SamplerHeapSize)
      d9mt::samplerHeapData()[m_descriptor.samplerIndex] = 0u;

    if (m_descriptor.samplerObject)
      NSObject_release(obj_handle_t(m_descriptor.samplerObject));
  }


  void DxvkSampler::release() {
    m_pool->releaseSampler(int32_t(m_descriptor.samplerIndex));
  }


  VkBorderColor DxvkSampler::determineBorderColorType() const {
    if (!m_key.u.p.hasBorder)
      return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    // pick the closest of Metal's three fixed border colors
    static const std::array<std::pair<VkBorderColor, std::array<float, 4>>, 3> s_colors = {{
      { VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, { 0.0f, 0.0f, 0.0f, 0.0f } },
      { VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,      { 0.0f, 0.0f, 0.0f, 1.0f } },
      { VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,      { 1.0f, 1.0f, 1.0f, 1.0f } },
    }};

    VkBorderColor result = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    float bestDist = -1.0f;

    for (const auto& candidate : s_colors) {
      float dist = 0.0f;

      for (uint32_t i = 0; i < 4; i++) {
        float d = m_key.borderColor.float32[i] - candidate.second[i];
        dist += d * d;
      }

      if (bestDist < 0.0f || dist < bestDist) {
        bestDist = dist;
        result = candidate.first;
      }
    }

    return result;
  }


  DxvkSamplerDescriptorHeap::DxvkSamplerDescriptorHeap(
          DxvkDevice* device,
          uint32_t    size)
  : m_device(device), m_descriptorCount(size) {
    // No Vulkan descriptor pool/set on the Metal backend: the sampler heap
    // is the d9mt::samplerHeapData() u64 gpuResourceID array, uploaded as
    // [[buffer(2)]] by the Draw stage.
  }


  DxvkSamplerDescriptorHeap::~DxvkSamplerDescriptorHeap() {

  }


  DxvkSamplerDescriptorSet DxvkSamplerDescriptorHeap::getDescriptorSetInfo() const {
    // null handles: consumed only by the fake Vulkan dispatch
    // (vkCreatePipelineLayout) which accepts them
    return DxvkSamplerDescriptorSet();
  }


  DxvkSamplerPool::DxvkSamplerPool(DxvkDevice* device)
  : m_device(device),
    m_descriptorHeap(device, MaxSamplerCount) {
    // Default sampler: keeps index 0 occupied with sane state
    DxvkSamplerKey key = { };
    key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    key.setAddressModes(
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setLodRange(0.0f, 15.0f, 0.0f);
    key.setAniso(0u);
    key.setDepthCompare(false, VK_COMPARE_OP_ALWAYS);

    m_default = createSampler(key);
  }


  DxvkSamplerPool::~DxvkSamplerPool() {
    // m_default is declared after the arrays and thus destroyed first;
    // remaining samplers die with m_samplers. Anything still referenced
    // at this point is a front-end bug — count and report it.
    m_default = nullptr;

    uint32_t live = m_samplersLive.load();
    if (live)
      Logger::warn(str::format("d9mt: DxvkSamplerPool: ", live, " samplers still in use at teardown"));
  }


  Rc<DxvkSampler> DxvkSamplerPool::createSampler(const DxvkSamplerKey& key) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    auto found = m_samplerLut.find(key);

    if (found != m_samplerLut.end()) {
      int32_t index = found->second;
      auto& entry = m_samplers[index];

      // resurrect from the LRU list if the sampler was unused
      if (samplerIsInLruList(entry, index)) {
        removeLru(entry, index);
        m_samplersLive.fetch_add(1u);
      }

      // take the reference inside the lock; releaseSampler re-checks the
      // ref count under the same lock, so resurrection is race-free
      return Rc<DxvkSampler>(&*entry.object);
    }

    int32_t index;

    if (m_samplerLut.size() < MaxSamplerCount) {
      // slots are allocated densely, so the LUT size is the next free index
      index = int32_t(m_samplerLut.size());
    } else if (m_lruHead >= 0) {
      // recycle the least-recently-used unreferenced sampler
      index = m_lruHead;
      auto& entry = m_samplers[index];
      removeLru(entry, index);
      m_samplerLut.erase(entry.object->key());
      entry.object.reset();
    } else {
      // every sampler is referenced: the front-end handles this by
      // spinning on getStats().liveCount — fail loud but graceful
      Logger::err("d9mt: DxvkSamplerPool: pool exhausted, no unused sampler to recycle");
      return nullptr;
    }

    auto& entry = m_samplers[index];
    entry.object.emplace(this, key, uint16_t(index));
    m_samplerLut.emplace(key, index);
    m_samplersLive.fetch_add(1u);

    return Rc<DxvkSampler>(&*entry.object);
  }


  void DxvkSamplerPool::releaseSampler(int32_t index) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    auto& entry = m_samplers[index];

    // raced with createSampler resurrecting the object — keep it live
    if (entry.object->m_refCount.load())
      return;

    if (!samplerIsInLruList(entry, index)) {
      appendLru(entry, index);
      m_samplersLive.fetch_sub(1u);
    }
  }


  void DxvkSamplerPool::appendLru(SamplerEntry& sampler, int32_t index) {
    sampler.lruPrev = m_lruTail;
    sampler.lruNext = -1;

    if (m_lruTail >= 0)
      m_samplers[m_lruTail].lruNext = index;
    else
      m_lruHead = index;

    m_lruTail = index;
  }


  void DxvkSamplerPool::removeLru(SamplerEntry& sampler, int32_t index) {
    if (sampler.lruPrev >= 0)
      m_samplers[sampler.lruPrev].lruNext = sampler.lruNext;
    else
      m_lruHead = sampler.lruNext;

    if (sampler.lruNext >= 0)
      m_samplers[sampler.lruNext].lruPrev = sampler.lruPrev;
    else
      m_lruTail = sampler.lruPrev;

    sampler.lruPrev = -1;
    sampler.lruNext = -1;
  }


  bool DxvkSamplerPool::samplerIsInLruList(SamplerEntry& sampler, int32_t index) const {
    return sampler.lruPrev >= 0
        || sampler.lruNext >= 0
        || m_lruHead == index;
  }


  // ==========================================================================
  // Pipeline manager. On the Metal backend this owns only the CPU-side
  // descriptor/pipeline layout caches (vendored dxvk_pipelayout.cpp objects
  // over the fake Vulkan dispatch). No compiler worker threads are spawned —
  // Metal PSOs are compiled at draw time by the Draw stage.
  // ==========================================================================

  DxvkPipelineWorkers::DxvkPipelineWorkers(DxvkDevice* device)
  : m_device(device) {
    // never spawns workers (METAL-BACKEND-NOTES.md: threadless policy;
    // registerShader/requestCompileShader are no-ops on this backend)
  }


  DxvkPipelineWorkers::~DxvkPipelineWorkers() {

  }


  DxvkPipelineManager::DxvkPipelineManager(DxvkDevice* device)
  : m_device(device), m_workers(device) {

  }


  DxvkPipelineManager::~DxvkPipelineManager() {

  }


  const DxvkDescriptorSetLayout* DxvkPipelineManager::createDescriptorSetLayout(
    const DxvkDescriptorSetLayoutKey& key) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto pair = m_descriptorSetLayouts.find(key);
    if (pair != m_descriptorSetLayouts.end())
      return &pair->second;

    auto iter = m_descriptorSetLayouts.emplace(
      std::piecewise_construct,
      std::tuple(key),
      std::tuple(m_device, key));

    return &iter.first->second;
  }


  const DxvkPipelineLayout* DxvkPipelineManager::createPipelineLayout(
    const DxvkPipelineLayoutKey& key) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto pair = m_pipelineLayouts.find(key);
    if (pair != m_pipelineLayouts.end())
      return &pair->second;

    auto iter = m_pipelineLayouts.emplace(
      std::piecewise_construct,
      std::tuple(key),
      std::tuple(m_device, key));

    return &iter.first->second;
  }


  DxvkPipelineCount DxvkPipelineManager::getPipelineCount() const {
    DxvkPipelineCount result = { };
    result.numGraphicsPipelines = m_stats.numGraphicsPipelines.load();
    result.numGraphicsLibraries = m_stats.numGraphicsLibraries.load();
    result.numComputePipelines  = m_stats.numComputePipelines.load();
    return result;
  }


  // ==========================================================================
  // GPU event pool. DxvkGpuEvent is a recyclable pool token on this backend:
  // event completion is tracked through the completion watcher (Queries
  // stage), not VkEvent objects, so the handle stays null.
  // ==========================================================================

  DxvkGpuEvent::DxvkGpuEvent(DxvkGpuEventPool* parent)
  : m_pool(parent) {

  }


  DxvkGpuEvent::~DxvkGpuEvent() {

  }


  void DxvkGpuEvent::free() {
    m_pool->freeEvent(this);
  }


  DxvkGpuEventPool::DxvkGpuEventPool(const DxvkDevice* device)
  : m_vkd(device->vkd()) {

  }


  DxvkGpuEventPool::~DxvkGpuEventPool() {
    for (auto* event : m_freeEvents)
      delete event;
  }


  Rc<DxvkGpuEvent> DxvkGpuEventPool::allocEvent() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_freeEvents.empty())
      return new DxvkGpuEvent(this);

    DxvkGpuEvent* event = m_freeEvents.back();
    m_freeEvents.pop_back();
    return event;
  }


  void DxvkGpuEventPool::freeEvent(DxvkGpuEvent* event) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_freeEvents.push_back(event);
  }


  DxvkEvent::DxvkEvent(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  DxvkEvent::~DxvkEvent() {

  }


  void DxvkEvent::assignGpuEvent(Rc<DxvkGpuEvent> event) {
    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_gpuEvent = std::move(event);
    m_status   = VK_NOT_READY;
  }


  // ==========================================================================
  // GPU query pool. Queries are pool tokens identified by (fake pool handle,
  // index); result storage and resolution land with the Queries stage. The
  // alloc/recycle machinery here is the real, final implementation.
  // ==========================================================================

  void DxvkGpuQuery::free() {
    m_allocator->freeQuery(this);
  }


  DxvkGpuQueryAllocator::DxvkGpuQueryAllocator(
          DxvkDevice* device,
          VkQueryType queryType,
          uint32_t    queryPoolSize)
  : m_device(device), m_queryType(queryType), m_queryPoolSize(queryPoolSize) {

  }


  DxvkGpuQueryAllocator::~DxvkGpuQueryAllocator() {
    for (auto& pool : m_pools)
      delete[] pool.queries;
  }


  Rc<DxvkGpuQuery> DxvkGpuQueryAllocator::allocQuery() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!m_free)
      createQueryPool();

    if (!m_free)
      return nullptr;

    DxvkGpuQuery* query = m_free;
    m_free = query->m_next;
    query->m_next = nullptr;
    return query;
  }


  void DxvkGpuQueryAllocator::freeQuery(DxvkGpuQuery* query) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    query->m_next = m_free;
    m_free = query;
  }


  void DxvkGpuQueryAllocator::createQueryPool() {
    // No VkQueryPool on Metal; the pool handle is a unique non-zero cookie
    // so that getQuery() pairs remain distinguishable.
    Pool& pool = m_pools.emplace_back();
    pool.pool    = ++d9mt::g_fakeCookie;
    pool.queries = new DxvkGpuQuery[m_queryPoolSize];

    for (uint32_t i = 0; i < m_queryPoolSize; i++) {
      pool.queries[i].m_allocator = this;
      pool.queries[i].m_pool      = pool.pool;
      pool.queries[i].m_index     = i;
      pool.queries[i].m_next      = (i + 1u < m_queryPoolSize)
        ? &pool.queries[i + 1u]
        : m_free;
    }

    m_free = &pool.queries[0];
  }


  DxvkGpuQueryPool::DxvkGpuQueryPool(DxvkDevice* device)
  : m_occlusion(device, VK_QUERY_TYPE_OCCLUSION, 16384),
    m_statistic(device, VK_QUERY_TYPE_PIPELINE_STATISTICS, 1024),
    m_timestamp(device, VK_QUERY_TYPE_TIMESTAMP, 1024),
    m_xfbStream(device, VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, 1024) {

  }


  DxvkGpuQueryPool::~DxvkGpuQueryPool() {

  }


  Rc<DxvkGpuQuery> DxvkGpuQueryPool::allocQuery(VkQueryType type) {
    switch (type) {
      case VK_QUERY_TYPE_OCCLUSION:
        return m_occlusion.allocQuery();
      case VK_QUERY_TYPE_PIPELINE_STATISTICS:
        return m_statistic.allocQuery();
      case VK_QUERY_TYPE_TIMESTAMP:
        return m_timestamp.allocQuery();
      case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
        return m_xfbStream.allocQuery();
      default:
        Logger::err(str::format("d9mt: DxvkGpuQueryPool: unsupported query type ",
          uint32_t(type)));
        return nullptr;
    }
  }


  DxvkQuery::DxvkQuery(
    const Rc<DxvkDevice>& device,
          VkQueryType     type,
          VkQueryControlFlags flags,
          uint32_t        index)
  : m_device(device), m_type(type), m_flags(flags), m_index(index) {

  }


  DxvkQuery::~DxvkQuery() {

  }


  void DxvkQuery::begin() {
    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_queries.clear();
    m_queryData = { };
    m_ended = false;
  }


  void DxvkQuery::end() {
    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_ended = true;
  }


  void DxvkQuery::addGpuQuery(Rc<DxvkGpuQuery> query) {
    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_queries.push_back(std::move(query));
  }


  // ==========================================================================
  // Unbound resources. nullDescriptor is advertised, so the dummy resources
  // are never needed at this stage; creation is lazy by design and the
  // accessors land together with the Resources stage.
  // ==========================================================================

  DxvkUnboundResources::DxvkUnboundResources(DxvkDevice* dev)
  : m_device(dev) {

  }


  DxvkUnboundResources::~DxvkUnboundResources() {

  }

}
