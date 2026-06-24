#pragma once

#include <cstdint>

#include "dxvk_include.h"

// GPU query / event shim. D3D9 occlusion / timestamp / event queries (D3D9Query)
// are NOT on the FF-triangle path; the shim has no GPU timeline to query. We
// provide opaque DxvkQuery / DxvkEvent stand-ins plus the status enums + the
// DxvkQueryData union the frontend reads, all answering "done with zeroes" so
// D3D9Query's GetData loop completes immediately. See SHIM_SPEC.md.

namespace dxvk {

  enum class DxvkGpuQueryStatus : uint32_t {
    Invalid   = 0,
    Pending   = 1,
    Available = 2,
    Failed    = 3,
  };

  enum class DxvkGpuEventStatus : uint32_t {
    Invalid   = 0,
    Pending   = 1,
    Signaled  = 2,
  };

  struct DxvkQueryOcclusionData { uint64_t samplesPassed; };
  struct DxvkQueryTimestampData { uint64_t time; };
  struct DxvkQueryStatisticData {
    uint64_t iaVertices, iaPrimitives, vsInvocations, gsInvocations, gsPrimitives,
             clipInvocations, clipPrimitives, fsInvocations, tcsPatches,
             tesInvocations, csInvocations;
  };
  struct DxvkQueryXfbStreamData { uint64_t primitivesWritten, primitivesNeeded; };

  // Union of all query result shapes; the frontend selects a member by type.
  struct DxvkQueryData {
    union {
      DxvkQueryOcclusionData occlusion;
      DxvkQueryTimestampData timestamp;
      DxvkQueryStatisticData statistic;
      DxvkQueryXfbStreamData xfbStream;
    };
    DxvkQueryData() : statistic{} { }
  };

  // Opaque GPU query, always Available so games never stall waiting on a result.
  //
  // Occlusion reports a large non-zero sample count ("conservatively visible")
  // rather than zero: a zero result reads as "fully occluded", so games that
  // gate drawing on occlusion queries (GTA IV's culling, lens-flare visibility)
  // would skip visible geometry. Reporting visible keeps rendering correct at
  // the cost of the culling optimization — the standard fallback when real GPU
  // occlusion counting is unavailable. (Metal does support visibility-result
  // counting; wiring it here needs per-query buffer slots plus a flush on
  // GetData to avoid the synchronous-present deadlock — a later refinement.)
  class DxvkQuery : public RcObject {

  public:

    DxvkGpuQueryStatus getData(DxvkQueryData& data) {
      data = DxvkQueryData();
      data.occlusion.samplesPassed = ~0ull;
      return DxvkGpuQueryStatus::Available;
    }

  };

  // Opaque GPU event — always reports Signaled (no in-flight GPU work).
  class DxvkEvent : public RcObject {

  public:

    DxvkGpuEventStatus test() { return DxvkGpuEventStatus::Signaled; }

  };

}
