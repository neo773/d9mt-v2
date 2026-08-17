#pragma once

#include "../mtl/mtl_presenter.h"

#include "wmt_handle.h"

namespace dxvk::backend {

  Rc<MtlPresenter> createMtlPresenterImpl(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc);

}
