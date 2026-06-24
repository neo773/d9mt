// Shim stubs: definitions for symbols whose real homes pull in machinery the
// Metal shim deliberately drops (Vulkan runtime, d3d11/dxgi interfaces, dead
// frontend paths kept alive only by linkage). Mirrors v1's d9mt_dxvk_stubs.cpp
// approach: provide trivial bodies here rather than compiling more of DXVK.

#include <iomanip>
#include <ostream>

#include "../util/com/com_guid.h"

// From util/com/com_guid.cpp, which is excluded from the build because it
// includes ../../d3d11 + ../../dxgi interface headers that don't exist here.
// The frontend only needs these two exports.

namespace dxvk {

  bool logQueryInterfaceError(REFIID, REFIID) {
    return false;
  }

}

std::ostream& operator << (std::ostream& os, REFIID guid) {
  os << std::hex << std::setfill('0')
     << std::setw(8) << guid.Data1 << '-'
     << std::setw(4) << guid.Data2 << '-'
     << std::setw(4) << guid.Data3 << '-'
     << std::setw(2) << static_cast<short>(guid.Data4[0])
     << std::setw(2) << static_cast<short>(guid.Data4[1]) << '-';
  for (int i = 2; i < 8; i++)
    os << std::setw(2) << static_cast<short>(guid.Data4[i]);
  return os;
}
