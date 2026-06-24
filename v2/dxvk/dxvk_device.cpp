#include "dxvk_device.h"

namespace dxvk {

  DxvkDevice::DxvkDevice(const Rc<DxvkAdapter>& adapter)
    : m_adapter(adapter), m_backend(std::make_unique<D9mtBackend>()) {
    // Features stay all-false and properties stay zeroed (their defaults); the
    // frontend only branches on a handful of bits, all of which we want off.
  }


  DxvkDevice::~DxvkDevice() {
  }


  bool DxvkDevice::initialize() {
    bool ok = m_backend->initialize();
    if (ok)
      g_activeBackend = m_backend.get();  // expose to the WSI surface stub for attachWindow
    return ok;
  }


  Rc<DxvkContext> DxvkDevice::createContext() {
    // DxvkContext's ctor takes the owning device (Rc<DxvkDevice>) and reads
    // backend() from it — matching the real DXVK signature the frontend records.
    return new DxvkContext(this);
  }


  Rc<DxvkCommandList> DxvkDevice::createCommandList() {
    // Command list is a passive record container in the synchronous shim.
    return new DxvkCommandList();
  }


  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    (void)memoryType; // all Metal shared buffers are CPU-writable + GPU-visible

    // Back the buffer with a shared Metal allocation. The CPU pointer lets the
    // resource module satisfy mapPtr()/allocateStorage() without a Vulkan map.
    void*    cpuPointer  = nullptr;
    uint64_t gpuAddress  = 0u;
    uint64_t metalHandle = m_backend->createSharedBuffer(createInfo.size, &cpuPointer, &gpuAddress);

    // Contract with the resource module (dxvk_buffer.h): wrap the Metal handle
    // + CPU pointer + GPU address + create info into a DxvkBuffer.
    return new DxvkBuffer(createInfo, metalHandle, cpuPointer, gpuAddress);
  }


  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    (void)memoryType;
    // Minimal image: the resource module decides whether it is a real Metal RT
    // (backbuffer) or an inert stub (app textures). We just forward the info.
    return new DxvkImage(createInfo, *m_backend);
  }


  void DxvkDevice::submitCommandList(
    const Rc<DxvkCommandList>&    commandList,
    const Rc<DxvkLatencyTracker>& tracker,
          uint64_t                frameId,
          DxvkSubmitStatus*       status) {
    // Synchronous shim: the context already encoded + executed everything when
    // the CS chunk was dispatched. Nothing to submit asynchronously.
    (void)commandList; (void)tracker; (void)frameId; (void)status;
  }


  void DxvkDevice::presentImage(
    const Rc<Presenter>&          presenter,
    const Rc<DxvkLatencyTracker>& tracker,
          uint64_t                frameId,
          DxvkSubmitStatus*       status) {
    // The per-frame present boundary: the backend now presents the frame that
    // DxvkContext::draw has been accumulating into the open encoder, ending the
    // encoder, presenting the drawable, and committing the command buffer.
    (void)presenter; (void)tracker; (void)frameId; (void)status;
    m_backend->endFrameAndPresent();
  }

}
