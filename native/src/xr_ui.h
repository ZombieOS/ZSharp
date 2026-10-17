#ifndef ZSHARP_XR_UI_H
#define ZSHARP_XR_UI_H

#include "game_vulkan.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* World-space rays use meters. Arbitrary nonzero finite directions are accepted;
   intersection results and max_distance are physical distances, not ray t. */
typedef struct ZSharpXRUIRay {
    float origin[3];
    float direction[3];
    float max_distance;
} ZSharpXRUIRay;

typedef struct ZSharpXRUIHit {
    size_t object_index; /* SIZE_MAX for a single-object test or a miss */
    size_t render_id;
    float distance;
    float position[3];
    float uv[2]; /* top-left is (0, 0), including mirrored object scales */
} ZSharpXRUIHit;

/* One cursor per controller, independent from Z# task input cursors. Serial
   counters preserve a short press/release occurring between UI updates. */
typedef struct ZSharpXRUIActivation {
    uint64_t pressed_seen;
    int armed;
} ZSharpXRUIActivation;

/* Reset on scene change, session start/end, or equivalent discontinuities.
   An initially held trigger cannot click until a valid release is observed. */
void zsharp_xr_ui_activation_reset(ZSharpXRUIActivation *cursor,
    uint64_t pressed_serial);

/* pointer_valid means tracked + active + focused, not necessarily hovering.
   All edges are consumed, including invalid pointers and presses off buttons.
   A fresh armed press returns hovered_render_id (zero means no button); held
   triggers never repeatedly activate or click by moving onto a new button. */
size_t zsharp_xr_ui_activation_update(ZSharpXRUIActivation *cursor,
    int pointer_valid, int trigger_held, uint64_t pressed_serial,
    size_t hovered_render_id);

/* Convert OpenXR aim pose (-Z forward, quaternion x/y/z/w) from tracking space
   to scene space using the same rig translation/yaw as the stereo renderer.
   The caller must reject untracked/inactive poses before calling. Invalid
   quaternions, nonfinite inputs, and negative ray lengths return zero. */
int zsharp_xr_ui_ray_from_pose(ZSharpXRUIRay *ray,
    const float tracked_position[3], const float quaternion[4],
    const float rig_position[3], float rig_yaw_degrees, float max_distance);

/* Buttons are centered local XY rectangles, transformed by Rz * Ry * Rx with
   Ry = rotation_y + rotation. Both faces are interactive, matching stereo UI
   rendering with backface culling disabled. Hidden/zero-opacity/degenerate
   buttons are not interactive. Outputs are reset on failure. */
int zsharp_xr_ui_hit_button(const ZSharpXRUIRay *ray,
    const ZSharpGameRenderObject *button, ZSharpXRUIHit *hit);

/* Return transformed top-left, top-right, bottom-right, bottom-left corners.
   This is the same surface used by hit testing; scale_z and depth do not affect
   a zero-thickness panel. Mirrored XY scales preserve texture orientation. */
int zsharp_xr_ui_button_corners(const ZSharpGameRenderObject *button,
    float corners[4][3]);

/* Pick the closest visible button only. This does not implement world/collider
   occlusion. Equal-distance overlaps prefer the smaller stable render_id so
   per-frame painter sorting cannot change the selected placement. The caller
   may pass a button-only array to avoid traversing a large scene. */
int zsharp_xr_ui_pick_button(const ZSharpXRUIRay *ray,
    const ZSharpGameRenderObject *objects, size_t object_count,
    ZSharpXRUIHit *hit);

#ifdef __cplusplus
}
#endif
#endif
