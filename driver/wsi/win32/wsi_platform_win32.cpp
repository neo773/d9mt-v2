#if defined(DXVK_WSI_WIN32)

#include "wsi_platform_win32.h"

namespace dxvk::wsi {

  std::vector<const char *> Win32WsiDriver::getInstanceExtensions() {
    // PHASE-C: returned the Vulkan WSI surface extensions (VK_KHR_*surface). The
    // Metal present path (CAMetalLayer) needs no instance extensions — empty set.
    return { };
  }

  static bool createWin32WsiDriver(WsiDriver **driver) {
    *driver = new Win32WsiDriver();
    return true;
  }

  WsiBootstrap Win32WSI = {
    "Win32",
    createWin32WsiDriver
  };

}

#endif
