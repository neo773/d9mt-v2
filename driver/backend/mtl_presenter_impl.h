#pragma once
/*
 * MtlPresenterImpl — the real CAMetalLayer-backed present path (contract 3e).
 *
 * Attaches a CAMetalLayer to the D3D9 HWND via winemetal's
 * CreateMetalViewFromHWND (the v1 d9mtmetal approach) and pulls drawables with
 * MetalLayer_nextDrawable. Scan-out uses the METAL 4 present handshake driven
 * by the context in synchronizeWsi (queue waitForDrawable -> commit -> queue
 * signalDrawable -> MetalDrawable_present); present() here only tracks frames.
 *
 * The stub factory createMtlPresenter (mtl_presenter_stub.cpp) stays the seam
 * default; this is a distinct factory (createMtlPresenterImpl) the swapchain
 * REWRITE selects once it has a HWND, so there is no duplicate-symbol clash.
 */

#include "../mtl/mtl_presenter.h"

#include "wmt_handle.h"

namespace dxvk::backend {

  Rc<MtlPresenter> createMtlPresenterImpl(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc);

}
