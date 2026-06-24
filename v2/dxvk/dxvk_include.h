#pragma once

// Shim base include (replaces DXVK's Vulkan-loader-pulling dxvk_include.h).
// Keeps DXVK's util layer and the real Vulkan TYPES/ENUMS (header-only), but
// provides empty vk:: function tables instead of the Vulkan loader — the shim
// never calls Vulkan. See SHIM_SPEC.md.

#include "../util/log/log.h"
#include "../util/log/log_debug.h"

#include "../util/util_env.h"
#include "../util/util_error.h"
#include "../util/util_flags.h"
#include "../util/util_likely.h"
#include "../util/util_math.h"
#include "../util/util_small_vector.h"
#include "../util/util_string.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

#include "../util/sha1/sha1_util.h"

#include "../util/sync/sync_signal.h"
#include "../util/sync/sync_spinlock.h"
#include "../util/sync/sync_ticketlock.h"

// Real Vulkan types/enums only — no loader, no runtime. The (shimmed)
// vulkan_loader.h pulls <vulkan/vulkan.h> and defines the empty vk::InstanceFn /
// vk::DeviceFn dispatch-table stand-ins. We include it (rather than redeclaring
// those structs here) so they have a single definition across every TU — the
// frontend's d3d9_interfaces.h includes the same header.
#include "../vulkan/vulkan_loader.h"

// Pure integer pipeline/HW limits (MaxNumRenderTargets, MaxNumSpecConstants,
// MaxSharedPushDataSize, …) referenced directly by the frontend.
#include "dxvk_limits.h"

namespace dxvk {
  // Minimal latency-stats stand-in (real one in dxvk-ref/dxvk_latency.h). Read
  // by the swapchain's HUD/latency path; the shim never produces real timings.
  // Defined here so both the latency tracker and HUD items share one definition.
  struct DxvkLatencyStats {
    uint64_t frameLatency = 0u;
    uint64_t sleepDuration = 0u;
  };
}
