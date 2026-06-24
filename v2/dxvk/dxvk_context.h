#pragma once

// DxvkContext shim: the command recorder on the critical path.
//
// Real DXVK's DxvkContext records hundreds of methods that build Vulkan command
// buffers. This shim implements ONLY the handful the D3D9 frontend records on
// the fixed-function-triangle path and turns them into one Metal render pass via
// the D9mtBackend. Everything else is a signature-matched no-op so the frontend
// links and runs unchanged. See SHIM_SPEC.md.
//
// Recorded path that actually reaches Metal:
//   beginRecording  -> reset per-frame state
//   bindRenderTargets / clearRenderTarget -> remember target + clear color
//   bindVertexBuffer(0, slice, stride)    -> remember Metal buffer + offset
//   setViewports                          -> stored (NDC triangle ignores it)
//   bindShader<Stage>                     -> IGNORED (backend owns the FF pipeline)
//   draw(count, VkDrawIndirectCommand*)   -> build D9mtDraw, renderAndPresent
//   flushCommandList / synchronizeWsi     -> no-op (present already happened)
//
// Signatures are copied EXACTLY from dxvk-ref/dxvk_context.h so the frontend's
// recorded lambdas bind to them. The bodies are gutted to the Metal path.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "dxvk_include.h"   // util layer + real Vk types/enums

#include "dxvk_backend.h"   // D9mtBackend, D9mtDraw
#include "dxvk_trace.h"     // per-frame CPU draw-path profiler (D9MT_TRACE)
#include "dxvk_shader_convert.h"         // MslShader (cached translation)
#include "dxvk_metal_shader_pipeline.h"  // MetalVertexLayout (PSO assembly)
#include "dxvk_buffer.h"    // DxvkBufferSlice (Metal buffer handle + offset)
#include "dxvk_constant_state.h"  // complete pipeline dynamic/constant-state types
#include "dxvk_dynamic_state.h"   // DxvkViewport + DxvkDynamicState (per-draw state)
#include "dxvk_gpu_query.h" // DxvkQuery / DxvkEvent stand-ins (begin/endQuery)
#include "dxvk_image.h"     // DxvkImage / DxvkImageView (backbuffer Metal texture)
#include "dxvk_sampler.h"   // DxvkSampler / DxvkSamplerKey (bound sampler state)
#include "dxvk_shader.h"    // DxvkShader (opaque, ignored)

namespace dxvk {

  class DxvkDevice;

  // Recorded command list (shim). The real DXVK builds Vulkan command buffers
  // here; the shim records nothing (DxvkContext drives Metal directly), so this
  // is an empty RcObject the frontend can hold in Rc<> and pass to the presenter.
  // The cmd* / track methods exist only for the dead format-conversion compute
  // path (d3d9_format_helpers.cpp); all are no-ops (templated where the argument
  // types are frontend-side and not worth pulling in).
  class DxvkCommandList : public RcObject {
  public:
    void cmdBindPipeline(DxvkCmdBuffer, VkPipelineBindPoint, VkPipeline) { }
    template<typename Layout, typename Write>
    void bindResources(DxvkCmdBuffer, const Layout*, uint32_t, const Write*, size_t, const void*) { }
    void cmdDispatch(DxvkCmdBuffer, uint32_t, uint32_t, uint32_t) { }
    void cmdPipelineBarrier(DxvkCmdBuffer, const VkDependencyInfo*) { }
    template<typename Resource>
    void track(Resource&&, DxvkAccess) { }
  };

  // Latency tracker (shim). DXVK's frame-pacing helper; the shim never tracks
  // latency. All methods are no-ops; getStatistics returns zeroed stats. The
  // swapchain present path calls these per frame.
  class DxvkLatencyTracker : public RcObject {
  public:
    bool needsAutoMarkers() const { return false; }
    void notifyCpuPresentBegin(uint64_t) { }
    void notifyCpuPresentEnd(uint64_t) { }
    void discardTimings() { }
    DxvkLatencyStats getStatistics(uint64_t) { return DxvkLatencyStats(); }
    void sleepAndBeginFrame(uint64_t, double) { }
  };

  // Sibling-module types referenced only in signature-matched stub methods
  // below. They are passed by Rc<>/const& so forward declarations are enough
  // for this header; their full definitions come from the resource/device/
  // shader shim headers in the frontend's translation unit.
  class DxvkBuffer;
  class DxvkBufferView;
  class DxvkSampler;
  class DxvkImage;
  class DxvkResourceAllocation;
  enum class DxvkStatCounter : uint32_t;

  // Additional image usage to fold in (dxvk-ref/dxvk_image.h). Built by value in
  // d3d9_device.cpp's ensureImageCompatibility path, so it must be complete. The
  // shim ignores it (ensureImageCompatibility always succeeds).
  struct DxvkImageUsageInfo {
    VkImageCreateFlags   flags            = 0u;
    VkImageUsageFlags    usage            = 0u;
    VkPipelineStageFlags stages           = 0u;
    VkAccessFlags        access           = 0u;
    VkImageLayout        layout           = VK_IMAGE_LAYOUT_UNDEFINED;
    VkColorSpaceKHR      colorSpace       = VK_COLOR_SPACE_MAX_ENUM_KHR;
    uint32_t             viewFormatCount  = 0u;
    const VkFormat*      viewFormats      = nullptr;
    VkBool32             stableGpuAddress = VK_FALSE;
  };

  // All the pipeline dynamic/constant-state value types (DxvkBlendConstants,
  // DxvkDepthBias(Representation/Bounds), DxvkInputAssemblyState,
  // DxvkRasterizerState, DxvkMultisampleState, DxvkDepthStencilState,
  // DxvkLogicOpState, DxvkBlendMode, DxvkVertexInput, …) — the frontend builds
  // these by value in EmitCs lambdas, so they must be COMPLETE here.

  // Barrier-control flags (dxvk-ref/dxvk_context_state.h). The shim issues no
  // barriers; kept only so setBarrierControl's signature matches.
  enum class DxvkBarrierControl : uint32_t {
    ComputeAllowWriteOnlyOverlap  = 0u,
    ComputeAllowReadWriteOverlap  = 1u,
    GraphicsAllowReadWriteOverlap = 2u,
  };
  using DxvkBarrierControlFlags = Flags<DxvkBarrierControl>;

  // Async-submit status (dxvk-ref/dxvk_queue.h). The shim submits synchronously,
  // so this is a trivial result holder.
  struct DxvkSubmitStatus {
    std::atomic<VkResult> result = { VK_SUCCESS };
  };

  // Present-sync semaphore pair — fully defined in dxvk_presenter.h. Only
  // forward-declared here; synchronizeWsi takes it by const-ref so the complete
  // type isn't needed in this header (avoids a context<->presenter include cycle).
  struct PresenterSync;

  // ---- Minimal state structs the recorded signatures reference ----
  // These mirror dxvk-ref but carry only what the shim consults. They live here
  // because they are tiny, Vulkan-free, and tightly coupled to the recorder.

  // One framebuffer attachment: a render-target view. Matched to
  // dxvk-ref/dxvk_framebuffer.h so DxvkRenderTargets compares by view.
  struct DxvkAttachment {
    Rc<DxvkImageView> view = nullptr;

    bool operator == (const DxvkAttachment& o) const { return view == o.view; }
    bool operator != (const DxvkAttachment& o) const { return view != o.view; }
  };

  // Bound render targets. Layout matches dxvk-ref: one depth + N color slots.
  // The shim only consults color[0] (the backbuffer).
  struct DxvkRenderTargets {
    DxvkAttachment depth;
    DxvkAttachment color[MaxNumRenderTargets];

    bool operator == (const DxvkRenderTargets& o) const {
      bool eq = depth == o.depth;
      for (uint32_t i = 0; i < MaxNumRenderTargets && eq; i++)
        eq = color[i] == o.color[i];
      return eq;
    }

    bool operator != (const DxvkRenderTargets& o) const {
      return !(*this == o);
    }
  };



  /**
   * \brief DXVK context (Metal shim)
   *
   * Holds RcObject so the frontend can keep it in Rc<>. Records frontend draw
   * state into the D9mtBackend and triggers clear+draw+present on draw().
   */
  class DxvkContext : public RcObject {

  public:

    // Constructed by DxvkDevice::createContext(). The backend is pulled from the
    // device (the device owns the single D9mtBackend).
    explicit DxvkContext(const Rc<DxvkDevice>& device);

    ~DxvkContext();

    // --- Frame lifecycle ---

    // Begins recording a frame. Resets the per-frame draw/clear state.
    void beginRecording(
      const Rc<DxvkCommandList>& cmdList);

    // Ends recording. The shim has nothing buffered (draw already presented),
    // so this returns the (null) command list the frontend ignores on this path.
    Rc<DxvkCommandList> endRecording(
      const VkDebugUtilsLabelEXT* reason);

    void endFrame() { }

    // Transparent flush. Present already happened inside draw(); nothing to do.
    void flushCommandList(
      const VkDebugUtilsLabelEXT* reason,
            DxvkSubmitStatus*     status);

    // WSI sync — no semaphores in the shim.
    void synchronizeWsi(const PresenterSync& sync) { (void) sync; }

    // The swapchain present lambda calls this to start "external" rendering and
    // then hands the result to the blitter. The shim returns itself wrapped so
    // the blitter can record through the same context.
    Rc<DxvkCommandList> beginExternalRendering();

    // --- Render target / clear ---

    // Remembers the bound color target (backbuffer view -> Metal texture).
    void bindRenderTargets(
            DxvkRenderTargets&&   targets,
            VkImageAspectFlags    feedbackLoop);

    // Records a clear color for the next render pass.
    void clearRenderTarget(
      const Rc<DxvkImageView>&    imageView,
            VkImageAspectFlags    clearAspects,
            VkClearValue          clearValue,
            VkImageAspectFlags    discardAspects);

    // --- Vertex input ---

    // Remembers binding-0's Metal buffer handle + offset; ignores others.
    void bindVertexBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            uint32_t              stride);

    // --- Shaders (ignored: backend owns the hardcoded FF pipeline) ---

    template<VkShaderStageFlagBits Stage>
    void bindShader(Rc<DxvkShader>&& shader) {
      // Remember the bound vertex/fragment shader so draw() can translate it to
      // MSL (shader-conversion module) and build a real Metal pipeline.
      if constexpr (Stage == VK_SHADER_STAGE_VERTEX_BIT)
        m_boundVertexShader = std::move(shader);
      else if constexpr (Stage == VK_SHADER_STAGE_FRAGMENT_BIT)
        m_boundFragmentShader = std::move(shader);
      else
        (void) shader;
    }

    // --- Viewport ---

    void setViewports(
            uint32_t              viewportCount,
      const DxvkViewport*         viewports);

    // --- Draw (the call that reaches Metal) ---

    // Reads draws[0], builds a D9mtDraw, and triggers clear+draw+present.
    void draw(
            uint32_t              count,
      const VkDrawIndirectCommand* draws);

    // ---------------------------------------------------------------------
    // Stubs: signature-matched no-ops for everything else the frontend may
    // record on non-triangle paths. Kept inline to keep the .cpp focused on
    // the live path. Signatures copied from dxvk-ref/dxvk_context.h.
    // ---------------------------------------------------------------------

    // Remember the bound index buffer's Metal handle + byte offset + type so
    // drawIndexed() can issue an indexed Metal draw. An empty slice (the
    // frontend's unbind) clears it, after which drawIndexed is a no-op.
    void bindIndexBuffer(DxvkBufferSlice&& buffer, VkIndexType indexType) {
      m_indexBuffer.handle = buffer.defined() ? buffer.bufferHandle() : 0u;
      m_indexBuffer.offset = buffer.defined() ? buffer.offset()       : 0u;
      m_indexBuffer.type   = indexType;
    }

    void bindIndexBufferRange(VkDeviceSize offset, VkDeviceSize length, VkIndexType indexType) {
      (void) offset; (void) length; (void) indexType;
    }

    void bindDrawBuffers(DxvkBufferSlice&& argBuffer, DxvkBufferSlice&& cntBuffer) {
      (void) argBuffer; (void) cntBuffer;
    }

    void bindVertexBufferRange(uint32_t binding, VkDeviceSize offset, VkDeviceSize length, uint32_t stride) {
      (void) binding; (void) offset; (void) length; (void) stride;
    }

    // Remember the GPU address + Metal handle of the buffer bound at `slot` so
    // draw() can write the address into the argument buffer the translated shader
    // reads its constants from (FF transform/clip/spec live in AB sets 2/3) AND
    // make the buffer resident (an argument buffer references it by address, so
    // Metal needs an explicit useResource or the GPU read faults to zero).
    void bindUniformBuffer(VkShaderStageFlags stages, uint32_t slot, DxvkBufferSlice&& buffer) {
      (void) stages;
      BoundUniformBuffer& bound = m_uniformBuffers[slot];
      bound.address = buffer.defined() ? buffer.gpuAddress()  : 0u;
      bound.handle  = buffer.defined() ? buffer.bufferHandle() : 0u;
    }

    void bindUniformBufferRange(VkShaderStageFlags stages, uint32_t slot, VkDeviceSize offset, VkDeviceSize length) {
      (void) stages; (void) slot; (void) offset; (void) length;
    }

    // Record the texture bound at `slot`: its argument-buffer resource ID (the
    // value written into a descriptor set's argument buffer so the shader can
    // sample it) and its Metal handle (made resident so the GPU may read it).
    void bindResourceImageView(VkShaderStageFlags stages, uint32_t slot, Rc<DxvkImageView>&& view) {
      (void) stages;
      BoundTexture& bound = m_textures[slot];
      bound.resourceId = view != nullptr ? view->gpuResourceId() : 0u;
      bound.handle     = view != nullptr ? view->metalTexture()  : 0u;
    }

    void bindResourceBufferView(VkShaderStageFlags stages, uint32_t slot, Rc<DxvkBufferView>&& view) {
      (void) stages; (void) slot; (void) view;
    }

    // Record the D3D9 sampler state bound at `slot`. The Metal sampler object is
    // created lazily and cached at draw time (see resolveSampler).
    void bindResourceSampler(VkShaderStageFlags stages, uint32_t slot, Rc<DxvkSampler>&& sampler) {
      (void) stages;
      if (sampler != nullptr)
        m_samplerKeys[slot] = sampler->key();
      else
        m_samplerKeys.erase(slot);
    }

    void bindXfbBuffer(uint32_t binding, DxvkBufferSlice&& buffer, DxvkBufferSlice&& counter) {
      (void) binding; (void) buffer; (void) counter;
    }

    // Indexed draw: same bind path as draw(), but fetches indices from the
    // bound index buffer (see bindIndexBuffer). Defined in the .cpp.
    void drawIndexed(uint32_t count, const VkDrawIndexedIndirectCommand* draws);

    void drawIndirect(VkDeviceSize offset, uint32_t count, uint32_t stride, bool unroll) {
      (void) offset; (void) count; (void) stride; (void) unroll;
    }

    void drawIndirectCount(VkDeviceSize offset, VkDeviceSize countOffset, uint32_t maxCount, uint32_t stride) {
      (void) offset; (void) countOffset; (void) maxCount; (void) stride;
    }

    void drawIndexedIndirect(VkDeviceSize offset, uint32_t count, uint32_t stride, bool unroll) {
      (void) offset; (void) count; (void) stride; (void) unroll;
    }

    void drawIndexedIndirectCount(VkDeviceSize offset, VkDeviceSize countOffset, uint32_t maxCount, uint32_t stride) {
      (void) offset; (void) countOffset; (void) maxCount; (void) stride;
    }

    void drawIndirectXfb(VkDeviceSize counterOffset, uint32_t counterDivisor, uint32_t counterBias) {
      (void) counterOffset; (void) counterDivisor; (void) counterBias;
    }

    void dispatch(uint32_t x, uint32_t y, uint32_t z) {
      (void) x; (void) y; (void) z;
    }

    void dispatchIndirect(VkDeviceSize offset) { (void) offset; }

    // Push data: the frontend writes FF transform/light constants into the
    // constant store at absolute `offset`. draw() later copies the dwords each
    // shader's push blocks reference into a Metal buffer bound at its push index.
    void pushData(VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) {
      // Route each push block to its stage region so per-stage blocks never
      // clobber the shared block (they all arrive at local offset 0).
      uint32_t storeOffset = pushRegionBaseForStages(stages) + offset;
      if (m_pushConstantStore.size() < storeOffset + size)
        m_pushConstantStore.resize(storeOffset + size, 0u);
      std::memcpy(m_pushConstantStore.data() + storeOffset, data, size);
    }


    // FF spec-constant values (one dword per id); the translated shader is
    // specialized with these before its MTLFunction is created.
    void setSpecConstants(VkPipelineBindPoint pipeline, uint32_t index, uint32_t count, const void* data) {
      (void) pipeline;
      if (m_specConstantData.size() < index + count)
        m_specConstantData.resize(index + count, 0u);
      std::memcpy(m_specConstantData.data() + index, data, count * sizeof(uint32_t));
    }

    // Image / buffer maintenance — no-ops. ensureImageCompatibility reports
    // success so the present lambda's colorspace check passes.
    bool ensureImageCompatibility(const Rc<DxvkImage>& image, const DxvkImageUsageInfo& usageInfo) {
      (void) image; (void) usageInfo;
      return true;
    }

    void ensureBufferAddress(const Rc<DxvkBuffer>& buffer) { (void) buffer; }

    // Remaining recorded state setters: pure no-ops on the FF triangle path.
    void setBlendConstants(DxvkBlendConstants blendConstants) { (void) blendConstants; }
    void setDepthBias(DxvkDepthBias depthBias) { m_dynamicState.setDepthBias(depthBias); }
    void setDepthBiasRepresentation(DxvkDepthBiasRepresentation r) { (void) r; }
    void setDepthBounds(DxvkDepthBounds depthBounds) { (void) depthBounds; }
    void setStencilReference(uint32_t reference) { m_dynamicState.setStencilReference(reference); }
    void setInputAssemblyState(const DxvkInputAssemblyState& ia) {
      m_dynamicState.setPrimitiveTopology(ia.primitiveTopology());
    }
    // Capture the D3D9 vertex declaration so draw() can build the Metal vertex
    // descriptor (attributes feed shader [[attribute(location)]]; bindings give
    // each stream's stride/step).
    void setInputLayout(uint32_t attributeCount, const DxvkVertexInput* attributes,
                        uint32_t bindingCount, const DxvkVertexInput* bindings) {
      m_vertexAttributes.assign(attributes, attributes + attributeCount);
      m_vertexBindings.assign(bindings, bindings + bindingCount);
    }
    void setRasterizerState(const DxvkRasterizerState& rs) { m_dynamicState.setRasterizerState(rs); }
    void setMultisampleState(const DxvkMultisampleState& ms) { (void) ms; }
    // Capture the depth-stencil state so draw() can bind a matching Metal
    // depth-stencil state object (resolved + cached at draw time).
    void setDepthStencilState(const DxvkDepthStencilState& ds) { m_depthStencilState = ds; }
    void setLogicOpState(const DxvkLogicOpState& lo) { (void) lo; }
    // Capture the color-attachment blend state so draw() can fold it into the
    // pipeline. Only attachment 0 (the backbuffer) is consulted by the shim.
    void setBlendMode(uint32_t attachment, const DxvkBlendMode& blendMode) {
      if (attachment == 0)
        m_blendMode = blendMode;
    }
    void setBarrierControl(DxvkBarrierControlFlags control) { (void) control; }

    // Debug labels / stat counters — no-ops.
    void beginDebugLabel(const VkDebugUtilsLabelEXT& label) { (void) label; }
    void endDebugLabel() { }
    void insertDebugLabel(const VkDebugUtilsLabelEXT& label) { (void) label; }
    void addStatCtr(DxvkStatCounter counter, uint64_t value) { (void) counter; (void) value; }
    void setDebugName(const Rc<DxvkPagedResource>& resource, const char* name) {
      (void) resource; (void) name;
    }

    // Latency tracking — no-ops (the shim presents synchronously).
    void beginLatencyTracking(const Rc<DxvkLatencyTracker>& tracker, uint64_t frameId) {
      (void) tracker; (void) frameId;
    }
    void endLatencyTracking(const Rc<DxvkLatencyTracker>& tracker) { (void) tracker; }

    // Queries — no GPU queries in the shim.
    void beginQuery(const Rc<DxvkQuery>& query) { (void) query; }
    void endQuery(const Rc<DxvkQuery>& query) { (void) query; }
    void writeTimestamp(const Rc<DxvkQuery>& query) { (void) query; }

    // GPU events / signals — fire nothing (single-threaded synchronous replay).
    void signalGpuEvent(const Rc<DxvkEvent>& event) { (void) event; }
    void signal(const Rc<sync::Signal>& signal, uint64_t value) { (void) signal; (void) value; }

    // Barriers — implicit in the single Metal pass; nothing to emit.
    void emitGraphicsBarrier(VkPipelineStageFlags srcStages, VkAccessFlags srcAccess,
                             VkPipelineStageFlags dstStages, VkAccessFlags dstAccess) {
      (void) srcStages; (void) srcAccess; (void) dstStages; (void) dstAccess;
    }

    // Resource maintenance — no-ops for app textures/buffers off the triangle path.
    // Only the swapchain calls this — for its backbuffers. Register each one's
    // Metal texture so the backend routes draws bound to it into the drawable.
    void initImage(const Rc<DxvkImage>& image, VkImageLayout initialLayout) {
      (void) initialLayout;
      if (m_backend && image != nullptr)
        m_backend->registerBackbufferTexture(image->metalTexture());
    }
    void invalidateBuffer(const Rc<DxvkBuffer>& buffer, Rc<DxvkResourceAllocation>&& slice) {
      (void) buffer; (void) slice;
    }
    void transformImage(const Rc<DxvkImage>& dstImage, const VkImageSubresourceRange& dstSubresources,
                        VkImageLayout srcLayout, VkImageLayout dstLayout) {
      (void) dstImage; (void) dstSubresources; (void) srcLayout; (void) dstLayout;
    }
    void generateMipmaps(const Rc<DxvkImageView>& imageView, VkFilter filter) {
      (void) imageView; (void) filter;
    }

    // Clears / blits / resolves of app images — no-ops; only the backbuffer
    // clear (clearRenderTarget) is wired to Metal.
    void clearImageView(const Rc<DxvkImageView>& imageView, VkOffset3D offset, VkExtent3D extent,
                        VkImageAspectFlags aspect, VkClearValue value) {
      (void) imageView; (void) offset; (void) extent; (void) aspect; (void) value;
    }
    void blitImageView(const Rc<DxvkImageView>& dstView, const VkOffset3D* dstOffsets,
                       const Rc<DxvkImageView>& srcView, const VkOffset3D* srcOffsets, VkFilter filter) {
      (void) dstView; (void) dstOffsets; (void) srcView; (void) srcOffsets; (void) filter;
    }
    void resolveImage(const Rc<DxvkImage>& dstImage, const Rc<DxvkImage>& srcImage,
                      const VkImageResolve& region, VkFormat format,
                      VkResolveModeFlagBits mode, VkResolveModeFlagBits stencilMode) {
      (void) dstImage; (void) srcImage; (void) region; (void) format; (void) mode; (void) stencilMode;
    }

    // Buffer-to-buffer copy. The frontend's FlushBuffer uploads a mapped
    // (staging) buffer into the device buffer the draw binds via this. Every
    // shim buffer is a host-visible coherent Metal allocation, so the Vulkan
    // staging+transfer dance collapses to a CPU memcpy in the synchronous shim
    // — without it, MAP_MODE_BUFFER vertex/index buffers reach the GPU as zero.
    void copyBuffer(const Rc<DxvkBuffer>& dstBuffer, VkDeviceSize dstOffset,
                    const Rc<DxvkBuffer>& srcBuffer, VkDeviceSize srcOffset, VkDeviceSize numBytes) {
      if (!dstBuffer || !srcBuffer || !numBytes)
        return;
      void*       dst = dstBuffer->mapPtr(dstOffset);
      const void* src = srcBuffer->mapPtr(srcOffset);
      if (dst && src)
        std::memcpy(dst, src, numBytes);
    }
    // Uploads tightly-packed pixels from a staging buffer into a real Metal
    // texture (mip 0..N of a 2D texture). The frontend packs the data with no
    // row padding, so bytes-per-row is one row of blocks: for block-compressed
    // formats (BC/DXT) that is ceil(width / blockWidth) * blockBytes, which
    // reduces to width * elementSize for uncompressed formats.
    void copyBufferToImage(const Rc<DxvkImage>& dstImage, VkImageSubresourceLayers dstSubresource,
                           VkOffset3D dstOffset, VkExtent3D dstExtent, const Rc<DxvkBuffer>& srcBuffer,
                           VkDeviceSize srcOffset, VkDeviceSize rowAlignment, VkDeviceSize sliceAlignment,
                           VkFormat srcFormat) {
      (void) rowAlignment; (void) sliceAlignment; (void) srcFormat;
      if (!dstImage || !srcBuffer || !dstImage->metalTexture())
        return;
      const void* src = srcBuffer->mapPtr(srcOffset);
      const DxvkFormatInfo* fmt = dstImage->formatInfo();
      if (!src || !fmt || !fmt->elementSize)
        return;
      uint32_t blockWidth   = fmt->blockSize.width ? fmt->blockSize.width : 1u;
      uint32_t blocksPerRow = (dstExtent.width + blockWidth - 1u) / blockWidth;
      m_backend->uploadTexture(dstImage->metalTexture(), dstSubresource.mipLevel,
        uint32_t(dstOffset.x), uint32_t(dstOffset.y), dstExtent.width, dstExtent.height,
        src, uint32_t(blocksPerRow * fmt->elementSize));
    }
    void copyImage(const Rc<DxvkImage>& dstImage, VkImageSubresourceLayers dstSubresource, VkOffset3D dstOffset,
                   const Rc<DxvkImage>& srcImage, VkImageSubresourceLayers srcSubresource, VkOffset3D srcOffset,
                   VkExtent3D extent) {
      (void) dstImage; (void) dstSubresource; (void) dstOffset;
      (void) srcImage; (void) srcSubresource; (void) srcOffset; (void) extent;
    }
    void copyImageToBuffer(const Rc<DxvkBuffer>& dstBuffer, VkDeviceSize dstOffset, VkDeviceSize rowAlignment,
                           VkDeviceSize sliceAlignment, VkFormat dstFormat, const Rc<DxvkImage>& srcImage,
                           VkImageSubresourceLayers srcSubresource, VkOffset3D srcOffset, VkExtent3D srcExtent) {
      (void) dstBuffer; (void) dstOffset; (void) rowAlignment; (void) sliceAlignment; (void) dstFormat;
      (void) srcImage; (void) srcSubresource; (void) srcOffset; (void) srcExtent;
    }
    void uploadBuffer(const Rc<DxvkBuffer>& buffer, VkDeviceSize bufferOffset, const Rc<DxvkBuffer>& source,
                      VkDeviceSize sourceOffset, VkDeviceSize size) {
      (void) buffer; (void) bufferOffset; (void) source; (void) sourceOffset; (void) size;
    }

  private:

    // A shader stage translated to MSL and compiled to a Metal library +
    // specialized function. Cached per shader cookie so a redraw of the same
    // shader skips SPIRV-Cross and the runtime MSL compile.
    struct CompiledShaderStage {
      MslShader translation;        // MSL text + reflection (from the converter)
      uint64_t  library  = 0;       // MTLLibrary
      uint64_t  function = 0;       // specialized "main0" MTLFunction
    };

    // Resolves (translate + compile + specialize, cached) the bound shader for a
    // stage. Returns nullptr if no shader is bound or translation fails.
    const CompiledShaderStage* resolveStage(const Rc<DxvkShader>& shader);

    // Builds the Metal vertex descriptor from the captured D3D9 input layout.
    MetalVertexLayout buildVertexLayout() const;

    // Translates the captured D3D9 blend state into Metal pipeline blend state.
    MetalColorBlend buildBlendState() const;

    // Resolves (creating + caching) the Metal depth-stencil state object for the
    // captured depth state. Returns 0 to leave Metal's default always-pass state.
    uint64_t resolveDepthStencilState();

    // Resolves shaders + PSO and assembles the per-draw buffer/argument/
    // residency bindings into `out` (minus the vertex/index count, which the
    // caller fills). Returns false if the draw cannot proceed. Shared by
    // draw() and drawIndexed().
    bool prepareDraw(D9mtPipelineDraw& out);

    // Copies the dwords a stage's push blocks reference out of the constant
    // store into the per-stage upload buffer (slot 0 = vertex, 1 = fragment, so
    // the two stages' push data don't clobber each other); returns the handle.
    uint64_t uploadPushData(const MslReflection& reflection, uint32_t slot);

    // Builds one argument buffer per descriptor set the stage's resources live
    // in (writing each bound uniform buffer's GPU address into its AB slot) and
    // appends a binding for each at buffer(setIndex). `stageKey` (0=vertex,
    // 1=fragment) keeps the two stages' scratch argument buffers separate.
    void bindArgumentBuffers(
      const MslReflection&             reflection,
            bool                       fragmentStage,
            uint32_t                   stageKey,
            std::vector<D9mtBufferBinding>& bindings,
            std::vector<uint64_t>&     residentBuffers);

    Rc<DxvkDevice> m_device;

    // The single Metal backend (owned by the device). All recorded state folds
    // into one renderAndPresent() call at draw() time.
    D9mtBackend*   m_backend = nullptr;

    // Translated+compiled programs, keyed by (shader cookie, spec-data hash): a
    // shader specialized against different spec-constant values compiles to a
    // distinct MSL program (e.g. color vs shadow sampler types).
    std::unordered_map<uint64_t, CompiledShaderStage> m_compiledStages;
    // Render pipeline states, keyed by (vs cookie, fs cookie, layout) hash.
    std::unordered_map<uint64_t, uint64_t> m_pipelineStates;

    // Captured D3D9 vertex declaration (set via setInputLayout).
    std::vector<DxvkVertexInput> m_vertexAttributes;
    std::vector<DxvkVertexInput> m_vertexBindings;

    // Push-constant store the frontend fills (pushData), indexed by absolute
    // push offset; assembled per-draw into a shared Metal buffer.
    std::vector<uint8_t> m_pushConstantStore;
    uint64_t             m_pushUploadBuffer[2]    = { 0, 0 };  // [0]=vertex, [1]=fragment
    void*                m_pushUploadBufferCpu[2] = { nullptr, nullptr };

    // FF spec-constant values (setSpecConstants), one dword per id. Used both to
    // specialize the MTLFunction AND, uploaded to m_specDataBuffer, as the
    // SpecConsts buffer the translated shader reads from descriptor set 3.
    std::vector<uint32_t> m_specConstantData;
    uint64_t              m_specDataBuffer    = 0;
    void*                 m_specDataBufferCpu = nullptr;
    uint64_t              m_specDataAddress   = 0;

    // Uploads m_specConstantData into m_specDataBuffer; returns its GPU address.
    uint64_t uploadSpecData();

    // GPU address + Metal handle of the uniform buffer bound at each slot
    // (bindUniformBuffer). The address goes into argument buffers; the handle is
    // made resident so the GPU can follow that address.
    struct BoundUniformBuffer { uint64_t address = 0; uint64_t handle = 0; };
    std::unordered_map<uint32_t, BoundUniformBuffer> m_uniformBuffers;

    // Scratch argument buffers, keyed by (stageKey << 8 | descriptorSet); each
    // holds the GPU-address slots one set's resources resolve to.
    struct ArgumentBuffer { uint64_t buffer = 0; void* cpu = nullptr; };
    std::unordered_map<uint32_t, ArgumentBuffer> m_argumentBuffers;

    // Shaders bound for the next draw, translated to MSL on demand.
    Rc<DxvkShader> m_boundVertexShader;
    Rc<DxvkShader> m_boundFragmentShader;

    // Bound vertex streams: D3D9 binding -> (Metal buffer handle, byte offset).
    // Every stream the input layout references must be bound (binding b at Metal
    // buffer VertexBufferBase + b), including the FF "null" stream that backs
    // unused attributes — leaving one unbound makes Metal drop the draw.
    struct BoundVertexStream { uint64_t handle = 0; uint64_t offset = 0; };
    std::unordered_map<uint32_t, BoundVertexStream> m_vertexBuffers;

    // Bound index buffer (bindIndexBuffer): Metal handle + byte offset + the
    // Vulkan index type the frontend decoded from the D3D9 format.
    struct BoundIndexBuffer { uint64_t handle = 0; uint64_t offset = 0; VkIndexType type = VK_INDEX_TYPE_UINT16; };
    BoundIndexBuffer m_indexBuffer;

    // Textures bound per DXVK slot (bindResourceImageView): the resource ID goes
    // into a descriptor set's argument buffer; the handle is made resident.
    struct BoundTexture { uint64_t resourceId = 0; uint64_t handle = 0; };
    std::unordered_map<uint32_t, BoundTexture> m_textures;

    // D3D9 sampler state bound per DXVK slot, plus a cache of the Metal sampler
    // objects created from each distinct key (keyed by the key's hash).
    std::unordered_map<uint32_t, DxvkSamplerKey> m_samplerKeys;
    struct MetalSampler { uint64_t handle = 0; uint64_t resourceId = 0; };
    std::unordered_map<size_t, MetalSampler> m_samplerCache;

    // Per-stage sampler-heap argument buffers (keyed by stageKey), holding the
    // sampler resource IDs the translated shader indexes by a push-data value.
    std::unordered_map<uint32_t, ArgumentBuffer> m_samplerHeaps;

    // Resolves (creating + caching) the Metal sampler for a bound slot's key.
    MetalSampler resolveSampler(uint32_t slot);

    // Builds the sampler heap for one stage, injects each sampler's heap index
    // into the stage's push buffer, and appends the heap binding.
    void bindSamplerHeap(
      const MslReflection&             reflection,
            bool                       fragmentStage,
            uint32_t                   stageKey,
            void*                      pushBufferCpu,
            std::vector<D9mtBufferBinding>& bindings);

    // Shared zeroed buffer bound to FF null vertex streams (unused attributes).
    uint64_t m_zeroVertexBuffer    = 0;
    void*    m_zeroVertexBufferCpu = nullptr;

    // Persistent per-draw scratch (cleared, not freed, each draw → no per-draw
    // heap allocation once capacity settles).
    std::vector<D9mtBufferBinding> m_drawBindings;
    std::vector<uint64_t>          m_drawResidentBuffers;

    // Color-attachment blend state (setBlendMode); folded into the PSO + its key.
    DxvkBlendMode m_blendMode = { };

    // Depth-stencil state (setDepthStencilState) + a cache of the Metal state
    // objects built from each distinct (compare op, write enable) combination.
    DxvkDepthStencilState m_depthStencilState = { };
    std::unordered_map<uint64_t, uint64_t> m_depthStencilCache;

    // Per-draw dynamic render state (viewport/scissor, topology, rasterizer,
    // depth bias, stencil reference). Owns its storage + Vulkan->Metal mapping.
    DxvkDynamicState m_dynamicState;

    bool     m_clearPending   = false;  // a clearRenderTarget was recorded
    uint32_t m_clearColorArgb = 0;      // raw D3DCOLOR-style ARGB clear value

  };

}
