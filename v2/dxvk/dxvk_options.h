#pragma once

// DxvkOptions shim — the device module's config() returns one of these.
//
// The real DXVK fills these from a Config file and uses them to drive Vulkan
// allocator / pipeline behaviour. The Metal shim has no Vulkan backend, so the
// fields are kept (so the frontend can read them) but all default to their
// "do nothing special" values. We deliberately mirror the real header's field
// set so any frontend access compiles and reads a sane default. See SHIM_SPEC.

#include "../util/config/config.h"

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace dxvk {

  // Mirrors dxvk-ref/dxvk_options.h field-for-field. The Config-taking ctor is
  // declared (the device may default-construct, but keeping it matches the real
  // type) and defined inline as a no-op so we don't need a .cpp just for this.
  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config) { (void)config; }

    bool         enableDebugUtils              = false;
    Tristate     enableMemoryDefrag            = Tristate::Auto;
    int32_t      numCompilerThreads            = 0;
    Tristate     enableGraphicsPipelineLibrary = Tristate::Auto;
    Tristate     enableDescriptorHeap          = Tristate::Auto;
    Tristate     enableDescriptorBuffer        = Tristate::Auto;
    bool         enableUnifiedImageLayout      = true;
    Tristate     trackPipelineLifetime         = Tristate::Auto;
    Tristate     useRawSsbo                     = Tristate::Auto;
    std::string  hud;
    Tristate     tearFree                       = Tristate::Auto;
    Tristate     latencySleep                   = Tristate::Auto;
    int32_t      latencyTolerance               = 0;
    Tristate     disableNvLowLatency2           = Tristate::Auto;
    bool         hideIntegratedGraphics         = false;
    bool         zeroMappedMemory               = false;
    bool         allowFse                       = false;
    Tristate     tilerMode                      = Tristate::Auto;
    VkDeviceSize maxMemoryBudget                = 0u;
    Tristate     lowerSinCos                    = Tristate::Auto;
    bool         enableImplicitResolves         = true;
    std::string  deviceFilter;
  };

}
