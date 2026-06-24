#pragma once

#include <cstdint>

#include "../util/util_flags.h"

#include "dxvk_include.h"

// Resource-access flags + paged-resource base (shim). The real DXVK uses these
// to track GPU in-flight state; the synchronous Metal shim never has work in
// flight, so DxvkPagedResource::isInUse() always returns false and the frontend
// never blocks. Copied enum verbatim from dxvk-ref/dxvk_access.h. See SHIM_SPEC.

namespace dxvk {

  enum class DxvkAccess : uint32_t {
    None  = 0,
    Read  = 1,
    Write = 2,
    Move  = 3,
  };

  using DxvkAccessFlags = Flags<DxvkAccess>;

  // Base for GPU-trackable resources (DxvkImage/DxvkBuffer). The shim submits
  // synchronously, so nothing is ever "in use" by the time the frontend asks.
  class DxvkPagedResource : public RcObject {

  public:

    bool isInUse(DxvkAccess) const { return false; }
    bool isInUse(DxvkAccessFlags) const { return false; }

  };

}
