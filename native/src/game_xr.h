#ifndef ZSHARP_GAME_XR_H
#define ZSHARP_GAME_XR_H
#include "xr_input.h"
#include "game_vulkan.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ZSharpGameXR ZSharpGameXR;
ZSharpGameXR *zsharp_game_xr_create(char *error,size_t capacity);
/* begin performs predicted-time tracking/input; draw closes that frame. */
int zsharp_game_xr_begin(ZSharpGameXR *,ZSharpXRState *,char *error,size_t capacity);
int zsharp_game_xr_draw(ZSharpGameXR *,ZSharpGameVulkan *,const ZSharpXRState *,const ZSharpGameRenderFrame *,char *error,size_t capacity);
/* Hover from the most recently rendered frame, as that frame's render_id. */
size_t zsharp_game_xr_hovered_button(const ZSharpGameXR *,int hand);
void zsharp_game_xr_destroy(ZSharpGameXR *);
#ifdef __cplusplus
}
#endif
#endif
