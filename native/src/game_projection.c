#include "game_projection.h"

#include <math.h>
#include <stdlib.h>

#define ZGAME_LOGICAL_WIDTH 1280.0f
#define ZGAME_LOGICAL_HEIGHT 720.0f
#define ZGAME_PI 3.14159265358979323846f
#define ZGAME_NEAR_DEPTH 0.05f

static void rotate_x(float point[3], float radians) {
    float sine = sinf(radians), cosine = cosf(radians);
    float y = point[1] * cosine - point[2] * sine;
    float z = point[1] * sine + point[2] * cosine;
    point[1] = y; point[2] = z;
}

static void rotate_y(float point[3], float radians) {
    float sine = sinf(radians), cosine = cosf(radians);
    float x = point[0] * cosine + point[2] * sine;
    float z = -point[0] * sine + point[2] * cosine;
    point[0] = x; point[2] = z;
}

static void rotate_z(float point[3], float radians) {
    float sine = sinf(radians), cosine = cosf(radians);
    float x = point[0] * cosine - point[1] * sine;
    float y = point[0] * sine + point[1] * cosine;
    point[0] = x; point[1] = y;
}

/* The camera looks down local -Z, preserving the old unrotated behavior. */
static void camera_space(const ZSharpGameRenderFrame *frame,
                         const float world[3], float output[3]) {
    output[0] = world[0] - frame->camera_x;
    output[1] = world[1] - frame->camera_y;
    output[2] = world[2] - frame->camera_z;
    /* Apply the inverse camera transform in reverse Euler order. The camera's
       local-to-world orientation is yaw, then pitch, then roll, so view space
       must undo yaw before pitch and roll. Applying pitch first made it act
       around a world-space axis; near 180 degrees of yaw that presented as an
       unwanted roll even when cameraRotationZ was zero. */
    rotate_y(output, -frame->camera_rotation_y * ZGAME_PI / 180.0f);
    rotate_x(output, -frame->camera_rotation_x * ZGAME_PI / 180.0f);
    rotate_z(output, -frame->camera_rotation_z * ZGAME_PI / 180.0f);
}

static int project_camera_point(const ZSharpGameRenderFrame *frame,
                                const float point[3], float output[2]) {
    float depth = -point[2];
    float fov = frame->camera_fov <= 1.0f ? 70.0f : frame->camera_fov;
    float focal = (ZGAME_LOGICAL_WIDTH * 0.5f) /
                  tanf(fov * 0.5f * ZGAME_PI / 180.0f);
    if (depth < ZGAME_NEAR_DEPTH) return 0;
    output[0] = ZGAME_LOGICAL_WIDTH * 0.5f + point[0] * focal / depth;
    output[1] = ZGAME_LOGICAL_HEIGHT * 0.5f - point[1] * focal / depth;
    return isfinite(output[0]) && isfinite(output[1]);
}

static void cube_points(const ZSharpGameRenderObject *object,
                        float points[8][3]) {
    float half_x = object->width * object->scale_x * 0.5f;
    float half_y = object->height * object->scale_y * 0.5f;
    float half_z = object->depth * object->scale_z * 0.5f;
    float rx = object->rotation_x * ZGAME_PI / 180.0f;
    /* `rotation` remains the legacy cube yaw for existing projects. */
    float ry = (object->rotation_y + object->rotation) * ZGAME_PI / 180.0f;
    float rz = object->rotation_z * ZGAME_PI / 180.0f;
    int index;
    for (index = 0; index < 8; index++) {
        float point[3] = {
            (index & 1) ? half_x : -half_x,
            (index & 2) ? half_y : -half_y,
            (index & 4) ? half_z : -half_z
        };
        rotate_x(point, rx); rotate_y(point, ry); rotate_z(point, rz);
        points[index][0] = object->x + point[0];
        points[index][1] = object->y + point[1];
        points[index][2] = object->z + point[2];
    }
}

static void cube_camera_points(const ZSharpGameRenderFrame *frame,
                               const ZSharpGameRenderObject *object,
                               float points[8][3]) {
    float world[8][3];
    int index;
    cube_points(object, world);
    for (index = 0; index < 8; index++)
        camera_space(frame, world[index], points[index]);
}

float zsharp_game_camera_depth(const ZSharpGameRenderFrame *frame,
                               const ZSharpGameRenderObject *object) {
    float world[3], camera[3];
    if (frame == NULL || object == NULL) return 0.0f;
    world[0] = object->x; world[1] = object->y; world[2] = object->z;
    camera_space(frame, world, camera);
    return -camera[2];
}

size_t zsharp_game_project_cube(const ZSharpGameRenderFrame *frame,
                                const ZSharpGameRenderObject *object,
                                float output[12][4]) {
    static const int edge_indices[12][2] = {
        {0,1},{0,2},{1,3},{2,3}, {4,5},{4,6},{5,7},{6,7},
        {0,4},{1,5},{2,6},{3,7}
    };
    float points[8][3];
    size_t edge_count = 0;
    int index;
    if (frame == NULL || object == NULL || output == NULL) return 0;
    cube_camera_points(frame, object, points);
    for (index = 0; index < 12; index++) {
        int first = edge_indices[index][0], second = edge_indices[index][1];
        float a[3] = {points[first][0], points[first][1], points[first][2]};
        float b[3] = {points[second][0], points[second][1], points[second][2]};
        float da = -a[2], db = -b[2];
        if (da < ZGAME_NEAR_DEPTH && db < ZGAME_NEAR_DEPTH) continue;
        if (da < ZGAME_NEAR_DEPTH || db < ZGAME_NEAR_DEPTH) {
            float *behind = da < ZGAME_NEAR_DEPTH ? a : b;
            const float *visible = da < ZGAME_NEAR_DEPTH ? b : a;
            float amount = (-ZGAME_NEAR_DEPTH - behind[2]) /
                           (visible[2] - behind[2]);
            behind[0] += (visible[0] - behind[0]) * amount;
            behind[1] += (visible[1] - behind[1]) * amount;
            behind[2] = -ZGAME_NEAR_DEPTH;
        }
        if (!project_camera_point(frame, a, &output[edge_count][0]) ||
            !project_camera_point(frame, b, &output[edge_count][2])) continue;
        edge_count++;
    }
    return edge_count;
}

static int compare_faces(const void *left, const void *right) {
    const ZSharpProjectedCubeFace *a = (const ZSharpProjectedCubeFace *)left;
    const ZSharpProjectedCubeFace *b = (const ZSharpProjectedCubeFace *)right;
    return a->depth < b->depth ? 1 : a->depth > b->depth ? -1 : 0;
}

size_t zsharp_game_project_cube_faces(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object,
    ZSharpProjectedCubeFace output[6]) {
    static const int face_indices[6][4] = {
        {0,2,3,1}, {4,5,7,6}, {0,1,5,4},
        {2,6,7,3}, {0,4,6,2}, {1,3,7,5}
    };
    static const float brightness[6] = {0.58f, 0.82f, 0.68f,
                                         1.00f, 0.74f, 0.90f};
    float points[8][3];
    size_t face_count = 0;
    int face;
    if (frame == NULL || object == NULL || output == NULL) return 0;
    cube_camera_points(frame, object, points);
    for (face = 0; face < 6; face++) {
        float input[6][5], clipped[6][5];
        size_t input_count = 4, clipped_count = 0, index;
        float depth_total = 0.0f;
        ZSharpProjectedCubeFace *result;
        for (index = 0; index < 4; index++) {
            int vertex = face_indices[face][index];
            input[index][0] = points[vertex][0];
            input[index][1] = points[vertex][1];
            input[index][2] = points[vertex][2];
            input[index][3] = (index == 1 || index == 2) ? 1.0f : 0.0f;
            input[index][4] = index >= 2 ? 1.0f : 0.0f;
        }
        for (index = 0; index < input_count; index++) {
            const float *current = input[index];
            const float *previous = input[(index + input_count - 1) % input_count];
            int current_inside = current[2] <= -ZGAME_NEAR_DEPTH;
            int previous_inside = previous[2] <= -ZGAME_NEAR_DEPTH;
            if (current_inside != previous_inside) {
                float amount = (-ZGAME_NEAR_DEPTH - previous[2]) /
                               (current[2] - previous[2]);
                int component;
                for (component = 0; component < 5; component++)
                    clipped[clipped_count][component] = previous[component] +
                        (current[component] - previous[component]) * amount;
                clipped[clipped_count][2] = -ZGAME_NEAR_DEPTH;
                clipped_count++;
            }
            if (current_inside) {
                int component;
                for (component = 0; component < 5; component++)
                    clipped[clipped_count][component] = current[component];
                clipped_count++;
            }
        }
        if (clipped_count < 3) continue;
        result = &output[face_count];
        result->point_count = clipped_count;
        result->brightness = brightness[face];
        for (index = 0; index < clipped_count; index++) {
            if (!project_camera_point(frame, clipped[index], result->points[index])) {
                clipped_count = 0; break;
            }
            result->texcoords[index][0] = clipped[index][3];
            result->texcoords[index][1] = clipped[index][4];
            depth_total += -clipped[index][2];
        }
        if (clipped_count < 3) continue;
        result->depth = depth_total / (float)clipped_count;
        face_count++;
    }
    qsort(output, face_count, sizeof(*output), compare_faces);
    return face_count;
}
