// d9mt: Metal backend — resources stage.
//
// DxvkMemoryAllocator allocation paths (buffer/image creation + import +
// free), DxvkResourceAllocation + per-allocation view maps, DxvkBuffer /
// DxvkBufferView, DxvkImage / DxvkImageView, DxvkPagedResource,
// DxvkResourceRef, DxvkStagingBuffer and the DxvkDevice resource factories.
//
// Structure mirrors upstream v2.7.1 (dxvk_buffer.cpp / dxvk_image.cpp /
// dxvk_memory.cpp / dxvk_sparse.cpp / dxvk_staging.cpp) with the Vulkan
// object layer replaced by winemetal:
//   VkBuffer    := MTLBuffer obj_handle_t (dedicated per allocation)
//   VkImage     := MTLTexture obj_handle_t
//   VkImageView := MTLTexture view obj_handle_t
//                  (descriptor->legacy.image.imageView)
//   VkBufferView:= MTLTexture (texture-buffer view) obj_handle_t
//                  (descriptor->legacy.bufferView)
//   descriptor->descriptor[0..7] = u64 argument-buffer word
//                  (gpu_resource_id for views, gpuAddress for raw buffers)
//
// See docs/METAL-BACKEND-NOTES.md "Stage decisions: resources".

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "d9mt_backend.h"
#include "d9mt_trace.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_buffer.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_image.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_staging.h"

namespace dxvk {

  namespace {

    // -----------------------------------------------------------------------
    // Texture side info. Vendored view-map classes have fixed (Vulkan-shaped)
    // members, so per-texture Metal metadata the view paths need (texture
    // type, sample count) lives in a process-global side table keyed by the
    // MTLTexture handle. Entries are inserted by createImageResource /
    // importImageResource and erased by the allocation destructor.
    // -----------------------------------------------------------------------

    struct D9MTTextureSideInfo {
      WMTTextureType  type        = WMTTextureType2D;
      WMTPixelFormat  format      = WMTPixelFormatInvalid;
      uint8_t         sampleCount = 1u;
    };

    dxvk::mutex& textureSideMutex() {
      static dxvk::mutex s_mutex;
      return s_mutex;
    }

    std::unordered_map<uint64_t, D9MTTextureSideInfo>& textureSideMap() {
      static std::unordered_map<uint64_t, D9MTTextureSideInfo> s_map;
      return s_map;
    }

    void registerTextureSideInfo(uint64_t handle, const D9MTTextureSideInfo& info) {
      std::lock_guard<dxvk::mutex> lock(textureSideMutex());
      textureSideMap()[handle] = info;
    }

    void unregisterTextureSideInfo(uint64_t handle) {
      std::lock_guard<dxvk::mutex> lock(textureSideMutex());
      textureSideMap().erase(handle);
    }

    D9MTTextureSideInfo lookupTextureSideInfo(uint64_t handle) {
      std::lock_guard<dxvk::mutex> lock(textureSideMutex());
      auto entry = textureSideMap().find(handle);

      if (entry != textureSideMap().end())
        return entry->second;

      Logger::err("d9mt: no side info for texture handle (using 2D/1x)");
      return D9MTTextureSideInfo();
    }


    // -----------------------------------------------------------------------
    // Format helpers
    // -----------------------------------------------------------------------

    constexpr WMTTextureSwizzleChannels d9mtIdentitySwizzle() {
      return { WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha };
    }

    // Maps the VkFormat's component order onto the Metal storage format's
    // channel order. Verified against the bit layouts used by the working
    // hand-rolled driver (src/d3d9/d3d9.cpp d3dfmt_to_wmt):
    //  - VK A4R4G4B4 (A:12-15 R:8-11 G:4-7 B:0-3) stored as Metal ABGR4Unorm
    //    (R:12-15 G:8-11 B:4-7 A:0-3 in channel terms) => swizzle (G,B,A,R).
    //  - VK B5G6R5 (B:11-15 R:0-4) stored as Metal B5G6R5Unorm, whose layout
    //    equals VK R5G6B5 (R:11-15 B:0-4) => swap R/B. VK R5G6B5 itself is
    //    identity (the d3d9 front-end maps D3DFMT_R5G6B5 to VK R5G6B5).
    WMTTextureSwizzleChannels d9mtFormatBaseSwizzle(VkFormat format) {
      switch (format) {
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
          return { WMTTextureSwizzleGreen, WMTTextureSwizzleBlue,
                   WMTTextureSwizzleAlpha, WMTTextureSwizzleRed };
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
          return { WMTTextureSwizzleBlue, WMTTextureSwizzleGreen,
                   WMTTextureSwizzleRed, WMTTextureSwizzleAlpha };
        default:
          return d9mtIdentitySwizzle();
      }
    }

    WMTTextureSwizzle d9mtComposeSwizzleChannel(
            VkComponentSwizzle              component,
            WMTTextureSwizzle               identityChannel,
      const WMTTextureSwizzleChannels&      base) {
      switch (component) {
        case VK_COMPONENT_SWIZZLE_IDENTITY: return identityChannel;
        case VK_COMPONENT_SWIZZLE_ZERO:     return WMTTextureSwizzleZero;
        case VK_COMPONENT_SWIZZLE_ONE:      return WMTTextureSwizzleOne;
        case VK_COMPONENT_SWIZZLE_R:        return base.r;
        case VK_COMPONENT_SWIZZLE_G:        return base.g;
        case VK_COMPONENT_SWIZZLE_B:        return base.b;
        case VK_COMPONENT_SWIZZLE_A:        return base.a;
        default:
          Logger::err(str::format("d9mt: invalid component swizzle ", uint32_t(component)));
          return identityChannel;
      }
    }

    // Composes the view key's packed swizzle (in VkFormat component terms)
    // with the format's storage swizzle into a Metal channel swizzle.
    WMTTextureSwizzleChannels d9mtComposeSwizzle(VkFormat format, uint16_t packedSwizzle) {
      WMTTextureSwizzleChannels base = d9mtFormatBaseSwizzle(format);

      VkComponentMapping mapping = {
        VkComponentSwizzle((packedSwizzle >>  0) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  4) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  8) & 0xf),
        VkComponentSwizzle((packedSwizzle >> 12) & 0xf) };

      WMTTextureSwizzleChannels result = { };
      result.r = d9mtComposeSwizzleChannel(mapping.r, base.r, base);
      result.g = d9mtComposeSwizzleChannel(mapping.g, base.g, base);
      result.b = d9mtComposeSwizzleChannel(mapping.b, base.b, base);
      result.a = d9mtComposeSwizzleChannel(mapping.a, base.a, base);
      return result;
    }

    bool d9mtIsIdentitySwizzle(const WMTTextureSwizzleChannels& s) {
      return s.r == WMTTextureSwizzleRed
          && s.g == WMTTextureSwizzleGreen
          && s.b == WMTTextureSwizzleBlue
          && s.a == WMTTextureSwizzleAlpha;
    }

    // Computes a tightly-packed linear size estimate for an image. Used for
    // pool accounting (getMemoryInfo().size) and memory statistics; Metal
    // does not expose the real allocation size through winemetal.
    VkDeviceSize d9mtImageDataSize(const VkImageCreateInfo& info) {
      const DxvkFormatInfo* formatInfo = lookupFormatInfo(info.format);

      if (!formatInfo)
        return 0u;

      VkDeviceSize layerSize = 0u;

      for (uint32_t i = 0; i < info.mipLevels; i++) {
        VkExtent3D mipExtent  = util::computeMipLevelExtent(info.extent, i);
        VkExtent3D blockCount = util::computeBlockCount(mipExtent, formatInfo->blockSize);

        layerSize += VkDeviceSize(blockCount.width) * blockCount.height
                   * blockCount.depth * formatInfo->elementSize;
      }

      uint32_t samples = std::max(1u, uint32_t(info.samples));
      return layerSize * info.arrayLayers * samples;
    }

    // Maps a Vulkan image view type onto the Metal texture (view) type,
    // taking multisampling of the underlying texture into account.
    WMTTextureType d9mtViewTextureType(VkImageViewType viewType, uint32_t sampleCount) {
      switch (viewType) {
        case VK_IMAGE_VIEW_TYPE_1D:         return WMTTextureType1D;
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY:   return WMTTextureType1DArray;
        case VK_IMAGE_VIEW_TYPE_2D:
          return sampleCount > 1u ? WMTTextureType2DMultisample : WMTTextureType2D;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
          return sampleCount > 1u ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray;
        case VK_IMAGE_VIEW_TYPE_3D:         return WMTTextureType3D;
        case VK_IMAGE_VIEW_TYPE_CUBE:       return WMTTextureTypeCube;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return WMTTextureTypeCubeArray;
        default:
          Logger::err(str::format("d9mt: invalid image view type ", uint32_t(viewType)));
          return WMTTextureType2D;
      }
    }

    // The u64 the Draw stage writes into the argument buffer for this
    // descriptor: gpu_resource_id for views, gpuAddress for raw buffers.
    void d9mtSetDescriptorWord(DxvkDescriptor& descriptor, uint64_t word) {
      std::memcpy(descriptor.descriptor.data(), &word, sizeof(word));
    }

  } // anonymous namespace


  // ==========================================================================
  // DxvkPagedResource (dxvk_sparse.h)
  // ==========================================================================

  std::atomic<uint64_t> DxvkPagedResource::s_cookie = { 0u };


  DxvkPagedResource::~DxvkPagedResource() {

  }


  void DxvkPagedResource::makeResourceResident() {
    m_allocator->requestMakeResident(this);
  }


  DxvkResourceRef::~DxvkResourceRef() {
    auto resource = reinterpret_cast<DxvkPagedResource*>(m_ptr & ~AccessMask);
    auto access = DxvkAccess(m_ptr & AccessMask);

    if (access != DxvkAccess::Move)
      resource->requestResidency();

    resource->release(access);
  }


  // ==========================================================================
  // View maps (dxvk_memory.h)
  // ==========================================================================

  DxvkResourceBufferViewMap::DxvkResourceBufferViewMap(
          DxvkMemoryAllocator*        allocator,
          VkBuffer                    buffer,
          VkDeviceAddress             va)
  : m_device(allocator->device()), m_buffer(buffer), m_va(va) {

  }


  DxvkResourceBufferViewMap::~DxvkResourceBufferViewMap() {
    for (const auto& view : m_views) {
      // formatted views own a Metal texture-buffer view
      if (view.first.format && view.second.legacy.bufferView)
        NSObject_release(obj_handle_t(view.second.legacy.bufferView));
    }
  }


  const DxvkDescriptor* DxvkResourceBufferViewMap::createBufferView(
    const DxvkBufferViewKey&          key,
          VkDeviceSize                baseOffset) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto entry = m_views.find(key);

    if (entry != m_views.end())
      return &entry->second;

    auto& descriptor = m_views.emplace(std::piecewise_construct,
      std::tuple<DxvkBufferViewKey>(key), std::tuple<>()).first->second;
    descriptor = DxvkDescriptor();

    if (key.format) {
      // formatted view: Metal texture-buffer view over the MTLBuffer
      const auto* caps = d9mt::lookupFormatCaps(key.format);
      const DxvkFormatInfo* formatInfo = lookupFormatInfo(key.format);

      if (!caps || caps->wmtFormat == WMTPixelFormatInvalid || !formatInfo) {
        Logger::err(str::format("d9mt: createBufferView: unsupported format ",
          uint32_t(key.format)));
        return &descriptor;
      }

      WMTTextureInfo info = { };
      info.pixel_format       = caps->wmtFormat;
      info.width              = uint32_t(key.size / formatInfo->elementSize);
      info.height             = 1u;
      info.depth              = 1u;
      info.array_length       = 1u;
      info.type               = WMTTextureTypeTextureBuffer;
      info.mipmap_level_count = 1u;
      info.sample_count       = 1u;
      info.usage              = (key.usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
        ? WMTTextureUsage(WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite)
        : WMTTextureUsageShaderRead;
      // all d9mt buffers are shared-storage (see createBufferResource)
      info.options            = WMTResourceStorageModeShared;

      obj_handle_t view = MTLBuffer_newTexture(obj_handle_t(m_buffer), &info,
        baseOffset + key.offset,
        VkDeviceSize(info.width) * formatInfo->elementSize);

      if (!view) {
        Logger::err(str::format("d9mt: MTLBuffer_newTexture failed (format ",
          uint32_t(key.format), ", offset ", baseOffset + key.offset,
          ", size ", key.size, ")"));
        return &descriptor;
      }

      descriptor.legacy.bufferView = VkBufferView(view);
      d9mtSetDescriptorWord(descriptor, info.gpu_resource_id);
    } else {
      // raw view: plain buffer range; AB word is the GPU address
      descriptor.legacy.buffer.buffer = m_buffer;
      descriptor.legacy.buffer.offset = baseOffset + key.offset;
      descriptor.legacy.buffer.range  = key.size;

      d9mtSetDescriptorWord(descriptor, m_va + baseOffset + key.offset);
    }

    return &descriptor;
  }


  DxvkResourceImageViewMap::DxvkResourceImageViewMap(
          DxvkMemoryAllocator*        allocator,
          VkImage                     image)
  : m_device(allocator->device()), m_image(image) {

  }


  DxvkResourceImageViewMap::~DxvkResourceImageViewMap() {
    for (const auto& view : m_views) {
      if (view.second.legacy.image.imageView)
        NSObject_release(obj_handle_t(view.second.legacy.image.imageView));
    }
  }


  const DxvkDescriptor* DxvkResourceImageViewMap::createImageView(
    const DxvkImageViewKey&           key) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto entry = m_views.find(key);

    if (entry != m_views.end())
      return &entry->second;

    auto& descriptor = m_views.emplace(std::piecewise_construct,
      std::tuple<DxvkImageViewKey>(key), std::tuple<>()).first->second;
    descriptor = DxvkDescriptor();
    descriptor.legacy.image.imageLayout = key.layout;

    const auto* caps = d9mt::lookupFormatCaps(key.format);

    if (!caps || caps->wmtFormat == WMTPixelFormatInvalid) {
      Logger::err(str::format("d9mt: createImageView: unsupported format ",
        uint32_t(key.format)));
      return &descriptor;
    }

    D9MTTextureSideInfo tex = lookupTextureSideInfo(uint64_t(m_image));

    // stencil-only sampling reads through the X32_Stencil8 alias
    WMTPixelFormat format = caps->wmtFormat;

    if ((key.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && !(key.aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
      format = WMTPixelFormatX32_Stencil8;

    // depth/stencil and multisampled views must use the default swizzle
    WMTTextureSwizzleChannels swizzle = d9mtIdentitySwizzle();

    bool isDepthStencil = (key.aspects
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0u;

    if (!isDepthStencil && tex.sampleCount <= 1u) {
      swizzle = d9mtComposeSwizzle(key.format, key.packedSwizzle);
    } else if (key.packedSwizzle && !d9mtIsIdentitySwizzle(d9mtComposeSwizzle(key.format, key.packedSwizzle))) {
      // Genuinely unimplementable (Metal forbids swizzled depth/MSAA views),
      // but benign in practice: DSV swizzles are zeroed by the front-end
      // (BACKEND-SURFACE §3.3, never reaches this branch), so this is a
      // SAMPLED depth/MSAA view losing only component replication/forcing
      // (e.g. alpha-one) — shaders still read the stored value in .r.
      // Warn once with details instead of erroring per view.
      static std::atomic<bool> s_warned = { false };
      if (!s_warned.exchange(true)) {
        Logger::warn(str::format("d9mt: createImageView: dropping swizzle on ",
          isDepthStencil ? "depth" : "MSAA", " view (format ",
          uint32_t(key.format), ", packedSwizzle 0x",
          std::hex, uint32_t(key.packedSwizzle), ") — further drops not logged"));
      }
    }

    uint64_t gpuResourceId = 0u;

    obj_handle_t view = MTLTexture_newTextureView(
      obj_handle_t(m_image), format,
      d9mtViewTextureType(key.viewType, tex.sampleCount),
      key.mipIndex, key.mipCount,
      key.layerIndex, key.layerCount,
      swizzle, &gpuResourceId);

    if (!view) {
      Logger::err(str::format("d9mt: MTLTexture_newTextureView failed (format ",
        uint32_t(key.format), ", viewType ", uint32_t(key.viewType),
        ", mips ", uint32_t(key.mipIndex), "+", uint32_t(key.mipCount),
        ", layers ", uint32_t(key.layerIndex), "+", uint32_t(key.layerCount), ")"));
      return &descriptor;
    }

    descriptor.legacy.image.imageView = VkImageView(view);
    d9mtSetDescriptorWord(descriptor, gpuResourceId);
    return &descriptor;
  }


  // ==========================================================================
  // DxvkResourceAllocation
  // ==========================================================================

  DxvkResourceAllocation::~DxvkResourceAllocation() {
    if (m_buffer) {
      if (unlikely(m_bufferViews))
        delete m_bufferViews;

      if (likely(m_flags.test(DxvkAllocationFlag::OwnsBuffer)))
        NSObject_release(obj_handle_t(m_buffer));
    }

    if (m_image) {
      if (likely(m_imageViews))
        delete m_imageViews;

      unregisterTextureSideInfo(uint64_t(m_image));

      if (likely(m_flags.test(DxvkAllocationFlag::OwnsImage)))
        NSObject_release(obj_handle_t(m_image));
    }

    // buffer allocations own VirtualAlloc'ed client memory (newBuffer with
    // bytes-no-copy); images and imports have no CPU allocation to free
    if (m_flags.test(DxvkAllocationFlag::OwnsMemory) && m_mapPtr)
      VirtualFree(m_mapPtr, 0, MEM_RELEASE);
  }


  const DxvkDescriptor* DxvkResourceAllocation::createBufferView(
    const DxvkBufferViewKey&          key) {
    if (unlikely(!m_bufferViews))
      m_bufferViews = new DxvkResourceBufferViewMap(m_allocator, m_buffer, m_bufferAddress);

    return m_bufferViews->createBufferView(key, m_bufferOffset);
  }


  const DxvkDescriptor* DxvkResourceAllocation::createImageView(
    const DxvkImageViewKey&           key) {
    if (unlikely(!m_imageViews))
      m_imageViews = new DxvkResourceImageViewMap(m_allocator, m_image);

    return m_imageViews->createImageView(key);
  }


  void DxvkResourceAllocation::destroyBufferViews() {
    if (m_bufferViews) {
      delete m_bufferViews;
      m_bufferViews = nullptr;
    }
  }


  void DxvkResourceAllocationPool::createPool() {
    auto pool = std::make_unique<StoragePool>();
    pool->next = std::move(m_pool);

    for (size_t i = 0; i < pool->objects.size(); i++)
      m_next = new (pool->objects[i].data) StorageList(m_next);

    m_pool = std::move(pool);
  }


  // ==========================================================================
  // DxvkMemoryAllocator — allocation paths. Bring-up design: one dedicated
  // MTLBuffer / MTLTexture per allocation, no suballocation or caching.
  //  - Buffers: ALWAYS shared-storage, backed by VirtualAlloc'ed 32-bit
  //    client memory (newBufferWithBytesNoCopy through winemetal), so every
  //    buffer has a valid mapPtr regardless of the requested memory type.
  //    memFlags()/getMemoryProperties() still echo the REQUESTED type.
  //  - Images: always private storage (DEVICE_LOCAL), no mapPtr.
  // ==========================================================================

  namespace {

    constexpr VkDeviceSize D9MTBufferAlignment = 1u << 16; // VirtualAlloc granularity

    // ------------------------------------------------------------------
    // Suballocation arena for small, frequently-renamed shared buffers.
    //
    // Bring-up minted a fresh VirtualAlloc + MTLDevice_newBuffer (under a
    // global mutex, crossing Rosetta x86->arm64) for EVERY buffer, including
    // each D3DLOCK_DISCARD rename of dynamic vertex/index/constant buffers.
    // Dynamic-heavy D3D9 content renames those many times per frame, so that
    // churn was the dominant CPU cost and a frame-pacing spike source (global
    // allocator contention + Metal alloc-latency variance + 64 KiB-min VM
    // churn).
    //
    // The arena carves small slices out of a few large persistent MTLBuffers
    // ("chunks") and recycles them through segregated free lists. Chunks are
    // leaked for the session lifetime (like the PSO cache). Correctness rests
    // on: (1) slices are shared-storage, byte-addressable, identical in kind
    // to dedicated d9mt buffers; (2) the front-end honors m_bufferOffset
    // end-to-end (getBufferInfo -> getSliceInfo -> every encode), so a
    // non-zero slice offset binds correctly; (3) freeAllocation only runs
    // after the command buffer retires (resource tracking), so a recycled
    // slice is never reused while the GPU may still read it.
    //
    // Slices carry neither OwnsBuffer nor OwnsMemory, so ~DxvkResourceAllocation
    // never releases the chunk handle or VirtualFrees the chunk memory. The
    // signature (m_buffer != 0 && !OwnsBuffer) uniquely identifies a slice on
    // the free path. Toggle with D9MT_SUBALLOC=0.
    class D9MTBufferArena {

    public:

      static constexpr VkDeviceSize SliceAlign  = 256;                 // safe for uniform/vertex/index offsets
      static constexpr VkDeviceSize MinSlice    = SliceAlign;          // smallest size class (2^8)
      static constexpr VkDeviceSize MaxSlice    = 64u * 1024;          // suballocate requests up to 64 KiB (2^16)
      static constexpr VkDeviceSize ChunkSize   = 8u * 1024 * 1024;    // 8 MiB chunks
      static constexpr uint32_t     NumClasses  = 9;                   // 256,512,1K,2K,4K,8K,16K,32K,64K

      struct Slice {
        obj_handle_t buffer  = 0;   // chunk MTLBuffer handle (NOT owned by the slice)
        void*        mapPtr  = nullptr;
        uint64_t     gpuAddr = 0;
        uint32_t     offset  = 0;
      };

      bool enabled() {
        return m_enabled;
      }

      // Attempts to carve a slice for the given byte size. Returns false for
      // anything larger than MaxSlice (caller falls back to a dedicated buffer).
      bool allocate(VkDeviceSize size, Slice& out) {
        if (!m_enabled || size == 0 || size > MaxSlice)
          return false;

        uint32_t cls = sizeClass(size);

        std::lock_guard<dxvk::mutex> lock(m_mutex);

        auto& freeList = m_free[cls];
        if (!freeList.empty()) {
          out = freeList.back();
          freeList.pop_back();
          return true;
        }

        VkDeviceSize csz = classSize(cls);

        Chunk* chunk = nullptr;
        for (auto& c : m_chunks) {
          if (ChunkSize - c->used >= csz) { chunk = c.get(); break; }
        }

        if (!chunk) {
          chunk = createChunk();
          if (!chunk)
            return false;
        }

        out.buffer  = chunk->buffer;
        out.offset  = uint32_t(chunk->used);
        out.mapPtr  = chunk->base + chunk->used;
        out.gpuAddr = chunk->gpuAddr + chunk->used;
        chunk->used += csz;
        return true;
      }

      // Returns a slice (described by an allocation about to be freed) to its
      // size-class free list. size = the allocation's original requested size.
      void free(VkDeviceSize size, const Slice& slice) {
        uint32_t cls = sizeClass(size);
        std::lock_guard<dxvk::mutex> lock(m_mutex);
        m_free[cls].push_back(slice);
      }

    private:

      struct Chunk {
        obj_handle_t buffer  = 0;
        uint8_t*     base    = nullptr;
        uint64_t     gpuAddr = 0;
        VkDeviceSize used    = 0;
      };

      static uint32_t sizeClass(VkDeviceSize size) {
        VkDeviceSize s = std::max<VkDeviceSize>(size, MinSlice);
        uint32_t cls = 0;
        VkDeviceSize cap = MinSlice;
        while (cap < s && cls + 1u < NumClasses) { cap <<= 1; cls++; }
        return cls;
      }

      static VkDeviceSize classSize(uint32_t cls) {
        return MinSlice << cls;
      }

      Chunk* createChunk() {
        void* mem = VirtualAlloc(nullptr, ChunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem)
          return nullptr;

        WMTBufferInfo info = { };
        info.length  = ChunkSize;
        info.options = WMTResourceStorageModeShared;
        info.memory.set(mem);

        obj_handle_t buffer = MTLDevice_newBuffer(d9mt::mtlDevice(), &info);
        if (!buffer) {
          VirtualFree(mem, 0, MEM_RELEASE);
          return nullptr;
        }

        // Pre-fault every page of the freshly Metal-registered block. Under
        // Rosetta x86_32 + WoW64 the FIRST touch of a newBuffer-no-copy backing
        // is pathologically slow (~hundreds of ms to >1s for big blocks): the
        // cliff otherwise lands on the draw hot path (the first frame that
        // writes a fresh chunk) as a multi-hundred-ms stutter. Faulting the
        // whole range here moves that cost to chunk creation, and the
        // suballocator recycles chunks so steady-state writes stay warm.
        std::memset(mem, 0, ChunkSize);

        auto chunk = std::make_unique<Chunk>();
        chunk->buffer  = buffer;
        chunk->base    = reinterpret_cast<uint8_t*>(mem);
        chunk->gpuAddr = info.gpu_address;
        chunk->used    = 0;
        m_chunks.push_back(std::move(chunk));
        return m_chunks.back().get();
      }

      static bool readEnabled() {
        const char* v = std::getenv("D9MT_SUBALLOC");
        return !(v && v[0] == '0' && v[1] == '\0');
      }

      const bool                                    m_enabled = readEnabled();
      dxvk::mutex                                   m_mutex;
      std::vector<std::unique_ptr<Chunk>>           m_chunks;
      std::array<std::vector<Slice>, NumClasses>    m_free;

    };

    D9MTBufferArena g_bufferArena;

    // Recycle pool for DEDICATED buffers (requests too big for the arena's
    // MaxSlice). Bring-up minted a fresh VirtualAlloc + MTLDevice_newBuffer +
    // full-buffer memset (Rosetta pre-fault) on EVERY such allocation; the
    // per-frame trace showed ~12/frame costing ~6 ms (the bufDed zone). These
    // are recurring sizes (UP-ring orphans, constant-ring wraps, large
    // staging), so an exact-aligned-size free list recycles them: a hit skips
    // the VirtualAlloc, the newBuffer AND the memset. freeAllocation only runs
    // after the command buffer retires, so a pooled buffer is already GPU-idle
    // (the same invariant the arena relies on). Disable with D9MT_BUFPOOL=0.
    class D9MTDedicatedPool {

    public:

      struct Entry {
        obj_handle_t buffer  = 0;
        void*        mem     = nullptr;
        uint64_t     gpuAddr = 0;
        VkDeviceSize cap     = 0;   // aligned VirtualAlloc size of the buffer
      };

      // Aligned capacity a request of `size` needs — matches the dedicated path.
      static VkDeviceSize capFor(VkDeviceSize size) {
        return align(std::max<VkDeviceSize>(size, 1u), D9MTBufferAlignment);
      }

      // Hands back a pooled buffer whose capacity exactly fits `size`, or false.
      bool acquire(VkDeviceSize size, Entry& out) {
        if (!m_enabled)
          return false;
        VkDeviceSize cap = capFor(size);
        std::lock_guard<dxvk::mutex> lock(m_mutex);
        auto it = m_free.find(cap);
        if (it == m_free.end() || it->second.empty())
          return false;
        out = it->second.back();
        it->second.pop_back();
        return true;
      }

      // Takes a freed dedicated buffer into the pool. Returns false (pool off or
      // bucket full) -> caller must release the buffer + memory as normal.
      bool recycle(const Entry& e) {
        if (!m_enabled || !e.buffer)
          return false;
        std::lock_guard<dxvk::mutex> lock(m_mutex);
        auto& bucket = m_free[e.cap];
        if (bucket.size() >= MaxPerClass)
          return false;
        bucket.push_back(e);
        return true;
      }

    private:

      static constexpr size_t MaxPerClass = 32;   // bound retained buffers/size

      static bool readEnabled() {
        const char* v = std::getenv("D9MT_BUFPOOL");
        return !(v && v[0] == '0' && v[1] == '\0');
      }

      const bool                                           m_enabled = readEnabled();
      dxvk::mutex                                           m_mutex;
      std::unordered_map<VkDeviceSize, std::vector<Entry>>  m_free;

    };

    D9MTDedicatedPool g_dedicatedPool;

  }


  DxvkMemoryType* d9mtFindMemoryType(
          std::array<DxvkMemoryType, VK_MAX_MEMORY_TYPES>& types,
          uint32_t              count,
          VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < count; i++) {
      if ((types[i].properties.propertyFlags & properties) == properties)
        return &types[i];
    }

    // fall back to the device-local base type; the property flags a
    // type reports back only drive front-end branching, never mapping
    return &types[0];
  }


  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::createBufferResource(
    const VkBufferCreateInfo&         createInfo,
    const DxvkAllocationInfo&         allocationInfo,
          DxvkLocalAllocationCache*   allocationCache) {
    // allocationCache deliberately unused: dedicated MTLBuffer per
    // allocation for bring-up (see METAL-BACKEND-NOTES.md)

    // Fast path: carve small buffers from the suballocation arena instead of
    // minting a fresh VirtualAlloc + MTLBuffer per request. This kills the
    // per-DISCARD-rename churn that dominated CPU frame time. The slice is a
    // sub-range of a persistent chunk MTLBuffer: it owns neither the handle
    // nor the memory, so ~DxvkResourceAllocation leaves both alone, and the
    // slice returns to its free list in freeAllocation.
    if (D9MTBufferArena::Slice slice; g_bufferArena.allocate(createInfo.size, slice)) {
      D9MT_ZONE(d9mt::ZoneBufAllocSub);
      DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
        allocationInfo.properties);

      std::lock_guard<dxvk::mutex> lock(m_mutex);

      DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
      // intentionally NOT OwnsBuffer / OwnsMemory: the chunk owns both
      allocation->m_resourceCookie = allocationInfo.resourceCookie;
      allocation->m_size           = createInfo.size;
      allocation->m_mapPtr         = slice.mapPtr;
      allocation->m_buffer         = VkBuffer(slice.buffer);
      allocation->m_bufferOffset   = slice.offset;
      allocation->m_bufferAddress  = slice.gpuAddr;
      return allocation;
    }

    // Recycle a pooled dedicated buffer if one of the exact aligned size is
    // free — skips VirtualAlloc + MTLDevice_newBuffer + the pre-fault memset.
    if (D9MTDedicatedPool::Entry pooled; g_dedicatedPool.acquire(createInfo.size, pooled)) {
      DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
        allocationInfo.properties);

      std::lock_guard<dxvk::mutex> lock(m_mutex);

      DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
      allocation->m_flags.set(DxvkAllocationFlag::OwnsBuffer);
      allocation->m_flags.set(DxvkAllocationFlag::OwnsMemory);
      allocation->m_resourceCookie = allocationInfo.resourceCookie;
      allocation->m_size           = createInfo.size;
      allocation->m_mapPtr         = pooled.mem;
      allocation->m_buffer         = VkBuffer(pooled.buffer);
      allocation->m_bufferOffset   = 0u;
      allocation->m_bufferAddress  = pooled.gpuAddr;

      // Re-account like a fresh dedicated alloc; the matching free decrements.
      type->stats.memoryAllocated += pooled.cap;
      type->stats.memoryUsed      += pooled.cap;
      return allocation;
    }

    D9MT_ZONE(d9mt::ZoneBufAllocDed);

    VkDeviceSize size = align(std::max<VkDeviceSize>(createInfo.size, 1u),
      D9MTBufferAlignment);

    void* mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!mem) {
      throw DxvkError(str::format("d9mt: createBufferResource: VirtualAlloc of ",
        size, " bytes failed"));
    }

    WMTBufferInfo info = { };
    info.length  = size;
    info.options = WMTResourceStorageModeShared;
    info.memory.set(mem);

    obj_handle_t buffer = MTLDevice_newBuffer(d9mt::mtlDevice(), &info);

    if (!buffer) {
      VirtualFree(mem, 0, MEM_RELEASE);
      throw DxvkError(str::format("d9mt: MTLDevice_newBuffer failed (",
        size, " bytes)"));
    }

    // Pre-fault the freshly Metal-registered backing — same Rosetta first-touch
    // cliff as createChunk(). Dedicated buffers (UP ring orphans, constant ring
    // wraps, large staging) are the ones the menu hot-path allocates fresh, so
    // faulting here keeps the per-draw write off the cliff.
    std::memset(mem, 0, size);

    DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
      allocationInfo.properties);

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsBuffer);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsMemory);
    allocation->m_resourceCookie = allocationInfo.resourceCookie;
    allocation->m_size           = createInfo.size;
    allocation->m_mapPtr         = mem;
    allocation->m_buffer         = VkBuffer(buffer);
    allocation->m_bufferOffset   = 0u;
    allocation->m_bufferAddress  = info.gpu_address;

    type->stats.memoryAllocated += size;
    type->stats.memoryUsed      += size;
    return allocation;
  }


  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::importBufferResource(
    const VkBufferCreateInfo&         createInfo,
    const DxvkAllocationInfo&         allocationInfo,
    const DxvkBufferImportInfo&       importInfo) {
    // interop-only path; the imported handle is borrowed (retained), no GPU
    // address is available through winemetal for foreign buffers
    if (importInfo.buffer)
      NSObject_retain(obj_handle_t(importInfo.buffer));

    DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
      allocationInfo.properties);

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
    allocation->m_flags.set(DxvkAllocationFlag::Imported);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsBuffer); // owns its reference
    allocation->m_resourceCookie = allocationInfo.resourceCookie;
    allocation->m_size           = createInfo.size;
    allocation->m_mapPtr         = importInfo.mapPtr;
    allocation->m_buffer         = importInfo.buffer;
    allocation->m_bufferOffset   = importInfo.offset;
    allocation->m_bufferAddress  = 0u;

    Logger::warn("d9mt: importBufferResource: imported buffer has no GPU address");
    return allocation;
  }


  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::createImageResource(
    const VkImageCreateInfo&          createInfo,
    const DxvkAllocationInfo&         allocationInfo,
    const void*                       next) {
    if (next) {
      // shared-handle export/import chains; the adapter reports the
      // corresponding features as unsupported, so this is unreachable
      Logger::err("d9mt: createImageResource: external memory info ignored");
    }

    const auto* caps = d9mt::lookupFormatCaps(createInfo.format);
    const DxvkFormatInfo* formatInfo = lookupFormatInfo(createInfo.format);

    if (!caps || caps->wmtFormat == WMTPixelFormatInvalid || !formatInfo) {
      throw DxvkError(str::format("d9mt: createImageResource: unsupported format ",
        uint32_t(createInfo.format)));
    }

    bool isDepthStencil = (formatInfo->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0u;
    bool isCompressed = formatInfo->flags.test(DxvkFormatFlag::BlockCompressed);

    uint32_t samples = std::max(1u, uint32_t(createInfo.samples));

    WMTTextureInfo info = { };
    info.pixel_format       = caps->wmtFormat;
    info.width              = createInfo.extent.width;
    info.height             = createInfo.extent.height;
    info.depth              = createInfo.extent.depth;
    info.mipmap_level_count = createInfo.mipLevels;
    info.sample_count       = samples;
    info.options            = WMTResourceStorageModePrivate;

    if (createInfo.imageType == VK_IMAGE_TYPE_1D) {
      info.type         = createInfo.arrayLayers > 1u ? WMTTextureType1DArray : WMTTextureType1D;
      info.array_length = createInfo.arrayLayers;
    } else if (createInfo.imageType == VK_IMAGE_TYPE_3D) {
      info.type         = WMTTextureType3D;
      info.array_length = 1u;
    } else if (samples > 1u) {
      info.type         = createInfo.arrayLayers > 1u
        ? WMTTextureType2DMultisampleArray
        : WMTTextureType2DMultisample;
      info.array_length = createInfo.arrayLayers;
    } else if ((createInfo.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
            && !(createInfo.arrayLayers % 6u)) {
      info.type         = createInfo.arrayLayers > 6u ? WMTTextureTypeCubeArray : WMTTextureTypeCube;
      info.array_length = createInfo.arrayLayers / 6u;
    } else {
      info.type         = createInfo.arrayLayers > 1u ? WMTTextureType2DArray : WMTTextureType2D;
      info.array_length = createInfo.arrayLayers;
    }

    // Permissive usage so ensureImageCompatibility can trivially return true:
    // shaderRead always; renderTarget whenever the format supports attachment
    // use (autogen/blit passes may need it even without an attachment usage
    // bit); shaderWrite for storage; pixelFormatView for plain color formats
    // (sampled views routinely carry component swizzles, e.g. X8R8G8B8).
    constexpr VkFormatFeatureFlags2 attachmentBits =
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;

    uint32_t usage = WMTTextureUsageShaderRead;

    if (caps->optimal & attachmentBits)
      usage |= WMTTextureUsageRenderTarget;

    if ((createInfo.usage & VK_IMAGE_USAGE_STORAGE_BIT)
     && (caps->optimal & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT))
      usage |= WMTTextureUsageShaderWrite;

    if (!isDepthStencil && !isCompressed && samples <= 1u)
      usage |= WMTTextureUsagePixelFormatView;

    info.usage = WMTTextureUsage(usage);

    obj_handle_t image = MTLDevice_newTexture(d9mt::mtlDevice(), &info);

    if (!image) {
      throw DxvkError(str::format("d9mt: MTLDevice_newTexture failed (",
        createInfo.extent.width, "x", createInfo.extent.height, "x",
        createInfo.extent.depth, ", format ", uint32_t(createInfo.format),
        ", mips ", createInfo.mipLevels, ", layers ", createInfo.arrayLayers,
        ", samples ", samples, ")"));
    }

    D9MTTextureSideInfo side = { };
    side.type        = info.type;
    side.format      = caps->wmtFormat;
    side.sampleCount = uint8_t(samples);
    registerTextureSideInfo(image, side);

    DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
      allocationInfo.properties);

    VkDeviceSize size = d9mtImageDataSize(createInfo);

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsImage);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsMemory);
    allocation->m_resourceCookie = allocationInfo.resourceCookie;
    allocation->m_size           = size;
    allocation->m_image          = VkImage(image);

    type->stats.memoryAllocated += size;
    type->stats.memoryUsed      += size;
    return allocation;
  }


  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::importImageResource(
    const VkImageCreateInfo&          createInfo,
    const DxvkAllocationInfo&         allocationInfo,
          VkImage                     imageHandle) {
    if (!imageHandle)
      throw DxvkError("d9mt: importImageResource: null image handle");

    // borrow a reference to the foreign MTLTexture (presenter proxy etc.)
    NSObject_retain(obj_handle_t(imageHandle));

    D9MTTextureSideInfo side = { };
    side.format      = WMTPixelFormatInvalid;
    side.sampleCount = uint8_t(std::max(1u, uint32_t(createInfo.samples)));

    if (const auto* caps = d9mt::lookupFormatCaps(createInfo.format))
      side.format = caps->wmtFormat;

    if (createInfo.imageType == VK_IMAGE_TYPE_3D)
      side.type = WMTTextureType3D;
    else if (side.sampleCount > 1u)
      side.type = WMTTextureType2DMultisample;
    else
      side.type = createInfo.arrayLayers > 1u ? WMTTextureType2DArray : WMTTextureType2D;

    registerTextureSideInfo(uint64_t(imageHandle), side);

    DxvkMemoryType* type = d9mtFindMemoryType(m_memTypes, m_memTypeCount,
      allocationInfo.properties);

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    DxvkResourceAllocation* allocation = m_allocationPool.create(this, type);
    allocation->m_flags.set(DxvkAllocationFlag::Imported);
    allocation->m_flags.set(DxvkAllocationFlag::OwnsImage); // owns its reference
    allocation->m_resourceCookie = allocationInfo.resourceCookie;
    allocation->m_size           = d9mtImageDataSize(createInfo);
    allocation->m_image          = imageHandle;
    return allocation;
  }


  void DxvkMemoryAllocator::freeAllocation(
          DxvkResourceAllocation* allocation) {
    if (unlikely(allocation->m_flags.test(DxvkAllocationFlag::ClearOnFree))) {
      if (allocation->m_mapPtr)
        std::memset(allocation->m_mapPtr, 0, allocation->m_size);
    }

    // Suballocated slices own neither the chunk handle nor its memory; the
    // unique signature is "has a buffer it does not own". Return the slice to
    // the arena (its own lock) before touching the pool. ~DxvkResourceAllocation
    // sees no Owns* flags and leaves the chunk buffer/memory untouched. This
    // only runs after the command buffer retires, so the GPU is done with it.
    if (allocation->m_buffer
     && !allocation->m_flags.test(DxvkAllocationFlag::OwnsBuffer)
     && !allocation->m_flags.test(DxvkAllocationFlag::OwnsMemory)
     && !allocation->m_flags.test(DxvkAllocationFlag::Imported)) {
      D9MTBufferArena::Slice slice;
      slice.buffer  = obj_handle_t(allocation->m_buffer);
      slice.mapPtr  = allocation->m_mapPtr;
      slice.gpuAddr = allocation->m_bufferAddress;
      slice.offset  = uint32_t(allocation->m_bufferOffset);
      g_bufferArena.free(allocation->m_size, slice);
    }

    // Dedicated-buffer recycle (mirrors the slice path above): hand whole
    // OwnsBuffer/OwnsMemory dedicated buffers to the size-keyed pool instead of
    // releasing them, so createBufferResource can reuse one and skip the
    // VirtualAlloc + newBuffer + memset. Post-retirement here, so GPU-idle.
    bool recycledDedicated = false;
    if (allocation->m_buffer
     && allocation->m_flags.test(DxvkAllocationFlag::OwnsBuffer)
     && allocation->m_flags.test(DxvkAllocationFlag::OwnsMemory)
     && !allocation->m_flags.test(DxvkAllocationFlag::Imported)
     && allocation->m_mapPtr) {
      D9MTDedicatedPool::Entry e;
      e.buffer  = obj_handle_t(allocation->m_buffer);
      e.mem     = allocation->m_mapPtr;
      e.gpuAddr = allocation->m_bufferAddress;
      e.cap     = D9MTDedicatedPool::capFor(allocation->m_size);
      recycledDedicated = g_dedicatedPool.recycle(e);
    }

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (allocation->m_flags.test(DxvkAllocationFlag::OwnsMemory) && allocation->m_type) {
      // buffers account their aligned VirtualAlloc size
      VkDeviceSize size = allocation->m_buffer
        ? align(std::max<VkDeviceSize>(allocation->m_size, 1u), D9MTBufferAlignment)
        : allocation->m_size;

      allocation->m_type->stats.memoryAllocated -= size;
      allocation->m_type->stats.memoryUsed      -= size;
    }

    // Pooled: keep the handle + memory alive for reuse. Drop the Owns* flags so
    // ~DxvkResourceAllocation skips NSObject_release + VirtualFree (it still
    // frees any buffer views, exactly like the suballocated-slice path).
    if (recycledDedicated) {
      allocation->m_flags.clr(DxvkAllocationFlag::OwnsBuffer);
      allocation->m_flags.clr(DxvkAllocationFlag::OwnsMemory);
    }

    m_allocationPool.free(allocation);
  }


  bool DxvkMemoryAllocator::getBufferMemoryRequirements(
    const VkBufferCreateInfo&     createInfo,
          VkMemoryRequirements2&  memoryRequirements) const {
    memoryRequirements.memoryRequirements.size           = align(
      std::max<VkDeviceSize>(createInfo.size, 1u), GlobalBufferAlignment);
    memoryRequirements.memoryRequirements.alignment      = GlobalBufferAlignment;
    memoryRequirements.memoryRequirements.memoryTypeBits = (1u << m_memTypeCount) - 1u;

    auto next = reinterpret_cast<VkBaseOutStructure*>(memoryRequirements.pNext);

    while (next) {
      if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
        auto dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(next);
        dedicated->prefersDedicatedAllocation  = VK_TRUE; // everything is dedicated here
        dedicated->requiresDedicatedAllocation = VK_FALSE;
      }
      next = next->pNext;
    }

    return true;
  }


  bool DxvkMemoryAllocator::getImageMemoryRequirements(
    const VkImageCreateInfo&      createInfo,
          VkMemoryRequirements2&  memoryRequirements) const {
    VkDeviceSize size = d9mtImageDataSize(createInfo);

    if (!size)
      return false;

    memoryRequirements.memoryRequirements.size           = align(size, D9MTBufferAlignment);
    memoryRequirements.memoryRequirements.alignment      = D9MTBufferAlignment;
    memoryRequirements.memoryRequirements.memoryTypeBits =
      getMemoryTypeMask(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    auto next = reinterpret_cast<VkBaseOutStructure*>(memoryRequirements.pNext);

    while (next) {
      if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
        auto dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(next);
        dedicated->prefersDedicatedAllocation  = VK_TRUE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
      }
      next = next->pNext;
    }

    return true;
  }


  void DxvkMemoryAllocator::registerResource(
          DxvkPagedResource*          resource) {
    std::lock_guard<dxvk::mutex> lock(m_resourceMutex);
    m_resourceMap.emplace(resource->cookie(), resource);
  }


  void DxvkMemoryAllocator::unregisterResource(
          DxvkPagedResource*          resource) {
    std::lock_guard<dxvk::mutex> lock(m_resourceMutex);
    m_resourceMap.erase(resource->cookie());
  }


  void DxvkMemoryAllocator::requestMakeResident(
          DxvkPagedResource*          resource) {
    // nothing is ever evicted on this backend (resources are created
    // resident and never relocated), so this can only fire on a logic bug
    Logger::err("d9mt: requestMakeResident called on never-evicting allocator");
  }


  void DxvkMemoryAllocator::lockResourceGpuAddress(
    const Rc<DxvkResourceAllocation>& allocation) {
    // allocations never set CanMove (no defragmentation), mirror
    // upstream's flag handling for completeness
    if (allocation->m_flags.test(DxvkAllocationFlag::CanMove)) {
      std::lock_guard<dxvk::mutex> lock(m_resourceMutex);
      allocation->m_flags.clr(DxvkAllocationFlag::CanMove);
    }
  }


  void DxvkMemoryAllocator::performTimedTasks() {
    // no chunk recycling, defragmentation or cache trimming to do:
    // every allocation is dedicated and freed eagerly
  }


  // ==========================================================================
  // DxvkBuffer / DxvkBufferView (mirrors upstream dxvk_buffer.cpp)
  // ==========================================================================

  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
          DxvkMemoryAllocator&  memAlloc,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(memAlloc),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_sharingMode   (device->getSharingMode()),
    m_info          (createInfo) {
    m_allocator->registerResource(this);

    // debug names are only materialized with debug tooling, which the
    // Metal backend does not support; keep the hot rename path cheap
    m_info.debugName = nullptr;

    assignStorage(allocateStorage());
  }


  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
    const DxvkBufferImportInfo& importInfo,
          DxvkMemoryAllocator&  memAlloc,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(memAlloc),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_sharingMode   (device->getSharingMode()),
    m_info          (createInfo),
    m_stableAddress (true) {
    m_allocator->registerResource(this);

    m_info.debugName = nullptr;

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();
    allocationInfo.properties = memFlags;

    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.flags = m_info.flags;
    info.usage = m_info.usage;
    info.size = m_info.size;
    m_sharingMode.fill(info);

    assignStorage(memAlloc.importBufferResource(info, allocationInfo, importInfo));
  }


  DxvkBuffer::~DxvkBuffer() {
    m_allocator->unregisterResource(this);
  }


  bool DxvkBuffer::canRelocate() const {
    return !m_bufferInfo.mapPtr && !m_stableAddress
        && !(m_info.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT);
  }


  Rc<DxvkBufferView> DxvkBuffer::createView(
    const DxvkBufferViewKey& info) {
    std::unique_lock<dxvk::mutex> lock(m_viewMutex);

    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));

    return &entry.first->second;
  }


  DxvkSparsePageTable* DxvkBuffer::getSparsePageTable() {
    return nullptr;
  }


  Rc<DxvkResourceAllocation> DxvkBuffer::relocateStorage(
          DxvkAllocationModes         mode) {
    // the allocator never relocates resources on this backend
    Logger::err("d9mt: DxvkBuffer::relocateStorage called");
    return nullptr;
  }


  void DxvkBuffer::setDebugName(const char* name) {
    if (likely(!m_info.debugName))
      return;

    m_debugName = createDebugName(name);
    m_info.debugName = m_debugName.c_str();

    updateDebugName();
  }


  void DxvkBuffer::updateDebugName() {
    // no Metal label plumbing through winemetal; unreachable since
    // m_info.debugName is never set on this backend
  }


  std::string DxvkBuffer::createDebugName(const char* name) const {
    return str::format(name && name[0] ? name : "Buffer", " (", cookie(), ")");
  }


  void DxvkBufferView::updateViews() {
    if (likely(m_key.format))
      m_formatted = m_buffer->m_storage->createBufferView(m_key);

    if (likely(m_buffer->info().usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      DxvkBufferViewKey rawKey = m_key;
      rawKey.format = VK_FORMAT_UNDEFINED;
      rawKey.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

      m_raw = m_buffer->m_storage->createBufferView(rawKey);
    }

    m_version = m_buffer->m_version;
  }


  // ==========================================================================
  // DxvkImage / DxvkImageView (mirrors upstream dxvk_image.cpp)
  // ==========================================================================

  DxvkImage::DxvkImage(
          DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_info          (createInfo) {
    m_allocator->registerResource(this);

    copyFormatList(createInfo.viewFormatCount, createInfo.viewFormats);

    m_info.debugName = nullptr;

    // Always enable depth-stencil attachment usage for depth-stencil
    // formats since some internal operations rely on it.
    if (lookupFormatInfo(createInfo.format)->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
      m_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    VkImageCreateInfo imageInfo = getImageCreateInfo(DxvkImageUsageInfo());
    m_shared = canShareImage(device, imageInfo, m_info.sharing);

    if (m_info.sharing.mode != DxvkSharedHandleMode::Import)
      m_uninitializedSubresourceCount = m_info.numLayers * m_info.mipLevels;

    assignStorage(allocateStorage());
  }


  DxvkImage::DxvkImage(
          DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          VkImage               imageHandle,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_info          (createInfo),
    m_stableAddress (true) {
    m_allocator->registerResource(this);

    copyFormatList(createInfo.viewFormatCount, createInfo.viewFormats);

    m_info.debugName = nullptr;

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();

    VkImageCreateInfo imageInfo = getImageCreateInfo(DxvkImageUsageInfo());
    assignStorage(m_allocator->importImageResource(imageInfo, allocationInfo, imageHandle));
  }


  DxvkImage::~DxvkImage() {
    m_allocator->unregisterResource(this);
  }


  bool DxvkImage::canRelocate() const {
    return false; // the Metal backend never relocates resources
  }


  HANDLE DxvkImage::sharedHandle() const {
    if (m_shared)
      Logger::err("d9mt: DxvkImage::sharedHandle: shared images not supported");

    return INVALID_HANDLE_VALUE;
  }


  DxvkSparsePageTable* DxvkImage::getSparsePageTable() {
    return nullptr;
  }


  Rc<DxvkResourceAllocation> DxvkImage::relocateStorage(
          DxvkAllocationModes         mode) {
    Logger::err("d9mt: DxvkImage::relocateStorage called");
    return nullptr;
  }


  uint64_t DxvkImage::getTrackingAddress(uint32_t mip, uint32_t layer, VkOffset3D coord) const {
    // For 2D and 3D images, use morton codes to linearize the address ranges
    // of pixel blocks (mirrors upstream; only uniqueness/monotonicity matter)
    uint64_t base = getTrackingAddress(mip, layer);

    if (likely(m_info.type == VK_IMAGE_TYPE_2D))
      return base + bit::interleave(coord.x, coord.y);

    if (m_info.type == VK_IMAGE_TYPE_1D)
      return base + coord.x;

    if (formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed))
      return base + bit::interleave(coord.x, coord.y) + (uint64_t(coord.z) << 32u);

    return base + bit::interleave(coord.x, coord.y, coord.z);
  }


  Rc<DxvkImageView> DxvkImage::createView(
    const DxvkImageViewKey& info) {
    std::unique_lock<dxvk::mutex> lock(m_viewMutex);

    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));

    return &entry.first->second;
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorage() {
    return allocateStorageWithUsage(DxvkImageUsageInfo(), 0u);
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorageWithUsage(
    const DxvkImageUsageInfo&         usageInfo,
          DxvkAllocationModes         mode) {
    VkImageCreateInfo imageInfo = getImageCreateInfo(usageInfo);

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();
    allocationInfo.properties = m_properties;
    allocationInfo.mode = mode;

    // Metal textures are created with permissive usage up front, so no
    // format list / external memory chains are needed here.
    return m_allocator->createImageResource(imageInfo, allocationInfo, nullptr);
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorage(
          Rc<DxvkResourceAllocation>&& resource) {
    return assignStorageWithUsage(std::move(resource), DxvkImageUsageInfo());
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorageWithUsage(
          Rc<DxvkResourceAllocation>&& resource,
    const DxvkImageUsageInfo&         usageInfo) {
    Rc<DxvkResourceAllocation> old = std::move(m_storage);

    // Self-assignment is possible here if we just update image properties
    bool invalidateViews = false;
    m_storage = std::move(resource);

    if (m_storage != old) {
      m_imageInfo = m_storage->getImageInfo();
      invalidateViews = true;
    }

    if ((m_info.access | usageInfo.access) != m_info.access)
      invalidateViews = true;

    m_info.flags  |= usageInfo.flags;
    m_info.usage  |= usageInfo.usage;
    m_info.stages |= usageInfo.stages;
    m_info.access |= usageInfo.access;

    if (usageInfo.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
      m_info.layout = usageInfo.layout;
      invalidateViews = true;
    }

    if (usageInfo.colorSpace != VK_COLOR_SPACE_MAX_ENUM_KHR)
      m_info.colorSpace = usageInfo.colorSpace;

    for (uint32_t i = 0; i < usageInfo.viewFormatCount; i++) {
      if (!isViewCompatible(usageInfo.viewFormats[i]))
        m_viewFormats.push_back(usageInfo.viewFormats[i]);
    }

    if (!m_viewFormats.empty()) {
      m_info.viewFormatCount = m_viewFormats.size();
      m_info.viewFormats = m_viewFormats.data();
    }

    m_stableAddress |= usageInfo.stableGpuAddress;

    if (invalidateViews)
      m_version += 1u;

    if (!(m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      auto common = m_properties & m_storage->getMemoryProperties();

      updateResidencyStatus((common & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ? DxvkResourceResidency::Resident
        : DxvkResourceResidency::Evicted);
    }

    return old;
  }


  void DxvkImage::trackInitialization(
    const VkImageSubresourceRange& subresources) {
    if (!m_uninitializedSubresourceCount)
      return;

    if (subresources.levelCount == m_info.mipLevels && subresources.layerCount == m_info.numLayers) {
      // Trivial case, everything gets initialized at once
      m_uninitializedSubresourceCount = 0u;
      m_uninitializedMipsPerLayer.clear();
    } else {
      // Partial initialization. Track each layer individually.
      if (m_uninitializedMipsPerLayer.empty()) {
        m_uninitializedMipsPerLayer.resize(m_info.numLayers);

        for (uint32_t i = 0; i < m_info.numLayers; i++)
          m_uninitializedMipsPerLayer[i] = uint16_t(1u << m_info.mipLevels) - 1u;
      }

      uint16_t mipMask = ((1u << subresources.levelCount) - 1u) << subresources.baseMipLevel;

      for (uint32_t i = subresources.baseArrayLayer; i < subresources.baseArrayLayer + subresources.layerCount; i++) {
        m_uninitializedSubresourceCount -= bit::popcnt(uint16_t(m_uninitializedMipsPerLayer[i] & mipMask));
        m_uninitializedMipsPerLayer[i] &= ~mipMask;
      }

      if (!m_uninitializedSubresourceCount)
        m_uninitializedMipsPerLayer.clear();
    }
  }


  bool DxvkImage::isInitialized(
    const VkImageSubresourceRange& subresources) const {
    if (likely(!m_uninitializedSubresourceCount))
      return true;

    if (m_uninitializedMipsPerLayer.empty())
      return false;

    uint16_t mipMask = ((1u << subresources.levelCount) - 1u) << subresources.baseMipLevel;

    for (uint32_t i = 0; i < subresources.layerCount; i++) {
      if (m_uninitializedMipsPerLayer[subresources.baseArrayLayer + i] & mipMask)
        return false;
    }

    return true;
  }


  void DxvkImage::setDebugName(const char* name) {
    if (likely(!m_info.debugName))
      return;

    m_debugName = createDebugName(name);
    m_info.debugName = m_debugName.c_str();

    updateDebugName();
  }


  void DxvkImage::updateDebugName() {
    // no Metal label plumbing through winemetal (see DxvkBuffer)
  }


  std::string DxvkImage::createDebugName(const char* name) const {
    return str::format(name && name[0] ? name : "Image", " (", cookie(), ")");
  }


  VkImageCreateInfo DxvkImage::getImageCreateInfo(
    const DxvkImageUsageInfo&         usageInfo) const {
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = m_info.flags | usageInfo.flags;
    info.imageType = m_info.type;
    info.format = m_info.format;
    info.extent = m_info.extent;
    info.mipLevels = m_info.mipLevels;
    info.arrayLayers = m_info.numLayers;
    info.samples = m_info.sampleCount;
    info.tiling = m_info.tiling;
    info.usage = m_info.usage | usageInfo.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = m_info.initialLayout;

    return info;
  }


  void DxvkImage::copyFormatList(uint32_t formatCount, const VkFormat* formats) {
    m_viewFormats.resize(formatCount);

    for (uint32_t i = 0; i < formatCount; i++)
      m_viewFormats[i] = formats[i];

    m_info.viewFormats = m_viewFormats.data();
  }


  bool DxvkImage::canShareImage(
          DxvkDevice*           device,
    const VkImageCreateInfo&    createInfo,
    const DxvkSharedHandleInfo& sharingInfo) const {
    if (sharingInfo.mode == DxvkSharedHandleMode::None)
      return false;

    Logger::err("d9mt: shared images are not supported on the Metal backend");
    return false;
  }


  DxvkImageView::DxvkImageView(
          DxvkImage*                image,
    const DxvkImageViewKey&         key)
  : m_image   (image),
    m_key     (key) {
    // If the view does not define a layout, pick one based on usage
    // (mirrors upstream; layouts are metadata-only on this backend)
    if (!m_key.layout) {
      switch (m_key.usage) {
        case VK_IMAGE_USAGE_SAMPLED_BIT:
          m_key.layout = (m_image->formatInfo()->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          break;

        case VK_IMAGE_USAGE_STORAGE_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_GENERAL;
          break;

        case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          break;

        case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
          break;

        default:
          break;
      }
    }

    updateProperties();
  }


  DxvkImageView::~DxvkImageView() {

  }


  const DxvkDescriptor* DxvkImageView::createView(VkImageViewType type) const {
    constexpr VkImageUsageFlags ViewUsage =
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    DxvkImageViewKey key = m_key;
    key.viewType = type;
    key.layout = getLayout();

    if (!(key.usage & ViewUsage))
      return nullptr;

    // Only use one layer for non-arrayed view types
    if (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_2D)
      key.layerCount = 1u;

    switch (m_image->info().type) {
      case VK_IMAGE_TYPE_1D: {
        if (type != VK_IMAGE_VIEW_TYPE_1D && type != VK_IMAGE_VIEW_TYPE_1D_ARRAY)
          return nullptr;
      } break;

      case VK_IMAGE_TYPE_2D: {
        if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
          if (key.layerCount < 6 || !(m_image->info().flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
            return nullptr;

          key.layerCount = type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
            ? key.layerCount - key.layerCount % 6u : 6u;
        } else if (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
          return nullptr;
        }
      } break;

      case VK_IMAGE_TYPE_3D: {
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
          // Metal cannot create 2D(-array) views of 3D textures at all
          Logger::err("d9mt: 2D view of a 3D image requested (unsupported on Metal)");
          return nullptr;
        } else if (type != VK_IMAGE_VIEW_TYPE_3D) {
          return nullptr;
        }
      } break;

      default:
        return nullptr;
    }

    // RT and UAV views must use the default component mapping
    if (key.usage != VK_IMAGE_USAGE_SAMPLED_BIT)
      key.packedSwizzle = 0u;

    return m_image->m_storage->createImageView(key);
  }


  void DxvkImageView::updateViews() {
    // Latch updated image properties
    updateProperties();

    // Update all views that are not currently null
    for (uint32_t i = 0; i < m_views.size(); i++) {
      if (m_views[i])
        m_views[i] = createView(VkImageViewType(i));
    }

    m_version = m_image->m_version;
  }


  void DxvkImageView::updateProperties() {
    m_properties.samples = m_image->info().sampleCount;
    m_properties.access = m_image->info().access;
  }


  // ==========================================================================
  // DxvkStagingBuffer (mirrors upstream dxvk_staging.cpp)
  // ==========================================================================

  DxvkStagingBuffer::DxvkStagingBuffer(
    const Rc<DxvkDevice>&     device,
          VkDeviceSize        size)
  : m_device(device), m_offset(0), m_size(size) {

  }


  DxvkStagingBuffer::~DxvkStagingBuffer() {

  }


  DxvkBufferSlice DxvkStagingBuffer::alloc(VkDeviceSize size) {
    DxvkBufferCreateInfo info;
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
                | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT
                | VK_ACCESS_SHADER_READ_BIT;
    info.debugName = "Staging buffer";

    VkDeviceSize alignedSize = dxvk::align(size, 256u);
    m_allocationCounter += alignedSize;

    if (2 * alignedSize > m_size) {
      return DxvkBufferSlice(m_device->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    }

    if (m_offset + alignedSize > m_size || m_buffer == nullptr) {
      info.size = m_size;

      // Free resources first if possible, in some rare
      // situations this may help avoid a memory allocation.
      m_buffer = nullptr;
      m_buffer = m_device->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      m_offset = 0;
    }

    DxvkBufferSlice slice(m_buffer, m_offset, size);
    m_offset += alignedSize;
    return slice;
  }


  void DxvkStagingBuffer::reset() {
    m_buffer = nullptr;
    m_offset = 0;

    m_allocationCounterValueOnReset = m_allocationCounter;
  }


  // ==========================================================================
  // DxvkDevice resource factories
  // ==========================================================================

  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo,
      m_objects.memoryManager(), memoryType);
  }


  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo,
      m_objects.memoryManager(), memoryType);
  }


  Rc<DxvkBuffer> DxvkDevice::importBuffer(
    const DxvkBufferCreateInfo& createInfo,
    const DxvkBufferImportInfo& importInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo, importInfo,
      m_objects.memoryManager(), memoryType);
  }


  Rc<DxvkImage> DxvkDevice::importImage(
    const DxvkImageCreateInfo&  createInfo,
          VkImage               image,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo, image,
      m_objects.memoryManager(), memoryType);
  }

}
