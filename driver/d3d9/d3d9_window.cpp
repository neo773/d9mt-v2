#include "d3d9_window.h"

#include "d3d9_swapchain.h"

// The window proc is deliberately NOT hooked on this backend.
//
// DXVK's hook resized the window to the backbuffer on WM_ACTIVATEAPP and
// minimized it on focus loss. On winemac that strands a fullscreen window in
// the top-left on the first click, and nothing restores it afterwards, because
// there is no Windows window manager to undo either action. Fullscreen is
// borderless (D3D9SwapChainEx::EnterFullscreenMode) so no message-driven
// geometry fixups are needed at all — DXMT takes the same approach.
//
// The entry points below are kept as no-ops to satisfy callers.

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
    // Bring the focus window forward without moving or resizing it.
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
#endif
  }

}
