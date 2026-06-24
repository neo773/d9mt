#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <ostream>
#include <string>

#include "dxvk_include.h"
#include "dxvk_pipelayout.h"   // kept: DxvkBindingInfo / DxvkPushDataBlock /
#include "dxvk_shader_io.h"    //       DxvkShaderBinding / layout builder / map
#include "dxvk_shader_key.h"

#include "../spirv/spirv_code_buffer.h"

// Shim shader base. In the real DXVK this object compiles SPIR-V and feeds
// Vulkan pipelines. In the shim the one rendered triangle uses a hardcoded MSL
// pipeline in D9mtBackend, so the shader is NEVER executed — every concrete
// shader simply remembers its create-info/debug-name and returns empty code.
// We keep the public method surface the frontend, device, and pipeline modules
// reference (only debugName() is touched by the D3D9 frontend; the rest exist
// so subclasses override a consistent interface and the device can hold them).
// See SHIM_SPEC.md (shader module = opaque, getCode → empty).

namespace dxvk {

  class DxvkShader;

  // Device-level shader compile flags. Carried verbatim from real DXVK so
  // DxvkShaderOptions stays bit-compatible (the frontend memcpy-hashes it and
  // copies it into DxvkIrShaderCreateInfo). The shim never acts on any flag.
  enum class DxvkShaderCompileFlag : uint32_t {
    InsertSharedMemoryBarriers  = 0u,
    InsertResourceBarriers      = 1u,
    TypedR32LoadRequiresFormat  = 2u,
    DisableMsaa                 = 3u,
    EnableSampleRateShading     = 4u,
    Supports16BitArithmetic     = 5u,
    SupportsSubDwordPushData    = 6u,
    LowerItoF                   = 7u,
    LowerFtoI                   = 8u,
    LowerSinCos                 = 9u,
    LowerF32toF16               = 10u,
    LowerConstantArrays         = 11u,
    SemanticIo                  = 12u,
    LowerInBoundsCbvToBda       = 13u,
  };

  using DxvkShaderCompileFlags = Flags<DxvkShaderCompileFlag>;

  enum class DxvkShaderSpirvFlag : uint32_t {
    ExportPointSize             = 0u,
    SupportsNvRawAccessChains   = 1u,
    SupportsSzInfNanPreserve16  = 2u,
    SupportsSzInfNanPreserve32  = 3u,
    SupportsSzInfNanPreserve64  = 4u,
    SupportsRte16               = 5u,
    SupportsRte32               = 6u,
    SupportsRte64               = 7u,
    SupportsRtz16               = 8u,
    SupportsRtz32               = 9u,
    SupportsRtz64               = 10u,
    SupportsDenormFlush16       = 11u,
    SupportsDenormFlush32       = 12u,
    SupportsDenormFlush64       = 13u,
    SupportsDenormPreserve16    = 14u,
    SupportsDenormPreserve32    = 15u,
    SupportsDenormPreserve64    = 16u,
    IndependentRoundMode        = 17u,
    IndependentDenormMode       = 18u,
    SupportsFloatControls2      = 19u,
    SupportsResourceIndexing    = 20u,
    SupportsDescriptorHeap      = 21u,
  };

  using DxvkShaderSpirvFlags = Flags<DxvkShaderSpirvFlag>;

  // Device-level shader options. Treated as an opaque, copyable blob by the
  // frontend (assigned into DxvkIrShaderCreateInfo, hashed by value).
  struct DxvkShaderOptions {
    DxvkShaderCompileFlags flags = 0u;
    DxvkShaderSpirvFlags   spirv = 0u;
    uint32_t maxUniformBufferSize     = 0u;
    int32_t  maxUniformBufferCount    = -1;
    uint32_t builtInPushDataOffset    = 0u;
    uint32_t minStorageBufferAlignment = 0u;
  };

  // Feature flags a shader advertises. The IR binding-model pass sets these
  // from the lowered code; the shim only reads metadata().stage, but the full
  // set must exist so the pass and getCode compile against the real interface.
  enum DxvkShaderFlag : uint64_t {
    HasSampleRateShading,
    HasTransformFeedback,
    ExportsPosition,
    ExportsStencilRef,
    ExportsViewportIndexLayerFromVertexStage,
    ExportsSampleMask,
    UsesFragmentCoverage,
    UsesSparseResidency,
    TessellationPoints,
    SemanticIo,
  };

  using DxvkShaderFlags = Flags<DxvkShaderFlag>;

  // Extra information about a shader. The IR binding-model pass fills all of
  // these; the shim's draw path consults only `stage`. Field layout mirrors
  // dxvk-ref so the ported pass assigns to real members.
  struct DxvkShaderMetadata {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    DxvkShaderFlags       flags = { };
    uint32_t              specConstantMask = 0u;
    DxvkShaderIo          inputs  = { };
    DxvkShaderIo          outputs = { };
    VkPrimitiveTopology   inputTopology  = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    VkPrimitiveTopology   outputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    uint32_t              flatShadingInputs = 0u;
    int32_t               rasterizedStream  = 0;
    uint32_t              patchVertexCount  = 0u;
    std::array<uint32_t, MaxNumXfbBuffers> xfbStrides = { };
  };

  // Linkage info passed to getCode() by the (kept) pipeline layer. The shim
  // always passes null, but the fields getCode references must exist.
  struct DxvkShaderLinkage {
    bool fsDualSrcBlend  = false;
    bool fsFlatShading   = false;
    bool sampleLocations = false;
    bool semanticIo      = false;

    VkPrimitiveTopology   inputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    VkShaderStageFlagBits prevStage     = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    DxvkShaderIo          prevStageOutputs = { };

    std::array<VkComponentMapping, MaxNumRenderTargets> rtSwizzles = { };

    bool eq(const DxvkShaderLinkage&) const { return true; }
    size_t hash() const { return 0u; }
  };

  /**
   * \brief Shader object (shim)
   *
   * Intrusively reference counted to match the real DXVK ABI: the frontend
   * holds shaders in Rc<DxvkShader>, which calls incRef()/decRef(). We use the
   * same self-deleting atomic counter rather than RcObject so the Rc<> contract
   * is identical.
   */
  class DxvkShader {

  public:

    DxvkShader()
    : m_cookie(++s_cookie) { }

    virtual ~DxvkShader() = default;

    force_inline void incRef() {
      m_refCount.fetch_add(1u, std::memory_order_acquire);
    }

    force_inline void decRef() {
      if (m_refCount.fetch_sub(1u, std::memory_order_acquire) == 1u)
        delete this;
    }

    // Unique per-object id for look-up tables.
    size_t getCookie() const {
      return m_cookie;
    }

    // Cached metadata; computed once via the virtual hook.
    const DxvkShaderMetadata& metadata() {
      if (unlikely(!m_metadata))
        m_metadata = getShaderMetadata();
      return *m_metadata;
    }

    // Compile bookkeeping kept so the pipeline layer's flow is unchanged. The
    // shim never actually compiles.
    bool needsCompile() const { return m_needsCompile.load(); }
    bool notifyCompile()      { return m_needsCompile.exchange(false); }

    virtual DxvkPipelineLayoutBuilder getLayout() = 0;
    virtual DxvkShaderMetadata        getShaderMetadata() = 0;
    virtual void                      compile() = 0;

    // Returns final shader code. The triangle is drawn by D9mtBackend, so the
    // shim always yields an empty buffer — nothing downstream consumes it.
    virtual SpirvCodeBuffer getCode(
      const DxvkShaderBindingMap*       bindings,
      const DxvkShaderLinkage*          linkage) = 0;

    // Returns SPIR-V with specialization constants folded to concrete values.
    // IR shaders override this to bake D3D9 sampler types (so color samplers
    // become texture2d, not depth2d); precompiled-SPIR-V shaders (fixed
    // function) carry no spec constants, so the default ignores the values.
    virtual SpirvCodeBuffer getCodeSpecialized(
      const uint32_t* specData, uint32_t specCount) {
      (void) specData; (void) specCount;
      return getCode(nullptr, nullptr);
    }

    virtual void        dump(std::ostream&) = 0;
    virtual std::string debugName() = 0;

    static uint32_t getCookie(const Rc<DxvkShader>& shader) {
      return shader != nullptr ? uint32_t(shader->getCookie()) : 0u;
    }

    // Optional SPIR-V dump directory. The shim never dumps, so this is always
    // empty; the IR shader's convertIr() skips dumping on an empty path.
    static const std::string& getShaderDumpPath() {
      static const std::string empty;
      return empty;
    }

  private:

    // inline so the header-only shim needs no companion .cpp.
    static inline std::atomic<uint32_t> s_cookie = { 0u };

    std::atomic<uint32_t>             m_refCount     = { 0u };
    uint32_t                          m_cookie       = 0u;
    std::atomic<bool>                 m_needsCompile = { true };

    std::optional<DxvkShaderMetadata> m_metadata;

  };


  // Set of shader pointers for a pipeline (kept for device/pipeline code that
  // groups stages). Empty in the shim's draw path.
  struct DxvkShaderSet {
    DxvkShader* vs  = nullptr;
    DxvkShader* tcs = nullptr;
    DxvkShader* tes = nullptr;
    DxvkShader* gs  = nullptr;
    DxvkShader* fs  = nullptr;
    DxvkShader* cs  = nullptr;
  };

}
