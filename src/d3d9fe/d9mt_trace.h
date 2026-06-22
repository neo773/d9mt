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
    ZoneAbRebuild,       // COUNT-only: AB built, words differ from last (real rebuild)
    ZoneAbIdentical,     // COUNT-only: AB built, words identical to last (skippable)
    ZonePushOnly,        // COUNT-only: bindRes call with no AB work (resDirty=false)
    ZoneShMiss,          // COUNT-only: AB rebuild reason = shader/PSO changed
    ZoneVSpurious,       // COUNT-only: views dirty but bound view handles UNCHANGED
    ZoneVReal,           // COUNT-only: views dirty AND bound view handles changed
    ZoneBaseMv,          // COUNT-only: views clean but base words moved (ring wrap etc)
    ZoneCount
  };

  inline const char* zoneName(uint32_t z) {
    static const char* const names[ZoneCount] = {
      "draw", "drawIdx", "psoLook", "psoMake", "shdComp",
      "startRP", "bufSub", "bufDed", "flush", "present", "bindRes",
      "vtxBind", "dynState", "drawEmit", "abRbld", "abIdent", "pushOnly",
      "shMiss", "vSame", "vDiff", "baseMv",
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

  // Count-only bump (no timing) for classification counters that ride the same
  // per-frame trace dump. No-op unless D9MT_TRACE is on.
  inline void zoneBump(uint32_t z) {
    if (traceEnabled())
      zoneRecord(z, 0);
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

  // RAII scope: reads the clock only when tracing is on.
  struct ScopedZone {
    uint32_t z;
    uint64_t start;
    bool     on;
#ifdef TRACY_ENABLE
    TracyCZoneCtx tracy;
#endif
    explicit ScopedZone(uint32_t zone) : z(zone), start(0), on(traceEnabled()) {
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

    std::fputc('\n', f);
    std::fflush(f);
#endif // D9MT_NO_TRACE
  }

}
