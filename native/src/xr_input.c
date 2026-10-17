#include "xr_input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int axis(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(name, prefix, n) && strlen(name) == n + 1 &&
        name[n] >= 'X' && name[n] <= 'Z' ? name[n] - 'X' : -1;
}
static int button_id(const char *part, int hand) {
    static const char *names[] = {"trigger", "grip", "stick", "a", "b", "x", "y"};
    int i;
    for (i = 0; i < ZXR_BUTTONS; i++) if (!strcmp(part, names[i])) {
        if ((hand == 0 && (i == ZXR_A || i == ZXR_B)) ||
            (hand == 1 && (i == ZXR_X || i == ZXR_Y))) return -1;
        return i;
    }
    return -1;
}
static int split(const char *path, char *part, const char **field) {
    const char *dot; size_t n;
    if (!path || strncmp(path,"XR.",3)) return 0;
    path += 3; dot = strchr(path,'.');
    n = dot ? (size_t)(dot-path) : strlen(path);
    if (n >= 32) return 0;
    memcpy(part,path,n); part[n]=0; *field=dot ? dot+1 : ""; return 1;
}
int zsharp_xr_property(const char *path, int writing) {
    char part[32], control[32]; const char *field,*state; int hand,id;
    if (!split(path,part,&field)) return 0;
    if (!*field) return !writing && (!strcmp(part,"available") || !strcmp(part,"active") || !strcmp(part,"focused"));
    if (!strcmp(part,"hands")) return !writing && !strcmp(field,"available");
    if (!strcmp(part,"rig")) return axis(field,"position") >= 0 || axis(field,"rotation") == 1;
    if (!strcmp(part,"head")) return !writing && (!strcmp(field,"tracked") || axis(field,"position") >= 0 || axis(field,"rotation") >= 0);
    hand = !strcmp(part,"left") ? 0 : !strcmp(part,"right") ? 1 : -1;
    if (hand < 0) return 0;
    if (!strncmp(field,"hand.",5)) return !writing && zsharp_xr_hand_property(field+5);
    if (writing) return !strcmp(field,"vibrate");
    if (!strcmp(field,"tracked") || axis(field,"position") >= 0 || axis(field,"rotation") >= 0) return 1;
    state=strchr(field,'.'); if (!state || (size_t)(state-field)>=sizeof(control)) return 0;
    memcpy(control,field,(size_t)(state-field));control[state-field]=0;state++;
    id=button_id(control,hand);if(id<0)return 0;
    if(!strcmp(state,"held") || !strcmp(state,"pressed") || !strcmp(state,"released"))return 1;
    if(id==ZXR_STICK)return !strcmp(state,"x") || !strcmp(state,"y") || !strcmp(state,"angle") || !strcmp(state,"magnitude");
    return (id==ZXR_TRIGGER || id==ZXR_GRIP) && !strcmp(state,"value");
}
void zsharp_xr_cursor_begin(ZSharpXRCursor *cursor,const ZSharpXRState *s) {
    if(!cursor || !s)return;
    int h;for(h=0;h<2;h++){memcpy(cursor->pressed[h],s->hands[h].pressed,sizeof(cursor->pressed[h]));memcpy(cursor->released[h],s->hands[h].released,sizeof(cursor->released[h]));zsharp_xr_hand_cursor_begin(&cursor->hands[h],&s->hands[h].hand);}
}
void zsharp_xr_button(ZSharpXRHand *hand,int button,int held) {
    if(!hand || button<0 || button>=ZXR_BUTTONS)return;
    held=!!held;
    if(hand->held[button]!=held){hand->held[button]=held;if(held)hand->pressed[button]++;else hand->released[button]++;}
}
void zsharp_xr_orientation(const float q[4],float degrees[3]) {
    const double x=q[0],y=q[1],z=q[2],w=q[3],rad=180.0/3.141592653589793;
    /* Rz * Ry * Rx: same local-to-world order as scene objects. */
    degrees[0]=(float)(atan2(2*(w*x+y*z),1-2*(x*x+y*y))*rad);
    degrees[1]=(float)(asin(fmax(-1,fmin(1,2*(w*y-z*x))))*rad);
    degrees[2]=(float)(atan2(2*(w*z+x*y),1-2*(y*y+z*z))*rad);
}
int zsharp_xr_read(const ZSharpXRState *s,ZSharpXRCursor *cursor,const char *path,char *out,size_t cap,int *status) {
    char part[32],control[32];const char *field,*state;const ZSharpXRPose *pose=NULL;
    double value=0;int h=-1,a,id;
    if(!s || !out || !cap || !status || !zsharp_xr_property(path,0) || !split(path,part,&field))return 0;
    *status=0;
    if(!*field){*status=1;value=!strcmp(part,"available")?s->available:!strcmp(part,"active")?s->active:s->focused;}
    else if(!strcmp(part,"hands")){*status=1;value=s->hand_tracking_available;}
    else if(!strcmp(part,"rig")){a=axis(field,"position");value=a>=0?s->rig_position[a]:s->rig_rotation[1];}
    else {
        if(!strcmp(part,"head"))pose=&s->head;
        else {h=!strcmp(part,"left")?0:1;pose=&s->hands[h].pose;}
        if(h>=0 && !strncmp(field,"hand.",5))return zsharp_xr_hand_read(&s->hands[h].hand,cursor?&cursor->hands[h]:NULL,field+5,out,cap,status);
        if(!strcmp(field,"tracked")){*status=1;value=pose->tracked;}
        else if((a=axis(field,"position"))>=0)value=pose->position[a];
        else if((a=axis(field,"rotation"))>=0)value=pose->rotation[a];
        else {
            const ZSharpXRHand *hand=&s->hands[h];state=strchr(field,'.');
            memcpy(control,field,(size_t)(state-field));control[state-field]=0;state++;
            id=button_id(control,h);
            if(!strcmp(state,"held")){*status=1;value=hand->held[id];}
            else if(!strcmp(state,"pressed") || !strcmp(state,"released")) {
                if(!cursor)return 0;
                int press=!strcmp(state,"pressed");uint64_t *seen=press?&cursor->pressed[h][id]:&cursor->released[h][id];
                uint64_t count=press?hand->pressed[id]:hand->released[id];
                *status=1;value=count>*seen;if(value)(*seen)++;
            }else if(id==ZXR_TRIGGER)value=hand->trigger;
            else if(id==ZXR_GRIP)value=hand->grip;
            else if(!strcmp(state,"x"))value=hand->stick[0];
            else if(!strcmp(state,"y"))value=hand->stick[1];
            else if(!strcmp(state,"magnitude"))value=fmin(1.0,hypot(hand->stick[0],hand->stick[1]));
            else if(hand->stick[0] || hand->stick[1]) {value=atan2(hand->stick[0],hand->stick[1])*180.0/3.141592653589793;if(value<0)value+=360;}
        }
    }
    if(*status)snprintf(out,cap,"%s",value?"alive":"dead");else snprintf(out,cap,"%.9f",value);
    return 1;
}
int zsharp_xr_write(ZSharpXRState *s,const char *path,const char *value,char *error,size_t cap) {
    char part[32];const char *field;char *end;double n;int a;
    if(!value)value="";
    if(!s || !zsharp_xr_property(path,1) || !split(path,part,&field)){snprintf(error,cap,"XR property is read-only or unknown: %s",path?path:"(null)");return 0;}
    n=strtod(value,&end);
    if(!strcmp(part,"rig")) {
        if(end==value || *end || !isfinite(n) || fabs(n)>1000000){snprintf(error,cap,"XR rig transform requires a finite number within +/-1000000");return 0;}
        a=axis(field,"position");if(a>=0)s->rig_position[a]=(float)n;else s->rig_rotation[1]=(float)fmod(n,360.0);return 1;
    }else {
        double ms;int h=!strcmp(part,"left")?0:1;
        const char *duration;
        if(end==value || *end!='|'){snprintf(error,cap,"XR vibration requires strength and duration");return 0;}
        duration=end+1;ms=strtod(duration,&end);
        if(end==duration || *end || !isfinite(n)||!isfinite(ms)||n<0||n>100||ms<0||ms>10000){snprintf(error,cap,"XR vibration strength must be 0..100 and duration 0..10000ms");return 0;}
        s->vibration[h]=(float)(n/100);s->vibration_ms[h]=(float)ms;s->vibration_serial[h]++;return 1;
    }
}
