// d9mt: Metal backend — DxvkContext / DxvkCommandList.
//
// Architecture (see docs/METAL-BACKEND-NOTES.md "Stage decisions: context"):
//  - Every DxvkCommandList is backed by one lazily-created MTLCommandBuffer
//    plus an encoder state machine (none/render/blit/compute), held in a
//    process-global side table keyed by the command-list pointer (the
//    vendored class has no usable members for Metal handles).
//  - wmtcmd structs are encoded IMMEDIATELY (encodeCommands is synchronous),
//    so commands can live on the stack — no command arena needed.
//  - Submission: DxvkDevice::submitCommandList commits the MTLCommandBuffer
//    and registers a completion-watcher callback that runs per-submission
//    completion work (EVENT query flips), notifyObjects() (signal +
//    tracked-resource release) and recycles the command list. Empty
//    submissions still signal (watcher cmdbuf==0 path, §7 risk 6).
//  - Clears are deferred in m_deferredClears (upstream semantics) and
//    executed as standalone Metal render passes whose load actions perform
//    the clear; the Draw stage will merge them into real passes.
//  - No Vulkan barriers anywhere: ordering comes from Metal encoder
//    boundaries (single queue, automatic hazard tracking).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "d9mt_backend.h"
#include "d9mt_trace.h"
#include "d9mt_hud.h"
#include "d9mt_draw.h"

#include "../../vendor/dxvk/src/util/thread.h"

#include "../../vendor/dxvk/src/dxvk/dxvk_cmdlist.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_context.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_gpu_event.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_gpu_query.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_staging.h"

namespace dxvk::d9mt {

  // ==========================================================================
  // GPU-query result side state (Queries stage). Each DxvkGpuQuery pool token
  // carries one result slot here: occlusion queries get the summed-up
  // visibility-result value, timestamp queries the command buffer's GPU end
  // time. Written by the watcher thread at submission retirement (value
  // first, then state with release), polled by DxvkQuery::getData on the app
  // thread (state acquire, then value) — results stay Pending until the
  // submission containing the query really retired (BACKEND-SURFACE §5.3).
  // Entries live for the pool-token lifetime (bounded by the allocator pool
  // sizes) and are reset whenever the token is re-allocated on the CS thread.
  // ==========================================================================

  struct GpuQueryResult {
    static constexpr uint32_t Pending   = 0u;
    static constexpr uint32_t Available = 1u;
    static constexpr uint32_t Failed    = 2u;

    std::atomic<uint32_t> state = { Pending };
    uint64_t              value = 0u;
  };

  GpuQueryResult& gpuQueryResult(const void* gpuQuery) {
    static std::mutex s_mutex;
    static std::unordered_map<const void*, std::unique_ptr<GpuQueryResult>> s_map;

    std::lock_guard<std::mutex> lock(s_mutex);
    auto& slot = s_map[gpuQuery];
    if (!slot)
      slot = std::make_unique<GpuQueryResult>();
    return *slot;
  }

  static void resetGpuQueryResult(const void* gpuQuery) {
    auto& result = gpuQueryResult(gpuQuery);
    result.value = 0u;
    result.state.store(GpuQueryResult::Pending, std::memory_order_relaxed);
  }

  // Marks a query result failed unless it was already resolved. Used for
  // command lists that get reset without ever being submitted, so pollers
  // see Failed instead of hanging on Pending forever.
  static void failGpuQueryResult(const void* gpuQuery) {
    auto& result = gpuQueryResult(gpuQuery);
    uint32_t expected = GpuQueryResult::Pending;
    result.state.compare_exchange_strong(expected, GpuQueryResult::Failed,
      std::memory_order_release, std::memory_order_relaxed);
  }


  // ==========================================================================
  // Command-list side state: MTLCommandBuffer + encoder state machine.
  //
  // Concurrency: the map structure is mutex-guarded (command lists are
  // created/recycled on the CS thread, completion work runs on the watcher
  // thread), but a single entry is never used concurrently — the CS thread
  // is done with a list before the watcher touches it.
  // ==========================================================================

  enum class EncoderKind : uint32_t {
    None, Render, Blit, Compute
  };

  // Open-addressing (linear-probe) set of Metal resource handles, used for
  // per-encoder residency dedup. Replaces std::unordered_set, which allocated a
  // node per insert and freed them all on clear — markResident runs for every
  // bound resource of every draw, so that churn was a real chunk of draw CPU.
  // This keeps a flat power-of-two table: add() is an integer probe, reset()
  // just zero-fills and keeps the capacity (no realloc between passes). Handle 0
  // is the empty sentinel (markResident never adds a null resource).
  struct ResidentSet {
    std::vector<obj_handle_t> slots;   // power-of-two, 0 = empty slot
    size_t mask  = 0;
    size_t count = 0;

    static uint64_t mix(uint64_t h) {  // fibonacci hash + avalanche
      h *= 0x9E3779B97F4A7C15ull;
      return h ^ (h >> 29);
    }

    void reset() {                     // per-encoder clear, keeps capacity
      if (count) {
        std::fill(slots.begin(), slots.end(), obj_handle_t(0));
        count = 0;
      }
    }

    void grow() {
      const size_t newCap = slots.empty() ? 256u : slots.size() * 2u;
      std::vector<obj_handle_t> old(newCap, obj_handle_t(0));
      slots.swap(old);
      mask  = newCap - 1u;
      count = 0;
      for (obj_handle_t h : old) {
        if (!h) continue;
        size_t i = mix(h) & mask;
        while (slots[i]) i = (i + 1u) & mask;
        slots[i] = h;
        count++;
      }
    }

    // Returns true if newly added (was not already resident this encoder).
    bool add(obj_handle_t h) {
      if (slots.empty() || (count + 1u) * 4u >= slots.size() * 3u)
        grow();
      size_t i = mix(h) & mask;
      while (slots[i]) {
        if (slots[i] == h) return false;
        i = (i + 1u) & mask;
      }
      slots[i] = h;
      count++;
      return true;
    }
  };

  struct CmdListState {
    obj_handle_t cmdbuf  = 0;                 // retained
    obj_handle_t encoder = 0;                 // retained while open
    EncoderKind  kind    = EncoderKind::None;

    // per-submission completion work (EVENT query flips etc.), run on the
    // watcher thread when the command buffer retires
    std::vector<std::function<void()>> onComplete;

    // render-encoder-scoped dedupe state (Draw stage); reset whenever a
    // render pass starts on this list
    obj_handle_t lastRenderPso  = 0;
    obj_handle_t lastRenderDsso = 0;
    // resources already made resident on the current render encoder. Flat
    // open-addressing set (see ResidentSet): markResident is called for every
    // bound resource of every draw, so the per-insert node allocation of a
    // std::unordered_set was a measurable chunk of draw-call CPU.
    ResidentSet renderResident;

    // visibility-result state (occlusion queries): one pooled shared-storage
    // MTLBuffer per submission that counts samples; slots are bump-allocated
    // (one per GPU query, new GPU query per encoder restart while active)
    // and summed up on the watcher thread at retirement
    obj_handle_t visBuffer    = 0;            // retained (vis-buffer pool)
    uint64_t*    visMem       = nullptr;      // VirtualAlloc'd, zeroed at acquire
    uint32_t     visSlotsUsed = 0;
    bool         visAttached  = false;        // open render encoder counts into visBuffer
    std::vector<std::pair<Rc<DxvkGpuQuery>, uint32_t>> visSlots;

    // timestamp queries resolved from the command buffer's GPU end time
    std::vector<Rc<DxvkGpuQuery>> tsQueries;

    // render-command batch arena (Draw stage). Render commands are copied here
    // and chained via wmtcmd_base::next, then handed to the bridge in ONE
    // encodeCommands call instead of one Rosetta crossing per command — the
    // dominant CPU cost in draw-call-heavy scenes. Flushed on overflow and in
    // endEncoder (the single choke point for every encoder transition). The
    // buffer is fixed-capacity so appended command addresses stay stable for
    // the chain's lifetime (no reallocation between flushes).
    std::vector<uint8_t> cmdArena;
    size_t               cmdArenaUsed = 0;
    wmtcmd_base*         cmdHead = nullptr;
    wmtcmd_base*         cmdTail = nullptr;
  };

  // Render-command arena capacity. Max single render command is well under
  // 256 bytes; 256 KiB batches ~100+ draws between flushes, so a busy pass
  // crosses Rosetta a few dozen times instead of tens of thousands.
  constexpr size_t CmdArenaCap = 256u * 1024u;

  // 64 KiB of visibility-result slots per submission (one slot per active-
  // occlusion-query encoder span, not per draw — generous for real apps).
  constexpr uint32_t VisSlotCount  = 8192u;
  constexpr size_t   VisBufferSize = size_t(VisSlotCount) * sizeof(uint64_t);

  namespace {
    std::mutex s_cmdListMutex;
    std::unordered_map<const void*, std::unique_ptr<CmdListState>> s_cmdListStates;

    // recycling pool for visibility-result buffers (VirtualAlloc memory
    // wrapped bytes-no-copy, same pattern as the sampler heap)
    struct VisBuffer {
      obj_handle_t buffer = 0;
      void*        mem    = nullptr;
    };
    std::mutex s_visPoolMutex;
    std::vector<VisBuffer> s_visPool;
    constexpr size_t MaxPooledVisBuffers = 8;
  }

  // Attaches a (pooled) zeroed visibility-result buffer to the command list.
  // Returns false on allocation failure (logged; occlusion queries then fail
  // loudly through the Failed result state).
  static bool acquireVisBuffer(CmdListState& state) {
    if (state.visBuffer)
      return true;

    VisBuffer vb = { };
    {
      std::lock_guard<std::mutex> lock(s_visPoolMutex);
      if (!s_visPool.empty()) {
        vb = s_visPool.back();
        s_visPool.pop_back();
      }
    }

    if (!vb.buffer) {
      void* mem = VirtualAlloc(nullptr, VisBufferSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      obj_handle_t device = mtlDevice();

      if (mem && device) {
        WMTBufferInfo info = { };
        info.length  = VisBufferSize;
        info.options = WMTResourceStorageModeShared;
        info.memory.set(mem);

        vb.buffer = MTLDevice_newBuffer(device, &info);
      }

      if (!vb.buffer) {
        logf("d9mt: visibility-result buffer creation failed (mem=%p dev=%llx)",
          mem, (unsigned long long)device);
        if (mem)
          VirtualFree(mem, 0, MEM_RELEASE);
        return false;
      }
      vb.mem = mem;
    }

    std::memset(vb.mem, 0, VisBufferSize);

    state.visBuffer    = vb.buffer;
    state.visMem       = reinterpret_cast<uint64_t*>(vb.mem);
    state.visSlotsUsed = 0;
    return true;
  }

  static void releaseVisBuffer(CmdListState& state) {
    if (!state.visBuffer)
      return;

    VisBuffer vb = { state.visBuffer, state.visMem };
    state.visBuffer    = 0;
    state.visMem       = nullptr;
    state.visSlotsUsed = 0;

    {
      std::lock_guard<std::mutex> lock(s_visPoolMutex);
      if (s_visPool.size() < MaxPooledVisBuffers) {
        s_visPool.push_back(vb);
        return;
      }
    }

    NSObject_release(vb.buffer);
    VirtualFree(vb.mem, 0, MEM_RELEASE);
  }

  static CmdListState& cmdListState(const void* list) {
    std::lock_guard<std::mutex> lock(s_cmdListMutex);
    auto& slot = s_cmdListStates[list];
    if (!slot)
      slot = std::make_unique<CmdListState>();
    return *slot;
  }

  // Render-command batching toggle (D9MT_BATCH=0 reverts to one crossing per
  // command). Default on.
  static bool batchRenderCmdsEnabled() {
    static const bool e = [] {
      const char* v = std::getenv("D9MT_BATCH");
      return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return e;
  }

  // Hands the accumulated render-command chain to the bridge in a single
  // encodeCommands crossing, then resets the arena. No-op if empty or if the
  // encoder is gone. Must run before the render encoder ends or switches.
  static void flushRenderCmds(CmdListState& state) {
    if (!state.cmdHead)
      return;
    if (state.kind == EncoderKind::Render && state.encoder)
      MTLRenderCommandEncoder_encodeCommands(state.encoder, state.cmdHead);
    state.cmdHead = nullptr;
    state.cmdTail = nullptr;
    state.cmdArenaUsed = 0;
  }

  static void endEncoder(CmdListState& state) {
    if (state.encoder) {
      flushRenderCmds(state); // drain pending render commands first
      MTLCommandEncoder_endEncoding(state.encoder);
      NSObject_release(state.encoder);
      state.encoder = 0;
      state.kind = EncoderKind::None;
    }
    state.visAttached = false;
  }

  static void resetCmdListState(const void* list) {
    std::unique_ptr<CmdListState> state;
    {
      std::lock_guard<std::mutex> lock(s_cmdListMutex);
      auto entry = s_cmdListStates.find(list);
      if (entry == s_cmdListStates.end())
        return;
      state = std::move(entry->second);
      s_cmdListStates.erase(entry);
    }

    endEncoder(*state);

    if (state->cmdbuf) {
      NSObject_release(state->cmdbuf);
      state->cmdbuf = 0;
    }

    state->onComplete.clear();

    // queries on a list that never got submitted (reset without commit) must
    // not stay Pending forever — pollers spin on GetData
    for (const auto& entry : state->visSlots)
      failGpuQueryResult(entry.first.ptr());
    state->visSlots.clear();

    for (const auto& query : state->tsQueries)
      failGpuQueryResult(query.ptr());
    state->tsQueries.clear();

    releaseVisBuffer(*state);
  }

  static obj_handle_t ensureCmdBuf(CmdListState& state) {
    if (!state.cmdbuf) {
      obj_handle_t queue = mtlCommandQueue();
      if (!queue)
        return 0;

      obj_handle_t pool = NSAutoreleasePool_alloc_init();
      obj_handle_t cmdbuf = MTLCommandQueue_commandBuffer(queue);
      if (cmdbuf)
        NSObject_retain(cmdbuf);
      else
        logf("d9mt: MTLCommandQueue_commandBuffer failed");
      NSObject_release(pool);

      state.cmdbuf = cmdbuf;
    }
    return state.cmdbuf;
  }

  static obj_handle_t getBlitEncoder(CmdListState& state) {
    if (state.kind == EncoderKind::Blit)
      return state.encoder;

    endEncoder(state);

    if (!ensureCmdBuf(state))
      return 0;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    obj_handle_t enc = MTLCommandBuffer_blitCommandEncoder(state.cmdbuf);
    if (enc)
      NSObject_retain(enc);
    else
      logf("d9mt: blitCommandEncoder failed");
    NSObject_release(pool);

    state.encoder = enc;
    state.kind = enc ? EncoderKind::Blit : EncoderKind::None;
    return enc;
  }

  static obj_handle_t getComputeEncoder(CmdListState& state) {
    if (state.kind == EncoderKind::Compute)
      return state.encoder;

    endEncoder(state);

    if (!ensureCmdBuf(state))
      return 0;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    obj_handle_t enc = MTLCommandBuffer_computeCommandEncoder(state.cmdbuf, false);
    if (enc)
      NSObject_retain(enc);
    else
      logf("d9mt: computeCommandEncoder failed");
    NSObject_release(pool);

    state.encoder = enc;
    state.kind = enc ? EncoderKind::Compute : EncoderKind::None;
    return enc;
  }

  // Encodes a single blit command (next pointer must be zero).
  static void encodeBlitCmd(CmdListState& state, const void* cmd) {
    obj_handle_t enc = getBlitEncoder(state);
    if (enc)
      MTLBlitCommandEncoder_encodeCommands(enc,
        reinterpret_cast<const wmtcmd_base*>(cmd));
  }

  // Encodes a single compute command (next pointer must be zero).
  static void encodeComputeCmd(CmdListState& state, const void* cmd) {
    obj_handle_t enc = getComputeEncoder(state);
    if (enc)
      MTLComputeCommandEncoder_encodeCommands(enc,
        reinterpret_cast<const wmtcmd_base*>(cmd));
  }

  // Standalone render pass with no draws: the load/store actions do all the
  // work (clears / discards). Ends any open encoder first.
  static void encodeEmptyRenderPass(CmdListState& state, WMTRenderPassInfo& pass) {
    endEncoder(state);

    if (!ensureCmdBuf(state))
      return;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(state.cmdbuf, &pass);
    if (enc)
      MTLCommandEncoder_endEncoding(enc);
    else
      logf("d9mt: renderCommandEncoder failed (clear pass %ux%u)",
        pass.render_target_width, pass.render_target_height);
    NSObject_release(pool);
  }

  // Ends recording on a command list and commits its MTLCommandBuffer.
  // Returns the (still state-retained) command buffer handle, or 0 for an
  // empty submission. Called by DxvkDevice::submitCommandList below.
  static obj_handle_t cmdListCommit(const void* list) {
    auto& state = cmdListState(list);
    endEncoder(state);

    if (state.cmdbuf)
      MTLCommandBuffer_commit(state.cmdbuf);

    return state.cmdbuf;
  }

  namespace {
    // Last resolved GPU timestamp (ns). Only the watcher thread writes it;
    // keeps timestamps monotonic across command buffers (GPU end times of
    // separately committed buffers are not strictly ordered by Metal).
    std::atomic<uint64_t> s_lastGpuTimestamp = { 0u };
  }

  // Runs per-submission completion work; called on the watcher thread after
  // the submission's command buffer retired (or immediately, in retirement
  // order, for empty submissions).
  static void cmdListRunCompletionWork(const void* list) {
    auto& state = cmdListState(list);

    // occlusion queries: each GPU query owns one visibility-result slot the
    // GPU has now written; publish value-then-state (release) for getData
    for (const auto& entry : state.visSlots) {
      auto& result = gpuQueryResult(entry.first.ptr());
      result.value = state.visMem ? state.visMem[entry.second] : 0u;
      result.state.store(GpuQueryResult::Available, std::memory_order_release);
    }
    state.visSlots.clear();

    // timestamp queries: command buffer GPU end time in ns
    // (timestampPeriod = 1.0), kept monotonic across submissions
    if (!state.tsQueries.empty()) {
      uint64_t time = state.cmdbuf
        ? MTLCommandBuffer_property(state.cmdbuf, WMTCommandBufferPropertyGPUEndTime)
        : 0u;
      time = std::max(time, s_lastGpuTimestamp.load(std::memory_order_relaxed));
      s_lastGpuTimestamp.store(time, std::memory_order_relaxed);

      for (const auto& query : state.tsQueries) {
        auto& result = gpuQueryResult(query.ptr());
        result.value = time;
        result.state.store(GpuQueryResult::Available, std::memory_order_release);
      }
      state.tsQueries.clear();
    }

    for (const auto& fn : state.onComplete)
      fn();

    state.onComplete.clear();
  }


  // --------------------------------------------------------------------------
  // encoder bridge for external TUs (declared in d9mt_backend.h; consumed by
  // the swapchain blitter in d9mt_presenter.cpp)
  // --------------------------------------------------------------------------

  obj_handle_t cmdListGetBlitEncoder(const void* list) {
    return getBlitEncoder(cmdListState(list));
  }


  obj_handle_t cmdListBeginRenderPass(const void* list, WMTRenderPassInfo& pass) {
    auto& state = cmdListState(list);
    endEncoder(state);

    if (!ensureCmdBuf(state))
      return 0;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(state.cmdbuf, &pass);
    if (enc)
      NSObject_retain(enc);
    else
      logf("d9mt: renderCommandEncoder failed (external pass %ux%u)",
        pass.render_target_width, pass.render_target_height);
    NSObject_release(pool);

    state.encoder = enc;
    state.kind = enc ? EncoderKind::Render : EncoderKind::None;
    return enc;
  }


  void cmdListEndEncoder(const void* list) {
    endEncoder(cmdListState(list));
  }


  // Compute bridge for the D3D9FormatHelper conversion path: the vendored
  // DxvkCommandList::cmdBindPipeline / cmdDispatch route through the fake
  // Vulkan device dispatch (d9mt_device.cpp) into these. The threadgroup
  // size travels with the PSO (wmtcmd_compute_setpso carries both).
  void cmdListBindComputePipeline(const void* list, obj_handle_t pso,
    const WMTSize& threadgroupSize) {
    auto& state = cmdListState(list);

    wmtcmd_compute_setpso cmd = { };
    cmd.type = WMTComputeCommandSetPSO;
    cmd.pso = pso;
    cmd.threadgroup_size = threadgroupSize;
    encodeComputeCmd(state, &cmd);
  }


  void cmdListDispatch(const void* list, uint32_t x, uint32_t y, uint32_t z) {
    auto& state = cmdListState(list);

    wmtcmd_compute_dispatch cmd = { };
    cmd.type = WMTComputeCommandDispatch;
    cmd.size = { x, y, z };
    encodeComputeCmd(state, &cmd);
  }


  // ==========================================================================
  // Draw stage support (consumed by the DxvkContext draw path below; see
  // METAL-BACKEND-NOTES.md "Stage decisions: draw").
  // ==========================================================================

  // Encodes one render command on the list's open render encoder. With
  // batching on (default), the command is copied into the per-list arena and
  // chained via wmtcmd_base::next; the whole chain crosses Rosetta once at the
  // next flush (overflow or endEncoder) instead of one crossing here. T is the
  // concrete wmtcmd_render_* type, so sizeof(T) gives the exact copy size and
  // every such struct shares the wmtcmd_base header layout (type/reserved/next
  // at the same offsets), making the reinterpret to set ::next valid.
  template<typename T>
  static void encodeRenderCmd(CmdListState& state, const T* cmd) {
    if (!(state.kind == EncoderKind::Render && state.encoder))
      return;

    if (!batchRenderCmdsEnabled()) {
      MTLRenderCommandEncoder_encodeCommands(state.encoder,
        reinterpret_cast<const wmtcmd_base*>(cmd));
      return;
    }

    constexpr size_t sz   = sizeof(T);
    constexpr size_t need = (sz + 7u) & ~size_t(7u); // 8-byte align entries

    if (state.cmdArena.size() < CmdArenaCap)
      state.cmdArena.resize(CmdArenaCap); // one-time, then stable

    if (state.cmdArenaUsed + need > CmdArenaCap)
      flushRenderCmds(state);

    uint8_t* dst = state.cmdArena.data() + state.cmdArenaUsed;
    std::memcpy(dst, cmd, sz);
    state.cmdArenaUsed += need;

    auto* base = reinterpret_cast<wmtcmd_base*>(dst);
    base->next.set(nullptr);

    if (!state.cmdHead)
      state.cmdHead = base;
    else
      state.cmdTail->next.set(base);

    state.cmdTail = base;
  }

  // useResource for indirectly referenced resources (argument-buffer words);
  // deduped per render encoder.
  static void markResident(CmdListState& state, obj_handle_t resource) {
    if (!resource)
      return;

    if (!state.renderResident.add(resource))
      return; // already resident on this encoder

    wmtcmd_render_useresource cmd = { };
    cmd.type = WMTRenderCommandUseResource;
    cmd.resource = resource;
    cmd.usage = WMTResourceUsageRead;
    cmd.stages = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
    encodeRenderCmd(state, &cmd);
  }


  // VkFormat -> MTLVertexFormat raw value (0 = unsupported). Covers every
  // format DecodeDecltype (d3d9_util.h) can emit. USCALED/SSCALED map to
  // Metal's unnormalized integer formats: when the shader declares a float
  // attribute, Metal numerically converts the integer — exactly the SCALED
  // semantics (§7 risk 1 resolution).
  static uint32_t vkVertexFormatToMtl(VkFormat format) {
    switch (uint32_t(format)) {
      case VK_FORMAT_R32_SFLOAT:                return 28; // Float
      case VK_FORMAT_R32G32_SFLOAT:             return 29; // Float2
      case VK_FORMAT_R32G32B32_SFLOAT:          return 30; // Float3
      case VK_FORMAT_R32G32B32A32_SFLOAT:       return 31; // Float4
      case VK_FORMAT_B8G8R8A8_UNORM:            return 42; // UChar4Normalized_BGRA
      case VK_FORMAT_R8G8B8A8_UNORM:            return 9;  // UChar4Normalized
      case VK_FORMAT_R8G8B8A8_UINT:             return 3;  // UChar4
      case VK_FORMAT_R8G8B8A8_USCALED:          return 3;  // UChar4 (int->float)
      case VK_FORMAT_R8G8B8A8_SSCALED:          return 6;  // Char4 (int->float)
      case VK_FORMAT_R16G16_SINT:               return 16; // Short2
      case VK_FORMAT_R16G16B16A16_SINT:         return 18; // Short4
      case VK_FORMAT_R16G16_SSCALED:            return 16; // Short2 (int->float)
      case VK_FORMAT_R16G16B16A16_SSCALED:      return 18; // Short4 (int->float)
      case VK_FORMAT_R16G16_SNORM:              return 22; // Short2Normalized
      case VK_FORMAT_R16G16B16A16_SNORM:        return 24; // Short4Normalized
      case VK_FORMAT_R16G16_UNORM:              return 19; // UShort2Normalized
      case VK_FORMAT_R16G16B16A16_UNORM:        return 21; // UShort4Normalized
      case VK_FORMAT_R16G16_SFLOAT:             return 25; // Half2
      case VK_FORMAT_R16G16B16A16_SFLOAT:       return 27; // Half4
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:  return 41; // UInt1010102Normalized
      case VK_FORMAT_A2B10G10R10_SNORM_PACK32:  return 40; // Int1010102Normalized
      default:                                  return 0;  // incl. UDEC3 (USCALED 1010102)
    }
  }


  static WMTBlendFactor vkBlendFactorToMtl(VkBlendFactor factor) {
    switch (factor) {
      default:
      case VK_BLEND_FACTOR_ZERO:                     return WMTBlendFactorZero;
      case VK_BLEND_FACTOR_ONE:                      return WMTBlendFactorOne;
      case VK_BLEND_FACTOR_SRC_COLOR:                return WMTBlendFactorSourceColor;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return WMTBlendFactorOneMinusSourceColor;
      case VK_BLEND_FACTOR_DST_COLOR:                return WMTBlendFactorDestinationColor;
      case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return WMTBlendFactorOneMinusDestinationColor;
      case VK_BLEND_FACTOR_SRC_ALPHA:                return WMTBlendFactorSourceAlpha;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return WMTBlendFactorOneMinusSourceAlpha;
      case VK_BLEND_FACTOR_DST_ALPHA:                return WMTBlendFactorDestinationAlpha;
      case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return WMTBlendFactorOneMinusDestinationAlpha;
      case VK_BLEND_FACTOR_CONSTANT_COLOR:           return WMTBlendFactorBlendColor;
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return WMTBlendFactorOneMinusBlendColor;
      case VK_BLEND_FACTOR_CONSTANT_ALPHA:           return WMTBlendFactorBlendAlpha;
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return WMTBlendFactorOneMinusBlendAlpha;
      case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:       return WMTBlendFactorSourceAlphaSaturated;
      case VK_BLEND_FACTOR_SRC1_COLOR:               return WMTBlendFactorSource1Color;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:     return WMTBlendFactorOneMinusSource1Color;
      case VK_BLEND_FACTOR_SRC1_ALPHA:               return WMTBlendFactorSource1Alpha;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:     return WMTBlendFactorOneMinusSource1Alpha;
    }
  }


  // Vk write mask bits R=1,G=2,B=4,A=8 -> WMT R=8,G=4,B=2,A=1
  static uint32_t vkWriteMaskToMtl(VkColorComponentFlags mask) {
    return ((mask & VK_COLOR_COMPONENT_R_BIT) ? WMTColorWriteMaskRed   : 0u)
         | ((mask & VK_COLOR_COMPONENT_G_BIT) ? WMTColorWriteMaskGreen : 0u)
         | ((mask & VK_COLOR_COMPONENT_B_BIT) ? WMTColorWriteMaskBlue  : 0u)
         | ((mask & VK_COLOR_COMPONENT_A_BIT) ? WMTColorWriteMaskAlpha : 0u);
  }


  // VK_PRIMITIVE_TOPOLOGY_* -> WMTPrimitiveType; TRIANGLE_FAN needs the
  // synthesized-index emulation, everything else is 1:1.
  static bool vkTopologyToMtl(VkPrimitiveTopology topology, WMTPrimitiveType* out) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     *out = WMTPrimitiveTypePoint;         return true;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      *out = WMTPrimitiveTypeLine;          return true;
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     *out = WMTPrimitiveTypeLineStrip;     return true;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  *out = WMTPrimitiveTypeTriangle;      return true;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: *out = WMTPrimitiveTypeTriangleStrip; return true;
      default:                                   return false;
    }
  }


  // --------------------------------------------------------------------------
  // render PSO cache: keyed by the shader pair + the FULL packed pipeline
  // state (DxvkGraphicsPipelineStateInfo; vertex strides are written into
  // ilBindings before lookup since the front-end uses dynamic strides).
  // Covers BACKEND-SURFACE §4.7 exactly: vertex layout, attachment formats +
  // sample count, per-MRT blend/writeMask, alphaToCoverage, topology class
  // (via ia), 6 spec dwords (sc), flat shading (rs), RT swizzles.
  // Process-global, entries live for the process lifetime (upstream pipeline
  // managers have the same policy); Rc refs keep the shaders alive.
  // --------------------------------------------------------------------------

  struct PsoEntry {
    // pso is the readiness signal. 0 = not ready (async compile pending) OR
    // permanently failed; non-zero = ready to bind. Written LAST by the
    // compile (worker or sync) with release semantics; the CS thread loads it
    // with acquire at the draw site, so vs/fs below are visible once non-zero.
    std::atomic<obj_handle_t> pso { 0 };
    const CompiledShader* vs  = nullptr;
    const CompiledShader* fs  = nullptr;
    Rc<DxvkShader>        vsRef;
    Rc<DxvkShader>        fsRef;
  };

  struct PsoKey {
    DxvkShader* vs = nullptr;
    DxvkShader* fs = nullptr;
    DxvkGraphicsPipelineStateInfo state;

    bool operator == (const PsoKey& other) const {
      return vs == other.vs && fs == other.fs && state == other.state;
    }
  };

  struct PsoKeyHash {
    size_t operator () (const PsoKey& key) const {
      size_t hash = std::hash<void*>()(key.vs) * 31u
                  ^ std::hash<void*>()(key.fs);
      const uint32_t* words = reinterpret_cast<const uint32_t*>(&key.state);
      for (size_t i = 0; i < sizeof(key.state) / sizeof(uint32_t); i++)
        hash = hash * 16777619u ^ words[i];
      return hash;
    }
  };

  namespace {
    std::mutex s_psoMutex;
    std::unordered_map<PsoKey, std::unique_ptr<PsoEntry>, PsoKeyHash> s_psoCache;
  }

  // Persist a PSO record for next-launch pre-warm, but ONLY for pipelines that
  // actually compiled (called from compilePso after success). Recording only
  // proven-good state is what keeps replay safe: a state that fails to build is
  // never written, so it can never be replayed into a permanently-null cache
  // entry that would stall its draws. No-ops if pre-warm is off. Thread-safe
  // (runs on worker threads). Defined with the pre-warm machinery below.
  static void prewarmRecordCompiled(
    uint64_t vsHash, uint64_t fsHash, const void* state, uint32_t stateSize);

  // Builds the Metal render PSO into a preallocated entry whose vsRef/fsRef
  // are already set (caller holds no locks; safe to run on a worker thread —
  // touches only the passed entry, the mutex-guarded shader caches, and
  // winemetal unixcalls, never s_psoCache). Leaves entry->pso == 0 on failure
  // (failures are cached: the same broken state would just fail every draw).
  // entry->pso is the LAST thing written, with release ordering, so once the
  // CS thread observes it non-zero every other field is visible.
  static void compilePso(
          PsoEntry*      entry,
    const PsoKey&        key) {
    const Rc<DxvkShader>& vs = entry->vsRef;
    const Rc<DxvkShader>& fs = entry->fsRef;

    // module fixups: undefined-input elimination is LOAD-BEARING on Metal
    // (an FS stage_in input with no matching VS output fails PSO creation),
    // RT swizzles compensate swizzle-less RTVs, flat shading per rs state
    DxvkShaderModuleCreateInfo vsInfo;

    DxvkShaderModuleCreateInfo fsInfo;
    fsInfo.fsDualSrcBlend  = key.state.useDualSourceBlending();
    fsInfo.fsFlatShading   = key.state.rs.flatShading()
                          && fs->info().flatShadingInputs;
    fsInfo.undefinedInputs = fs->info().inputMask & ~vs->info().outputMask;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++)
      fsInfo.rtSwizzles[i] = key.state.omSwizzle[i].mapping();

    entry->vs = getCompiledShader(vs, vsInfo);
    entry->fs = getCompiledShader(fs, fsInfo);

    if (!entry->vs || !entry->fs)
      return;

    obj_handle_t vsFn = getShaderFunction(entry->vs, key.state.sc.specConstants);
    obj_handle_t fsFn = getShaderFunction(entry->fs, key.state.sc.specConstants);

    if (!vsFn || !fsFn)
      return;

    d9mt_pso_info info;
    std::memset(&info, 0, sizeof(info));
    info.vertex_function   = vsFn;
    info.fragment_function = fsFn;

    // color attachments + blend state
    for (uint32_t i = 0; i < MaxNumRenderTargets && i < 8u; i++) {
      VkFormat format = key.state.rt.getColorFormat(i);
      if (!format)
        continue;

      WMTPixelFormat wmt = wmtFormatFor(format);
      if (wmt == WMTPixelFormatInvalid) {
        Logger::err(str::format("d9mt: PSO: unsupported color format ", uint32_t(format)));
        return;
      }

      const auto& blend = key.state.omBlend[i];
      auto& color = info.colors[i];
      color.pixel_format = uint32_t(wmt);
      color.write_mask   = vkWriteMaskToMtl(blend.colorWriteMask());
      color.blending_enabled = blend.blendEnable() ? 1u : 0u;

      if (blend.blendEnable()) {
        color.rgb_blend_op          = uint32_t(blend.colorBlendOp());
        color.alpha_blend_op        = uint32_t(blend.alphaBlendOp());
        color.src_rgb_blend_factor  = uint32_t(vkBlendFactorToMtl(blend.srcColorBlendFactor()));
        color.dst_rgb_blend_factor  = uint32_t(vkBlendFactorToMtl(blend.dstColorBlendFactor()));
        color.src_alpha_blend_factor = uint32_t(vkBlendFactorToMtl(blend.srcAlphaBlendFactor()));
        color.dst_alpha_blend_factor = uint32_t(vkBlendFactorToMtl(blend.dstAlphaBlendFactor()));
      }
    }

    // unified depth-stencil format (all depth views are Depth32Float_Stencil8)
    if (key.state.rt.getDepthStencilFormat()) {
      info.depth_pixel_format   = uint32_t(WMTPixelFormatDepth32Float_Stencil8);
      info.stencil_pixel_format = uint32_t(WMTPixelFormatDepth32Float_Stencil8);
    }

    // VkSampleCountFlagBits values equal the sample count
    uint32_t sampleCount = key.state.ms.sampleCount();
    info.raster_sample_count = sampleCount ? sampleCount : 1u;
    info.alpha_to_coverage   = key.state.ms.enableAlphaToCoverage() ? 1u : 0u;

    if (key.state.ms.sampleMask() != 0xffffu) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: PSO: non-trivial sample mask ignored (no Metal equivalent)");
    }

    // vertex descriptor
    uint32_t attrCount = key.state.il.attributeCount();
    uint32_t bindCount = key.state.il.bindingCount();

    if (attrCount > 18u || bindCount > 16u) {
      Logger::err(str::format("d9mt: PSO: vertex layout too large (",
        attrCount, " attributes, ", bindCount, " bindings)"));
      return;
    }

    for (uint32_t i = 0; i < attrCount; i++) {
      const auto& attr = key.state.ilAttributes[i];

      uint32_t format = vkVertexFormatToMtl(attr.format());
      if (!format) {
        Logger::err(str::format("d9mt: PSO: unsupported vertex format ",
          uint32_t(attr.format())));
        return;
      }

      info.attributes[i].format       = format;
      info.attributes[i].offset       = attr.offset();
      info.attributes[i].buffer_index = VertexBufferBase + attr.binding();
      info.attributes[i].location     = attr.location();
    }
    info.num_attributes = attrCount;

    for (uint32_t i = 0; i < bindCount; i++) {
      const auto& binding = key.state.ilBindings[i];

      auto& layout = info.layouts[i];
      layout.buffer_index = VertexBufferBase + binding.binding();
      layout.stride       = binding.stride();

      if (!layout.stride) {
        // Metal validation rejects stride 0 even for constant-step layouts:
        // use the binding's attribute extent (null stream / stride-0 reads
        // always fetch element 0 via the Constant step function below)
        uint32_t extent = 4u;
        for (uint32_t a = 0; a < attrCount; a++) {
          const auto& attr = key.state.ilAttributes[a];
          if (attr.binding() == binding.binding()) {
            extent = std::max(extent, attr.offset()
              + uint32_t(lookupFormatInfo(attr.format())->elementSize));
          }
        }
        layout.stride = align(extent, 4u);
      }

      if (binding.inputRate() == VK_VERTEX_INPUT_RATE_INSTANCE) {
        if (binding.divisor()) {
          layout.step_function = 2u; // MTLVertexStepFunctionPerInstance
          layout.step_rate     = binding.divisor();
        } else {
          layout.step_function = 0u; // Constant (divisor 0: same data for all)
          layout.step_rate     = 0u;
        }
      } else if (!binding.stride()) {
        layout.step_function = 0u; // Constant (stride 0 / null stream)
        layout.step_rate     = 0u;
      } else {
        layout.step_function = 1u; // MTLVertexStepFunctionPerVertex
        layout.step_rate     = 1u;
      }
    }
    info.num_layouts = bindCount;

    d9mt_newpso_params params;
    std::memset(&params, 0, sizeof(params));
    params.device   = mtlDevice();
    params.info_ptr = uint64_t(uintptr_t(&info));

    int status = D9MT_UnixCall(D9MT_FUNC_NEW_RENDER_PSO, &params);
    if (status != 0 || !params.ret_pso) {
      Logger::err(str::format("d9mt: PSO creation failed, status ", status));
      if (params.ret_error)
        logNSError("d9mt: newRenderPipelineState", params.ret_error);
      return;
    }
    if (params.ret_error)
      NSObject_release(params.ret_error);

    // release store: publishes vs/fs and the whole entry to the CS thread
    entry->pso.store(params.ret_pso, std::memory_order_release);

    // Compile succeeded -> this exact (shaders + state) is buildable, so it is
    // safe to persist for next-launch pre-warm. Recording here (not at the
    // miss site) is the invariant that makes replay correct: only pipelines
    // proven to build ever reach the cache file. Deduped + no-op when off.
    prewarmRecordCompiled(uint64_t(vs->getHash()), uint64_t(fs->getHash()),
                          &key.state, uint32_t(sizeof(key.state)));
  }

  // --------------------------------------------------------------------------
  // Asynchronous pipeline compilation (mirrors DXVK's DxvkPipelineWorkers).
  //
  // D3D9 titles materialize new shader+pipeline-state combinations continuously
  // as they stream content; compiling those synchronously on the CS/draw thread
  // stalls the frame for as long as the Metal pipeline compile takes (tens to
  // hundreds of ms for a single state, far more for a burst). DXVK's base/fast
  // dual-pipeline trick doesn't map to Metal — there is no cheap base pipeline,
  // every PSO is a full monolithic compile — so we mirror the dxvk-async model
  // instead: hand the compile to a background worker pool and SKIP the draw
  // until the pipeline is hot. The draw site already returns false on pso == 0,
  // so a not-ready pipeline simply defers the affected geometry by a frame or
  // two while the frame thread keeps running. Toggle with D9MT_ASYNC=0.
  // --------------------------------------------------------------------------

  namespace {

    bool asyncPsoEnabled() {
      static const bool e = [] {
        const char* v = std::getenv("D9MT_ASYNC");
        return !(v && v[0] == '0' && v[1] == '\0'); // default on
      }();
      return e;
    }

    class PsoWorkers {

    public:

      // Enqueue a placeholder entry (vsRef/fsRef preset, pso == 0) for
      // background compilation. The entry pointer is stable for the process
      // lifetime (heap node owned by s_psoCache, never erased).
      void enqueue(const PsoKey& key, PsoEntry* entry) {
        {
          std::unique_lock<dxvk::mutex> lk(m_mutex);
          ensureStartedLocked();
          m_queue.push_back(Job{ key, entry });
        }
        m_cond.notify_one();
      }

      ~PsoWorkers() {
        {
          std::unique_lock<dxvk::mutex> lk(m_mutex);
          m_stop = true;
        }
        m_cond.notify_all();
        for (auto& t : m_threads) {
          if (t.joinable())
            t.join();
        }
      }

    private:

      struct Job {
        PsoKey    key;
        PsoEntry* entry;
      };

      void ensureStartedLocked() {
        if (m_started)
          return;
        m_started = true;

        uint32_t hw = dxvk::thread::hardware_concurrency();
        uint32_t n  = hw > 3u ? std::min(4u, hw - 2u) : 1u;

        for (uint32_t i = 0; i < n; i++) {
          m_threads.emplace_back([this] { run(); });
          // Lowest priority: a compile burst (entering a new area) must not
          // preempt the CS/draw thread off its core — that starvation is what
          // turns a background compile into a visible frame stutter. The
          // unixcall runs the native Metal compile on this same OS thread, so
          // de-prioritizing here de-prioritizes the heavy work too. Compiles
          // just take the spare cycles; geometry pops in a touch later.
          m_threads.back().set_priority(dxvk::ThreadPriority::Lowest);
        }

        d9mt::logf("d9mt: async PSO workers started (%u threads, low priority)", n);
      }

      void run() {
        for (;;) {
          Job job;
          {
            std::unique_lock<dxvk::mutex> lk(m_mutex);
            m_cond.wait(lk, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) {
              if (m_stop)
                return;
              continue;
            }
            job = m_queue.front();
            m_queue.pop_front();
          }

          // Heavy compile off the frame thread. compilePso touches only the
          // entry + the mutex-guarded shader caches + unixcalls, never the
          // PSO map, so no s_psoMutex is needed here.
          compilePso(job.entry, job.key);
        }
      }

      dxvk::mutex                m_mutex;
      dxvk::condition_variable   m_cond;
      std::deque<Job>            m_queue;
      std::vector<dxvk::thread>  m_threads;
      bool                       m_started = false;
      bool                       m_stop    = false;

    };

    // Declared AFTER s_psoCache so it destructs (and joins workers) FIRST,
    // before the cache they reference is torn down.
    PsoWorkers s_psoWorkers;

  }

  // ==========================================================================
  // Persistent PSO pre-warm ("state cache"). The metallib (shader source->AIR)
  // is disk-cached, but the PSO objects (newRenderPipelineState, vs+fs+state)
  // are rebuilt lazily on first draw each run -> geometry pops in. This records
  // every PSO key to a per-game file keyed by shader CONTENT hash (getHash(),
  // stable across runs because it derives from the SPIR-V SHA-1), and on the
  // next launch pre-compiles each recorded PSO as soon as both of its shaders
  // have been seen — so they're ready before the draw. Correctness is never at
  // risk from hash collisions: the real draw matches by shader POINTER+state, so
  // a collided pre-warm entry simply never matches (a wasted compile, not a bug).
  // CS-thread only (called under s_psoMutex). D9MT_PSO_PREWARM=0 disables.
  // ==========================================================================
  namespace {
    bool psoPrewarmEnabled() {
      // Pre-warm replays recorded PSOs as their shaders are first seen, so warm
      // runs reach steady state faster (this driver's analogue of DXVK's
      // dxvk_state_cache). Only pipelines that have actually compiled are ever
      // persisted (see prewarmRecordCompiled), so every replayed record is
      // known-buildable — replay can't produce a permanently-null PSO entry that
      // would stall its draws. D9MT_PSO_PREWARM=0 disables it; needs async PSO on.
      static const bool e = [] {
        const char* v = std::getenv("D9MT_PSO_PREWARM");
        return !(v && v[0] == '0' && v[1] == '\0');
      }();
      return e;
    }

    struct PrewarmRec { uint64_t vsHash; uint64_t fsHash; std::vector<uint8_t> state; bool done; };
    std::vector<PrewarmRec>                              s_prewarmRecords; // disk records to replay
    std::unordered_map<uint64_t, std::vector<uint32_t>>  s_pendingByHash;  // shaderHash -> record idxs
    std::unordered_map<uint64_t, Rc<DxvkShader>>         s_shaderByHash;   // shaders seen this run
    uint32_t s_prewarmRemaining = 0;  // records not yet built (when 0 we free everything)
    FILE*    s_prewarmFile   = nullptr;
    bool     s_prewarmLoaded = false;
    constexpr uint32_t kPrewarmMagic = 0x57503944u; // 'D9PW'
    // v2: records are now written only after a successful compile (v1 wrote them
    // at the miss site, before the pipeline was proven to build, so a v1 file can
    // contain states that fail on replay -> stalled draws). Reject v1 files.
    constexpr uint32_t kPrewarmVer   = 2u;

    // The cache file is now written from the PSO worker threads (record-on-
    // success), so file access + the dedup set need their own lock — separate
    // from s_psoMutex, which the workers deliberately never take. Lock order is
    // only ever s_psoMutex -> s_prewarmFileMutex (prewarmLoad); workers take
    // s_prewarmFileMutex alone, so there is no inversion.
    std::mutex                   s_prewarmFileMutex;
    std::unordered_set<uint64_t> s_prewarmPersisted; // (vs,fs,state) digests on disk

    // FNV-1a over (vsHash, fsHash, state bytes): the dedup identity of a record,
    // so a state already on disk (from a prior run or earlier this run) is never
    // written twice and never replays as a duplicate compile.
    uint64_t prewarmDigest(uint64_t vsHash, uint64_t fsHash,
                           const void* state, uint32_t n) {
      uint64_t h = 1469598103934665603ull;
      auto mix = [&](const uint8_t* p, size_t len) {
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
      };
      mix(reinterpret_cast<const uint8_t*>(&vsHash), 8);
      mix(reinterpret_cast<const uint8_t*>(&fsHash), 8);
      mix(reinterpret_cast<const uint8_t*>(state), n);
      return h;
    }

    void prewarmLoad(uint32_t stateSize) {
      // Holds the file lock so the open file handle + dedup set are fully
      // published before any worker thread can record against them.
      std::lock_guard<std::mutex> lk(s_prewarmFileMutex);
      s_prewarmLoaded = true;
      bool valid = false;
      if (FILE* f = std::fopen("d9mt_pso_cache.bin", "rb")) {
        uint32_t magic = 0, ver = 0, ss = 0;
        if (std::fread(&magic, 4, 1, f) == 1 && std::fread(&ver, 4, 1, f) == 1 &&
            std::fread(&ss, 4, 1, f) == 1 && magic == kPrewarmMagic &&
            ver == kPrewarmVer && ss == stateSize) {
          valid = true;
          uint64_t h[2];
          std::vector<uint8_t> st(stateSize);
          while (std::fread(h, 8, 2, f) == 2 &&
                 std::fread(st.data(), 1, stateSize, f) == stateSize) {
            uint32_t idx = uint32_t(s_prewarmRecords.size());
            s_prewarmRecords.push_back({h[0], h[1], st, false});
            // Index by BOTH shaders so a record is found in O(records-for-this-shader)
            // when either of its shaders is seen — no O(all-pending) scan, so the
            // shader-creation hook holds s_psoMutex only briefly (no draw-thread stutter).
            s_pendingByHash[h[0]].push_back(idx);
            if (h[1] != h[0]) s_pendingByHash[h[1]].push_back(idx);
            // Remember it as already-on-disk so record-on-success won't rewrite it.
            s_prewarmPersisted.insert(prewarmDigest(h[0], h[1], st.data(), stateSize));
          }
          s_prewarmRemaining = uint32_t(s_prewarmRecords.size());
        }
        std::fclose(f);
      }
      // Append to a valid file; otherwise (new/stale/version-bumped) rewrite the header.
      s_prewarmFile = std::fopen("d9mt_pso_cache.bin", valid ? "ab" : "wb");
      if (s_prewarmFile && !valid) {
        std::fwrite(&kPrewarmMagic, 4, 1, s_prewarmFile);
        std::fwrite(&kPrewarmVer, 4, 1, s_prewarmFile);
        std::fwrite(&stateSize, 4, 1, s_prewarmFile);
        std::fflush(s_prewarmFile);
        s_prewarmRecords.clear(); s_pendingByHash.clear(); s_prewarmRemaining = 0;
        s_prewarmPersisted.clear();
      }
      d9mt::logf("d9mt: PSO pre-warm: %u recorded PSOs to replay", s_prewarmRemaining);
    }
  }

  // Persist a proven-good PSO (called from compilePso on a worker after the
  // pipeline compiled). Deduped against everything already on disk, and a no-op
  // when pre-warm is off — so the cache file only ever accumulates pipelines
  // that are known to build, which is what makes next-launch replay safe.
  static void prewarmRecordCompiled(
      uint64_t vsHash, uint64_t fsHash, const void* state, uint32_t stateSize) {
    if (!psoPrewarmEnabled() || !asyncPsoEnabled())
      return;
    std::lock_guard<std::mutex> lk(s_prewarmFileMutex);
    if (!s_prewarmFile)
      return;
    if (!s_prewarmPersisted.insert(prewarmDigest(vsHash, fsHash, state, stateSize)).second)
      return; // already on disk
    uint64_t h[2] = { vsHash, fsHash };
    std::fwrite(h, 8, 2, s_prewarmFile);
    std::fwrite(state, 1, stateSize, s_prewarmFile);
    std::fflush(s_prewarmFile);
  }

  // fwd: defined just below, builds+enqueues a PSO from a key (caller holds s_psoMutex)
  static void prewarmShaderSeen(uint64_t newHash);

  static const PsoEntry* getRenderPso(
    const PsoKey&         key,
    const Rc<DxvkShader>& vs,
    const Rc<DxvkShader>& fs) {
    // The PSO map is mutated ONLY here, on the single CS thread, under the
    // lock. Workers only write entry->pso (atomic) on their own entry; they
    // never touch the map, so cache lookups/inserts here are race-free.
    std::lock_guard<std::mutex> lock(s_psoMutex);

    // Pre-warm: register these shaders by content hash; the first time each one
    // is seen, replay every recorded PSO that uses it (compiled in the background)
    // so its variants are ready before their draw. Only meaningful with async on.
    const bool prewarm = psoPrewarmEnabled() && asyncPsoEnabled();
    if (prewarm) {
      if (!s_prewarmLoaded) prewarmLoad(uint32_t(sizeof(key.state)));
      // Memory bound: only register/retain shaders WHILE there are recorded PSOs
      // still waiting to be pre-warmed. Once they drain (warmup done), prewarmShaderSeen
      // frees s_shaderByHash + the index, so we don't hold every shader Rc for the whole
      // session — the replay machinery costs nothing post-warmup.
      if (s_prewarmRemaining) {
        uint64_t vh = uint64_t(vs->getHash());
        uint64_t fh = uint64_t(fs->getHash());
        if (s_shaderByHash.emplace(vh, vs).second) prewarmShaderSeen(vh);
        if (s_shaderByHash.emplace(fh, fs).second) prewarmShaderSeen(fh);
      }
    }

    {
      D9MT_ZONE(d9mt::ZonePsoLookup);
      auto it = s_psoCache.find(key);
      if (it != s_psoCache.end())
        return it->second.get(); // ready, compiling, or permanently failed
    }

    // Miss: insert a placeholder holding the shader refs, then either kick a
    // background compile (async) or compile inline (D9MT_ASYNC=0 fallback).
    auto entry = std::make_unique<PsoEntry>();
    entry->vsRef = vs;
    entry->fsRef = fs;
    PsoEntry* ptr = entry.get();
    s_psoCache.emplace(key, std::move(entry));

    // NOTE: the record is NOT written here. The pipeline isn't proven to build
    // yet — compilePso persists it (deduped) only after a successful compile, so
    // a state that fails never lands in the cache file to be replayed next run.

    if (asyncPsoEnabled()) {
      // pso stays 0 until the worker finishes; the draw site skips until then.
      s_psoWorkers.enqueue(key, ptr);
    } else {
      D9MT_ZONE(d9mt::ZonePsoCreate);
      compilePso(ptr, key);
    }

    return ptr;
  }

  // Caller holds s_psoMutex. A shader (newHash) just became known this run: build
  // + background-compile every recorded PSO that uses it whose OTHER shader is also
  // known now, so it's ready before its draw. Matches by shader pointer at draw
  // time, so a hash collision here only wastes a compile (never binds wrong state).
  static void prewarmShaderSeen(uint64_t newHash) {
    auto it = s_pendingByHash.find(newHash);
    if (it == s_pendingByHash.end())
      return;
    for (uint32_t idx : it->second) {
      PrewarmRec& rec = s_prewarmRecords[idx];
      if (rec.done)
        continue;
      auto vsIt = s_shaderByHash.find(rec.vsHash);
      auto fsIt = s_shaderByHash.find(rec.fsHash);
      if (vsIt == s_shaderByHash.end() || fsIt == s_shaderByHash.end())
        continue; // the other shader isn't known yet; fires when it is (its hash list)
      PsoKey key;
      key.vs = vsIt->second.ptr();
      key.fs = fsIt->second.ptr();
      std::memcpy(&key.state, rec.state.data(), sizeof(key.state));
      if (s_psoCache.find(key) == s_psoCache.end()) {
        auto entry = std::make_unique<PsoEntry>();
        entry->vsRef = vsIt->second;
        entry->fsRef = fsIt->second;
        PsoEntry* ptr = entry.get();
        s_psoCache.emplace(key, std::move(entry));
        s_psoWorkers.enqueue(key, ptr); // background pre-compile
      }
      rec.done = true;
      if (s_prewarmRemaining) s_prewarmRemaining--;
    }
    // newHash fully processed: undone records here are still reachable via their OTHER
    // shader's index list, so drop this list to keep the index small.
    s_pendingByHash.erase(it);
    // Warmup done: free the index + retained shader Rcs (the built PSOs keep their own
    // refs via entry->vsRef/fsRef). Bounds replay memory to the warmup window.
    if (s_prewarmRemaining == 0) {
      s_shaderByHash.clear();
      s_pendingByHash.clear();
      s_prewarmRecords.clear();
      s_prewarmRecords.shrink_to_fit();
    }
  }

  // PUBLIC hook (external linkage): called from D3D9DeviceEx::Create{Vertex,Pixel}Shader
  // when the GAME creates a shader — which happens during the LOADING screen, BEFORE
  // any draw. Registering here (instead of only at first draw via getRenderPso) lets us
  // pre-compile every recorded PSO for that shader during load, so the world is fully
  // built before gameplay instead of "filling in" over the first frames. Runs on the
  // app thread, so it takes s_psoMutex (same lock getRenderPso uses on the CS thread);
  // lock order is app(DXVK)→s_psoMutex and CS→s_psoMutex only, so no deadlock.
  void prewarmOnShaderCreated(const Rc<DxvkShader>& shader) {
    if (!shader || !psoPrewarmEnabled() || !asyncPsoEnabled())
      return;
    std::lock_guard<std::mutex> lock(s_psoMutex);
    if (!s_prewarmLoaded)
      prewarmLoad(uint32_t(sizeof(DxvkGraphicsPipelineStateInfo)));
    if (!s_prewarmRemaining)
      return; // nothing recorded to pre-warm (e.g. first ever run, or warmup done)
    uint64_t h = uint64_t(shader->getHash());
    if (s_shaderByHash.emplace(h, shader).second)
      prewarmShaderSeen(h);
  }


  // --------------------------------------------------------------------------
  // depth-stencil state objects, deduped on the packed state + context bits
  // --------------------------------------------------------------------------

  namespace {
    std::mutex s_dssoMutex;
    std::map<std::pair<uint64_t, uint64_t>, obj_handle_t> s_dssoCache;
  }

  static void fillStencilInfo(
          WMTStencilInfo&     out,
    const DxvkStencilOp&      op,
          bool                writable) {
    out.enabled = true;
    out.depth_stencil_pass_op     = WMTStencilOperation(uint32_t(op.passOp()));
    out.stencil_fail_op           = WMTStencilOperation(uint32_t(op.failOp()));
    out.depth_fail_op             = WMTStencilOperation(uint32_t(op.depthFailOp()));
    out.stencil_compare_function  = WMTCompareFunction(uint32_t(op.compareOp()));
    out.read_mask  = op.compareMask();
    out.write_mask = writable ? op.writeMask() : 0u;
  }

  static obj_handle_t getDsso(
    const DxvkDepthStencilState& ds,
          bool                   hasDepthAttachment,
          VkImageAspectFlags     readOnlyAspects) {
    std::pair<uint64_t, uint64_t> key = { 0u, 0u };

    if (hasDepthAttachment) {
      auto packOp = [] (const DxvkStencilOp& op) -> uint64_t {
        return uint64_t(op.passOp())
            | (uint64_t(op.failOp())      << 3)
            | (uint64_t(op.depthFailOp()) << 6)
            | (uint64_t(op.compareOp())   << 9)
            | (uint64_t(op.compareMask()) << 12)
            | (uint64_t(op.writeMask())   << 20);
      };

      key.first = 1u
          | (uint64_t(ds.depthTest())    << 1)
          | (uint64_t(ds.depthWrite())   << 2)
          | (uint64_t(ds.depthCompareOp()) << 3)
          | (uint64_t(ds.stencilTest())  << 6)
          | (uint64_t(readOnlyAspects & 3u) << 7)
          | (packOp(ds.stencilOpFront()) << 9);
      key.second = packOp(ds.stencilOpBack());
    }

    std::lock_guard<std::mutex> lock(s_dssoMutex);

    auto entry = s_dssoCache.find(key);
    if (entry != s_dssoCache.end())
      return entry->second;

    WMTDepthStencilInfo info = { };
    info.depth_compare_function = WMTCompareFunction(7); // Always
    info.depth_write_enabled = false;

    if (hasDepthAttachment) {
      if (ds.depthTest())
        info.depth_compare_function = WMTCompareFunction(uint32_t(ds.depthCompareOp()));

      info.depth_write_enabled = ds.depthTest() && ds.depthWrite()
        && !(readOnlyAspects & VK_IMAGE_ASPECT_DEPTH_BIT);

      if (ds.stencilTest()) {
        bool writable = !(readOnlyAspects & VK_IMAGE_ASPECT_STENCIL_BIT);
        fillStencilInfo(info.front_stencil, ds.stencilOpFront(), writable);
        fillStencilInfo(info.back_stencil,  ds.stencilOpBack(),  writable);
      }
    }

    obj_handle_t dsso = MTLDevice_newDepthStencilState(mtlDevice(), &info);
    if (!dsso)
      Logger::err("d9mt: newDepthStencilState failed");

    s_dssoCache.insert({ key, dsso });
    return dsso;
  }


  // DSSO for the depth(+stencil) SAMPLE_ZERO resolve pass (resolveImage):
  // unconditional depth write; the stencil variant relies on shader stencil
  // export ([[stencil]] replaces the reference value) + op Replace.
  static obj_handle_t getDepthResolveDsso(bool withStencil) {
    static obj_handle_t s_resolveDsso[2] = { };

    std::lock_guard<std::mutex> lock(s_dssoMutex);

    obj_handle_t& cached = s_resolveDsso[withStencil ? 1 : 0];
    if (cached)
      return cached;

    WMTDepthStencilInfo info = { };
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = true;

    if (withStencil) {
      for (auto* s : { &info.front_stencil, &info.back_stencil }) {
        s->enabled = true;
        s->depth_stencil_pass_op    = WMTStencilOperationReplace;
        s->stencil_fail_op          = WMTStencilOperationKeep;
        s->depth_fail_op            = WMTStencilOperationKeep;
        s->stencil_compare_function = WMTCompareFunctionAlways;
        s->write_mask = 0xffu;
        s->read_mask  = 0xffu;
      }
    }

    cached = MTLDevice_newDepthStencilState(mtlDevice(), &info);
    if (!cached)
      Logger::err("d9mt: depth resolve: newDepthStencilState failed");
    return cached;
  }


  // DSSO for the scissored depth/stencil clear draw (clearImageView): the
  // cleared aspect writes unconditionally (depth: write-always at the
  // viewport-encoded value; stencil: op Replace at the stencil reference),
  // the other aspect is left untouched.
  static obj_handle_t getDepthStencilClearDsso(bool clearDepth, bool clearStencil) {
    static obj_handle_t s_clearDsso[4] = { };

    std::lock_guard<std::mutex> lock(s_dssoMutex);

    obj_handle_t& cached =
      s_clearDsso[(clearDepth ? 1u : 0u) | (clearStencil ? 2u : 0u)];
    if (cached)
      return cached;

    WMTDepthStencilInfo info = { };
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = clearDepth;

    if (clearStencil) {
      for (auto* s : { &info.front_stencil, &info.back_stencil }) {
        s->enabled = true;
        s->depth_stencil_pass_op    = WMTStencilOperationReplace;
        s->stencil_fail_op          = WMTStencilOperationKeep;
        s->depth_fail_op            = WMTStencilOperationKeep;
        s->stencil_compare_function = WMTCompareFunctionAlways;
        s->write_mask = 0xffu;
        s->read_mask  = 0xffu;
      }
    }

    cached = MTLDevice_newDepthStencilState(mtlDevice(), &info);
    if (!cached)
      Logger::err("d9mt: ds clear: newDepthStencilState failed");
    return cached;
  }


  // --------------------------------------------------------------------------
  // per-context draw state (single CS-thread consumer; the map itself is
  // mutex-guarded like the other side tables)
  // --------------------------------------------------------------------------

  struct ContextDrawState {
    std::unique_ptr<DxvkStagingBuffer> ring; // AB + push + fan-index uploads

    // current framebuffer info (recomputed in updateRenderTargets)
    VkExtent2D          fbExtent = { };
    bool                fbHasAttachments = false;
    bool                fbHasDepth = false;
    VkImageAspectFlags  fbReadOnlyAspects = 0;
    uint32_t            fbLayerCount = 1u;

    // occlusion queries currently inside begin/end (Queries stage): render
    // passes attach a visibility-result buffer while this is non-zero
    uint32_t            activeOcclusionCount = 0u;

    const PsoEntry*     pso = nullptr;

    // Single-entry memo for the render-PSO lookup. Consecutive draws very often
    // resolve to the same pipeline, so cache the last key + result and skip the
    // full-state FNV hash + s_psoMutex + unordered_map probe in getRenderPso on
    // a match. Purely a lookup-cost optimization: the resolved entry is byte-for-
    // byte what a fresh lookup would return, so this changes timing only, never
    // observable behavior (downstream dirty/AB logic is unaffected).
    PsoKey              lastPsoKey = { };
    const PsoEntry*     lastPsoEntry = nullptr;
  };

  namespace {
    std::mutex s_ctxDrawMutex;
    std::unordered_map<const void*, std::unique_ptr<ContextDrawState>> s_ctxDrawStates;
  }

  static ContextDrawState& ctxDrawStateImpl(const void* ctx) {
    std::lock_guard<std::mutex> lock(s_ctxDrawMutex);
    auto& slot = s_ctxDrawStates[ctx];
    if (!slot)
      slot = std::make_unique<ContextDrawState>();
    return *slot;
  }

  void eraseCtxDrawState(const void* ctx) {
    std::lock_guard<std::mutex> lock(s_ctxDrawMutex);
    s_ctxDrawStates.erase(ctx);
  }

}

namespace dxvk {

  // ==========================================================================
  // DxvkObjectTracker — keeps tracked objects alive until command-list
  // completion. Final implementation per dxvk_access.h semantics: a chain of
  // 1024-entry storage lists; clear() runs the virtual releases.
  // ==========================================================================

  DxvkObjectTracker::DxvkObjectTracker()
  : m_head(std::make_unique<List>()), m_next(m_head.get()) {

  }


  DxvkObjectTracker::~DxvkObjectTracker() {
    this->clear();
  }


  void DxvkObjectTracker::clear() {
    List* list = m_head.get();

    for (size_t i = 0; i < m_size; i++) {
      if (i && !(i & ListMask))
        list = list->next.get();

      std::launder(reinterpret_cast<DxvkTrackingRef*>(
        list->storage[i & ListMask].data))->~DxvkTrackingRef();
    }

    m_size = 0u;
    m_next = m_head.get();
  }


  void DxvkObjectTracker::advanceList() {
    if (!m_next->next)
      m_next->next = std::make_unique<List>();

    m_next = m_next->next.get();
  }


  // ==========================================================================
  // DxvkSignalTracker — signals queued on a command list, notified by the
  // completion watcher when the submission retires.
  // ==========================================================================

  DxvkSignalTracker::DxvkSignalTracker() {

  }


  DxvkSignalTracker::~DxvkSignalTracker() {

  }


  void DxvkSignalTracker::add(const Rc<sync::Signal>& signal, uint64_t value) {
    m_signals.push_back({ signal, value });
  }


  void DxvkSignalTracker::notify() {
    for (const auto& pair : m_signals)
      pair.first->signal(pair.second);

    m_signals.clear();
  }


  void DxvkSignalTracker::reset() {
    m_signals.clear();
  }


  // ==========================================================================
  // DxvkStatCounters
  // ==========================================================================

  DxvkStatCounters::DxvkStatCounters() {
    m_counters.fill(0u);
  }


  DxvkStatCounters::~DxvkStatCounters() {

  }


  DxvkStatCounters DxvkStatCounters::diff(const DxvkStatCounters& other) const {
    DxvkStatCounters result;

    for (size_t i = 0; i < m_counters.size(); i++)
      result.m_counters[i] = m_counters[i] - other.m_counters[i];

    return result;
  }


  void DxvkStatCounters::merge(const DxvkStatCounters& other) {
    for (size_t i = 0; i < m_counters.size(); i++)
      m_counters[i] += other.m_counters[i];
  }


  void DxvkStatCounters::reset() {
    m_counters.fill(0u);
  }


  // ==========================================================================
  // DxvkCommandSubmission — semaphore/command-buffer batch for one Vulkan
  // queue submission. Never populated on the Metal backend (submission goes
  // through winemetal commit + completion watcher), but the object lives
  // inside every DxvkCommandList.
  // ==========================================================================

  DxvkCommandSubmission::DxvkCommandSubmission() {

  }


  DxvkCommandSubmission::~DxvkCommandSubmission() {

  }


  // ==========================================================================
  // DxvkCommandList — Metal-backed command list shell. The wmtcmd encoding
  // surface (cmd*/bindResources) is the Context stage; everything tracking-
  // related here is final.
  // ==========================================================================

  DxvkCommandList::DxvkCommandList(DxvkDevice* device)
  : m_device(device),
    m_vkd(device->vkd()) {
    // no Vulkan command pools: the Metal command buffer is created
    // lazily by the context at flush time (Context stage)
  }


  DxvkCommandList::~DxvkCommandList() {
    this->reset();
  }


  void DxvkCommandList::init() {
    m_cmd = DxvkCommandSubmissionInfo();

    // Smuggle the list pointer through the exec VkCommandBuffer slot so the
    // vendored inline cmd* methods (cmdBindPipeline / cmdDispatch /
    // cmdPipelineBarrier — D3D9FormatHelper path) can reach the Metal side
    // state via the fake device dispatch (d9mt_device.cpp). VkCommandBuffer
    // is a dispatchable (pointer-sized) handle and nothing else reads it.
    m_cmd.cmdBuffers[uint32_t(DxvkCmdBuffer::ExecBuffer)] =
      reinterpret_cast<VkCommandBuffer>(this);
  }


  void DxvkCommandList::finalize() {
    // Metal: end any open encoder so the command buffer can be committed.
    // The actual commit happens in DxvkDevice::submitCommandList.
    d9mt::endEncoder(d9mt::cmdListState(this));
  }


  void DxvkCommandList::bindResources(
          DxvkCmdBuffer                 cmdBuffer,
    const DxvkPipelineLayout*           layout,
          uint32_t                      descriptorCount,
    const DxvkDescriptorWrite*          descriptorInfos,
          size_t                        pushDataSize,
    const void*                         pushData) {
    // Metal compute-encoder argument convention for built-in pipelines
    // (must match the Draw stage's SPIRV-Cross MSL resource mapping, see
    // METAL-BACKEND-NOTES.md): descriptor i of an image/texel-buffer type
    // binds to texture slot i, of a buffer type to buffer slot i; push
    // constants go to buffer slot 30 via setBytes.
    auto& state = d9mt::cmdListState(this);

    for (uint32_t i = 0; i < descriptorCount; i++) {
      const auto& write = descriptorInfos[i];

      switch (write.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
          wmtcmd_compute_settexture cmd = { };
          cmd.type = WMTComputeCommandSetTexture;
          cmd.texture = write.descriptor
            ? obj_handle_t(write.descriptor->legacy.image.imageView)
            : 0u;
          cmd.index = uint8_t(i);
          d9mt::encodeComputeCmd(state, &cmd);
        } break;

        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
          wmtcmd_compute_settexture cmd = { };
          cmd.type = WMTComputeCommandSetTexture;
          cmd.texture = write.descriptor
            ? obj_handle_t(write.descriptor->legacy.bufferView)
            : 0u;
          cmd.index = uint8_t(i);
          d9mt::encodeComputeCmd(state, &cmd);
        } break;

        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
          wmtcmd_compute_setbuffer cmd = { };
          cmd.type = WMTComputeCommandSetBuffer;
          cmd.buffer = obj_handle_t(write.buffer.buffer);
          cmd.offset = write.buffer.offset;
          cmd.index = uint8_t(i);
          d9mt::encodeComputeCmd(state, &cmd);
        } break;

        default:
          Logger::err(str::format("d9mt: DxvkCommandList::bindResources: "
            "unsupported descriptor type ", uint32_t(write.descriptorType)));
          break;
      }
    }

    if (pushDataSize && pushData) {
      wmtcmd_compute_setbytes cmd = { };
      cmd.type = WMTComputeCommandSetBytes;
      cmd.bytes.set(const_cast<void*>(pushData));
      cmd.length = pushDataSize;
      cmd.index = 30u;
      d9mt::encodeComputeCmd(state, &cmd);
    }
  }


  void DxvkCommandList::reset() {
    // drop the Metal command buffer / encoder backing this list (normally
    // already drained by the submission completion path; this also covers
    // lists that get reset without ever being submitted)
    d9mt::resetCmdListState(this);

    // release everything the GPU might have referenced, without
    // notifying signals (that is notifyObjects' job on completion)
    m_objectTracker.clear();
    m_signalTracker.reset();
    m_statCounters.reset();

    if (!m_pipelines.empty()) {
      // pipelines are only tracked on the lifetime-tracking path,
      // which mustTrackPipelineLifetime() disables on this backend
      Logger::err("d9mt: DxvkCommandList::reset: unexpected tracked pipelines");
      m_pipelines.clear();
    }

    m_waitSemaphores.clear();
    m_signalSemaphores.clear();

    m_cmdSubmissions.clear();
    m_cmdSparseBinds.clear();

    m_descriptorPools.clear();
    m_descriptorPool    = nullptr;
    m_descriptorManager = nullptr;
    m_descriptorSync    = sync::SyncPoint();

    m_descriptorHeap   = nullptr;
    m_descriptorRange  = nullptr;
    m_descriptorOffset = 0u;

    m_wsiSemaphores = PresenterSync();
    m_trackingId    = 0u;

    m_cmd        = DxvkCommandSubmissionInfo();
    m_execBuffer = VK_NULL_HANDLE;
  }


  // ==========================================================================
  // Barrier containers — constructed as members of DxvkContext. The Metal
  // context replaces Vulkan barriers with render-pass splits, so these stay
  // empty; construction state below is valid-and-empty.
  // ==========================================================================

  DxvkBarrierBatch::DxvkBarrierBatch(DxvkCmdBuffer cmdBuffer)
  : m_cmdBuffer(cmdBuffer) {

  }


  DxvkBarrierBatch::~DxvkBarrierBatch() {

  }


  DxvkBarrierTracker::DxvkBarrierTracker() {
    // node 0 is the implicit null node, nodes 1..2*HashTableSize are the
    // hash-table roots (computeRootIndex), as in the upstream layout
    m_nodes.resize(1u + 2u * HashTableSize);
  }


  DxvkBarrierTracker::~DxvkBarrierTracker() {

  }


  // ==========================================================================
  // DxvkFramebufferInfo — by-value member of the context's output-merger
  // state. The default state is "no attachments" (m_attachmentCount = 0);
  // the populating constructor and lookup helpers are the Context stage.
  // ==========================================================================

  DxvkFramebufferInfo::DxvkFramebufferInfo() {

  }


  DxvkFramebufferInfo::~DxvkFramebufferInfo() {

  }


  // ==========================================================================
  // DxvkGpuQueryManager — upstream bookkeeping (active virtual queries per
  // type, one shared GPU query per active span) over Metal visibility-result
  // slots instead of VkQueryPools. A GPU query == one visibility slot in the
  // current command list's buffer; "ending" it needs no Metal command (the
  // slot is final once the encoder ends or the mode changes), and queries
  // spanning encoder restarts simply accumulate additional GPU queries on
  // the virtual DxvkQuery (BACKEND-SURFACE §5.3 / §7 risk 4).
  //
  // beginQueries/endQueries are driven by the context's render-pass
  // lifecycle (startRenderPass / spillRenderPass / updateRenderTargets).
  // Encoders that die behind the manager's back (encoder-kind switches in
  // copy paths) are safe: restartQueries only encodes into a live render
  // encoder with an attached visibility buffer, and the next startRenderPass
  // restarts every active query with a fresh slot.
  // ==========================================================================

  DxvkGpuQueryManager::DxvkGpuQueryManager(DxvkGpuQueryPool& pool)
  : m_pool(&pool) {

  }


  DxvkGpuQueryManager::~DxvkGpuQueryManager() {

  }


  void DxvkGpuQueryManager::enableQuery(
    const Rc<DxvkCommandList>&  cmd,
    const Rc<DxvkQuery>&        query) {
    query->begin();

    uint32_t index = getQueryTypeIndex(query->type(), query->index());

    m_activeQueries[index].queries.push_back(query);

    if (m_activeTypes & getQueryTypeBit(query->type()))
      restartQueries(cmd, query->type(), query->index());
  }


  void DxvkGpuQueryManager::disableQuery(
    const Rc<DxvkCommandList>&  cmd,
    const Rc<DxvkQuery>&        query) {
    uint32_t index = getQueryTypeIndex(query->type(), query->index());

    for (auto& q : m_activeQueries[index].queries) {
      if (q == query) {
        q = std::move(m_activeQueries[index].queries.back());
        m_activeQueries[index].queries.pop_back();
        break;
      }
    }

    if (m_activeTypes & getQueryTypeBit(query->type()))
      restartQueries(cmd, query->type(), query->index());

    query->end();
  }


  void DxvkGpuQueryManager::writeTimestamp(
    const Rc<DxvkCommandList>&  cmd,
    const Rc<DxvkQuery>&        query) {
    Rc<DxvkGpuQuery> q = m_pool->allocQuery(query->type());

    if (q == nullptr) {
      Logger::err("d9mt: writeTimestamp: failed to allocate GPU query");
      return;
    }

    d9mt::resetGpuQueryResult(q.ptr());

    query->begin();
    query->addGpuQuery(q);
    query->end();

    // make sure the submission carries a real command buffer so the
    // resolved GPU end time is meaningful (empty submissions fall back to
    // the last resolved timestamp)
    auto& state = d9mt::cmdListState(cmd.ptr());
    d9mt::ensureCmdBuf(state);

    state.tsQueries.push_back(std::move(q));
  }


  void DxvkGpuQueryManager::beginQueries(
    const Rc<DxvkCommandList>&  cmd,
          VkQueryType           type) {
    m_activeTypes |= getQueryTypeBit(type);

    restartQueries(cmd, type, 0);
  }


  void DxvkGpuQueryManager::endQueries(
    const Rc<DxvkCommandList>&  cmd,
          VkQueryType           type) {
    m_activeTypes &= ~getQueryTypeBit(type);

    restartQueries(cmd, type, 0);
  }


  void DxvkGpuQueryManager::restartQueries(
    const Rc<DxvkCommandList>&  cmd,
          VkQueryType           type,
          uint32_t              index) {
    if (type != VK_QUERY_TYPE_OCCLUSION) {
      Logger::err(str::format("d9mt: GpuQueryManager: unsupported query type ",
        uint32_t(type)));
      return;
    }

    auto& array = m_activeQueries[getQueryTypeIndex(type, index)];
    auto& state = d9mt::cmdListState(cmd.ptr());

    // Ending the current GPU query needs no Metal command: its slot value is
    // final once the encoder stops counting into it (mode change below or
    // encoder end).
    bool hadQuery = array.gpuQuery != nullptr;
    array.gpuQuery = nullptr;

    bool encoderReady = state.kind == d9mt::EncoderKind::Render
                     && state.encoder
                     && state.visAttached;

    if ((m_activeTypes & getQueryTypeBit(type)) && !array.queries.empty()) {
      // Outside a visibility-enabled render encoder nothing can be counted;
      // the next startRenderPass restarts active queries with a fresh slot.
      if (!encoderReady)
        return;

      Rc<DxvkGpuQuery> q = m_pool->allocQuery(type);
      auto& result = d9mt::gpuQueryResult(q.ptr());

      if (state.visSlotsUsed >= d9mt::VisSlotCount) {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
          Logger::err("d9mt: visibility-result slots exhausted; occlusion query failed");

        result.value = 0u;
        result.state.store(d9mt::GpuQueryResult::Failed, std::memory_order_release);

        // stop counting into the previous (already ended) slot
        wmtcmd_render_setvisibilitymode mode = { };
        mode.type = WMTRenderCommandSetVisibilityMode;
        mode.mode = WMTVisibilityResultModeDisabled;
        d9mt::encodeRenderCmd(state, &mode);
      } else {
        uint32_t slot = state.visSlotsUsed++;

        result.value = 0u;
        result.state.store(d9mt::GpuQueryResult::Pending, std::memory_order_relaxed);
        state.visSlots.push_back({ q, slot });

        // Metal visibility counting is always per-sample exact, which
        // satisfies VK_QUERY_CONTROL_PRECISE_BIT for every active query
        wmtcmd_render_setvisibilitymode mode = { };
        mode.type = WMTRenderCommandSetVisibilityMode;
        mode.offset = uint64_t(slot) * sizeof(uint64_t);
        mode.mode = WMTVisibilityResultModeCounting;
        d9mt::encodeRenderCmd(state, &mode);
      }

      for (const auto& vq : array.queries)
        vq->addGpuQuery(q);

      array.gpuQuery = std::move(q);
    } else if (hadQuery && encoderReady) {
      // active set went empty mid-encoder: stop counting so later draws do
      // not corrupt the ended query's slot
      wmtcmd_render_setvisibilitymode mode = { };
      mode.type = WMTRenderCommandSetVisibilityMode;
      mode.mode = WMTVisibilityResultModeDisabled;
      d9mt::encodeRenderCmd(state, &mode);
    }
  }


  uint32_t DxvkGpuQueryManager::getQueryTypeBit(
          VkQueryType           type) {
    return 1u << getQueryTypeIndex(type, 0u);
  }


  uint32_t DxvkGpuQueryManager::getQueryTypeIndex(
          VkQueryType           type,
          uint32_t              index) {
    switch (type) {
      case VK_QUERY_TYPE_OCCLUSION:                     return 0u;
      case VK_QUERY_TYPE_PIPELINE_STATISTICS:           return 1u;
      case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: return 2u + index;
      default:                                          return 0u;
    }
  }


  // ==========================================================================
  // DxvkDescriptorCopyWorker — THREADLESS on this backend (descriptor writes
  // are 8-byte argument-buffer stores; off-loading them buys nothing). The
  // fences exist so getSyncHandle() stays valid; with no worker, append ==
  // consume at all times.
  // ==========================================================================

  DxvkDescriptorCopyWorker::DxvkDescriptorCopyWorker(const Rc<DxvkDevice>& device)
  : m_device(device),
    m_vkd(device->vkd()),
    m_appendFence(new sync::Fence()),
    m_consumeFence(new sync::Fence()) {
    // no worker thread on purpose (METAL-BACKEND-NOTES.md)
  }


  DxvkDescriptorCopyWorker::~DxvkDescriptorCopyWorker() {

  }


  // ==========================================================================
  // DxvkImplicitResolveTracker — shell; resolve logic is the Context stage.
  // ==========================================================================

  DxvkImplicitResolveTracker::DxvkImplicitResolveTracker(Rc<DxvkDevice> device)
  : m_device(std::move(device)) {

  }


  DxvkImplicitResolveTracker::~DxvkImplicitResolveTracker() {

  }


  // ==========================================================================
  // DxvkDescriptorUpdateList teardown (the building constructor is only used
  // on the descriptor-buffer path, which this backend disables; it stays a
  // loud stub in stubs.cpp).
  // ==========================================================================

  DxvkDescriptorUpdateList::~DxvkDescriptorUpdateList() {

  }


  // ==========================================================================
  // DxvkEvent (D3DQUERYTYPE_EVENT) — watcher-based status, no VkEvent.
  // signalGpuEvent marks the event VK_EVENT_RESET (pending) and registers a
  // completion-work entry on the current command list that flips it to
  // VK_EVENT_SET when the submission retires. VK_NOT_READY = never recorded.
  // ==========================================================================

  DxvkGpuEventStatus DxvkEvent::test() {
    std::lock_guard<sync::Spinlock> lock(m_mutex);

    switch (m_status) {
      case VK_EVENT_SET:   return DxvkGpuEventStatus::Signaled;
      case VK_EVENT_RESET: return DxvkGpuEventStatus::Pending;
      default:             return DxvkGpuEventStatus::Invalid;
    }
  }


  // ==========================================================================
  // DxvkQuery::getData — upstream v2.7.1 structure, with the per-GPU-query
  // readback going through the d9mt result side state (resolved on the
  // watcher thread at submission retirement) instead of
  // vkGetQueryPoolResults. Polled non-blocking from the app thread.
  // ==========================================================================

  DxvkGpuQueryStatus DxvkQuery::getData(DxvkQueryData& queryData) {
    queryData = DxvkQueryData();

    // Callers must ensure that no begin call is pending when
    // calling this. Given that, once the query is ended, we
    // know that no other thread will access query state.
    std::lock_guard<sync::Spinlock> lock(m_mutex);

    if (!m_ended)
      return DxvkGpuQueryStatus::Invalid;

    // Accumulate query data from all available queries
    DxvkGpuQueryStatus status = accumulateQueryDataLocked();

    // Treat non-precise occlusion queries as available
    // if we already know the result will be non-zero
    if ((status == DxvkGpuQueryStatus::Pending)
     && (m_type == VK_QUERY_TYPE_OCCLUSION)
     && !(m_flags & VK_QUERY_CONTROL_PRECISE_BIT)
     && (m_queryData.occlusion.samplesPassed))
      status = DxvkGpuQueryStatus::Available;

    // Write back accumulated query data if the result is useful
    if (status == DxvkGpuQueryStatus::Available)
      queryData = m_queryData;

    return status;
  }


  DxvkGpuQueryStatus DxvkQuery::accumulateQueryDataForGpuQueryLocked(
    const Rc<DxvkGpuQuery>&           query) {
    const auto& result = d9mt::gpuQueryResult(query.ptr());

    uint32_t state = result.state.load(std::memory_order_acquire);

    if (state == d9mt::GpuQueryResult::Pending)
      return DxvkGpuQueryStatus::Pending;
    if (state == d9mt::GpuQueryResult::Failed)
      return DxvkGpuQueryStatus::Failed;

    switch (m_type) {
      case VK_QUERY_TYPE_OCCLUSION:
        m_queryData.occlusion.samplesPassed += result.value;
        break;

      case VK_QUERY_TYPE_TIMESTAMP:
        m_queryData.timestamp.time = result.value;
        break;

      default:
        Logger::err(str::format("d9mt: DxvkQuery: unhandled query type ",
          uint32_t(m_type)));
        return DxvkGpuQueryStatus::Invalid;
    }

    return DxvkGpuQueryStatus::Available;
  }


  DxvkGpuQueryStatus DxvkQuery::accumulateQueryDataLocked() {
    DxvkGpuQueryStatus status = DxvkGpuQueryStatus::Available;

    // Process available queries and release them
    // if possible to keep the in-flight count low.
    size_t queriesAvailable = 0;

    while (queriesAvailable < m_queries.size()) {
      status = accumulateQueryDataForGpuQueryLocked(m_queries[queriesAvailable]);

      if (status != DxvkGpuQueryStatus::Available)
        break;

      queriesAvailable += 1;
    }

    if (queriesAvailable) {
      for (size_t i = queriesAvailable; i < m_queries.size(); i++)
        m_queries[i - queriesAvailable] = m_queries[i];

      m_queries.resize(m_queries.size() - queriesAvailable);
    }

    return status;
  }


  // ==========================================================================
  // DxvkDevice::submitCommandList — threadless synchronous submission.
  //
  // Lives here (not d9mt_device.cpp) because it is welded to the command-
  // list side state above. Commits the MTLCommandBuffer on the calling (CS)
  // thread and registers the completion callback with the watcher; the
  // status atomic flips to VK_SUCCESS as soon as the submission is queued
  // (waitForSubmission semantics). Signals fire even on empty submissions
  // through the watcher's cmdbuf==0 path (BACKEND-SURFACE §7 risk 6).
  // ==========================================================================

  void DxvkDevice::submitCommandList(
    const Rc<DxvkCommandList>&      commandList,
    const Rc<DxvkLatencyTracker>&   tracker,
          uint64_t                  frameId,
          DxvkSubmitStatus*         status) {
    {
      std::lock_guard<sync::Spinlock> lock(m_statLock);
      m_statCounters.merge(commandList->statCounters());
      m_statCounters.addCtr(DxvkStatCounter::QueueSubmitCount, 1u);
    }

    obj_handle_t cmdbuf = d9mt::cmdListCommit(commandList.ptr());

    DxvkDevice* device = this;
    Rc<DxvkCommandList> cmd = commandList;

    d9mt::watchCommandBuffer(cmdbuf, [device, cmd] {
      // per-submission completion work first (EVENT flips), then signals +
      // tracked-resource release, then recycle (resets the side state)
      d9mt::cmdListRunCompletionWork(cmd.ptr());
      cmd->notifyObjects();
      device->recycleCommandList(cmd);
    });

    if (status)
      status->result.store(VK_SUCCESS);
  }


  // ==========================================================================
  // DxvkContext
  // ==========================================================================

  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device(device),
    m_common(&device->m_objects),
    m_sdmaAcquires(DxvkCmdBuffer::SdmaBarriers),
    m_sdmaBarriers(DxvkCmdBuffer::SdmaBuffer),
    m_initAcquires(DxvkCmdBuffer::InitBarriers),
    m_initBarriers(DxvkCmdBuffer::InitBuffer),
    m_execBarriers(DxvkCmdBuffer::ExecBuffer),
    m_queryManager(m_common->queryPool()),
    m_descriptorWorker(device),
    m_implicitResolves(device) {
    d9mt::logf("DxvkContext: created");
  }


  DxvkContext::~DxvkContext() {
    d9mt::eraseCtxDrawState(this);
    d9mt::logf("DxvkContext: destroyed");
  }


  // --------------------------------------------------------------------------
  // recording / submission
  // --------------------------------------------------------------------------

  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) {
    m_cmd = cmdList;
    m_cmd->init();

    this->beginCurrentCommands();
  }


  Rc<DxvkCommandList> DxvkContext::endRecording(
    const VkDebugUtilsLabelEXT*       reason) {
    this->endCurrentCommands();

    m_cmd->finalize();
    return std::exchange(m_cmd, nullptr);
  }


  void DxvkContext::flushCommandList(
    const VkDebugUtilsLabelEXT*       reason,
          DxvkSubmitStatus*           status) {
    D9MT_ZONE(d9mt::ZoneFlush);
    m_device->submitCommandList(this->endRecording(reason),
      m_latencyTracker, m_latencyFrameId, status);

    if (m_endLatencyTracking) {
      m_latencyTracker = nullptr;
      m_latencyFrameId = 0u;
      m_endLatencyTracking = false;
    }

    // If we have a zero buffer, see if we can get rid of it
    freeZeroBuffer();

    this->beginRecording(
      m_device->createCommandList());
  }


  Rc<DxvkCommandList> DxvkContext::beginExternalRendering() {
    // Flush and invalidate everything; external users (FormatHelper,
    // swapchain blitter) encode directly on the command list afterwards.
    endCurrentCommands();
    beginCurrentCommands();

    return m_cmd;
  }


  void DxvkContext::endFrame() {
    m_renderPassIndex = 0u;
  }


  void DxvkContext::beginLatencyTracking(
    const Rc<DxvkLatencyTracker>&     tracker,
          uint64_t                    frameId) {
    // createLatencyTracker returns nullptr on this backend, so this is a
    // no-op in practice; keep upstream semantics for robustness
    if (tracker && (!m_latencyTracker || m_latencyTracker == tracker)) {
      tracker->notifyCsRenderBegin(frameId);

      m_latencyTracker = tracker;
      m_latencyFrameId = frameId;

      m_endLatencyTracking = false;
    }
  }


  void DxvkContext::endLatencyTracking(
    const Rc<DxvkLatencyTracker>&     tracker) {
    if (tracker && tracker == m_latencyTracker)
      m_endLatencyTracking = true;
  }


  void DxvkContext::signal(const Rc<sync::Signal>& signal, uint64_t value) {
    m_cmd->queueSignal(signal, value);
  }


  void DxvkContext::beginCurrentCommands() {
    // The current state of the internal command buffer is undefined, so we
    // have to bind and set up everything before any draw command is recorded.
    m_flags.clr(
      DxvkContextFlag::GpRenderPassBound,
      DxvkContextFlag::GpXfbActive,
      DxvkContextFlag::GpIndependentSets);

    m_flags.set(
      DxvkContextFlag::GpDirtyRenderTargets,
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilTest,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyMultisampleState,
      DxvkContextFlag::GpDirtyRasterizerState,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds,
      DxvkContextFlag::GpDirtyDepthClip,
      DxvkContextFlag::GpDirtyDepthTest,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::DirtyDrawBuffer);

    m_descriptorState.dirtyStages(
      VK_SHADER_STAGE_ALL_GRAPHICS |
      VK_SHADER_STAGE_COMPUTE_BIT);

    m_state.gp.pipeline = nullptr;
    m_state.cp.pipeline = nullptr;

    m_cmd->setTrackingId(++m_trackingId);

    // no descriptor pools / descriptor heaps on the Metal backend
  }


  void DxvkContext::endCurrentCommands() {
    this->spillRenderPass(true);

    // end any remaining encoder so external users / submission
    // get a clean command buffer
    d9mt::endEncoder(d9mt::cmdListState(m_cmd.ptr()));
  }


  void DxvkContext::spillRenderPass(bool suspend) {
    // Execute pending deferred clears and close the render encoder.
    if (!m_deferredClears.empty())
      this->flushClears(false);

    // occlusion queries: end the current GPU-query span (bookkeeping only;
    // the visibility slot is final once the encoder ends below)
    m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);

    auto& state = d9mt::cmdListState(m_cmd.ptr());
    if (state.kind == d9mt::EncoderKind::Render)
      d9mt::endEncoder(state);

    m_flags.clr(DxvkContextFlag::GpRenderPassBound);
  }


  // --------------------------------------------------------------------------
  // layout / hazard / barrier surface — Metal needs no barriers (single
  // queue, tracked resources); pass splits handle attachment feedback
  // --------------------------------------------------------------------------

  void DxvkContext::emitGraphicsBarrier(
          VkPipelineStageFlags      srcStages,
          VkAccessFlags             srcAccess,
          VkPipelineStageFlags      dstStages,
          VkAccessFlags             dstAccess) {
    this->spillRenderPass(true);
  }


  void DxvkContext::changeImageLayout(
    const Rc<DxvkImage>&        image,
          VkImageLayout         layout) {
    if (image->info().layout != layout) {
      this->spillRenderPass(true);
      this->prepareImage(image, image->getAvailableSubresources());

      image->setLayout(layout);

      for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
        const DxvkAttachment& rt = m_state.om.renderTargets.color[i];
        if (rt.view != nullptr && rt.view->image() == image)
          m_rtLayouts.color[i] = layout;
      }

      const DxvkAttachment& ds = m_state.om.renderTargets.depth;
      if (ds.view != nullptr && ds.view->image() == image)
        m_rtLayouts.depth = layout;

      m_cmd->track(image, DxvkAccess::Write);
    }
  }


  void DxvkContext::transformImage(
    const Rc<DxvkImage>&            dstImage,
    const VkImageSubresourceRange&  dstSubresources,
          VkImageLayout             srcLayout,
          VkImageLayout             dstLayout) {
    // layout metadata only; just make sure pending clears are ordered
    this->spillRenderPass(false);
  }


  bool DxvkContext::ensureImageCompatibility(
    const Rc<DxvkImage>&            image,
    const DxvkImageUsageInfo&       usageInfo) {
    // Images are created with permissive Metal usage (notes, resources
    // decision 3), so the backing storage never needs to be replaced;
    // merge the requested metadata and report success.
    bool compatible = (image->info().usage & usageInfo.usage) == usageInfo.usage
                   && (image->info().flags & usageInfo.flags) == usageInfo.flags
                   && (image->info().stages & usageInfo.stages) == usageInfo.stages
                   && (image->info().access & usageInfo.access) == usageInfo.access
                   && (!usageInfo.layout || image->info().layout == usageInfo.layout)
                   && (usageInfo.colorSpace == VK_COLOR_SPACE_MAX_ENUM_KHR
                    || usageInfo.colorSpace == image->info().colorSpace);

    for (uint32_t i = 0; i < usageInfo.viewFormatCount && compatible; i++)
      compatible &= image->isViewCompatible(usageInfo.viewFormats[i]);

    if (compatible)
      return true;

    this->spillRenderPass(true);
    this->prepareImage(image, image->getAvailableSubresources());

    image->assignStorageWithUsage(image->storage(), usageInfo);
    return true;
  }


  // --------------------------------------------------------------------------
  // clears
  // --------------------------------------------------------------------------

  void DxvkContext::clearRenderTarget(
    const Rc<DxvkImageView>&    imageView,
          VkImageAspectFlags    clearAspects,
          VkClearValue          clearValue,
          VkImageAspectFlags    discardAspects) {
    // Make sure the color components are ordered correctly (RTVs are
    // created swizzle-less, so apply the inverse view swizzle here)
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      clearValue.color = util::swizzleClearColor(clearValue.color,
        util::invertComponentMapping(imageView->info().unpackSwizzle()));
    }

    // Unconditionally defer; deferred clears are executed as standalone
    // clear passes at the next flush point (and merged into real render
    // passes once the Draw stage lands).
    if (discardAspects)
      this->deferDiscard(imageView, discardAspects);

    if (clearAspects)
      this->deferClear(imageView, clearAspects, clearValue);
  }


  void DxvkContext::deferClear(
    const Rc<DxvkImageView>&        imageView,
          VkImageAspectFlags        clearAspects,
          VkClearValue              clearValue) {
    for (auto& entry : m_deferredClears) {
      if (entry.imageView->matchesView(imageView)) {
        entry.imageView = imageView;
        entry.discardAspects &= ~clearAspects;
        entry.clearAspects |= clearAspects;

        if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT)
          entry.clearValue.color = clearValue.color;
        if (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
          entry.clearValue.depthStencil.depth = clearValue.depthStencil.depth;
        if (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
          entry.clearValue.depthStencil.stencil = clearValue.depthStencil.stencil;

        return;
      } else if (entry.imageView->checkSubresourceOverlap(imageView)) {
        this->spillRenderPass(false);
        break;
      }
    }

    m_deferredClears.push_back({ imageView, 0, clearAspects, clearValue });
  }


  void DxvkContext::deferDiscard(
    const Rc<DxvkImageView>&        imageView,
          VkImageAspectFlags        discardAspects) {
    for (auto& entry : m_deferredClears) {
      if (entry.imageView->matchesView(imageView)) {
        entry.imageView = imageView;
        entry.discardAspects |= discardAspects;
        entry.clearAspects &= ~discardAspects;
        return;
      } else if (entry.imageView->checkSubresourceOverlap(imageView)) {
        this->spillRenderPass(false);
        break;
      }
    }

    m_deferredClears.push_back({ imageView, discardAspects });
  }


  DxvkDeferredClear* DxvkContext::findDeferredClear(
    const Rc<DxvkImage>&          image,
    const VkImageSubresourceRange& subresources) {
    for (auto& entry : m_deferredClears) {
      if ((entry.imageView->image() == image.ptr()) && ((subresources.aspectMask & entry.clearAspects) == subresources.aspectMask)
       && (vk::checkSubresourceRangeSuperset(entry.imageView->imageSubresources(), subresources)))
        return &entry;
    }

    return nullptr;
  }


  DxvkDeferredClear* DxvkContext::findOverlappingDeferredClear(
    const Rc<DxvkImage>&          image,
    const VkImageSubresourceRange& subresources) {
    for (auto& entry : m_deferredClears) {
      if ((entry.imageView->image() == image.ptr())
       && ((entry.clearAspects | entry.discardAspects) | subresources.aspectMask)
       && (vk::checkSubresourceRangeOverlap(entry.imageView->imageSubresources(), subresources)))
        return &entry;
    }

    return nullptr;
  }


  void DxvkContext::flushClears(
          bool                      useRenderPass) {
    auto clears = std::move(m_deferredClears);
    m_deferredClears.clear();

    for (const auto& clear : clears) {
      this->performClear(clear.imageView, -1,
        clear.discardAspects, clear.clearAspects, clear.clearValue);
    }
  }


  void DxvkContext::prepareImage(
    const Rc<DxvkImage>&          image,
    const VkImageSubresourceRange& subresources,
          bool                    flushClears) {
    // Images that can't be used as attachments never have deferred clears
    if (!(image->info().usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
      return;

    if (flushClears && findOverlappingDeferredClear(image, image->getAvailableSubresources()))
      this->flushClears(false);
  }


  void DxvkContext::performClear(
    const Rc<DxvkImageView>&        imageView,
          int32_t                   attachmentIndex,
          VkImageAspectFlags        discardAspects,
          VkImageAspectFlags        clearAspects,
          VkClearValue              clearValue) {
    // Metal: standalone render pass whose load actions perform the clear.
    auto& state = d9mt::cmdListState(m_cmd.ptr());

    obj_handle_t viewHandle = obj_handle_t(imageView->handle());

    if (!viewHandle) {
      Logger::err("d9mt: performClear: image view has no Metal texture");
      return;
    }

    auto formatInfo = imageView->formatInfo();

    uint32_t layerCount = imageView->info().layerCount;
    uint32_t mipCount   = imageView->info().mipCount;

    // Standalone clears cover every mip the view includes (almost always 1)
    for (uint32_t level = 0; level < mipCount; level++) {
      VkExtent3D extent = imageView->mipLevelExtent(level);

      WMTRenderPassInfo pass = { };
      pass.render_target_width  = extent.width;
      pass.render_target_height = extent.height;

      if (layerCount > 1u)
        pass.render_target_array_length = uint8_t(layerCount);

      if (formatInfo->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
        auto& att = pass.colors[0];
        att.texture = viewHandle;
        att.level = uint16_t(level);
        att.store_action = WMTStoreActionStore;

        if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
          att.load_action = WMTLoadActionClear;

          if (formatInfo->flags.test(DxvkFormatFlag::SampledUInt)) {
            att.clear_color.r = double(clearValue.color.uint32[0]);
            att.clear_color.g = double(clearValue.color.uint32[1]);
            att.clear_color.b = double(clearValue.color.uint32[2]);
            att.clear_color.a = double(clearValue.color.uint32[3]);
          } else if (formatInfo->flags.test(DxvkFormatFlag::SampledSInt)) {
            att.clear_color.r = double(clearValue.color.int32[0]);
            att.clear_color.g = double(clearValue.color.int32[1]);
            att.clear_color.b = double(clearValue.color.int32[2]);
            att.clear_color.a = double(clearValue.color.int32[3]);
          } else {
            att.clear_color.r = double(clearValue.color.float32[0]);
            att.clear_color.g = double(clearValue.color.float32[1]);
            att.clear_color.b = double(clearValue.color.float32[2]);
            att.clear_color.a = double(clearValue.color.float32[3]);
          }
        } else {
          att.load_action = WMTLoadActionDontCare;
        }
      } else {
        // All depth formats map to Depth32Float_Stencil8 (unified DS
        // decision) — Metal requires both attachments to be set then.
        pass.depth.texture = viewHandle;
        pass.depth.level = uint16_t(level);
        pass.depth.store_action = WMTStoreActionStore;
        pass.depth.clear_depth = clearValue.depthStencil.depth;
        pass.depth.load_action = (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
          ? WMTLoadActionClear
          : ((discardAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
            ? WMTLoadActionDontCare : WMTLoadActionLoad);

        pass.stencil.texture = viewHandle;
        pass.stencil.level = uint16_t(level);
        pass.stencil.store_action = WMTStoreActionStore;
        pass.stencil.clear_stencil = uint8_t(clearValue.depthStencil.stencil);
        pass.stencil.load_action = (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
          ? WMTLoadActionClear
          : ((discardAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
            ? WMTLoadActionDontCare : WMTLoadActionLoad);
      }

      d9mt::encodeEmptyRenderPass(state, pass);
    }

    m_cmd->track(imageView->image(), DxvkAccess::Write);
  }


  // Scissored depth/stencil rect clear (clearImageView partial path): a
  // standalone fullscreen-triangle pass over the view's depth+stencil
  // attachments (both planes always bound, unified-DS rule) where ONLY the
  // requested aspects write. Both attachments load+store; the clear depth
  // value is encoded as viewport znear (the VS emits z = 0, so the written
  // depth is exactly znear), the stencil value as the DSSO stencil reference
  // with op Replace; the scissor rect restricts the write to the given rect.
  // Returns false on failure (caller stays fail-loud).
  static bool clearDepthStencilRect(
          DxvkCommandList*          cmd,
    const Rc<DxvkImageView>&        imageView,
          VkOffset3D                offset,
          VkExtent3D                extent,
          VkImageAspectFlags        aspect,
          VkClearValue              value) {
    obj_handle_t viewHandle = obj_handle_t(imageView->handle());

    if (!viewHandle) {
      Logger::err("d9mt: clearImageView: depth view has no Metal texture");
      return false;
    }

    if (imageView->info().mipCount != 1u) {
      // d3d9 DSVs are single-mip; the rect would not scale across levels
      Logger::err("d9mt: clearImageView: partial DS clear of multi-mip view not implemented");
      return false;
    }

    uint32_t sampleCount = uint32_t(imageView->image()->info().sampleCount);

    obj_handle_t pso = d9mt::getDepthStencilClearPso(sampleCount);
    obj_handle_t dsso = d9mt::getDepthStencilClearDsso(
      aspect & VK_IMAGE_ASPECT_DEPTH_BIT,
      aspect & VK_IMAGE_ASPECT_STENCIL_BIT);

    if (!pso || !dsso)
      return false;

    VkExtent3D viewExtent = imageView->mipLevelExtent(0);

    // clamp the rect into the view (Metal validates the scissor)
    uint64_t x0 = uint64_t(std::max(offset.x, 0));
    uint64_t y0 = uint64_t(std::max(offset.y, 0));
    uint64_t x1 = std::min(uint64_t(offset.x) + extent.width,  uint64_t(viewExtent.width));
    uint64_t y1 = std::min(uint64_t(offset.y) + extent.height, uint64_t(viewExtent.height));

    if (x0 >= x1 || y0 >= y1)
      return true; // empty rect: nothing to clear

    double clearDepth = double(value.depthStencil.depth);
    clearDepth = std::min(std::max(clearDepth, 0.0), 1.0);

    for (uint32_t layer = 0; layer < imageView->info().layerCount; layer++) {
      WMTRenderPassInfo pass = { };
      pass.render_target_width  = viewExtent.width;
      pass.render_target_height = viewExtent.height;

      pass.depth.texture = viewHandle;
      pass.depth.slice = uint16_t(layer);
      pass.depth.load_action  = WMTLoadActionLoad;
      pass.depth.store_action = WMTStoreActionStore;

      pass.stencil.texture = viewHandle;
      pass.stencil.slice = uint16_t(layer);
      pass.stencil.load_action  = WMTLoadActionLoad;
      pass.stencil.store_action = WMTStoreActionStore;

      obj_handle_t enc = d9mt::cmdListBeginRenderPass(cmd, pass);
      if (!enc)
        return false;

      wmtcmd_render_setpso setPso = { };
      wmtcmd_render_setdsso setDsso = { };
      wmtcmd_render_setviewport setVp = { };
      wmtcmd_render_setscissorrect setSc = { };
      wmtcmd_render_draw drawCmd = { };

      setPso.type = WMTRenderCommandSetPSO;
      setPso.next.set(&setDsso);
      setPso.pso = pso;

      setDsso.type = WMTRenderCommandSetDSSO;
      setDsso.next.set(&setVp);
      setDsso.dsso = dsso;
      setDsso.stencil_ref = uint8_t(value.depthStencil.stencil);

      // VS z = 0 ⇒ written depth = znear (zfar never sampled)
      setVp.type = WMTRenderCommandSetViewport;
      setVp.next.set(&setSc);
      setVp.viewport = { 0.0, 0.0,
                         double(viewExtent.width), double(viewExtent.height),
                         clearDepth, 1.0 };

      setSc.type = WMTRenderCommandSetScissorRect;
      setSc.next.set(&drawCmd);
      setSc.scissor_rect = { x0, y0, x1 - x0, y1 - y0 };

      drawCmd.type = WMTRenderCommandDraw;
      drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
      drawCmd.vertex_start = 0;
      drawCmd.vertex_count = 3;
      drawCmd.instance_count = 1;
      drawCmd.base_instance = 0;

      MTLRenderCommandEncoder_encodeCommands(enc,
        reinterpret_cast<const wmtcmd_base*>(&setPso));

      d9mt::cmdListEndEncoder(cmd);
    }

    return true;
  }


  void DxvkContext::clearImageView(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    auto formatInfo = imageView->formatInfo();
    auto image = imageView->image();

    this->spillRenderPass(false);

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      value.color = util::swizzleClearColor(value.color,
        util::invertComponentMapping(imageView->info().unpackSwizzle()));
    }

    // BC-alias clears (R32G32_UINT views over BC blocks) cannot be rendered
    // or blitted on Metal (no BC <-> non-BC texture views) — Draw stage will
    // need a compute writeback path (BACKEND-SURFACE §7 risk 7).
    if (image->formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed)) {
      Logger::err("d9mt: clearImageView: clearing block-compressed images not implemented");
      return;
    }

    VkExtent3D viewExtent = imageView->mipLevelExtent(0);

    bool isFullSize = offset == VkOffset3D { 0, 0, 0 }
                   && extent == viewExtent;

    if (isFullSize) {
      // full-view clear: same as a standalone render-target clear
      this->performClear(imageView, -1, 0, aspect, value);
      return;
    }

    // Partial depth/stencil clear (any aspect subset, any sample count):
    // scissored fullscreen-triangle draw writing only the requested aspects
    // (the GTA IV black-world case: the game clears the depth aspect of its
    // D24S8 surface with a sub-viewport rect every frame).
    if (formatInfo->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      if (clearDepthStencilRect(m_cmd.ptr(), imageView, offset, extent, aspect, value))
        m_cmd->track(image, DxvkAccess::Write);
      return;
    }

    // Partial color clear: clear a temporary image of matching format via a
    // render pass, then blit-copy the rect into the destination. Avoids
    // needing per-format CPU pixel packing or a draw-based scissored clear.
    if (image->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      Logger::err("d9mt: clearImageView: partial clear of multisampled color image not implemented");
      return;
    }

    DxvkImageCreateInfo tmpInfo = { };
    tmpInfo.type        = VK_IMAGE_TYPE_2D;
    tmpInfo.format      = imageView->info().format;
    tmpInfo.flags       = 0u;
    tmpInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    tmpInfo.extent      = { extent.width, extent.height, 1u };
    tmpInfo.numLayers   = 1u;
    tmpInfo.mipLevels   = 1u;
    tmpInfo.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    tmpInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT;
    tmpInfo.access      = VK_ACCESS_TRANSFER_READ_BIT;
    tmpInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    tmpInfo.layout      = VK_IMAGE_LAYOUT_GENERAL;

    Rc<DxvkImage> tmpImage;

    try {
      tmpImage = m_device->createImage(tmpInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (const DxvkError& e) {
      Logger::err(str::format("d9mt: clearImageView: failed to create temp image: ", e.message()));
      return;
    }

    DxvkImageViewKey tmpViewKey = { };
    tmpViewKey.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    tmpViewKey.usage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    tmpViewKey.format     = tmpInfo.format;
    tmpViewKey.aspects    = lookupFormatInfo(tmpInfo.format)->aspectMask;
    tmpViewKey.mipIndex   = 0u;
    tmpViewKey.mipCount   = 1u;
    tmpViewKey.layerIndex = 0u;
    tmpViewKey.layerCount = 1u;
    tmpViewKey.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    Rc<DxvkImageView> tmpView = tmpImage->createView(tmpViewKey);

    if (tmpView == nullptr || !tmpView->handle()) {
      Logger::err("d9mt: clearImageView: failed to create temp view");
      return;
    }

    this->performClear(tmpView, -1, 0, aspect, value);

    // copy the cleared rect into the destination subresources
    auto& state = d9mt::cmdListState(m_cmd.ptr());

    auto subresources = imageView->imageSubresources();

    for (uint32_t layer = 0; layer < imageView->info().layerCount; layer++) {
      for (uint32_t z = 0; z < extent.depth; z++) {
        wmtcmd_blit_copy_from_texture_to_texture cmd = { };
        cmd.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd.src = obj_handle_t(tmpImage->handle());
        cmd.src_slice = 0u;
        cmd.src_level = 0u;
        cmd.src_origin = { 0u, 0u, 0u };
        cmd.src_size = { extent.width, extent.height, 1u };
        cmd.dst = obj_handle_t(image->handle());
        cmd.dst_slice = subresources.baseArrayLayer + layer;
        cmd.dst_level = subresources.baseMipLevel;
        cmd.dst_origin = { uint64_t(offset.x), uint64_t(offset.y), uint64_t(offset.z + z) };
        d9mt::encodeBlitCmd(state, &cmd);
      }
    }

    m_cmd->track(tmpImage.ptr(), DxvkAccess::Write);
    m_cmd->track(image, DxvkAccess::Write);
  }


  // --------------------------------------------------------------------------
  // copies / init
  // --------------------------------------------------------------------------

  void DxvkContext::copyBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    this->spillRenderPass(true);

    auto srcSlice = srcBuffer->getSliceInfo(srcOffset, numBytes);
    auto dstSlice = dstBuffer->getSliceInfo(dstOffset, numBytes);

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    wmtcmd_blit_copy_from_buffer_to_buffer cmd = { };
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = obj_handle_t(srcSlice.buffer);
    cmd.src_offset = srcSlice.offset;
    cmd.dst = obj_handle_t(dstSlice.buffer);
    cmd.dst_offset = dstSlice.offset;
    cmd.copy_length = numBytes;
    d9mt::encodeBlitCmd(state, &cmd);

    m_cmd->track(dstBuffer, DxvkAccess::Write);
    m_cmd->track(srcBuffer, DxvkAccess::Read);
  }


  // Resolves the buffer-side texel size and required blit option for a
  // buffer<->image copy. Returns false (fail loud) for unsupported cases:
  // packed interleaved depth-stencil data and texel sizes that do not match
  // the unified Depth32Float_Stencil8 plane layout.
  static bool getBufferImageCopyDesc(
    const Rc<DxvkImage>&        image,
          VkImageAspectFlags    aspect,
          VkFormat              bufferFormat,
          uint32_t*             elementSize,
          VkExtent3D*           blockSize,
          WMTBlitOption*        option) {
    auto imageFormatInfo = image->formatInfo();

    if (!bufferFormat)
      bufferFormat = image->info().format;

    auto bufferFormatInfo = lookupFormatInfo(bufferFormat);

    if (!bufferFormatInfo) {
      Logger::err(str::format("d9mt: buffer-image copy: unknown buffer format ",
        uint32_t(bufferFormat)));
      return false;
    }

    *elementSize = bufferFormatInfo->elementSize;
    *blockSize = bufferFormatInfo->blockSize;
    *option = WMTBlitOptionNone;

    if (!(imageFormatInfo->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
      // color: buffer data must have the image's texel size (same-size
      // format aliases are fine, the copy is a raw bit copy)
      if (bufferFormatInfo->elementSize != imageFormatInfo->elementSize
       || (bufferFormatInfo->blockSize != imageFormatInfo->blockSize)) {
        Logger::err(str::format("d9mt: buffer-image copy: incompatible formats: image ",
          uint32_t(image->info().format), ", buffer ", uint32_t(bufferFormat)));
        return false;
      }
      return true;
    }

    // depth-stencil image (Metal: always Depth32Float_Stencil8)
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
      // depth plane is 32-bit float; packed/interleaved buffer data and
      // non-32-bit depth (D16) would need conversion (§7 risk 3)
      if (bufferFormat != VK_FORMAT_D32_SFLOAT) {
        Logger::err(str::format("d9mt: buffer-image copy: depth aspect with buffer format ",
          uint32_t(bufferFormat), " needs conversion (not implemented)"));
        return false;
      }
      *elementSize = 4u;
      *option = WMTBlitOptionDepthFromDepthStencil;
      return true;
    }

    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
      *elementSize = 1u;
      *option = WMTBlitOptionStencilFromDepthStencil;
      return true;
    }

    Logger::err(str::format("d9mt: buffer-image copy: packed depth-stencil data "
      "(aspect mask 0x", std::hex, aspect, ") not implemented"));
    return false;
  }


  void DxvkContext::copyBufferToImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkExtent3D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment,
          VkFormat              srcFormat) {
    this->spillRenderPass(true);
    this->prepareImage(dstImage, vk::makeSubresourceRange(dstSubresource));

    uint32_t elementSize = 0u;
    VkExtent3D blockSize = { };
    WMTBlitOption option = WMTBlitOptionNone;

    if (!getBufferImageCopyDesc(dstImage, dstSubresource.aspectMask,
        srcFormat, &elementSize, &blockSize, &option))
      return;

    VkExtent3D blockCount = util::computeBlockCount(dstExtent, blockSize);

    VkDeviceSize bytesPerRow = VkDeviceSize(blockCount.width) * elementSize;
    if (rowAlignment > elementSize)
      bytesPerRow = align(bytesPerRow, rowAlignment);

    VkDeviceSize bytesPerSlice = VkDeviceSize(blockCount.height) * bytesPerRow;
    if (sliceAlignment > elementSize)
      bytesPerSlice = align(bytesPerSlice, sliceAlignment);

    auto srcSlice = srcBuffer->getSliceInfo(srcOffset,
      bytesPerSlice * blockCount.depth * dstSubresource.layerCount);

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    // GTA IV black-texture root cause: the prebuilt winemetal bridge does NOT
    // implement WMTBlitCommandCopyFromBufferToTextureWithBlitOption (its
    // dylib has no copyFromBuffer:...options: selector) — encoding it made
    // EVERY texture upload silently vanish. Use the plain, proven command for
    // option-less copies (all color formats); packed depth-stencil locks
    // (option != None) cannot be expressed and are skipped loudly.
    if (option != WMTBlitOptionNone) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: copyBufferToImage: depth-stencil blit options not "
          "supported by winemetal — upload skipped");
      m_cmd->track(dstImage, DxvkAccess::Write);
      m_cmd->track(srcBuffer, DxvkAccess::Read);
      return;
    }

    for (uint32_t layer = 0; layer < dstSubresource.layerCount; layer++) {
      wmtcmd_blit_copy_from_buffer_to_texture cmd = { };
      cmd.type = WMTBlitCommandCopyFromBufferToTexture;
      cmd.src = obj_handle_t(srcSlice.buffer);
      cmd.src_offset = srcSlice.offset
        + VkDeviceSize(layer) * bytesPerSlice * blockCount.depth;
      cmd.bytes_per_row = uint32_t(bytesPerRow);
      cmd.bytes_per_image = uint32_t(bytesPerSlice);
      cmd.size = { dstExtent.width, dstExtent.height, dstExtent.depth };
      cmd.dst = obj_handle_t(dstImage->handle());
      cmd.slice = dstSubresource.baseArrayLayer + layer;
      cmd.level = dstSubresource.mipLevel;
      cmd.origin = { uint64_t(dstOffset.x), uint64_t(dstOffset.y), uint64_t(dstOffset.z) };
      d9mt::encodeBlitCmd(state, &cmd);
    }

    m_cmd->track(dstImage, DxvkAccess::Write);
    m_cmd->track(srcBuffer, DxvkAccess::Read);
  }


  void DxvkContext::copyImageToBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment,
          VkFormat              dstFormat,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            srcExtent) {
    this->spillRenderPass(true);
    this->prepareImage(srcImage, vk::makeSubresourceRange(srcSubresource));

    // winemetal's texture-to-buffer copy has no blit-option field, so
    // depth-stencil readback (INTZ / depth Lock) is not available yet
    if (srcImage->formatInfo()->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      Logger::err("d9mt: copyImageToBuffer: depth-stencil readback not implemented");
      return;
    }

    uint32_t elementSize = 0u;
    VkExtent3D blockSize = { };
    WMTBlitOption option = WMTBlitOptionNone;

    if (!getBufferImageCopyDesc(srcImage, srcSubresource.aspectMask,
        dstFormat, &elementSize, &blockSize, &option))
      return;

    VkExtent3D blockCount = util::computeBlockCount(srcExtent, blockSize);

    VkDeviceSize bytesPerRow = VkDeviceSize(blockCount.width) * elementSize;
    if (rowAlignment > elementSize)
      bytesPerRow = align(bytesPerRow, rowAlignment);

    VkDeviceSize bytesPerSlice = VkDeviceSize(blockCount.height) * bytesPerRow;
    if (sliceAlignment > elementSize)
      bytesPerSlice = align(bytesPerSlice, sliceAlignment);

    auto dstSlice = dstBuffer->getSliceInfo(dstOffset,
      bytesPerSlice * blockCount.depth * srcSubresource.layerCount);

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    for (uint32_t layer = 0; layer < srcSubresource.layerCount; layer++) {
      wmtcmd_blit_copy_from_texture_to_buffer cmd = { };
      cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
      cmd.src = obj_handle_t(srcImage->handle());
      cmd.slice = srcSubresource.baseArrayLayer + layer;
      cmd.level = srcSubresource.mipLevel;
      cmd.origin = { uint64_t(srcOffset.x), uint64_t(srcOffset.y), uint64_t(srcOffset.z) };
      cmd.size = { srcExtent.width, srcExtent.height, srcExtent.depth };
      cmd.dst = obj_handle_t(dstSlice.buffer);
      cmd.offset = dstSlice.offset
        + VkDeviceSize(layer) * bytesPerSlice * blockCount.depth;
      cmd.bytes_per_row = uint32_t(bytesPerRow);
      cmd.bytes_per_image = uint32_t(bytesPerSlice);
      d9mt::encodeBlitCmd(state, &cmd);
    }

    m_cmd->track(dstBuffer, DxvkAccess::Write);
    m_cmd->track(srcImage, DxvkAccess::Read);
  }


  void DxvkContext::copyImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    this->spillRenderPass(true);
    this->prepareImage(dstImage, vk::makeSubresourceRange(dstSubresource));
    this->prepareImage(srcImage, vk::makeSubresourceRange(srcSubresource));

    const auto* srcCaps = d9mt::lookupFormatCaps(srcImage->info().format);
    const auto* dstCaps = d9mt::lookupFormatCaps(dstImage->info().format);

    if (!srcCaps || !dstCaps) {
      Logger::err(str::format("d9mt: copyImage: unsupported formats ",
        uint32_t(srcImage->info().format), " -> ", uint32_t(dstImage->info().format)));
      return;
    }

    if (srcImage->info().sampleCount != dstImage->info().sampleCount) {
      Logger::err("d9mt: copyImage: sample counts differ (use resolveImage)");
      return;
    }

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    // Cross-format size-compatible copies (Vulkan raw-bit semantics):
    // alias the DESTINATION subresource with the source's pixel format via
    // a transient texture view (plain color images are created with
    // PixelFormatView usage, resources decision 3). Depth/BC/MSAA aliasing
    // is not possible on Metal — fail loud.
    bool crossFormat = srcCaps->wmtFormat != dstCaps->wmtFormat;

    if (crossFormat) {
      bool srcDepth = bool(srcImage->formatInfo()->aspectMask
        & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
      bool dstDepth = bool(dstImage->formatInfo()->aspectMask
        & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

      bool blockCompressed =
          srcImage->formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed)
       || dstImage->formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed);

      if (srcDepth || dstDepth || blockCompressed
       || srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
       || srcImage->formatInfo()->elementSize != dstImage->formatInfo()->elementSize) {
        Logger::err(str::format("d9mt: copyImage: incompatible cross-format copy ",
          uint32_t(srcImage->info().format), " -> ", uint32_t(dstImage->info().format)));
        return;
      }
    }

    bool is3D = dstImage->info().type == VK_IMAGE_TYPE_3D;

    if (crossFormat && is3D) {
      Logger::err("d9mt: copyImage: cross-format copy of 3D images not implemented");
      return;
    }

    WMTTextureSwizzleChannels identity = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
      WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha };

    for (uint32_t layer = 0; layer < dstSubresource.layerCount; layer++) {
      obj_handle_t dstHandle = obj_handle_t(dstImage->handle());
      uint32_t dstLevel = dstSubresource.mipLevel;
      uint32_t dstSlice = dstSubresource.baseArrayLayer + layer;

      obj_handle_t aliasView = 0;

      if (crossFormat) {
        // view scoped to the destination subresource, in the source format
        uint64_t gpuResourceId = 0;
        aliasView = MTLTexture_newTextureView(dstHandle,
          srcCaps->wmtFormat, WMTTextureType2D,
          dstLevel, 1u, dstSlice, 1u, identity, &gpuResourceId);

        if (!aliasView) {
          Logger::err("d9mt: copyImage: format alias view creation failed "
            "(destination lacks PixelFormatView usage?)");
          return;
        }

        dstHandle = aliasView;
        dstLevel = 0u;
        dstSlice = 0u;
      }

      wmtcmd_blit_copy_from_texture_to_texture cmd = { };
      cmd.type = WMTBlitCommandCopyFromTextureToTexture;
      cmd.src = obj_handle_t(srcImage->handle());
      cmd.src_slice = srcSubresource.baseArrayLayer + layer;
      cmd.src_level = srcSubresource.mipLevel;
      cmd.src_origin = { uint64_t(srcOffset.x), uint64_t(srcOffset.y), uint64_t(srcOffset.z) };
      cmd.src_size = { extent.width, extent.height, extent.depth };
      cmd.dst = dstHandle;
      cmd.dst_slice = dstSlice;
      cmd.dst_level = dstLevel;
      cmd.dst_origin = { uint64_t(dstOffset.x), uint64_t(dstOffset.y), uint64_t(dstOffset.z) };
      d9mt::encodeBlitCmd(state, &cmd);

      // the command buffer holds its own reference once encoded
      if (aliasView)
        NSObject_release(aliasView);
    }

    m_cmd->track(dstImage, DxvkAccess::Write);
    m_cmd->track(srcImage, DxvkAccess::Read);
  }


  void DxvkContext::initBuffer(
    const Rc<DxvkBuffer>&           buffer) {
    auto dstSlice = buffer->getSliceInfo();

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    wmtcmd_blit_fillbuffer cmd = { };
    cmd.type = WMTBlitCommandFillBuffer;
    cmd.buffer = obj_handle_t(dstSlice.buffer);
    cmd.offset = dstSlice.offset;
    cmd.length = dstSlice.size;
    cmd.value = 0u;
    d9mt::encodeBlitCmd(state, &cmd);

    m_cmd->track(buffer, DxvkAccess::Write);
  }


  void DxvkContext::initImage(
    const Rc<DxvkImage>&            image,
          VkImageLayout             initialLayout) {
    if (initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      m_cmd->track(image, DxvkAccess::None);
      return;
    }

    VkImageSubresourceRange subresources = image->getAvailableSubresources();
    auto formatInfo = image->formatInfo();

    const auto* caps = d9mt::lookupFormatCaps(image->info().format);

    bool isDepth = bool(formatInfo->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

    bool renderable = caps && (caps->optimal
      & (VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT
       | VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT));

    if (renderable && !formatInfo->flags.test(DxvkFormatFlag::BlockCompressed)) {
      // clear to zero via render-pass load actions, one pass per mip
      // (all layers at once via render_target_array_length)
      VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;

      if (image->info().type == VK_IMAGE_TYPE_3D)
        viewType = VK_IMAGE_VIEW_TYPE_3D;
      else if (image->info().numLayers > 1u)
        viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

      DxvkImageViewKey viewKey = { };
      viewKey.viewType   = viewType;
      viewKey.usage      = isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      viewKey.format     = image->info().format;
      viewKey.aspects    = formatInfo->aspectMask;
      viewKey.layerIndex = 0u;
      viewKey.layerCount = uint16_t(image->info().numLayers);
      viewKey.layout     = isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      viewKey.mipCount   = 1u;

      auto& state = d9mt::cmdListState(m_cmd.ptr());

      for (uint32_t level = 0; level < image->info().mipLevels; level++) {
        viewKey.mipIndex = uint8_t(level);

        Rc<DxvkImageView> view = image->createView(viewKey);
        obj_handle_t viewHandle = view != nullptr ? obj_handle_t(view->handle()) : 0u;

        if (!viewHandle) {
          Logger::err("d9mt: initImage: failed to create clear view");
          return;
        }

        VkExtent3D extent = image->mipLevelExtent(level);

        // 3D render targets address one depth plane per pass
        uint32_t planeCount = (viewType == VK_IMAGE_VIEW_TYPE_3D) ? extent.depth : 1u;

        for (uint32_t plane = 0; plane < planeCount; plane++) {
          WMTRenderPassInfo pass = { };
          pass.render_target_width  = extent.width;
          pass.render_target_height = extent.height;

          if (viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY)
            pass.render_target_array_length = uint8_t(image->info().numLayers);

          if (isDepth) {
            pass.depth.texture = viewHandle;
            pass.depth.load_action = WMTLoadActionClear;
            pass.depth.store_action = WMTStoreActionStore;
            pass.depth.clear_depth = 0.0f;
            pass.stencil.texture = viewHandle;
            pass.stencil.load_action = WMTLoadActionClear;
            pass.stencil.store_action = WMTStoreActionStore;
            pass.stencil.clear_stencil = 0u;
          } else {
            pass.colors[0].texture = viewHandle;
            pass.colors[0].load_action = WMTLoadActionClear;
            pass.colors[0].store_action = WMTStoreActionStore;
            pass.colors[0].depth_plane = plane;
          }

          d9mt::encodeEmptyRenderPass(state, pass);
        }
      }
    } else {
      // non-renderable (BC, ...): copy zeroes from a buffer per subresource
      VkExtent3D topExtent = image->mipLevelExtent(0);
      VkExtent3D topBlocks = util::computeBlockCount(topExtent, formatInfo->blockSize);

      VkDeviceSize dataSize = VkDeviceSize(util::flattenImageExtent(topBlocks))
                            * formatInfo->elementSize;

      auto zeroSlice = createZeroBuffer(dataSize)->getSliceInfo();

      auto& state = d9mt::cmdListState(m_cmd.ptr());

      for (uint32_t level = 0; level < image->info().mipLevels; level++) {
        VkExtent3D extent = image->mipLevelExtent(level);
        VkExtent3D blocks = util::computeBlockCount(extent, formatInfo->blockSize);

        for (uint32_t layer = 0; layer < image->info().numLayers; layer++) {
          wmtcmd_blit_copy_from_buffer_to_texture cmd = { };
          cmd.type = WMTBlitCommandCopyFromBufferToTexture;
          cmd.src = obj_handle_t(zeroSlice.buffer);
          cmd.src_offset = zeroSlice.offset;
          cmd.bytes_per_row = blocks.width * formatInfo->elementSize;
          cmd.bytes_per_image = blocks.height * blocks.width * formatInfo->elementSize;
          cmd.size = { extent.width, extent.height, extent.depth };
          cmd.dst = obj_handle_t(image->handle());
          cmd.slice = layer;
          cmd.level = level;
          cmd.origin = { 0u, 0u, 0u };
          d9mt::encodeBlitCmd(state, &cmd);
        }
      }
    }

    m_cmd->track(image, DxvkAccess::Write);
  }


  void DxvkContext::invalidateBuffer(
    const Rc<DxvkBuffer>&           buffer,
          Rc<DxvkResourceAllocation>&& slice) {
    Rc<DxvkResourceAllocation> prevAllocation = buffer->assignStorage(std::move(slice));
    m_cmd->track(std::move(prevAllocation));

    buffer->resetTracking();

    // We also need to update all bindings that the buffer
    // may be bound to either directly or through views.
    VkBufferUsageFlags usage = buffer->info().usage &
      ~(VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    // Fast early-out for plain uniform buffers, very common
    if (likely(usage == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
      m_descriptorState.dirtyBuffers(buffer->getShaderStages());
      return;
    }

    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
      m_descriptorState.dirtyBuffers(buffer->getShaderStages());

    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
      m_descriptorState.dirtyViews(buffer->getShaderStages());

    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);

    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);

    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::DirtyDrawBuffer);

    if (usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT)
      m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
  }


  Rc<DxvkBuffer> DxvkContext::createZeroBuffer(
          VkDeviceSize              size) {
    if (m_zeroBuffer && m_zeroBuffer->info().size >= size) {
      m_cmd->track(m_zeroBuffer, DxvkAccess::Read);
      return m_zeroBuffer;
    }

    DxvkBufferCreateInfo bufInfo;
    bufInfo.size    = align<VkDeviceSize>(size, 1 << 20);
    bufInfo.usage   = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.stages  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    bufInfo.access  = VK_ACCESS_TRANSFER_WRITE_BIT
                    | VK_ACCESS_TRANSFER_READ_BIT;
    bufInfo.debugName = "Zero buffer";

    m_zeroBuffer = m_device->createBuffer(bufInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // No GPU fill needed: backend buffers are VirtualAlloc'ed zero-filled
    // shared-storage allocations and nothing ever writes this one
    // (METAL-BACKEND-NOTES.md, resources decision 2).
    m_cmd->track(m_zeroBuffer, DxvkAccess::Read);
    return m_zeroBuffer;
  }


  void DxvkContext::freeZeroBuffer() {
    constexpr uint64_t ZeroBufferLifetime = 4096u;

    // Don't free the zero buffer if it is still kept alive by a prior
    // submission anyway
    if (!m_zeroBuffer || m_zeroBuffer->isInUse(DxvkAccess::Write))
      return;

    // Delete zero buffer if it hasn't been actively used in a while
    if (m_zeroBuffer->getTrackId() + ZeroBufferLifetime < m_trackingId)
      m_zeroBuffer = nullptr;
  }


  void DxvkContext::generateMipmaps(
    const Rc<DxvkImageView>&        imageView,
          VkFilter                  filter) {
    if (imageView->info().mipCount <= 1)
      return;

    this->spillRenderPass(true);
    this->prepareImage(imageView->image(), imageView->imageSubresources());

    // Metal blit mip generation always uses a linear-ish filter; the point
    // filter distinction is lost (matches the hand-rolled driver).
    auto& state = d9mt::cmdListState(m_cmd.ptr());

    wmtcmd_blit_generate_mipmaps cmd = { };
    cmd.type = WMTBlitCommandGenerateMipmaps;
    cmd.texture = obj_handle_t(imageView->image()->handle());
    d9mt::encodeBlitCmd(state, &cmd);

    m_cmd->track(imageView->image(), DxvkAccess::Write);
  }


  // --------------------------------------------------------------------------
  // Draw-stage operations — fail loud, never silently wrong
  // --------------------------------------------------------------------------

  void DxvkContext::blitImageView(
    const Rc<DxvkImageView>&    dstView,
    const VkOffset3D*           dstOffsets,
    const Rc<DxvkImageView>&    srcView,
    const VkOffset3D*           srcOffsets,
          VkFilter              filter) {
    // Identity-size, format-identical blits can be served by a plain copy
    VkOffset3D dstSize = {
      dstOffsets[1].x - dstOffsets[0].x,
      dstOffsets[1].y - dstOffsets[0].y,
      dstOffsets[1].z - dstOffsets[0].z };

    VkOffset3D srcSize = {
      srcOffsets[1].x - srcOffsets[0].x,
      srcOffsets[1].y - srcOffsets[0].y,
      srcOffsets[1].z - srcOffsets[0].z };

    const auto* srcCaps = d9mt::lookupFormatCaps(srcView->image()->info().format);
    const auto* dstCaps = d9mt::lookupFormatCaps(dstView->image()->info().format);

    bool isCopy = dstSize == srcSize
               && dstSize.x > 0 && dstSize.y > 0 && dstSize.z > 0
               && srcCaps && dstCaps && srcCaps->wmtFormat == dstCaps->wmtFormat
               && srcView->image()->info().sampleCount == VK_SAMPLE_COUNT_1_BIT
               && dstView->image()->info().sampleCount == VK_SAMPLE_COUNT_1_BIT;

    if (isCopy) {
      auto dstSubresource = vk::pickSubresourceLayers(dstView->imageSubresources(), 0);
      auto srcSubresource = vk::pickSubresourceLayers(srcView->imageSubresources(), 0);

      this->copyImage(
        dstView->image(), dstSubresource, dstOffsets[0],
        srcView->image(), srcSubresource, srcOffsets[0],
        VkExtent3D { uint32_t(dstSize.x), uint32_t(dstSize.y), uint32_t(dstSize.z) });
      return;
    }

    // General path: fullscreen-triangle sample pass (scaling, format
    // conversion, mirroring, view swizzles via the sampled source view).
    this->spillRenderPass(true);
    this->prepareImage(dstView->image(), dstView->imageSubresources());
    this->prepareImage(srcView->image(), srcView->imageSubresources());

    if (srcView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
     || dstView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      Logger::err("d9mt: blitImageView: multisampled blit not implemented");
      return;
    }

    if (dstView->formatInfo()->aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      Logger::err("d9mt: blitImageView: depth-stencil blit not implemented");
      return;
    }

    if (dstView->info().layerCount > 1u || srcView->info().layerCount > 1u
     || dstSize.z != 1 || srcSize.z != 1) {
      Logger::err("d9mt: blitImageView: layered/3D blit not implemented");
      return;
    }

    // normalize mirroring: flip both rects per axis so the destination is
    // forward; reversed source rects become a negative uv scale
    VkOffset3D d0 = dstOffsets[0], d1 = dstOffsets[1];
    VkOffset3D s0 = srcOffsets[0], s1 = srcOffsets[1];

    if (d1.x < d0.x) { std::swap(d0.x, d1.x); std::swap(s0.x, s1.x); }
    if (d1.y < d0.y) { std::swap(d0.y, d1.y); std::swap(s0.y, s1.y); }

    WMTPixelFormat dstFormat = d9mt::wmtFormatFor(dstView->info().format);
    if (dstFormat == WMTPixelFormatInvalid) {
      Logger::err(str::format("d9mt: blitImageView: unsupported destination format ",
        uint32_t(dstView->info().format)));
      return;
    }

    if (dstView->info().packedSwizzle) {
      static bool s_warnedSwizzle = false;
      if (!std::exchange(s_warnedSwizzle, true))
        Logger::err("d9mt: blitImageView: swizzled destination rendered "
          "without swizzle (known deviation)");
    }

    // identity-swizzle RTV over the destination subresource
    auto dstSubres = dstView->imageSubresources();

    DxvkImageViewKey rtKey = { };
    rtKey.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    rtKey.usage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    rtKey.format     = dstView->info().format;
    rtKey.aspects    = VK_IMAGE_ASPECT_COLOR_BIT;
    rtKey.mipIndex   = uint8_t(dstSubres.baseMipLevel);
    rtKey.mipCount   = 1u;
    rtKey.layerIndex = uint16_t(dstSubres.baseArrayLayer);
    rtKey.layerCount = 1u;
    rtKey.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    Rc<DxvkImageView> rtv = dstView->image()->createView(rtKey);
    obj_handle_t rtvHandle = rtv != nullptr ? obj_handle_t(rtv->handle()) : 0u;

    if (!rtvHandle) {
      Logger::err("d9mt: blitImageView: failed to create destination RTV");
      return;
    }

    // 2D sample view of the source (carries the view swizzle)
    obj_handle_t srcHandle = obj_handle_t(srcView->handle(VK_IMAGE_VIEW_TYPE_2D));
    if (!srcHandle) {
      Logger::err("d9mt: blitImageView: failed to create 2D source view");
      return;
    }

    obj_handle_t pso = d9mt::getBlitPso(dstFormat, filter == VK_FILTER_NEAREST);
    if (!pso)
      return;

    VkExtent3D dstExtent = dstView->mipLevelExtent(0);
    VkExtent3D srcExtent = srcView->mipLevelExtent(0);

    bool fullDst = d0.x == 0 && d0.y == 0
      && uint32_t(d1.x) == dstExtent.width
      && uint32_t(d1.y) == dstExtent.height;

    WMTRenderPassInfo pass = { };
    pass.render_target_width  = dstExtent.width;
    pass.render_target_height = dstExtent.height;
    pass.colors[0].texture = rtvHandle;
    pass.colors[0].load_action = fullDst ? WMTLoadActionDontCare : WMTLoadActionLoad;
    pass.colors[0].store_action = WMTStoreActionStore;

    obj_handle_t enc = d9mt::cmdListBeginRenderPass(m_cmd.ptr(), pass);
    if (!enc)
      return;

    d9mt::BlitParams params = { };
    params.uvOffset[0] = float(s0.x) / float(srcExtent.width);
    params.uvOffset[1] = float(s0.y) / float(srcExtent.height);
    params.uvScale[0]  = float(s1.x - s0.x) / float(srcExtent.width);
    params.uvScale[1]  = float(s1.y - s0.y) / float(srcExtent.height);

    wmtcmd_render_setpso setPso = { };
    wmtcmd_render_setviewport setVp = { };
    wmtcmd_render_setscissorrect setSc = { };
    wmtcmd_render_useresource use = { };
    wmtcmd_render_settexture setTex = { };
    wmtcmd_render_setbytes setBytes = { };
    wmtcmd_render_draw drawCmd = { };

    setPso.type = WMTRenderCommandSetPSO;
    setPso.next.set(&setVp);
    setPso.pso = pso;

    setVp.type = WMTRenderCommandSetViewport;
    setVp.next.set(&setSc);
    setVp.viewport = { double(d0.x), double(d0.y),
                       double(d1.x - d0.x), double(d1.y - d0.y), 0.0, 1.0 };

    setSc.type = WMTRenderCommandSetScissorRect;
    setSc.next.set(&use);
    setSc.scissor_rect = {
      uint64_t(std::max(d0.x, 0)),
      uint64_t(std::max(d0.y, 0)),
      std::min(uint64_t(d1.x - d0.x), uint64_t(dstExtent.width)),
      std::min(uint64_t(d1.y - d0.y), uint64_t(dstExtent.height)) };

    use.type = WMTRenderCommandUseResource;
    use.next.set(&setTex);
    use.resource = srcHandle;
    use.usage = WMTResourceUsageRead;
    use.stages = WMTRenderStages(WMTRenderStageFragment);

    setTex.type = WMTRenderCommandSetFragmentTexture;
    setTex.next.set(&setBytes);
    setTex.texture = srcHandle;
    setTex.index = 0;

    setBytes.type = WMTRenderCommandSetFragmentBytes;
    setBytes.next.set(&drawCmd);
    setBytes.bytes.set(&params);
    setBytes.length = sizeof(params);
    setBytes.index = 0;

    drawCmd.type = WMTRenderCommandDraw;
    drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
    drawCmd.vertex_start = 0;
    drawCmd.vertex_count = 3;
    drawCmd.instance_count = 1;
    drawCmd.base_instance = 0;

    MTLRenderCommandEncoder_encodeCommands(enc,
      reinterpret_cast<const wmtcmd_base*>(&setPso));

    d9mt::cmdListEndEncoder(m_cmd.ptr());

    m_cmd->track(dstView->image(), DxvkAccess::Write);
    m_cmd->track(srcView->image(), DxvkAccess::Read);
  }


  void DxvkContext::resolveImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format,
          VkResolveModeFlagBits     mode,
          VkResolveModeFlagBits     stencilMode) {
    this->spillRenderPass(true);
    this->prepareImage(dstImage, vk::makeSubresourceRange(region.dstSubresource));
    this->prepareImage(srcImage, vk::makeSubresourceRange(region.srcSubresource));

    VkExtent3D srcExtent = srcImage->mipLevelExtent(region.srcSubresource.mipLevel);

    if (region.srcOffset != VkOffset3D { 0, 0, 0 }
     || region.dstOffset != VkOffset3D { 0, 0, 0 }
     || region.extent != srcExtent) {
      static bool s_warnedPartial = false;
      if (!std::exchange(s_warnedPartial, true))
        Logger::err("d9mt: resolveImage: partial resolve not implemented");
      return;
    }

    if (region.srcSubresource.aspectMask
      & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      // upstream's framebuffer-resolve helper slot doubles as our
      // sample-pass depth-stencil resolve (declared in the vendored header)
      this->resolveImageFb(dstImage, srcImage, region, format, mode, stencilMode);
      return;
    }

    if (mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) {
      static bool s_warnedZero = false;
      if (!std::exchange(s_warnedZero, true))
        Logger::err("d9mt: resolveImage: SAMPLE_ZERO color resolve "
          "approximated as AVERAGE (Metal resolve attachment)");
    }

    // empty render pass over the MSAA source whose store action resolves
    // into the destination; Store+Resolve keeps the source contents intact
    auto& state = d9mt::cmdListState(m_cmd.ptr());

    for (uint32_t layer = 0; layer < region.dstSubresource.layerCount; layer++) {
      WMTRenderPassInfo pass = { };
      pass.render_target_width  = srcExtent.width;
      pass.render_target_height = srcExtent.height;

      auto& att = pass.colors[0];
      att.texture = obj_handle_t(srcImage->handle());
      att.level = uint16_t(region.srcSubresource.mipLevel);
      att.slice = uint16_t(region.srcSubresource.baseArrayLayer + layer);
      att.load_action  = WMTLoadActionLoad;
      att.store_action = WMTStoreActionStoreAndMultisampleResolve;
      att.resolve_texture = obj_handle_t(dstImage->handle());
      att.resolve_level = uint16_t(region.dstSubresource.mipLevel);
      att.resolve_slice = uint16_t(region.dstSubresource.baseArrayLayer + layer);

      d9mt::encodeEmptyRenderPass(state, pass);
    }

    m_cmd->track(dstImage, DxvkAccess::Write);
    m_cmd->track(srcImage, DxvkAccess::Read);
  }


  void DxvkContext::resolveImageFb(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format,
          VkResolveModeFlagBits     depthMode,
          VkResolveModeFlagBits     stencilMode) {
    // Depth(-stencil) SAMPLE_ZERO resolve (BACKEND-SURFACE §1.4; the
    // GTA IV ResolveZ / INTZ path): winemetal depth attachments expose no
    // resolve target, so this is a fullscreen-triangle pass over the 1x
    // destination's depth/stencil attachments whose fragment shader exports
    // [[depth(any)]] (+ [[stencil]]) read from sample 0 of the MSAA source.
    // AVERAGE depth is performed as SAMPLE_ZERO — which is the documented
    // d3d9 resolve semantic anyway ("the resolve operation copies the depth
    // value from the first sample only", AMD Advanced DX9 Capabilities).
    // Caller (resolveImage) already spilled the render pass, prepared both
    // images and rejected partial regions.
    VkImageAspectFlags aspects = region.srcSubresource.aspectMask;

    bool resolveDepth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      && depthMode != VK_RESOLVE_MODE_NONE;
    bool resolveStencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
      && stencilMode != VK_RESOLVE_MODE_NONE;

    if (!resolveDepth && !resolveStencil)
      return;

    if (!resolveDepth) {
      // both shader variants export depth; no d3d9 call site resolves
      // stencil alone (modes per BACKEND-SURFACE §1.4)
      Logger::err("d9mt: resolveImage: stencil-only resolve not implemented");
      return;
    }

    const auto* srcCaps = d9mt::lookupFormatCaps(srcImage->info().format);
    const auto* dstCaps = d9mt::lookupFormatCaps(dstImage->info().format);

    if (!srcCaps || !dstCaps
     || srcCaps->wmtFormat != WMTPixelFormatDepth32Float_Stencil8
     || dstCaps->wmtFormat != WMTPixelFormatDepth32Float_Stencil8) {
      Logger::err(str::format("d9mt: resolveImage: unexpected depth formats (src ",
        uint32_t(srcImage->info().format), ", dst ", uint32_t(dstImage->info().format), ")"));
      return;
    }

    if (resolveDepth && depthMode != VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) {
      // not a deviation worth a warn: native d3d9 depth resolves copy the
      // first sample only (upstream picks AVERAGE on this path for Vulkan
      // driver-support reasons that do not apply here)
      static bool s_loggedAvg = false;
      if (!std::exchange(s_loggedAvg, true))
        Logger::info("d9mt: resolveImage: AVERAGE depth resolve performed as SAMPLE_ZERO");
    }

    obj_handle_t pso = d9mt::getDepthResolvePso(resolveStencil);
    obj_handle_t dsso = d9mt::getDepthResolveDsso(resolveStencil);

    if (!pso || !dsso)
      return;

    VkExtent3D dstExtent = dstImage->mipLevelExtent(region.dstSubresource.mipLevel);

    for (uint32_t layer = 0; layer < region.dstSubresource.layerCount; layer++) {
      // sample-0 source views (cached on the image's allocation; kept alive
      // by the source image's command-list tracking below)
      DxvkImageViewKey srcKey = { };
      srcKey.viewType   = VK_IMAGE_VIEW_TYPE_2D;
      srcKey.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
      srcKey.format     = srcImage->info().format;
      srcKey.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      srcKey.aspects    = VK_IMAGE_ASPECT_DEPTH_BIT;
      srcKey.mipIndex   = uint8_t(region.srcSubresource.mipLevel);
      srcKey.mipCount   = 1u;
      srcKey.layerIndex = uint16_t(region.srcSubresource.baseArrayLayer + layer);
      srcKey.layerCount = 1u;

      Rc<DxvkImageView> srcDepthView = srcImage->createView(srcKey);
      obj_handle_t srcDepthHandle = srcDepthView != nullptr
        ? obj_handle_t(srcDepthView->handle()) : 0u;

      obj_handle_t srcStencilHandle = 0u;

      if (resolveStencil) {
        srcKey.aspects = VK_IMAGE_ASPECT_STENCIL_BIT; // → X32_Stencil8 alias
        Rc<DxvkImageView> srcStencilView = srcImage->createView(srcKey);
        srcStencilHandle = srcStencilView != nullptr
          ? obj_handle_t(srcStencilView->handle()) : 0u;
      }

      if (!srcDepthHandle || (resolveStencil && !srcStencilHandle)) {
        Logger::err("d9mt: resolveImage: failed to create depth resolve source views");
        return;
      }

      WMTRenderPassInfo pass = { };
      pass.render_target_width  = dstExtent.width;
      pass.render_target_height = dstExtent.height;

      // unified-DS rule: both planes always attached (Depth32Float_Stencil8);
      // the aspect we do not resolve is loaded and stored unchanged
      pass.depth.texture = obj_handle_t(dstImage->handle());
      pass.depth.level = uint16_t(region.dstSubresource.mipLevel);
      pass.depth.slice = uint16_t(region.dstSubresource.baseArrayLayer + layer);
      pass.depth.load_action  = WMTLoadActionDontCare; // fully overwritten
      pass.depth.store_action = WMTStoreActionStore;

      pass.stencil.texture = pass.depth.texture;
      pass.stencil.level = pass.depth.level;
      pass.stencil.slice = pass.depth.slice;
      pass.stencil.load_action  = resolveStencil ? WMTLoadActionDontCare : WMTLoadActionLoad;
      pass.stencil.store_action = WMTStoreActionStore;

      obj_handle_t enc = d9mt::cmdListBeginRenderPass(m_cmd.ptr(), pass);
      if (!enc)
        return;

      wmtcmd_render_setpso setPso = { };
      wmtcmd_render_setdsso setDsso = { };
      wmtcmd_render_setviewport setVp = { };
      wmtcmd_render_useresource useDepth = { };
      wmtcmd_render_useresource useStencil = { };
      wmtcmd_render_settexture setTexDepth = { };
      wmtcmd_render_settexture setTexStencil = { };
      wmtcmd_render_draw drawCmd = { };

      setPso.type = WMTRenderCommandSetPSO;
      setPso.next.set(&setDsso);
      setPso.pso = pso;

      setDsso.type = WMTRenderCommandSetDSSO;
      setDsso.next.set(&setVp);
      setDsso.dsso = dsso;
      setDsso.stencil_ref = 0u; // replaced per-fragment by [[stencil]] export

      setVp.type = WMTRenderCommandSetViewport;
      setVp.next.set(&useDepth);
      setVp.viewport = { 0.0, 0.0,
                         double(dstExtent.width), double(dstExtent.height), 0.0, 1.0 };

      useDepth.type = WMTRenderCommandUseResource;
      useDepth.next.set(&setTexDepth);
      useDepth.resource = srcDepthHandle;
      useDepth.usage = WMTResourceUsageRead;
      useDepth.stages = WMTRenderStages(WMTRenderStageFragment);

      setTexDepth.type = WMTRenderCommandSetFragmentTexture;
      setTexDepth.texture = srcDepthHandle;
      setTexDepth.index = 0;

      if (resolveStencil) {
        setTexDepth.next.set(&useStencil);

        useStencil.type = WMTRenderCommandUseResource;
        useStencil.next.set(&setTexStencil);
        useStencil.resource = srcStencilHandle;
        useStencil.usage = WMTResourceUsageRead;
        useStencil.stages = WMTRenderStages(WMTRenderStageFragment);

        setTexStencil.type = WMTRenderCommandSetFragmentTexture;
        setTexStencil.next.set(&drawCmd);
        setTexStencil.texture = srcStencilHandle;
        setTexStencil.index = 1;
      } else {
        setTexDepth.next.set(&drawCmd);
      }

      drawCmd.type = WMTRenderCommandDraw;
      drawCmd.primitive_type = WMTPrimitiveTypeTriangle;
      drawCmd.vertex_start = 0;
      drawCmd.vertex_count = 3;
      drawCmd.instance_count = 1;
      drawCmd.base_instance = 0;

      MTLRenderCommandEncoder_encodeCommands(enc,
        reinterpret_cast<const wmtcmd_base*>(&setPso));

      d9mt::cmdListEndEncoder(m_cmd.ptr());
    }

    m_cmd->track(dstImage, DxvkAccess::Write);
    m_cmd->track(srcImage, DxvkAccess::Read);
  }


  // --------------------------------------------------------------------------
  // queries / events
  // --------------------------------------------------------------------------

  void DxvkContext::beginQuery(const Rc<DxvkQuery>& query) {
    if (query->type() != VK_QUERY_TYPE_OCCLUSION) {
      Logger::err(str::format("d9mt: beginQuery: unsupported query type ",
        uint32_t(query->type())));
      return;
    }

    // A pass that started without a visibility-result buffer cannot count
    // samples: split it, so the next draw restarts the pass with the buffer
    // attached and the query restarted into a fresh slot.
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());
    if (cstate.kind == d9mt::EncoderKind::Render && !cstate.visAttached)
      this->spillRenderPass(true);

    d9mt::ctxDrawStateImpl(this).activeOcclusionCount += 1u;

    m_queryManager.enableQuery(m_cmd, query);
  }


  void DxvkContext::endQuery(const Rc<DxvkQuery>& query) {
    if (query->type() != VK_QUERY_TYPE_OCCLUSION) {
      Logger::err(str::format("d9mt: endQuery: unsupported query type ",
        uint32_t(query->type())));
      return;
    }

    auto& dstate = d9mt::ctxDrawStateImpl(this);
    if (dstate.activeOcclusionCount)
      dstate.activeOcclusionCount -= 1u;

    m_queryManager.disableQuery(m_cmd, query);
  }


  void DxvkContext::writeTimestamp(const Rc<DxvkQuery>& query) {
    if (query->type() != VK_QUERY_TYPE_TIMESTAMP) {
      Logger::err(str::format("d9mt: writeTimestamp: unsupported query type ",
        uint32_t(query->type())));
      return;
    }

    m_queryManager.writeTimestamp(m_cmd, query);
  }


  void DxvkContext::signalGpuEvent(const Rc<DxvkEvent>& event) {
    this->spillRenderPass(true);

    // mark pending; flipped to VK_EVENT_SET on the watcher thread when the
    // submission containing this signal retires (DxvkContext is a friend
    // of DxvkEvent, and so are lambdas defined in its member functions)
    {
      std::lock_guard<sync::Spinlock> lock(event->m_mutex);
      event->m_status = VK_EVENT_RESET;
    }

    auto& state = d9mt::cmdListState(m_cmd.ptr());

    Rc<DxvkEvent> trackedEvent = event;
    state.onComplete.push_back([trackedEvent] {
      std::lock_guard<sync::Spinlock> lock(trackedEvent->m_mutex);
      trackedEvent->m_status = VK_EVENT_SET;
    });
  }


  // --------------------------------------------------------------------------
  // debug labels — no Metal debug groups for now
  // --------------------------------------------------------------------------

  void DxvkContext::beginDebugLabel(const VkDebugUtilsLabelEXT& label) {

  }


  void DxvkContext::endDebugLabel() {

  }


  void DxvkContext::insertDebugLabel(const VkDebugUtilsLabelEXT& label) {

  }


  // --------------------------------------------------------------------------
  // state setters — PSO-key / dynamic-state shadow, mirrored from upstream
  // v2.7.1 dxvk_context.cpp (the Draw stage consumes m_state + m_flags)
  // --------------------------------------------------------------------------

  void DxvkContext::setViewports(
          uint32_t            viewportCount,
    const DxvkViewport*       viewports) {
    for (uint32_t i = 0; i < viewportCount; i++) {
      m_state.vp.viewports[i] = viewports[i].viewport;
      m_state.vp.scissorRects[i] = viewports[i].scissor;

      // Vulkan viewports are not allowed to have a width or
      // height of zero, so we fall back to a dummy viewport
      // and instead set an empty scissor rect, which is legal.
      if (viewports[i].viewport.width <= 0.0f || viewports[i].viewport.height == 0.0f) {
        m_state.vp.viewports[i] = VkViewport {
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        m_state.vp.scissorRects[i] = VkRect2D {
          VkOffset2D { 0, 0 },
          VkExtent2D { 0, 0 } };
      }
    }

    m_state.vp.viewportCount = viewportCount;
    m_flags.set(DxvkContextFlag::GpDirtyViewport);
  }


  void DxvkContext::setBlendConstants(
          DxvkBlendConstants  blendConstants) {
    if (m_state.dyn.blendConstants != blendConstants) {
      m_state.dyn.blendConstants = blendConstants;
      m_flags.set(DxvkContextFlag::GpDirtyBlendConstants);
    }
  }


  void DxvkContext::setDepthBias(
          DxvkDepthBias       depthBias) {
    if (m_state.dyn.depthBias != depthBias) {
      m_state.dyn.depthBias = depthBias;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBias);
    }
  }


  void DxvkContext::setDepthBiasRepresentation(
          DxvkDepthBiasRepresentation  depthBiasRepresentation) {
    if (m_state.dyn.depthBiasRepresentation != depthBiasRepresentation) {
      m_state.dyn.depthBiasRepresentation = depthBiasRepresentation;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBias);
    }
  }


  void DxvkContext::setDepthBounds(
          DxvkDepthBounds     depthBounds) {
    if (m_state.dyn.depthBounds != depthBounds) {
      m_state.dyn.depthBounds = depthBounds;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBounds);
    }
  }


  void DxvkContext::setStencilReference(
          uint32_t            reference) {
    if (m_state.dyn.stencilReference != reference) {
      m_state.dyn.stencilReference = reference;
      m_flags.set(DxvkContextFlag::GpDirtyStencilRef);
    }
  }


  void DxvkContext::setInputAssemblyState(const DxvkInputAssemblyState& ia) {
    m_state.gp.state.ia = DxvkIaInfo(
      ia.primitiveTopology(),
      ia.primitiveRestart(),
      ia.patchVertexCount());

    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }


  void DxvkContext::setInputLayout(
          uint32_t             attributeCount,
    const DxvkVertexInput*     attributes,
          uint32_t             bindingCount,
    const DxvkVertexInput*     bindings) {
    m_flags.set(
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyVertexBuffers);

    for (uint32_t i = 0; i < bindingCount; i++) {
      auto binding = bindings[i].binding();

      m_state.gp.state.ilBindings[i] = DxvkIlBinding(
        binding.binding, 0,
        binding.inputRate,
        binding.divisor);
      m_state.vi.vertexExtents[i] = binding.extent;
    }

    for (uint32_t i = bindingCount; i < m_state.gp.state.il.bindingCount(); i++) {
      m_state.gp.state.ilBindings[i] = DxvkIlBinding();
      m_state.vi.vertexExtents[i] = 0;
    }

    for (uint32_t i = 0; i < attributeCount; i++) {
      auto attribute = attributes[i].attribute();

      m_state.gp.state.ilAttributes[i] = DxvkIlAttribute(
        attribute.location,
        attribute.binding,
        attribute.format,
        attribute.offset);
    }

    for (uint32_t i = attributeCount; i < m_state.gp.state.il.attributeCount(); i++)
      m_state.gp.state.ilAttributes[i] = DxvkIlAttribute();

    m_state.gp.state.il = DxvkIlInfo(attributeCount, bindingCount);
  }


  void DxvkContext::setRasterizerState(const DxvkRasterizerState& rs) {
    VkCullModeFlags cullMode = rs.cullMode();
    VkFrontFace frontFace = rs.frontFace();

    if (m_state.dyn.cullMode != cullMode || m_state.dyn.frontFace != frontFace) {
      m_state.dyn.cullMode = cullMode;
      m_state.dyn.frontFace = frontFace;

      m_flags.set(DxvkContextFlag::GpDirtyRasterizerState);
    }

    if (unlikely(rs.sampleCount() != m_state.gp.state.rs.sampleCount())) {
      if (!m_state.gp.state.ms.sampleCount())
        m_flags.set(DxvkContextFlag::GpDirtyMultisampleState);

      if (!m_features.test(DxvkContextFeature::VariableMultisampleRate))
        m_flags.set(DxvkContextFlag::GpRenderPassNeedsFlush);
    }

    DxvkRsInfo rsInfo(
      rs.depthClip(),
      rs.polygonMode(),
      rs.sampleCount(),
      rs.conservativeMode(),
      rs.flatShading(),
      rs.lineMode());

    if (!m_state.gp.state.rs.eq(rsInfo)) {
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState,
                  DxvkContextFlag::GpDirtyDepthClip);
      m_state.gp.state.rs = rsInfo;
    }
  }


  void DxvkContext::setMultisampleState(const DxvkMultisampleState& ms) {
    m_state.gp.state.ms = DxvkMsInfo(
      m_state.gp.state.ms.sampleCount(),
      ms.sampleMask(),
      ms.alphaToCoverage());

    m_flags.set(
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyMultisampleState);
  }


  void DxvkContext::setDepthStencilState(const DxvkDepthStencilState& ds) {
    if (m_state.dyn.depthStencilState.depthTest() != ds.depthTest()
     || m_state.dyn.depthStencilState.depthWrite() != ds.depthWrite()
     || m_state.dyn.depthStencilState.depthCompareOp() != ds.depthCompareOp())
      m_flags.set(DxvkContextFlag::GpDirtyDepthTest);

    if (m_state.dyn.depthStencilState.stencilTest() != ds.stencilTest()
     || !m_state.dyn.depthStencilState.stencilOpFront().eq(ds.stencilOpFront())
     || !m_state.dyn.depthStencilState.stencilOpBack().eq(ds.stencilOpBack()))
      m_flags.set(DxvkContextFlag::GpDirtyStencilTest);

    m_state.dyn.depthStencilState = ds;
  }


  void DxvkContext::setLogicOpState(const DxvkLogicOpState& lo) {
    m_state.gp.state.om = DxvkOmInfo(
      lo.logicOpEnable(),
      lo.logicOp(),
      m_state.gp.state.om.feedbackLoop());

    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }


  void DxvkContext::setBlendMode(
          uint32_t            attachment,
    const DxvkBlendMode&      blendMode) {
    m_state.gp.state.omBlend[attachment] = DxvkOmAttachmentBlend(
      blendMode.blendEnable(),
      blendMode.colorSrcFactor(),
      blendMode.colorDstFactor(),
      blendMode.colorBlendOp(),
      blendMode.alphaSrcFactor(),
      blendMode.alphaDstFactor(),
      blendMode.alphaBlendOp(),
      blendMode.writeMask());

    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }

}

namespace dxvk {

  // ==========================================================================
  // Draw path (Draw stage). Lazily (re)starts the Metal render pass, builds
  // the PSO from the packed pipeline state, flushes argument-buffer words +
  // push data + sampler heap, then encodes the draw. See METAL-BACKEND-
  // NOTES.md "Stage decisions: draw" for the full design.
  // ==========================================================================

  // ----------------------------------------------------------------------
  // render targets: packed rt/swizzle/sample-count state for the PSO key +
  // framebuffer info for pass creation
  // ----------------------------------------------------------------------

  void DxvkContext::updateRenderTargets() {
    auto& dstate = d9mt::ctxDrawStateImpl(this);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    // end the current pass; pending clears get handled at startRenderPass
    if (cstate.kind == d9mt::EncoderKind::Render) {
      m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);
      d9mt::endEncoder(cstate);
    }

    m_flags.clr(DxvkContextFlag::GpRenderPassBound);

    const auto& rt = m_state.om.renderTargets;

    std::array<VkFormat, MaxNumRenderTargets> colorFormats = { };
    uint32_t colorCount  = 0u;
    uint32_t sampleCount = 0u;
    uint32_t layerCount  = 1u;

    VkExtent2D extent = { ~0u, ~0u };
    bool anyAttachment = false;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      m_state.gp.state.omSwizzle[i] = DxvkOmAttachmentSwizzle();

      const auto& view = rt.color[i].view;
      if (view == nullptr)
        continue;

      colorFormats[i] = view->info().format;
      colorCount = i + 1u;

      m_state.gp.state.omSwizzle[i] =
        DxvkOmAttachmentSwizzle(view->info().unpackSwizzle());

      VkExtent3D mip = view->mipLevelExtent(0);
      extent.width  = std::min(extent.width,  mip.width);
      extent.height = std::min(extent.height, mip.height);
      sampleCount   = view->image()->info().sampleCount;
      layerCount    = std::max(layerCount, uint32_t(view->info().layerCount));
      anyAttachment = true;
    }

    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags readOnlyAspects = 0u;

    if (rt.depth.view != nullptr) {
      const auto& view = rt.depth.view;
      depthFormat = view->info().format;

      switch (view->info().layout) {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
          readOnlyAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
          break;
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
          readOnlyAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
          break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
          readOnlyAspects = VK_IMAGE_ASPECT_STENCIL_BIT;
          break;
        default:
          break;
      }

      VkExtent3D mip = view->mipLevelExtent(0);
      extent.width  = std::min(extent.width,  mip.width);
      extent.height = std::min(extent.height, mip.height);
      sampleCount   = view->image()->info().sampleCount;
      layerCount    = std::max(layerCount, uint32_t(view->info().layerCount));
      anyAttachment = true;
    }

    m_state.gp.state.rt = DxvkRtInfo(colorCount, colorFormats.data(),
      depthFormat, readOnlyAspects);
    m_state.gp.state.ms.setSampleCount(sampleCount ? sampleCount : 1u);

    dstate.fbExtent          = anyAttachment ? extent : VkExtent2D { 0u, 0u };
    dstate.fbHasAttachments  = anyAttachment;
    dstate.fbHasDepth        = rt.depth.view != nullptr;
    dstate.fbReadOnlyAspects = readOnlyAspects;
    dstate.fbLayerCount      = layerCount;

    m_flags.clr(DxvkContextFlag::GpDirtyRenderTargets);
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }


  // ----------------------------------------------------------------------
  // render pass begin: deferred clears matching bound attachments become
  // load actions; everything else is flushed standalone first
  // ----------------------------------------------------------------------

  void DxvkContext::startRenderPass() {
    D9MT_ZONE(d9mt::ZoneStartRenderPass);
    auto& dstate = d9mt::ctxDrawStateImpl(this);
    const auto& rt = m_state.om.renderTargets;

    // partition pending clears
    auto clears = std::move(m_deferredClears);
    m_deferredClears = std::vector<DxvkDeferredClear>();

    auto subresEq = [] (const VkImageSubresourceRange& a, const VkImageSubresourceRange& b) {
      return a.baseMipLevel == b.baseMipLevel && a.levelCount == b.levelCount
          && a.baseArrayLayer == b.baseArrayLayer && a.layerCount == b.layerCount;
    };

    auto takeClear = [&] (const Rc<DxvkImageView>& view) -> const DxvkDeferredClear* {
      for (auto& entry : clears) {
        if (entry.imageView == nullptr)
          continue;
        if (entry.imageView->matchesView(view)
         || (entry.imageView->image() == view->image()
          && subresEq(entry.imageView->imageSubresources(), view->imageSubresources()))) {
          return &entry;
        }
      }
      return nullptr;
    };

    std::array<const DxvkDeferredClear*, MaxNumRenderTargets> colorClears = { };
    const DxvkDeferredClear* depthClear = nullptr;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (rt.color[i].view != nullptr)
        colorClears[i] = takeClear(rt.color[i].view);
    }
    if (rt.depth.view != nullptr)
      depthClear = takeClear(rt.depth.view);

    // standalone-flush the rest BEFORE opening the new pass
    for (const auto& entry : clears) {
      bool hoisted = depthClear == &entry;
      for (uint32_t i = 0; i < MaxNumRenderTargets && !hoisted; i++)
        hoisted = colorClears[i] == &entry;

      if (!hoisted) {
        this->performClear(entry.imageView, -1,
          entry.discardAspects, entry.clearAspects, entry.clearValue);
      }
    }

    // build the pass descriptor
    WMTRenderPassInfo pass = { };
    pass.render_target_width  = dstate.fbExtent.width;
    pass.render_target_height = dstate.fbExtent.height;

    if (dstate.fbLayerCount > 1u)
      pass.render_target_array_length = uint8_t(dstate.fbLayerCount);

    // occlusion queries inside begin/end need a visibility-result buffer on
    // the pass descriptor (it can only be attached at pass creation)
    if (dstate.activeOcclusionCount) {
      auto& cstate = d9mt::cmdListState(m_cmd.ptr());
      if (d9mt::acquireVisBuffer(cstate)) {
        pass.visibility_buffer = cstate.visBuffer;
      } else {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
          Logger::err("d9mt: startRenderPass: no visibility-result buffer; occlusion queries will not count");
      }
    }

    bool valid = true;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const auto& view = rt.color[i].view;
      if (view == nullptr)
        continue;

      obj_handle_t handle = obj_handle_t(view->handle());
      if (!handle) {
        Logger::err("d9mt: startRenderPass: color attachment has no Metal view");
        valid = false;
        continue;
      }

      auto& att = pass.colors[i];
      att.texture = handle;
      att.load_action  = WMTLoadActionLoad;
      att.store_action = WMTStoreActionStore;

      if (const auto* clear = colorClears[i]) {
        if (clear->clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
          att.load_action = WMTLoadActionClear;

          auto formatInfo = view->formatInfo();
          if (formatInfo->flags.test(DxvkFormatFlag::SampledUInt)) {
            att.clear_color = { double(clear->clearValue.color.uint32[0]),
                                double(clear->clearValue.color.uint32[1]),
                                double(clear->clearValue.color.uint32[2]),
                                double(clear->clearValue.color.uint32[3]) };
          } else if (formatInfo->flags.test(DxvkFormatFlag::SampledSInt)) {
            att.clear_color = { double(clear->clearValue.color.int32[0]),
                                double(clear->clearValue.color.int32[1]),
                                double(clear->clearValue.color.int32[2]),
                                double(clear->clearValue.color.int32[3]) };
          } else {
            att.clear_color = { double(clear->clearValue.color.float32[0]),
                                double(clear->clearValue.color.float32[1]),
                                double(clear->clearValue.color.float32[2]),
                                double(clear->clearValue.color.float32[3]) };
          }
        } else if (clear->discardAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
          att.load_action = WMTLoadActionDontCare;
        }
      }

      m_cmd->track(view->image(), DxvkAccess::Write);
    }

    if (rt.depth.view != nullptr) {
      const auto& view = rt.depth.view;
      obj_handle_t handle = obj_handle_t(view->handle());

      if (!handle) {
        Logger::err("d9mt: startRenderPass: depth attachment has no Metal view");
        valid = false;
      } else {
        // unified Depth32Float_Stencil8: always bind BOTH planes
        pass.depth.texture = handle;
        pass.depth.load_action  = WMTLoadActionLoad;
        pass.depth.store_action = WMTStoreActionStore;

        pass.stencil.texture = handle;
        pass.stencil.load_action  = WMTLoadActionLoad;
        pass.stencil.store_action = WMTStoreActionStore;

        if (depthClear) {
          if (depthClear->clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
            pass.depth.load_action = WMTLoadActionClear;
            pass.depth.clear_depth = depthClear->clearValue.depthStencil.depth;
          } else if (depthClear->discardAspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
            pass.depth.load_action = WMTLoadActionDontCare;
          }

          if (depthClear->clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
            pass.stencil.load_action = WMTLoadActionClear;
            pass.stencil.clear_stencil = uint8_t(depthClear->clearValue.depthStencil.stencil);
          } else if (depthClear->discardAspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
            pass.stencil.load_action = WMTLoadActionDontCare;
          }
        }

        m_cmd->track(view->image(), DxvkAccess::Write);
      }
    }

    if (!valid)
      return; // GpRenderPassBound stays clear; the draw is dropped loudly

    obj_handle_t encoder = d9mt::cmdListBeginRenderPass(m_cmd.ptr(), pass);
    if (!encoder)
      return;

    auto& cstate = d9mt::cmdListState(m_cmd.ptr());
    cstate.lastRenderPso  = 0;
    cstate.lastRenderDsso = 0;
    cstate.renderResident.reset();

    // restart active occlusion queries into a fresh visibility slot of the
    // new encoder (queries span pass splits by accumulating GPU queries)
    cstate.visAttached = pass.visibility_buffer != 0;
    m_queryManager.beginQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);

    m_flags.set(DxvkContextFlag::GpRenderPassBound);

    // encoder state is fresh: re-emit everything encoder-scoped
    m_flags.set(
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyRasterizerState,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthClip,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyDepthTest,
      DxvkContextFlag::GpDirtyStencilTest,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::DirtyPushData);

    m_descriptorState.dirtyStages(VK_SHADER_STAGE_ALL_GRAPHICS);

    m_renderPassIndex += 1u;
    m_cmd->addStatCtr(DxvkStatCounter::CmdRenderPassCount, 1u);
  }


  // ----------------------------------------------------------------------
  // spec constants: copy the masked dwords into the PSO-key sc block
  // ----------------------------------------------------------------------

  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updateSpecConstants() {
    auto& constants = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.constants : m_state.cp.constants;

    auto& sc = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.sc : m_state.cp.state.sc;

    for (uint32_t i = 0; i < MaxNumSpecConstants; i++) {
      sc.specConstants[i] = (constants.mask & (1u << i))
        ? constants.data[i] : 0u;
    }
  }


  // ----------------------------------------------------------------------
  // PSO lookup + bind
  // ----------------------------------------------------------------------

  bool DxvkContext::updateGraphicsPipelineState() {
    auto& dstate = d9mt::ctxDrawStateImpl(this);

    const auto& vs = m_state.gp.shaders.vs;
    const auto& fs = m_state.gp.shaders.fs;

    if (vs == nullptr || fs == nullptr) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: draw without complete VS/FS pair — skipped");
      return false;
    }

    this->updateSpecConstants<VK_PIPELINE_BIND_POINT_GRAPHICS>();

    d9mt::PsoKey key;
    key.vs = vs.ptr();
    key.fs = fs.ptr();
    key.state = m_state.gp.state;

    // the front-end uses dynamic strides (ilBindings carry stride 0);
    // Metal bakes strides into the vertex descriptor, so write them in
    for (uint32_t i = 0; i < key.state.il.bindingCount(); i++) {
      uint32_t binding = key.state.ilBindings[i].binding();
      key.state.ilBindings[i].setStride(m_state.vi.vertexStrides[binding]);
    }

    // Memo hit: same key as the previous draw → reuse the resolved entry and
    // skip the hash + mutex + map probe. entry->pso is atomic, so a still-
    // compiling entry memoed here is re-checked below and picked up once ready.
    const d9mt::PsoEntry* entry;
    if (dstate.lastPsoEntry && key == dstate.lastPsoKey) {
      entry = dstate.lastPsoEntry;
    } else {
      entry = d9mt::getRenderPso(key, vs, fs);
      if (entry) {
        dstate.lastPsoKey   = key;
        dstate.lastPsoEntry = entry;
      }
    }

    if (!entry || !entry->pso)
      return false; // creation failure already logged once

    if (dstate.pso != entry) {
      // The set-0 argument-buffer LAYOUT comes from shader reflection, so it
      // depends only on the vs/fs pair — NOT on blend/raster/RT-format state.
      // GTA IV swaps PSOs for state-only changes constantly while keeping the
      // same shaders and the same bound resources; forcing a full resource
      // rebuild (the expensive AB assembly + residency + track loop) on those
      // is wasted work. Only re-dirty resources when the shader pair actually
      // changed. Real resource changes still rebuild: per-resource binds dirty
      // descriptors themselves, DISCARD renames dirty via invalidateBuffer, and
      // pass restarts re-dirty independently in startRenderPass. Push data
      // (constants + sampler-heap indices) still refreshes on every PSO swap.
      const d9mt::PsoEntry* prev = dstate.pso;
      dstate.pso = entry;
      if (!prev || prev->vs != entry->vs || prev->fs != entry->fs)
        m_descriptorState.dirtyStages(VK_SHADER_STAGE_ALL_GRAPHICS);
      m_flags.set(DxvkContextFlag::DirtyPushData);
    }

    m_flags.clr(
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtySpecConstants);
    return true;
  }


  // ----------------------------------------------------------------------
  // vertex / index buffer bindings
  // ----------------------------------------------------------------------

  void DxvkContext::updateVertexBufferBindings() {
    D9MT_ZONE(d9mt::ZoneVtxBind);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    for (uint32_t i = 0; i < m_state.gp.state.il.bindingCount(); i++) {
      uint32_t binding = m_state.gp.state.ilBindings[i].binding();
      const auto& slice = m_state.vi.vertexBuffers[binding];

      DxvkResourceBufferInfo info = { };

      if (slice.defined()) {
        info = slice.getSliceInfo();
        m_cmd->track(slice.buffer(), DxvkAccess::Read);
      } else {
        // null stream / unbound binding: nullDescriptor semantics (reads
        // return zero) via the context zero buffer + Constant step layout
        info = this->createZeroBuffer(4096u)->getSliceInfo();
      }

      wmtcmd_render_setbuffer cmd = { };
      cmd.type = WMTRenderCommandSetVertexBuffer;
      cmd.buffer = obj_handle_t(info.buffer);
      cmd.offset = info.offset;
      cmd.index = uint8_t(d9mt::VertexBufferBase + binding);
      d9mt::encodeRenderCmd(cstate, &cmd);
    }

    m_flags.clr(DxvkContextFlag::GpDirtyVertexBuffers);
  }


  void DxvkContext::updateIndexBufferBinding() {
    // index buffers are draw arguments on Metal (no encoder state); only
    // lifetime tracking happens here
    if (m_state.vi.indexBuffer.defined())
      m_cmd->track(m_state.vi.indexBuffer.buffer(), DxvkAccess::Read);

    m_flags.clr(DxvkContextFlag::GpDirtyIndexBuffer);
  }


  // ----------------------------------------------------------------------
  // dynamic state: viewport/scissor, rasterizer + depth bias, blend
  // constants, depth-stencil state object + stencil reference
  // ----------------------------------------------------------------------

  void DxvkContext::updateDynamicState() {
    D9MT_ZONE(d9mt::ZoneDynState);
    auto& dstate = d9mt::ctxDrawStateImpl(this);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    if (m_flags.test(DxvkContextFlag::GpDirtyViewport)) {
      const VkViewport& vp = m_state.vp.viewports[0];

      // undo the front-end's Vulkan y-flip (negative height): Metal clip
      // space matches D3D, so the original D3D viewport is what we want
      double x = vp.x, y = vp.y, w = vp.width, h = vp.height;
      if (h < 0.0) {
        y = y + h;
        h = -h;
      }

      wmtcmd_render_setviewport setVp = { };
      setVp.type = WMTRenderCommandSetViewport;
      setVp.viewport = { x, y, w, h, double(vp.minDepth), double(vp.maxDepth) };
      d9mt::encodeRenderCmd(cstate, &setVp);

      // Metal requires the scissor to stay inside the render target
      const VkRect2D& sr = m_state.vp.scissorRects[0];

      uint64_t x0 = uint64_t(std::max(sr.offset.x, 0));
      uint64_t y0 = uint64_t(std::max(sr.offset.y, 0));
      x0 = std::min(x0, uint64_t(dstate.fbExtent.width));
      y0 = std::min(y0, uint64_t(dstate.fbExtent.height));

      uint64_t sw = std::min(uint64_t(sr.extent.width),
        uint64_t(dstate.fbExtent.width) - x0);
      uint64_t sh = std::min(uint64_t(sr.extent.height),
        uint64_t(dstate.fbExtent.height) - y0);

      wmtcmd_render_setscissorrect setSc = { };
      setSc.type = WMTRenderCommandSetScissorRect;
      setSc.scissor_rect = { x0, y0, sw, sh };
      d9mt::encodeRenderCmd(cstate, &setSc);

      m_flags.clr(DxvkContextFlag::GpDirtyViewport);
    }

    if (m_flags.any(
          DxvkContextFlag::GpDirtyRasterizerState,
          DxvkContextFlag::GpDirtyDepthBias,
          DxvkContextFlag::GpDirtyDepthClip)) {
      wmtcmd_render_setrasterizerstate cmd = { };
      cmd.type = WMTRenderCommandSetRasterizerState;

      VkPolygonMode polygonMode = m_state.gp.state.rs.polygonMode();
      cmd.fill_mode = polygonMode == VK_POLYGON_MODE_FILL
        ? WMTTriangleFillModeFill
        : WMTTriangleFillModeLines;

      if (polygonMode == VK_POLYGON_MODE_POINT) {
        static bool s_warned = false;
        if (!std::exchange(s_warned, true))
          Logger::err("d9mt: D3DFILL_POINT approximated as wireframe");
      }

      // 1:1: VK_CULL_MODE_NONE/FRONT_BIT/BACK_BIT == WMTCullMode 0/1/2;
      // winding is screen-space in both APIs
      cmd.cull_mode = WMTCullMode(uint32_t(m_state.dyn.cullMode) & 3u);
      cmd.winding = m_state.dyn.frontFace == VK_FRONT_FACE_CLOCKWISE
        ? WMTWindingClockwise
        : WMTWindingCounterClockwise;
      cmd.depth_clip_mode = m_state.gp.state.rs.depthClipEnable()
        ? WMTDepthClipModeClip
        : WMTDepthClipModeClamp;

      cmd.depth_bias       = m_state.dyn.depthBias.depthBiasConstant;
      cmd.scole_scale      = m_state.dyn.depthBias.depthBiasSlope;
      cmd.depth_bias_clamp = m_state.dyn.depthBias.depthBiasClamp;

      d9mt::encodeRenderCmd(cstate, &cmd);

      m_flags.clr(
        DxvkContextFlag::GpDirtyRasterizerState,
        DxvkContextFlag::GpDirtyDepthBias,
        DxvkContextFlag::GpDirtyDepthClip);
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyBlendConstants)) {
      wmtcmd_render_setblendcolor cmd = { };
      cmd.type = WMTRenderCommandSetBlendFactorAndStencilRef;
      cmd.red   = m_state.dyn.blendConstants.r;
      cmd.green = m_state.dyn.blendConstants.g;
      cmd.blue  = m_state.dyn.blendConstants.b;
      cmd.alpha = m_state.dyn.blendConstants.a;
      cmd.stencil_ref = uint8_t(m_state.dyn.stencilReference);
      d9mt::encodeRenderCmd(cstate, &cmd);

      m_flags.clr(DxvkContextFlag::GpDirtyBlendConstants);
    }

    if (m_flags.any(
          DxvkContextFlag::GpDirtyDepthTest,
          DxvkContextFlag::GpDirtyStencilTest,
          DxvkContextFlag::GpDirtyStencilRef)) {
      obj_handle_t dsso = d9mt::getDsso(m_state.dyn.depthStencilState,
        dstate.fbHasDepth, dstate.fbReadOnlyAspects);

      if (dsso) {
        wmtcmd_render_setdsso cmd = { };
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = dsso;
        cmd.stencil_ref = uint8_t(m_state.dyn.stencilReference);
        d9mt::encodeRenderCmd(cstate, &cmd);
        cstate.lastRenderDsso = dsso;
      }

      m_flags.clr(
        DxvkContextFlag::GpDirtyDepthTest,
        DxvkContextFlag::GpDirtyStencilTest,
        DxvkContextFlag::GpDirtyStencilRef);
    }
  }


  // ----------------------------------------------------------------------
  // resource flush: per-stage argument buffer (set-0 u64 words), push data
  // block (incl. sampler heap-index dwords), sampler heap binding
  // ----------------------------------------------------------------------

  bool DxvkContext::updateGraphicsShaderResources() {
    D9MT_ZONE(d9mt::ZoneBindRes);
    D9MT_HUD_BINDRES_TIMER();
    auto& dstate = d9mt::ctxDrawStateImpl(this);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    const d9mt::PsoEntry* pso = dstate.pso;
    if (!pso)
      return false;

    if (!dstate.ring) {
      dstate.ring = std::make_unique<DxvkStagingBuffer>(
        m_device, VkDeviceSize(4u) << 20u);
    }

    // Split the rebuild by what actually changed. The argument buffer (textures
    // + uniform-buffer addresses) and the static sampler-heap binding only need
    // rebuilding when descriptors are dirty; the push block (shader constants)
    // when push data is dirty. The common per-object draw only changes constants
    // (new transform), so this skips the expensive AB assembly + residency +
    // resource-tracking loop on those draws. Encoder bindings persist within a
    // pass, and every pass restart re-dirties all of this (startRenderPass), so
    // skipping an unchanged rebind is safe. The push block embeds sampler-heap
    // indices, so it also rebuilds when descriptors change.
    const bool resDirty  = m_descriptorState.hasDirtyResources(VK_SHADER_STAGE_ALL_GRAPHICS);
    const bool pushDirty = m_flags.test(DxvkContextFlag::DirtyPushData);

    for (uint32_t stage = 0; stage < 2u; stage++) {
      const d9mt::CompiledShader* shader = stage ? pso->fs : pso->vs;
      WMTRenderCommandType setBufferType = stage
        ? WMTRenderCommandSetFragmentBuffer
        : WMTRenderCommandSetVertexBuffer;

      // ---- set-0 argument buffer
      if (resDirty && shader->abEntryCount) {
        DxvkBufferSlice slice = dstate.ring->alloc(
          VkDeviceSize(shader->abEntryCount) * 8u);

        uint64_t* ab = reinterpret_cast<uint64_t*>(slice.mapPtr(0));
        if (!ab) {
          Logger::err("d9mt: argument buffer allocation failed");
          return false;
        }
        std::memset(ab, 0, size_t(shader->abEntryCount) * 8u);

        for (const auto& ref : shader->resources) {
          if (ref.abId >= shader->abEntryCount)
            continue; // defensive: never write outside the allocation

          switch (ref.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
              if (!ref.isUniformBuffer) {
                static bool s_warned = false;
                if (!std::exchange(s_warned, true))
                  Logger::err("d9mt: storage-buffer-view binding not implemented (SWVP)");
                break;
              }

              const auto& buffer = m_uniformBuffers[ref.slot];
              if (!buffer.defined())
                break; // nullDescriptor: word stays 0

              auto info = buffer.getSliceInfo();
              ab[ref.abId] = info.gpuAddress;

              d9mt::markResident(cstate, obj_handle_t(info.buffer));
              m_cmd->track(buffer.buffer(), DxvkAccess::Read);
            } break;

            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
              const auto& view = m_resources[ref.slot].imageView;
              if (view == nullptr)
                break; // nullDescriptor

              const DxvkDescriptor* descriptor = view->getDescriptor();
              if (!descriptor || !descriptor->legacy.image.imageView)
                break;

              uint64_t word;
              std::memcpy(&word, descriptor->descriptor.data(), sizeof(word));
              ab[ref.abId] = word;

              d9mt::markResident(cstate,
                obj_handle_t(descriptor->legacy.image.imageView));
              m_cmd->track(view->image(),
                ref.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                  ? DxvkAccess::Write : DxvkAccess::Read);
            } break;

            default: {
              static bool s_warned = false;
              if (!std::exchange(s_warned, true))
                Logger::err(str::format("d9mt: unsupported draw descriptor type ",
                  uint32_t(ref.type)));
            } break;
          }
        }

        auto sliceInfo = slice.getSliceInfo();

        wmtcmd_render_setbuffer cmd = { };
        cmd.type = setBufferType;
        cmd.buffer = obj_handle_t(sliceInfo.buffer);
        cmd.offset = sliceInfo.offset;
        cmd.index = uint8_t(shader->abBufferIndex);
        d9mt::encodeRenderCmd(cstate, &cmd);

        m_cmd->track(slice.buffer(), DxvkAccess::Read);
      }

      // ---- push data block (+ sampler heap indices at their blockOffsets)
      if ((pushDirty || resDirty) && shader->pushBufferIndex >= 0 && shader->pushDataSize) {
        DxvkBufferSlice slice = dstate.ring->alloc(shader->pushDataSize);

        uint8_t* data = reinterpret_cast<uint8_t*>(slice.mapPtr(0));
        if (!data) {
          Logger::err("d9mt: push data allocation failed");
          return false;
        }
        std::memset(data, 0, shader->pushDataSize);

        for (const auto& block : shader->pushBlocks) {
          for (uint32_t dw = 0; dw < block.size / 4u; dw++) {
            if (!(block.resourceMask & (uint64_t(1u) << dw))) {
              std::memcpy(data + block.dstOffset + 4u * dw,
                &m_state.pc.constantData[block.srcOffset + 4u * dw], 4u);
            }
          }
        }

        for (const auto& ref : shader->samplers) {
          uint16_t index = 0u;

          const auto& sampler = m_samplers[ref.slot];
          if (sampler != nullptr) {
            index = sampler->getDescriptor().samplerIndex;
            m_cmd->track(sampler);
          }

          if (uint32_t(ref.blockOffset) + 2u <= shader->pushDataSize)
            std::memcpy(data + ref.blockOffset, &index, sizeof(index));
        }

        auto sliceInfo = slice.getSliceInfo();

        wmtcmd_render_setbuffer cmd = { };
        cmd.type = setBufferType;
        cmd.buffer = obj_handle_t(sliceInfo.buffer);
        cmd.offset = sliceInfo.offset;
        cmd.index = uint8_t(shader->pushBufferIndex);
        d9mt::encodeRenderCmd(cstate, &cmd);

        m_cmd->track(slice.buffer(), DxvkAccess::Read);
      }

      // ---- set-15 sampler heap
      if (resDirty && shader->samplerHeapIndex >= 0) {
        obj_handle_t heap = d9mt::samplerHeapBuffer();

        if (heap) {
          wmtcmd_render_setbuffer cmd = { };
          cmd.type = setBufferType;
          cmd.buffer = heap;
          cmd.offset = 0u;
          cmd.index = uint8_t(shader->samplerHeapIndex);
          d9mt::encodeRenderCmd(cstate, &cmd);
        } else {
          static bool s_warned = false;
          if (!std::exchange(s_warned, true))
            Logger::err("d9mt: sampler heap buffer unavailable");
        }
      }
    }

    m_descriptorState.clearStages(VK_SHADER_STAGE_ALL_GRAPHICS);
    m_flags.clr(DxvkContextFlag::DirtyPushData);
    return true;
  }


  // ----------------------------------------------------------------------
  // commit: full pre-draw sequence
  // ----------------------------------------------------------------------

  template<bool Indexed, bool Indirect, bool Resolve>
  bool DxvkContext::commitGraphicsState() {
    auto& dstate = d9mt::ctxDrawStateImpl(this);

    if (m_state.gp.shaders.gs != nullptr) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: geometry stage (SWVP ProcessVertices) not supported — draw skipped");
      return false;
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyRenderTargets))
      this->updateRenderTargets();

    if (!dstate.fbHasAttachments) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: draw without any render target — skipped");
      return false;
    }

    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    // (re)start the render pass when none is active, or when pending clears
    // must turn into load actions to keep clear/draw ordering correct
    if (!m_flags.test(DxvkContextFlag::GpRenderPassBound)
     || cstate.kind != d9mt::EncoderKind::Render
     || !m_deferredClears.empty()) {
      if (cstate.kind == d9mt::EncoderKind::Render)
        d9mt::endEncoder(cstate);
      m_flags.clr(DxvkContextFlag::GpRenderPassBound);

      this->startRenderPass();

      if (!m_flags.test(DxvkContextFlag::GpRenderPassBound))
        return false;
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline)) {
      // new shader pair: update the spec-constant mask, force PSO lookup
      uint32_t mask = 0u;
      if (m_state.gp.shaders.vs != nullptr)
        mask |= m_state.gp.shaders.vs->getSpecConstantMask();
      if (m_state.gp.shaders.fs != nullptr)
        mask |= m_state.gp.shaders.fs->getSpecConstantMask();

      m_state.gp.constants.mask = mask & ((1u << MaxNumSpecConstants) - 1u);

      m_flags.clr(DxvkContextFlag::GpDirtyPipeline);
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }

    if (m_flags.any(
          DxvkContextFlag::GpDirtyPipelineState,
          DxvkContextFlag::GpDirtySpecConstants) || !dstate.pso) {
      if (!this->updateGraphicsPipelineState())
        return false;
    }

    if (cstate.lastRenderPso != dstate.pso->pso) {
      wmtcmd_render_setpso cmd = { };
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = dstate.pso->pso;
      d9mt::encodeRenderCmd(cstate, &cmd);
      cstate.lastRenderPso = dstate.pso->pso;
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyVertexBuffers))
      this->updateVertexBufferBindings();

    if (Indexed && m_flags.test(DxvkContextFlag::GpDirtyIndexBuffer))
      this->updateIndexBufferBinding();

    this->updateDynamicState();

    if (m_descriptorState.hasDirtyResources(VK_SHADER_STAGE_ALL_GRAPHICS)
     || m_flags.test(DxvkContextFlag::DirtyPushData)) {
      if (!this->updateGraphicsShaderResources())
        return false;
    }

    return true;
  }


  // ----------------------------------------------------------------------
  // draws
  // ----------------------------------------------------------------------

  void DxvkContext::draw(
          uint32_t          count,
    const VkDrawIndirectCommand* draws) {
    D9MT_ZONE(d9mt::ZoneDraw);
    if (!this->commitGraphicsState<false, false>())
      return;

    D9MT_ZONE(d9mt::ZoneDrawEmit);
    auto& dstate = d9mt::ctxDrawStateImpl(this);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    VkPrimitiveTopology topology = m_state.gp.state.ia.primitiveTopology();

    for (uint32_t i = 0; i < count; i++) {
      const auto& draw = draws[i];

      if (!draw.vertexCount || !draw.instanceCount)
        continue;

      if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
        // Metal has no fans: synthesize a triangle-list index buffer
        if (draw.vertexCount < 3u)
          continue;

        uint32_t triCount = draw.vertexCount - 2u;
        DxvkBufferSlice slice = dstate.ring->alloc(
          VkDeviceSize(triCount) * 3u * sizeof(uint32_t));

        uint32_t* indices = reinterpret_cast<uint32_t*>(slice.mapPtr(0));
        if (!indices)
          continue;

        for (uint32_t t = 0; t < triCount; t++) {
          indices[3u * t + 0u] = 0u;
          indices[3u * t + 1u] = t + 1u;
          indices[3u * t + 2u] = t + 2u;
        }

        auto info = slice.getSliceInfo();

        wmtcmd_render_draw_indexed cmd = { };
        cmd.type = WMTRenderCommandDrawIndexed;
        cmd.primitive_type = WMTPrimitiveTypeTriangle;
        cmd.index_type = WMTIndexTypeUInt32;
        cmd.index_count = uint64_t(triCount) * 3u;
        cmd.index_buffer = obj_handle_t(info.buffer);
        cmd.index_buffer_offset = info.offset;
        cmd.instance_count = draw.instanceCount;
        cmd.base_vertex = int32_t(draw.firstVertex);
        cmd.base_instance = draw.firstInstance;
        d9mt::encodeRenderCmd(cstate, &cmd);

        m_cmd->track(slice.buffer(), DxvkAccess::Read);
        continue;
      }

      WMTPrimitiveType primType;
      if (!d9mt::vkTopologyToMtl(topology, &primType)) {
        static bool s_warned = false;
        if (!std::exchange(s_warned, true))
          Logger::err(str::format("d9mt: unsupported primitive topology ",
            uint32_t(topology)));
        return;
      }

      wmtcmd_render_draw cmd = { };
      cmd.type = WMTRenderCommandDraw;
      cmd.primitive_type = primType;
      cmd.vertex_start = draw.firstVertex;
      cmd.vertex_count = draw.vertexCount;
      cmd.instance_count = draw.instanceCount;
      cmd.base_instance = draw.firstInstance;
      d9mt::encodeRenderCmd(cstate, &cmd);
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, count);
#ifdef D9MT_HUD
    ::d9mt::hud::g_draws.fetch_add(count, std::memory_order_relaxed);
#endif
  }


  void DxvkContext::drawIndexed(
          uint32_t          count,
    const VkDrawIndexedIndirectCommand* draws) {
    D9MT_ZONE(d9mt::ZoneDrawIndexed);
    if (!this->commitGraphicsState<true, false>())
      return;

    D9MT_ZONE(d9mt::ZoneDrawEmit);
    if (!m_state.vi.indexBuffer.defined()) {
      static bool s_warned = false;
      if (!std::exchange(s_warned, true))
        Logger::err("d9mt: indexed draw without index buffer — skipped");
      return;
    }

    auto& dstate = d9mt::ctxDrawStateImpl(this);
    auto& cstate = d9mt::cmdListState(m_cmd.ptr());

    VkPrimitiveTopology topology = m_state.gp.state.ia.primitiveTopology();

    auto ibInfo = m_state.vi.indexBuffer.getSliceInfo();
    bool index32 = m_state.vi.indexType == VK_INDEX_TYPE_UINT32;
    uint32_t indexSize = index32 ? 4u : 2u;

    for (uint32_t i = 0; i < count; i++) {
      const auto& draw = draws[i];

      if (!draw.indexCount || !draw.instanceCount)
        continue;

      if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
        // fetch the app's indices through the persistent mapping and emit
        // a triangulated list (all buffers are host-visible on this backend)
        if (draw.indexCount < 3u)
          continue;

        const void* srcIndices = m_state.vi.indexBuffer.mapPtr(
          VkDeviceSize(draw.firstIndex) * indexSize);

        if (!srcIndices) {
          static bool s_warned = false;
          if (!std::exchange(s_warned, true))
            Logger::err("d9mt: indexed fan: index buffer has no mapping — skipped");
          continue;
        }

        uint32_t triCount = draw.indexCount - 2u;
        DxvkBufferSlice slice = dstate.ring->alloc(
          VkDeviceSize(triCount) * 3u * sizeof(uint32_t));

        uint32_t* indices = reinterpret_cast<uint32_t*>(slice.mapPtr(0));
        if (!indices)
          continue;

        auto fetch = [&] (uint32_t n) -> uint32_t {
          return index32
            ? reinterpret_cast<const uint32_t*>(srcIndices)[n]
            : uint32_t(reinterpret_cast<const uint16_t*>(srcIndices)[n]);
        };

        for (uint32_t t = 0; t < triCount; t++) {
          indices[3u * t + 0u] = fetch(0u);
          indices[3u * t + 1u] = fetch(t + 1u);
          indices[3u * t + 2u] = fetch(t + 2u);
        }

        auto info = slice.getSliceInfo();

        wmtcmd_render_draw_indexed cmd = { };
        cmd.type = WMTRenderCommandDrawIndexed;
        cmd.primitive_type = WMTPrimitiveTypeTriangle;
        cmd.index_type = WMTIndexTypeUInt32;
        cmd.index_count = uint64_t(triCount) * 3u;
        cmd.index_buffer = obj_handle_t(info.buffer);
        cmd.index_buffer_offset = info.offset;
        cmd.instance_count = draw.instanceCount;
        cmd.base_vertex = draw.vertexOffset;
        cmd.base_instance = draw.firstInstance;
        d9mt::encodeRenderCmd(cstate, &cmd);

        m_cmd->track(slice.buffer(), DxvkAccess::Read);
        continue;
      }

      WMTPrimitiveType primType;
      if (!d9mt::vkTopologyToMtl(topology, &primType)) {
        static bool s_warned = false;
        if (!std::exchange(s_warned, true))
          Logger::err(str::format("d9mt: unsupported primitive topology ",
            uint32_t(topology)));
        return;
      }

      wmtcmd_render_draw_indexed cmd = { };
      cmd.type = WMTRenderCommandDrawIndexed;
      cmd.primitive_type = primType;
      cmd.index_type = index32 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
      cmd.index_count = draw.indexCount;
      cmd.index_buffer = obj_handle_t(ibInfo.buffer);
      cmd.index_buffer_offset = ibInfo.offset
        + VkDeviceSize(draw.firstIndex) * indexSize;
      cmd.instance_count = draw.instanceCount;
      cmd.base_vertex = draw.vertexOffset;
      cmd.base_instance = draw.firstInstance;
      d9mt::encodeRenderCmd(cstate, &cmd);
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, count);
#ifdef D9MT_HUD
    ::d9mt::hud::g_draws.fetch_add(count, std::memory_order_relaxed);
#endif
  }

}
