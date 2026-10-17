#ifndef ZSHARP_XR_PASSTHROUGH_H
#define ZSHARP_XR_PASSTHROUGH_H

/* Deferred MR experiment. Not compiled, requested or exposed by the current
   PCVR release; retained here only for a future, separately validated update. */
#if 0
#include <openxr/openxr.h>

/* Optional runtime-composited passthrough. Camera pixels never enter the VM.
   A failure disables this optional layer, not the existing VR session. */
class XRPassthroughBackend {
public:
    struct Functions {
        PFN_xrCreatePassthroughFB create{};
        PFN_xrDestroyPassthroughFB destroy{};
        PFN_xrPassthroughStartFB start{};
        PFN_xrPassthroughPauseFB pause{};
        PFN_xrCreatePassthroughLayerFB create_layer{};
        PFN_xrDestroyPassthroughLayerFB destroy_layer{};
        PFN_xrPassthroughLayerResumeFB resume_layer{};
        PFN_xrPassthroughLayerPauseFB pause_layer{};
    };
private:
    Functions functions{};
    XrSession session{};
    XrPassthroughFB feature{};
    XrPassthroughLayerFB layer{};
    bool running=false,available_=false,blocked=false;

    void pause() {
        if(!running)return;
        functions.pause_layer(layer);functions.pause(feature);running=false;
    }
    void fail() { shutdown(); }
public:
    XRPassthroughBackend()=default;
    XRPassthroughBackend(const XRPassthroughBackend &)=delete;
    XRPassthroughBackend &operator=(const XRPassthroughBackend &)=delete;
    ~XRPassthroughBackend(){shutdown();}

    /* The explicit function table also allows deterministic tests without a
       connected headset, while production resolves the same entry points. */
    bool initialize(XrSession new_session,const Functions &entry) {
        shutdown();functions=entry;session=new_session;
        if(!session||!functions.create||!functions.destroy||!functions.start||
            !functions.pause||!functions.create_layer||!functions.destroy_layer||
            !functions.resume_layer||!functions.pause_layer)return false;
        XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        if(XR_FAILED(functions.create(session,&info,&feature))){fail();return false;}
        XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        layer_info.passthrough=feature;
        layer_info.purpose=XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        if(XR_FAILED(functions.create_layer(session,&layer_info,&layer))){fail();return false;}
        available_=true;return true;
    }
    bool init(XrInstance instance,XrSession new_session,XrSystemId system,bool extension_enabled) {
        shutdown();if(!extension_enabled)return false;
        XrSystemPassthroughPropertiesFB support{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};properties.next=&support;
        if(XR_FAILED(xrGetSystemProperties(instance,system,&properties))||!support.supportsPassthrough)return false;
        Functions entry{};
#define ZXR_PASSTHROUGH_LOAD(member,name) \
        if(XR_FAILED(xrGetInstanceProcAddr(instance,name,reinterpret_cast<PFN_xrVoidFunction *>(&entry.member))))return false
        ZXR_PASSTHROUGH_LOAD(create,"xrCreatePassthroughFB");
        ZXR_PASSTHROUGH_LOAD(destroy,"xrDestroyPassthroughFB");
        ZXR_PASSTHROUGH_LOAD(start,"xrPassthroughStartFB");
        ZXR_PASSTHROUGH_LOAD(pause,"xrPassthroughPauseFB");
        ZXR_PASSTHROUGH_LOAD(create_layer,"xrCreatePassthroughLayerFB");
        ZXR_PASSTHROUGH_LOAD(destroy_layer,"xrDestroyPassthroughLayerFB");
        ZXR_PASSTHROUGH_LOAD(resume_layer,"xrPassthroughLayerResumeFB");
        ZXR_PASSTHROUGH_LOAD(pause_layer,"xrPassthroughLayerPauseFB");
#undef ZXR_PASSTHROUGH_LOAD
        return initialize(new_session,entry);
    }
    void update(bool requested,bool session_running,bool focused) {
        bool wanted=requested&&session_running&&focused&&available_&&!blocked;
        if(!wanted){pause();return;}
        if(running)return;
        if(XR_FAILED(functions.start(feature))){fail();return;}
        if(XR_FAILED(functions.resume_layer(layer))){functions.pause(feature);fail();return;}
        running=true;
    }
    void state_changed(XrPassthroughStateChangedFlagsFB flags) {
        if(flags&XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB){fail();return;}
        if(flags&XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB) {
            auto entry=functions;auto previous=session;initialize(previous,entry);
        }
        if(flags&XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB){blocked=true;pause();}
        if(flags&XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB)blocked=false;
    }
    bool available()const{return available_;}
    bool active()const{return running;}
    XrCompositionLayerPassthroughFB composition()const {
        XrCompositionLayerPassthroughFB result{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        result.flags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        result.space=XR_NULL_HANDLE;result.layerHandle=layer;return result;
    }
    void shutdown() {
        pause();
        if(layer&&functions.destroy_layer)functions.destroy_layer(layer);
        if(feature&&functions.destroy)functions.destroy(feature);
        layer=XR_NULL_HANDLE;feature=XR_NULL_HANDLE;session=XR_NULL_HANDLE;
        available_=false;blocked=false;
    }
};
#endif /* deferred MR experiment */
#endif
