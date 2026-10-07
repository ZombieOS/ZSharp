#include "style_transition.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int zsharp_style_transition_parse(const char *text, double *seconds, int *easing) {
    char property[32], duration[48], timing[32] = "ease", extra[2];
    char *end;
    double value;
    int tokens;
    if (text == NULL) return 0;
    if (strcmp(text,"none")==0) { *seconds=0;*easing=0;return 1; }
    tokens=sscanf(text,"%31s %47s %31s %1s",property,duration,timing,extra);
    if(tokens<2 || tokens>3 || strcmp(property,"all")!=0) return 0;
    value=strtod(duration,&end);
    if(end==duration || !isfinite(value) || value<0) return 0;
    if(strcmp(end,"ms")==0)value/=1000;
    else if(strcmp(end,"s")!=0)return 0;
    *easing=strcmp(timing,"linear")==0 ? 0 : strcmp(timing,"ease")==0 ? 1 :
        strcmp(timing,"ease-in")==0 ? 2 : strcmp(timing,"ease-out")==0 ? 3 :
        strcmp(timing,"ease-in-out")==0 ? 4 : -1;
    if(*easing<0)return 0;
    *seconds=value;return 1;
}
/* Evaluate the standard CSS cubic-bezier curves by solving their X component. */
double zsharp_style_ease(double t, int easing) {
    double x1,x2,y1,y2,low=0,high=1,u=t;
    int i;
    if(t<=0)return 0;if(t>=1)return 1;if(easing==0)return t;
    x1=easing==1 ? .25 : easing==3 ? 0 : .42;
    x2=easing==1 ? .25 : easing==2 ? 1 : .58;
    y1=easing==1 ? .1 : 0;y2=1;
    for(i=0;i<24;i++) {
        double v=1-u,x=3*v*v*u*x1+3*v*u*u*x2+u*u*u;
        if(x<t)low=u;else high=u;u=(low+high)/2;
    }
    return 3*(1-u)*(1-u)*u*y1+3*(1-u)*u*u*y2+u*u*u;
}
void zsharp_style_tween(ZSharpStyleTween *tween, const double target[6], double now,
                        double duration, int easing, double output[6]) {
    int i,changed=0;
    double progress;
    if(!tween->initialized) {
        memcpy(tween->from,target,sizeof(tween->from));
        memcpy(tween->target,target,sizeof(tween->target));
        tween->initialized=1;tween->started=now;
    }
    progress=duration>0 ? zsharp_style_ease((now-tween->started)/duration,easing) : 1;
    for(i=0;i<6;i++) {
        output[i]=tween->from[i]+(tween->target[i]-tween->from[i])*progress;
        if(target[i]!=tween->target[i])changed=1;
    }
    if(changed) {
        memcpy(tween->from,output,sizeof(tween->from));
        memcpy(tween->target,target,sizeof(tween->target));
        tween->started=now;
        if(duration<=0)memcpy(output,target,sizeof(tween->target));
    }
}
