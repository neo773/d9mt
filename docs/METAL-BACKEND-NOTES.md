# Metal backend implementation notes (living doc)

Companion to BACKEND-SURFACE.md. This records the ACTUAL implementation
decisions for the Metal DxvkDevice/DxvkContext in src/d3d9fe/. Update as
classes move from stubs.cpp to real implementations.

## Core architecture decisions

1. **Vendored headers stay verbatim.** We implement the missing .cpp
   bodies (upstream's dxvk_*.cpp Vulkan implementations are NOT vendored;
   every out-of-line method is ours to define). Private members are
   Vulkan-shaped; we either use them (smuggling, below) or leave them
   null. NO header patches unless unavoidable (mark `// d9mt:`).

2. **Handle smuggling.** Vulkan non-dispatchable handles are u64; winemetal
   obj_handle_t is u64. Mapping:
   - `VkBuffer`   := WMT MTLBuffer obj_handle_t
   - `VkImage`    := WMT MTLTexture obj_handle_t
   - `VkImageView`:= WMT MTLTexture (view) obj_handle_t
     (lives in DxvkDescriptor::legacy.image.imageView)
   - `VkDeviceAddress / gpuAddress` := MTLBuffer.gpuAddress (real)
   - `VkSampler`  := WMT MTLSamplerState obj_handle_t (in descriptor)
   - `VkDeviceMemory` := 0 (never dereferenced by front-end)
   - `VkPipeline` (builtin compute) := opaque pointer to our PSO struct
   - `VkBufferView` := WMT MTLTexture (texture-buffer view) obj_handle_t
     (lives in DxvkDescriptor::legacy.bufferView)
   - `DxvkDescriptor::descriptor[0..7]` := the u64 argument-buffer word the
     Draw stage writes into set-0 AB slots: gpu_resource_id for image /
     texel-buffer views, gpuAddress+offset for raw buffer descriptors.
   Inline header methods read these fields directly (getSliceInfo,
   handle(), getDescriptor()) — fill them correctly and the entire inline
   surface works untouched.

3. **Side state.** Where a class needs Metal-only state with no usable
   member, use a global side-table keyed by object pointer
   (`d9mt::sideState<T>()`), or allocate our own struct and stash the
   pointer in an unused member. Prefer real members when types permit.

4. **Synchronous CS first.** DxvkCsThread is vendored and real, but
   bring-up may run with the CS thread enabled since chunks just execute
   our context methods. Submission is synchronous-ish: flushCommandList
   commits the MTLCommandBuffer; completion handlers fire signals.

## Key ABI facts (read from headers, do not re-derive)

- `DxvkResourceAllocation` (dxvk_memory.h): friend DxvkMemoryAllocator →
  our allocator code fills privates: m_buffer, m_bufferOffset,
  m_bufferAddress, m_mapPtr, m_size, m_image, m_type (must be non-null →
  points into allocator's m_memTypes; getMemoryProperties reads
  m_type->properties.propertyFlags), m_flags. free() →
  m_allocator->freeAllocation(this) (ours; releases WMT handles).
  m_address: chunk-address; offset part masked by ChunkAddressMask in
  getMemoryInfo — set = 0 for dedicated allocs.
- `DxvkBuffer` (dxvk_buffer.h): everything hot is inline over
  m_bufferInfo (DxvkResourceBufferInfo {buffer,offset,size,mapPtr,
  gpuAddress}) and m_storage. allocateStorage() is INLINE → calls
  m_allocator->createBufferResource (our allocator). Ours: ctors, dtor,
  createView, canRelocate (return !m_stableAddress), setDebugName,
  relocateStorage (never called if we never relocate → abort ok),
  getSparsePageTable (nullptr).
- `DxvkImage` (dxvk_image.h): inline over m_imageInfo {image,mapPtr} +
  m_storage. Ours: ctors, dtor, createView, allocateStorage,
  allocateStorageWithUsage, assignStorage(+WithUsage), canRelocate,
  trackInitialization/isInitialized, getTrackingAddress(coord),
  sharedHandle (abort), setDebugName. m_viewFormats copied at create.
  NB assignStorage must update m_imageInfo and ++m_version.
- `DxvkImageView`: getDescriptor inline; calls our out-of-line
  createView(viewType) + updateViews(). Views cached per allocation in
  DxvkResourceImageViewMap (ours) — descriptor->legacy.image.imageView
  holds the MTLTexture view handle.
- `DxvkPagedResource` (dxvk_sparse.h): fully inline refcount/use-count.
  acquire(DxvkAccess) increments use bits (Read=1<<20,Write=1<<40 via
  getIncrement). isInUse() drives waitForResource. virtual dtor is
  out-of-line → define in our resources TU. Cmdlist `track()` must
  acquire(access) and release on completion.
- `DxvkAdapter`: by-value member DxvkDeviceCapabilities m_capabilities;
  its ctor (instance, VkPhysicalDevice, const VkDeviceCreateInfo*) is
  OURS → single place that fills ALL properties/features/memory/queues.
- `DxvkDevice` member cascade: DxvkObjects m_objects {DescriptorProperties,
  MemoryAllocator, SamplerPool, PipelineManager, GpuEventPool,
  GpuQueryPool, UnboundResources, 4 Lazy<Meta*> (never .get() in our
  backend)} + DxvkSubmissionQueue m_submissionQueue (ctor must NOT spawn
  the upstream worker threads — our presentImage/submitCommandList
  bypass the queue; getLastError()/synchronizeUntil are inline and used).
- `getSamplerStats()` inline → m_objects.samplerPool().getStats() must work.

## Caps decisions (DxvkDeviceCapabilities ctor)

Features ON: robustBufferAccess, independentBlend, samplerAnisotropy,
textureCompressionBC, occlusionQueryPrecise, imageCubeArray,
fullDrawIndexUint32, shaderClipDistance, depthClamp, depthBiasClamp,
fillModeNonSolid, multiViewport; extRobustness2.robustBufferAccess2 +
nullDescriptor=true (bring-up: unbound slots just write 0 into AB);
extAttachmentFeedbackLoopLayout=true (hazard path); extDepthClipEnable;
extDepthBiasControl {depthBiasControl, depthBiasExact,
floatRepresentation} = true; khrSwapchain=true.
Features OFF: vertexPipelineStoresAndAtomics + vk12.shaderInt8 (gates
SWVP GS path off), geometryShader, tessellation, depthBounds,
extGraphicsPipelineLibrary, extCustomBorderColor, extNonSeamlessCubeMap,
sparse*, logicOp.
Properties: deviceName "Apple M1 Max (d9mt)", vendor/device 0x106b/0xa1,
apiVersion 1.3, timestampPeriod 1.0, maxUniformBufferRange 65536,
minUniformBufferOffsetAlignment 256, minStorageBufferOffsetAlignment 16,
maxPushConstantsSize 256, maxColorAttachments 8 (d3d9 uses 4),
framebufferColor/DepthSampleCounts 1|2|4|8, pointSizeRange {1,511},
maxSamplerAnisotropy 16, maxImageDimension2D/Cube 16384, 3D 2048,
maxVertexInputAttributes/Bindings 32, maxViewports 16.
Memory: heap0 DEVICE_LOCAL 3 GiB (GetAvailableTextureMem budget match);
types: 0=DEVICE_LOCAL, 1=DL|HOST_VISIBLE|COHERENT, 2=DL|HV|COHERENT|CACHED.
Queues: graphics=transfer=family 0 index 0; sparse VK_QUEUE_FAMILY_IGNORED.

## File layout (src/d3d9fe/)

- `d9mt_backend.h` — shared: winemetal includes, log, globals
  (g_mtlDevice, g_queue), VkFormat→WMT format table decl, helpers.
- `d9mt_instance.cpp` — DxvkInstance, DxvkAdapter, DxvkDeviceCapabilities,
  format features/limits tables.
- `d9mt_device.cpp` — DxvkDevice ctor/dtor/factories/waits/present,
  DxvkSubmissionQueue (threadless), DxvkObjects member ctors
  (DescriptorProperties, SamplerPool, PipelineManager minimal,
  GpuEventPool, GpuQueryPool, UnboundResources trivial), DxvkStagingBuffer.
- `d9mt_resources.cpp` — MemoryAllocator, ResourceAllocation dtor/free,
  view maps, DxvkBuffer, DxvkBufferView, DxvkImage, DxvkImageView,
  DxvkSampler/SamplerPool, PagedResource dtor.
- `d9mt_context.cpp` — DxvkContext + DxvkCommandList (+ their CPU-side
  member classes: object/signal trackers, stat counters, barriers,
  query-manager/descriptor-worker/implicit-resolve shells).
- `d9mt_watcher.cpp` — completion watcher thread (liveness backbone).
- `d9mt_presenter.cpp` — Presenter (CAMetalLayer via winemetal),
  DxvkSwapchainBlitter.
- `d9mt_queries.cpp` — DxvkQuery/DxvkEvent/pools.
Stubs remain in stubs.cpp for anything not yet implemented; REMOVE a
symbol from stubs.cpp when implementing it (duplicate symbol = link error
tells you exactly what moved).

## Stage decisions: instance/adapter/capabilities (d9mt_instance.cpp)

1. **Logging**: backend logs to `d3d9fe.log` in the process cwd
   (`d9mt::logf` in d9mt_backend.h, std::mutex-guarded; mingw thread model
   is posix so std::mutex is fine). Stub hits additionally append the
   symbol name to `d3d9fe-stub.log` before abort (winewrapper swallows
   stderr).
2. **Globals**: lazy `d9mt::mtlDevice()` / `d9mt::mtlCommandQueue()`
   (magic statics) copy the init pattern of D9MTDevice::Init —
   WMTCopyAllDevices → first device retained; newCommandQueue(dev, 8).
3. **VkPhysicalDevice** = pointer to a private static byte
   (`d9mt::vkPhysicalDevice()`); non-null, stable, NEVER dereferenced.
4. **DxvkInstance**: `m_config = Config::getUserConfig()` then
   `merge(Config::getAppConfig(env::getExePath()))` (merge keeps existing
   keys → user config wins, upstream semantics). `m_options` stays
   default-constructed — dxvk_options.cpp is NOT built (Vulkan-coupled);
   defaults are correct for us (latencySleep Auto etc.). The Vulkan-import
   ctor (non-null instance/loaderProc) throws DxvkError (fail loud).
   `DxvkInstance::handle()` would null-deref m_vki — interop-only, never
   called by d3d9.
5. **deviceName** = real `MTLDevice_name` + " (d9mt)" suffix (NOT the
   literal "Apple M1 Max" from the caps table — games match vendor/device
   IDs 0x106b/0xa1, the name is cosmetic and should be truthful per-host).
6. **Format table** (`d9mt::lookupFormatCaps`, d9mt_instance.cpp):
   - depth unified: D16_UNORM / D32_SFLOAT / D32_SFLOAT_S8_UINT all →
     WMTPixelFormatDepth32Float_Stencil8; D24_UNORM_S8 + D16_UNORM_S8
     reported UNSUPPORTED so D3D9VkFormatTable's own fallback picks
     D32_SFLOAT_S8_UINT (it logs the remap at device init).
   - video formats (G8B8G8R8_422 / B8G8R8G8_422 / 2-3 plane 420) and
     R4G4 (A4L4): unsupported until the FormatHelper compute path lands —
     CheckDeviceFormat returns NOTAVAILABLE, apps fall back.
   - 32-bit float formats: no SAMPLED_IMAGE_FILTER_LINEAR, no
     ATTACHMENT_BLEND (pre-M3 Apple GPUs support neither).
   - BC: sampled+filtered only, TRANSFER but no BLIT bits (Metal cannot
     blit/render BC); ⇒ AUTOGENMIPMAP on DXT correctly yields
     D3DOK_NOAUTOGEN.
   - linear tiling features = optimal minus attachment/storage bits;
     depth formats report linear = 0.
   - A4R4G4B4 → ABGR4Unorm and R5G6B5 → B5G6R5Unorm carry view-level
     component swizzles (applied at image-view creation, same trick as
     the hand-rolled driver / winemetal's synthetic BGRA4).
   - UINT alias formats R32G32_UINT / R32G32B32A32_UINT included for the
     BC-block clearImageView path.
7. **getFormatLimits**: rejects shared-handle queries (Logger::err +
   nullopt); MSAA counts 1|2|4|8 only for 2D/OPTIMAL/non-cube formats
   with attachment features. NOTE the caps table advertises 8x but M1
   Metal caps color samples at 4 — revisit when MSAA RTs are actually
   created (CheckDeviceMultiSampleType may over-promise 8x).
8. **DxvkDeviceCapabilities ctor** fills everything per the caps table
   above; additionally vk13 dynamicRendering/synchronization2/
   maintenance4 = true (DXVK baseline, read nowhere in d3d9),
   extRobustness2 alignments storage=4 / uniform=16,
   vk11.deviceLUIDValid = false (GetAdapterLUID falls back to ordinal
   LUID). m_featuresSupported == m_featuresEnabled.
9. **DxvkAdapter::createDevice() is still a stub** — next stage wires the
   DxvkDevice member cascade. vki() logs err + returns nullptr (interop).
9b. **wsi::init()/wsi::quit() in DxvkInstance ctor/dtor** (added during the
   context stage): upstream initializes the WSI driver there and the
   vendored wsi_platform.cpp dispatches every wsi::* call through the
   s_driver pointer — without init the FIRST swapchain creation
   (UpdatePresentRegion -> wsi::getWindowSize) null-derefs. Found via
   winedbg backtrace (disable the bottle crash dialog with
   HKCU\Software\Wine\WineDbg ShowCrashDialog=0 to get text backtraces;
   i686-w64-mingw32-nm + module offset maps the faulting symbol).
10. **build-dxvkfe.sh** now compiles all of src/d3d9fe/*.cpp, links
    `-lwinemetal` (prebuilt/) + `-ld9mtmetal32` (build/d9mtmetal/) exactly
    like scripts/build.sh, and force-recompiles backend TUs when
    d9mt_backend.h changes.

## Stage decisions: device (d9mt_device.cpp / d9mt_context.cpp / d9mt_watcher.cpp)

1. **Fake Vulkan device dispatch** (d9mt_device.cpp): vk::InstanceLoader/
   DeviceLoader/DeviceFn are constructed once in createDevice with dummy
   non-null handles. DeviceLoader::sym serves exactly 6 functions used by
   vendored dxvk_pipelayout.cpp (vkCreate/DestroyDescriptorSetLayout,
   vkCreate/DestroyDescriptorUpdateTemplate, vkCreate/DestroyPipelineLayout)
   returning unique u64 cookies; every other name resolves to an aborting
   trampoline (loud by design). vk::LibraryLoader dtor is real-empty (only
   referenced via Rc); LibraryFn/InstanceFn dtors stay aborting stubs.
2. **createDevice** throws DxvkError if mtlDevice()/mtlCommandQueue() are
   null; queues.graphics = queues.transfer = {fake VkQueue, family 0, idx 0}.
3. **DxvkDevice ctor cascade**: m_debugFlags 0, m_perfHints only
   preferRenderPassOps=TRUE, m_options from instance, m_objects(this),
   m_submissionQueue threadless. Dtor = waitForIdle() (watcher drain) only.
4. **Completion watcher** (d9mt_watcher.cpp, d9mt::watchCommandBuffer /
   watcherWaitIdle): ONE process-global dxvk::thread waits on watched
   MTLCommandBuffers FIFO (single MTLCommandQueue ⇒ retirement order ==
   submission order) via MTLCommandBuffer_waitUntilCompleted, then runs the
   registered callback; cmdbuf==0 entries are pure callbacks (empty
   submissions still signal, §7 risk 6). watch() retains the cmdbuf, the
   watcher releases it. The watcher object is intentionally LEAKED (never
   joined): joining from static dtors during Wine PE teardown hangs.
   waitForIdle == watcherWaitIdle (queue empty && not busy).
5. **Threadless policy everywhere**: DxvkSubmissionQueue spawns no workers
   (synchronize() no-op, lock/unlockDeviceQueue = mutex + queueCallback);
   DxvkPipelineWorkers spawns none (registerShader/requestCompileShader
   no-ops); DxvkDescriptorCopyWorker spawns none (fences exist, append ==
   consume). NB DxvkShader::needsLibraryCompile() CAN return true (vendored
   dxvk_shader.cpp sets it from canUsePipelineLibrary) — harmless: the only
   d3d9 use (d3d9_device.cpp BindShader) just calls requestCompileShader.
6. **presentImage** (device side): synchronous on calling thread —
   presenter->presentImage(frameId, tracker), store status, then
   signalFrame UNCONDITIONALLY (also on failure) per §5.1. Presenter
   methods themselves are Present-stage stubs.
7. **waits**: waitForSubmission spin-yields on the status atomic;
   waitForFence = sync::Fence::wait + GpuSync stats; waitForResource polls
   isInUse with yield (use-counts dropped by watcher callbacks).
8. **DxvkDescriptorProperties**: every descriptor type {size 8, align 8}
   (u64 argument-buffer slots), set alignment 16. Descriptor-buffer paths
   are never exercised; sizes only keep size math meaningful.
9. **MemoryAllocator**: ctor fills m_memTypes/m_memHeaps from adapter
   memoryProperties (budget = heap size); getMemoryTypeMask/
   createAllocationCache/stats real; allocation paths are Resources stage.
   DxvkLocalAllocationCache::freeCache logs err + leaks if it ever holds
   anything pre-Resources (cannot happen; loud guard).
10. **SamplerPool**: full upstream LRU-recycling semantics (dedupe by key,
    resurrect from LRU under one mutex, release() re-checks refcount under
    lock). DxvkSampler ctor creates the MTLSamplerState directly
    (support_argument_buffers=true) and publishes gpu_resource_id into
    d9mt::samplerHeapData()[index] (2048-entry u64 shadow array == set-15
    sampler heap, uploaded by the Draw stage); dtor zeroes the slot +
    releases. Border colors snap to Metal's 3 fixed colors (nearest by
    L2 distance); **sampler LOD bias is DROPPED** (no Metal equivalent;
    known visual deviation — shader-level bias would be a Draw-stage fix).
    VkCompareOp values map 1:1 onto WMTCompareFunction. Pool exhaustion
    returns nullptr after Logger::err (front-end EnsureSamplerLimit spins
    on getStats().liveCount before that can happen).
    DxvkSamplerDescriptorHeap is a metadata shell (no Vulkan pool/set;
    getDescriptorSetInfo returns null handles, consumed only by the fake
    dispatch).
11. **PipelineManager**: owns only the CPU-side descriptor/pipeline layout
    caches (vendored dxvk_pipelayout.cpp objects over the fake dispatch),
    cached per key under m_mutex with stable addresses (node-based map).
    createBuiltInPipelineLayout follows upstream exactly (merged layout,
    push data block, stage mask from bindings).
12. **createBuiltInComputePipeline returns VK_NULL_HANDLE + Logger::err**
    (SPIR-V→MSL is Draw-stage; only consumer is D3D9FormatHelper whose
    video formats the adapter reports unsupported ⇒ unreachable).
13. **GpuEventPool/GpuQueryPool**: real alloc/recycle machinery (free-list
    pools, fake u64 query-pool cookies so (pool,index) pairs stay unique);
    VkEvent/VkQueryPool handles stay null — completion is watcher-based
    (Queries stage). DxvkQuery::getData / DxvkEvent::test remain stubs.
14. **UnboundResources**: ctor/dtor only; nullDescriptor=true means the
    dummies are unreachable until Resources stage.
15. **DxvkCommandList/DxvkContext**: full ctor cascade so createContext()/
    createCommandList() return real objects: ObjectTracker (1024-entry list
    chain, clear() runs virtual releases, advanceList real), SignalTracker,
    StatCounters, BarrierBatch/Tracker (empty; Metal uses pass splits),
    GpuQueryManager shell, ImplicitResolveTracker shell,
    DxvkFramebufferInfo default ctor (no attachments). All 57 context
    methods + bindResources remain loud stubs (Context stage).
16. **Never-constructed Vulkan-only classes** (CommandPool, Compute/
    GraphicsPipeline + GPL libraries, DescriptorPool/PoolSet, DxvkFence,
    Meta*Objects, ResourceDescriptorHeap, SparseBindSubmission): their
    dtors are referenced by dtor chains of real objects but can never run;
    they stay aborting stubs in stubs.cpp on purpose.

## Stage decisions: resources (d9mt_resources.cpp)

1. **Dedicated objects, no suballocation.** createBufferResource ignores
   the allocation cache and creates one MTLBuffer per allocation;
   createImageResource creates one MTLTexture. KNOWN PERF RISK (§7.5):
   DISCARD renaming allocates a fresh 64 KiB VirtualAlloc + MTLBuffer per
   rename — a recycling suballocator is the first perf backlog item.
2. **ALL buffers are shared-storage** regardless of requested memory
   properties: VirtualAlloc'ed (64 KiB-granular, zero-filled) 32-bit client
   memory wrapped via MTLDevice_newBuffer bytes-no-copy (same pattern as
   the hand-rolled driver). Every buffer therefore has a valid mapPtr;
   memFlags()/getMemoryProperties() still echo the REQUESTED type so
   front-end branching (HOST_VISIBLE memset vs ctx->initBuffer) is
   unchanged. m_bufferAddress = real MTLBuffer.gpuAddress. This also means
   buffer-texture views can hard-code Shared storage options.
3. **Images are always private storage.** Permissive usage at creation:
   ShaderRead always; RenderTarget whenever the format supports attachment
   use (independent of requested usage — blit/mipgen passes need it);
   ShaderWrite for STORAGE; PixelFormatView for plain color formats
   (sampled views routinely carry swizzles, e.g. X8R8G8B8 alpha-one) but
   NOT for depth, BC or MSAA. ensureImageCompatibility can return true
   unconditionally (Context stage). Lossless-compression perf impact of
   blanket PixelFormatView accepted for bring-up.
4. **Texture side table** (`textureSideMap`, process-global, mutex-guarded,
   keyed by MTLTexture handle): vendored view-map classes have fixed
   Vulkan-shaped members, so view creation looks up Metal texture type /
   sample count there. Filled by createImageResource/importImageResource,
   erased in ~DxvkResourceAllocation.
5. **Allocation bookkeeping**: m_address = 0 (dedicated; getMemoryInfo
   offset 0), m_type points into m_memTypes (first type whose flags are a
   superset of the request, fallback type 0), stats add/sub the aligned
   VirtualAlloc size for buffers and a tight linear-size estimate for
   images (winemetal exposes no real allocation sizes). OwnsMemory ⇒ dtor
   VirtualFrees mapPtr; OwnsBuffer/OwnsImage ⇒ dtor NSObject_releases the
   handle. Imports retain the foreign handle (OwnsImage|Imported) so the
   reference is owned even though the object is not.
6. **Image views**: MTLTexture_newTextureView per DxvkImageViewKey, cached
   in DxvkResourceImageViewMap. descriptor->legacy.image.imageView = view
   handle, descriptor word = view gpu_resource_id. STENCIL-only aspect ⇒
   WMTPixelFormatX32_Stencil8 alias. View swizzle = compose(format base
   swizzle, key.packedSwizzle); base swizzles: VK A4R4G4B4→(G,B,A,R) over
   ABGR4Unorm, VK B5G6R5→(B,G,R,A) over B5G6R5Unorm. NB the instance
   format TABLE comment has R5G6B5/B5G6R5 swapped — verified against the
   working driver: VK R5G6B5 (the d3d9 mapping of D3DFMT_R5G6B5) is
   bit-identical to Metal B5G6R5Unorm ⇒ identity. Depth/MSAA views force
   identity swizzle (Metal restriction; loud if a swizzle is dropped).
   2D(-array) views of 3D images: impossible on Metal — Logger::err +
   nullptr from DxvkImageView::createView (upstream returns the Vulkan
   view; volume blit views become a Draw-stage problem if ever hit).
   Swizzled formats (A4R4G4B4 etc.) as RENDER TARGETS would write wrong
   channels (RTVs are created swizzle-less per upstream) — known
   deviation, d3d9 apps rendering to 4444/565 are rare.
7. **Buffer views**: raw views (format UNDEFINED) fill legacy.buffer
   {handle, offset, range} + descriptor word = gpuAddress+offset, no Metal
   object. Formatted views create a WMTTextureTypeTextureBuffer view via
   MTLBuffer_newTexture (consumer: FormatHelper texel buffers — currently
   unreachable since video formats are unsupported). View-map failures log
   + return a zeroed descriptor (loud, non-fatal).
8. **Cube images** are created as WMTTextureTypeCube/CubeArray
   (array_length = layers/6) when CUBE_COMPATIBLE && layers%6==0; per-face
   RTVs work as 2D views with slice = face. MSAA ⇒ 2DMultisample(Array),
   mips forced 1 by the front-end.
9. **DxvkBuffer/DxvkImage/DxvkImageView/DxvkStagingBuffer mirror upstream
   v2.7.1 sources** (/tmp/dxvk-src) line-for-line where Vulkan-free:
   uninitialized-subresource tracking, assignStorageWithUsage (view-format
   merge + ++m_version + residency), view-type legalization in
   DxvkImageView::createView, morton-code getTrackingAddress, staging ring
   (per-chunk DxvkBuffer renaming, dedicated path for >half-size allocs).
   Debug names are disabled outright (m_info.debugName = nullptr in ctors
   ⇒ setDebugName/updateDebugName are no-ops; keeps assignStorage hot path
   free of label churn).
10. **canRelocate() == false** for images, and for buffers anything mapped
    (i.e. everything — all buffers have mapPtr) ⇒ the front-end never sees
    relocatable resources; relocateStorage/requestMakeResident are
    loud-unreachable; performTimedTasks is a no-op (nothing to defrag or
    trim); registerResource/unregisterResource maintain the cookie map.
11. **Memory requirement queries**: buffers {size align 256, all types},
    images {tight size align 64K, DEVICE_LOCAL types}; dedicated-
    requirements chain reports prefers=TRUE/requires=FALSE.
12. **Not implemented (unreferenced by the d3d9 closure, loud if that ever
    changes)**: DxvkMemoryAllocator::allocateMemory/allocateDedicatedMemory/
    createSparsePage, DxvkSharedAllocationCache, DxvkLocalAllocationCache
    statics, DxvkRelocationList::poll/addResource/clear, page/pool
    allocator algorithms — none are referenced once createBufferResource
    bypasses the cache; the link is the watchdog.

## Stage decisions: context part 1 (d9mt_context.cpp)

1. **Command-list side state** (`d9mt::cmdListState`, process-global
   mutex-guarded map keyed by DxvkCommandList pointer): one lazily-created
   MTLCommandBuffer per list + encoder state machine (None/Render/Blit/
   Compute; switching kinds ends the previous encoder) + per-submission
   completion-work vector (EVENT query flips). The vendored class has no
   usable members for Metal handles. A single entry is never used
   concurrently: the CS thread is done with a list before the watcher
   touches it; reset() removes+releases the entry.
2. **Immediate encoding, no command arena.** winemetal encodeCommands is
   synchronous, so wmtcmd structs live on the stack of each context
   method; setBytes data is copied by Metal at encode time.
3. **Submission path**: DxvkDevice::submitCommandList lives in
   d9mt_context.cpp (welded to the side state). It merges stat counters,
   commits the MTLCommandBuffer on the calling (CS) thread, sets the
   status atomic to VK_SUCCESS immediately (waitForSubmission semantics),
   and registers a watcher callback that runs, in order: per-submission
   completion work (EVENT flips) → cmd->notifyObjects() (object-tracker
   clear = resource use-count release + signal notify) → device->
   recycleCommandList (reset() drops the Metal side state). EMPTY
   submissions still signal via the watcher's cmdbuf==0 pure-callback
   path (§7 risk 6) — flushCommandList never loses its signals.
4. **No Vulkan barriers anywhere**: emitGraphicsBarrier == spillRenderPass
   (render-encoder break); ordering within a queue comes from Metal
   encoder boundaries + automatic hazard tracking. BarrierBatch/Tracker
   members stay valid-and-empty.
5. **Clears**: clearRenderTarget defers (upstream deferClear/deferDiscard
   merge semantics, inverse-swizzle on clear colors because RTVs are
   swizzle-less); flushClears executes each as a STANDALONE render pass
   whose load actions do the work (store=Store, discard=DontCare).
   Unified-depth rule: every depth clear pass binds BOTH depth and
   stencil attachments (all depth formats are Depth32Float_Stencil8);
   the untouched aspect uses load=Load. Draw stage will merge pending
   clears into real passes instead.
6. **clearImageView**: full-view → performClear; partial → clear a temp
   image via render pass then blit-copy the rect in (avoids per-format
   CPU packing). Fail-loud: BC-alias clears (needs compute writeback,
   §7 risk 7), partial MSAA clears, partial single-aspect DS clears.
7. **Buffer/image copies**: blit encoder. Buffer<->image copies honor
   rowAlignment/sliceAlignment exactly like upstream (align only when
   alignment > elementSize); depth aspect uses
   WMTBlitOptionDepthFromDepthStencil and REQUIRES D32_SFLOAT buffer
   data (D16/packed D24S8 conversion fail loud, §7 risk 3); stencil
   aspect = 1 byte + StencilFromDepthStencil. copyImageToBuffer of
   depth-stencil images fails loud (winemetal texture-to-buffer has no
   blit-option field). copyImage requires identical WMT formats and
   equal sample counts; format-converting copies fail loud until the
   Draw stage's sample pass. blitImageView degenerates to copyImage for
   1:1 same-format blits, else fail-loud (Draw stage).
8. **initBuffer = blit fillBuffer 0; initImage**: renderable formats are
   cleared to zero via load-action passes (one per mip; 2D arrays in one
   pass via render_target_array_length, 3D one pass per depth plane);
   non-renderable (BC) formats copy from the context zero buffer
   (createZeroBuffer needs no GPU fill: backend buffers are zero-filled
   VirtualAlloc shared storage and the zero buffer is never written).
   PREINITIALIZED layout = tracking-only (mapped uploads).
9. **invalidateBuffer / createZeroBuffer / freeZeroBuffer / setters**:
   mirrored from upstream v2.7.1 line-for-line (dirty flags + PSO-key
   shadow into m_state; Draw stage consumes them). bindShader/bind*/
   pushData/setSpecConstant/synchronizeWsi are header-inline already.
10. **ensureImageCompatibility**: images are created with permissive
    Metal usage (resources decision 3) so storage never changes; merge
    requested metadata via assignStorageWithUsage(storage(), usageInfo)
    and return true.
11. **changeImageLayout/transformImage**: metadata no-ops (setLayout +
    rt-layout shadow update / pending-clear ordering only).
12. **DxvkCommandList::bindResources** (built-in compute pipelines only):
    descriptor i of image/texel-buffer type → compute setTexture slot i,
    buffer type → setBuffer slot i, push constants → setBytes buffer
    slot 30. MUST match the Draw stage's SPIRV-Cross MSL resource-binding
    map when createBuiltInComputePipeline becomes real.
13. **EVENT queries work end-to-end**: signalGpuEvent sets the event to
    VK_EVENT_RESET (Pending) and queues completion work that flips it to
    VK_EVENT_SET on the watcher thread; DxvkEvent::test reads the status
    under the spinlock. Occlusion/timestamp queries
    (beginQuery/endQuery/writeTimestamp) log once + no-op until the
    Queries stage (visibility buffer); DxvkQuery::getData stays a stub.
14. **Latency tracking**: createLatencyTracker returns nullptr on this
    backend, so begin/endLatencyTracking keep upstream bookkeeping but
    are no-ops in practice; debug labels are silent no-ops (no Metal
    debug groups for now).
15. **generateMipmaps** uses the blit encoder's generateMipmaps for the
    WHOLE image (winemetal exposes no per-range variant); the VkFilter
    point/linear distinction is lost (matches the hand-rolled driver).
16. **Still stubbed in stubs.cpp after this stage**: DxvkContext::draw/
    drawIndexed (Draw stage: PSO build + AB writes + render encoder),
    DxvkQuery::getData (Queries stage), Presenter/SwapchainBlitter/Hud
    (Present stage), never-constructed Vulkan-only dtors.
17. **Runtime status (verified with triangle.exe in the isolated bottle
    dir drive_c/d9mtfe-test)**: device + context + initial CS chunk
    (beginRecording, initial state setters) execute cleanly; the process
    now aborts deterministically at the Presenter ctor stub inside
    D3D9SwapChainEx::UpdateWindowCtx -> CreatePresenter. That required
    the wsi::init() fix (instance decision 9b). Out-of-line DxvkContext
    methods NOT implemented (updateBuffer, uploadBuffer/Image, dispatch*,
    draw*Indirect*, clearBuffer(View), copyBufferRegion/copyImageRegion,
    copyPackedBufferImage, discardImage, emitBuffer/ImageBarrier,
    invalidateImage(WithUsage), ensureBufferAddress, wait/signalFence,
    setBarrierControl, updatePageTable, launchCuKernelNVX, initSparseImage,
    copySparsePages*, setDebugName, DxvkCommandList::next/submit) are
    LINK-PROVEN unreferenced by the d3d9 closure (no stub, no definition;
    PE ld would fail otherwise).

## Stage decisions: presenter + blitter (d9mt_presenter.cpp)

1. **HWND channel = the surface proc, made safe.** The CreatePresenter
   lambda is the ONLY verbatim-compliant carrier of the HWND
   (`wsi::createSurface(cWindow, vki->getLoaderProc(), vki->instance(), ...)`).
   Instead of bypassing it, DxvkAdapter::vki() now returns a REAL
   Rc<vk::InstanceFn> over a LibraryLoader carrying
   `d9mt::fakeGetInstanceProcAddr` (d9mt_instance.cpp), which serves exactly
   vkCreateWin32SurfaceKHR (smuggles `*pSurface := (u64)hwnd`) and
   vkDestroySurfaceKHR (no-op); every other name resolves to nullptr (wsi
   handles that as FEATURE_NOT_PRESENT). InstanceFn's ~60 member pointers
   resolve through InstanceLoader::sym = aborting trampoline — interop
   stays loudly unsupported. New real symbols: vk::LibraryLoader(PFN ctor),
   vk::InstanceFn ctor/dtor (stub removed). So Presenter::m_surface == HWND.
   (This supersedes the earlier "never call the surface proc" guidance:
   calling it is safe by construction now, and was runtime-verified.)
2. **Presenter side state** (`d9mt::presenterState`, mutex-guarded map keyed
   by Presenter pointer): hwnd, metal view (ReleaseMetalView to free), layer
   (owned by the view, not retained), proxy Rc<DxvkImage> + extent, plus a
   per-presenter mutex ordering the app thread (acquire/destroyResources)
   against the CS thread (presentImage). Vendored members used for the rest:
   m_surface (smuggled HWND), m_preferredExtent/Format/SyncInterval,
   m_dirtySwapchain/m_dirtySurface (app-thread-only), m_fpsLimiter,
   m_lastSignaled.
3. **Proxy design (acquire never touches drawables):** acquireNextImage
   returns a B8G8R8A8_UNORM private DxvkImage (usage COLOR_ATTACHMENT |
   SAMPLED | TRANSFER_SRC/DST, layout GENERAL, SRGB_NONLINEAR colorSpace)
   created via device->createImage; d3d9 blits its backbuffer INTO the proxy
   (blitter), presentImage blits proxy -> CAMetalDrawable. Proxy and layer
   drawable size are updated together under the presenter lock, so the
   present-side copy never needs clamping. Fresh proxies get a watched
   zero-clear pass (black borders for letterboxed dstRects). PresenterSync
   stays all-null (synchronizeWsi stores it, nothing reads it).
4. **Layer setup mirrors the hand-rolled driver:** CreateMetalViewFromHWND
   (app thread), props {device, contents_scale 1.0, drawable size = proxy
   extent, opaque, display_sync_enabled = (syncInterval != 0),
   **framebuffer_only=false (REQUIRED — drawable is a blit target)**,
   pixel_format BGRA8Unorm}. setSyncInterval clamps >1 to 1 (FIFO),
   marks the swapchain dirty only on change (it is called every Present).
   Non-BGRA8-able preferred formats (10-bit etc.) log a precision warning
   and go through the B8G8R8A8 proxy anyway.
5. **presentImage (CS thread)** creates its OWN command buffer on the global
   queue (the frame's work was already committed by flushCommandList; one
   queue ⇒ ordered): nextDrawable -> blit proxy->drawable ->
   presentDrawable -> commit -> `watchCommandBuffer(cmdbuf, signalFrame)`.
   **signalFrame fires on EVERY path**: failures (no layer/drawable/cmdbuf)
   route through the watcher's cmdbuf==0 pure-callback entry, which runs
   after all previously watched work — late but ordered and monotonic.
   The watcher callback holds Rc<Presenter> + Rc proxy, so neither dies
   with a present in flight. DxvkDevice::presentImage no longer calls
   signalFrame itself (that would release SyncFrameLatency before the GPU
   finished); it only stores the status + bumps stats.
6. **signalFrame runs on the watcher thread**: fpsLimiter.delay() (vendored
   FpsLimiter; util_fps_limiter.cpp+util_sleep.cpp already compiled) ->
   tracker->notifyGpuPresentEnd (tracker is always null on this backend) ->
   m_signal->signal(frameId). Presents retire in submission order ⇒
   monotonic per presenter.
7. **destroyResources = watcherWaitIdle() + drop proxy + ReleaseMetalView**
   ("blocks until pending swapchain ops complete" contract); dtor calls it
   then erases the side state. invalidateSurface sets m_dirtySurface;
   the next acquire rebuilds surface + swapchain (deferSurfaceCreation
   simply skips the ctor-time createSurface()).
8. **Blitter present = fullscreen-triangle sample pass** into dstView
   (loadAction: DontCare when dstRect covers the target, else Clear black =
   letterbox), drawing via the d9mt_context encoder bridge
   (`cmdListBeginRenderPass`/`cmdListGetBlitEncoder`/`cmdListEndEncoder`,
   non-static wrappers over the command-list side state, declared in
   d9mt_backend.h). MSL compiled at runtime through d9mtmetal
   NEW_LIBRARY_FROM_SOURCE (winemetal has no source entry point); PSO per
   dst WMTPixelFormat in a process-global leaked cache;
   MTLDevice_newRenderPipelineState exactly like the hand-rolled GetBlitPso
   (no depth/stencil format, max_tessellation_factor 16). srcRect maps to
   uv via SetFragmentBytes {uv_offset, uv_scale} (buffer 0); sampler is a
   constexpr linear/clamp MSL sampler (no MTLSamplerState object). Viewport
   = dstRect does the scaling. Fast path: blit-encoder copy when same WMT
   format + equal rect sizes + **no packed swizzle on either view** (Metal
   cannot blit swizzled texture views; X8R8G8B8 sample views carry RGB1).
   Both images tracked on the command list (src Read, dst Write).
9. **Blitter unimplemented-composition policy (fail loud, keep presenting):**
   MSAA present sources skip the blit with one err; non-identity gamma ramps
   (identity detected with ±256/65535 tolerance ⇒ treated as disabled, the
   windowed path always sends identity/0) and software-cursor composition
   log one err and are ignored. m_gammaCpCount + a small side struct
   (cursor-set + warn-once flags) under the vendored m_mutex
   (setCursorPos is CS-thread, the rest app-thread — §5.2).
10. **hud::Hud::createHud returns nullptr** (upstream does this when no HUD
   elements are enabled; all front-end uses are null-guarded). Every other
   Hud/HudItem/HudRenderer symbol is now an unreachable dtor-chain stub.
11. **vkDestroyPipeline added to the fake DEVICE dispatch** (no-op + err on
   non-null): ~D3D9FormatHelper destroys its 8 built-in compute pipelines
   through vkd() at device teardown — with createBuiltInComputePipeline
   returning VK_NULL_HANDLE this aborted via the trampoline. When the Draw
   stage makes built-in pipelines real, it must take over this release path.
12. **Runtime verified** (CrossOver bottle, drive_c/d9mtfe-test,
   WINEDLLOVERRIDES=d3d9=n): triangle.exe now reaches the
   DxvkContext::draw stub (Draw stage is next); a clear-only variant
   (Clear+Present loop, no draws) presents 30 vsynced frames with zero
   stub hits and exits cleanly through device teardown — §5.1 liveness
   (acquire -> blitter pass -> flush -> present -> watcher signalFrame)
   holds end-to-end, including the maxLatency window (would deadlock at
   frame ~4 if frame signals were lost).

## Stage decisions: draw (d9mt_context.cpp part 2 + d9mt_shader.cpp + d9mt_draw.h)

1. **Shader translation** (d9mt_shader.cpp, decls in d9mt_draw.h): per
   (DxvkShader*, DxvkShaderModuleCreateInfo) cache —
   `shader->getCode(nullptr, moduleInfo)` (nullptr binding map keeps the
   SHADER-DEFINED set/binding ids = DXVK slot ids and shader-defined push
   offsets; moduleInfo still applies undefined-input elimination, RT output
   swizzles, flat-shading decorations) → SPIRV-Cross CompilerMSL (msl 3.0,
   argument buffers tier 2, all-automatic bindings, same flags as
   d9mt_translate.cpp) → d9mtmetal NEW_LIBRARY_FROM_SOURCE → "main0".
   **Undefined-input elimination is LOAD-BEARING on Metal**: an FS stage_in
   input with no matching VS output fails PSO creation
   (`undefinedInputs = fs.inputMask & ~vs.outputMask`).
2. **Reflection by SPIR-V decorations, not dxso names** (works for the FF
   generator too): set-0 resources keyed by their Binding decoration
   (= slot id), AB dword = `get_automatic_msl_resource_binding` ([[id]] in
   the tier-2 AB; **-1 = dead resource, MUST be skipped** — first run wrote
   ab[0xFFFF] out of bounds). Set-0 AB itself is `[[buffer(0)]]` (SPIRV-Cross
   binds AB N at buffer N), push block + set-15 sampler heap at their
   reflected automatic indices (typically 1/2). Defensive
   `abId < abEntryCount` check at AB-write time.
   Sampler/push metadata comes from `shader->getLayout()`:
   SAMPLER descriptors give {slot, blockOffset}; push blocks give
   {shader-space dstOffset, size, resourceMask} with the constantData source
   offset recomputed via the upstream block-index formula (shared block 0 →
   0, per-stage local → 64+32·(index−1)).
3. **Function constants**: cached per spec-value vector inside the compiled
   shader; ALL declared constants supplied (ids < MaxNumSpecConstants(12)
   from m_state sc data, gate id 12 = true, others = SPIR-V default), typed
   per reflection (WMTDataTypeBool for OpSpecConstantTrue/False gates, UInt
   otherwise — supplying UInt for a bool constant fails newFunction).
4. **PSO cache** (process-global, leaked, Rc-held shaders): key =
   {vs, fs, FULL DxvkGraphicsPipelineStateInfo} with the ACTUAL vertex
   strides written into ilBindings (front-end uses dynamic strides = 0).
   That covers §4.7 exactly (ia/il/rs/ms/om/rt/sc/omSwizzle/omBlend +
   strides). omSwizzle is filled at updateRenderTargets from each RTV's
   unpackSwizzle() and feeds BOTH the PSO key and the FS output-swizzle
   patch — swizzled-format RTs now render correctly through swizzle-less
   RTVs (closes the resources-stage deviation FOR DRAWS; blitImageView
   destinations still deviate, warn-once). PSO build failures are cached
   (pso==0) so a broken state fails loud once, not per draw.
5. **Vertex descriptor**: metal buffer = `14 + binding` (d9mt_draw.h
   VertexBufferBase; §4.2's "16+binding" would exceed Metal's 31-slot
   buffer table for bindings 15/16; 14..30 fits all 17 d3d9 bindings and
   stays clear of the reflected AB/push/heap indices 0..2).
   USCALED/SSCALED → Metal unnormalized integer formats (UChar4/Short2/4):
   Metal converts integer attributes to float shader types numerically,
   which IS the scaled semantic (§7 risk 1 resolved; UDEC3/A2B10G10R10_
   USCALED has no Metal equivalent — PSO fails loud). D3DCOLOR →
   UChar4Normalized_BGRA. Stride-0 bindings (incl. null stream 16): Metal
   validation rejects stride 0, so stride = max attribute extent (aligned 4)
   with stepFunction Constant + **stepRate 0** (required `// d9mt:`-free fix
   in OUR src/d9mtmetal/unix.m: don't force stepRate 1 for Constant; rebuilt
   + reinstalled via tools/build-d9mtmetal.sh). Unbound-but-referenced
   bindings bind the context zero buffer (nullDescriptor semantics: reads 0).
   Instance rate divisor>0 → PerInstance/stepRate=divisor; divisor==0 →
   Constant.
6. **Render pass lifecycle**: passes start LAZILY at draw commit
   (commitGraphicsState) and restart whenever the encoder kind changed,
   render targets changed, or **m_deferredClears is non-empty** (a mid-pass
   Clear() must become load actions of a NEW pass to stay ordered —
   upstream's vkCmdClearAttachments path has no Metal equivalent).
   startRenderPass partitions pending clears: entries matching a bound
   attachment (matchesView or same image+subresource) hoist into
   load_action Clear/DontCare; the rest flush standalone first. Unified-DS
   rule from the clears stage holds (both planes always attached).
   spillRenderPass now also clears GpRenderPassBound. Encoder restart sets
   all encoder-scoped dirty bits (viewport/raster/blend/stencil/dsso/vertex
   buffers/push) + dirtyStages(ALL_GRAPHICS); per-encoder dedupe state
   (lastRenderPso/lastRenderDsso/renderResident) lives in CmdListState.
7. **Binding flush** (updateGraphicsShaderResources), per stage: AB u64
   words written into a per-context DxvkStagingBuffer ring (4 MiB chunks;
   chunks stay alive via cmd-list tracking; KNOWN PERF: a new chunk every
   ~4 frames under load, same class as the DISCARD-rename backlog item) —
   uniform/storage-with-UniformBuffer-flag slots → gpuAddress+offset from
   m_uniformBuffers[slot], sampled/storage images → the descriptor word the
   resources stage stored in DxvkDescriptor::descriptor[0..8) via
   view->getDescriptor() (default view type handles the spec-resolved
   VIEW_TYPE_MAX_ENUM sampler-type bindings). Push block: scratch built from
   m_state.pc.constantData per block (skipping resourceMask dwords), then
   sampler heap indices (DxvkSampler::getDescriptor().samplerIndex) written
   as u16 at each SAMPLER binding's shader-space blockOffset; uploaded via
   the ring (winemetal has SetFragmentBytes but NO SetVertexBytes, so
   buffer-bind keeps both stages uniform). Sampler heap = one process-global
   shared MTLBuffer wrapping the samplerHeapData() shadow (VirtualAlloc +
   bytes-no-copy; samplerHeapData/samplerHeapBuffer now out-of-line in
   d9mt_shader.cpp) bound directly at the reflected heap index — no per-draw
   heap uploads, sampler ctor writes are GPU-visible immediately.
   Residency: indirectly-referenced resources (cbuffer MTLBuffers, texture
   view objects) get per-encoder-deduped useResource(Read, Vertex|Fragment);
   directly-bound buffers (AB ring, push, heap, vertex/index) need none.
   Lifetime: m_cmd->track on every referenced buffer/image/sampler + ring
   chunks.
8. **Dynamic state**: viewport — the front-end emits VULKAN-convention
   flipped viewports (y+h, −h); Metal clip space matches D3D, so negative
   heights are UN-flipped back to the original D3D viewport (verified:
   corner readback matches; hand-rolled driver never flips either).
   Scissor clamped into the fb extent (Metal validates). Cull mode 1:1,
   winding CW→WMTWindingClockwise (both APIs screen-space). One rasterizer
   cmd carries fill/cull/winding/depth-clip/depth-bias. DSSO cache keyed on
   packed DxvkDepthStencilState + hasDepth + read-only aspects (read-only
   depth forces write off, read-only stencil forces write_mask 0; no DS
   attachment → always/no-write DSSO since Metal validates DSSO against
   pass attachments). VkCompareOp/VkStencilOp map 1:1. Stencil ref rides
   both setdsso and setblendcolor cmds. POINT fill → wireframe warn-once;
   sampleMask != 0xffff warn-once.
9. **Draws**: TRIANGLE_FAN emulated with synthesized u32 triangle-list
   indices from the ring (non-indexed: 0,t+1,t+2 + base_vertex; indexed:
   source indices fetched through the index buffer's persistent mapPtr —
   all buffers are host-visible on this backend). firstInstance →
   base_instance, vertexOffset → base_vertex, firstIndex → byte offset on
   the bound index slice. GEOMETRY shader bound (SWVP ProcessVertices) →
   warn-once + draw skipped (§7 risk 2 stance unchanged).
10. **blitImageView general path**: fullscreen-triangle sample pass (the
    presenter blit pipeline, now exported via d9mt_backend.h getBlitPso
    with a NEAREST variant) into an identity-swizzle COLOR_ATTACHMENT view
    of the destination subresource, sampling the source's 2D view (carries
    the view swizzle). Mirroring normalized into forward dst rect +
    negative uv scale. Depth/MSAA/3D/array blits fail loud.
11. **copyImage cross-format** (raw-bit Vulkan semantics): transient
    MTLTexture_newTextureView of the DESTINATION subresource in the
    source's WMT format (PixelFormatView usage from the resources stage),
    then plain blit copy; released after encode (cmdbuf retains). Depth/BC/
    MSAA/different-texel-size/3D aliasing fails loud.
12. **resolveImage**: color resolves via an empty render pass over the MSAA
    source with store_action StoreAndMultisampleResolve + resolve_texture =
    dst (source contents preserved). SAMPLE_ZERO color approximated as
    AVERAGE (warn-once). Depth-stencil resolves fail loud (winemetal depth
    attachments expose no resolve target) — ResolveZ/INTZ users will hit
    this. Partial-region resolves fail loud.
13. **Build**: build-dxvkfe.sh adds the 6 SPIRV-Cross TUs (same set as
    scripts/build.sh) and recompiles backend TUs when d9mt_draw.h changes.
14. **Runtime verified** (CrossOver bottle, drive_c/d9mtfe-test):
    `verifytri.exe` (new test/verifytri.c: draws the FF triangle, reads the
    backbuffer back via GetRenderTargetData→LockRect) prints
    `corner=101040 center=555555 PASS` — pixel-exact clear color + exact
    1/3·(R+G+B) interpolation at the centroid, proving clear → FF shader
    gen → SPIR-V→MSL → PSO → AB/push bindings → draw → readback end-to-end;
    triangle30 (30-frame triangle.c variant) runs clean (no stub hits, no
    validation errors); shadertri.exe (real SM3 dxso VS/PS) reaches
    "PASS: runtime SM3 -> SPIR-V -> MSL pipeline rendering"; texquad.exe
    reaches "PASS: texture + sampler path rendering" (sampler heap +
    texture AB words live: FS reflection heap=2 smp=1). All four re-run
    clean against a from-scratch rebuild (build-dxvkfe.sh + build.sh +
    build-host.sh all green) with zero stub hits and zero err/warn lines
    in d3d9fe.log.

## Liveness contracts (deadlock sources — BACKEND-SURFACE §5.1)

1. presenter->signalFrame(frameId) after each present.
2. ctx->signal(submissionFence, id) at submission completion.
3. staging fence likewise.
All wired through MTLCommandBuffer addCompletedHandler... NOTE winemetal
has no generic completion-callback export — poll MTLCommandBuffer_status
from a watcher thread, or use MTLSharedEvent. Decision: completion
watcher thread per device draining a queue of (cmdbuf, callbacks).
IMPLEMENTED in d9mt_watcher.cpp (one process-global thread, FIFO
waitUntilCompleted; see "Stage decisions: device" item 4).

## Status checklist

- [x] d9mt_backend.h + build script wiring
- [x] Instance/Adapter/Capabilities + format tables (createDevice still stubbed)
- [x] Device ctor cascade compiles (objects, queue threadless) + completion watcher
- [x] MemoryAllocator + Buffer/Image/Views/Sampler
- [x] Context skeleton: beginRecording/flush/signal + clears/copies
- [x] Presenter + blitter (CAMetalLayer + proxy; §5.1 liveness runtime-verified clear-only)
- [x] triangle.exe renders under d3d9fe.dll (verifytri.exe pixel readback PASS; shadertri SM3 PASS)
- [x] 5-test suite under d3d9fe.dll: triangle, shadertri, texquad, depthtri, extest all PASS
      (2026-06-12, isolated bottle dir drive_c/d9mtfe-test, WINEDLLOVERRIDES=d3d9=n; zero
      d3d9fe-stub.log hits, zero err:/warn: in d3d9fe.log across all five. depthtri exercises
      D24S8 auto depth-stencil + DSSO cache + two depth-tested draws; extest exercises
      Direct3DCreate9Ex/CreateDeviceEx/GetBackBuffer/Get+SetRenderTarget/DepthStencilSurface/
      Set+GetMaximumFrameLatency/CheckDeviceState/PresentEx. Suite runner:
      scripts/run-fe-suite.sh — launches each exe via DOS path, polls *_out.txt for PASS/FAIL,
      taskkills in-bottle, archives per-test logs as d3d9fe-<exe>.log. Tests render forever
      after PASS by design; the runner kills them.)
- [ ] Queries (DxvkQuery::getData + occlusion via Metal visibility-result buffer; timestamps),
      remaining context ops, backlog (DISCARD-rename suballocator, DS resolve, gamma LUT,
      software cursor, 8x MSAA over-promise, sampler LOD bias)
