// d9mt: lightweight in-engine zone profiler.
//
// Purpose: see what the driver spends CPU on, per frame, in nanoseconds —
// without a full tracing framework. Each zone is a named scope; on every
// present we dump one line per frame with, for each zone: call count, total
// ms, and the SINGLE WORST call (max us). The max column is the spike finder
// — a frame at 33 ms with "psoMake x1 (mx 12000us)" tells you instantly that
// one synchronous pipeline compile ate the frame.
//
// Timing uses QueryPerformanceCounter (high-res, wine-backed). Accumulators
// are process-global atomics so it is correct across the app/CS/watcher
// threads. When D9MT_TRACE is unset the scope just reads one cached bool and
// skips the clock read — negligible overhead, safe to leave compiled in.
//
// Enable: D9MT_TRACE=1   Output: d3d9fe-trace.log in the process cwd.
// Limit which zones are timed (less observer overhead): D9MT_TRACE_ZONES=<mask>,
// a hex/dec bitmask where bit i selects D9MTZone i. Unset = all zones.

#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// TRACY=1 build: bridge the existing D9MT_ZONE scopes into Tracy so the same
// zones show up live in the Tracy GUI (in addition to the d3d9fe-trace.log
// dump). On-demand client -> near-zero cost until the GUI connects.
#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#endif

namespace dxvk::d9mt {

  enum D9MTZone : uint32_t {
    ZoneDraw = 0,        // DxvkContext::draw
    ZoneDrawIndexed,     // DxvkContext::drawIndexed
    ZonePsoLookup,       // getRenderPso cache hit path
    ZonePsoCreate,       // createRenderPso (synchronous Metal pipeline compile)
    ZoneShaderCompile,   // DXSO -> MSL compile (cold)
    ZoneStartRenderPass, // startRenderPass + encoder begin
    ZoneBufAllocSub,     // createBufferResource via suballocation arena
    ZoneBufAllocDed,     // createBufferResource dedicated MTLBuffer
    ZoneFlush,           // flushCommandList (commit)
    ZonePresent,         // presentImage
    ZoneBindRes,         // updateGraphicsShaderResources (per-draw AB/push/resident rebuild)
    ZoneVtxBind,         // updateVertexBufferBindings (per-stream setbuffer)
    ZoneDynState,        // updateDynamicState (viewport/scissor/raster)
    ZoneDrawEmit,        // post-commit draw emission (trifan idx-gen + draw cmd)
    ZonePsoState,        // updateGraphicsPipelineState whole (key build + lookup)
    ZoneIdxBind,         // updateIndexBufferBinding
    ZoneCommit,          // commitGraphicsState whole (glue = this minus substages)
    ZoneCount
  };

  inline const char* zoneName(uint32_t z) {
    static const char* const names[ZoneCount] = {
      "draw", "drawIdx", "psoLook", "psoMake", "shdComp",
      "startRP", "bufSub", "bufDed", "flush", "present", "bindRes",
      "vtxBind", "dynState", "drawEmit", "psoState", "idxBind", "commit",
    };
    return z < ZoneCount ? names[z] : "?";
  }

  inline bool traceEnabled() {
    static const bool e = [] {
      const char* v = std::getenv("D9MT_TRACE");
      return v && v[0] == '1' && v[1] == '\0';
    }();
    return e;
  }

  // Per-zone enable mask. Timing every zone reads QPC twice per scope on the
  // ~12k-zone/frame hot path, which is itself a measurable CPU/FPS hit and skews
  // the numbers. D9MT_TRACE_ZONES=<bitmask> (hex/dec, bit i = D9MTZone i) limits
  // timing to the zones of interest so unselected zones cost only a cached bool
  // + bit test (no clock read). Unset => all zones (backward compatible).
  // Example: only the per-draw substages bindRes|vtxBind|dynState|psoLook|
  // drawEmit + draw|drawIdx => D9MT_TRACE_ZONES=0x3C07.
  inline uint32_t traceZoneMask() {
    static const uint32_t m = [] {
      const char* v = std::getenv("D9MT_TRACE_ZONES");
      if (!v || !v[0])
        return ~0u;
      return uint32_t(std::strtoul(v, nullptr, 0));
    }();
    return m;
  }

  inline uint64_t qpcFreq() {
    static const uint64_t f = [] {
      LARGE_INTEGER li;
      QueryPerformanceFrequency(&li);
      return uint64_t(li.QuadPart ? li.QuadPart : 1);
    }();
    return f;
  }

  inline uint64_t qpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return uint64_t(li.QuadPart);
  }

  struct ZoneAccum {
    std::atomic<uint64_t> calls;
    std::atomic<uint64_t> ticks;
    std::atomic<uint64_t> maxTicks;
  };

  inline ZoneAccum* zoneTable() {
    static ZoneAccum t[ZoneCount] = {};
    return t;
  }

  inline void zoneRecord(uint32_t z, uint64_t dt) {
    ZoneAccum& a = zoneTable()[z];
    a.calls.fetch_add(1, std::memory_order_relaxed);
    a.ticks.fetch_add(dt, std::memory_order_relaxed);
    uint64_t cur = a.maxTicks.load(std::memory_order_relaxed);
    while (dt > cur && !a.maxTicks.compare_exchange_weak(
        cur, dt, std::memory_order_relaxed)) { }
  }

#ifdef TRACY_ENABLE
  // One static Tracy source-location per zone (built once) — avoids the
  // per-call malloc of ___tracy_alloc_srcloc on the ~12k-zone/frame hot path.
  inline const ___tracy_source_location_data* zoneSrcLoc(uint32_t z) {
    static ___tracy_source_location_data locs[ZoneCount];
    static const bool init = [] {
      for (uint32_t i = 0; i < ZoneCount; i++)
        locs[i] = ___tracy_source_location_data{ zoneName(i), "d9mt-zone", "d9mt", 0, 0 };
      return true;
    }();
    (void) init;
    return &locs[z < ZoneCount ? z : 0];
  }
#endif

  // --------------------------------------------------------------------------
  // Low-overhead micro-probe. The QPC ScopedZone calls QueryPerformanceCounter
  // (a wine API call) twice per scope, which dwarfs and inflates a region as
  // small as a mutex+map lookup. microNow() reads the CPU/virtual counter via
  // rdtsc directly — under Rosetta that's a cheap emulated register read, no
  // wine round-trip — so it can honestly time tiny hot blocks. Ticks are self-
  // calibrated to ms each frame against the QPC frame delta in traceFrameEnd.
  // Separate from the zone mask: gated only on D9MT_TRACE so you can disable all
  // QPC zones (D9MT_TRACE_ZONES=0) and pay only rdtsc on the probed block.
  // --------------------------------------------------------------------------
  inline uint64_t microNow() {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return qpcNow();
#endif
  }

  struct MicroAccum {
    std::atomic<uint64_t> ticks { 0 };
    std::atomic<uint64_t> count { 0 };
  };

  static constexpr uint32_t MicroCount = 20;

  inline MicroAccum* microTable() {
    static MicroAccum t[MicroCount] = {};
    return t;
  }

  inline const char* microName(uint32_t i) {
    static const char* const n[MicroCount] = {
      "look", "ctxLk", "rt", "cmdLk", "rpRst", "spec", "psoSt",
      "setPso", "vtx", "idx", "dyn", "bindRes",
      "abAlloc", "abLoop", "abEnc", "push", "smpLoop", "pushEnc", "m18", "m19",
    };
    return i < MicroCount ? n[i] : "?";
  }

  inline void microAdd(uint32_t i, uint64_t dt) {
    MicroAccum& a = microTable()[i];
    a.ticks.fetch_add(dt, std::memory_order_relaxed);
    a.count.fetch_add(1, std::memory_order_relaxed);
  }

  struct ScopedMicro {
    uint32_t i;
    uint64_t t0;
    bool     on;
    explicit ScopedMicro(uint32_t idx) : i(idx), t0(0), on(traceEnabled()) {
      if (on) t0 = microNow();
    }
    ~ScopedMicro() {
      if (on) {
        MicroAccum& a = microTable()[i];
        a.ticks.fetch_add(microNow() - t0, std::memory_order_relaxed);
        a.count.fetch_add(1, std::memory_order_relaxed);
      }
    }
    ScopedMicro(const ScopedMicro&) = delete;
    ScopedMicro& operator=(const ScopedMicro&) = delete;
  };

#ifdef D9MT_NO_TRACE
  #define D9MT_MICRO(i) ((void) 0)
  #define D9MT_MICRO_BEG(var) ((void) 0)
  #define D9MT_MICRO_END(i, var) ((void) 0)
#else
  #define D9MT_MICRO(i) ::dxvk::d9mt::ScopedMicro \
    D9MT_TRACE_CONCAT(d9mtMicro_, __LINE__)(i)
  // Manual begin/end for line-by-line timing of regions that declare
  // function-scope references (which can't live in a RAII sub-block).
  #define D9MT_MICRO_BEG(var) \
    uint64_t var = ::dxvk::d9mt::traceEnabled() ? ::dxvk::d9mt::microNow() : 0u
  #define D9MT_MICRO_END(i, var) \
    do { if (var) ::dxvk::d9mt::microAdd((i), ::dxvk::d9mt::microNow() - (var)); } while (0)
#endif

  // RAII scope: reads the clock only when tracing is on.
  struct ScopedZone {
    uint32_t z;
    uint64_t start;
    bool     on;
#ifdef TRACY_ENABLE
    TracyCZoneCtx tracy;
#endif
    explicit ScopedZone(uint32_t zone) : z(zone), start(0),
        on(traceEnabled() && (traceZoneMask() & (1u << zone))) {
      if (on) start = qpcNow();
#ifdef TRACY_ENABLE
      tracy = ___tracy_emit_zone_begin(zoneSrcLoc(zone), 1);
#endif
    }
    ~ScopedZone() {
      if (on) zoneRecord(z, qpcNow() - start);
#ifdef TRACY_ENABLE
      ___tracy_emit_zone_end(tracy);
#endif
    }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
  };

  // Release builds define D9MT_NO_TRACE: the zone scopes and the per-frame
  // dump compile to nothing — no bool check, no ScopedZone ctor/dtor on the
  // hot path (it wraps the ~12k draws/binds per frame), zero overhead.
#ifdef D9MT_NO_TRACE
  #define D9MT_ZONE(z) ((void) 0)
#else
  #define D9MT_TRACE_CONCAT_(a, b) a##b
  #define D9MT_TRACE_CONCAT(a, b)  D9MT_TRACE_CONCAT_(a, b)
  #define D9MT_ZONE(z) ::dxvk::d9mt::ScopedZone \
    D9MT_TRACE_CONCAT(d9mtZone_, __LINE__)(z)
#endif

  // Called once per present: stamps frame wall time, dumps a per-zone line,
  // resets the accumulators. One line per frame in d3d9fe-trace.log.
  inline void traceFrameEnd() {
#ifdef TRACY_ENABLE
    TracyCFrameMark;
#endif
#ifdef D9MT_NO_TRACE
    return;
#else
    if (!traceEnabled())
      return;

    static std::mutex m;
    static FILE*      f           = nullptr;
    static uint64_t   lastPresent = 0;
    static uint64_t   frameNo     = 0;

    std::lock_guard<std::mutex> lock(m);

    const uint64_t now  = qpcNow();
    const double   freq = double(qpcFreq());
    const double   frameMs = lastPresent ? (now - lastPresent) * 1000.0 / freq : 0.0;
    lastPresent = now;

    if (!f) {
      f = std::fopen("d3d9fe-trace.log", "w");
      if (!f)
        return;
    }

    std::fprintf(f, "f%llu %6.2fms |", (unsigned long long) frameNo++, frameMs);

    ZoneAccum* t = zoneTable();
    for (uint32_t i = 0; i < ZoneCount; i++) {
      const uint64_t c  = t[i].calls.exchange(0, std::memory_order_relaxed);
      const uint64_t tk = t[i].ticks.exchange(0, std::memory_order_relaxed);
      const uint64_t mx = t[i].maxTicks.exchange(0, std::memory_order_relaxed);
      if (!c)
        continue;
      const double totMs = tk * 1000.0 / freq;
      const double maxUs = mx * 1.0e6 / freq;
      std::fprintf(f, " %s x%llu %.2fms(mx%.0fus)",
        zoneName(i), (unsigned long long) c, totMs, maxUs);
    }

    // Micro-probes: self-calibrate rdtsc ticks -> ms against this frame's QPC
    // wall time (rdtsc delta over the same present-to-present interval).
    static uint64_t lastRdtsc = 0;
    const uint64_t  rdNow     = microNow();
    const double    rdPerMs   = (lastRdtsc && frameMs > 0.0)
      ? double(rdNow - lastRdtsc) / frameMs : 0.0;
    lastRdtsc = rdNow;

    MicroAccum* mt = microTable();
    for (uint32_t i = 0; i < MicroCount; i++) {
      const uint64_t c  = mt[i].count.exchange(0, std::memory_order_relaxed);
      const uint64_t tk = mt[i].ticks.exchange(0, std::memory_order_relaxed);
      if (!c)
        continue;
      const double ms = rdPerMs > 0.0 ? tk / rdPerMs : 0.0;
      std::fprintf(f, " %s x%llu %.2fms", microName(i), (unsigned long long) c, ms);
    }

    std::fputc('\n', f);
    std::fflush(f);
#endif // D9MT_NO_TRACE
  }

}
