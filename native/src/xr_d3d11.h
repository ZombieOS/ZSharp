#pragma once
#include <d3d11.h>

// The swapchain owns typeless storage; views must use its negotiated format.
inline D3D11_RENDER_TARGET_VIEW_DESC zsharp_xr_target_desc(DXGI_FORMAT format) {
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    return desc;
}
