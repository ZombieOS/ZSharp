#ifndef ZSHARP_XR_UI_WORLD_H
#define ZSHARP_XR_UI_WORLD_H

#include "xr_ui.h"
#include <algorithm>
#include <cmath>
#include <vector>

/* Interaction tests the same world triangles submitted to both eyes, not
   screen-space bounds or physics-only colliders. No GPU readback is needed. */
namespace XRUIWorld {
inline bool intersection(const ZSharpXRUIRay &ray,
    const ZSharpGameWorldTriangle &triangle,float &distance,float &u,float &v) {
    double edge1[3],edge2[3],p[3],t[3],q[3];
    for(int a=0;a<3;a++) {
        edge1[a]=triangle.points[1][a]-triangle.points[0][a];
        edge2[a]=triangle.points[2][a]-triangle.points[0][a];
        t[a]=ray.origin[a]-triangle.points[0][a];
    }
    p[0]=ray.direction[1]*edge2[2]-ray.direction[2]*edge2[1];
    p[1]=ray.direction[2]*edge2[0]-ray.direction[0]*edge2[2];
    p[2]=ray.direction[0]*edge2[1]-ray.direction[1]*edge2[0];
    double det=edge1[0]*p[0]+edge1[1]*p[1]+edge1[2]*p[2];
    if(!std::isfinite(det)||std::abs(det)<1e-12)return false;
    double inv=1/det,bu=(t[0]*p[0]+t[1]*p[1]+t[2]*p[2])*inv;
    if(bu<0||bu>1)return false;
    q[0]=t[1]*edge1[2]-t[2]*edge1[1];
    q[1]=t[2]*edge1[0]-t[0]*edge1[2];
    q[2]=t[0]*edge1[1]-t[1]*edge1[0];
    double bv=(ray.direction[0]*q[0]+ray.direction[1]*q[1]+ray.direction[2]*q[2])*inv;
    if(bv<0||bu+bv>1)return false;
    double d=(edge2[0]*q[0]+edge2[1]*q[1]+edge2[2]*q[2])*inv;
    if(!std::isfinite(d)||d<0||d>ray.max_distance)return false;
    distance=(float)d;u=(float)bu;v=(float)bv;return true;
}

inline bool opaque_at(const ZSharpGameWorldTriangle &t,float u,float v) {
    float alpha=t.color[3];
    if(t.pixels&&t.texture_width>0&&t.texture_height>0&&
        t.texture_pitch>=t.texture_width*4) {
        float x=t.uv[0][0]*(1-u-v)+t.uv[1][0]*u+t.uv[2][0]*v;
        float y=t.uv[0][1]*(1-u-v)+t.uv[1][1]*u+t.uv[2][1]*v;
        if(!std::isfinite(x)||!std::isfinite(y))return true;
        int px=(int)(std::clamp(x,0.f,1.f)*(t.texture_width-1)+.5f);
        int py=(int)(std::clamp(y,0.f,1.f)*(t.texture_height-1)+.5f);
        alpha*=t.pixels[(size_t)py*t.texture_pitch+(size_t)px*4+3]/255.f;
    }
    /* Solid surfaces block activation. Nearly clear decorative glass and
       transparent glyph/texture holes do not create invisible click walls. */
    return std::isfinite(alpha)&&alpha>=.5f;
}

inline float nearest_surface(const ZSharpXRUIRay &ray,
    const std::vector<ZSharpGameWorldTriangle> &world,size_t ignored_button_id=0,
    float *visible_surface=nullptr) {
    float nearest=ray.max_distance,visible=ray.max_distance;
    for(const auto &triangle:world) {
        const bool own=ignored_button_id&&triangle.ui_button_id==ignored_button_id;
        if(own&&!visible_surface)continue;
        float distance,u,v;
        if(intersection(ray,triangle,distance,u,v)&&
            (distance<visible||(!own&&distance<nearest))&&opaque_at(triangle,u,v)) {
            visible=std::min(visible,distance);
            if(!own)nearest=std::min(nearest,distance);
        }
    }
    if(visible_surface)*visible_surface=visible;
    return nearest;
}

inline size_t pick(const ZSharpXRUIRay &ray,const ZSharpGameRenderFrame &frame,
    const std::vector<ZSharpGameWorldTriangle> &world,float &endpoint) {
    ZSharpXRUIHit hit{};bool found=false;
    endpoint=ray.max_distance;
    for(size_t index=0;index<frame.object_count;index++) {
        if(frame.objects[index].shape!=ZGAME_SHAPE_BUTTON)continue;
        auto button=frame.objects[index];
        if(button.model_index) {
            /* :hover scaling must not move the activation boundary under a
               stationary pointer and cause an enter/leave feedback loop. */
            button.scale_x=button.ui_base_scale_x;button.scale_y=button.ui_base_scale_y;
        }
        ZSharpXRUIHit candidate{};
        if(zsharp_xr_ui_hit_button(&ray,&button,&candidate)&&
            (!found||candidate.distance<hit.distance||
             (candidate.distance==hit.distance&&candidate.render_id<hit.render_id))) {
            hit=candidate;found=true;
        }
    }
    if(!found){endpoint=nearest_surface(ray,world);return 0;}
    /* A button's glyphs sit just above its face. At grazing angles their
       separation along the ray can be arbitrarily larger than the normal
       offset, so a global distance tolerance cannot distinguish self-glyphs
       from a real wall. Ignore only this button's own triangles when checking
       blockers. Keep the original visible-surface endpoint for the laser. */
    if(nearest_surface(ray,world,hit.render_id,&endpoint)<hit.distance)return 0;
    endpoint=std::min(endpoint,hit.distance);
    return hit.render_id;
}

inline void add_pointer(std::vector<ZSharpGameWorldTriangle> &world,
    const ZSharpXRUIRay &ray,float distance,int hand,bool hover) {
    float end[3],u[3]={-ray.direction[2],0,ray.direction[0]},v[3];
    float length=std::hypot(u[0],u[2]);
    if(length<.001f){u[0]=1;u[1]=u[2]=0;}else for(float &a:u)a/=length;
    v[0]=ray.direction[1]*u[2]-ray.direction[2]*u[1];
    v[1]=ray.direction[2]*u[0]-ray.direction[0]*u[2];
    v[2]=ray.direction[0]*u[1]-ray.direction[1]*u[0];
    distance=std::max(0.f,distance-.0015f);
    for(int a=0;a<3;a++)end[a]=ray.origin[a]+ray.direction[a]*distance;
    auto quad=[&](float points[4][3]) {
        constexpr int indices[2][3]={{0,1,2},{0,2,3}};
        for(const auto &index:indices) {
            ZSharpGameWorldTriangle t{};
            for(int c=0;c<3;c++)for(int a=0;a<3;a++)t.points[c][a]=points[index[c]][a];
            t.color[0]=hover?1.f:(hand?.4f:.25f);t.color[1]=hover?1.f:.8f;
            t.color[2]=hover?.4f:(hand?.6f:1.f);t.color[3]=1;
            world.push_back(t);
        }
    };
    for(const auto *axis:{u,v}) {
        float points[4][3];
        for(int a=0;a<3;a++) {
            points[0][a]=ray.origin[a]-axis[a]*.001f;
            points[1][a]=ray.origin[a]+axis[a]*.001f;
            points[2][a]=end[a]+axis[a]*.001f;
            points[3][a]=end[a]-axis[a]*.001f;
        }
        quad(points);
    }
    float dot[4][3];
    for(int c=0;c<4;c++)for(int a=0;a<3;a++)
        dot[c][a]=end[a]+(c<2?-1:1)*u[a]*.005f+((c==0||c==3)?-1:1)*v[a]*.005f;
    quad(dot);
}
}
#endif
