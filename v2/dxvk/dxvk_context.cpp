// DxvkContext shim implementation — see dxvk_context.h.
//
// Only the live fixed-function-triangle path has real bodies here. The state
// setters and the long tail of recorded commands are inline no-ops in the
// header. This file folds the recorded clear/bind/draw into one Metal
// renderAndPresent() per draw call.

#include "dxvk_context.h"

#include "dxvk_device.h"   // DxvkDevice::backend() — owner of the D9mtBackend

#include <algorithm>
#include <cstdio>

namespace {
  // First Metal vertex-buffer slot used for D3D9 vertex streams (stream b binds
  // at buffer(VertexBufferBase + b)). 14 leaves the low slots for the argument
  // buffer / push / sampler-heap indices SPIRV-Cross assigns. Matches v1.
  constexpr uint32_t VertexBufferBase = 14u;

  // Descriptor sets the spec-constant buffer lives in: the fixed-function
  // shader module uses set 3, the programmable IR shader uses set 7
  // (DxvkIrShader::SpecDataSet). Resources in either set are filled from
  // setSpecConstants, NOT bindUniformBuffer, so they bind the uploaded spec
  // buffer rather than a CBV.
  constexpr bool isSpecDataSet(uint32_t set) { return set == 3u || set == 7u; }

  void logLine(const char* message) {
    if (FILE* file = std::fopen("C:\\d9mt-test\\v2.log", "a")) {
      std::fprintf(file, "[context] %s\n", message);
      std::fclose(file);
    }
  }
}

namespace dxvk {

  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device(device) {
    // The device owns the single Metal backend; the context records into it.
    // backend() is the agreed accessor on the device shim (SHIM_SPEC: "DxvkDevice
    // owns it; DxvkContext records into it").
    m_backend = device->backend();
  }


  DxvkContext::~DxvkContext() {
  }


  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) {
    (void) cmdList;

    // Reset per-frame recorded state at the start of each frame. Bound vertex
    // streams / uniform buffers persist (the frontend rebinds what changes).
    m_clearPending   = false;
    m_clearColorArgb = 0;
  }


  Rc<DxvkCommandList> DxvkContext::endRecording(const VkDebugUtilsLabelEXT* reason) {
    (void) reason;
    // Nothing is buffered: draw() already presented. The frontend does not use
    // the returned command list on this path.
    return nullptr;
  }


  void DxvkContext::flushCommandList(
      const VkDebugUtilsLabelEXT* reason,
            DxvkSubmitStatus*     status) {
    (void) reason;
    (void) status;
    // The present lambda calls this after the blitter; present already happened
    // inside draw()/blitter present, so there is nothing to submit. DxvkSubmitStatus
    // defaults to VK_SUCCESS, so leaving *status untouched is the success path.
  }


  Rc<DxvkCommandList> DxvkContext::beginExternalRendering() {
    // The swapchain present lambda passes the result to the blitter as
    // "context objects". With no Vulkan command list, the shim hands back null;
    // the blitter shim is expected to record through the backend directly.
    return nullptr;
  }


  void DxvkContext::bindRenderTargets(
            DxvkRenderTargets&&   targets,
            VkImageAspectFlags    feedbackLoop) {
    (void) feedbackLoop;
    // Select the color[0] target. An offscreen render target view carries a real
    // Metal texture; the backbuffer view has none (it renders to the drawable),
    // so a zero handle tells the backend to use the swapchain drawable.
    uint64_t targetTexture = targets.color[0].view != nullptr
      ? targets.color[0].view->metalTexture() : 0u;
    if (m_backend) {
      // A clear recorded for the current target is normally applied lazily by
      // the next draw's render pass; if the target switches first with no draw
      // (a clear-only pass, e.g. clearing a render target before rendering
      // elsewhere), flush it now so the clear reaches that target.
      if (m_clearPending) {
        m_backend->beginFrame(m_clearPending, m_clearColorArgb);
        m_clearPending = false;
      }
      m_backend->setColorTarget(targetTexture);
    }
  }


  void DxvkContext::clearRenderTarget(
      const Rc<DxvkImageView>&    imageView,
            VkImageAspectFlags    clearAspects,
            VkClearValue          clearValue,
            VkImageAspectFlags    discardAspects) {
    (void) imageView;
    (void) discardAspects;

    // Only a color clear matters for the triangle milestone.
    if (!(clearAspects & VK_IMAGE_ASPECT_COLOR_BIT))
      return;

    // VkClearValue carries normalized float RGBA. The backend wants a raw
    // D3DCOLOR-style ARGB word, so pack [0,1] floats back into 8-bit channels.
    const float* rgba = clearValue.color.float32;

    auto toByte = [](float v) -> uint32_t {
      float c = std::min(1.0f, std::max(0.0f, v));
      return uint32_t(c * 255.0f + 0.5f);
    };

    uint32_t a = toByte(rgba[3]);
    uint32_t r = toByte(rgba[0]);
    uint32_t g = toByte(rgba[1]);
    uint32_t b = toByte(rgba[2]);

    m_clearColorArgb = (a << 24) | (r << 16) | (g << 8) | b;
    m_clearPending   = true;
  }


  void DxvkContext::bindVertexBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            uint32_t              stride) {
    (void) stride;

    // Remember every bound stream (not just binding 0) — the FF input layout
    // also references a null stream for unused attributes, and Metal drops the
    // draw if any referenced vertex buffer index is unbound. An empty slice
    // (the frontend's unbind) yields handle 0, which we skip at draw time.
    BoundVertexStream& stream = m_vertexBuffers[binding];
    stream.handle = buffer.bufferHandle();
    stream.offset = buffer.offset();
  }


  void DxvkContext::setViewports(
            uint32_t              viewportCount,
      const DxvkViewport*         viewports) {
    // D3D9 binds a single viewport/scissor pair; store it for the draw path.
    if (viewportCount > 0)
      m_dynamicState.setViewport(viewports[0]);
  }


  const DxvkContext::CompiledShaderStage* DxvkContext::resolveStage(const Rc<DxvkShader>& shader) {
    if (shader == nullptr)
      return nullptr;

    // Specialize against the current spec-constant data (D3D9 sampler types,
    // alpha-test op, …) so dead paths fold out — notably the shadow-sample path
    // that would otherwise force a depth2d texture. The compiled result is
    // therefore cached per (shader, spec-data), like a driver recompiling a
    // specialized pipeline when spec constants change.
    uint32_t cookie = uint32_t(shader->getCookie());
    uint32_t specHash = uint32_t(bit::fnv1a_hash(
      reinterpret_cast<const char*>(m_specConstantData.data()),
      m_specConstantData.size() * sizeof(uint32_t)));
    uint64_t cacheKey = (uint64_t(cookie) << 32) | specHash;

    auto cached = m_compiledStages.find(cacheKey);
    if (cached != m_compiledStages.end())
      return cached->second.function ? &cached->second : nullptr;

    CompiledShaderStage stage;
    // Translate the specialized SPIR-V. The sampler-heap set comes from the
    // shader's layout so the converter can locate the heap's buffer index (or
    // ~0u if no samplers, e.g. the FF triangle).
    SpirvCodeBuffer code = shader->getCodeSpecialized(
      m_specConstantData.data(), uint32_t(m_specConstantData.size()));
    DxvkPipelineLayoutBuilder layout = shader->getLayout();
    stage.translation = convertSpirvToMsl(
      code, layout, shader->metadata().stage, layout.getSamplerHeapSet());

    if (stage.translation.ok) {
      uint64_t device = m_backend->device();
      stage.library = metalCompileLibrary(device, stage.translation.source);
      if (stage.library) {
        stage.function = metalResolveFunction(stage.library,
          stage.translation.reflection.specConstants,
          m_specConstantData.data(), uint32_t(m_specConstantData.size()));
      }
    }
    if (!stage.function) {
      const char* where = !stage.translation.ok ? "convert" : !stage.library ? "compile" : "resolve";
      logLine(("shader fail [" + std::string(where) + "] " + shader->debugName()
               + " err=" + stage.translation.error).c_str());
    }

    auto& slot = m_compiledStages.emplace(cacheKey, std::move(stage)).first->second;
    return slot.function ? &slot : nullptr;
  }


  MetalVertexLayout DxvkContext::buildVertexLayout() const {
    MetalVertexLayout layout;

    for (const DxvkVertexInput& input : m_vertexAttributes) {
      DxvkVertexAttribute attribute = input.attribute();
      MetalVertexAttribute metal;
      metal.metalFormat = metalVertexFormatFromVulkan(uint32_t(attribute.format));
      metal.offset      = attribute.offset;
      metal.bufferIndex = VertexBufferBase + attribute.binding;
      metal.location    = attribute.location;
      layout.attributes.push_back(metal);
    }

    for (const DxvkVertexInput& input : m_vertexBindings) {
      DxvkVertexBinding binding = input.binding();
      MetalVertexStream stream;
      stream.bufferIndex  = VertexBufferBase + binding.binding;
      stream.stride       = binding.extent ? binding.extent : 4u;  // Metal rejects stride 0
      stream.stepFunction = binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2u : 1u;
      stream.stepRate     = 1u;
      layout.streams.push_back(stream);
    }

    return layout;
  }


  MetalColorBlend DxvkContext::buildBlendState() const {
    // Vulkan and Metal order their blend-factor enums differently (Vulkan groups
    // all color factors before the alpha ones; Metal interleaves), so map each.
    auto toMetalFactor = [](VkBlendFactor f) -> uint32_t {
      switch (f) {
        case VK_BLEND_FACTOR_ZERO:                     return 0u;
        case VK_BLEND_FACTOR_ONE:                      return 1u;
        case VK_BLEND_FACTOR_SRC_COLOR:                return 2u;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return 3u;
        case VK_BLEND_FACTOR_SRC_ALPHA:                return 4u;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return 5u;
        case VK_BLEND_FACTOR_DST_COLOR:                return 6u;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return 7u;
        case VK_BLEND_FACTOR_DST_ALPHA:                return 8u;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return 9u;
        case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:       return 10u;
        case VK_BLEND_FACTOR_CONSTANT_COLOR:           return 11u;
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 12u;
        case VK_BLEND_FACTOR_CONSTANT_ALPHA:           return 13u;
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return 14u;
        case VK_BLEND_FACTOR_SRC1_COLOR:               return 15u;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:     return 16u;
        case VK_BLEND_FACTOR_SRC1_ALPHA:               return 17u;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:     return 18u;
        default:                                       return 1u;
      }
    };
    // VkBlendOp and MTLBlendOperation share the same Add/Subtract/Reverse/Min/Max
    // ordering for the basic ops D3D9 exposes.
    auto toMetalOp = [](VkBlendOp op) -> uint32_t {
      switch (op) {
        case VK_BLEND_OP_ADD:              return 0u;
        case VK_BLEND_OP_SUBTRACT:         return 1u;
        case VK_BLEND_OP_REVERSE_SUBTRACT: return 2u;
        case VK_BLEND_OP_MIN:              return 3u;
        case VK_BLEND_OP_MAX:              return 4u;
        default:                           return 0u;
      }
    };

    MetalColorBlend blend;
    blend.enabled   = m_blendMode.blendEnable();
    blend.rgbOp     = toMetalOp(m_blendMode.colorBlendOp());
    blend.alphaOp   = toMetalOp(m_blendMode.alphaBlendOp());
    blend.srcRgb    = toMetalFactor(m_blendMode.colorSrcFactor());
    blend.dstRgb    = toMetalFactor(m_blendMode.colorDstFactor());
    blend.srcAlpha  = toMetalFactor(m_blendMode.alphaSrcFactor());
    blend.dstAlpha  = toMetalFactor(m_blendMode.alphaDstFactor());
    blend.writeMask = uint32_t(m_blendMode.writeMask()) & 0xFu;
    return blend;
  }


  uint64_t DxvkContext::resolveDepthStencilState() {
    // VkCompareOp and the Metal compare function share the same NEVER..ALWAYS
    // ordering. With depth testing off, the comparison is forced to ALWAYS so
    // fragments are never depth-rejected; depth writes still follow the state.
    bool     write   = m_depthStencilState.depthWrite();
    uint32_t compare = m_depthStencilState.depthTest()
      ? uint32_t(m_depthStencilState.depthCompareOp())
      : 7u;  // WMTCompareFunctionAlways

    bool stencil = m_depthStencilState.stencilTest();
    DxvkStencilOp frontOp = m_depthStencilState.stencilOpFront();
    DxvkStencilOp backOp  = m_depthStencilState.stencilOpBack();

    auto toFace = [](const DxvkStencilOp& op) {
      D9mtBackend::StencilFace face;
      face.failOp          = uint32_t(op.failOp());
      face.passOp          = uint32_t(op.passOp());
      face.depthFailOp     = uint32_t(op.depthFailOp());
      face.compareFunction = uint32_t(op.compareOp());
      face.readMask        = op.compareMask();
      face.writeMask       = op.writeMask();
      return face;
    };
    D9mtBackend::StencilFace front = toFace(frontOp);
    D9mtBackend::StencilFace back  = toFace(backOp);

    // Key on the whole depth-stencil state (depth + both stencil faces); the
    // stencil reference is dynamic and bound separately, so it is excluded.
    uint64_t key = bit::fnv1a_hash(
      reinterpret_cast<const char*>(&m_depthStencilState), sizeof(m_depthStencilState));
    key = (key << 1) | (write ? 1u : 0u);
    auto cached = m_depthStencilCache.find(key);
    if (cached != m_depthStencilCache.end())
      return cached->second;

    uint64_t state = m_backend->createDepthStencilState(compare, write, stencil, front, back);
    m_depthStencilCache.emplace(key, state);
    return state;
  }


  uint64_t DxvkContext::uploadPushData(const MslReflection& reflection, uint32_t slot) {
    if (!reflection.pushDataSize)
      return 0u;

    if (!m_pushUploadBuffer[slot]) {
      m_pushUploadBuffer[slot] = m_backend->createSharedBuffer(
        MaxTotalPushDataSize, &m_pushUploadBufferCpu[slot]);
    }
    if (!m_pushUploadBufferCpu[slot])
      return 0u;

    auto* dst = static_cast<uint8_t*>(m_pushUploadBufferCpu[slot]);
    std::memset(dst, 0, reflection.pushDataSize);

    // Each push block copies `size` bytes from the constant store (where the
    // frontend wrote them, at absolute push offset srcOffset) into push space
    // at dstOffset — exactly the layout the translated shader reads.
    for (const MslPushBlock& block : reflection.pushBlocks) {
      if (block.srcOffset + block.size <= m_pushConstantStore.size()
       && block.dstOffset + block.size <= MaxTotalPushDataSize) {
        std::memcpy(dst + block.dstOffset,
                    m_pushConstantStore.data() + block.srcOffset, block.size);
      }
    }
    return m_pushUploadBuffer[slot];
  }


  uint64_t DxvkContext::uploadSpecData() {
    // Always hand back a valid, zeroed buffer even when no spec constants were
    // set: the translated shaders dereference the spec/common-state buffer
    // unconditionally (e.g. clipPlaneCount reads it), so a null argument-buffer
    // slot would fault the GPU and drop the draw. Zero reads as "no clip planes,
    // default state", which is correct for a shader that set no spec data.
    if (!m_specDataBuffer) {
      m_specDataBuffer = m_backend->createSharedBuffer(
        MaxNumSpecConstants * sizeof(uint32_t), &m_specDataBufferCpu, &m_specDataAddress);
    }
    if (!m_specDataBufferCpu)
      return 0u;

    size_t bytes = m_specConstantData.size() * sizeof(uint32_t);
    std::memset(m_specDataBufferCpu, 0, MaxNumSpecConstants * sizeof(uint32_t));
    if (bytes)
      std::memcpy(m_specDataBufferCpu, m_specConstantData.data(), bytes);
    return m_specDataAddress;
  }


  void DxvkContext::bindArgumentBuffers(
    const MslReflection&             reflection,
          bool                       fragmentStage,
          uint32_t                   stageKey,
          std::vector<D9mtBufferBinding>& bindings,
          std::vector<uint64_t>&     residentBuffers) {
    if (reflection.resources.empty())
      return;

    // Group resources by descriptor set; each set becomes one argument buffer
    // bound at buffer(set). entryCount = highest AB slot + 1 in that set.
    uint32_t entryCountPerSet[16] = { };
    for (const MslResourceRef& ref : reflection.resources) {
      if (ref.set < 16u)
        entryCountPerSet[ref.set] = std::max<uint32_t>(entryCountPerSet[ref.set], ref.abId + 1u);
    }

    for (uint32_t set = 0; set < 16u; set++) {
      uint32_t entryCount = entryCountPerSet[set];
      if (!entryCount)
        continue;

      uint32_t bufferKey = (stageKey << 8) | set;
      ArgumentBuffer& argumentBuffer = m_argumentBuffers[bufferKey];
      if (!argumentBuffer.buffer) {
        argumentBuffer.buffer = m_backend->createSharedBuffer(
          16u * 8u, &argumentBuffer.cpu);   // 16 slots is the most any FF set uses
      }
      if (!argumentBuffer.cpu)
        continue;

      // Each AB slot holds the GPU address of the uniform buffer bound at the
      // resource's DXVK slot (textures/samplers are off the FF triangle path, so
      // their slots stay zero — a null descriptor the shader must not deref).
      auto* slots = static_cast<uint64_t*>(argumentBuffer.cpu);
      std::memset(slots, 0, entryCount * sizeof(uint64_t));
      for (const MslResourceRef& ref : reflection.resources) {
        if (ref.set != set || ref.abId >= entryCount)
          continue;

        if (ref.isUniformBuffer) {
          if (isSpecDataSet(ref.set)) {
            // Spec-constant buffer: filled from setSpecConstants, not a CBV.
            uint64_t address = uploadSpecData();
            if (address) {
              slots[ref.abId] = address;
              residentBuffers.push_back(m_specDataBuffer);
            }
          } else {
            auto bound = m_uniformBuffers.find(ref.slot);
            if (bound != m_uniformBuffers.end() && bound->second.address) {
              slots[ref.abId] = bound->second.address;
              residentBuffers.push_back(bound->second.handle);
            }
          }
        } else if (ref.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
          // Texture descriptor: the argument-buffer slot holds the texture's
          // resource ID; the texture itself must be made resident.
          auto tex = m_textures.find(ref.slot);
          if (tex != m_textures.end() && tex->second.resourceId) {
            slots[ref.abId] = tex->second.resourceId;
            if (tex->second.handle)
              residentBuffers.push_back(tex->second.handle);
          }
        }
      }

      bindings.push_back({ argumentBuffer.buffer, 0u, set, fragmentStage });
    }
  }


  DxvkContext::MetalSampler DxvkContext::resolveSampler(uint32_t slot) {
    auto keyIt = m_samplerKeys.find(slot);
    if (keyIt == m_samplerKeys.end())
      return {};

    const DxvkSamplerKey& key = keyIt->second;
    size_t hash = key.hash();
    auto cached = m_samplerCache.find(hash);
    if (cached != m_samplerCache.end())
      return cached->second;

    // Vulkan address modes (REPEAT=0, MIRRORED=1, CLAMP_TO_EDGE=2,
    // CLAMP_TO_BORDER=3, MIRROR_CLAMP=4) map onto the winemetal enum, which
    // orders them differently.
    auto toMetalAddress = [](uint32_t vk) -> uint32_t {
      switch (vk) {
        case 0u: return 2u;  // Repeat
        case 1u: return 3u;  // MirrorRepeat
        case 2u: return 0u;  // ClampToEdge
        case 3u: return 5u;  // ClampToBorderColor
        case 4u: return 1u;  // MirrorClampToEdge
        default: return 0u;
      }
    };
    // VkFilter and VkSamplerMipmapMode (NEAREST=0 / LINEAR=1) match the
    // winemetal min/mag enum; mip uses NotMipmapped(0)/Nearest(1)/Linear(2).
    uint32_t mipFilter = key.u.p.mipMode ? 2u : 1u;

    // LOD clamps are stored as 4.8 fixed-point (see DxvkSamplerKey::setLodRange).
    float lodMin = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.minLod);
    float lodMax = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.maxLod);

    MetalSampler sampler;
    sampler.handle = m_backend->createSampler(
      key.u.p.minFilter, key.u.p.magFilter, mipFilter,
      toMetalAddress(key.u.p.addressU), toMetalAddress(key.u.p.addressV),
      toMetalAddress(key.u.p.addressW), key.u.p.anisotropy, lodMin, lodMax,
      &sampler.resourceId);

    m_samplerCache.emplace(hash, sampler);
    return sampler;
  }


  void DxvkContext::bindSamplerHeap(
    const MslReflection&             reflection,
          bool                       fragmentStage,
          uint32_t                   stageKey,
          void*                      pushBufferCpu,
          std::vector<D9mtBufferBinding>& bindings) {
    if (reflection.samplerHeapIndex < 0 || reflection.samplers.empty())
      return;

    ArgumentBuffer& heap = m_samplerHeaps[stageKey];
    if (!heap.buffer)
      heap.buffer = m_backend->createSharedBuffer(16u * 8u, &heap.cpu);
    if (!heap.cpu)
      return;

    auto* heapSlots = static_cast<uint64_t*>(heap.cpu);
    std::memset(heapSlots, 0, 16u * sizeof(uint64_t));

    // Each declared sampler takes a dense heap index; its Metal resource ID goes
    // into that heap slot and the index is written into the push buffer at the
    // dword the shader reads it from (reflected blockOffset).
    for (uint32_t i = 0; i < reflection.samplers.size() && i < 16u; i++) {
      const MslSamplerRef& ref = reflection.samplers[i];
      MetalSampler sampler = resolveSampler(ref.slot);
      heapSlots[i] = sampler.resourceId;
      if (pushBufferCpu) {
        auto* index = reinterpret_cast<uint16_t*>(
          static_cast<uint8_t*>(pushBufferCpu) + ref.blockOffset);
        *index = uint16_t(i);
      }
    }

    bindings.push_back({ heap.buffer, 0u, uint32_t(reflection.samplerHeapIndex), fragmentStage });
  }


  bool DxvkContext::prepareDraw(D9mtPipelineDraw& pipelineDraw) {
    // Translate + compile the bound shaders (cached). The vertex shader is
    // mandatory; the fragment shader is optional (depth-only passes have none).
    const CompiledShaderStage* vertexStage;
    const CompiledShaderStage* fragmentStage;
    {
      TraceScope resolveScope(TraceZone::ResolveShader);
      vertexStage   = resolveStage(m_boundVertexShader);
      fragmentStage = resolveStage(m_boundFragmentShader);
    }
    if (!vertexStage)
      return false;

    uint64_t pipeline = 0;
    {
      TraceScope pipelineScope(TraceZone::BuildPipeline);
      MetalVertexLayout layout = buildVertexLayout();
      MetalColorBlend   blend  = buildBlendState();

      // Pipeline cache keyed by the shader pair plus the blend state — the same
      // shaders drawn opaque vs alpha-blended need distinct pipelines. The
      // shader cookies and the 32-bit packed blend bits are mixed into one key.
      uint32_t blendBits = 0u;
      std::memcpy(&blendBits, &m_blendMode, sizeof(blendBits));
      uint64_t key = (uint64_t(DxvkShader::getCookie(m_boundVertexShader)) << 32)
                   |  uint64_t(DxvkShader::getCookie(m_boundFragmentShader));
      key ^= uint64_t(blendBits) * 0x9E3779B97F4A7C15ull;
      auto cachedPipeline = m_pipelineStates.find(key);
      if (cachedPipeline != m_pipelineStates.end()) {
        pipeline = cachedPipeline->second;
      } else {
        pipeline = metalBuildRenderPipeline(m_backend->device(),
          vertexStage->function, fragmentStage ? fragmentStage->function : 0u,
          layout, /* MTLPixelFormatBGRA8Unorm */ 80u, blend);
        m_pipelineStates.emplace(key, pipeline);
        if (!pipeline)
          logLine("render pipeline build failed");
      }
    }
    if (!pipeline)
      return false;

    TraceScope bindScope(TraceZone::BindResources);

    // Bind the vertex stream (binding 0 at VertexBufferBase) and each stage's
    // push block at its reflected index.
    // Bind every stream the input layout references: the frontend's real
    // buffer where bound, else a shared zero buffer (the FF null stream for
    // unused attributes — Metal drops the draw if any referenced index is
    // unbound, and the frontend never binds that stream).
    // Reuse persistent scratch (capacity survives clear()) so a steady draw
    // stream allocates nothing per draw.
    std::vector<D9mtBufferBinding>& bindings = m_drawBindings;
    std::vector<uint64_t>&          residentBuffers = m_drawResidentBuffers;
    bindings.clear();
    residentBuffers.clear();

    for (const DxvkVertexInput& input : m_vertexBindings) {
      uint32_t streamBinding = input.binding().binding;
      auto bound = m_vertexBuffers.find(streamBinding);
      if (bound != m_vertexBuffers.end() && bound->second.handle) {
        bindings.push_back({ bound->second.handle, bound->second.offset,
                             VertexBufferBase + streamBinding, false });
      } else {
        if (!m_zeroVertexBuffer)
          m_zeroVertexBuffer = m_backend->createSharedBuffer(256u, &m_zeroVertexBufferCpu);
        bindings.push_back({ m_zeroVertexBuffer, 0u,
                             VertexBufferBase + streamBinding, false });
      }
    }

    if (vertexStage->translation.reflection.pushBufferIndex >= 0) {
      if (uint64_t push = uploadPushData(vertexStage->translation.reflection, 0u))
        bindings.push_back({ push, 0u,
          uint32_t(vertexStage->translation.reflection.pushBufferIndex), false });
    }
    if (fragmentStage && fragmentStage->translation.reflection.pushBufferIndex >= 0) {
      if (uint64_t push = uploadPushData(fragmentStage->translation.reflection, 1u))
        bindings.push_back({ push, 0u,
          uint32_t(fragmentStage->translation.reflection.pushBufferIndex), true });
    }

    // Bindless sampler heaps. Must follow uploadPushData: the heap index is
    // injected into each stage's push buffer (where the shader reads it).
    bindSamplerHeap(vertexStage->translation.reflection, false, 0u, m_pushUploadBufferCpu[0], bindings);
    if (fragmentStage)
      bindSamplerHeap(fragmentStage->translation.reflection, true, 1u, m_pushUploadBufferCpu[1], bindings);

    // Argument buffers carrying the FF constant buffers (transform, clip planes,
    // spec data) the translated shaders read from descriptor sets 2/3. The
    // buffers they point to must be made resident (residentBuffers).
    bindArgumentBuffers(vertexStage->translation.reflection, false, 0u, bindings, residentBuffers);
    if (fragmentStage)
      bindArgumentBuffers(fragmentStage->translation.reflection, true, 1u, bindings, residentBuffers);

    pipelineDraw.pipelineState     = pipeline;
    pipelineDraw.bindings          = bindings.data();
    pipelineDraw.bindingCount      = uint32_t(bindings.size());
    pipelineDraw.residentBuffers   = residentBuffers.data();
    pipelineDraw.residentCount     = uint32_t(residentBuffers.size());
    pipelineDraw.depthStencilState = resolveDepthStencilState();
    m_dynamicState.applyTo(pipelineDraw);
    pipelineDraw.clear             = m_clearPending;
    pipelineDraw.clearColorArgb    = m_clearColorArgb;
    return true;
  }


  void DxvkContext::draw(
            uint32_t              count,
      const VkDrawIndirectCommand* draws) {
    if (!m_backend || !count || !draws)
      return;

    TraceScope drawScope(TraceZone::Draw);
    D9mtPipelineDraw pipelineDraw;
    if (!prepareDraw(pipelineDraw))
      return;

    pipelineDraw.vertexCount = draws[0].vertexCount;
    m_backend->beginFrame(m_clearPending, m_clearColorArgb);
    m_backend->appendDraw(pipelineDraw);
    m_clearPending = false;
    FrameTrace::countDraw();
  }


  void DxvkContext::drawIndexed(
            uint32_t                     count,
      const VkDrawIndexedIndirectCommand* draws) {
    if (!m_backend || !count || !draws || !m_indexBuffer.handle)
      return;

    TraceScope drawScope(TraceZone::Draw);
    D9mtPipelineDraw pipelineDraw;
    if (!prepareDraw(pipelineDraw))
      return;

    const VkDrawIndexedIndirectCommand& d = draws[0];
    // The index buffer's element size (2 or 4 bytes) advances firstIndex into a
    // byte offset on top of the slice's own offset.
    uint32_t indexSize = m_indexBuffer.type == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
    pipelineDraw.indexCount        = d.indexCount;
    pipelineDraw.indexBuffer       = m_indexBuffer.handle;
    pipelineDraw.indexBufferOffset = m_indexBuffer.offset + uint64_t(d.firstIndex) * indexSize;
    pipelineDraw.indexType         = m_indexBuffer.type == VK_INDEX_TYPE_UINT32 ? 1u : 0u;
    pipelineDraw.baseVertex        = d.vertexOffset;
    m_backend->beginFrame(m_clearPending, m_clearColorArgb);
    m_backend->appendDraw(pipelineDraw);
    m_clearPending = false;
    FrameTrace::countDraw();
  }

}
