#pragma once
/*
 * MtlPresenterImpl — the real CAMetalLayer-backed present path (contract 3e).
 *
 * Attaches a CAMetalLayer to the D3D9 HWND via winemetal's
 * CreateMetalViewFromHWND (the v1 d9mtmetal approach), pulls drawables with
 * MetalLayer_nextDrawable, and drives scan-out through
 * MTLCommandBuffer_presentDrawable (recorded by the context in synchronizeWsi).
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
