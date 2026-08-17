#include "d3d9_window.h"

#include "d3d9_swapchain.h"

namespace dxvk
{

  D3D9WindowMessageFilter::D3D9WindowMessageFilter(HWND window, bool filter) {

  }

  D3D9WindowMessageFilter::~D3D9WindowMessageFilter() {

  }

  void ResetWindowProc(HWND window) {

  }

  void HookWindowProc(HWND window, D3D9SwapChainEx* swapchain) {

  }

  void SetActivateProcessed(HWND window, bool processed) {

  }

  void ActivateFocusWindow(HWND window) {
#ifdef _WIN32
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
#endif
  }

}
