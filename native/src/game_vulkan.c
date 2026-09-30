#define _CRT_SECURE_NO_WARNINGS

#include "game_vulkan.h"
#include "game_projection.h"

#ifdef ZSHARP_HAS_GAME_RUNTIME

#include <SDL3/SDL.h>
#include <ufbx.h>

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZGAME_LOGICAL_WIDTH 1280.0f
#define ZGAME_LOGICAL_HEIGHT 720.0f
#define ZGAME_PI 3.14159265358979323846f

struct ZSharpGameVulkan {
    SDL_Window *window;
    SDL_Renderer *renderer;
    char driver[64];
    struct ZSharpTextureCache *textures;
    struct ZSharpMeshCache *meshes;
    SDL_Texture *depth_texture;
    uint32_t *color_buffer;
    float *depth_buffer;
    int depth_active;
};

typedef struct ZSharpTextureCache {
    char *path;
    SDL_Texture *texture;
    SDL_Surface *surface;
    struct ZSharpTextureCache *next;
} ZSharpTextureCache;

typedef struct ZSharpCachedTriangle {
    float points[3][3];
    float uv[3][2];
    uint32_t vertex_indices[3];
    const ufbx_material *material;
    const ufbx_node *node;
    float pivot[3];
} ZSharpCachedTriangle;

typedef struct ZSharpMeshCache {
    char *path;
    ufbx_scene *scene;
    ZSharpCachedTriangle *triangles;
    size_t triangle_count;
    float source_extent[3];
    struct ZSharpMeshCache *next;
} ZSharpMeshCache;

static void renderer_error(char *error, size_t error_size,
                           const char *message) {
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s", message == NULL ? "render error"
                                                            : message);
}

static SDL_Color color_value(unsigned rgb) {
    SDL_Color color;
    color.r = (Uint8)((rgb >> 16u) & 0xffu);
    color.g = (Uint8)((rgb >> 8u) & 0xffu);
    color.b = (Uint8)(rgb & 0xffu);
    color.a = 255;
    return color;
}

static SDL_FColor float_color_value(unsigned rgb) {
    SDL_FColor color;
    color.r = (float)((rgb >> 16u) & 0xffu) / 255.0f;
    color.g = (float)((rgb >> 8u) & 0xffu) / 255.0f;
    color.b = (float)(rgb & 0xffu) / 255.0f;
    color.a = 1.0f;
    return color;
}

static void set_color(SDL_Renderer *renderer, unsigned rgb) {
    SDL_Color color = color_value(rgb);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

static float screen_x(float world, float camera) {
    return ZGAME_LOGICAL_WIDTH * 0.5f + world - camera;
}

static float screen_y(float world, float camera) {
    return ZGAME_LOGICAL_HEIGHT * 0.5f - world + camera;
}

static int render_rectangle(SDL_Renderer *renderer,
                            const ZSharpGameRenderObject *object,
                            float camera_x, float camera_y) {
    float width = object->width * object->scale_x;
    float height = object->height * object->scale_y;
    float cx = screen_x(object->x, camera_x);
    float cy = screen_y(object->y, camera_y);
    SDL_FColor color = float_color_value(object->color);
    SDL_Vertex vertices[4];
    int indices[6] = {0, 1, 2, 0, 2, 3};
    float radians = -object->rotation * ZGAME_PI / 180.0f;
    float sine = sinf(radians);
    float cosine = cosf(radians);
    float local[4][2] = {
        {-width * 0.5f, -height * 0.5f},
        { width * 0.5f, -height * 0.5f},
        { width * 0.5f,  height * 0.5f},
        {-width * 0.5f,  height * 0.5f}
    };
    int index;
    memset(vertices, 0, sizeof(vertices));
    for (index = 0; index < 4; index++) {
        vertices[index].position.x =
            cx + local[index][0] * cosine - local[index][1] * sine;
        vertices[index].position.y =
            cy + local[index][0] * sine + local[index][1] * cosine;
        vertices[index].color = color;
    }
    return SDL_RenderGeometry(renderer, NULL, vertices, 4, indices, 6);
}

static int render_triangle(SDL_Renderer *renderer,
                           const ZSharpGameRenderObject *object,
                           float camera_x, float camera_y) {
    float width = object->width * object->scale_x;
    float height = object->height * object->scale_y;
    float cx = screen_x(object->x, camera_x);
    float cy = screen_y(object->y, camera_y);
    SDL_FColor color = float_color_value(object->color);
    SDL_Vertex vertices[3];
    int indices[3] = {0, 1, 2};
    memset(vertices, 0, sizeof(vertices));
    vertices[0].position.x = cx;
    vertices[0].position.y = cy - height * 0.5f;
    vertices[1].position.x = cx + width * 0.5f;
    vertices[1].position.y = cy + height * 0.5f;
    vertices[2].position.x = cx - width * 0.5f;
    vertices[2].position.y = cy + height * 0.5f;
    vertices[0].color = vertices[1].color = vertices[2].color = color;
    return SDL_RenderGeometry(renderer, NULL, vertices, 3, indices, 3);
}

static int render_circle(SDL_Renderer *renderer,
                         const ZSharpGameRenderObject *object,
                         float camera_x, float camera_y) {
    enum { SEGMENTS = 40 };
    SDL_Vertex vertices[SEGMENTS + 1];
    int indices[SEGMENTS * 3];
    SDL_FColor color = float_color_value(object->color);
    float cx = screen_x(object->x, camera_x);
    float cy = screen_y(object->y, camera_y);
    float radius_x = object->width * object->scale_x * 0.5f;
    float radius_y = object->height * object->scale_y * 0.5f;
    int index;
    memset(vertices, 0, sizeof(vertices));
    vertices[0].position.x = cx;
    vertices[0].position.y = cy;
    vertices[0].color = color;
    for (index = 0; index < SEGMENTS; index++) {
        float angle = (float)index * 2.0f * ZGAME_PI / (float)SEGMENTS;
        vertices[index + 1].position.x = cx + cosf(angle) * radius_x;
        vertices[index + 1].position.y = cy + sinf(angle) * radius_y;
        vertices[index + 1].color = color;
        indices[index * 3] = 0;
        indices[index * 3 + 1] = index + 1;
        indices[index * 3 + 2] = (index + 1) % SEGMENTS + 1;
    }
    return SDL_RenderGeometry(renderer, NULL, vertices, SEGMENTS + 1,
                              indices, SEGMENTS * 3);
}

static SDL_Texture *load_sprite(ZSharpGameVulkan *renderer,
                                const ZSharpGameRenderFrame *frame,
                                const char *relative, char *error,
                                size_t error_size);

static ZSharpMeshCache *load_mesh(ZSharpGameVulkan *renderer,
                                  const ZSharpGameRenderFrame *frame,
                                  const char *relative, char *error,
                                  size_t error_size) {
    ZSharpMeshCache *cached;
    size_t root_length, relative_length, node_index;
    char *path;
    ufbx_error import_error;
    if (relative == NULL || frame->project_root == NULL) {
        renderer_error(error, error_size, "model has no FBX parent asset");
        return NULL;
    }
    root_length = strlen(frame->project_root);
    relative_length = strlen(relative);
    path = (char *)malloc(root_length + relative_length + 2);
    if (path == NULL) goto memory_error;
    snprintf(path, root_length + relative_length + 2, "%s/%s",
             frame->project_root, relative);
    for (cached = renderer->meshes; cached != NULL; cached = cached->next) {
        if (strcmp(cached->path, path) == 0) { free(path); return cached; }
    }
    cached = (ZSharpMeshCache *)calloc(1, sizeof(*cached));
    if (cached == NULL) { free(path); goto memory_error; }
    cached->path = path;
    cached->scene = ufbx_load_file(path, NULL, &import_error);
    if (cached->scene == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "could not import FBX '%s': %.180s",
                     relative, import_error.description.data);
        free(cached->path); free(cached);
        return NULL;
    }
    for (node_index = 0; node_index < cached->scene->nodes.count; node_index++) {
        const ufbx_node *node = cached->scene->nodes.data[node_index];
        const ufbx_mesh *mesh = node->mesh;
        size_t face_index;
        if (mesh == NULL) continue;
        for (face_index = 0; face_index < mesh->faces.count; face_index++) {
            ufbx_face face = mesh->faces.data[face_index];
            uint32_t *indices;
            uint32_t count, triangle;
            const ufbx_material *material = NULL;
            if (face.num_indices < 3 || face.num_indices > 100000) continue;
            indices = (uint32_t *)malloc((size_t)(face.num_indices - 2) *
                                          3 * sizeof(uint32_t));
            if (indices == NULL) goto failed;
            count = ufbx_triangulate_face(indices,
                (size_t)(face.num_indices - 2) * 3, mesh, face);
            if (count == 0) { free(indices); continue; }
            if (face_index < mesh->face_material.count) {
                uint32_t material_index = mesh->face_material.data[face_index];
                if (material_index < node->materials.count)
                    material = node->materials.data[material_index];
            }
            if (cached->triangle_count + count > 200000) {
                free(indices);
                renderer_error(error, error_size,
                               "FBX exceeds the current 200000 triangle runtime limit");
                goto failed;
            }
            {
                ZSharpCachedTriangle *resized = (ZSharpCachedTriangle *)realloc(
                    cached->triangles, (cached->triangle_count + count) *
                    sizeof(*resized));
                if (resized == NULL) { free(indices); goto failed; }
                cached->triangles = resized;
            }
            for (triangle = 0; triangle < count; triangle++) {
                ZSharpCachedTriangle *target =
                    &cached->triangles[cached->triangle_count + triangle];
                int corner;
                memset(target, 0, sizeof(*target));
                target->material = material;
                target->node = node;
                {
                    ufbx_vec3 origin = {0};
                    ufbx_vec3 pivot = ufbx_transform_position(
                        &node->geometry_to_world, origin);
                    target->pivot[0] = (float)pivot.x;
                    target->pivot[1] = (float)pivot.y;
                    target->pivot[2] = (float)pivot.z;
                }
                for (corner = 0; corner < 3; corner++) {
                    uint32_t vertex_index = indices[triangle * 3 + corner];
                    target->vertex_indices[corner] = vertex_index;
                    ufbx_vec3 vertex = ufbx_get_vertex_vec3(
                        &mesh->vertex_position, vertex_index);
                    ufbx_vec3 world = ufbx_transform_position(
                        &node->geometry_to_world, vertex);
                    target->points[corner][0] = (float)world.x;
                    target->points[corner][1] = (float)world.y;
                    target->points[corner][2] = (float)world.z;
                    if (mesh->vertex_uv.exists) {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2(
                            &mesh->vertex_uv, vertex_index);
                        target->uv[corner][0] = (float)uv.x;
                        target->uv[corner][1] = (float)uv.y;
                    }
                }
            }
            cached->triangle_count += count;
            free(indices);
        }
    }
    if (cached->triangle_count == 0) {
        renderer_error(error, error_size, "FBX has no visible triangles");
        goto failed;
    }
    {
        float low[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float high[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        size_t triangle;
        int axis, corner;
        for (triangle = 0; triangle < cached->triangle_count; triangle++)
            for (corner = 0; corner < 3; corner++)
                for (axis = 0; axis < 3; axis++) {
                    float value = cached->triangles[triangle].points[corner][axis];
                    if (value < low[axis]) low[axis] = value;
                    if (value > high[axis]) high[axis] = value;
                }
        for (axis = 0; axis < 3; axis++) {
            float extent = high[axis] - low[axis];
            cached->source_extent[axis] = extent > 0.000001f ? extent : 1.0f;
        }
    }
    cached->next = renderer->meshes;
    renderer->meshes = cached;
    return cached;
failed:
    ufbx_free_scene(cached->scene);
    free(cached->triangles);
    free(cached->path);
    free(cached);
memory_error:
    renderer_error(error, error_size, "out of memory loading FBX mesh");
    return NULL;
}

static float clamp_unit(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static void unrotate_shadow_point(float point[3],
                                  const ZSharpGameRenderObject *object) {
    float angle, sine, cosine, first, second;
    point[0] -= object->x;
    point[1] -= object->y;
    point[2] -= object->z;
    angle = -object->rotation_z * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[0]*cosine-point[1]*sine;
    second = point[0]*sine+point[1]*cosine;
    point[0] = first; point[1] = second;
    angle = -(object->rotation_y + object->rotation) *
            ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[0]*cosine+point[2]*sine;
    second = -point[0]*sine+point[2]*cosine;
    point[0] = first; point[2] = second;
    angle = -object->rotation_x * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[1]*cosine-point[2]*sine;
    second = point[1]*sine+point[2]*cosine;
    point[1] = first; point[2] = second;
}

/* Coarse real-time shadows: boxes intersect an oriented volume; models use
   a rotation-invariant bounding sphere. Exact model shadow silhouettes would
   require a separate shadow map or per-triangle ray intersections. */
static int light_occluded(const ZSharpGameRenderFrame *frame,
                          const ZSharpGameRenderObject *self,
                          const float point[3], const float light[3]) {
    size_t index;
    for (index = 0; index < frame->object_count; index++) {
        const ZSharpGameRenderObject *box = &frame->objects[index];
        float half[3] = {fabsf(box->width * box->scale_x) * 0.5f,
                         fabsf(box->height * box->scale_y) * 0.5f,
                         fabsf(box->depth * box->scale_z) * 0.5f};
        float entry = 0.002f, leave = 0.998f;
        int axis;
        if (box == self || (self->render_id != 0 &&
            box->render_id == self->render_id) || !box->visible ||
            (box->shape != ZGAME_SHAPE_CUBE &&
             box->shape != ZGAME_SHAPE_MESH) ||
            box->opacity < 0.99f) continue;
        if (box->shape == ZGAME_SHAPE_MESH) {
            float direction[3] = {light[0]-point[0], light[1]-point[1],
                                  light[2]-point[2]};
            float offset[3] = {point[0]-box->x, point[1]-box->y,
                               point[2]-box->z};
            float radius = fmaxf(half[0], fmaxf(half[1], half[2]));
            float length_squared = direction[0]*direction[0] +
                direction[1]*direction[1] + direction[2]*direction[2];
            float projection, closest_squared;
            if (length_squared <= 0.00001f) continue;
            projection = -(offset[0]*direction[0] +
                           offset[1]*direction[1] +
                           offset[2]*direction[2]) / length_squared;
            projection = fminf(fmaxf(projection, 0.002f), 0.998f);
            closest_squared = 0.0f;
            for (axis = 0; axis < 3; axis++) {
                float separation = offset[axis] +
                                   projection * direction[axis];
                closest_squared += separation * separation;
            }
            if (closest_squared < radius * radius) return 1;
            continue;
        }
        {
            float local_point[3] = {point[0], point[1], point[2]};
            float local_light[3] = {light[0], light[1], light[2]};
            unrotate_shadow_point(local_point, box);
            unrotate_shadow_point(local_light, box);
            for (axis = 0; axis < 3; axis++) {
                float direction = local_light[axis] - local_point[axis];
                float low = -half[axis];
                float high = half[axis];
                float first, second;
                if (fabsf(direction) < 0.00001f) {
                    if (local_point[axis] < low || local_point[axis] > high) break;
                    continue;
                }
                first = (low - local_point[axis]) / direction;
                second = (high - local_point[axis]) / direction;
                if (first > second) {
                    float swap = first;
                    first = second;
                    second = swap;
                }
                if (first > entry) entry = first;
                if (second < leave) leave = second;
                if (entry > leave) break;
            }
        }
        if (axis == 3 && entry <= leave) return 1;
    }
    return 0;
}

static SDL_FColor lit_face_color(const ZSharpGameRenderFrame *frame,
                                 const ZSharpGameRenderObject *object,
                                 const ZSharpProjectedCubeFace *face) {
    SDL_FColor base = float_color_value(object->color);
    float illumination[3] = {0.20f, 0.20f, 0.20f};
    float glow = clamp_unit(object->emissive);
    size_t index;
    int has_light = 0;
    for (index = 0; index < frame->object_count; index++) {
        const ZSharpGameRenderObject *source = &frame->objects[index];
        float light[3] = {source->x, source->y, source->z};
        float direction[3], distance, incidence, attenuation, strength;
        SDL_FColor tint;
        int axis;
        if (!source->visible || source->shape != ZGAME_SHAPE_LIGHT) continue;
        has_light = 1;
        if (source->light_type == 3) {
            float yaw = source->rotation_y * ZGAME_PI / 180.0f;
            float pitch = source->rotation_x * ZGAME_PI / 180.0f;
            direction[0] = sinf(yaw) * cosf(pitch);
            direction[1] = -sinf(pitch);
            direction[2] = cosf(yaw) * cosf(pitch);
            distance = 1.0f;
            for (axis = 0; axis < 3; axis++)
                light[axis] = face->world_center[axis] + direction[axis] * 10000.0f;
        } else {
            for (axis = 0; axis < 3; axis++)
                direction[axis] = light[axis] - face->world_center[axis];
            distance = sqrtf(direction[0]*direction[0] + direction[1]*direction[1] +
                             direction[2]*direction[2]);
            if (distance < 0.001f) distance = 0.001f;
            if (source->light_range > 0.0f && distance > source->light_range) continue;
            for (axis = 0; axis < 3; axis++) direction[axis] /= distance;
        }
        incidence = face->world_normal[0]*direction[0] +
                    face->world_normal[1]*direction[1] +
                    face->world_normal[2]*direction[2];
        if (incidence <= 0.0f) continue;
        if (source->light_type == 2) {
            float yaw = source->rotation_y * ZGAME_PI / 180.0f;
            float pitch = source->rotation_x * ZGAME_PI / 180.0f;
            float forward[3] = {-sinf(yaw)*cosf(pitch), sinf(pitch),
                                 -cosf(yaw)*cosf(pitch)};
            float cone = -(forward[0]*direction[0] + forward[1]*direction[1] +
                           forward[2]*direction[2]);
            if (cone < cosf(source->light_angle * 0.5f * ZGAME_PI / 180.0f))
                continue;
        }
        if (source->cast_shadows &&
            light_occluded(frame, object, face->world_center, light)) continue;
        attenuation = source->light_type != 3 && source->light_range > 0.0f ?
            1.0f - distance / source->light_range : 1.0f;
        strength = (source->light_intensity / 100.0f) * incidence *
                   attenuation * attenuation;
        /* A small view-dependent highlight makes roughness and metallic
           scene overrides observable without changing the base tint. */
        if (object->metallic > 0.0f) {
            float view[3] = {frame->camera_x - face->world_center[0],
                             frame->camera_y - face->world_center[1],
                             frame->camera_z - face->world_center[2]};
            float view_length = sqrtf(view[0]*view[0]+view[1]*view[1]+view[2]*view[2]);
            float half_vector[3], half_length, highlight;
            if (view_length < 0.001f) view_length = 0.001f;
            for (axis = 0; axis < 3; axis++)
                half_vector[axis] = direction[axis] + view[axis] / view_length;
            half_length = sqrtf(half_vector[0]*half_vector[0] +
                                half_vector[1]*half_vector[1] +
                                half_vector[2]*half_vector[2]);
            if (half_length > 0.001f) {
                highlight = clamp_unit((face->world_normal[0]*half_vector[0] +
                    face->world_normal[1]*half_vector[1] +
                    face->world_normal[2]*half_vector[2]) / half_length);
                strength += object->metallic * powf(highlight,
                    2.0f + (1.0f - object->roughness) * 62.0f) *
                    (source->light_intensity / 100.0f) * attenuation;
            }
        }
        tint = float_color_value(source->color);
        illumination[0] += tint.r * strength;
        illumination[1] += tint.g * strength;
        illumination[2] += tint.b * strength;
    }
    if (!has_light) {
        illumination[0] = illumination[1] = illumination[2] = 1.0f;
    }
    base.r = clamp_unit(base.r * illumination[0] * face->brightness + glow * base.r);
    base.g = clamp_unit(base.g * illumination[1] * face->brightness + glow * base.g);
    base.b = clamp_unit(base.b * illumination[2] * face->brightness + glow * base.b);
    base.a = clamp_unit(object->opacity);
    return base;
}

static float raster_edge(float ax, float ay, float bx, float by,
                         float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static uint32_t raster_pack(float red, float green, float blue, float alpha) {
    uint32_t r = (uint32_t)(clamp_unit(red) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(clamp_unit(green) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(clamp_unit(blue) * 255.0f + 0.5f);
    uint32_t a = (uint32_t)(clamp_unit(alpha) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

static SDL_Surface *raster_surface(const ZSharpGameVulkan *renderer,
                                   const SDL_Texture *texture) {
    ZSharpTextureCache *entry;
    for (entry = renderer->textures; entry != NULL; entry = entry->next)
        if (entry->texture == texture) return entry->surface;
    return NULL;
}

static void raster_triangle(ZSharpGameVulkan *renderer,
                            const ZSharpProjectedCubeFace *face,
                            size_t a, size_t b, size_t c,
                            SDL_FColor tint, SDL_Surface *surface) {
    float ax = face->points[a][0], ay = face->points[a][1];
    float bx = face->points[b][0], by = face->points[b][1];
    float cx = face->points[c][0], cy = face->points[c][1];
    float area = raster_edge(ax, ay, bx, by, cx, cy);
    int x0, x1, y0, y1, x, y;
    if (!isfinite(area) || fabsf(area) < 0.00001f) return;
    x0 = (int)fmaxf(0.0f, floorf(fminf(ax, fminf(bx, cx))));
    x1 = (int)fminf(ZGAME_LOGICAL_WIDTH - 1.0f,
                    ceilf(fmaxf(ax, fmaxf(bx, cx))));
    y0 = (int)fmaxf(0.0f, floorf(fminf(ay, fminf(by, cy))));
    y1 = (int)fminf(ZGAME_LOGICAL_HEIGHT - 1.0f,
                    ceilf(fmaxf(ay, fmaxf(by, cy))));
    if (x0 > x1 || y0 > y1) return;
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) {
        float px = (float)x + 0.5f, py = (float)y + 0.5f;
        float wa = raster_edge(bx, by, cx, cy, px, py) / area;
        float wb = raster_edge(cx, cy, ax, ay, px, py) / area;
        float wc = 1.0f - wa - wb;
        float inverse_depth, depth, red = tint.r, green = tint.g,
              blue = tint.b, alpha = tint.a;
        size_t pixel;
        if (wa < -0.00001f || wb < -0.00001f || wc < -0.00001f)
            continue;
        inverse_depth = wa / face->point_depth[a] +
                        wb / face->point_depth[b] +
                        wc / face->point_depth[c];
        if (inverse_depth <= 0.0f) continue;
        depth = 1.0f / inverse_depth;
        pixel = (size_t)y * (size_t)ZGAME_LOGICAL_WIDTH + (size_t)x;
        if (depth >= renderer->depth_buffer[pixel]) continue;
        if (surface != NULL && surface->w > 0 && surface->h > 0) {
            float u = (wa*face->texcoords[a][0]/face->point_depth[a] +
                       wb*face->texcoords[b][0]/face->point_depth[b] +
                       wc*face->texcoords[c][0]/face->point_depth[c]) /
                      inverse_depth;
            float v = (wa*face->texcoords[a][1]/face->point_depth[a] +
                       wb*face->texcoords[b][1]/face->point_depth[b] +
                       wc*face->texcoords[c][1]/face->point_depth[c]) /
                      inverse_depth;
            int tx = (int)(clamp_unit(u) * (float)(surface->w - 1));
            int ty = (int)(clamp_unit(v) * (float)(surface->h - 1));
            const unsigned char *sample =
                (const unsigned char *)surface->pixels +
                (size_t)ty * (size_t)surface->pitch + (size_t)tx * 4;
            red *= (float)sample[0] / 255.0f;
            green *= (float)sample[1] / 255.0f;
            blue *= (float)sample[2] / 255.0f;
            alpha *= (float)sample[3] / 255.0f;
        }
        if (alpha <= 0.0f) continue;
        if (alpha < 0.999f) {
            uint32_t behind = renderer->color_buffer[pixel];
            red = red * alpha + ((behind & 255u) / 255.0f) * (1.0f-alpha);
            green = green * alpha +
                (((behind >> 8) & 255u) / 255.0f) * (1.0f-alpha);
            blue = blue * alpha +
                (((behind >> 16) & 255u) / 255.0f) * (1.0f-alpha);
        } else {
            renderer->depth_buffer[pixel] = depth;
        }
        renderer->color_buffer[pixel] = raster_pack(red, green, blue, 1.0f);
    }
}

static void raster_face(ZSharpGameVulkan *renderer,
                        const ZSharpProjectedCubeFace *face,
                        SDL_FColor color, SDL_Texture *texture) {
    SDL_Surface *surface = texture == NULL ? NULL :
        raster_surface(renderer, texture);
    size_t corner;
    for (corner = 1; corner + 1 < face->point_count; corner++)
        raster_triangle(renderer, face, 0, corner, corner + 1,
                        color, surface);
}

static int render_cube(SDL_Renderer *renderer, SDL_Texture *texture,
                       const ZSharpGameRenderFrame *frame,
                       const ZSharpGameRenderObject *object) {
    ZSharpProjectedCubeFace faces[6];
    size_t count = zsharp_game_project_cube_faces(frame, object, faces);
    size_t face_index;
    for (face_index = 0; face_index < count; face_index++) {
        const ZSharpProjectedCubeFace *face = &faces[face_index];
        SDL_Vertex vertices[6];
        int indices[12];
        SDL_FColor color = lit_face_color(frame, object, face);
        size_t index;
        memset(vertices, 0, sizeof(vertices));
        for (index = 0; index < face->point_count; index++) {
            vertices[index].position.x = face->points[index][0];
            vertices[index].position.y = face->points[index][1];
            vertices[index].color = color;
            vertices[index].tex_coord.x = face->texcoords[index][0];
            vertices[index].tex_coord.y = face->texcoords[index][1];
        }
        for (index = 0; index + 2 < face->point_count; index++) {
            indices[index * 3] = 0;
            indices[index * 3 + 1] = (int)index + 1;
            indices[index * 3 + 2] = (int)index + 2;
        }
        if (!SDL_RenderGeometry(renderer, texture, vertices,
                                (int)face->point_count, indices,
                                ((int)face->point_count - 2) * 3)) return 0;
    }
    return 1;
}

static void render_cube_depth(ZSharpGameVulkan *renderer,
                              SDL_Texture *texture,
                              const ZSharpGameRenderFrame *frame,
                              const ZSharpGameRenderObject *object) {
    ZSharpProjectedCubeFace faces[6];
    size_t count = zsharp_game_project_cube_faces(frame, object, faces);
    size_t index;
    for (index = 0; index < count; index++)
        raster_face(renderer, &faces[index],
                    lit_face_color(frame, object, &faces[index]), texture);
}

typedef struct ZSharpProjectedMeshTriangle {
    ZSharpProjectedCubeFace face;
    SDL_Texture *texture;
} ZSharpProjectedMeshTriangle;

static int compare_mesh_faces(const void *left, const void *right) {
    const ZSharpProjectedMeshTriangle *a =
        (const ZSharpProjectedMeshTriangle *)left;
    const ZSharpProjectedMeshTriangle *b =
        (const ZSharpProjectedMeshTriangle *)right;
    return a->face.depth < b->face.depth ? 1 :
           a->face.depth > b->face.depth ? -1 : 0;
}

static void rotate_mesh_point(float point[3],
                              const ZSharpGameRenderObject *object,
                              const ZSharpMeshCache *mesh) {
    float angle, sine, cosine, first, second;
    point[0] *= object->width * object->scale_x / mesh->source_extent[0];
    point[1] *= object->height * object->scale_y / mesh->source_extent[1];
    point[2] *= object->depth * object->scale_z / mesh->source_extent[2];
    angle = object->rotation_x * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[1]*cosine-point[2]*sine;
    second = point[1]*sine+point[2]*cosine;
    point[1] = first; point[2] = second;
    angle = (object->rotation_y + object->rotation) * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[0]*cosine+point[2]*sine;
    second = -point[0]*sine+point[2]*cosine;
    point[0] = first; point[2] = second;
    angle = object->rotation_z * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    first = point[0]*cosine-point[1]*sine;
    second = point[0]*sine+point[1]*cosine;
    point[0] = first + object->x;
    point[1] = second + object->y;
    point[2] += object->z;
}

static void animate_mesh_part(float point[3],
                              const ZSharpCachedTriangle *triangle,
                              const ZSharpGameRenderObject *object) {
    size_t index;
    const ufbx_string *node_name = &triangle->node->name;
    for (index = 0; index < object->part_pose_count; index++) {
        const ZSharpGamePartPose *pose = &object->part_poses[index];
        float angle, sine, cosine, first, second;
        if (pose->name == NULL ||
            strlen(pose->name) != node_name->length ||
            memcmp(pose->name, node_name->data, node_name->length) != 0)
            continue;
        point[0] -= triangle->pivot[0];
        point[1] -= triangle->pivot[1];
        point[2] -= triangle->pivot[2];
        angle = pose->rotation[0] * ZGAME_PI / 180.0f;
        sine = sinf(angle); cosine = cosf(angle);
        first = point[1]*cosine-point[2]*sine;
        second = point[1]*sine+point[2]*cosine;
        point[1] = first; point[2] = second;
        angle = pose->rotation[1] * ZGAME_PI / 180.0f;
        sine = sinf(angle); cosine = cosf(angle);
        first = point[0]*cosine+point[2]*sine;
        second = -point[0]*sine+point[2]*cosine;
        point[0] = first; point[2] = second;
        angle = pose->rotation[2] * ZGAME_PI / 180.0f;
        sine = sinf(angle); cosine = cosf(angle);
        first = point[0]*cosine-point[1]*sine;
        second = point[0]*sine+point[1]*cosine;
        point[0] = first + triangle->pivot[0] + pose->position[0];
        point[1] = second + triangle->pivot[1] + pose->position[1];
        point[2] += triangle->pivot[2] + pose->position[2];
        return;
    }
}

static const char *mesh_texture_path(const ZSharpGameRenderObject *object,
                                     const ufbx_material *material) {
    size_t index;
    if (material != NULL) {
        for (index = 0; index < object->material_count; index++) {
            const char *slot = object->material_names[index];
            if (slot != NULL && strlen(slot) == material->name.length &&
                memcmp(slot, material->name.data, material->name.length) == 0)
                return object->material_textures[index];
        }
    }
    return object->asset_path;
}

static int compare_bone_overrides(const void *left, const void *right) {
    const ufbx_transform_override *a = (const ufbx_transform_override *)left;
    const ufbx_transform_override *b = (const ufbx_transform_override *)right;
    return (a->node_id > b->node_id) - (a->node_id < b->node_id);
}

/* ufbx evaluates the entire bone hierarchy and performs weighted vertex
   skinning. This keeps child bones and vertices with multiple influences in
   sync, rather than rotating each mesh triangle around a single pivot. */
static ufbx_scene *evaluate_bone_pose(const ZSharpMeshCache *mesh,
                                      const ZSharpGameRenderObject *object,
                                      char *error, size_t error_size) {
    ufbx_transform_override *overrides;
    ufbx_anim_opts options = {0};
    ufbx_evaluate_opts evaluate_options = {0};
    ufbx_anim *animation;
    ufbx_scene *evaluated;
    ufbx_error detail;
    size_t node_index, count = 0;
    if (object->part_pose_count == 0) return NULL;
    overrides = (ufbx_transform_override *)calloc(mesh->scene->nodes.count,
                                                   sizeof(*overrides));
    if (overrides == NULL) {
        renderer_error(error, error_size, "out of memory posing FBX bones");
        return NULL;
    }
    for (node_index = 0; node_index < mesh->scene->nodes.count; node_index++) {
        const ufbx_node *node = mesh->scene->nodes.data[node_index];
        size_t pose_index;
        if (node->bone == NULL) continue;
        for (pose_index = 0; pose_index < object->part_pose_count; pose_index++) {
            const ZSharpGamePartPose *pose = &object->part_poses[pose_index];
            ufbx_vec3 angles;
            ufbx_quat offset;
            if (pose->name == NULL ||
                strlen(pose->name) != node->name.length ||
                memcmp(pose->name, node->name.data, node->name.length) != 0)
                continue;
            overrides[count].node_id = node->typed_id;
            overrides[count].transform = node->local_transform;
            overrides[count].transform.translation.x += pose->position[0];
            overrides[count].transform.translation.y += pose->position[1];
            overrides[count].transform.translation.z += pose->position[2];
            angles.x = pose->rotation[0];
            angles.y = pose->rotation[1];
            angles.z = pose->rotation[2];
            offset = ufbx_euler_to_quat(angles, node->rotation_order);
            overrides[count].transform.rotation = ufbx_quat_mul(
                node->local_transform.rotation, offset);
            count++;
            break;
        }
    }
    if (count == 0) { free(overrides); return NULL; }
    qsort(overrides, count, sizeof(*overrides), compare_bone_overrides);
    options.transform_overrides.data = overrides;
    options.transform_overrides.count = count;
    animation = ufbx_create_anim(mesh->scene, &options, &detail);
    free(overrides);
    if (animation == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "could not pose FBX bones: %.180s",
                     detail.description.data);
        return NULL;
    }
    evaluate_options.evaluate_skinning = true;
    evaluated = ufbx_evaluate_scene(mesh->scene, animation, 0.0,
                                    &evaluate_options, &detail);
    ufbx_free_anim(animation);
    if (evaluated == NULL && error != NULL && error_size > 0)
        snprintf(error, error_size, "could not skin FBX mesh: %.180s",
                 detail.description.data);
    return evaluated;
}

static int render_mesh(ZSharpGameVulkan *renderer,
                       const ZSharpGameRenderFrame *frame,
                       const ZSharpGameRenderObject *object,
                       char *error, size_t error_size) {
    ZSharpMeshCache *mesh = load_mesh(renderer, frame, object->mesh_path,
                                      error, error_size);
    ZSharpProjectedMeshTriangle *faces;
    ufbx_scene *posed = NULL;
    char pose_error[256] = {0};
    size_t face_count = 0, index;
    if (mesh == NULL) return 0;
    posed = evaluate_bone_pose(mesh, object, pose_error, sizeof(pose_error));
    if (pose_error[0] != '\0') {
        renderer_error(error, error_size, pose_error);
        return 0;
    }
    faces = (ZSharpProjectedMeshTriangle *)calloc(mesh->triangle_count,
                                                   sizeof(*faces));
    if (faces == NULL) {
        if (posed != NULL) ufbx_free_scene(posed);
        renderer_error(error, error_size, "out of memory projecting FBX mesh");
        return 0;
    }
    for (index = 0; index < mesh->triangle_count; index++) {
        const ZSharpCachedTriangle *triangle = &mesh->triangles[index];
        float points[3][3];
        const char *texture_path;
        int corner;
        for (corner = 0; corner < 3; corner++) {
            if (posed != NULL && triangle->node->mesh != NULL &&
                triangle->node->mesh->skin_deformers.count > 0) {
                const ufbx_node *node =
                    posed->nodes.data[triangle->node->typed_id];
                const ufbx_mesh *skinned = node->mesh;
                ufbx_vec3 vertex = ufbx_get_vertex_vec3(
                    &skinned->skinned_position,
                    triangle->vertex_indices[corner]);
                if (skinned->skinned_is_local)
                    vertex = ufbx_transform_position(
                        &node->geometry_to_world, vertex);
                points[corner][0] = (float)vertex.x;
                points[corner][1] = (float)vertex.y;
                points[corner][2] = (float)vertex.z;
            } else {
                memcpy(points[corner], triangle->points[corner],
                       sizeof(points[corner]));
                animate_mesh_part(points[corner], triangle, object);
            }
            rotate_mesh_point(points[corner], object, mesh);
        }
        if (!zsharp_game_project_mesh_triangle(frame,
            (const float (*)[3])points, triangle->uv,
            &faces[face_count].face)) continue;
        texture_path = mesh_texture_path(object, triangle->material);
        if (texture_path != NULL && texture_path[0] != '\0') {
            faces[face_count].texture = load_sprite(renderer, frame,
                texture_path, error, error_size);
            if (faces[face_count].texture == NULL) {
                if (posed != NULL) ufbx_free_scene(posed);
                free(faces);
                return 0;
            }
        }
        face_count++;
    }
    qsort(faces, face_count, sizeof(*faces), compare_mesh_faces);
    for (index = 0; index < face_count; index++) {
        const ZSharpProjectedCubeFace *face = &faces[index].face;
        SDL_Vertex vertices[6];
        int indices[12];
        SDL_FColor color = lit_face_color(frame, object, face);
        size_t corner;
        if (renderer->depth_active) {
            raster_face(renderer, face, color, faces[index].texture);
            continue;
        }
        memset(vertices, 0, sizeof(vertices));
        for (corner = 0; corner < face->point_count; corner++) {
            vertices[corner].position.x = face->points[corner][0];
            vertices[corner].position.y = face->points[corner][1];
            vertices[corner].tex_coord.x = face->texcoords[corner][0];
            vertices[corner].tex_coord.y = face->texcoords[corner][1];
            vertices[corner].color = color;
        }
        for (corner = 0; corner + 2 < face->point_count; corner++) {
            indices[corner * 3] = 0;
            indices[corner * 3 + 1] = (int)corner + 1;
            indices[corner * 3 + 2] = (int)corner + 2;
        }
        if (!SDL_RenderGeometry(renderer->renderer, faces[index].texture,
                vertices, (int)face->point_count, indices,
                ((int)face->point_count - 2) * 3)) {
            if (posed != NULL) ufbx_free_scene(posed);
            free(faces);
            return 0;
        }
    }
    free(faces);
    if (posed != NULL) ufbx_free_scene(posed);
    return 1;
}

static int render_before(const ZSharpGameRenderFrame *frame,
                         const ZSharpGameRenderObject *a,
                         const ZSharpGameRenderObject *b) {
    float a_depth, b_depth;
    if (a->layer != b->layer) return a->layer < b->layer;
    if (!frame->is_3d) return a->z > b->z;
    /* A long floor's center can pass behind the player even while much of
       that floor extends farther away. Drawing by its backmost corner keeps
       the floor underneath foreground actors instead of overpainting them. */
    a_depth = zsharp_game_camera_far_depth(frame, a);
    b_depth = zsharp_game_camera_far_depth(frame, b);
    return a_depth > b_depth;
}

static SDL_Texture *load_sprite(ZSharpGameVulkan *renderer,
                                const ZSharpGameRenderFrame *frame,
                                const char *relative, char *error,
                                size_t error_size) {
    ZSharpTextureCache *cached;
    size_t root_length;
    size_t relative_length;
    char *path;
    SDL_Surface *surface;
    SDL_Texture *texture;
    if (relative == NULL || relative[0] == '\0' ||
        frame->project_root == NULL) {
        renderer_error(error, error_size,
                       "textured objects require an asset path");
        return NULL;
    }
    root_length = strlen(frame->project_root);
    relative_length = strlen(relative);
    path = (char *)malloc(root_length + relative_length + 2);
    if (path == NULL) {
        renderer_error(error, error_size, "out of memory");
        return NULL;
    }
    snprintf(path, root_length + relative_length + 2, "%s/%s",
             frame->project_root, relative);
    for (cached = renderer->textures; cached != NULL; cached = cached->next) {
        if (strcmp(cached->path, path) == 0) {
            free(path);
            return cached->texture;
        }
    }
    surface = SDL_LoadBMP(path);
    if (surface == NULL) surface = SDL_LoadPNG(path);
    if (surface == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not load sprite '%s': %s",
                     relative, SDL_GetError());
        free(path);
        return NULL;
    }
    {
        SDL_Surface *converted = SDL_ConvertSurface(surface,
                                                     SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        surface = converted;
    }
    if (surface == NULL) {
        renderer_error(error, error_size, SDL_GetError());
        free(path);
        return NULL;
    }
    texture = SDL_CreateTextureFromSurface(renderer->renderer, surface);
    if (texture == NULL) {
        renderer_error(error, error_size, SDL_GetError());
        SDL_DestroySurface(surface);
        free(path);
        return NULL;
    }
    cached = (ZSharpTextureCache *)calloc(1, sizeof(*cached));
    if (cached == NULL) {
        SDL_DestroyTexture(texture);
        SDL_DestroySurface(surface);
        free(path);
        renderer_error(error, error_size, "out of memory");
        return NULL;
    }
    cached->path = path;
    cached->texture = texture;
    cached->surface = surface;
    cached->next = renderer->textures;
    renderer->textures = cached;
    return texture;
}

ZSharpGameVulkan *zsharp_game_vulkan_create(SDL_Window *window, char *error,
                                             size_t error_size) {
    ZSharpGameVulkan *renderer;
    const char *driver;
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
    renderer = (ZSharpGameVulkan *)calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        renderer_error(error, error_size, "out of memory");
        return NULL;
    }
    renderer->window = window;
    renderer->renderer = SDL_CreateRenderer(window, "vulkan");
    if (renderer->renderer == NULL) {
        renderer_error(error, error_size, SDL_GetError());
        free(renderer);
        return NULL;
    }
    driver = SDL_GetRendererName(renderer->renderer);
    snprintf(renderer->driver, sizeof(renderer->driver), "%s",
             driver == NULL ? "vulkan" : driver);
    if (strstr(renderer->driver, "vulkan") == NULL) {
        renderer_error(error, error_size,
                       "SDL created a non-Vulkan game renderer");
        zsharp_game_vulkan_destroy(renderer);
        return NULL;
    }
    if (!SDL_SetRenderLogicalPresentation(
            renderer->renderer, (int)ZGAME_LOGICAL_WIDTH,
            (int)ZGAME_LOGICAL_HEIGHT,
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        renderer_error(error, error_size, SDL_GetError());
        zsharp_game_vulkan_destroy(renderer);
        return NULL;
    }
    SDL_SetRenderDrawBlendMode(renderer->renderer, SDL_BLENDMODE_BLEND);
    return renderer;
}

static int ensure_depth_buffer(ZSharpGameVulkan *renderer,
                               char *error, size_t error_size) {
    size_t pixels = (size_t)ZGAME_LOGICAL_WIDTH *
                    (size_t)ZGAME_LOGICAL_HEIGHT;
    if (renderer->depth_texture != NULL && renderer->color_buffer != NULL &&
        renderer->depth_buffer != NULL) return 1;
    renderer->depth_texture = SDL_CreateTexture(renderer->renderer,
        SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        (int)ZGAME_LOGICAL_WIDTH, (int)ZGAME_LOGICAL_HEIGHT);
    renderer->color_buffer = (uint32_t *)malloc(pixels * sizeof(uint32_t));
    renderer->depth_buffer = (float *)malloc(pixels * sizeof(float));
    if (renderer->depth_texture == NULL || renderer->color_buffer == NULL ||
        renderer->depth_buffer == NULL) {
        renderer_error(error, error_size, "could not allocate 3D depth buffer");
        if (renderer->depth_texture != NULL)
            SDL_DestroyTexture(renderer->depth_texture);
        free(renderer->color_buffer);
        free(renderer->depth_buffer);
        renderer->depth_texture = NULL;
        renderer->color_buffer = NULL;
        renderer->depth_buffer = NULL;
        return 0;
    }
    return 1;
}

int zsharp_game_vulkan_draw(ZSharpGameVulkan *renderer, int resized,
                            const ZSharpGameRenderFrame *frame,
                            char *error, size_t error_size) {
    ZSharpGameRenderObject *ordered = NULL;
    size_t index;
    (void)resized;
    if (renderer == NULL || renderer->renderer == NULL || frame == NULL) {
        renderer_error(error, error_size, "invalid Vulkan frame");
        return 0;
    }
    set_color(renderer->renderer, frame->background);
    if (!SDL_RenderClear(renderer->renderer)) goto failed;
    if (frame->object_count != 0) {
        ordered = (ZSharpGameRenderObject *)malloc(
            frame->object_count * sizeof(*ordered));
        if (ordered == NULL) {
            renderer_error(error, error_size, "out of memory");
            return 0;
        }
        memcpy(ordered, frame->objects,
               frame->object_count * sizeof(*ordered));
        /* qsort cannot receive the active camera, so use a small stable sort.
           Games normally draw tens or hundreds of objects and correct
           camera-space ordering matters more than world-Z ordering. */
        for (index = 1; index < frame->object_count; index++) {
            ZSharpGameRenderObject value = ordered[index];
            size_t position = index;
            while (position > 0 &&
                   render_before(frame, &value, &ordered[position - 1])) {
                ordered[position] = ordered[position - 1];
                position--;
            }
            ordered[position] = value;
        }
    }
    if (frame->is_3d) {
        size_t pixel, pixels =
            (size_t)ZGAME_LOGICAL_WIDTH * (size_t)ZGAME_LOGICAL_HEIGHT;
        if (!ensure_depth_buffer(renderer, error, error_size)) {
            free(ordered);
            return 0;
        }
        uint32_t background = raster_pack(
            (float)((frame->background >> 16) & 255u) / 255.0f,
            (float)((frame->background >> 8) & 255u) / 255.0f,
            (float)(frame->background & 255u) / 255.0f, 1.0f);
        for (pixel = 0; pixel < pixels; pixel++) {
            renderer->color_buffer[pixel] = background;
            renderer->depth_buffer[pixel] = INFINITY;
        }
        renderer->depth_active = 1;
        for (index = 0; index < frame->object_count; index++) {
            const ZSharpGameRenderObject *object = &ordered[index];
            SDL_Texture *texture = NULL;
            if (!object->visible) continue;
            if (object->shape == ZGAME_SHAPE_MESH) {
                if (!render_mesh(renderer, frame, object, error, error_size))
                    goto failed;
            } else if (object->shape == ZGAME_SHAPE_CUBE) {
                if (object->asset_path != NULL && object->asset_path[0] != '\0') {
                    texture = load_sprite(renderer, frame, object->asset_path,
                                          error, error_size);
                    if (texture == NULL) goto failed;
                }
                render_cube_depth(renderer, texture, frame, object);
            }
        }
        renderer->depth_active = 0;
        if (!SDL_UpdateTexture(renderer->depth_texture, NULL,
                               renderer->color_buffer,
                               (int)ZGAME_LOGICAL_WIDTH * 4) ||
            !SDL_RenderTexture(renderer->renderer, renderer->depth_texture,
                               NULL, NULL)) goto failed;
    }
    for (index = 0; index < frame->object_count; index++) {
        const ZSharpGameRenderObject *object = &ordered[index];
        int ok = 1;
        if (!object->visible || object->shape == ZGAME_SHAPE_LIGHT ||
            object->shape == ZGAME_SHAPE_NAV) continue;
        if (frame->is_3d && (object->shape == ZGAME_SHAPE_MESH ||
                             object->shape == ZGAME_SHAPE_CUBE)) continue;
        if (object->shape == ZGAME_SHAPE_MESH) {
            ok = render_mesh(renderer, frame, object, error, error_size);
        } else if (object->shape == ZGAME_SHAPE_CUBE) {
            SDL_Texture *texture = NULL;
            if (object->asset_path != NULL && object->asset_path[0] != '\0') {
                texture = load_sprite(renderer, frame, object->asset_path,
                                      error, error_size);
                if (texture == NULL) goto failed;
            }
            ok = render_cube(renderer->renderer, texture, frame, object);
        } else if (object->shape == ZGAME_SHAPE_CIRCLE) {
            ok = render_circle(renderer->renderer, object,
                               frame->camera_x, frame->camera_y);
        } else if (object->shape == ZGAME_SHAPE_TRIANGLE) {
            ok = render_triangle(renderer->renderer, object,
                                 frame->camera_x, frame->camera_y);
        } else if (object->shape == ZGAME_SHAPE_SPRITE) {
            SDL_Texture *texture = load_sprite(renderer, frame,
                                               object->asset_path, error,
                                               error_size);
            SDL_FRect destination;
            if (texture == NULL) goto failed;
            destination.w = object->width * object->scale_x;
            destination.h = object->height * object->scale_y;
            destination.x = screen_x(object->x, frame->camera_x) -
                            destination.w * 0.5f;
            destination.y = screen_y(object->y, frame->camera_y) -
                            destination.h * 0.5f;
            ok = SDL_RenderTextureRotated(
                renderer->renderer, texture, NULL, &destination,
                -object->rotation, NULL, SDL_FLIP_NONE);
        } else if (object->shape == ZGAME_SHAPE_TEXT) {
            float scale = object->scale_x <= 0.0f ? 1.0f : object->scale_x;
            float x = screen_x(object->x, frame->camera_x);
            float y = screen_y(object->y, frame->camera_y);
            set_color(renderer->renderer, object->color);
            SDL_SetRenderScale(renderer->renderer, scale, scale);
            ok = SDL_RenderDebugText(
                renderer->renderer, x / scale, y / scale,
                object->text == NULL ? "" : object->text);
            SDL_SetRenderScale(renderer->renderer, 1.0f, 1.0f);
        } else {
            ok = render_rectangle(renderer->renderer, object,
                                  frame->camera_x, frame->camera_y);
        }
        if (!ok) goto failed;
    }
    free(ordered);
    if (!SDL_RenderPresent(renderer->renderer)) goto failed_without_objects;
    return 1;
failed:
    free(ordered);
failed_without_objects:
    renderer_error(error, error_size, SDL_GetError());
    return 0;
}

const char *zsharp_game_vulkan_driver(const ZSharpGameVulkan *renderer) {
    return renderer == NULL ? "unavailable" : renderer->driver;
}

void zsharp_game_vulkan_destroy(ZSharpGameVulkan *renderer) {
    ZSharpTextureCache *texture;
    ZSharpMeshCache *mesh;
    if (renderer == NULL) return;
    if (renderer->depth_texture != NULL)
        SDL_DestroyTexture(renderer->depth_texture);
    free(renderer->color_buffer);
    free(renderer->depth_buffer);
    texture = renderer->textures;
    while (texture != NULL) {
        ZSharpTextureCache *next = texture->next;
        SDL_DestroyTexture(texture->texture);
        SDL_DestroySurface(texture->surface);
        free(texture->path);
        free(texture);
        texture = next;
    }
    mesh = renderer->meshes;
    while (mesh != NULL) {
        ZSharpMeshCache *next = mesh->next;
        ufbx_free_scene(mesh->scene);
        free(mesh->triangles);
        free(mesh->path);
        free(mesh);
        mesh = next;
    }
    if (renderer->renderer != NULL) SDL_DestroyRenderer(renderer->renderer);
    free(renderer);
}

#endif
