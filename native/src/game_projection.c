#include "game_projection.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ZGAME_LOGICAL_WIDTH 1280.0f
#define ZGAME_LOGICAL_HEIGHT 720.0f
#define ZGAME_PI 3.14159265358979323846f
#define ZGAME_NEAR_DEPTH 0.05f

static void rotate_x(float point[3], float radians) {
    if(radians==0)return;
    float sine = sinf(radians), cosine = cosf(radians);
    float y = point[1] * cosine - point[2] * sine;
    float z = point[1] * sine + point[2] * cosine;
    point[1] = y; point[2] = z;
}

static void rotate_y(float point[3], float radians) {
    if(radians==0)return;
    float sine = sinf(radians), cosine = cosf(radians);
    float x = point[0] * cosine + point[2] * sine;
    float z = -point[0] * sine + point[2] * cosine;
    point[0] = x; point[2] = z;
}

static void rotate_z(float point[3], float radians) {
    if(radians==0)return;
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
    if(frame->projection_prepared) {
        float x=output[0],y=output[1],z=output[2];
        int axis;
        for(axis=0;axis<3;axis++)output[axis]=frame->view_basis[axis]*x+
            frame->view_basis[3+axis]*y+frame->view_basis[6+axis]*z;
        return;
    }
    /* Apply the inverse camera transform in reverse Euler order. The camera's
       local-to-world orientation is yaw, then pitch, then roll, so view space
       must undo yaw before pitch and roll. Applying pitch first made it act
       around a world-space axis; near 180 degrees of yaw that presented as an
       unwanted roll even when cameraRotationZ was zero. */
    rotate_y(output, -frame->camera_rotation_y * ZGAME_PI / 180.0f);
    rotate_x(output, -frame->camera_rotation_x * ZGAME_PI / 180.0f);
    rotate_z(output, -frame->camera_rotation_z * ZGAME_PI / 180.0f);
}

void zsharp_game_prepare_projection(ZSharpGameRenderFrame *frame) {
    int axis;
    for(axis=0;axis<3;axis++) {
        float vector[3]={0,0,0};vector[axis]=1;
        rotate_y(vector,-frame->camera_rotation_y*ZGAME_PI/180.0f);
        rotate_x(vector,-frame->camera_rotation_x*ZGAME_PI/180.0f);
        rotate_z(vector,-frame->camera_rotation_z*ZGAME_PI/180.0f);
        memcpy(frame->view_basis+axis*3,vector,sizeof(vector));
    }
    frame->projection_tangent=tanf((frame->camera_fov<=1?70:frame->camera_fov)*.5f*ZGAME_PI/180.0f);
    frame->projection_focal=ZGAME_LOGICAL_WIDTH*.5f/frame->projection_tangent;
    frame->projection_prepared=1;
}

int zsharp_game_cube_in_view(const ZSharpGameRenderFrame *frame,
                            const ZSharpGameRenderObject *object) {
    float world[3] = {object->x, object->y, object->z}, point[3];
    float hx = object->width * object->scale_x * .5f;
    float hy = object->height * object->scale_y * .5f;
    float hz = object->depth * object->scale_z * .5f;
    float radius = sqrtf(hx*hx + hy*hy + hz*hz), depth;
    float fov = frame->camera_fov <= 1 ? 70 : frame->camera_fov;
    float tangent = frame->projection_prepared ? frame->projection_tangent : tanf(fov * .5f * ZGAME_PI / 180.0f);
    float vertical = tangent * ZGAME_LOGICAL_HEIGHT / ZGAME_LOGICAL_WIDTH;
    camera_space(frame, world, point);
    depth = -point[2];
    return depth + radius >= ZGAME_NEAR_DEPTH &&
        fabsf(point[0]) - depth*tangent <= radius*sqrtf(1+tangent*tangent) &&
        fabsf(point[1]) - depth*vertical <= radius*sqrtf(1+vertical*vertical);
}

static int project_camera_point(const ZSharpGameRenderFrame *frame,
                                const float point[3], float output[2]) {
    float depth = -point[2];
    float fov = frame->camera_fov <= 1.0f ? 70.0f : frame->camera_fov;
    float focal = frame->projection_prepared ? frame->projection_focal : (ZGAME_LOGICAL_WIDTH * 0.5f) /
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

float zsharp_game_camera_far_depth(const ZSharpGameRenderFrame *frame,
                                   const ZSharpGameRenderObject *object) {
    float corners[8][3];
    float farthest = -INFINITY;
    int index;
    if (frame == NULL || object == NULL) return 0.0f;
    if (object->shape != ZGAME_SHAPE_CUBE)
        return zsharp_game_camera_depth(frame, object);
    cube_camera_points(frame, object, corners);
    for (index = 0; index < 8; index++) {
        float depth = -corners[index][2];
        if (depth > farthest) farthest = depth;
    }
    return farthest;
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

int zsharp_game_project_mesh_triangle(
    const ZSharpGameRenderFrame *frame,
    const float world[3][3], const float uv[3][2],
    ZSharpProjectedCubeFace *face) {
    float camera[3][5], clipped[6][5];
    float edge_a[3], edge_b[3], length, depth_total = 0.0f;
    size_t count = 0, index;
    int axis;
    if (frame == NULL || world == NULL || uv == NULL || face == NULL) return 0;
    memset(face, 0, sizeof(*face));
    for (index = 0; index < 3; index++) {
        camera_space(frame, world[index], camera[index]);
        camera[index][3] = uv[index][0];
        camera[index][4] = uv[index][1];
    }
    for (index = 0; index < 3; index++) {
        const float *current = camera[index];
        const float *previous = camera[(index + 2) % 3];
        int inside = current[2] <= -ZGAME_NEAR_DEPTH;
        int previous_inside = previous[2] <= -ZGAME_NEAR_DEPTH;
        if (inside != previous_inside) {
            float amount = (-ZGAME_NEAR_DEPTH - previous[2]) /
                           (current[2] - previous[2]);
            for (axis = 0; axis < 5; axis++)
                clipped[count][axis] = previous[axis] +
                    (current[axis] - previous[axis]) * amount;
            clipped[count][2] = -ZGAME_NEAR_DEPTH;
            count++;
        }
        if (inside) {
            for (axis = 0; axis < 5; axis++) clipped[count][axis] = current[axis];
            count++;
        }
    }
    if (count < 3) return 0;
    face->point_count = count;
    face->brightness = 1.0f;
    for (index = 0; index < count; index++) {
        if (!project_camera_point(frame, clipped[index], face->points[index]))
            return 0;
        face->texcoords[index][0] = clipped[index][3];
        face->texcoords[index][1] = 1.0f - clipped[index][4];
        face->point_depth[index] = -clipped[index][2];
        depth_total += -clipped[index][2];
    }
    face->depth = depth_total / (float)count;
    for (axis = 0; axis < 3; axis++) {
        edge_a[axis] = world[1][axis] - world[0][axis];
        edge_b[axis] = world[2][axis] - world[0][axis];
        face->world_center[axis] =
            (world[0][axis] + world[1][axis] + world[2][axis]) / 3.0f;
    }
    face->world_normal[0] = edge_a[1]*edge_b[2]-edge_a[2]*edge_b[1];
    face->world_normal[1] = edge_a[2]*edge_b[0]-edge_a[0]*edge_b[2];
    face->world_normal[2] = edge_a[0]*edge_b[1]-edge_a[1]*edge_b[0];
    length = sqrtf(face->world_normal[0]*face->world_normal[0] +
                   face->world_normal[1]*face->world_normal[1] +
                   face->world_normal[2]*face->world_normal[2]);
    if (length < 0.00001f) return 0;
    for (axis = 0; axis < 3; axis++) face->world_normal[axis] /= length;
    return 1;
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
    float world_points[8][3];
    size_t face_count = 0;
    int face;
    if (frame == NULL || object == NULL || output == NULL) return 0;
    cube_points(object, world_points);
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
        {
            const float *a = world_points[face_indices[face][0]];
            const float *b = world_points[face_indices[face][1]];
            const float *c = world_points[face_indices[face][2]];
            float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
            float ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
            float length;
            int axis;
            result->world_normal[0] = ab[1]*ac[2]-ab[2]*ac[1];
            result->world_normal[1] = ab[2]*ac[0]-ab[0]*ac[2];
            result->world_normal[2] = ab[0]*ac[1]-ab[1]*ac[0];
            length = sqrtf(result->world_normal[0]*result->world_normal[0] +
                           result->world_normal[1]*result->world_normal[1] +
                           result->world_normal[2]*result->world_normal[2]);
            if (length < 0.00001f) length = 1.0f;
            for (axis = 0; axis < 3; axis++) {
                result->world_normal[axis] /= length;
                result->world_center[axis] = (a[axis] + b[axis] + c[axis] +
                    world_points[face_indices[face][3]][axis]) * 0.25f;
            }
        }
        for (index = 0; index < clipped_count; index++) {
            if (!project_camera_point(frame, clipped[index], result->points[index])) {
                clipped_count = 0; break;
            }
            result->texcoords[index][0] = clipped[index][3];
            result->texcoords[index][1] = clipped[index][4];
            result->point_depth[index] = -clipped[index][2];
            depth_total += -clipped[index][2];
        }
        if (clipped_count < 3) continue;
        result->depth = depth_total / (float)clipped_count;
        face_count++;
    }
    qsort(output, face_count, sizeof(*output), compare_faces);
    return face_count;
}
