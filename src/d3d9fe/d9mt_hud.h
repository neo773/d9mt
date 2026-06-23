// Custom in-driver HUD: draws our D3D9-internal per-frame counters (draw count,
// per-draw bindRes cost, buffer-pool hit rate) onto Apple's Metal Performance
// HUD via the private _CADeveloperHUDProperties API (exposed through winemetal's
// DeveloperHUDProperties_* calls). These are the metrics Apple's native HUD
// CANNOT know — it already shows fps / GPU time / frame interval / memory, so we
// only add the driver-internal signal that explains the CPU side.
//
// Entirely compiled out unless built with -DD9MT_HUD (see scripts/build-dxvkfe.sh
// D9MT_HUD=1). In a production RELEASE build every hook below is a no-op macro /
// empty inline, so there is ZERO overhead and zero metrics of our own.
#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>

namespace d9mt::hud {

#ifdef D9MT_HUD

// Per-frame counters, bumped from the hot paths, drained + reset in frame().
extern std::atomic<uint64_t> g_draws;        // draw calls this frame
extern std::atomic<uint64_t> g_bindResNs;    // summed bindRes wall ns this frame
extern std::atomic<uint64_t> g_bindResCalls; // bindRes invocations this frame
extern std::atomic<uint64_t> g_poolHits;     // buffer-pool hits (cumulative)
extern std::atomic<uint64_t> g_poolMisses;   // buffer-pool misses (cumulative)

// Lazy idempotent init (grabs the HUD singleton + registers our label slots).
// Safe to call every present; only the first does work.
void init();

// Once per displayed frame: snapshot counters, format lines, push to the HUD.
void frame();

// Scoped timer accumulating into the bindRes counters. One per
// updateGraphicsShaderResources call.
struct BindResTimer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~BindResTimer() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    g_bindResNs.fetch_add((uint64_t)ns, std::memory_order_relaxed);
    g_bindResCalls.fetch_add(1, std::memory_order_relaxed);
  }
};

#define D9MT_HUD_INIT()          ::d9mt::hud::init()
#define D9MT_HUD_FRAME()         ::d9mt::hud::frame()
#define D9MT_HUD_DRAW()          (::d9mt::hud::g_draws.fetch_add(1, std::memory_order_relaxed))
#define D9MT_HUD_BINDRES_TIMER() ::d9mt::hud::BindResTimer _d9mt_hud_bindres_timer

#else  // !D9MT_HUD — production: nothing.

#define D9MT_HUD_INIT()          ((void)0)
#define D9MT_HUD_FRAME()         ((void)0)
#define D9MT_HUD_DRAW()          ((void)0)
#define D9MT_HUD_BINDRES_TIMER() ((void)0)

#endif

} // namespace d9mt::hud

// Buffer-pool hit/miss accounting lives in d9mt_resources.cpp (where the pool
// is). Declared here so frame() can read it; defined + bumped only under D9MT_HUD.
namespace d9mt {
void hudPoolStats(uint64_t& hits, uint64_t& misses);
}
