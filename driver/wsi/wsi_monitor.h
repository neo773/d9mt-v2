#pragma once

#include <windows.h>

#include <array>
#include <vector>
#include <cstdint>

#include "wsi_edid.h"

namespace dxvk::wsi {

  struct WsiRational {
    uint32_t numerator;
    uint32_t denominator;
  };

  struct WsiMode {
    uint32_t     width;
    uint32_t     height;
    WsiRational  refreshRate;
    uint32_t     bitsPerPixel;
    bool         interlaced;
  };

  HMONITOR getDefaultMonitor();

  HMONITOR enumMonitors(uint32_t index);

  HMONITOR enumMonitors(const LUID *adapterLUID[], uint32_t numLUIDs, uint32_t index);

  bool getDisplayName(
          HMONITOR         hMonitor,
          WCHAR            (&Name)[32]);

  bool getDesktopCoordinates(
          HMONITOR         hMonitor,
          RECT*            pRect);

  bool getDisplayMode(
          HMONITOR         hMonitor,
          uint32_t         modeNumber,
          WsiMode*         pMode);

  bool getCurrentDisplayMode(
          HMONITOR         hMonitor,
          WsiMode*         pMode);

  bool getDesktopDisplayMode(
          HMONITOR         hMonitor,
          WsiMode*         pMode);

  inline void getMonitorClientSize(
          HMONITOR                hMonitor,
          UINT*                   pWidth,
          UINT*                   pHeight) {
    RECT rect = { };
    getDesktopCoordinates(hMonitor, &rect);

    if (pWidth)
      *pWidth = rect.right - rect.left;

    if (pHeight)
      *pHeight = rect.bottom - rect.top;
  }

  WsiEdidData getMonitorEdid(HMONITOR hMonitor);

}
