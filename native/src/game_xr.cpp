/* Experimental Windows OpenXR backend. Desktop SDL/Vulkan is independent.
 * Shared scene/material/light/shadow preparation; GPU stereo rasterization. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include "xr_d3d11.h"
#include "xr_scene_d3d11.h"
#include <dxgi.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include "game_xr.h"
#include "xr_ui.h"
#include "xr_ui_world.h"
#include "xr_hands_openxr.h"
using Microsoft::WRL::ComPtr;
static constexpr float pi=3.14159265358979323846f;
static void check(XrResult r,const char *what){if(XR_FAILED(r))throw std::runtime_error(std::string(what)+" (OpenXR "+std::to_string(r)+")");}
static void hr(HRESULT r,const char *what){if(FAILED(r))throw std::runtime_error(std::string(what)+" (Direct3D "+std::to_string(r)+")");}
static XrPosef identity(){XrPosef p{};p.orientation.w=1;return p;}
struct Eye {
    XrSwapchain swap=XR_NULL_HANDLE; uint32_t width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> targets;
    ComPtr<ID3D11DepthStencilView> depth;
};
struct ZSharpGameXR {
    XrInstance instance=XR_NULL_HANDLE;XrSystemId system=XR_NULL_SYSTEM_ID;
    XrSession session=XR_NULL_HANDLE;XrSpace space=XR_NULL_HANDLE,head=XR_NULL_HANDLE,hand[2]{},aim_space[2]{};
    XrActionSet actions=XR_NULL_HANDLE;
    XrAction pose{},aim_pose{},trigger{},grip{},stick{},click{},buttons[4]{},haptic{};
    XrPath hands[2]{};
    Eye eyes[2];XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    bool running=false,begun=false,focused=false,views_valid=false;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    std::unique_ptr<XRSceneGraphics> graphics;
    std::vector<ZSharpGameWorldTriangle> world;
    unsigned vibration_seen[2]{};
    size_t hovered_button[2]{};
    XRHandsBackend bare_hands;
};
static XrPath xrpath(ZSharpGameXR *x,const char *name){XrPath p{};check(xrStringToPath(x->instance,name,&p),"xrStringToPath");return p;}
static XrAction action(ZSharpGameXR *x,const char *name,XrActionType type){
    XrAction a{};XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    snprintf(info.actionName,sizeof(info.actionName),"%s",name);snprintf(info.localizedActionName,sizeof(info.localizedActionName),"%s",name);
    info.actionType=type;info.countSubactionPaths=2;info.subactionPaths=x->hands;
    check(xrCreateAction(x->actions,&info,&a),"xrCreateAction");return a;
}
static void setup_actions(ZSharpGameXR *x){
    x->hands[0]=xrpath(x,"/user/hand/left");x->hands[1]=xrpath(x,"/user/hand/right");
    XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};strcpy_s(info.actionSetName,"gameplay");strcpy_s(info.localizedActionSetName,"Gameplay");
    check(xrCreateActionSet(x->instance,&info,&x->actions),"xrCreateActionSet");
    x->pose=action(x,"hand_pose",XR_ACTION_TYPE_POSE_INPUT);
    x->aim_pose=action(x,"controller_aim",XR_ACTION_TYPE_POSE_INPUT);
    x->trigger=action(x,"trigger",XR_ACTION_TYPE_FLOAT_INPUT);
    x->grip=action(x,"grip",XR_ACTION_TYPE_FLOAT_INPUT);x->stick=action(x,"stick",XR_ACTION_TYPE_VECTOR2F_INPUT);
    x->click=action(x,"stick_click",XR_ACTION_TYPE_BOOLEAN_INPUT);x->haptic=action(x,"vibration",XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const char *names[]={"a","b","x","y"};for(int i=0;i<4;i++)x->buttons[i]=action(x,names[i],XR_ACTION_TYPE_BOOLEAN_INPUT);
    std::vector<XrActionSuggestedBinding> bindings;
    for(int h=0;h<2;h++){
        std::string root=h?"/user/hand/right":"/user/hand/left";
        auto bind=[&](XrAction a,const char *suffix){bindings.push_back({a,xrpath(x,(root+suffix).c_str())});};
        bind(x->pose,"/input/grip/pose");bind(x->aim_pose,"/input/aim/pose");
        bind(x->trigger,"/input/trigger/value");bind(x->grip,"/input/squeeze/value");
        bind(x->stick,"/input/thumbstick");bind(x->click,"/input/thumbstick/click");bind(x->haptic,"/output/haptic");
        bind(x->buttons[h?0:2],h?"/input/a/click":"/input/x/click");bind(x->buttons[h?1:3],h?"/input/b/click":"/input/y/click");
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile=xrpath(x,"/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings=(uint32_t)bindings.size();suggested.suggestedBindings=bindings.data();
    check(xrSuggestInteractionProfileBindings(x->instance,&suggested),"xrSuggestInteractionProfileBindings");
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attach.countActionSets=1;attach.actionSets=&x->actions;
    check(xrAttachSessionActionSets(x->session,&attach),"xrAttachSessionActionSets");
    for(int h=0;h<2;h++){
        XrActionSpaceCreateInfo s{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        s.action=x->pose;s.subactionPath=x->hands[h];s.poseInActionSpace=identity();
        check(xrCreateActionSpace(x->session,&s,&x->hand[h]),"controller grip space");
        s.action=x->aim_pose;
        check(xrCreateActionSpace(x->session,&s,&x->aim_space[h]),"controller aim space");
    }
}
static void setup_graphics(ZSharpGameXR *x){
    PFN_xrGetD3D11GraphicsRequirementsKHR requirements=nullptr;
    check(xrGetInstanceProcAddr(x->instance,"xrGetD3D11GraphicsRequirementsKHR",(PFN_xrVoidFunction*)&requirements),"D3D11 extension");
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};check(requirements(x->instance,x->system,&req),"xrGetD3D11GraphicsRequirementsKHR");
    ComPtr<IDXGIFactory1> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);if(!memcmp(&desc.AdapterLuid,&req.adapterLuid,sizeof(LUID))){adapter=candidate;break;}}
    if(!adapter)throw std::runtime_error("OpenXR graphics adapter was not found");
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,2,D3D11_SDK_VERSION,&x->device,nullptr,&x->context),"D3D11CreateDevice");
    if(x->device->GetFeatureLevel()<req.minFeatureLevel)throw std::runtime_error("OpenXR requires a newer GPU feature level");
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=x->device.Get();
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};session.next=&binding;session.systemId=x->system;
    check(xrCreateSession(x->instance,&session,&x->session),"xrCreateSession");
    uint32_t count=0;check(xrEnumerateViewConfigurationViews(x->instance,x->system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"view count");
    if(count!=2)throw std::runtime_error("This PCVR prototype requires stereo views");
    XrViewConfigurationView config[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    check(xrEnumerateViewConfigurationViews(x->instance,x->system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,config),"view configuration");
    check(xrEnumerateSwapchainFormats(x->session,0,&count,nullptr),"swapchain formats");std::vector<int64_t> formats(count);
    check(xrEnumerateSwapchainFormats(x->session,count,&count,formats.data()),"swapchain formats");
    int64_t format=0;for(auto f:formats)if(f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f==DXGI_FORMAT_R8G8B8A8_UNORM){format=f;break;}
    if(!format)throw std::runtime_error("OpenXR does not support RGBA8 swapchains");
    for(int i=0;i<2;i++){
        Eye &e=x->eyes[i];e.width=config[i].recommendedImageRectWidth;e.height=config[i].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format=format;e.format=(DXGI_FORMAT)format;sc.sampleCount=1;sc.width=e.width;sc.height=e.height;sc.faceCount=sc.arraySize=sc.mipCount=1;
        check(xrCreateSwapchain(x->session,&sc,&e.swap),"xrCreateSwapchain");
        check(xrEnumerateSwapchainImages(e.swap,0,&count,nullptr),"swapchain image count");e.images.resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        check(xrEnumerateSwapchainImages(e.swap,count,&count,(XrSwapchainImageBaseHeader*)e.images.data()),"swapchain images");
        e.targets.resize(count);
        auto target_desc=zsharp_xr_target_desc(e.format);
        for(uint32_t image=0;image<count;image++)hr(x->device->CreateRenderTargetView(e.images[image].texture,&target_desc,&e.targets[image]),"eye render target");
        D3D11_TEXTURE2D_DESC depth{};depth.Width=e.width;depth.Height=e.height;depth.MipLevels=depth.ArraySize=1;
        depth.Format=DXGI_FORMAT_D32_FLOAT;depth.SampleDesc.Count=1;depth.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture;hr(x->device->CreateTexture2D(&depth,nullptr,&texture),"depth texture");
        hr(x->device->CreateDepthStencilView(texture.Get(),nullptr,&e.depth),"depth view");
    }
    x->graphics=std::make_unique<XRSceneGraphics>(x->device.Get(),x->context.Get());
}
extern "C" ZSharpGameXR *zsharp_game_xr_create(char *error,size_t cap){
    ZSharpGameXR *x=new ZSharpGameXR;
    try{
        uint32_t count=0;
        check(xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"OpenXR extension count");
        std::vector<XrExtensionProperties> supported(count,{XR_TYPE_EXTENSION_PROPERTIES});
        check(xrEnumerateInstanceExtensionProperties(nullptr,count,&count,supported.data()),"OpenXR extensions");
        auto supports=[&](const char *name){for(const auto &extension:supported)if(!strcmp(extension.extensionName,name))return true;return false;};
        std::vector<const char *> extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        bool hands=supports(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        if(hands)extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(info.applicationInfo.applicationName,"ZSharp PCVR");
        strcpy_s(info.applicationInfo.engineName,"ZVM");info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        info.enabledExtensionCount=(uint32_t)extensions.size();info.enabledExtensionNames=extensions.data();
        check(xrCreateInstance(&info,&x->instance),"Could not initialize OpenXR runtime");
        XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};system.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        check(xrGetSystem(x->instance,&system,&x->system),"No headset connection was found");
        setup_graphics(x);setup_actions(x);
        x->bare_hands.init(x->instance,x->session,x->system,hands);
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};space.poseInReferenceSpace=identity();space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;
        if(XR_FAILED(xrCreateReferenceSpace(x->session,&space,&x->space))){space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;space.poseInReferenceSpace.position.y=-1.6f;check(xrCreateReferenceSpace(x->session,&space,&x->space),"floor fallback reference space");}
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;space.poseInReferenceSpace=identity();check(xrCreateReferenceSpace(x->session,&space,&x->head),"head space");
        return x;
    }catch(const std::exception &e){snprintf(error,cap,"%s",e.what());zsharp_game_xr_destroy(x);return nullptr;}
}
static void pose_out(const XrSpaceLocation &location,ZSharpXRPose &out){
    const auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const auto tracked=XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    out.tracked=(location.locationFlags&tracked)==tracked;
    if((location.locationFlags&valid)!=valid){out.tracked=0;return;}
    out.position[0]=location.pose.position.x;out.position[1]=location.pose.position.y;out.position[2]=location.pose.position.z;
    auto q=location.pose.orientation;const float values[]={q.x,q.y,q.z,q.w};
    zsharp_xr_orientation(values,out.rotation);
}
static float float_action(ZSharpGameXR *x,XrAction a,int h){XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=a;info.subactionPath=x->hands[h];XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};check(xrGetActionStateFloat(x->session,&info,&s),"float action");return s.isActive?s.currentState:0;}
static bool bool_action(ZSharpGameXR *x,XrAction a,int h){XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=a;info.subactionPath=x->hands[h];XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};check(xrGetActionStateBoolean(x->session,&info,&s),"button action");return s.isActive&&s.currentState;}
extern "C" int zsharp_game_xr_begin(ZSharpGameXR *x,ZSharpXRState *s,char *error,size_t cap){
    try{
        s->available=s->active=1;
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};XrResult event_result;
        while((event_result=xrPollEvent(x->instance,&event))==XR_SUCCESS){
            if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){auto changed=(XrEventDataSessionStateChanged*)&event;
                x->focused=changed->state==XR_SESSION_STATE_FOCUSED;
                if(changed->state==XR_SESSION_STATE_READY){XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;check(xrBeginSession(x->session,&begin),"xrBeginSession");x->running=true;}
                if(changed->state==XR_SESSION_STATE_STOPPING){check(xrEndSession(x->session),"xrEndSession");x->running=false;}
                if(changed->state==XR_SESSION_STATE_EXITING||changed->state==XR_SESSION_STATE_LOSS_PENDING)return 2;
            }else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)return 2;
            event={XR_TYPE_EVENT_DATA_BUFFER};
        }
        if(event_result!=XR_EVENT_UNAVAILABLE)check(event_result,"xrPollEvent");
        s->focused=x->focused;
        if(!x->running){
            x->bare_hands.update(XR_NULL_HANDLE,0,false,*s);
            s->head.tracked=0;for(auto &h:s->hands){h.pose.tracked=h.aim_tracked=0;h.trigger=h.grip=h.stick[0]=h.stick[1]=0;for(int b=0;b<ZXR_BUTTONS;b++)zsharp_xr_button(&h,b,0);}return 1;
        }
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};check(xrWaitFrame(x->session,&wait,&x->frame),"xrWaitFrame");
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};check(xrBeginFrame(x->session,&begin),"xrBeginFrame");x->begun=true;
        x->bare_hands.update(x->space,x->frame.predictedDisplayTime,x->focused,*s);
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;locate.displayTime=x->frame.predictedDisplayTime;locate.space=x->space;
        XrViewState viewstate{XR_TYPE_VIEW_STATE};uint32_t count=0;
        check(xrLocateViews(x->session,&locate,&viewstate,2,&count,x->views),"xrLocateViews");
        x->views_valid=count==2 && (viewstate.viewStateFlags&(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT))==(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};check(xrLocateSpace(x->head,x->space,x->frame.predictedDisplayTime,&location),"head tracking");pose_out(location,s->head);
        XrActiveActionSet set{x->actions,XR_NULL_PATH};XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&set;
        check(xrSyncActions(x->session,&sync),"xrSyncActions");
        for(int h=0;h<2;h++){
            auto &out=s->hands[h];location={XR_TYPE_SPACE_LOCATION};check(xrLocateSpace(x->hand[h],x->space,x->frame.predictedDisplayTime,&location),"controller grip tracking");pose_out(location,out.pose);
            location={XR_TYPE_SPACE_LOCATION};
            check(xrLocateSpace(x->aim_space[h],x->space,x->frame.predictedDisplayTime,&location),"controller aim tracking");
            ZSharpXRPose aim{};pose_out(location,aim);
            XrActionStateGetInfo aim_get{XR_TYPE_ACTION_STATE_GET_INFO};aim_get.action=x->aim_pose;aim_get.subactionPath=x->hands[h];
            XrActionStatePose aim_state{XR_TYPE_ACTION_STATE_POSE};
            check(xrGetActionStatePose(x->session,&aim_get,&aim_state),"controller aim action");
            out.aim_tracked=aim.tracked&&aim_state.isActive&&x->focused;
            memcpy(out.aim_position,aim.position,sizeof(out.aim_position));
            out.aim_orientation[0]=location.pose.orientation.x;out.aim_orientation[1]=location.pose.orientation.y;
            out.aim_orientation[2]=location.pose.orientation.z;out.aim_orientation[3]=location.pose.orientation.w;
            out.trigger=x->focused?float_action(x,x->trigger,h):0;out.grip=x->focused?float_action(x,x->grip,h):0;
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};get.action=x->stick;get.subactionPath=x->hands[h];XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};check(xrGetActionStateVector2f(x->session,&get,&stick),"stick input");
            out.stick[0]=x->focused&&stick.isActive?stick.currentState.x:0;out.stick[1]=x->focused&&stick.isActive?stick.currentState.y:0;
            if(hypotf(out.stick[0],out.stick[1])<0.15f)out.stick[0]=out.stick[1]=0;
            zsharp_xr_button(&out,ZXR_TRIGGER,out.trigger>=0.5f);zsharp_xr_button(&out,ZXR_GRIP,out.grip>=0.5f);
            zsharp_xr_button(&out,ZXR_STICK,x->focused&&bool_action(x,x->click,h));
            for(int b=0;b<4;b++)zsharp_xr_button(&out,ZXR_A+b,x->focused&&bool_action(x,x->buttons[b],h));
            if(s->vibration_serial[h]!=x->vibration_seen[h]){
                x->vibration_seen[h]=s->vibration_serial[h];XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};info.action=x->haptic;info.subactionPath=x->hands[h];
                if(x->focused&&s->vibration[h]>0&&s->vibration_ms[h]>0){XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};v.amplitude=s->vibration[h];v.duration=(XrDuration)(s->vibration_ms[h]*1000000.0);v.frequency=XR_FREQUENCY_UNSPECIFIED;check(xrApplyHapticFeedback(x->session,&info,(XrHapticBaseHeader*)&v),"controller vibration");}
                else check(xrStopHapticFeedback(x->session,&info),"stop vibration");
            }
        }
        return 1;
    }catch(const std::exception &e){snprintf(error,cap,"%s",e.what());return 0;}
}
static XrVector3f euler(XrVector3f p,float x,float y,float z){
    x*=pi/180;y*=pi/180;z*=pi/180;
    float a=p.y*cosf(x)-p.z*sinf(x),b=p.y*sinf(x)+p.z*cosf(x);p.y=a;p.z=b;
    a=p.x*cosf(y)+p.z*sinf(y);b=-p.x*sinf(y)+p.z*cosf(y);p.x=a;p.z=b;
    a=p.x*cosf(z)-p.y*sinf(z);b=p.x*sinf(z)+p.y*cosf(z);p.x=a;p.y=b;return p;
}
struct WorldCollector {std::vector<ZSharpGameWorldTriangle> *world;XRSceneView views[2];};
static int collect_world(void *user,const ZSharpGameWorldTriangle *triangle) {
    auto *collector=static_cast<WorldCollector *>(user);
    try {collector->world->push_back(*triangle);return 1;}catch(...){return 0;}
}
static int visible_world(void *user,const ZSharpGameRenderObject *object) {
    auto *collector=static_cast<WorldCollector *>(user);
    return XRSceneGraphics::visible(*object,collector->views[0]) || XRSceneGraphics::visible(*object,collector->views[1]);
}
static XRSceneView scene_view(const XrView &eye,const ZSharpXRState &state) {
    XRSceneView view;
    view.position[0]=eye.pose.position.x;view.position[1]=eye.pose.position.y;view.position[2]=eye.pose.position.z;
    view.inverse_orientation[0]=-eye.pose.orientation.x;view.inverse_orientation[1]=-eye.pose.orientation.y;view.inverse_orientation[2]=-eye.pose.orientation.z;view.inverse_orientation[3]=eye.pose.orientation.w;
    memcpy(view.rig_position,state.rig_position,sizeof(view.rig_position));view.rig_yaw=state.rig_rotation[1];
    view.left=tanf(eye.fov.angleLeft);view.right=tanf(eye.fov.angleRight);view.down=tanf(eye.fov.angleDown);view.up=tanf(eye.fov.angleUp);return view;
}
extern "C" int zsharp_game_xr_draw(ZSharpGameXR *x,ZSharpGameVulkan *shared,const ZSharpXRState *state,const ZSharpGameRenderFrame *frame,char *error,size_t cap){
    x->hovered_button[0]=x->hovered_button[1]=0;
    if(!x->begun)return 1;
    XrSwapchain acquired=XR_NULL_HANDLE;
    try{
        XrCompositionLayerProjectionView projections[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        bool render=x->frame.shouldRender&&x->views_valid;
        WorldCollector collector{&x->world,{scene_view(x->views[0],*state),scene_view(x->views[1],*state)}};
        if(render) {
            auto lit_frame=*frame;XrVector3f head{state->head.position[0],state->head.position[1],state->head.position[2]};
            head=euler(head,0,state->rig_rotation[1],0);
            lit_frame.camera_x=head.x+state->rig_position[0];lit_frame.camera_y=head.y+state->rig_position[1];lit_frame.camera_z=head.z+state->rig_position[2];
            x->world.clear();
            if(!zsharp_game_vulkan_world_filtered(shared,&lit_frame,collect_world,visible_world,&collector,error,cap))throw std::runtime_error(error);
            bool has_ui=false;
            for(size_t i=0;i<frame->object_count;i++)if(frame->objects[i].shape==ZGAME_SHAPE_BUTTON&&
                frame->objects[i].visible&&frame->objects[i].opacity>0){has_ui=true;break;}
            if(has_ui&&state->active&&state->focused) {
                ZSharpXRUIRay rays[2]{};float ends[2]{};bool valid[2]{};
                for(int hand=0;hand<2;hand++) {
                    const auto &input=state->hands[hand];
                    valid[hand]=input.aim_tracked&&zsharp_xr_ui_ray_from_pose(&rays[hand],input.aim_position,
                        input.aim_orientation,state->rig_position,state->rig_rotation[1],10.f);
                    if(valid[hand])x->hovered_button[hand]=XRUIWorld::pick(rays[hand],*frame,x->world,ends[hand]);
                }
                /* Pick both before adding either laser; pointers never occlude
                   each other or consume the script tasks' input edge cursors. */
                for(int hand=0;hand<2;hand++)if(valid[hand])
                    XRUIWorld::add_pointer(x->world,rays[hand],ends[hand],hand,x->hovered_button[hand]!=0);
            }
        }
        for(int i=0;render&&i<2;i++){
            Eye &e=x->eyes[i];uint32_t image=0;XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            check(xrAcquireSwapchainImage(e.swap,&acquire,&image),"xrAcquireSwapchainImage");
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;check(xrWaitSwapchainImage(e.swap,&wait),"xrWaitSwapchainImage");
            acquired=e.swap;
            // Typed views are created once for the runtime's typeless images.
            auto target=e.targets[image].Get();
            float clear[]={((frame->background>>16)&255)/255.0f,((frame->background>>8)&255)/255.0f,(frame->background&255)/255.0f,1};
            x->context->ClearRenderTargetView(target,clear);x->context->ClearDepthStencilView(e.depth.Get(),D3D11_CLEAR_DEPTH,1,0);
            ID3D11RenderTargetView *targets[]={target};x->context->OMSetRenderTargets(1,targets,e.depth.Get());
            D3D11_VIEWPORT viewport{0,0,(float)e.width,(float)e.height,0,1};x->context->RSSetViewports(1,&viewport);
            x->graphics->draw(x->world,collector.views[i]);
            x->context->OMSetRenderTargets(0,nullptr,nullptr);x->context->Flush();
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};check(xrReleaseSwapchainImage(e.swap,&release),"xrReleaseSwapchainImage");
            acquired=XR_NULL_HANDLE;
            projections[i].pose=x->views[i].pose;projections[i].fov=x->views[i].fov;projections[i].subImage.swapchain=e.swap;projections[i].subImage.imageRect.extent={(int32_t)e.width,(int32_t)e.height};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.space=x->space;layer.viewCount=2;layer.views=projections;
        const XrCompositionLayerBaseHeader *layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=x->frame.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=render?1u:0u;end.layers=render?layers:nullptr;
        check(xrEndFrame(x->session,&end),"xrEndFrame");x->begun=false;return 1;
    }catch(const std::exception &e){
        if(acquired){XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(acquired,&release);}
        if(x->begun){XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=x->frame.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;xrEndFrame(x->session,&end);x->begun=false;}
        snprintf(error,cap,"%s",e.what());return 0;
    }
}
extern "C" size_t zsharp_game_xr_hovered_button(const ZSharpGameXR *x,int hand) {
    return x&&hand>=0&&hand<2?x->hovered_button[hand]:0;
}
extern "C" void zsharp_game_xr_destroy(ZSharpGameXR *x){
    if(!x)return;
    if(x->begun){XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=x->frame.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;xrEndFrame(x->session,&end);}
    if(x->context){x->context->ClearState();x->context->Flush();}
    for(auto &e:x->eyes){e.targets.clear();if(e.swap)xrDestroySwapchain(e.swap);}
    for(auto h:x->hand)if(h)xrDestroySpace(h);
    for(auto a:x->aim_space)if(a)xrDestroySpace(a);
    x->bare_hands.shutdown();
    if(x->head)xrDestroySpace(x->head);if(x->space)xrDestroySpace(x->space);
    if(x->session)xrDestroySession(x->session);if(x->actions)xrDestroyActionSet(x->actions);if(x->instance)xrDestroyInstance(x->instance);
    delete x;
}
