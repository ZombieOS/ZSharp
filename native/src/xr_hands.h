#ifndef ZSHARP_XR_HANDS_H
#define ZSHARP_XR_HANDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OpenXR's standard 26-joint ordering. Positions are tracked-space metres;
   rotations use the same XYZ degrees as existing controller/head poses. */
enum {
    ZXR_HAND_PALM, ZXR_HAND_WRIST,
    ZXR_HAND_THUMB_METACARPAL, ZXR_HAND_THUMB_PROXIMAL,
    ZXR_HAND_THUMB_DISTAL, ZXR_HAND_THUMB_TIP,
    ZXR_HAND_INDEX_METACARPAL, ZXR_HAND_INDEX_PROXIMAL,
    ZXR_HAND_INDEX_INTERMEDIATE, ZXR_HAND_INDEX_DISTAL, ZXR_HAND_INDEX_TIP,
    ZXR_HAND_MIDDLE_METACARPAL, ZXR_HAND_MIDDLE_PROXIMAL,
    ZXR_HAND_MIDDLE_INTERMEDIATE, ZXR_HAND_MIDDLE_DISTAL, ZXR_HAND_MIDDLE_TIP,
    ZXR_HAND_RING_METACARPAL, ZXR_HAND_RING_PROXIMAL,
    ZXR_HAND_RING_INTERMEDIATE, ZXR_HAND_RING_DISTAL, ZXR_HAND_RING_TIP,
    ZXR_HAND_LITTLE_METACARPAL, ZXR_HAND_LITTLE_PROXIMAL,
    ZXR_HAND_LITTLE_INTERMEDIATE, ZXR_HAND_LITTLE_DISTAL, ZXR_HAND_LITTLE_TIP,
    ZXR_HAND_JOINTS
};

typedef struct ZSharpXRHandJoint {
    int tracked;
    float position[3], rotation[3];
    float radius;
} ZSharpXRHandJoint;

typedef struct ZSharpXRTrackedHand {
    int tracked;
    ZSharpXRHandJoint joints[ZXR_HAND_JOINTS];
    float pinch_value;
    int pinch_held, pinch_armed, pinch_tracking;
    uint64_t pinch_pressed, pinch_released;
    uint64_t generation, press_floor, release_floor;
} ZSharpXRTrackedHand;

typedef struct ZSharpXRHandCursor {
    uint64_t pressed, released, generation;
} ZSharpXRHandCursor;

/* Invalid/missing joints retain their last pose but become untracked. Pinch
   needs both tip joints: <=2cm presses, >=4cm releases, 6cm maps to value 0.
   Initial/reacquired tracking must observe an open pinch before it can press. */
void zsharp_xr_hand_update(ZSharpXRTrackedHand *hand,
                           const ZSharpXRHandJoint joints[ZXR_HAND_JOINTS],
                           int tracked, int focused);
void zsharp_xr_hand_cursor_begin(ZSharpXRHandCursor *cursor,
                                 const ZSharpXRTrackedHand *hand);
/* Path is relative to XR.left/right.hand. */
int zsharp_xr_hand_property(const char *path);
int zsharp_xr_hand_read(const ZSharpXRTrackedHand *hand,
                        ZSharpXRHandCursor *cursor, const char *path,
                        char *output, size_t capacity, int *status);

#ifdef __cplusplus
}
#endif
#endif
