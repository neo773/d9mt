// Replaces vendor/dxvk/src/dxso/dxso_options.cpp, which is excluded from the
// build because its second constructor takes a live D3D9DeviceEx*. The
// default constructor below is copied verbatim from that file; all option
// values come from the member initializers in dxso_options.h.

#include "../vendor/dxvk/src/dxso/dxso_options.h"
#include "../vendor/dxvk/src/util/log/log.h"

namespace dxvk {

  DxsoOptions::DxsoOptions() {}

  // every dxvk frontend dll defines the logger instance (d3d9_main.cpp:12)
  Logger Logger::s_instance("dxso2spv.log");

}
