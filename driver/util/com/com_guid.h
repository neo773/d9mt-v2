#pragma once

#include <ostream>
#include <iomanip>

#include "com_include.h"

namespace dxvk {

  bool logQueryInterfaceError(REFIID objectGuid, REFIID requestedGuid);

};

std::ostream& operator << (std::ostream& os, REFIID guid);
