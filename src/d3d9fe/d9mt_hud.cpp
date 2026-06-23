// Custom in-driver HUD — see d9mt_hud.h. Entire translation unit is inert unless
// built with -DD9MT_HUD; in a production RELEASE build it compiles to nothing.
#include "d9mt_hud.h"

#ifdef D9MT_HUD

#include "../winemetal.h"
#include "../d9mtmetal/d9mtmetal.h"  // D9MT_UnixCall + d9mt_hud_color_params
#include <cstdio>

namespace d9mt::hud {

std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_bindResNs{0};
std::atomic<uint64_t> g_bindResCalls{0};
std::atomic<uint64_t> g_poolHits{0};
std::atomic<uint64_t> g_poolMisses{0};

namespace {

obj_handle_t s_hud   = 0;     // _CADeveloperHUDProperties singleton
bool         s_ready = false; // label slots registered
obj_handle_t s_kDraws = 0, s_kBindRes = 0, s_kPool = 0; // retained label keys
obj_handle_t s_vDraws = 0, s_vBindRes = 0, s_vPool = 0; // retained current values

// Retained NSString (process-lifetime label key); never released.
obj_handle_t makeKey(const char* s) {
  return NSString_alloc_init(s, WMTUTF8StringEncoding);
}

// Force a label's text white. macOS 27's HUD defaults custom labels to black =>
// invisible on the dark overlay; winemetal has no color setter, so route through
// d9mtmetal's updateMetricColor unixcall.
void setWhite(obj_handle_t key) {
  struct d9mt_hud_color_params c;
  c.hud = (uint64_t)s_hud;
  c.key = (uint64_t)key;
  c.name_color  = 0xFFFFFFFFu;
  c.value_color = 0xFFFFFFFFu;
  D9MT_UnixCall(D9MT_FUNC_HUD_SET_COLOR, &c);
}

// Sets a label's value with a RETAINED string and releases the prior one. The
// HUD renders asynchronously (~1 Hz) long after this returns, so the value must
// outlive the call — an autoreleased string would be freed first => blank line.
void setLine(obj_handle_t key, obj_handle_t& slot, const char* text) {
  obj_handle_t v = NSString_alloc_init(text, WMTUTF8StringEncoding); // +1
  DeveloperHUDProperties_updateLabel(s_hud, key, v);
  setWhite(key);  // re-assert each update — updateLabel may reset the color
  if (slot)
    NSObject_release(slot);
  slot = v;
}

} // namespace

void init() {
  if (s_ready)
    return;
  if (!s_hud)
    s_hud = DeveloperHUDProperties_instance();
  if (!s_hud)
    return; // Metal HUD not enabled (no MTL_HUD_ENABLED) — retry next frame.

  s_kDraws   = makeKey("com.d9mt.hud-draws");
  s_kBindRes = makeKey("com.d9mt.hud-bindres");
  s_kPool    = makeKey("com.d9mt.hud-pool");

  // Chain the lines under the "Custom Metrics" section, each anchored to the
  // previous. NOTE: on macOS 27 the "com.apple.hud-graph.default" anchor no
  // longer resolves — pass a nil anchor (0) for the first line so it appends to
  // the default section; chaining the rest by key keeps them ordered.
  DeveloperHUDProperties_addLabel(s_hud, s_kDraws,   0);
  DeveloperHUDProperties_addLabel(s_hud, s_kBindRes, s_kDraws);
  DeveloperHUDProperties_addLabel(s_hud, s_kPool,    s_kBindRes);
  s_ready = true;

  // Seed initial text (setLine also forces white — invisible otherwise on 27).
  setLine(s_kDraws,   s_vDraws,   "draws --");
  setLine(s_kBindRes, s_vBindRes, "bindRes --");
  setLine(s_kPool,    s_vPool,    "bufpool --");
}

void frame() {
  if (!s_ready) {
    init();
    if (!s_ready)
      return;
  }

  uint64_t draws = g_draws.exchange(0, std::memory_order_relaxed);
  uint64_t brNs  = g_bindResNs.exchange(0, std::memory_order_relaxed);
  uint64_t brN   = g_bindResCalls.exchange(0, std::memory_order_relaxed);
  double   brUs  = brN ? (double)brNs / 1000.0 / (double)brN : 0.0;

  uint64_t hits = 0, misses = 0;
  d9mt::hudPoolStats(hits, misses);
  uint64_t ptot = hits + misses;
  double   pPct = ptot ? (double)hits * 100.0 / (double)ptot : 0.0;

  char buf[96];
  std::snprintf(buf, sizeof(buf), "draws %llu", (unsigned long long)draws);
  setLine(s_kDraws, s_vDraws, buf);
  std::snprintf(buf, sizeof(buf), "bindRes %.2f us/call", brUs);
  setLine(s_kBindRes, s_vBindRes, buf);
  std::snprintf(buf, sizeof(buf), "bufpool %.0f%% hit (%llu/%llu)",
                pPct, (unsigned long long)hits, (unsigned long long)ptot);
  setLine(s_kPool, s_vPool, buf);

  // Guaranteed live readout regardless of the on-screen HUD: one line, rewritten
  // each frame. `tail -F d9mt-metrics.log` in a terminal beside the game.
  static FILE* mf = std::fopen("d9mt-metrics.log", "w");
  if (mf) {
    std::rewind(mf);
    std::fprintf(mf, "draws %llu | bindRes %.2f us/call | bufpool %.0f%% (%llu/%llu)    \n",
                 (unsigned long long)draws, brUs, pPct,
                 (unsigned long long)hits, (unsigned long long)ptot);
    std::fflush(mf);
  }
}

} // namespace d9mt::hud

namespace d9mt {
void hudPoolStats(uint64_t& hits, uint64_t& misses) {
  hits   = hud::g_poolHits.load(std::memory_order_relaxed);
  misses = hud::g_poolMisses.load(std::memory_order_relaxed);
}
} // namespace d9mt

#endif // D9MT_HUD
