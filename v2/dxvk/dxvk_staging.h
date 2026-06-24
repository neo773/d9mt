#pragma once

// Metal-backed shim for DXVK's staging allocator (replaces
// dxvk-ref/dxvk_staging.h).
//
// A DxvkStagingBuffer is a linear bump allocator over one shared, CPU-coherent
// Metal buffer. The D3D9 frontend uses it for texture/buffer uploads:
//   - ctor(Rc<DxvkDevice>, size)               (D3D9DeviceEx member init)
//   - DxvkBufferSlice alloc(size)              (AllocStagingBuffer, then reads
//                                                slice.mapPtr(0))
//   - getStatistics() -> DxvkStagingBufferStats (ThrottleAllocation)
//   - reset()
//
// In the shim, uploads to GPU-private textures are stubs, but the slice's
// mapPtr must be valid so the frontend can write its source data without
// crashing. We therefore back the allocator with a real createBuffer call and
// bump within it, growing by allocating a fresh buffer when exhausted.

#include "dxvk_include.h"
#include "dxvk_buffer.h"
#include "dxvk_device.h"

namespace dxvk {

  /**
   * \brief Staging buffer statistics
   *
   * Read by D3D9DeviceEx::ThrottleAllocation to throttle uploads.
   */
  struct DxvkStagingBufferStats {
    /// Total amount allocated since the buffer was created
    VkDeviceSize allocatedTotal = 0u;
    /// Amount allocated since the last reset
    VkDeviceSize allocatedSinceLastReset = 0u;
  };


  /**
   * \brief Linear staging buffer allocator
   */
  class DxvkStagingBuffer {

  public:

    /**
     * \brief Creates staging buffer
     * \param [in] device DXVK device (used to allocate backing buffers)
     * \param [in] size Default backing buffer size
     */
    DxvkStagingBuffer(
      const Rc<DxvkDevice>& device,
            VkDeviceSize    size)
    : m_device(device), m_size(size) { }

    ~DxvkStagingBuffer() { }

    /**
     * \brief Allocates staging memory
     *
     * Suballocates from the current backing buffer, or allocates a new one
     * (sized for the request) when the current buffer is exhausted.
     * \param [in] size Number of bytes to allocate
     * \returns Slice into a CPU-mapped buffer
     */
    DxvkBufferSlice alloc(VkDeviceSize size) {
      VkDeviceSize alignedSize = align(size, SliceAlignment);

      m_allocationCounter += alignedSize;

      if (unlikely(!m_buffer || m_offset + alignedSize > m_buffer->info().size)) {
        // Grow: pick at least the default size so common small uploads share
        // one buffer, but never smaller than the current request.
        VkDeviceSize bufferSize = std::max(m_size, alignedSize);

        DxvkBufferCreateInfo info;
        info.size      = bufferSize;
        info.usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.stages    = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access    = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        info.debugName = "Staging buffer";

        m_buffer = m_device->createBuffer(info,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

        m_offset = 0u;
      }

      DxvkBufferSlice slice(m_buffer, m_offset, size);
      m_offset += alignedSize;
      return slice;
    }

    /**
     * \brief Resets the allocator
     */
    void reset() {
      m_buffer = nullptr;
      m_offset = 0u;
      m_allocationCounterValueOnReset = m_allocationCounter;
    }

    /**
     * \brief Retrieves allocation statistics
     * \returns Current allocation statistics
     */
    DxvkStagingBufferStats getStatistics() const {
      DxvkStagingBufferStats result = { };
      result.allocatedTotal = m_allocationCounter;
      result.allocatedSinceLastReset = m_allocationCounter - m_allocationCounterValueOnReset;
      return result;
    }

  private:

    // Match dxvk-ref's 256-byte slice granularity for upload coalescing.
    constexpr static VkDeviceSize SliceAlignment = 256u;

    Rc<DxvkDevice> m_device = nullptr;
    Rc<DxvkBuffer> m_buffer = nullptr;
    VkDeviceSize   m_offset = 0u;
    VkDeviceSize   m_size   = 0u;

    VkDeviceSize   m_allocationCounter = 0u;
    VkDeviceSize   m_allocationCounterValueOnReset = 0u;

  };

}
