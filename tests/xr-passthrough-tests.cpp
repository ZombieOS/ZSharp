/* Deferred with the MR backend. This is not a built or registered release test. */
#if 0
#include "xr_passthrough.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static std::vector<std::string> calls;
static bool fail_layer=false,fail_start=false,fail_resume=false;
static XrResult XRAPI_PTR create(XrSession,const XrPassthroughCreateInfoFB *info,XrPassthroughFB *out) {
    if(info->type!=XR_TYPE_PASSTHROUGH_CREATE_INFO_FB||info->flags)return XR_ERROR_VALIDATION_FAILURE;
    calls.push_back("create");*out=reinterpret_cast<XrPassthroughFB>(uintptr_t(1));return XR_SUCCESS;
}
static XrResult XRAPI_PTR destroy(XrPassthroughFB){calls.push_back("destroy");return XR_SUCCESS;}
static XrResult XRAPI_PTR start(XrPassthroughFB){calls.push_back("start");return fail_start?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;}
static XrResult XRAPI_PTR pause(XrPassthroughFB){calls.push_back("pause");return XR_SUCCESS;}
static XrResult XRAPI_PTR create_layer(XrSession,const XrPassthroughLayerCreateInfoFB *info,XrPassthroughLayerFB *out) {
    calls.push_back("create_layer");
    if(fail_layer)return XR_ERROR_FEATURE_UNSUPPORTED;
    if(info->type!=XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB||info->flags||
        info->purpose!=XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB||!info->passthrough)return XR_ERROR_VALIDATION_FAILURE;
    *out=reinterpret_cast<XrPassthroughLayerFB>(uintptr_t(2));return XR_SUCCESS;
}
static XrResult XRAPI_PTR destroy_layer(XrPassthroughLayerFB){calls.push_back("destroy_layer");return XR_SUCCESS;}
static XrResult XRAPI_PTR resume_layer(XrPassthroughLayerFB){calls.push_back("resume_layer");return fail_resume?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;}
static XrResult XRAPI_PTR pause_layer(XrPassthroughLayerFB){calls.push_back("pause_layer");return XR_SUCCESS;}
#define CHECK(test) do{if(!(test)){std::fprintf(stderr,"FAIL passthrough line %d: %s\n",__LINE__,#test);return 1;}}while(0)
int main() {
    XRPassthroughBackend backend;
    CHECK(!backend.init(XR_NULL_HANDLE,XR_NULL_HANDLE,0,false));
    CHECK(!backend.available()&&!backend.active());
    XRPassthroughBackend::Functions functions{create,destroy,start,pause,create_layer,destroy_layer,resume_layer,pause_layer};
    XrSession session=reinterpret_cast<XrSession>(uintptr_t(3));
    CHECK(backend.initialize(session,functions));CHECK(backend.available()&&!backend.active());
    CHECK((calls==std::vector<std::string>{"create","create_layer"}));
    backend.update(true,false,true);backend.update(true,true,false);CHECK(calls.size()==2);
    backend.update(true,true,true);CHECK(backend.active());CHECK(calls.size()==4);
    backend.update(true,true,true);CHECK(calls.size()==4);
    auto composition=backend.composition();CHECK(composition.type==XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB);
    CHECK(composition.space==XR_NULL_HANDLE&&composition.layerHandle&&
        composition.flags==XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
    backend.update(true,true,false);CHECK(!backend.active()&&backend.available());
    CHECK(calls[calls.size()-2]=="pause_layer"&&calls.back()=="pause");
    backend.update(true,true,true);CHECK(backend.active());
    backend.state_changed(XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB);
    CHECK(!backend.active());backend.update(true,true,true);CHECK(!backend.active());
    backend.state_changed(XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB);
    backend.update(true,true,true);CHECK(backend.active());
    backend.state_changed(XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB);
    CHECK(backend.available()&&!backend.active());backend.update(true,true,true);CHECK(backend.active());
    backend.shutdown();CHECK(!backend.available()&&!backend.active());
    CHECK(calls[calls.size()-2]=="destroy_layer"&&calls.back()=="destroy");
    size_t count=calls.size();backend.shutdown();CHECK(calls.size()==count);
    fail_layer=true;CHECK(!backend.initialize(session,functions));CHECK(calls.back()=="destroy");fail_layer=false;
    CHECK(backend.initialize(session,functions));fail_start=true;backend.update(true,true,true);
    CHECK(!backend.active()&&!backend.available());fail_start=false;
    CHECK(backend.initialize(session,functions));fail_resume=true;backend.update(true,true,true);
    CHECK(!backend.active()&&!backend.available());fail_resume=false;
    CHECK(backend.initialize(session,functions));backend.update(true,true,true);
    backend.state_changed(XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB);
    CHECK(!backend.active()&&!backend.available());
    std::puts("PASS: optional passthrough lifecycle, focus, composition flags, recovery and safe VR fallback");
    return 0;
}
#endif /* deferred MR experiment */
