#ifndef ZSHARP_XR_INPUT_H
#define ZSHARP_XR_INPUT_H
#include <stddef.h>
#include <stdint.h>
#include "xr_hands.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Backend-independent tracked-space snapshot. Distances are meters. */
typedef struct ZSharpXRPose { int tracked; float position[3], rotation[3]; } ZSharpXRPose;
enum { ZXR_TRIGGER, ZXR_GRIP, ZXR_STICK, ZXR_A, ZXR_B, ZXR_X, ZXR_Y, ZXR_BUTTONS };
typedef struct ZSharpXRHand {
    ZSharpXRPose pose;
    /* OpenXR controller aim pose for internal world-space UI. Grip pose above
       remains the public XR.left/right transform; this is not hand tracking. */
    int aim_tracked;
    float aim_position[3], aim_orientation[4];
    float trigger, grip, stick[2];
    int held[ZXR_BUTTONS];
    uint64_t pressed[ZXR_BUTTONS], released[ZXR_BUTTONS];
    ZSharpXRTrackedHand hand;
} ZSharpXRHand;
typedef struct ZSharpXRState {
    int available, active, focused;
    int hand_tracking_available;
    ZSharpXRPose head;
    ZSharpXRHand hands[2];
    float rig_position[3], rig_rotation[3];
    float vibration[2], vibration_ms[2];
    unsigned vibration_serial[2];
} ZSharpXRState;
typedef struct ZSharpXRCursor {
    uint64_t pressed[2][ZXR_BUTTONS], released[2][ZXR_BUTTONS];
    ZSharpXRHandCursor hands[2];
} ZSharpXRCursor;
void zsharp_xr_cursor_begin(ZSharpXRCursor *, const ZSharpXRState *);
void zsharp_xr_button(ZSharpXRHand *, int button, int held);
void zsharp_xr_orientation(const float quaternion[4], float degrees[3]);
int zsharp_xr_property(const char *path, int writing);
int zsharp_xr_read(const ZSharpXRState *, ZSharpXRCursor *, const char *path,
                   char *output, size_t capacity, int *status);
int zsharp_xr_write(ZSharpXRState *, const char *path, const char *value,
                    char *error, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
