#include "xr_input.h"
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    ZSharpXRState s={0};ZSharpXRCursor a={0},b={0};char out[128],error[256];int status;
    assert(zsharp_xr_property("XR.head.rotationZ",0));
    assert(!zsharp_xr_property("XR.head.positionX",1));
    assert(!zsharp_xr_property("XR.left.a.pressed",0));
    assert(!zsharp_xr_property("XR.right.x.pressed",0));
    assert(!zsharp_xr_property("XR.left.stick.invalid",0));
    {
        float q[]={0,0.5f,0,0.8660254038f},angles[3];
        zsharp_xr_orientation(q,angles);
        assert(fabs(angles[0])<0.001 && fabs(angles[1]-60)<0.001 && fabs(angles[2])<0.001);
    }
    assert(zsharp_xr_write(&s,"XR.rig.positionX","2.5",error,sizeof(error)));
    assert(s.rig_position[0]==2.5f);
    assert(!zsharp_xr_write(&s,"XR.rig.positionX","nan",error,sizeof(error)));
    assert(zsharp_xr_write(&s,"XR.right.vibrate","75|200",error,sizeof(error)));
    assert(s.vibration[1]==0.75f && s.vibration_ms[1]==200);
    assert(!zsharp_xr_write(&s,"XR.right.vibrate","101|200",error,sizeof(error)));
    zsharp_xr_cursor_begin(&a,&s);zsharp_xr_cursor_begin(&b,&s);
    zsharp_xr_button(&s.hands[1],ZXR_TRIGGER,1);zsharp_xr_button(&s.hands[1],ZXR_TRIGGER,0);
    assert(zsharp_xr_read(&s,&a,"XR.right.trigger.pressed",out,sizeof(out),&status));assert(!strcmp(out,"alive"));
    zsharp_xr_read(&s,&a,"XR.right.trigger.pressed",out,sizeof(out),&status);assert(!strcmp(out,"dead"));
    zsharp_xr_read(&s,&b,"XR.right.trigger.pressed",out,sizeof(out),&status);assert(!strcmp(out,"alive"));
    zsharp_xr_read(&s,&a,"XR.right.trigger.released",out,sizeof(out),&status);assert(!strcmp(out,"alive"));
    zsharp_xr_read(&s,&a,"XR.right.trigger.held",out,sizeof(out),&status);assert(!strcmp(out,"dead"));
    for(int i=0;i<4;i++) {
        s.hands[1].stick[0]=i==1?1:i==3?-1:0;s.hands[1].stick[1]=i==0?1:i==2?-1:0;
        zsharp_xr_read(&s,&a,"XR.right.stick.angle",out,sizeof(out),&status);assert(fabs(atof(out)-i*90)<0.001);
    }
    s.hands[1].stick[0]=0.3f;s.hands[1].stick[1]=0.4f;
    zsharp_xr_read(&s,&a,"XR.right.stick.magnitude",out,sizeof(out),&status);assert(fabs(atof(out)-0.5)<0.00001);
    // Every valid physical button must retain both short edges independently
    // for each task, including multiple queued taps between task iterations.
    for(int hand=0;hand<2;hand++) {
        const char *buttons[]={"trigger","grip","stick",hand?"a":"x",hand?"b":"y"};
        const int slots[]={ZXR_TRIGGER,ZXR_GRIP,ZXR_STICK,hand?ZXR_A:ZXR_X,hand?ZXR_B:ZXR_Y};
        for(int button=0;button<5;button++) {
            char path[128];zsharp_xr_cursor_begin(&a,&s);zsharp_xr_cursor_begin(&b,&s);
            for(int tap=0;tap<2;tap++){zsharp_xr_button(&s.hands[hand],slots[button],1);zsharp_xr_button(&s.hands[hand],slots[button],0);}
            snprintf(path,sizeof(path),"XR.%s.%s.held",hand?"right":"left",buttons[button]);
            assert(zsharp_xr_read(&s,&a,path,out,sizeof(out),&status) && !strcmp(out,"dead"));
            for(int edge=0;edge<2;edge++) {
                snprintf(path,sizeof(path),"XR.%s.%s.%s",hand?"right":"left",buttons[button],edge?"released":"pressed");
                for(int tap=0;tap<2;tap++) {
                    assert(zsharp_xr_read(&s,&a,path,out,sizeof(out),&status) && !strcmp(out,"alive"));
                    assert(zsharp_xr_read(&s,&b,path,out,sizeof(out),&status) && !strcmp(out,"alive"));
                }
                assert(zsharp_xr_read(&s,&a,path,out,sizeof(out),&status) && !strcmp(out,"dead"));
            }
        }
    }
    assert(zsharp_xr_write(&s,"XR.rig.positionY","1.25",error,sizeof(error)));
    assert(zsharp_xr_write(&s,"XR.rig.positionZ","-3",error,sizeof(error)));
    assert(zsharp_xr_write(&s,"XR.rig.rotationY","450",error,sizeof(error)));
    assert(s.rig_position[1]==1.25f && s.rig_position[2]==-3 && s.rig_rotation[1]==90);
    assert(zsharp_xr_write(&s,"XR.left.vibrate","0|0",error,sizeof(error)));
    assert(s.vibration[0]==0 && s.vibration_ms[0]==0);
    puts("PASS: XR property validation, transforms, haptics, latched edges and stick angle/magnitude");
    return 0;
}
