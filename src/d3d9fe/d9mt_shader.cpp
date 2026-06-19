// d9mt: Metal backend — DxvkShader -> MSL translation + caches.
//
// Mirrors the WORKING in-process chain of the hand-rolled driver
// (src/d3d9/d9mt_translate.cpp + d3d9.cpp CompileShader):
//   DxvkShader::getCode(nullptr, moduleInfo)   [shader-defined bindings kept,
//     undefined-input elimination / RT swizzles / flat shading applied]
//   -> SPIRV-Cross CompilerMSL (msl 3.0, argument buffers tier 2)
//   -> d9mtmetal NEW_LIBRARY_FROM_SOURCE (runtime MSL compile)
//   -> MTLLibrary_newFunctionWithConstants ("main0", ALL function constants)
//
// Reflection differences from d9mt_translate.cpp: resources are identified
// by their SPIR-V Binding decoration (== DXVK resource slot id, BACKEND-
// SURFACE §4.2), not by dxso variable names — this also covers the
// fixed-function shader generator. Sampler metadata (heap-index push dwords)
// and push-block layout come from the shader's DxvkPipelineLayoutBuilder.
//
// All caches are process-global and hold Rc<DxvkShader> refs for the process
// lifetime (upstream's pipeline manager has the same lifetime policy).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "d9mt_draw.h"
#include "d9mt_trace.h"

#include "../../vendor/spirv-cross/spirv_msl.hpp"
#include "../../vendor/dxvk/src/util/sha1/sha1_util.h"

namespace dxvk::d9mt {

  // ==========================================================================
  // metallib disk-cache keying (Layer A). We compute a stable content key over
  // the FINAL MSL text plus the codegen options; the native side (Layer C)
  // owns the compile, the disk store, the load, and all fallback. The .metallib
  // bytes never cross the ABI — only this key goes down, only a handle comes up.
  //
  // Keying on the final MSL is the tightest correct key: it transparently
  // absorbs every upstream DXVK/spirv-cross transform. codegen_epoch is a cheap
  // belt-and-suspenders global kill switch (bump when our translation pipeline
  // changes the MSL bytes for the same input). The Metal/OS toolchain
  // fingerprint is folded in native-side, so it is not part of this key.
  // ==========================================================================

  namespace {
    // Bump to invalidate every cached metallib at once (codegen change that
    // alters MSL bytes for the same input: spirv-cross bump, reflection change).
    constexpr uint32_t kMetallibCodegenEpoch = 1u;

    // 20-byte SHA-1 content key over (domain ‖ epoch ‖ msl_lang ‖ fast_math ‖
    // source_kind ‖ MSL bytes). 160-bit, stored as the cache primary key.
    // The header is serialized into a PACKED, explicit-endian byte buffer —
    // never a padded struct — so the hashed bytes are deterministic across
    // builds/compilers (struct padding bytes are unspecified and must not feed
    // the digest). source_kind domain-separates input kinds so a future
    // SPIR-V/DXBC backend keyed through the same digest can't collide with MSL.
    Sha1Hash computeMetallibKey(
      const std::string& msl,
            uint16_t     mslLangVersion,
            uint8_t      fastMath,
            uint32_t     sourceKind) {
      uint8_t hdr[13];
      hdr[0]  = 'L';
      hdr[1]  = '1';
      hdr[2]  = uint8_t(kMetallibCodegenEpoch);
      hdr[3]  = uint8_t(kMetallibCodegenEpoch >> 8);
      hdr[4]  = uint8_t(kMetallibCodegenEpoch >> 16);
      hdr[5]  = uint8_t(kMetallibCodegenEpoch >> 24);
      hdr[6]  = uint8_t(mslLangVersion);
      hdr[7]  = uint8_t(mslLangVersion >> 8);
      hdr[8]  = fastMath;
      hdr[9]  = uint8_t(sourceKind);
      hdr[10] = uint8_t(sourceKind >> 8);
      hdr[11] = uint8_t(sourceKind >> 16);
      hdr[12] = uint8_t(sourceKind >> 24);

      const Sha1Data chunks[] = {
        { hdr,        sizeof(hdr) },
        { msl.data(), msl.size()  },
      };
      return Sha1Hash::compute(2, chunks);
    }

    bool metallibCacheEnabled() {
      // Default on (verified in-game: 149 hits / 0 failures, robust degrade to
      // live compile on any miss/corruption/toolchain-absence). D9MT_METALLIB_CACHE=0
      // disables, matching the D9MT_BATCH/SUBALLOC/ASYNC sibling flags.
      static const bool s_enabled = []() {
        const char* v = std::getenv("D9MT_METALLIB_CACHE");
        return !(v && v[0] == '0' && v[1] == '\0');
      }();
      return s_enabled;
    }

    // Fast-math: the Metal MSL compiler default here is ON (-ffast-math), which
    // assumes no NaN/Inf and lets it reorder/contract float ops — a known source
    // of subtle precision artifacts (specular sparkles, thin bright slivers,
    // normalize/division edge cases) for game shaders that rely on IEEE behavior.
    // DEFAULT OFF here: in-game A/B confirmed the residual COD4 artifacts (specular
    // sparkles / thin slivers) were fast-math precision, and disabling it removes
    // them. We're CPU-bound (GPU has headroom), so the slightly slower precise-math
    // shaders cost ~no framerate — correctness wins. D9MT_FASTMATH=1 opts back into
    // fast-math for GPU-bound cases. The value is folded into the metallib cache key,
    // so toggling it recompiles rather than reusing a mismatched cached library.
    bool shaderFastMath() {
      static const bool s_fast = []() {
        const char* v = std::getenv("D9MT_FASTMATH");
        return v && v[0] == '1' && v[1] == '\0';
      }();
      return s_fast;
    }
  }

  // ==========================================================================
  // sampler heap: shadow array living inside a shared MTLBuffer so the draw
  // path binds it directly ([[buffer(samplerHeapIndex)]]). Written by the
  // DxvkSampler ctor/dtor (under the sampler-pool mutex) BEFORE any draw
  // that references the slot is committed; live draws never reference slots
  // being (re)written, so CPU stores racing GPU reads of other slots are
  // benign (aligned 8-byte stores).
  // ==========================================================================

  namespace {
    struct SamplerHeapStorage {
      uint64_t*    data = nullptr;
      obj_handle_t buffer = 0;
    };

    SamplerHeapStorage& samplerHeapStorage() {
      static SamplerHeapStorage s_storage = []() -> SamplerHeapStorage {
        SamplerHeapStorage result = { };

        const size_t size = size_t(SamplerHeapSize) * sizeof(uint64_t);

        void* mem = VirtualAlloc(nullptr, size,
          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        obj_handle_t device = mtlDevice();

        if (mem && device) {
          WMTBufferInfo info = { };
          info.length  = size;
          info.options = WMTResourceStorageModeShared;
          info.memory.set(mem);

          result.buffer = MTLDevice_newBuffer(device, &info);
        }

        if (mem && result.buffer) {
          result.data = reinterpret_cast<uint64_t*>(mem);
        } else {
          logf("d9mt: sampler heap buffer creation failed (mem=%p dev=%llx)"
               " — falling back to CPU-only shadow",
            mem, (unsigned long long)device);
          if (mem)
            VirtualFree(mem, 0, MEM_RELEASE);
          static uint64_t s_fallback[SamplerHeapSize] = { };
          result.data = s_fallback;
          result.buffer = 0;
        }
        return result;
      }();
      return s_storage;
    }
  }

  uint64_t* samplerHeapData() {
    return samplerHeapStorage().data;
  }

  obj_handle_t samplerHeapBuffer() {
    return samplerHeapStorage().buffer;
  }


  // ==========================================================================
  // compile cache
  // ==========================================================================

  struct CompiledShader::FunctionCache {
    std::mutex mutex;
    // key = the spec values supplied, in specConstants order
    std::map<std::vector<uint32_t>, obj_handle_t> functions;
  };

  namespace {

    // shared-block source offset in m_state.pc.constantData for a given
    // push data block index (same formula as the private static
    // DxvkContext::computePushDataBlockOffset)
    uint32_t pushDataBlockSrcOffset(uint32_t index) {
      return index
        ? MaxSharedPushDataSize + MaxPerStagePushDataSize * (index - 1u)
        : 0u;
    }


    std::unique_ptr<CompiledShader> compileShader(
      const Rc<DxvkShader>&             shader,
      const DxvkShaderModuleCreateInfo& moduleInfo) {
      D9MT_ZONE(d9mt::ZoneShaderCompile);
      auto result = std::make_unique<CompiledShader>();
      result->stage = shader->info().stage;

      std::string msl;
      // slot -> AB ids. MULTImap: dxso emits aliased variables for one
      // binding (t0_2d + t0_2d_shadow share the SPIR-V Binding); every
      // variant's AB dword must be written or depth-compare sampling reads
      // a null texture (GTA IV black-world bug).
      std::unordered_multimap<uint32_t, uint32_t> slotToAbId;

      // -------- SPIR-V -> MSL + reflection
      try {
        SpirvCodeBuffer code = shader->getCode(nullptr, moduleInfo);

        spirv_cross::CompilerMSL compiler(code.data(), code.dwords());

        spirv_cross::CompilerMSL::Options opts;
        opts.set_msl_version(3, 0, 0);
        opts.argument_buffers = true;
        opts.argument_buffers_tier =
          spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        compiler.set_msl_options(opts);

        msl = compiler.compile();

        spirv_cross::ShaderResources res = compiler.get_shader_resources();

        auto reflectSet0 = [&] (const spirv_cross::Resource& r) {
          uint32_t set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
          if (set != 0u)
            return;

          uint32_t slot = compiler.get_decoration(r.id, spv::DecorationBinding);
          uint32_t abId = compiler.get_automatic_msl_resource_binding(r.id);

          // unassigned (-1) = the variable never made it into the argument
          // buffer (dead resource) — it has no AB slot to write
          if (abId == uint32_t(-1))
            return;

          slotToAbId.emplace(slot, abId);
          result->abEntryCount = std::max(result->abEntryCount, abId + 1u);
        };

        for (const auto& r : res.uniform_buffers)
          reflectSet0(r);
        for (const auto& r : res.storage_buffers)
          reflectSet0(r);
        for (const auto& r : res.separate_images)
          reflectSet0(r);

        // SPIRV-Cross binds the set-0 argument buffer at [[buffer(0)]]
        // (= set index); the working driver hardcodes this too.
        if (result->abEntryCount)
          result->abBufferIndex = 0;

        for (const auto& r : res.push_constant_buffers)
          result->pushBufferIndex =
            int32_t(compiler.get_automatic_msl_resource_binding(r.id));

        for (const auto& r : res.separate_samplers) {
          uint32_t set = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
          if (set == shader->info().samplerHeap.getSet())
            result->samplerHeapIndex =
              int32_t(compiler.get_automatic_msl_resource_binding(r.id));
        }

        for (const auto& sc : compiler.get_specialization_constants()) {
          const auto& cst  = compiler.get_constant(sc.id);
          const auto& type = compiler.get_type(cst.constant_type);

          ShaderSpecConstant info = { };
          info.id           = sc.constant_id;
          info.defaultValue = cst.scalar(0, 0);
          info.isBool       = type.basetype == spirv_cross::SPIRType::Boolean;
          result->specConstants.push_back(info);
        }
      } catch (const DxvkError& e) {
        Logger::err(str::format("d9mt: shader translation failed (",
          shader->debugName(), "): ", e.message()));
        return nullptr;
      } catch (const std::exception& e) {
        Logger::err(str::format("d9mt: shader translation failed (",
          shader->debugName(), "): ", e.what()));
        return nullptr;
      }

      // -------- layout metadata: resource refs, samplers, push blocks
      DxvkPipelineLayoutBuilder layout = shader->getLayout();
      DxvkPipelineBindingRange bindings = layout.getBindings();

      for (size_t i = 0; i < bindings.bindingCount; i++) {
        const DxvkShaderDescriptor& binding = bindings.bindings[i];

        if (binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_SAMPLER) {
          // sampler descriptors carry no Binding decoration (dxso + FF set
          // only resourceIndex); the bound DxvkSampler lives in
          // m_samplers[resourceIndex] — using getBinding() here reads
          // m_samplers[0] for every sampler (GTA IV wrong-sampler bug)
          ShaderSamplerRef ref = { };
          ref.slot        = uint16_t(binding.getResourceIndex());
          ref.blockOffset = uint16_t(binding.getBlockOffset());
          result->samplers.push_back(ref);
          continue;
        }

        if (binding.getSet() != 0u)
          continue;

        // write EVERY AB dword that aliases this binding (shadow variants)
        auto range = slotToAbId.equal_range(binding.getBinding());

        for (auto entry = range.first; entry != range.second; ++entry) {
          ShaderResourceRef ref = { };
          ref.slot            = uint16_t(binding.getBinding());
          ref.abId            = uint16_t(entry->second);
          ref.type            = binding.getDescriptorType();
          ref.isUniformBuffer = binding.isUniformBuffer()
            || binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          result->resources.push_back(ref);
        }
      }

      uint32_t pushMask = layout.getPushDataMask();
      for (uint32_t index = 0; index < DxvkPushDataBlock::MaxBlockCount; index++) {
        if (!(pushMask & (1u << index)))
          continue;

        DxvkPushDataBlock block = layout.getPushDataBlock(index);

        ShaderPushBlock info = { };
        info.dstOffset    = block.getOffset();
        info.size         = block.getSize();
        info.srcOffset    = pushDataBlockSrcOffset(index);
        info.resourceMask = block.getResourceDwordMask();
        result->pushBlocks.push_back(info);

        result->pushDataSize = std::max(result->pushDataSize,
          info.dstOffset + info.size);
      }

      if (result->pushDataSize > MaxTotalPushDataSize) {
        Logger::err(str::format("d9mt: shader push data exceeds limit: ",
          result->pushDataSize));
        return nullptr;
      }

      if (const char* dump = std::getenv("D9MT_DUMP_MSL")) {
        std::string path = std::string(dump) + "\\" + shader->debugName() + ".metal";
        if (FILE* f = std::fopen(path.c_str(), "w")) {
          std::fwrite(msl.data(), 1, msl.size(), f);
          std::fclose(f);
        }
      }

      // -------- MSL -> MTLLibrary (d9mtmetal runtime compile)
      obj_handle_t device = mtlDevice();
      if (!device)
        return nullptr;

      obj_handle_t library = 0;
      obj_handle_t compileError = 0;

      if (metallibCacheEnabled()) {
        // Cache path: compute the content key and let the native side resolve
        // hit-load / miss-compile-store-load / live-fallback uniformly. The
        // MSL source is only read native-side on a cache MISS.
        const uint8_t fastMath = shaderFastMath() ? 1u : 0u;
        Sha1Hash key = computeMetallibKey(msl, 0x0300u, fastMath,
                                          uint32_t(D9MT_SOURCE_MSL_TEXT));

        d9mt_library_params lp;
        std::memset(&lp, 0, sizeof(lp));
        lp.device       = device;
        lp.key_ptr      = uint64_t(uintptr_t(&key));
        lp.key_len      = sizeof(Sha1Digest);
        lp.source_ptr   = uint64_t(uintptr_t(msl.data()));
        lp.source_len   = msl.size();
        lp.source_kind  = D9MT_SOURCE_MSL_TEXT;
        lp.target_flags = fastMath ? D9MT_TARGET_FAST_MATH : 0u;

        int status = D9MT_UnixCall(D9MT_FUNC_LIBRARY_FOR_KEY, &lp);
        if (status != 0 || !lp.ret_library) {
          Logger::err(str::format("d9mt: metallib-cache compile failed (",
            shader->debugName(), "), status ", status));
          if (lp.ret_error)
            logNSError("d9mt: metallib cache", lp.ret_error);
          return nullptr;
        }
        if (lp.ret_error)
          compileError = lp.ret_error; // warnings only

        library = lp.ret_library;
        logf("d9mt: metallib %s: %s",
          lp.ret_status == D9MT_LIBRARY_HIT      ? "HIT" :
          lp.ret_status == D9MT_LIBRARY_COMPILED ? "COMPILED" :
          lp.ret_status == D9MT_LIBRARY_FELL_BACK ? "FELL_BACK" : "?",
          shader->debugName().c_str());
      } else {
        // Default path (unchanged): live runtime newLibraryWithSource.
        d9mt_newlibrary_params lp;
        std::memset(&lp, 0, sizeof(lp));
        lp.device     = device;
        lp.source_ptr = uint64_t(uintptr_t(msl.data()));
        lp.source_len = msl.size();
        lp.fast_math  = shaderFastMath() ? 1u : 0u;

        int status = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
        if (status != 0 || !lp.ret_library) {
          Logger::err(str::format("d9mt: MSL compile failed (",
            shader->debugName(), "), status ", status));
          if (lp.ret_error)
            logNSError("d9mt: MSL compile", lp.ret_error);
          return nullptr;
        }
        if (lp.ret_error)
          compileError = lp.ret_error; // warnings only

        library = lp.ret_library;
      }

      if (compileError)
        NSObject_release(compileError);

      result->library   = library;
      result->functions = new CompiledShader::FunctionCache();

      logf("d9mt: shader compiled: %s (%zu bytes MSL, ab=%u push=%d heap=%d "
           "res=%zu smp=%zu spec=%zu)",
        shader->debugName().c_str(), msl.size(), result->abEntryCount,
        result->pushBufferIndex, result->samplerHeapIndex,
        result->resources.size(), result->samplers.size(),
        result->specConstants.size());

      return result;
    }


    struct CompileKey {
      DxvkShader*                 shader;
      DxvkShaderModuleCreateInfo  moduleInfo;
    };

    struct CompileKeyHash {
      size_t operator () (const CompileKey& k) const {
        return std::hash<void*>()(k.shader) ^ k.moduleInfo.hash();
      }
    };

    struct CompileKeyEq {
      bool operator () (const CompileKey& a, const CompileKey& b) const {
        return a.shader == b.shader && a.moduleInfo.eq(b.moduleInfo);
      }
    };

    struct CompileEntry {
      Rc<DxvkShader>                  shader;   // keeps the key pointer alive
      std::unique_ptr<CompiledShader> compiled; // null = failed (cached)
    };

    std::mutex s_compileMutex;
    std::unordered_map<CompileKey, CompileEntry, CompileKeyHash, CompileKeyEq>
      s_compileCache;

  } // namespace


  const CompiledShader* getCompiledShader(
    const Rc<DxvkShader>&             shader,
    const DxvkShaderModuleCreateInfo& moduleInfo) {
    if (shader == nullptr)
      return nullptr;

    CompileKey key = { shader.ptr(), moduleInfo };

    std::lock_guard<std::mutex> lock(s_compileMutex);

    auto entry = s_compileCache.find(key);
    if (entry != s_compileCache.end())
      return entry->second.compiled.get();

    CompileEntry newEntry;
    newEntry.shader   = shader;
    newEntry.compiled = compileShader(shader, moduleInfo);

    return s_compileCache.emplace(key, std::move(newEntry))
      .first->second.compiled.get();
  }


  obj_handle_t getShaderFunction(
    const CompiledShader*             shader,
    const uint32_t*                   specData) {
    if (!shader || !shader->library)
      return 0;

    // resolve the value of every declared function constant
    std::vector<uint32_t> values(shader->specConstants.size());

    for (size_t i = 0; i < shader->specConstants.size(); i++) {
      const auto& sc = shader->specConstants[i];

      if (sc.id < MaxNumSpecConstants)
        values[i] = specData[sc.id];
      else if (sc.id == MaxNumSpecConstants)
        values[i] = 1u; // gate: spec data is baked into this function
      else
        values[i] = sc.defaultValue;
    }

    auto* cache = shader->functions;
    std::lock_guard<std::mutex> lock(cache->mutex);

    auto entry = cache->functions.find(values);
    if (entry != cache->functions.end())
      return entry->second;

    obj_handle_t function = 0;
    obj_handle_t pool = NSAutoreleasePool_alloc_init();

    if (values.empty()) {
      function = MTLLibrary_newFunction(shader->library, "main0");
      if (!function)
        Logger::err("d9mt: getShaderFunction: main0 not found");
    } else {
      // supply ALL declared function constants — creating the function with
      // any constant missing crashes PSO creation (hard-won lesson)
      std::vector<WMTFunctionConstant> consts(values.size());

      for (size_t i = 0; i < values.size(); i++) {
        std::memset(&consts[i], 0, sizeof(consts[i]));
        consts[i].data.set(&values[i]);
        consts[i].type = shader->specConstants[i].isBool
          ? WMTDataTypeBool
          : WMTDataTypeUInt;
        consts[i].index = uint16_t(shader->specConstants[i].id);
      }

      obj_handle_t err = 0;
      function = MTLLibrary_newFunctionWithConstants(shader->library, "main0",
        consts.data(), uint32_t(consts.size()), &err);

      if (!function)
        logNSError("d9mt: newFunctionWithConstants", err);
      else if (err)
        NSObject_release(err);
    }

    NSObject_release(pool);

    cache->functions.insert({ std::move(values), function });
    return function;
  }

}
