#include "xr_ui.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define XR_UI_PI 3.14159265358979323846264338327950288

typedef struct XRUIButtonTransform {
    double center[3];
    double width, height;
    double sine[3], cosine[3];
} XRUIButtonTransform;

void zsharp_xr_ui_activation_reset(ZSharpXRUIActivation *cursor,
    uint64_t pressed_serial) {
    if (!cursor) return;
    cursor->pressed_seen = pressed_serial;
    cursor->armed = 0;
}

size_t zsharp_xr_ui_activation_update(ZSharpXRUIActivation *cursor,
    int pointer_valid, int trigger_held, uint64_t pressed_serial,
    size_t hovered_render_id) {
    int fresh;
    size_t activated = 0;
    if (!cursor) return 0;
    fresh = pressed_serial != cursor->pressed_seen;
    cursor->pressed_seen = pressed_serial;
    if (!pointer_valid) {
        cursor->armed = 0;
        return 0;
    }
    if (fresh) {
        if (cursor->armed) activated = hovered_render_id;
        cursor->armed = 0;
    }
    /* Down+up can both precede this update. Its fresh edge activates above, and
       the already released trigger safely arms the next independent press. */
    if (!trigger_held) cursor->armed = 1;
    return activated;
}

static void clear_hit(ZSharpXRUIHit *hit) {
    if (hit) {
        memset(hit, 0, sizeof(*hit));
        hit->object_index = SIZE_MAX;
    }
}

static int finite_vector(const float *values, size_t count) {
    size_t i;
    if (!values) return 0;
    for (i = 0; i < count; ++i) if (!isfinite(values[i])) return 0;
    return 1;
}

static int normalized_direction(const float direction[3], double out[3]) {
    double length;
    size_t i;
    if (!finite_vector(direction, 3)) return 0;
    length = sqrt((double)direction[0] * direction[0] +
                  (double)direction[1] * direction[1] +
                  (double)direction[2] * direction[2]);
    if (!(length > 0)) return 0;
    for (i = 0; i < 3; ++i) out[i] = direction[i] / length;
    return 1;
}

static int ray_valid(const ZSharpXRUIRay *ray, double direction[3]) {
    return ray && finite_vector(ray->origin, 3) &&
        isfinite(ray->max_distance) && ray->max_distance >= 0 &&
        normalized_direction(ray->direction, direction);
}

static void rotate_axis(double point[3], int axis, double sine, double cosine) {
    double first, second;
    if (axis == 0) {
        first = point[1] * cosine - point[2] * sine;
        second = point[1] * sine + point[2] * cosine;
        point[1] = first; point[2] = second;
    } else if (axis == 1) {
        first = point[0] * cosine + point[2] * sine;
        second = -point[0] * sine + point[2] * cosine;
        point[0] = first; point[2] = second;
    } else {
        first = point[0] * cosine - point[1] * sine;
        second = point[0] * sine + point[1] * cosine;
        point[0] = first; point[1] = second;
    }
}

static int button_transform(const ZSharpGameRenderObject *button,
                            XRUIButtonTransform *transform) {
    double degrees[3];
    size_t i;
    if (!button || button->shape != ZGAME_SHAPE_BUTTON || !button->visible ||
        !isfinite(button->opacity) || button->opacity <= 0 ||
        !isfinite(button->x) || !isfinite(button->y) || !isfinite(button->z) ||
        !isfinite(button->width) || !isfinite(button->height) ||
        !isfinite(button->scale_x) || !isfinite(button->scale_y) ||
        !isfinite(button->rotation) || !isfinite(button->rotation_x) ||
        !isfinite(button->rotation_y) || !isfinite(button->rotation_z)) return 0;
    transform->center[0] = button->x;
    transform->center[1] = button->y;
    transform->center[2] = button->z;
    transform->width = (double)button->width * button->scale_x;
    transform->height = (double)button->height * button->scale_y;
    if (transform->width == 0 || transform->height == 0) return 0;
    degrees[0] = button->rotation_x;
    degrees[1] = (double)button->rotation_y + button->rotation;
    degrees[2] = button->rotation_z;
    for (i = 0; i < 3; ++i) {
        double radians = fmod(degrees[i], 360.0) * (XR_UI_PI / 180.0);
        transform->sine[i] = sin(radians);
        transform->cosine[i] = cos(radians);
    }
    return 1;
}

static void transform_point(const XRUIButtonTransform *transform,
                            double point[3]) {
    int axis;
    for (axis = 0; axis < 3; ++axis)
        rotate_axis(point, axis, transform->sine[axis], transform->cosine[axis]);
    for (axis = 0; axis < 3; ++axis) point[axis] += transform->center[axis];
}

static void inverse_rotate(const XRUIButtonTransform *transform,
                           double point[3]) {
    int axis;
    for (axis = 2; axis >= 0; --axis)
        rotate_axis(point, axis, -transform->sine[axis], transform->cosine[axis]);
}

int zsharp_xr_ui_ray_from_pose(ZSharpXRUIRay *ray,
    const float tracked_position[3], const float quaternion[4],
    const float rig_position[3], float rig_yaw_degrees, float max_distance) {
    ZSharpXRUIRay result = {{0}, {0}, 0};
    double q[4], position[3], direction[3], norm = 0, yaw;
    size_t i;
    if (!ray) return 0;
    memset(ray, 0, sizeof(*ray));
    if (!finite_vector(tracked_position, 3) || !finite_vector(quaternion, 4) ||
        !finite_vector(rig_position, 3) || !isfinite(rig_yaw_degrees) ||
        !isfinite(max_distance) || max_distance < 0) return 0;
    for (i = 0; i < 4; ++i) norm += (double)quaternion[i] * quaternion[i];
    if (!(norm > 0)) return 0;
    norm = sqrt(norm);
    for (i = 0; i < 4; ++i) q[i] = quaternion[i] / norm;
    direction[0] = -2 * (q[0] * q[2] + q[3] * q[1]);
    direction[1] = 2 * (q[3] * q[0] - q[1] * q[2]);
    direction[2] = 2 * (q[0] * q[0] + q[1] * q[1]) - 1;
    for (i = 0; i < 3; ++i) position[i] = tracked_position[i];
    yaw = fmod((double)rig_yaw_degrees, 360.0) * (XR_UI_PI / 180.0);
    rotate_axis(position, 1, sin(yaw), cos(yaw));
    rotate_axis(direction, 1, sin(yaw), cos(yaw));
    for (i = 0; i < 3; ++i) {
        position[i] += rig_position[i];
        if (!isfinite(position[i]) || fabs(position[i]) > FLT_MAX) return 0;
        result.origin[i] = (float)position[i];
        result.direction[i] = (float)direction[i];
    }
    result.max_distance = max_distance;
    *ray = result;
    return 1;
}

int zsharp_xr_ui_hit_button(const ZSharpXRUIRay *ray,
    const ZSharpGameRenderObject *button, ZSharpXRUIHit *hit) {
    XRUIButtonTransform transform;
    ZSharpXRUIHit result;
    double direction[3], local_origin[3], local_direction[3], distance;
    double local_x, local_y, half_width, half_height, u, v;
    size_t i;
    clear_hit(hit);
    if (!ray_valid(ray, direction) || !button_transform(button, &transform))
        return 0;
    for (i = 0; i < 3; ++i) {
        local_origin[i] = (double)ray->origin[i] - transform.center[i];
        local_direction[i] = direction[i];
    }
    inverse_rotate(&transform, local_origin);
    inverse_rotate(&transform, local_direction);
    /* A ray lying in the plane has no unique hover point. Very nearly parallel
       rays are rejected rather than amplifying floating-point pose noise. */
    if (fabs(local_direction[2]) <= 1e-9) return 0;
    distance = -local_origin[2] / local_direction[2];
    if (!isfinite(distance) || distance < 0 || distance > ray->max_distance)
        return 0;
    local_x = local_origin[0] + local_direction[0] * distance;
    local_y = local_origin[1] + local_direction[1] * distance;
    half_width = fabs(transform.width) * 0.5;
    half_height = fabs(transform.height) * 0.5;
    if (fabs(local_x) > half_width * (1 + 1e-6) ||
        fabs(local_y) > half_height * (1 + 1e-6)) return 0;
    u = 0.5 + local_x / transform.width;
    v = 0.5 - local_y / transform.height;
    clear_hit(&result);
    result.render_id = button->render_id;
    result.distance = (float)distance;
    result.uv[0] = (float)fmax(0, fmin(1, u));
    result.uv[1] = (float)fmax(0, fmin(1, v));
    for (i = 0; i < 3; ++i) {
        double coordinate = ray->origin[i] + direction[i] * distance;
        if (!isfinite(coordinate) || fabs(coordinate) > FLT_MAX) return 0;
        result.position[i] = (float)coordinate;
    }
    if (hit) *hit = result;
    return 1;
}

int zsharp_xr_ui_button_corners(const ZSharpGameRenderObject *button,
    float corners[4][3]) {
    XRUIButtonTransform transform;
    float result[4][3];
    size_t corner, axis;
    if (!corners) return 0;
    memset(corners, 0, sizeof(result));
    if (!button_transform(button, &transform)) return 0;
    for (corner = 0; corner < 4; ++corner) {
        double point[3] = {
            transform.width * ((corner == 0 || corner == 3) ? -0.5 : 0.5),
            transform.height * (corner < 2 ? 0.5 : -0.5), 0
        };
        transform_point(&transform, point);
        for (axis = 0; axis < 3; ++axis) {
            if (!isfinite(point[axis]) || fabs(point[axis]) > FLT_MAX) return 0;
            result[corner][axis] = (float)point[axis];
        }
    }
    memcpy(corners, result, sizeof(result));
    return 1;
}

int zsharp_xr_ui_pick_button(const ZSharpXRUIRay *ray,
    const ZSharpGameRenderObject *objects, size_t object_count,
    ZSharpXRUIHit *hit) {
    ZSharpXRUIHit nearest, candidate;
    double direction[3];
    size_t i;
    int found = 0;
    clear_hit(hit);
    clear_hit(&nearest);
    if (!ray_valid(ray, direction) || (!objects && object_count)) return 0;
    for (i = 0; i < object_count; ++i) {
        if (!zsharp_xr_ui_hit_button(ray, &objects[i], &candidate)) continue;
        if (!found || candidate.distance < nearest.distance ||
            (candidate.distance == nearest.distance &&
             candidate.render_id < nearest.render_id)) {
            nearest = candidate;
            nearest.object_index = i;
            found = 1;
        }
    }
    if (found && hit) *hit = nearest;
    return found;
}
