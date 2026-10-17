#include "xr_ui.h"
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int near(float actual, float expected) {
    return fabsf(actual - expected) < 0.0001f;
}

static ZSharpGameRenderObject button(void) {
    ZSharpGameRenderObject result;
    memset(&result, 0, sizeof(result));
    result.shape = ZGAME_SHAPE_BUTTON;
    result.render_id = 7;
    result.width = 2; result.height = 1;
    result.scale_x = result.scale_y = result.scale_z = 1;
    result.visible = 1; result.opacity = 1;
    result.z = -3;
    return result;
}

static ZSharpXRUIRay forward_ray(void) {
    ZSharpXRUIRay result = {{0, 0, 0}, {0, 0, -1}, 10};
    return result;
}

static void basic_intersections(void) {
    ZSharpGameRenderObject object = button();
    ZSharpXRUIRay ray = forward_ray();
    ZSharpXRUIHit hit;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(hit.object_index == SIZE_MAX && hit.render_id == 7);
    assert(near(hit.distance, 3) && near(hit.position[2], -3));
    assert(near(hit.uv[0], 0.5f) && near(hit.uv[1], 0.5f));
    assert(zsharp_xr_ui_hit_button(&ray, &object, NULL));
    ray.origin[0] = 0.5f; ray.origin[1] = 0.25f;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.uv[0], 0.75f) && near(hit.uv[1], 0.25f));
    ray.origin[0] = 1; ray.origin[1] = 0.5f;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.uv[0], 1) && near(hit.uv[1], 0));
    ray.origin[0] = 1.001f;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(hit.object_index == SIZE_MAX && hit.render_id == 0);
    ray = forward_ray(); ray.max_distance = 2.9f;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit));
    ray.max_distance = 3;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    ray.direction[2] = -5; /* Non-unit direction still yields meters. */
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.distance, 3));
    ray.origin[2] = -6; ray.direction[2] = 1;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit)); /* Double sided. */
    assert(near(hit.distance, 3));
    ray.direction[2] = -1;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit)); /* Behind ray. */
    ray.origin[2] = -3; ray.max_distance = 0;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(hit.distance == 0);
    ray.direction[2] = 0; ray.direction[0] = 1;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit)); /* Coplanar. */
    ray.origin[2] = 0;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit)); /* Parallel. */
}

static void transforms_and_corners(void) {
    ZSharpGameRenderObject object = button();
    ZSharpXRUIRay ray = forward_ray();
    ZSharpXRUIHit hit;
    float corners[4][3];
    assert(zsharp_xr_ui_button_corners(&object, corners));
    assert(near(corners[0][0], -1) && near(corners[0][1], 0.5f));
    assert(near(corners[2][0], 1) && near(corners[2][1], -0.5f));
    object.scale_x = -2; object.scale_y = -3;
    ray.origin[0] = 1; ray.origin[1] = 0.75f;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.uv[0], 0.25f) && near(hit.uv[1], 0.75f));
    assert(zsharp_xr_ui_button_corners(&object, corners));
    assert(near(corners[0][0], 2) && near(corners[0][1], -1.5f));
    object = button(); object.rotation_z = 90;
    assert(zsharp_xr_ui_button_corners(&object, corners));
    assert(near(corners[0][0], -0.5f) && near(corners[0][1], -1));
    ray = forward_ray(); ray.origin[1] = 0.9f;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    ray.origin[0] = 0.6f;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit));
    object = button(); object.x = -3; object.z = 0;
    object.rotation_y = 35; object.rotation = 55;
    ray = forward_ray(); ray.direction[0] = -1; ray.direction[2] = 0;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.distance, 3));
    object = button(); object.y = 3; object.z = 0; object.rotation_x = 90;
    ray = forward_ray(); ray.direction[1] = 1; ray.direction[2] = 0;
    assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
    assert(near(hit.distance, 3));
    /* Compound transforms must agree with the rendered quad, including mirrors.
       Build a point at UV (0.2, 0.7), and approach it along the quad normal. */
    for (int rotation = 0; rotation < 40; ++rotation) {
        double along[3], down[3], normal[3], length = 0;
        object = button();
        object.x = 4; object.y = 2; object.z = -7;
        object.rotation_x = (float)rotation * 11;
        object.rotation_y = (float)rotation * 19;
        object.rotation_z = (float)rotation * 7;
        object.rotation = 13;
        object.scale_x = rotation % 2 ? -2 : 2;
        object.scale_y = rotation % 3 ? -3 : 3;
        assert(zsharp_xr_ui_button_corners(&object, corners));
        for (int axis = 0; axis < 3; ++axis) {
            along[axis] = (double)corners[1][axis] - corners[0][axis];
            down[axis] = (double)corners[3][axis] - corners[0][axis];
        }
        normal[0] = along[1] * down[2] - along[2] * down[1];
        normal[1] = along[2] * down[0] - along[0] * down[2];
        normal[2] = along[0] * down[1] - along[1] * down[0];
        for (int axis = 0; axis < 3; ++axis) length += normal[axis] * normal[axis];
        length = sqrt(length);
        ray.max_distance = 10;
        for (int axis = 0; axis < 3; ++axis) {
            normal[axis] /= length;
            ray.origin[axis] = (float)(corners[0][axis] +
                0.2 * along[axis] + 0.7 * down[axis] + 2 * normal[axis]);
            ray.direction[axis] = (float)-normal[axis];
        }
        assert(zsharp_xr_ui_hit_button(&ray, &object, &hit));
        assert(near(hit.distance, 2));
        assert(near(hit.uv[0], 0.2f) && near(hit.uv[1], 0.7f));
    }
}

static void poses(void) {
    const float zero[3] = {0, 0, 0}, identity[4] = {0, 0, 0, 1};
    const float position[3] = {1, 2, 3}, rig[3] = {10, 20, 30};
    const float yaw90[4] = {0, 0.70710678f, 0, 0.70710678f};
    const float pitch90[4] = {0.70710678f, 0, 0, 0.70710678f};
    const float nonunit[4] = {0, 0, 0, 2};
    ZSharpXRUIRay ray;
    assert(zsharp_xr_ui_ray_from_pose(&ray, zero, identity, zero, 0, 8));
    assert(near(ray.direction[2], -1) && ray.max_distance == 8);
    assert(zsharp_xr_ui_ray_from_pose(&ray, position, identity, rig, 90, 8));
    assert(near(ray.origin[0], 13) && near(ray.origin[1], 22) && near(ray.origin[2], 29));
    assert(near(ray.direction[0], -1) && near(ray.direction[2], 0));
    assert(zsharp_xr_ui_ray_from_pose(&ray, zero, yaw90, zero, 0, 8));
    assert(near(ray.direction[0], -1) && near(ray.direction[2], 0));
    assert(zsharp_xr_ui_ray_from_pose(&ray, zero, yaw90, zero, 90, 8));
    assert(near(ray.direction[0], 0) && near(ray.direction[2], 1));
    assert(zsharp_xr_ui_ray_from_pose(&ray, zero, pitch90, zero, 0, 8));
    assert(near(ray.direction[1], 1) && near(ray.direction[2], 0));
    assert(zsharp_xr_ui_ray_from_pose(&ray, zero, nonunit, zero, 0, 8));
    assert(near(ray.direction[2], -1));
    {
        const float extreme[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
        assert(zsharp_xr_ui_ray_from_pose(&ray, zero, extreme, zero, 0, 8));
        assert(near(ray.direction[0], -1) && near(ray.direction[2], 0));
    }
}

static void invalid_inputs(void) {
    ZSharpGameRenderObject object;
    ZSharpXRUIRay ray;
    ZSharpXRUIHit hit;
    float corners[4][3];
    float zero[3] = {0}, quaternion[4] = {0, 0, 0, 1};
    for (int test = 0; test < 17; ++test) {
        object = button(); ray = forward_ray();
        switch (test) {
        case 0: object.visible = 0; break;
        case 1: object.opacity = 0; break;
        case 2: object.opacity = NAN; break;
        case 3: object.shape = ZGAME_SHAPE_CUBE; break;
        case 4: object.scale_x = 0; break;
        case 5: object.height = 0; break;
        case 6: object.width = INFINITY; break;
        case 7: object.x = NAN; break;
        case 8: object.rotation_y = NAN; break;
        case 9: object.rotation = INFINITY; break;
        case 10: ray.origin[1] = NAN; break;
        case 11: ray.direction[1] = INFINITY; break;
        case 12: ray.direction[2] = 0; break;
        case 13: ray.max_distance = -1; break;
        case 14: ray.max_distance = INFINITY; break;
        case 15: ray.direction[2] = -1e-12f; ray.direction[0] = 1; break;
        case 16: object.scale_y = NAN; break;
        }
        assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit));
        assert(hit.render_id == 0 && hit.object_index == SIZE_MAX);
    }
    assert(!zsharp_xr_ui_hit_button(NULL, &object, &hit));
    assert(!zsharp_xr_ui_hit_button(&ray, NULL, &hit));
    assert(!zsharp_xr_ui_button_corners(NULL, corners));
    assert(!zsharp_xr_ui_button_corners(&object, NULL));
    assert(!zsharp_xr_ui_ray_from_pose(&ray, zero, quaternion, zero, 0, -1));
    quaternion[3] = 0;
    assert(!zsharp_xr_ui_ray_from_pose(&ray, zero, quaternion, zero, 0, 10));
    quaternion[3] = NAN;
    assert(!zsharp_xr_ui_ray_from_pose(&ray, zero, quaternion, zero, 0, 10));
    quaternion[3] = 1;
    assert(!zsharp_xr_ui_ray_from_pose(&ray, zero, quaternion, zero, INFINITY, 10));
    assert(!zsharp_xr_ui_ray_from_pose(&ray, NULL, quaternion, zero, 0, 10));
    assert(!zsharp_xr_ui_ray_from_pose(NULL, zero, quaternion, zero, 0, 10));
    {
        float extreme[3] = {FLT_MAX, 0, 0};
        assert(!zsharp_xr_ui_ray_from_pose(&ray, extreme, quaternion, extreme, 0, 10));
    }
    object = button(); object.width = FLT_MAX; object.scale_x = FLT_MAX;
    assert(!zsharp_xr_ui_button_corners(&object, corners));
    /* Tiny nonzero panels do not acquire an absolute, oversized tolerance. */
    object = button(); object.width = 1e-12f;
    ray = forward_ray(); ray.origin[0] = 1e-10f;
    assert(!zsharp_xr_ui_hit_button(&ray, &object, &hit));
}

static void nearest_buttons(void) {
    ZSharpGameRenderObject objects[5];
    ZSharpXRUIRay ray = forward_ray();
    ZSharpXRUIHit hit;
    for (int i = 0; i < 5; ++i) {
        objects[i] = button(); objects[i].render_id = (size_t)i + 1;
        objects[i].z = (float)-i - 1;
    }
    objects[0].shape = ZGAME_SHAPE_CUBE; /* Non-UI occlusion is caller policy. */
    objects[1].visible = 0;
    assert(zsharp_xr_ui_pick_button(&ray, objects, 5, &hit));
    assert(hit.object_index == 2 && hit.render_id == 3 && near(hit.distance, 3));
    objects[3].z = -3; objects[3].render_id = 2;
    assert(zsharp_xr_ui_pick_button(&ray, objects, 5, &hit));
    assert(hit.object_index == 3 && hit.render_id == 2);
    {
        ZSharpGameRenderObject temporary = objects[2];
        objects[2] = objects[3]; objects[3] = temporary;
    }
    assert(zsharp_xr_ui_pick_button(&ray, objects, 5, &hit));
    assert(hit.object_index == 2 && hit.render_id == 2);
    assert(zsharp_xr_ui_pick_button(&ray, objects, 5, NULL));
    ray.max_distance = 2;
    assert(!zsharp_xr_ui_pick_button(&ray, objects, 5, &hit));
    assert(hit.object_index == SIZE_MAX && hit.render_id == 0);
    assert(!zsharp_xr_ui_pick_button(&ray, NULL, 0, &hit));
    assert(!zsharp_xr_ui_pick_button(&ray, NULL, 5, &hit));
    assert(!zsharp_xr_ui_pick_button(NULL, objects, 5, &hit));
}

static void independent_activation(void) {
    ZSharpXRUIActivation hands[2];
    zsharp_xr_ui_activation_reset(&hands[0], 4);
    zsharp_xr_ui_activation_reset(&hands[1], 8);
    /* Held on startup/scene entry: discard even a new edge until release. */
    assert(!zsharp_xr_ui_activation_update(&hands[0], 1, 1, 5, 10));
    assert(!zsharp_xr_ui_activation_update(&hands[0], 1, 1, 5, 11));
    assert(!zsharp_xr_ui_activation_update(&hands[0], 1, 0, 5, 11));
    assert(zsharp_xr_ui_activation_update(&hands[0], 1, 1, 6, 11) == 11);
    assert(!zsharp_xr_ui_activation_update(&hands[0], 1, 1, 6, 12));
    /* Left and right are independent and short taps survive final held=false. */
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 0, 8, 20));
    assert(zsharp_xr_ui_activation_update(&hands[1], 1, 0, 9, 20) == 20);
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 0, 9, 20));
    assert(zsharp_xr_ui_activation_update(&hands[1], 1, 0, 10, 20) == 20);
    /* A press away from a button must not later activate by aiming at one. */
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 1, 11, 0));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 1, 11, 21));
    /* Tracking/focus loss consumes retained edges and disarms while held. */
    assert(!zsharp_xr_ui_activation_update(&hands[1], 0, 1, 12, 21));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 1, 12, 21));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 1, 13, 21));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 0, 13, 21));
    assert(zsharp_xr_ui_activation_update(&hands[1], 1, 1, 14, 21) == 21);
    zsharp_xr_ui_activation_reset(&hands[1], 14);
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 1, 15, 22));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 0, 15, 22));
    assert(zsharp_xr_ui_activation_update(&hands[1], 1, 1, 16, 22) == 22);
    /* Zero-initialized cursors are safe; uint64 serial wrap remains fresh. */
    memset(&hands[1], 0, sizeof(hands[1]));
    assert(!zsharp_xr_ui_activation_update(&hands[1], 1, 0, UINT64_MAX, 22));
    assert(zsharp_xr_ui_activation_update(&hands[1], 1, 0, 0, 22) == 22);
    assert(!zsharp_xr_ui_activation_update(NULL, 1, 0, 1, 22));
    zsharp_xr_ui_activation_reset(NULL, 0);
}

int main(void) {
    basic_intersections();
    transforms_and_corners();
    poses();
    invalid_inputs();
    nearest_buttons();
    independent_activation();
    puts("PASS: world-space XR button rays, compound/mirrored transforms, two-sided hits, nearest selection, invalid inputs and independent latched activation");
    return 0;
}
