#pragma once

// Metal-backed shim for DXVK's buffer layer (replaces dxvk-ref/dxvk_buffer.h).
//
// The D3D9 frontend is UNMODIFIED, so every type/method here matches the real
// DXVK surface the frontend calls — only the bodies change: instead of a Vulkan
// VkBuffer + suballocated memory, a DxvkBuffer wraps ONE winemetal MTLBuffer
// handle plus its persistently-mapped CPU pointer (both produced by
// D9mtBackend::createSharedBuffer via DxvkDevice::createBuffer). This is the
// vertex/index/constant/UP/staging upload path for the one triangle.
//
// What the frontend actually touches (verified in ../d3d9):
//   DxvkBuffer:          mapPtr(off), mapPtr(), info(), storage(),
//                        allocateStorage(), setDebugName() (via createBuffer)
//   DxvkBufferSlice:     default ctor, ctor(Rc<DxvkBuffer>,off,len),
//                        ctor(Rc<DxvkBuffer>), mapPtr(off), buffer(), offset(),
//                        length(), defined()
//   DxvkResourceAllocation: mapPtr()  (UP-buffer / constant-buffer recycling)
//
// Everything that drove real Vulkan suballocation (views, descriptors, sparse,
// relocation, memory pools) is gone — the command module binds straight from a
// slice's Metal handle + offset.  See SHIM_SPEC.md (resource module).

#include <cstdint>
#include <string>
#include <utility>

#include "../util/util_gdi.h"  // D3DKMT_HANDLE (kmtLocal())

#include "dxvk_access.h"   // DxvkPagedResource base (GPU-tracking, no-op in shim)
#include "dxvk_include.h"
#include "dxvk_backend.h"
#include "dxvk_pipelayout.h"  // DxvkDescriptor (view getDescriptor(), dead path)

namespace dxvk {

  class DxvkDevice;
  class DxvkBuffer;
  class DxvkBufferView;
  // Forward-declared so DxvkBuffer::createView's by-reference parameter resolves
  // before the full definition (which follows the DxvkBuffer class below).
  struct DxvkBufferViewKey;

  /**
   * \brief Buffer create info
   *
   * The subset of fields the D3D9 frontend sets on the path to
   * DxvkDevice::createBuffer. Verified against d3d9_common_buffer.cpp,
   * d3d9_constant_buffer.cpp and d3d9_device.cpp (UP buffer).
   *
   * In the shim only \c size and \c debugName carry meaning; the Vulkan usage /
   * stage / access masks are recorded verbatim so the frontend compiles and so
   * we can keep them around for debugging, but Metal allocates one shared,
   * CPU-coherent buffer regardless of these flags.
   */
  struct DxvkBufferCreateInfo {
    /// Size of the buffer, in bytes
    VkDeviceSize size = 0u;
    /// Buffer usage flags (recorded, not acted on by the Metal shim)
    VkBufferUsageFlags usage = 0u;
    /// Pipeline stages that can access the buffer (recorded)
    VkPipelineStageFlags stages = 0u;
    /// Allowed access patterns (recorded)
    VkAccessFlags access = 0u;
    /// Buffer create flags (recorded)
    VkBufferCreateFlags flags = 0u;
    /// Debug name (used for Metal label / logging)
    const char* debugName = nullptr;
  };


  /**
   * \brief Backing storage for a buffer
   *
   * In real DXVK this is a reference-counted suballocation that a buffer can be
   * re-pointed at (the "rename"/discard mechanism). The frontend uses it only as
   * an opaque handle returned by \c DxvkBuffer::storage / \c allocateStorage and
   * then queries \c mapPtr() — see d3d9_constant_buffer.cpp::Alloc and the UP
   * buffer path in d3d9_device.cpp.
   *
   * In the shim a buffer never relocates (one MTLBuffer for its whole life), so
   * an allocation just remembers the buffer's CPU pointer. \c allocateStorage
   * therefore hands back the same backing region, which is correct because the
   * shim runs the CS lambdas synchronously: there is no in-flight GPU read of
   * the old contents to rename away from.
   */
  class DxvkResourceAllocation : public RcObject {

  public:

    DxvkResourceAllocation() = default;

    explicit DxvkResourceAllocation(void* mapPtr)
    : m_mapPtr(mapPtr) { }

    /// Pointer to the mapped, CPU-writable region this allocation backs.
    void* mapPtr() const {
      return m_mapPtr;
    }

    // KMT-local D3DKMT handle for the storage — the shim never exports kernel
    // handles, so this is always 0.
    D3DKMT_HANDLE kmtLocal() const { return 0; }

  private:

    void* m_mapPtr = nullptr;

  };


  /**
   * \brief Buffer resource (Metal-backed)
   *
   * Wraps a single winemetal MTLBuffer handle and its persistently-mapped CPU
   * pointer. Created exclusively through \ref DxvkDevice::createBuffer, which
   * routes to \ref D9mtBackend::createSharedBuffer.
   */
  class DxvkBuffer : public DxvkPagedResource {

  public:

    /**
     * \brief Creates a buffer from an already-allocated Metal buffer
     *
     * \param [in] createInfo Properties the frontend requested
     * \param [in] metalBuffer winemetal obj_handle_t for the MTLBuffer (0 if none)
     * \param [in] mapPtr CPU pointer to the coherent, mapped contents
     */
    DxvkBuffer(
      const DxvkBufferCreateInfo& createInfo,
            uint64_t              metalBuffer,
            void*                 mapPtr,
            uint64_t              gpuAddress = 0u)
    : m_info(createInfo),
      m_metalBuffer(metalBuffer),
      m_mapPtr(mapPtr),
      m_gpuAddress(gpuAddress),
      m_storage(new DxvkResourceAllocation(mapPtr)) {
      if (createInfo.debugName)
        m_debugName = createInfo.debugName;
    }

    /**
     * \brief Buffer properties
     * \returns Buffer create info
     */
    const DxvkBufferCreateInfo& info() const {
      return m_info;
    }

    /**
     * \brief Map pointer at a byte offset
     *
     * The Metal buffer is persistently mapped + coherent, so this is just
     * pointer arithmetic. Matches DxvkBuffer::mapPtr(VkDeviceSize) in dxvk-ref.
     * \param [in] offset Byte offset into the mapped region
     * \returns CPU pointer, or nullptr if the buffer has no mapping
     */
    void* mapPtr(VkDeviceSize offset) const {
      return m_mapPtr
        ? reinterpret_cast<char*>(m_mapPtr) + offset
        : nullptr;
    }

    /**
     * \brief Map pointer at offset zero
     * \returns CPU pointer to the start of the mapped region
     */
    void* mapPtr() const {
      return m_mapPtr;
    }

    /**
     * \brief Underlying Metal buffer handle
     *
     * Used by the command module to bind this buffer at draw time.
     * \returns winemetal obj_handle_t MTLBuffer (0 if none)
     */
    uint64_t metalBuffer() const {
      return m_metalBuffer;
    }

    /**
     * \brief Metal GPU address of the buffer's start
     *
     * Used to reference this buffer from an argument buffer (the FF constant
     * buffers live in argument-buffer descriptor sets). 0 if unknown.
     */
    uint64_t gpuAddress() const {
      return m_gpuAddress;
    }

    /**
     * \brief Retrieves current backing storage
     *
     * Frontend uses the returned allocation only to read \c mapPtr().
     * \returns Current buffer allocation
     */
    Rc<DxvkResourceAllocation> storage() const {
      return m_storage;
    }

    /**
     * \brief Allocates new backing storage
     *
     * Real DXVK renames the buffer onto fresh memory to avoid stalling on an
     * in-flight GPU read. The shim executes draws synchronously and never
     * relocates the buffer, so the existing region is returned unchanged.
     * \returns Backing resource (same mapped region)
     */
    Rc<DxvkResourceAllocation> allocateStorage() {
      return m_storage;
    }

    /**
     * \brief Creates or retrieves a buffer view
     *
     * Formatted texel views are off the triangle path; the shim returns a
     * lightweight stub view bound to this buffer. Defined out-of-line below
     * because it needs the complete \ref DxvkBufferView type.
     * \param [in] key Buffer view properties
     * \returns Buffer view
     */
    Rc<DxvkBufferView> createView(const DxvkBufferViewKey& key);

    /**
     * \brief Sets debug name for the backing resource
     * \param [in] name New debug name
     */
    void setDebugName(const char* name) {
      m_debugName = name ? name : "";
    }

    /**
     * \brief Retrieves debug name
     * \returns Debug name
     */
    const char* getDebugName() const {
      return m_debugName.c_str();
    }

  private:

    DxvkBufferCreateInfo       m_info        = { };
    uint64_t                   m_metalBuffer = 0u;
    void*                      m_mapPtr      = nullptr;
    uint64_t                   m_gpuAddress  = 0u;
    Rc<DxvkResourceAllocation> m_storage;
    std::string                m_debugName;

  };


  /**
   * \brief Buffer view key
   *
   * Properties of a formatted/texel buffer view. Mirrors dxvk-ref
   * (dxvk_memory.h) so the frontend's createView call compiles. Texel buffer
   * views are off the fixed-function triangle path, so the view itself is a
   * stub — it only remembers its parameters.
   */
  struct DxvkBufferViewKey {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkBufferUsageFlagBits usage = VkBufferUsageFlagBits(0u);
    VkDeviceSize offset = 0u;
    VkDeviceSize size = 0u;
  };


  /**
   * \brief Buffer view (stub)
   *
   * Real DXVK creates a VkBufferView for formatted texel access (D3D9 R2VB /
   * DF buffers). The Metal triangle path never samples a texel buffer, so this
   * just holds the parent buffer + key; \c buffer() lets callers fall back to
   * the underlying slice if needed.
   */
  class DxvkBufferView : public RcObject {

  public:

    DxvkBufferView(
            Rc<DxvkBuffer>     buffer,
      const DxvkBufferViewKey& key)
    : m_buffer(std::move(buffer)), m_key(key) { }

    const DxvkBufferViewKey& info() const { return m_key; }

    const Rc<DxvkBuffer>& buffer() const { return m_buffer; }

    // Opaque descriptor — only used by the dead format-conversion compute path.
    const DxvkDescriptor* getDescriptor(bool /*raw*/ = false) const {
      static DxvkDescriptor d; return &d;
    }

  private:

    Rc<DxvkBuffer>    m_buffer;
    DxvkBufferViewKey m_key = { };

  };


  /**
   * \brief Buffer slice
   *
   * A sub-range [offset, offset+length) of a \ref DxvkBuffer. Matches the
   * dxvk-ref DxvkBufferSlice surface the frontend relies on (default ctor,
   * (buffer,off,len) ctor, explicit (buffer) ctor, mapPtr(off), buffer(),
   * offset(), length(), defined()).
   *
   * The command module binds a slice by reading buffer()->metalBuffer() and
   * offset().
   */
  class DxvkBufferSlice {

  public:

    DxvkBufferSlice() { }

    DxvkBufferSlice(
            Rc<DxvkBuffer>  buffer,
            VkDeviceSize    rangeOffset,
            VkDeviceSize    rangeLength)
    : m_buffer(std::move(buffer)),
      m_offset(rangeOffset),
      m_length(rangeLength) { }

    explicit DxvkBufferSlice(Rc<DxvkBuffer> buffer)
    : DxvkBufferSlice(buffer, 0, buffer != nullptr ? buffer->info().size : 0) { }

    DxvkBufferSlice(const DxvkBufferSlice& ) = default;
    DxvkBufferSlice(      DxvkBufferSlice&&) = default;
    DxvkBufferSlice& operator = (const DxvkBufferSlice& ) = default;
    DxvkBufferSlice& operator = (      DxvkBufferSlice&&) = default;

    // NOTE: dxvk-ref's DxvkBufferSlice exposes mapPtr only as a METHOD
    // (mapPtr(offset)); there is no data member. The `slice.mapPtr` the
    // frontend reads belongs to D3D9BufferSlice (d3d9_device.h), a separate
    // struct. Adding a data member here would clash with the method below.

    /**
     * \brief Buffer slice offset and length
     */
    VkDeviceSize offset() const { return m_offset; }
    VkDeviceSize length() const { return m_length; }

    /**
     * \brief Underlying buffer
     * \returns The buffer this slice refers to
     */
    const Rc<DxvkBuffer>& buffer() const {
      return m_buffer;
    }

    /**
     * \brief Buffer info
     * \returns Properties of the underlying buffer
     */
    const DxvkBufferCreateInfo& bufferInfo() const {
      return m_buffer->info();
    }

    /**
     * \brief Underlying Metal buffer handle
     *
     * Convenience for the command module. 0 if the slice is undefined.
     * \returns winemetal obj_handle_t MTLBuffer
     */
    uint64_t metalBuffer() const {
      return m_buffer != nullptr ? m_buffer->metalBuffer() : 0u;
    }

    // Alias used by the command module (dxvk_context.cpp) — same value as
    // metalBuffer(); kept so both spellings compile (cross-module naming).
    uint64_t bufferHandle() const { return metalBuffer(); }

    /**
     * \brief GPU address of this slice's start (buffer base + slice offset)
     *
     * What an argument buffer slot stores to reference a uniform buffer. 0 if
     * the slice is undefined.
     */
    uint64_t gpuAddress() const {
      return m_buffer != nullptr ? m_buffer->gpuAddress() + m_offset : 0u;
    }

    /**
     * \brief Pointer to mapped memory at a sub-offset
     * \param [in] offset Offset into the slice
     * \returns CPU pointer, or nullptr if undefined
     */
    void* mapPtr(VkDeviceSize offset) const {
      return m_buffer != nullptr
        ? m_buffer->mapPtr(m_offset + offset)
        : nullptr;
    }

    /**
     * \brief Sub-slice of this slice
     * \param [in] offset Offset relative to this slice
     * \param [in] length Length of the sub-slice
     */
    DxvkBufferSlice subSlice(VkDeviceSize offset, VkDeviceSize length) const {
      return DxvkBufferSlice(m_buffer, m_offset + offset, length);
    }

    /**
     * \brief Whether the slice points at a buffer
     * \returns \c true if defined
     */
    bool defined() const {
      return m_buffer != nullptr;
    }

    /**
     * \brief Sets buffer range
     * \param [in] offset New offset
     * \param [in] length New length
     */
    void setRange(VkDeviceSize offset, VkDeviceSize length) {
      m_offset = offset;
      m_length = length;
    }

    /**
     * \brief Whether two slices reference the same buffer + range
     */
    bool matches(const DxvkBufferSlice& other) const {
      return m_buffer == other.m_buffer
          && m_offset == other.m_offset
          && m_length == other.m_length;
    }

  private:

    Rc<DxvkBuffer> m_buffer = nullptr;
    VkDeviceSize   m_offset = 0;
    VkDeviceSize   m_length = 0;

  };


  // Out-of-line: DxvkBufferView is a complete type only here.
  inline Rc<DxvkBufferView> DxvkBuffer::createView(const DxvkBufferViewKey& key) {
    return new DxvkBufferView(this, key);
  }

}
