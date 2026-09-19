#include "game_projection.h"

#include <math.h>

#define ZGAME_LOGICAL_WIDTH 1280.0f
#define ZGAME_LOGICAL_HEIGHT 720.0f
#define ZGAME_PI 3.14159265358979323846f
#define ZGAME_NEAR_DEPTH 0.05f

static int project_point(const ZSharpGameRenderFrame *frame,
                         const float point[3], float output[2]) {
    float depth = frame->camera_z - point[2];
    float fov = frame->camera_fov <= 1.0f ? 70.0f : frame->camera_fov;
    float focal = (ZGAME_LOGICAL_WIDTH * 0.5f) /
                  tanf(fov * 0.5f * ZGAME_PI / 180.0f);
    if (depth < ZGAME_NEAR_DEPTH) return 0;
    output[0] = ZGAME_LOGICAL_WIDTH * 0.5f +
                (point[0] - frame->camera_x) * focal / depth;
    output[1] = ZGAME_LOGICAL_HEIGHT * 0.5f -
                (point[1] - frame->camera_y) * focal / depth;
    return isfinite(output[0]) && isfinite(output[1]);
}

size_t zsharp_game_project_cube(const ZSharpGameRenderFrame *frame,
                                const ZSharpGameRenderObject *object,
                                float output[12][4]) {
    static const int edge_indices[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    float half_x, half_y, half_z, radians, sine, cosine;
    float points[8][3];
    size_t edge_count = 0;
    int index;
    if (frame == NULL || object == NULL || output == NULL) return 0;
    half_x = object->width * object->scale_x * 0.5f;
    half_y = object->height * object->scale_y * 0.5f;
    half_z = object->depth * object->scale_z * 0.5f;
    radians = object->rotation * ZGAME_PI / 180.0f;
    sine = sinf(radians);
    cosine = cosf(radians);
    for (index = 0; index < 8; index++) {
        float x = (index & 1) ? half_x : -half_x;
        float y = (index & 2) ? half_y : -half_y;
        float z = (index & 4) ? half_z : -half_z;
        points[index][0] = object->x + x * cosine - z * sine;
        points[index][1] = object->y + y;
        points[index][2] = object->z + x * sine + z * cosine;
    }
    for (index = 0; index < 12; index++) {
        int first = edge_indices[index][0], second = edge_indices[index][1];
        float a[3] = {points[first][0], points[first][1], points[first][2]};
        float b[3] = {points[second][0], points[second][1], points[second][2]};
        float da = frame->camera_z - a[2];
        float db = frame->camera_z - b[2];
        if (da < ZGAME_NEAR_DEPTH && db < ZGAME_NEAR_DEPTH) continue;
        if (da < ZGAME_NEAR_DEPTH || db < ZGAME_NEAR_DEPTH) {
            float *behind = da < ZGAME_NEAR_DEPTH ? a : b;
            const float *visible = da < ZGAME_NEAR_DEPTH ? b : a;
            float clip_z = frame->camera_z - ZGAME_NEAR_DEPTH;
            float divisor = visible[2] - behind[2];
            float amount = divisor == 0.0f ? 0.0f :
                (clip_z - behind[2]) / divisor;
            behind[0] += (visible[0] - behind[0]) * amount;
            behind[1] += (visible[1] - behind[1]) * amount;
            behind[2] = clip_z;
        }
        if (!project_point(frame, a, &output[edge_count][0]) ||
            !project_point(frame, b, &output[edge_count][2])) continue;
        edge_count++;
    }
    return edge_count;
}
