// PHASE-C10: minimal local firewall impl of the com_guid seam.
//
// The full util/com/com_guid.cpp is excluded from CORE because it pulls
// ../../d3d11 + ../../dxgi interface headers (for __uuidof registration) that
// are not part of the ported driver tree. The D3D9 frontend only needs the two
// free functions — logQueryInterfaceError() and operator<<(ostream, REFIID) —
// so we provide a self-contained implementation here (no d3d11/dxgi, no
// DxvkHash dedup set). Behaviour: log every unknown QI at Warn level.
#include "../util/com/com_guid.h"

#include "../util/log/log.h"

#include <iomanip>

namespace dxvk {

  bool logQueryInterfaceError(REFIID objectGuid, REFIID requestedGuid) {
    (void)objectGuid;
    (void)requestedGuid;
    return Logger::logLevel() <= LogLevel::Warn;
  }

}

std::ostream& operator << (std::ostream& os, REFIID guid) {
  os << std::hex << std::setfill('0')
     << std::setw(8) << guid.Data1 << '-';

  os << std::hex << std::setfill('0')
     << std::setw(4) << guid.Data2 << '-';

  os << std::hex << std::setfill('0')
     << std::setw(4) << guid.Data3 << '-';

  os << std::hex << std::setfill('0')
     << std::setw(2) << static_cast<short>(guid.Data4[0])
     << std::setw(2) << static_cast<short>(guid.Data4[1])
     << '-'
     << std::setw(2) << static_cast<short>(guid.Data4[2])
     << std::setw(2) << static_cast<short>(guid.Data4[3])
     << std::setw(2) << static_cast<short>(guid.Data4[4])
     << std::setw(2) << static_cast<short>(guid.Data4[5])
     << std::setw(2) << static_cast<short>(guid.Data4[6])
     << std::setw(2) << static_cast<short>(guid.Data4[7]);
  return os;
}
