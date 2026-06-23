// metal_device — owns the MTLDevice and its primary command queue.
//
// Root of the Metal backend. Everything that issues GPU work (pipeline cache,
// allocators, command executor) borrows the device and queue from here. No
// D3D9 or command-stream knowledge lives here — it knows Metal and nothing
// else (V2_ARCHITECTURE §6 boundaries). Pure C++ via metal-cpp; no Objective-C.

#pragma once

#include <Metal/Metal.hpp>

namespace d9mt::metal {

  // Owns the system-default GPU and one command queue. Single instance per
  // process: created at device-create time, destroyed at teardown. Follows
  // metal-cpp ownership rules — it retains what it holds and releases in dtor.
  class MetalDevice {
  public:
    MetalDevice();
    ~MetalDevice();

    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    bool isValid() const { return m_device != nullptr && m_queue != nullptr; }

    MTL::Device*       device() const { return m_device; }
    MTL::CommandQueue* queue()  const { return m_queue; }

  private:
    MTL::Device*       m_device = nullptr;
    MTL::CommandQueue* m_queue  = nullptr;
  };

}
