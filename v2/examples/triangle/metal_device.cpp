#include "metal_device.h"

namespace d9mt::metal {

  MetalDevice::MetalDevice() {
    // CreateSystemDefaultDevice returns a +1 reference we own (metal-cpp rule 1).
    m_device = MTL::CreateSystemDefaultDevice();
    if (m_device != nullptr)
      m_queue = m_device->newCommandQueue();
  }

  MetalDevice::~MetalDevice() {
    if (m_queue)  m_queue->release();
    if (m_device) m_device->release();
  }

}
