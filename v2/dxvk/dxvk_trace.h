#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Per-frame CPU draw-path profiler. Each zone accumulates wall-clock time spent
// inside it during a frame; the totals print at Present when the D9MT_TRACE
// environment variable is set, and every probe is a cheap early-out otherwise.
// The shim replays draws on a single thread, so a plain global accumulator is
// enough — no atomics. This is the instrumentation the Vulkan-rip phases use to
// justify each cut with a measured before/after, not a guess.

namespace dxvk {

  enum class TraceZone : uint32_t {
    Draw,            // a whole DrawPrimitive/DrawIndexedPrimitive
    ResolveShader,   // translate + compile + specialize (cached after warmup)
    BuildPipeline,   // render-pipeline-state lookup/build (cached after warmup)
    BindResources,   // per-draw buffer/argument/sampler/residency assembly
    AppendDraw,      // encoding the draw into the Metal command encoder
    Present,         // end-encoding + present + commit + wait
    Count
  };

  class FrameTrace {

  public:

    // Read once from the environment; subsequent probes are a single load.
    static bool enabled() {
      static const bool on = [] {
        const char* value = std::getenv("D9MT_TRACE");
        return value && value[0] == '1';
      }();
      return on;
    }

    static void addNanos(TraceZone zone, uint64_t nanos) {
      s_nanos[uint32_t(zone)] += nanos;
    }

    static void countDraw() {
      s_drawCount++;
    }

    // Prints the accumulated per-frame breakdown and resets for the next frame.
    static void endFrame() {
      if (!enabled())
        return;

      static const char* zoneNames[] = {
        "draw", "resolveShader", "buildPipeline", "bindResources", "appendDraw", "present" };

      if (FILE* file = std::fopen("C:\\d9mt-test\\d9mt-trace.log", "a")) {
        std::fprintf(file, "frame draws=%llu", (unsigned long long) s_drawCount);
        for (uint32_t i = 0; i < uint32_t(TraceZone::Count); i++)
          std::fprintf(file, " %s=%.4fms", zoneNames[i], double(s_nanos[i]) / 1.0e6);
        std::fprintf(file, "\n");
        std::fclose(file);
      }

      s_nanos = { };
      s_drawCount = 0;
    }

  private:

    static inline std::array<uint64_t, uint32_t(TraceZone::Count)> s_nanos = { };
    static inline uint64_t s_drawCount = 0;

  };

  // RAII timer: measures its own lifetime and folds the elapsed time into a zone
  // when tracing is enabled. Construction caches the enabled flag so a disabled
  // build pays only a branch.
  class TraceScope {

  public:

    explicit TraceScope(TraceZone zone)
    : m_zone(zone), m_active(FrameTrace::enabled()) {
      if (m_active)
        m_start = std::chrono::steady_clock::now();
    }

    ~TraceScope() {
      if (m_active) {
        auto elapsed = std::chrono::steady_clock::now() - m_start;
        FrameTrace::addNanos(m_zone,
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
      }
    }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator = (const TraceScope&) = delete;

  private:

    TraceZone m_zone;
    bool      m_active;
    std::chrono::steady_clock::time_point m_start;

  };

}
