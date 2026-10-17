#pragma once
#include "game_vulkan.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

// Independent of OpenXR handles so GPU scene submission is testable on WARP.
struct XRSceneView {
    float position[3]={0,0,0}, inverse_orientation[4]={0,0,0,1};
    float rig_position[3]={0,0,0}, rig_yaw=0;
    float left=-1,right=1,down=-1,up=1;
};
class XRSceneGraphics {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Texture {Ptr<ID3D11ShaderResourceView> view;bool blend=false;};
    struct Vertex {float position[4],color[4],uv[2];};
    struct Draw {Vertex vertices[3];Texture *texture;bool blend;float distance;size_t sequence;};
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11InputLayout> layout;Ptr<ID3D11Buffer> buffer;
    Ptr<ID3D11SamplerState> sampler;Ptr<ID3D11RasterizerState> raster;
    Ptr<ID3D11DepthStencilState> opaque_depth,blend_depth;
    Ptr<ID3D11BlendState> blend_state;
    Texture white;
    std::unordered_map<const void *,Texture> textures;
    size_t capacity=0;
    std::vector<Draw> draws;
    std::vector<Vertex> vertices;
    static void checked(HRESULT result,const char *label) {
        if(FAILED(result))throw std::runtime_error(std::string(label)+" (Direct3D "+std::to_string(result)+")");
    }
    Texture upload(const unsigned char *pixels,int width,int height,int pitch) {
        if(!pixels || width<=0 || height<=0 || width>16384 || height>16384 || pitch<width*4)
            throw std::runtime_error("Invalid XR texture dimensions or pixel stride");
        Texture result;
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            unsigned alpha=pixels[(size_t)y*pitch+x*4+3];
            if(alpha && alpha<255){result.blend=true;break;}
        }
        D3D11_TEXTURE2D_DESC info{};info.Width=width;info.Height=height;info.MipLevels=info.ArraySize=1;
        info.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;info.SampleDesc.Count=1;
        info.Usage=D3D11_USAGE_IMMUTABLE;info.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels,(UINT)pitch,0};Ptr<ID3D11Texture2D> image;
        checked(device->CreateTexture2D(&info,&data,&image),"XR texture upload");
        checked(device->CreateShaderResourceView(image.Get(),nullptr,&result.view),"XR texture view");
        return result;
    }
    Texture *texture(const ZSharpGameWorldTriangle &triangle) {
        if(!triangle.texture_key)return &white;
        auto found=textures.find(triangle.texture_key);
        if(found!=textures.end())return &found->second;
        auto image=upload(triangle.pixels,triangle.texture_width,triangle.texture_height,triangle.texture_pitch);
        return &textures.emplace(triangle.texture_key,std::move(image)).first->second;
    }
    static void project(const float source[3],const XRSceneView &view,float clip[4]) {
        float x=source[0]-view.rig_position[0],y=source[1]-view.rig_position[1],z=source[2]-view.rig_position[2];
        float yaw=-view.rig_yaw*3.14159265358979323846f/180,rx=x*cosf(yaw)+z*sinf(yaw);
        z=-x*sinf(yaw)+z*cosf(yaw);x=rx;
        x-=view.position[0];y-=view.position[1];z-=view.position[2];
        const float *q=view.inverse_orientation;
        float tx=2*(q[1]*z-q[2]*y),ty=2*(q[2]*x-q[0]*z),tz=2*(q[0]*y-q[1]*x);
        rx=x+q[3]*tx+q[1]*tz-q[2]*ty;
        float ry=y+q[3]*ty+q[2]*tx-q[0]*tz;
        float rz=z+q[3]*tz+q[0]*ty-q[1]*tx;
        float distance=-rz;
        clip[0]=2*rx/(view.right-view.left)-(view.right+view.left)*distance/(view.right-view.left);
        clip[1]=2*ry/(view.up-view.down)-(view.up+view.down)*distance/(view.up-view.down);
        clip[2]=(100.0f/99.95f)*distance-(5.0f/99.95f);clip[3]=distance;
    }
    static bool outside(const Draw &draw) {
        for(int plane=0;plane<6;plane++) {
            int count=0;
            for(auto &v:draw.vertices) {
                auto p=v.position;
                float distance=plane==0?p[0]+p[3]:plane==1?p[3]-p[0]:plane==2?p[1]+p[3]:
                    plane==3?p[3]-p[1]:plane==4?p[2]:p[3]-p[2];
                count+=distance<0;
            }
            if(count==3)return true;
        }return false;
    }
public:
    static bool visible(const ZSharpGameRenderObject &object,const XRSceneView &view) {
        // Imported FBX origins need not be at the bounds center; leave meshes
        // unculled here rather than accidentally hiding offset mesh geometry.
        if(object.shape!=ZGAME_SHAPE_CUBE)return true;
        float source[]={object.x,object.y,object.z},clip[4];project(source,view,clip);
        float x=object.width*object.scale_x,y=object.height*object.scale_y,z=object.depth*object.scale_z;
        float radius=.5f*sqrtf(x*x+y*y+z*z);
        if(clip[3]+radius<.05f || clip[3]-radius>100)return false;
        float ax=2/(view.right-view.left),bx=(view.right+view.left)/(view.right-view.left);
        float ay=2/(view.up-view.down),by=(view.up+view.down)/(view.up-view.down);
        return clip[0]+clip[3]>=-radius*sqrtf(ax*ax+(1-bx)*(1-bx)) &&
            clip[3]-clip[0]>=-radius*sqrtf(ax*ax+(1+bx)*(1+bx)) &&
            clip[1]+clip[3]>=-radius*sqrtf(ay*ay+(1-by)*(1-by)) &&
            clip[3]-clip[1]>=-radius*sqrtf(ay*ay+(1+by)*(1+by));
    }
    XRSceneGraphics(ID3D11Device *d,ID3D11DeviceContext *c):device(d),context(c) {
        const char *shader=
            "Texture2D image:register(t0);SamplerState sampleImage:register(s0);"
            "struct V{float4 p:POSITION;float4 c:COLOR;float2 uv:TEXCOORD;};"
            "struct P{float4 p:SV_POSITION;float4 c:COLOR;float2 uv:TEXCOORD;};"
            "P vs(V v){P p;p.p=v.p;p.c=v.c;p.uv=v.uv;return p;}"
            "float4 ps(P p):SV_TARGET{float4 c=image.Sample(sampleImage,p.uv)*p.c;clip(c.a-0.0039);return c;}";
        Ptr<ID3DBlob> vertex,pixel,error;
        checked(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vertex,&error),"XR vertex shader");
        checked(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&pixel,&error),"XR pixel shader");
        checked(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs),"XR vertex shader creation");
        checked(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps),"XR pixel shader creation");
        D3D11_INPUT_ELEMENT_DESC fields[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
        checked(device->CreateInputLayout(fields,3,vertex->GetBufferPointer(),vertex->GetBufferSize(),&layout),"XR input layout");
        D3D11_SAMPLER_DESC sample{};sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sample.MaxLOD=D3D11_FLOAT32_MAX;
        sample.MaxAnisotropy=1;sample.ComparisonFunc=D3D11_COMPARISON_NEVER;
        checked(device->CreateSamplerState(&sample,&sampler),"XR texture sampler");
        D3D11_RASTERIZER_DESC raster_info{};raster_info.FillMode=D3D11_FILL_SOLID;raster_info.CullMode=D3D11_CULL_NONE;raster_info.DepthClipEnable=TRUE;
        checked(device->CreateRasterizerState(&raster_info,&raster),"XR rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        checked(device->CreateDepthStencilState(&depth,&opaque_depth),"XR opaque depth state");
        depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        checked(device->CreateDepthStencilState(&depth,&blend_depth),"XR transparent depth state");
        D3D11_BLEND_DESC blend{};auto &target=blend.RenderTarget[0];target.BlendEnable=TRUE;
        target.SrcBlend=D3D11_BLEND_SRC_ALPHA;target.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;target.BlendOp=D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha=D3D11_BLEND_ONE;target.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;target.BlendOpAlpha=D3D11_BLEND_OP_ADD;target.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        checked(device->CreateBlendState(&blend,&blend_state),"XR transparent blend state");
        const unsigned char pixel_white[]={255,255,255,255};white=upload(pixel_white,1,1,4);
    }
    void draw(const std::vector<ZSharpGameWorldTriangle> &world,const XRSceneView &view) {
        draws.clear();vertices.clear();
        for(auto &triangle:world) {
            Draw draw{};draw.texture=texture(triangle);draw.blend=triangle.color[3]<.999f || draw.texture->blend;
            draw.sequence=draws.size();
            for(int corner=0;corner<3;corner++) {
                project(triangle.points[corner],view,draw.vertices[corner].position);
                memcpy(draw.vertices[corner].color,triangle.color,sizeof(triangle.color));
                memcpy(draw.vertices[corner].uv,triangle.uv[corner],sizeof(triangle.uv[corner]));
                draw.distance+=draw.vertices[corner].position[3]/3;
            }
            if(!outside(draw))draws.push_back(draw);
        }
        std::sort(draws.begin(),draws.end(),[](const Draw &a,const Draw &b){
            if(a.blend!=b.blend)return !a.blend;
            if(a.blend && a.distance!=b.distance)return a.distance>b.distance;
            if(!a.blend && a.texture!=b.texture)return std::less<Texture *>()(a.texture,b.texture);
            return a.sequence<b.sequence;
        });
        if(draws.empty())return;
        if(draws.size()>(std::numeric_limits<UINT>::max)()/(3*sizeof(Vertex)))throw std::runtime_error("XR vertex buffer exceeds Direct3D limits");
        vertices.reserve(draws.size()*3);for(auto &draw:draws)vertices.insert(vertices.end(),draw.vertices,draw.vertices+3);
        if(vertices.size()>capacity) {
            buffer.Reset();capacity=vertices.size();D3D11_BUFFER_DESC info{};info.ByteWidth=(UINT)(capacity*sizeof(Vertex));
            info.Usage=D3D11_USAGE_DYNAMIC;info.BindFlags=D3D11_BIND_VERTEX_BUFFER;info.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            checked(device->CreateBuffer(&info,nullptr,&buffer),"XR vertex buffer");
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"XR vertex upload");
        memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(Vertex));context->Unmap(buffer.Get(),0);
        UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer *buffers[]={buffer.Get()};
        context->IASetVertexBuffers(0,1,buffers,&stride,&offset);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(raster.Get());context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11SamplerState *samplers[]={sampler.Get()};context->PSSetSamplers(0,1,samplers);
        for(size_t start=0;start<draws.size();) {
            size_t end=start+1;while(end<draws.size() && draws[end].texture==draws[start].texture && draws[end].blend==draws[start].blend)end++;
            ID3D11ShaderResourceView *images[]={draws[start].texture->view.Get()};context->PSSetShaderResources(0,1,images);
            context->OMSetBlendState(draws[start].blend?blend_state.Get():nullptr,nullptr,0xffffffff);
            context->OMSetDepthStencilState(draws[start].blend?blend_depth.Get():opaque_depth.Get(),0);
            context->Draw((UINT)((end-start)*3),(UINT)(start*3));start=end;
        }
        ID3D11ShaderResourceView *empty[]={nullptr};context->PSSetShaderResources(0,1,empty);
    }
};
