#pragma once

// DxvkDevice shim — owns the single Metal backend and is the resource/context
// factory the D3D9 frontend drives.
//
// Frontend usage (d3d9_device.cpp ctor + hot paths):
//   - createContext()            -> Rc<DxvkContext>   (records into the backend)
//   - createCommandList()        -> Rc<DxvkCommandList>
//   - createBuffer(info, memType)-> Rc<DxvkBuffer>    (Metal shared buffer)
//   - createImage(info, memType) -> Rc<DxvkImage>     (minimal Metal RT/stub)
//   - features()                 -> const DxvkDeviceFeatures& (all-false)
//   - properties()               -> const DxvkDeviceInfo&     (canned)
//   - config()                   -> const DxvkOptions&
//   - debugFlags()               -> DxvkDebugFlags (0)
//   - perfHints()                -> DxvkDevicePerfHints (all 0)
//   - adapter()/kmtLocal()
//   - waitForIdle()              -> no-op
//   - submitCommandList(...)/presentImage(...) -> drive backend present
//
// The backend (D9mtBackend) is the only thing behind all of this. See SHIM_SPEC.

#include "dxvk_access.h"      // DxvkAccess / DxvkPagedResource
#include "dxvk_adapter.h"
#include "dxvk_backend.h"
#include "dxvk_buffer.h"      // resource module: DxvkBuffer + DxvkBufferCreateInfo
#include "dxvk_context.h"     // command module:  DxvkContext, DxvkCommandList
#include "dxvk_device_info.h"
#include "dxvk_gpu_query.h"   // DxvkQuery / DxvkEvent stand-ins
#include "dxvk_image.h"       // resource module: DxvkImage + DxvkImageCreateInfo
#include "dxvk_instance.h"
#include "dxvk_options.h"
#include "dxvk_sampler.h"     // DxvkSamplerKey / DxvkSampler / DxvkSamplerStats
#include "dxvk_shader_ir.h"   // DxvkIrShaderCreateInfo / DxvkIrShaderConverter

#include <memory>

namespace dxvk {

  class Presenter;
  struct DxvkSubmitStatus;
  class DxvkLatencyTracker;

  // Device performance hints. The shim wants every hint OFF, so the struct is
  // value-initialised to all-zero. Kept bit-for-bit compatible with the real
  // struct's field set (the frontend reads individual bits).
  struct DxvkDevicePerfHints {
    VkBool32 preferFbDepthStencilCopy    : 1;
    VkBool32 renderPassClearFormatBug    : 1;
    VkBool32 renderPassResolveFormatBug  : 1;
    VkBool32 preferRenderPassOps         : 1;
    VkBool32 preferPrimaryCmdBufs        : 1;
    VkBool32 preferComputeMipGen         : 1;
    VkBool32 preferDescriptorByteOffsets : 1;
    VkBool32 preferCachedMemory          : 1;
  };


  // GPU queue handle set (dxvk-ref/dxvk_device.h). All null in the shim; only
  // the interop device reads .graphics to report "no Vulkan queue".
  struct DxvkDeviceQueue {
    VkQueue  queueHandle = VK_NULL_HANDLE;
    uint32_t queueFamily = 0u;
    uint32_t queueIndex  = 0u;
  };

  struct DxvkDeviceQueueSet {
    DxvkDeviceQueue graphics;
    DxvkDeviceQueue transfer;
    DxvkDeviceQueue sparse;
  };


  class DxvkDevice : public RcObject {

  public:

    explicit DxvkDevice(const Rc<DxvkAdapter>& adapter);
    ~DxvkDevice();

    // Brings up the Metal backend (device/queue/FF pipeline). Returns false on
    // failure so createDevice() can refuse to hand back a broken device.
    bool initialize();

    // Exposes the backend to the context/swapchain shims. Returns a raw pointer
    // (DxvkContext stores D9mtBackend*); the device retains ownership.
    D9mtBackend* backend() { return m_backend.get(); }

    Rc<DxvkAdapter>  adapter()  const { return m_adapter; }
    D3DKMT_HANDLE    kmtLocal() const { return 0; }

    DxvkDebugFlags debugFlags() const { return DxvkDebugFlags(0u); }

    const DxvkOptions&        config()     const { return m_options; }
    const DxvkDeviceFeatures& features()   const { return m_features; }
    const DxvkDeviceInfo&     properties() const { return m_properties; }
    DxvkDevicePerfHints       perfHints()  const { return m_perfHints; }

    // --- factories ---------------------------------------------------------

    // The command module owns DxvkContext; we just hand it the backend.
    Rc<DxvkContext> createContext();

    // Command list is a thin record holder in the synchronous shim.
    Rc<DxvkCommandList> createCommandList();

    // Allocates a CPU-writable, GPU-visible Metal buffer via the backend and
    // wraps it in the resource module's DxvkBuffer.
    Rc<DxvkBuffer> createBuffer(
      const DxvkBufferCreateInfo& createInfo,
            VkMemoryPropertyFlags memoryType);

    // Minimal Metal render-target image (stub for app textures; real for the
    // backbuffer path the present module drives).
    Rc<DxvkImage> createImage(
      const DxvkImageCreateInfo& createInfo,
            VkMemoryPropertyFlags memoryType);

    // --- submission / present ---------------------------------------------

    // Synchronous shim: the work is already encoded by the context, so submit
    // just marks the status complete.
    void submitCommandList(
      const Rc<DxvkCommandList>&    commandList,
      const Rc<DxvkLatencyTracker>& tracker,
            uint64_t                frameId,
            DxvkSubmitStatus*       status);

    // Drives the backend's present (drawable acquire + present + wait).
    void presentImage(
      const Rc<Presenter>&          presenter,
      const Rc<DxvkLatencyTracker>& tracker,
            uint64_t                frameId,
            DxvkSubmitStatus*       status);

    // No async GPU work to drain — no-op.
    void waitForIdle() { }

    // --- shader factory / pipeline-stage queries --------------------------
    // The shim renders a fixed pipeline, so shader creation is opaque bookkeeping
    // (see dxvk_shader_ir.h). createCachedShader builds a DxvkIrShader from the
    // converter (or returns null when there is no cache entry, prompting the
    // frontend to build the converter itself); registerShader is a no-op.
    Rc<DxvkShader> createCachedShader(
            std::string                 name,
      const DxvkIrShaderCreateInfo&     info,
            Rc<DxvkIrShaderConverter>   converter) {
      if (converter == nullptr)
        return nullptr;
      return new DxvkIrShader(info, std::move(converter));
    }

    void registerShader(const Rc<DxvkShader>&) { }

    // Async shader-compile request. The shim has nothing to compile (the FF
    // triangle uses D9mtBackend's prebuilt MSL pipeline), so this is a no-op;
    // the shader's needsCompile flag stays set but is never acted on.
    void requestCompileShader(const Rc<DxvkShader>&) { }

    // All graphics + compute stages "enabled" — the frontend only uses this to
    // size barriers/markers, which the shim ignores.
    VkPipelineStageFlags getShaderPipelineStages() const {
      return VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }

    DxvkShaderOptions getShaderCompileOptions() const { return DxvkShaderOptions(); }

    // --- samplers ----------------------------------------------------------
    Rc<DxvkSampler> createSampler(const DxvkSamplerKey& key) { return new DxvkSampler(key); }
    DxvkSamplerStats getSamplerStats() const { return DxvkSamplerStats(); }

    // --- format limits (forwarded to the adapter's canned table) ----------
    std::optional<DxvkFormatLimits> getFormatLimits(const DxvkFormatQuery& query) const {
      return m_adapter->getFormatLimits(query);
    }

    // --- synchronization (synchronous shim: nothing is ever in flight) -----
    // Templated so the exact frontend resource/fence types bind without the
    // shim pulling them in; all are immediate no-ops.
    template<typename Fence>
    void waitForFence(Fence&, uint64_t) { }

    template<typename Resource>
    void waitForResource(const Resource&, DxvkAccess) { }

    void waitForSubmission(DxvkSubmitStatus*) { }

    // --- GPU queries / events (opaque; the shim has no GPU timeline) --------
    // Return ready-made stand-ins so D3D9Query completes immediately. Args
    // (query type / control flags / index) are ignored.
    Rc<DxvkQuery> createGpuQuery(VkQueryType, VkQueryControlFlags, uint32_t) {
      return new DxvkQuery();
    }

    Rc<DxvkEvent> createGpuEvent() { return new DxvkEvent(); }

    // Built-in compute pipeline (format-conversion path; never dispatched by the
    // shim). Returns VK_NULL_HANDLE — the helper stores it but never binds a real
    // pipeline. Templated on the stage type to avoid pulling dxvk_util.h here.
    template<typename Stage>
    VkPipeline createBuiltInComputePipeline(const DxvkPipelineLayout*, const Stage&) {
      return VK_NULL_HANDLE;
    }

    // Built-in pipeline layout (format-conversion path). Returns a singleton
    // empty layout; never used to bind anything in the shim.
    const DxvkPipelineLayout* createBuiltInPipelineLayout(
            uint32_t, VkShaderStageFlags, uint32_t,
            size_t, const DxvkDescriptorSetLayoutBinding*) {
      static DxvkPipelineLayout layout;
      return &layout;
    }

    // --- Vulkan-interop accessors (ID3D9VkInteropDevice) -------------------
    // The shim exposes no real Vulkan objects; these return null handles so the
    // interop interface compiles and reports "no Vulkan" cleanly.
    Rc<DxvkInstance> instance() const { return m_adapter->instance(); }
    VkDevice handle() const { return VK_NULL_HANDLE; }
    DxvkDeviceQueueSet queues() const { return DxvkDeviceQueueSet(); }
    Rc<vk::DeviceFn> vkd() const { return m_vkd; }
    void lockSubmission() { }
    void unlockSubmission() { }

    // Frame-latency tracker (shim no-op). The swapchain holds it and calls
    // notify/sleep methods that all do nothing.
    template<typename PresenterT>
    Rc<DxvkLatencyTracker> createLatencyTracker(const PresenterT&) {
      return new DxvkLatencyTracker();
    }

  private:

    Rc<DxvkAdapter>              m_adapter;
    std::unique_ptr<D9mtBackend> m_backend;

    Rc<vk::DeviceFn>             m_vkd         = new vk::DeviceFn();  // empty table
    DxvkOptions                  m_options;
    DxvkDeviceFeatures           m_features    = { };  // all VK_FALSE
    DxvkDeviceInfo               m_properties  = { };  // canned/zero
    DxvkDevicePerfHints          m_perfHints   = { };  // all 0

  };

}
