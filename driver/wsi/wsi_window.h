#pragma once

#include <windows.h>

#include "wsi_monitor.h"

namespace dxvk::wsi {

  struct DxvkWindowState {
#if defined(DXVK_WSI_WIN32)
    struct {
      LONG style   = 0;
      LONG exstyle = 0;
      RECT rect    = { 0, 0, 0, 0 };
    } win;
#endif
#if defined(DXVK_WSI_SDL3)
    struct {
      WsiMode fullscreenMode = { };
    } sdl3;
#endif
#if defined(DXVK_WSI_SDL2)
#endif
#if defined(DXVK_WSI_GLFW)
#endif
  };

  void getWindowSize(
          HWND      hWindow,
          uint32_t* pWidth,
          uint32_t* pWeight);

  void resizeWindow(
          HWND             hWindow,
          DxvkWindowState* pState,
          uint32_t         width,
          uint32_t         weight);

  void saveWindowState(
          HWND             hWindow,
          DxvkWindowState* pState,
          bool             saveStyle);

  void restoreWindowState(
          HWND             hWindow,
          DxvkWindowState* pState,
          bool             restoreCoordinates);

  bool setWindowMode(
          HMONITOR         hMonitor,
          HWND             hWindow,
          DxvkWindowState* pState,
    const WsiMode&         mode);

  bool enterFullscreenMode(
          HMONITOR         hMonitor,
          HWND             hWindow,
          DxvkWindowState* pState,
          [[maybe_unused]]
          bool             modeSwitch);

  bool leaveFullscreenMode(
          HWND             hWindow,
          DxvkWindowState* pState);

  bool restoreDisplayMode();

  HMONITOR getWindowMonitor(HWND hWindow);

  bool isWindow(HWND hWindow);

  bool isMinimized(HWND hWindow);

  bool isOccluded(HWND hWindow);

  void updateFullscreenWindow(
          HMONITOR hMonitor,
          HWND     hWindow,
          bool     forceTopmost);

}
