#include "xr_d3d11.h"
#include "xr_scene_d3d11.h"
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(result)) return 1;
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = texture_desc.Height = 16;
    texture_desc.MipLevels = texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &texture))) return 2;
    ComPtr<ID3D11RenderTargetView> target;
    if (SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) return 3;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
        auto desc = zsharp_xr_target_desc(format);
        target.Reset();
        if (FAILED(device->CreateRenderTargetView(texture.Get(), &desc, &target))) return 4;
        D3D11_RENDER_TARGET_VIEW_DESC actual{};
        target->GetDesc(&actual);
        if (actual.Format != format) return 5;
    }
    std::puts("PASS: OpenXR typeless eye textures accept explicit linear and sRGB views");
    try {
        D3D11_TEXTURE2D_DESC color_info{};color_info.Width=color_info.Height=32;
        color_info.MipLevels=color_info.ArraySize=1;color_info.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        color_info.SampleDesc.Count=1;color_info.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> color,readback,depth;
        if(FAILED(device->CreateTexture2D(&color_info,nullptr,&color)))return 6;
        ComPtr<ID3D11RenderTargetView> output;
        if(FAILED(device->CreateRenderTargetView(color.Get(),nullptr,&output)))return 7;
        auto depth_info=color_info;depth_info.Format=DXGI_FORMAT_D32_FLOAT;depth_info.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        if(FAILED(device->CreateTexture2D(&depth_info,nullptr,&depth)))return 8;
        ComPtr<ID3D11DepthStencilView> depth_view;
        if(FAILED(device->CreateDepthStencilView(depth.Get(),nullptr,&depth_view)))return 9;
        auto staging=color_info;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateTexture2D(&staging,nullptr,&readback)))return 10;
        XRSceneGraphics renderer(device.Get(),context.Get());XRSceneView view;
        ZSharpGameRenderObject cube{};cube.shape=ZGAME_SHAPE_CUBE;cube.z=-3;
        cube.width=cube.height=cube.depth=1;cube.scale_x=cube.scale_y=cube.scale_z=1;
        if(!XRSceneGraphics::visible(cube,view))return 17;
        cube.z=3;if(XRSceneGraphics::visible(cube,view))return 18;
        cube.z=-3;cube.x=20;if(XRSceneGraphics::visible(cube,view))return 19;
        cube.x=4;cube.width=6;if(!XRSceneGraphics::visible(cube,view))return 20;
        ZSharpGameWorldTriangle triangle{};
        const float points[3][3]={{-2,-2,-2},{2,-2,-2},{0,2,-2}};
        memcpy(triangle.points,points,sizeof(points));triangle.color[0]=triangle.color[3]=1;
        std::vector<ZSharpGameWorldTriangle> world{triangle};
        auto render=[&]() {
            float black[]={0,0,0,1};context->ClearRenderTargetView(output.Get(),black);
            context->ClearDepthStencilView(depth_view.Get(),D3D11_CLEAR_DEPTH,1,0);
            ID3D11RenderTargetView *targets[]={output.Get()};context->OMSetRenderTargets(1,targets,depth_view.Get());
            D3D11_VIEWPORT viewport{0,0,32,32,0,1};context->RSSetViewports(1,&viewport);renderer.draw(world,view);
            context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(readback.Get(),color.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)))throw std::runtime_error("pixel readback failed");
            auto *pixel=(const unsigned char *)mapped.pData+16*mapped.RowPitch+16*4;
            std::vector<unsigned> result{pixel[0],pixel[1],pixel[2],pixel[3]};context->Unmap(readback.Get(),0);return result;
        };
        auto red=render();if(red[0]<250 || red[1]>2 || red[2]>2)return 11;
        // Textured triangles use cached RGBA images, not the solid tint path.
        const unsigned char blue[]={0,0,255,255};world[0].texture_key=blue;world[0].pixels=blue;
        world[0].texture_width=world[0].texture_height=1;world[0].texture_pitch=4;
        world[0].color[0]=world[0].color[1]=world[0].color[2]=1;
        auto textured=render();if(textured[2]<250 || textured[0]>2 || textured[1]>2)return 12;
        // A translucent green surface blends over opaque red while retaining depth tests.
        world={triangle,triangle};for(auto &p:world[1].points){p[0]*=.5f;p[1]*=.5f;p[2]=-1;}
        world[1].color[0]=0;world[1].color[1]=1;world[1].color[3]=.5f;
        auto blended=render();if(blended[0]<120 || blended[0]>135 || blended[1]<120 || blended[1]>135 || blended[2]>2)return 13;
        // A fully transparent cutout must not hide the red triangle behind it.
        const unsigned char clear[]={0,0,255,0};world[1].texture_key=clear;world[1].pixels=clear;
        world[1].texture_width=world[1].texture_height=1;world[1].texture_pitch=4;world[1].color[3]=1;
        auto cutout=render();if(cutout[0]<250 || cutout[1]>2 || cutout[2]>2)return 14;
        view.rig_position[0]=10;auto moved=render();if(moved[0] || moved[1] || moved[2])return 15;
        std::puts("PASS: XR offscreen GPU solid/textured triangles, transparency, cutout depth and rig translation");
    }catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 16;}
    return 0;
}
