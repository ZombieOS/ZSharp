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
#include "game_gpu.h"

typedef struct ShadowNode {
    float low[3], high[3];
    size_t begin, count, left, right;
} ShadowNode;
typedef struct RenderEntry {
    const ZSharpGameRenderObject *object;
    float depth;
} RenderEntry;
typedef struct FaceLightCache {
    float key[10];
    SDL_FColor color;
    int valid;
} FaceLightCache;
typedef struct ShadowIndex {
    ShadowNode *nodes;
    const ZSharpGameRenderObject **objects;
    size_t capacity, count, node_count;
    size_t *indices, previous_count;
    unsigned char *previous_keys;
    unsigned char *previous_occluder_keys;
    size_t rebuild_count;
    FaceLightCache *face_colors;
    unsigned char *moving_lights, *shadow_results;
    size_t *light_slots, shadow_light_count;
    FaceLightCache *shadow_face_keys;
    int uncached_shadows; /* opt-in differential profiling/verification */
} ShadowIndex;

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
    GameGPU gpu;
    int captured;
    const ZSharpGameRenderObject **lights;
    size_t light_capacity;
    ShadowIndex shadows;
    RenderEntry *ordered, *sort_scratch;
    size_t order_capacity;
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

static void unrotate_shadow_point(float point[3],float other[3],
                                  const ZSharpGameRenderObject *object) {
    float angle, sine, cosine, first, second;
    int index;
    other[0] -= object->x;
    other[1] -= object->y;
    other[2] -= object->z;
    point[0] -= object->x;
    point[1] -= object->y;
    point[2] -= object->z;
    if(object->rotation_x==0 && object->rotation_y+object->rotation==0 && object->rotation_z==0) return;
    angle = -object->rotation_z * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    for(index=0;index<2;index++) {
    float *p=index?other:point;
    first = p[0]*cosine-p[1]*sine;
    second = p[0]*sine+p[1]*cosine;
    p[0] = first; p[1] = second;
    }
    angle = -(object->rotation_y + object->rotation) *
            ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    for(index=0;index<2;index++) {
    float *p=index?other:point;
    first = p[0]*cosine+p[2]*sine;
    second = -p[0]*sine+p[2]*cosine;
    p[0] = first; p[2] = second;
    }
    angle = -object->rotation_x * ZGAME_PI / 180.0f;
    sine = sinf(angle); cosine = cosf(angle);
    for(index=0;index<2;index++) {
    float *p=index?other:point;
    first = p[1]*cosine-p[2]*sine;
    second = p[1]*sine+p[2]*cosine;
    p[1] = first; p[2] = second;
    }
}

/* Coarse real-time shadows: boxes intersect an oriented volume; models use
   a rotation-invariant bounding sphere. Exact model shadow silhouettes would
   require a separate shadow map or per-triangle ray intersections. */
static int light_occluded_impl(const ZSharpGameRenderFrame *frame,
                          const ZSharpGameRenderObject *self,
                          const float point[3], const float light[3],int broadphase) {
    size_t index;
    float ray[3]={light[0]-point[0],light[1]-point[1],light[2]-point[2]};
    float ray_length=ray[0]*ray[0]+ray[1]*ray[1]+ray[2]*ray[2];
    if(ray_length<=.00001f)return 0;
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
        /* Conservative segment/sphere rejection before expensive OBB
           transforms. The half-diagonal contains every rotated cube. */
        if(broadphase) {
            float offset[3]={point[0]-box->x,point[1]-box->y,point[2]-box->z};
            float t=-(offset[0]*ray[0]+offset[1]*ray[1]+offset[2]*ray[2])/ray_length;
            float radius2=half[0]*half[0]+half[1]*half[1]+half[2]*half[2];
            float distance2=0;
            t=fminf(fmaxf(t,.002f),.998f);
            for(axis=0;axis<3;axis++) {float d=offset[axis]+t*ray[axis];distance2+=d*d;}
            if(distance2>radius2+.0001f)continue;
        }
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
            unrotate_shadow_point(local_point, local_light, box);
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

static float shadow_center(const ZSharpGameRenderObject *o, int axis) {
    return axis == 0 ? o->x : axis == 1 ? o->y : o->z;
}

/* Conservative sphere bounds retain the existing rotated-box and mesh
   narrow phase. Rebuild from the current snapshot, including moving casters. */
static size_t shadow_build_node(ShadowIndex *tree, size_t begin, size_t count) {
    size_t node_id = tree->node_count++, i, split;
    ShadowNode *node = &tree->nodes[node_id];
    float centers_low[3]={FLT_MAX,FLT_MAX,FLT_MAX}, centers_high[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    int axis, longest = 0;
    for(axis=0;axis<3;axis++){node->low[axis]=FLT_MAX;node->high[axis]=-FLT_MAX;}
    node->begin=begin;node->count=count;
    for(i=begin;i<begin+count;i++) {
        const ZSharpGameRenderObject *o=tree->objects[i];
        float x=o->width*o->scale_x*.5f,y=o->height*o->scale_y*.5f,z=o->depth*o->scale_z*.5f;
        float radius=sqrtf(x*x+y*y+z*z)+.001f;
        for(axis=0;axis<3;axis++) {
            float center=shadow_center(o,axis);
            float extent=radius;
            if(o->shape==ZGAME_SHAPE_CUBE && o->rotation_x==0 && o->rotation_y+o->rotation==0 && o->rotation_z==0)
                extent=fabsf(axis==0?x:axis==1?y:z)+.001f;
            centers_low[axis]=fminf(centers_low[axis],center);
            centers_high[axis]=fmaxf(centers_high[axis],center);
            node->low[axis]=fminf(node->low[axis],center-extent);
            node->high[axis]=fmaxf(node->high[axis],center+extent);
        }
    }
    if(count<=8)return node_id;
    for(axis=1;axis<3;axis++)if(centers_high[axis]-centers_low[axis]>centers_high[longest]-centers_low[longest])longest=axis;
    {
        float middle=(centers_high[longest]+centers_low[longest])*.5f;
        split=begin;
        for(i=begin;i<begin+count;i++)if(shadow_center(tree->objects[i],longest)<middle) {
            const ZSharpGameRenderObject *swap=tree->objects[split];
            tree->objects[split++]=tree->objects[i];tree->objects[i]=swap;
        }
    }
    /* Bound recursion even for strongly clustered or coincident placements. */
    if(split-begin<count/4 || split-begin>count-count/4)split=begin+count/2;
    node->count=0;
    node->left=shadow_build_node(tree,begin,split-begin);
    node->right=shadow_build_node(tree,split,begin+count-split);
    return node_id;
}

static int shadow_prepare(ShadowIndex *tree,const ZSharpGameRenderFrame *frame) {
    size_t i;
    size_t key_size=offsetof(ZSharpGameRenderObject,text);
    int changed=tree->previous_count!=frame->object_count;
    int geometry_changed=changed;
    int stationary_light_changed=changed;
    size_t light_count=0;
    for(i=0;i<frame->object_count;i++)if(frame->objects[i].shape==ZGAME_SHAPE_LIGHT)light_count++;
    if(frame->object_count>tree->capacity) {
        size_t capacity=frame->object_count;
        ShadowNode *nodes=malloc((capacity*2+1)*sizeof(*nodes));
        const ZSharpGameRenderObject **objects=malloc(capacity*sizeof(*objects));
        size_t *indices=malloc(capacity*sizeof(*indices));
        unsigned char *keys=calloc(capacity,key_size);
        unsigned char *occluder_keys=calloc(capacity,key_size);
        FaceLightCache *colors=calloc(capacity*6,sizeof(*colors));
        unsigned char *moving=calloc(capacity,1);
        size_t *slots=calloc(capacity,sizeof(*slots));
        FaceLightCache *shadow_keys=calloc(capacity*6,sizeof(*shadow_keys));
        if(!nodes||!objects||!indices||!keys||!occluder_keys||!colors||!moving||!slots||!shadow_keys){free(nodes);free(objects);free(indices);free(keys);free(occluder_keys);free(colors);free(moving);free(slots);free(shadow_keys);return 0;}
        free(tree->nodes);free(tree->objects);
        free(tree->indices);free(tree->previous_keys);free(tree->face_colors);
        free(tree->previous_occluder_keys);
        free(tree->moving_lights);free(tree->light_slots);free(tree->shadow_face_keys);
        free(tree->shadow_results);tree->shadow_results=NULL;tree->shadow_light_count=0;
        tree->nodes=nodes;tree->objects=objects;tree->capacity=capacity;
        tree->indices=indices;tree->previous_keys=keys;tree->face_colors=colors;
        tree->previous_occluder_keys=occluder_keys;
        tree->moving_lights=moving;tree->light_slots=slots;tree->shadow_face_keys=shadow_keys;
        changed=geometry_changed=1;
    }
    /* Compare values, never snapshot pointers or camera state. Invisible
       players cannot change shadows. Visible moving casters and lights do. */
    for(i=0;i<frame->object_count;i++) {
        ZSharpGameRenderObject key={0};
        ZSharpGameRenderObject occluder={0};
        const ZSharpGameRenderObject *o=&frame->objects[i];
        if(o->visible&&(o->shape==ZGAME_SHAPE_CUBE||o->shape==ZGAME_SHAPE_MESH||o->shape==ZGAME_SHAPE_LIGHT)) {
            if(o->shape==ZGAME_SHAPE_LIGHT && o->light_intensity==0) {
                /* Presence still affects ambient mode; transform/color do not. */
                key.shape=ZGAME_SHAPE_LIGHT;key.visible=1;
            } else memcpy(&key,o,key_size);
        }
        if(o->visible && o->opacity>=.99f &&
           (o->shape==ZGAME_SHAPE_CUBE||o->shape==ZGAME_SHAPE_MESH)) {
            occluder.shape=o->shape;occluder.visible=1;occluder.render_id=o->render_id;
            occluder.x=o->x;occluder.y=o->y;occluder.z=o->z;
            occluder.width=o->width;occluder.height=o->height;occluder.depth=o->depth;
            occluder.rotation=o->rotation;occluder.rotation_x=o->rotation_x;
            occluder.rotation_y=o->rotation_y;occluder.rotation_z=o->rotation_z;
            occluder.scale_x=o->scale_x;occluder.scale_y=o->scale_y;occluder.scale_z=o->scale_z;
        }
        if(memcmp(tree->previous_occluder_keys+i*key_size,&occluder,key_size))geometry_changed=1;
        memcpy(tree->previous_occluder_keys+i*key_size,&occluder,key_size);
        if(memcmp(tree->previous_keys+i*key_size,&key,key_size)) {
            changed=1;
            if(o->shape==ZGAME_SHAPE_LIGHT && tree->previous_count && !tree->moving_lights[i]) {
                tree->moving_lights[i]=1;stationary_light_changed=1;
            }
        }
        memcpy(tree->previous_keys+i*key_size,&key,key_size);
    }
    if(light_count!=tree->shadow_light_count) {
        size_t faces=tree->capacity*6;
        free(tree->shadow_results);tree->shadow_results=NULL;
        /* Optional acceleration is bounded; huge light counts use the exact
           uncached path rather than allocating an unbounded matrix. */
        if(light_count && faces <= (128u*1024u*1024u)/light_count)
            tree->shadow_results=calloc(faces,light_count);
        tree->shadow_light_count=light_count;stationary_light_changed=1;
    }
    light_count=0;
    for(i=0;i<frame->object_count;i++) {
        tree->light_slots[i]=frame->objects[i].shape==ZGAME_SHAPE_LIGHT?light_count++:SIZE_MAX;
    }
    if(geometry_changed||stationary_light_changed) {
        memset(tree->shadow_face_keys,0,tree->capacity*6*sizeof(*tree->shadow_face_keys));
        if(tree->shadow_results)memset(tree->shadow_results,0,tree->capacity*6*tree->shadow_light_count);
    }
    tree->previous_count=frame->object_count;
    if(changed)memset(tree->face_colors,0,tree->capacity*6*sizeof(*tree->face_colors));
    if(!geometry_changed) {
        for(i=0;i<tree->count;i++)tree->objects[i]=&frame->objects[tree->indices[i]];
        return 1;
    }
    tree->rebuild_count++;
    tree->count=tree->node_count=0;
    for(i=0;i<frame->object_count;i++) {
        const ZSharpGameRenderObject *o=&frame->objects[i];
        if(o->visible&&o->opacity>=.99f&&(o->shape==ZGAME_SHAPE_CUBE||o->shape==ZGAME_SHAPE_MESH))
            tree->objects[tree->count++]=o;
    }
    if(tree->count)shadow_build_node(tree,0,tree->count);
    for(i=0;i<tree->count;i++)tree->indices[i]=(size_t)(tree->objects[i]-frame->objects);
    return 1;
}

static int shadow_query(const ShadowIndex *tree,size_t id,
                        const ZSharpGameRenderFrame *frame,
                        const ZSharpGameRenderObject *self,
                        const float point[3],const float light[3],
                        const float inverse[3],const unsigned char parallel[3]) {
    const ShadowNode *node=&tree->nodes[id];
    float entry=.002f,leave=.998f;
    int axis;
    size_t i;
    for(axis=0;axis<3;axis++) {
        float a,b;
        if(parallel[axis]) {
            if(point[axis]<node->low[axis]||point[axis]>node->high[axis])return 0;
            continue;
        }
        a=(node->low[axis]-point[axis])*inverse[axis];b=(node->high[axis]-point[axis])*inverse[axis];
        if(a>b){float swap=a;a=b;b=swap;}
        entry=fmaxf(entry,a);leave=fminf(leave,b);
        if(entry>leave)return 0;
    }
    if(!node->count) {
        /* Visit the child nearest the ray origin first, so nearby blockers
           terminate traversal before distant store geometry is visited. */
        size_t first=node->left,second=node->right;
        float left_distance=0,right_distance=0;
        for(axis=0;axis<3;axis++) {
            float a=(tree->nodes[first].low[axis]+tree->nodes[first].high[axis])*.5f-point[axis];
            float b=(tree->nodes[second].low[axis]+tree->nodes[second].high[axis])*.5f-point[axis];
            left_distance+=a*a;right_distance+=b*b;
        }
        if(right_distance<left_distance){size_t swap=first;first=second;second=swap;}
        return shadow_query(tree,first,frame,self,point,light,inverse,parallel)||
               shadow_query(tree,second,frame,self,point,light,inverse,parallel);
    }
    for(i=node->begin;i<node->begin+node->count;i++) {
        ZSharpGameRenderFrame single=*frame;
        single.objects=tree->objects[i];single.object_count=1;
        if(light_occluded_impl(&single,self,point,light,1))return 1;
    }
    return 0;
}

static int light_occluded(const ZSharpGameRenderFrame *frame,
                          const ZSharpGameRenderObject *self,
                          const float point[3],const float light[3]) {
    const ShadowIndex *tree=frame->shadow_index;
    if(tree) {
        float inverse[3];unsigned char parallel[3];int axis;
        for(axis=0;axis<3;axis++) {
            float direction=light[axis]-point[axis];
            parallel[axis]=(unsigned char)(fabsf(direction)<.00001f);
            inverse[axis]=parallel[axis]?0:1.0f/direction;
        }
        return tree->count?shadow_query(tree,0,frame,self,point,light,inverse,parallel):0;
    }
    return light_occluded_impl(frame,self,point,light,1);
}

static int cached_light_occluded(const ZSharpGameRenderFrame *frame,
                                 const ZSharpGameRenderObject *object,
                                 const ZSharpProjectedCubeFace *face,
                                 const ZSharpGameRenderObject *source,
                                 const float light[3]) {
    ShadowIndex *tree=(ShadowIndex *)frame->shadow_index;
    size_t source_index=(size_t)(source-frame->objects),slot,chosen=SIZE_MAX;
    float key[7];unsigned char *result;
    if(!tree || tree->uncached_shadows || !tree->shadow_results || source_index>=frame->object_count ||
       tree->moving_lights[source_index] || !object->render_id || object->render_id>tree->capacity ||
       object->shape!=ZGAME_SHAPE_CUBE)
        return light_occluded(frame,object,face->world_center,light);
    memcpy(key,face->world_center,3*sizeof(float));
    memcpy(key+3,face->world_normal,3*sizeof(float));key[6]=face->brightness;
    for(slot=0;slot<6;slot++) {
        size_t id=(object->render_id-1)*6+slot;
        FaceLightCache *entry=&tree->shadow_face_keys[id];
        if(entry->valid && !memcmp(entry->key,key,sizeof(key))){chosen=id;break;}
        if(!entry->valid && chosen==SIZE_MAX)chosen=id;
    }
    if(chosen==SIZE_MAX)return light_occluded(frame,object,face->world_center,light);
    if(!tree->shadow_face_keys[chosen].valid) {
        memcpy(tree->shadow_face_keys[chosen].key,key,sizeof(key));
        tree->shadow_face_keys[chosen].valid=1;
        memset(tree->shadow_results+chosen*tree->shadow_light_count,0,tree->shadow_light_count);
    }
    result=&tree->shadow_results[chosen*tree->shadow_light_count+tree->light_slots[source_index]];
    if(!*result)*result=light_occluded(frame,object,face->world_center,light)?2:1;
    return *result==2;
}

static SDL_FColor lit_face_color(const ZSharpGameRenderFrame *frame,
                                 const ZSharpGameRenderObject *object,
                                 const ZSharpProjectedCubeFace *face) {
    SDL_FColor base = float_color_value(object->color);
    float illumination[3] = {0.20f, 0.20f, 0.20f};
    float glow = clamp_unit(object->emissive);
    size_t index;
    int has_light = 0;
    ShadowIndex *tree=(ShadowIndex *)frame->shadow_index;
    FaceLightCache *cache=NULL;
    float cache_key[10]={0};
    /* Metallic highlights depend on camera position, not camera orientation.
       Preserve exact highlights when walking, but reuse them when looking. */
    if(tree && object->shape==ZGAME_SHAPE_CUBE &&
       object->render_id>0 && object->render_id<=tree->capacity) {
        size_t slot;
        memcpy(cache_key,face->world_center,3*sizeof(float));
        memcpy(cache_key+3,face->world_normal,3*sizeof(float));cache_key[6]=face->brightness;
        if(object->metallic>0) {
            cache_key[7]=frame->camera_x;cache_key[8]=frame->camera_y;cache_key[9]=frame->camera_z;
        }
        for(slot=0;slot<6;slot++) {
            FaceLightCache *candidate=&tree->face_colors[(object->render_id-1)*6+slot];
            if(candidate->valid && !memcmp(candidate->key,cache_key,sizeof(cache_key)))return candidate->color;
            if(!candidate->valid && !cache)cache=candidate;
            /* Same face, changed camera: replace rather than exhaust slots. */
            if(candidate->valid && !memcmp(candidate->key,cache_key,7*sizeof(float)))cache=candidate;
        }
    }
    for (index = 0; index < (frame->indexed_lights ? frame->light_count : frame->object_count); index++) {
        const ZSharpGameRenderObject *source = frame->indexed_lights ? frame->lights[index] : &frame->objects[index];
        float light[3] = {source->x, source->y, source->z};
        float direction[3], distance, incidence, attenuation, strength;
        SDL_FColor tint;
        int axis;
        if (!source->visible || source->shape != ZGAME_SHAPE_LIGHT) continue;
        has_light = 1;
        /* Zero-output lights still select the scene's lit ambient mode, but
           cannot contribute illumination or require shadow queries. */
        if (source->light_intensity == 0.0f) continue;
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
            distance = direction[0]*direction[0] + direction[1]*direction[1] +
                       direction[2]*direction[2];
            if (source->light_range > 0.0f &&
                distance > source->light_range*source->light_range) continue;
            distance = sqrtf(distance);
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
            cached_light_occluded(frame, object, face, source, light)) continue;
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
    if(cache) {memcpy(cache->key,cache_key,sizeof(cache_key));cache->color=base;cache->valid=1;}
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
    float inverse_a = 1.0f / face->point_depth[a];
    float inverse_b = 1.0f / face->point_depth[b];
    float inverse_c = 1.0f / face->point_depth[c];
    uint32_t solid = raster_pack(tint.r, tint.g, tint.b, 1.0f);
    int x0, x1, y0, y1, x, y;
    if (!isfinite(area) || fabsf(area) < 0.00001f) return;
    x0 = (int)fmaxf(0.0f, floorf(fminf(ax, fminf(bx, cx))));
    x1 = (int)fminf(ZGAME_LOGICAL_WIDTH - 1.0f,
                    ceilf(fmaxf(ax, fmaxf(bx, cx))));
    y0 = (int)fmaxf(0.0f, floorf(fminf(ay, fminf(by, cy))));
    y1 = (int)fminf(ZGAME_LOGICAL_HEIGHT - 1.0f,
                    ceilf(fmaxf(ay, fmaxf(by, cy))));
    if (x0 > x1 || y0 > y1) return;
    for (y = y0; y <= y1; y++) {
      /* Restrict each row to the triangle's span instead of scanning the
         bounding box (especially costly for thin diagonal triangles). */
      float scan_y = (float)y + .5f, left = FLT_MAX, right = -FLT_MAX;
      float vx[3] = {ax,bx,cx}, vy[3] = {ay,by,cy};
      int edge, begin, end;
      for (edge = 0; edge < 3; edge++) {
          int next = (edge + 1) % 3;
          if (vy[edge] == vy[next]) continue;
          if (scan_y < fminf(vy[edge],vy[next]) || scan_y > fmaxf(vy[edge],vy[next])) continue;
          {
              float hit = vx[edge] + (scan_y-vy[edge]) * (vx[next]-vx[edge]) / (vy[next]-vy[edge]);
              left = fminf(left,hit); right = fmaxf(right,hit);
          }
      }
      if (left > right) continue;
      begin = (int)fmaxf((float)x0, floorf(left)-1);
      end = (int)fminf((float)x1, ceilf(right)+1);
      for (x = begin; x <= end; x++) {
        float px = (float)x + 0.5f, py = (float)y + 0.5f;
        float wa = raster_edge(bx, by, cx, cy, px, py) / area;
        float wb = raster_edge(cx, cy, ax, ay, px, py) / area;
        float wc = 1.0f - wa - wb;
        float inverse_depth, red = tint.r, green = tint.g,
              blue = tint.b, alpha = tint.a;
        size_t pixel;
        if (wa < -0.00001f || wb < -0.00001f || wc < -0.00001f)
            continue;
        inverse_depth = wa * inverse_a + wb * inverse_b + wc * inverse_c;
        if (inverse_depth <= 0.0f) continue;
        pixel = (size_t)y * (size_t)ZGAME_LOGICAL_WIDTH + (size_t)x;
        if (inverse_depth <= renderer->depth_buffer[pixel]) continue;
        if (surface == NULL && alpha >= .999f) {
            renderer->depth_buffer[pixel] = inverse_depth;
            renderer->color_buffer[pixel] = solid;
            continue;
        }
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
            renderer->depth_buffer[pixel] = inverse_depth;
        }
        renderer->color_buffer[pixel] = raster_pack(red, green, blue, 1.0f);
      }
    }
}

static void raster_face(ZSharpGameVulkan *renderer,
                        const ZSharpProjectedCubeFace *face,
                        SDL_FColor color, SDL_Texture *texture) {
    if(renderer->gpu.device) {
        game_gpu_face(&renderer->gpu,face,color,texture,texture!=NULL||color.a<.999f);
        return;
    }
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
    float hx=object->width*object->scale_x*.5f,hy=object->height*object->scale_y*.5f,hz=object->depth*object->scale_z*.5f;
    float dx=frame->camera_x-object->x,dy=frame->camera_y-object->y,dz=frame->camera_z-object->z;
    int outside=hx>0 && hy>0 && hz>0 && dx*dx+dy*dy+dz*dz>hx*hx+hy*hy+hz*hz+.001f;
    for (index = 0; index < count; index++) {
        /* Closed opaque cubes hide their back faces. Keep two-sided behavior
           for alpha textures/translucency and cameras inside the volume. */
        if(texture==NULL && object->opacity>=.999f && outside) {
            const ZSharpProjectedCubeFace *face=&faces[index];
            float facing=(frame->camera_x-face->world_center[0])*face->world_normal[0]+
                         (frame->camera_y-face->world_center[1])*face->world_normal[1]+
                         (frame->camera_z-face->world_center[2])*face->world_normal[2];
            if(facing<=0)continue;
        }
        raster_face(renderer, &faces[index],
                    lit_face_color(frame, object, &faces[index]), texture);
    }
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
                         const RenderEntry *first,
                         const RenderEntry *second) {
    const ZSharpGameRenderObject *a=first->object,*b=second->object;
    float a_depth, b_depth;
    if ((a->shape == ZGAME_SHAPE_BUTTON) != (b->shape == ZGAME_SHAPE_BUTTON))
        return a->shape != ZGAME_SHAPE_BUTTON;
    if (a->layer != b->layer) return a->layer < b->layer;
    if (a->shape == ZGAME_SHAPE_BUTTON && b->shape == ZGAME_SHAPE_BUTTON) return 0;
    if (!frame->is_3d) return a->z > b->z;
    /* A long floor's center can pass behind the player even while much of
       that floor extends farther away. Drawing by its backmost corner keeps
       the floor underneath foreground actors instead of overpainting them. */
    a_depth = first->depth;
    b_depth = second->depth;
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
    texture = renderer->gpu.device ? SDL_CreateTexture(renderer->renderer,
        SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,surface->w,surface->h) :
        SDL_CreateTextureFromSurface(renderer->renderer, surface);
    if(texture && renderer->gpu.device && !game_gpu_upload(&renderer->gpu,texture,
         surface->pixels,surface->w,surface->h,surface->pitch)) {
        SDL_DestroyTexture(texture);texture=NULL;
    }
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

static int raster_regression(void) {
    ZSharpGameVulkan test = {0};
    ZSharpProjectedCubeFace face = {0};
    SDL_FColor red = {1,0,0,1}, green = {0,1,0,1};
    size_t pixels = (size_t)ZGAME_LOGICAL_WIDTH * (size_t)ZGAME_LOGICAL_HEIGHT;
    size_t hit = 12 * (size_t)ZGAME_LOGICAL_WIDTH + 12;
    int ok;
    test.color_buffer = calloc(pixels,sizeof(*test.color_buffer));
    test.depth_buffer = calloc(pixels,sizeof(*test.depth_buffer));
    if (!test.color_buffer || !test.depth_buffer) { free(test.color_buffer);free(test.depth_buffer);return 0; }
    face.points[0][0]=10;face.points[0][1]=10;
    face.points[1][0]=30;face.points[1][1]=10;
    face.points[2][0]=10;face.points[2][1]=30;
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=2;
    raster_triangle(&test,&face,0,1,2,red,NULL);
    ok = test.color_buffer[hit] == raster_pack(1,0,0,1) && test.depth_buffer[hit] == .5f;
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=4;
    raster_triangle(&test,&face,0,1,2,green,NULL);
    ok = ok && test.color_buffer[hit] == raster_pack(1,0,0,1);
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=1;
    raster_triangle(&test,&face,0,1,2,green,NULL);
    ok = ok && test.color_buffer[hit] == raster_pack(0,1,0,1) && test.depth_buffer[hit] == 1;
    ok = ok && test.color_buffer[28*(size_t)ZGAME_LOGICAL_WIDTH+28] == 0;
    free(test.color_buffer);free(test.depth_buffer);
    {
        ZSharpGameRenderObject objects[64]={{0}},self={0};
        ZSharpGameRenderFrame scene={0};
        ShadowIndex tree={0};
        unsigned seed=12345;size_t trial,j;
        scene.objects=objects;scene.object_count=64;
        for(j=0;j<64;j++) {
            objects[j].shape=j%4==3?ZGAME_SHAPE_MESH:ZGAME_SHAPE_CUBE;
            objects[j].visible=1;objects[j].opacity=1;objects[j].render_id=j+1;
            objects[j].width=3;objects[j].height=4;objects[j].depth=2;
            objects[j].scale_x=objects[j].scale_y=objects[j].scale_z=1;
            objects[j].x=(float)(j%8)*4-14;objects[j].y=(float)(j/8)*4-14;
            objects[j].rotation=(float)(j%3)*17;objects[j].rotation_y=(float)j*33;
            objects[j].rotation_z=(float)j*17;objects[j].rotation_x=(float)j*11;
        }
        if(!shadow_prepare(&tree,&scene))return 0;
        scene.shadow_index=&tree;
        for(trial=0;trial<1000;trial++) {
            float point[3],light[3];
            if(trial%100==0) {
                objects[trial%64].x+=.5f;
                objects[(trial+1)%64].visible=!objects[(trial+1)%64].visible;
                if(!shadow_prepare(&tree,&scene)){ok=0;break;}
            }
            for(j=0;j<3;j++) {
                seed=seed*1664525u+1013904223u;point[j]=(float)(seed%4001)/100-20;
                seed=seed*1664525u+1013904223u;light[j]=(float)(seed%4001)/100-20;
            }
            self.render_id=trial%65;
            if(light_occluded_impl(&scene,&self,point,light,0)!=
               light_occluded(&scene,&self,point,light)){ok=0;break;}
        }
        /* Stable snapshots must reuse the index/cache; value edits must
           invalidate it, including the legacy cube rotation property. */
        tree.face_colors[0].valid=1;
        if(!shadow_prepare(&tree,&scene)||!tree.face_colors[0].valid)ok=0;
        objects[0].rotation+=20;
        if(!shadow_prepare(&tree,&scene)||tree.face_colors[0].valid)ok=0;
        free(tree.nodes);free(tree.objects);free(tree.indices);
        free(tree.previous_keys);free(tree.face_colors);
        free(tree.previous_occluder_keys);
        free(tree.moving_lights);free(tree.light_slots);free(tree.shadow_face_keys);free(tree.shadow_results);
    }
    {
        ZSharpGameRenderObject objects[2]={{0}};
        ZSharpGameRenderFrame scene={0},reference;
        ZSharpProjectedCubeFace sample_face={0};
        ShadowIndex tree={0};size_t rebuilds,j;
        objects[0].shape=ZGAME_SHAPE_CUBE;objects[0].visible=1;objects[0].opacity=1;
        objects[0].render_id=1;objects[0].width=objects[0].height=objects[0].depth=2;
        objects[0].scale_x=objects[0].scale_y=objects[0].scale_z=1;
        objects[0].metallic=.6f;objects[0].roughness=.4f;objects[0].color=0x808080;
        objects[1].shape=ZGAME_SHAPE_LIGHT;objects[1].visible=1;
        objects[1].y=4;objects[1].color=0xFFFFFF;objects[1].light_range=20;
        scene.objects=objects;scene.object_count=2;scene.camera_y=4;scene.camera_z=4;
        sample_face.world_center[1]=1;sample_face.world_normal[1]=1;sample_face.brightness=1;
        if(!shadow_prepare(&tree,&scene))return 0;
        scene.shadow_index=&tree;rebuilds=tree.rebuild_count;
        tree.face_colors[0].valid=1;
        objects[1].x=3;objects[1].rotation_y=90;
        if(!shadow_prepare(&tree,&scene)||tree.rebuild_count!=rebuilds||!tree.face_colors[0].valid)ok=0;
        objects[1].light_intensity=30;
        if(!shadow_prepare(&tree,&scene)||tree.rebuild_count!=rebuilds||tree.face_colors[0].valid)ok=0;
        /* Exact cached metallic colors must match the uncached path even as
           the camera walks and the per-face slot is replaced repeatedly. */
        for(j=0;j<12;j++) {
            SDL_FColor cached,uncached;
            scene.camera_x=(float)j*.2f;reference=scene;reference.shadow_index=NULL;
            cached=lit_face_color(&scene,&objects[0],&sample_face);
            uncached=lit_face_color(&reference,&objects[0],&sample_face);
            if(memcmp(&cached,&uncached,sizeof(cached)))ok=0;
            cached=lit_face_color(&scene,&objects[0],&sample_face);
            if(memcmp(&cached,&uncached,sizeof(cached)))ok=0;
        }
        objects[0].x+=2;
        if(!shadow_prepare(&tree,&scene)||tree.rebuild_count==rebuilds||tree.face_colors[0].valid)ok=0;
        free(tree.nodes);free(tree.objects);free(tree.indices);
        free(tree.previous_keys);free(tree.previous_occluder_keys);free(tree.face_colors);
        free(tree.moving_lights);free(tree.light_slots);free(tree.shadow_face_keys);free(tree.shadow_results);
    }
    {
        ZSharpGameRenderObject objects[3]={{0}};
        ZSharpGameRenderFrame scene={0};ZSharpProjectedCubeFace sample={0};
        ShadowIndex tree={0};float light[3]={0,5,0};int blocked;
        size_t j;
        for(j=0;j<2;j++) {
            objects[j].shape=ZGAME_SHAPE_CUBE;objects[j].visible=1;objects[j].opacity=1;
            objects[j].render_id=j+1;objects[j].width=objects[j].height=objects[j].depth=2;
            objects[j].scale_x=objects[j].scale_y=objects[j].scale_z=1;
        }
        objects[1].y=3;
        objects[2].shape=ZGAME_SHAPE_LIGHT;objects[2].visible=1;
        objects[2].y=5;objects[2].light_intensity=100;objects[2].cast_shadows=1;
        scene.objects=objects;scene.object_count=3;
        sample.world_center[1]=1;sample.world_normal[1]=1;sample.brightness=1;
        if(!shadow_prepare(&tree,&scene))return 0;scene.shadow_index=&tree;
        blocked=cached_light_occluded(&scene,&objects[0],&sample,&objects[2],light);
        if(!blocked || blocked!=light_occluded_impl(&scene,&objects[0],sample.world_center,light,0))ok=0;
        /* Walking changes highlights, but cannot change stationary-light rays. */
        scene.camera_x=50;
        if(!shadow_prepare(&tree,&scene)||!tree.shadow_face_keys[0].valid ||
           !cached_light_occluded(&scene,&objects[0],&sample,&objects[2],light))ok=0;
        /* A moved obstacle must invalidate the old blocked result. */
        objects[1].x=20;
        if(!shadow_prepare(&tree,&scene)||tree.shadow_face_keys[0].valid ||
           cached_light_occluded(&scene,&objects[0],&sample,&objects[2],light))ok=0;
        /* A light which starts moving uses live queries from then on. */
        objects[2].x=20;light[0]=20;
        if(!shadow_prepare(&tree,&scene)||!tree.moving_lights[2])ok=0;
        blocked=cached_light_occluded(&scene,&objects[0],&sample,&objects[2],light);
        if(blocked!=light_occluded_impl(&scene,&objects[0],sample.world_center,light,0))ok=0;
        free(tree.nodes);free(tree.objects);free(tree.indices);
        free(tree.previous_keys);free(tree.previous_occluder_keys);free(tree.face_colors);
        free(tree.moving_lights);free(tree.light_slots);free(tree.shadow_face_keys);free(tree.shadow_results);
    }
    return ok;
}

ZSharpGameVulkan *zsharp_game_vulkan_create(SDL_Window *window, char *error,
                                             size_t error_size) {
    ZSharpGameVulkan *renderer;
    const char *driver;
    if (getenv("ZSHARP_GAME_RASTER_TEST") && !raster_regression()) {
        renderer_error(error,error_size,"raster regression failed"); return NULL;
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
    renderer = (ZSharpGameVulkan *)calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        renderer_error(error, error_size, "out of memory");
        return NULL;
    }
    renderer->window = window;
    renderer->shadows.uncached_shadows=getenv("ZSHARP_GAME_UNCACHED_SHADOWS")!=NULL;
    if (getenv("ZSHARP_GAME_SOFTWARE") == NULL) {
        renderer->gpu.device=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,false,"vulkan");
        if(renderer->gpu.device)renderer->renderer=SDL_CreateGPURenderer(renderer->gpu.device,window);
        if(renderer->renderer && !game_gpu_init(&renderer->gpu,renderer->renderer)) {
            game_gpu_destroy(&renderer->gpu);SDL_DestroyRenderer(renderer->renderer);renderer->renderer=NULL;
        }
        if(!renderer->renderer && renderer->gpu.device) {
            SDL_DestroyGPUDevice(renderer->gpu.device);memset(&renderer->gpu,0,sizeof(renderer->gpu));
        }
    }
    if(!renderer->renderer) renderer->renderer = SDL_CreateRenderer(window, "vulkan");
    if (renderer->renderer == NULL) {
        renderer_error(error, error_size, SDL_GetError());
        free(renderer);
        return NULL;
    }
    driver = SDL_GetRendererName(renderer->renderer);
    snprintf(renderer->driver, sizeof(renderer->driver), "%s",
             driver == NULL ? "vulkan" : driver);
    if (renderer->gpu.device) snprintf(renderer->driver,sizeof(renderer->driver),"gpu-vulkan");
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
    if (renderer->gpu.device && getenv("ZSHARP_GAME_RASTER_TEST") &&
        !game_gpu_regression(&renderer->gpu,renderer->renderer)) {
        renderer_error(error,error_size,"GPU depth/blend regression failed");
        zsharp_game_vulkan_destroy(renderer);return NULL;
    }
    if(getenv("ZSHARP_GAME_PROFILE"))fprintf(stderr,"[Z# renderer] %s\n",renderer->driver);
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
    RenderEntry *ordered = NULL;
    ZSharpGameRenderFrame indexed;
    size_t index, ordered_count=0;
    int profile=getenv("ZSHARP_GAME_PROFILE")!=NULL;
    Uint64 timing_start=profile?SDL_GetTicksNS():0,timing_index=0,timing_sort=0,timing_geometry=0,timing_submit=0;
    (void)resized;
    if (renderer == NULL || renderer->renderer == NULL || frame == NULL) {
        renderer_error(error, error_size, "invalid Vulkan frame");
        return 0;
    }
    indexed=*frame;
    zsharp_game_prepare_projection(&indexed);
    if(!shadow_prepare(&renderer->shadows,frame)) {
        renderer_error(error,error_size,"out of memory building shadow index");return 0;
    }
    indexed.shadow_index=&renderer->shadows;
    if(frame->object_count>renderer->light_capacity) {
        void *memory=realloc(renderer->lights,frame->object_count*sizeof(*renderer->lights));
        if(!memory){renderer_error(error,error_size,"out of memory");return 0;}
        renderer->lights=memory;renderer->light_capacity=frame->object_count;
    }
    indexed.light_count=0;indexed.lights=renderer->lights;indexed.indexed_lights=1;
    for(index=0;index<frame->object_count;index++)
        if(frame->objects[index].visible&&frame->objects[index].shape==ZGAME_SHAPE_LIGHT)
            renderer->lights[indexed.light_count++]=&frame->objects[index];
    frame=&indexed;
    if(profile)timing_index=SDL_GetTicksNS();
    set_color(renderer->renderer, frame->background);
    if (!SDL_RenderClear(renderer->renderer)) goto failed;
    if (frame->object_count != 0) {
        if(frame->object_count>renderer->order_capacity) {
            RenderEntry *entries, *scratch;
            if(frame->object_count>SIZE_MAX/sizeof(*entries)) {
                renderer_error(error,error_size,"draw list too large");return 0;
            }
            entries=malloc(frame->object_count*sizeof(*entries));
            scratch=malloc(frame->object_count*sizeof(*scratch));
            if(!entries||!scratch) {
                free(entries);free(scratch);
                renderer_error(error,error_size,"out of memory");return 0;
            }
            free(renderer->ordered);free(renderer->sort_scratch);
            renderer->ordered=entries;renderer->sort_scratch=scratch;
            renderer->order_capacity=frame->object_count;
        }
        ordered=renderer->ordered;
        /* Cull only the draw list: offscreen objects remain in the original
           snapshot for shadow casting, physics, and script properties. */
        for(index=0;index<frame->object_count;index++) {
            const ZSharpGameRenderObject *object=&frame->objects[index];
            if(!object->visible || object->shape==ZGAME_SHAPE_LIGHT || object->shape==ZGAME_SHAPE_NAV)continue;
            if(frame->is_3d && object->shape==ZGAME_SHAPE_CUBE && !zsharp_game_cube_in_view(frame,object))continue;
            ordered[ordered_count].object=object;
            ordered[ordered_count].depth=frame->is_3d?zsharp_game_camera_far_depth(frame,object):0;
            ordered_count++;
        }
        /* Stable O(n log n) ordering; insertion sort made camera changes
           quadratic in the number of placed objects. */
        {
            size_t width, count = ordered_count;
            RenderEntry *scratch = renderer->sort_scratch;
            for (width = 1; width < count; width *= 2) {
                size_t start;
                for (start = 0; start < count; start += 2 * width) {
                    size_t mid = start + width < count ? start + width : count;
                    size_t end = mid + width < count ? mid + width : count;
                    size_t a = start, b = mid, out = start;
                    while (a < mid || b < end) {
                        if (b == end || (a < mid && !render_before(frame, &ordered[b], &ordered[a])))
                            scratch[out++] = ordered[a++];
                        else scratch[out++] = ordered[b++];
                    }
                }
                memcpy(ordered, scratch, count * sizeof(*ordered));
            }
        }
    }
    if(profile)timing_sort=SDL_GetTicksNS();
    if (frame->is_3d) {
        size_t pixel, pixels =
            (size_t)ZGAME_LOGICAL_WIDTH * (size_t)ZGAME_LOGICAL_HEIGHT;
        renderer->gpu.vertex_count=renderer->gpu.draw_count=0;renderer->gpu.failed=0;
        if (!renderer->gpu.device && !ensure_depth_buffer(renderer, error, error_size)) {
            return 0;
        }
        uint32_t background = raster_pack(
            (float)((frame->background >> 16) & 255u) / 255.0f,
            (float)((frame->background >> 8) & 255u) / 255.0f,
            (float)(frame->background & 255u) / 255.0f, 1.0f);
        for (pixel = 0; !renderer->gpu.device && pixel < pixels; pixel++) {
            renderer->color_buffer[pixel] = background;
            renderer->depth_buffer[pixel] = 0.0f;
        }
        renderer->depth_active = 1;
        for (index = 0; index < ordered_count; index++) {
            const ZSharpGameRenderObject *object = ordered[index].object;
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
        if(profile)timing_geometry=SDL_GetTicksNS();
        if(renderer->gpu.device) {
            if(!game_gpu_present(&renderer->gpu,renderer->renderer,frame->background)) goto failed;
        } else if (!SDL_UpdateTexture(renderer->depth_texture, NULL,
                               renderer->color_buffer,
                               (int)ZGAME_LOGICAL_WIDTH * 4) ||
            !SDL_RenderTexture(renderer->renderer, renderer->depth_texture,
                               NULL, NULL)) goto failed;
    }
    if(profile)timing_submit=SDL_GetTicksNS();
    for (index = 0; index < ordered_count; index++) {
        const ZSharpGameRenderObject *object = ordered[index].object;
        int ok = 1;
        if (!object->visible || object->shape == ZGAME_SHAPE_BUTTON || object->shape == ZGAME_SHAPE_LIGHT ||
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
            scale *= object->font_size > 0 ? object->font_size / 8.0f : 1.0f;
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
    /* Screen-space buttons overlay the game, including 3D scenes. */
    for (index = 0; index < ordered_count; ++index) {
        const ZSharpGameRenderObject *object = ordered[index].object;
        ZSharpGameRenderObject button;
        int ok;
        float camera_x = frame->is_3d ? 0 : frame->camera_x;
        float camera_y = frame->is_3d ? 0 : frame->camera_y;
        float width, height, x, y;
        const char *label;
        if (!object->visible || object->shape != ZGAME_SHAPE_BUTTON) continue;
        button = *object;
        button.rotation = 0;
        button.width = fabsf(button.width);
        button.height = fabsf(button.height);
        button.scale_x = fabsf(button.scale_x);
        button.scale_y = fabsf(button.scale_y);
        if (!object->transparent_background &&
            !render_rectangle(renderer->renderer, &button, camera_x, camera_y)) goto failed;
        width = button.width * button.scale_x;
        height = button.height * button.scale_y;
        label = object->text == NULL ? "" : object->text;
        {
            float font_scale = object->font_size > 0 ? object->font_size / 8.0f : 1.0f;
            x = screen_x(object->x, camera_x) - (float)strlen(label) * 4.0f * font_scale;
            y = screen_y(object->y, camera_y) - 4.0f * font_scale;
        }
        {
            SDL_Rect clip = {(int)(screen_x(object->x, camera_x) - width * 0.5f),
                             (int)(screen_y(object->y, camera_y) - height * 0.5f),
                             (int)width, (int)height};
            /* A transparent button has no visible fill to contrast against.
               Hover/transition colors must not flip its label black/white. */
            unsigned color = object->transparent_background ? frame->background : object->color;
            unsigned brightness = ((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255);
            set_color(renderer->renderer, brightness > 382 ? 0x000000 : 0xFFFFFF);
            {
                float font_scale = object->font_size > 0 ? object->font_size / 8.0f : 1.0f;
                SDL_SetRenderScale(renderer->renderer, font_scale, font_scale);
                clip.x = (int)(clip.x / font_scale);
                clip.y = (int)(clip.y / font_scale);
                clip.w = (int)(clip.w / font_scale);
                clip.h = (int)(clip.h / font_scale);
                SDL_SetRenderClipRect(renderer->renderer, &clip);
                ok = SDL_RenderDebugText(renderer->renderer, x / font_scale, y / font_scale, label);
                SDL_SetRenderClipRect(renderer->renderer, NULL);
                SDL_SetRenderScale(renderer->renderer, 1.0f, 1.0f);
            }
            SDL_SetRenderClipRect(renderer->renderer, NULL);
            if (!ok) goto failed;
        }
    }
    if (!renderer->captured && getenv("ZSHARP_GAME_CAPTURE")) {
        SDL_Surface *capture=SDL_RenderReadPixels(renderer->renderer,NULL);
        if(capture) {SDL_SaveBMP(capture,getenv("ZSHARP_GAME_CAPTURE"));SDL_DestroySurface(capture);}
        renderer->captured=1;
    }
    if (!SDL_RenderPresent(renderer->renderer)) goto failed_without_objects;
    if(profile)fprintf(stderr,"[Z# render phases] index=%.3fms sort=%.3fms geometry/lighting=%.3fms upload/submit=%.3fms overlay/present=%.3fms\n",
        (double)(timing_index-timing_start)/1e6,(double)(timing_sort-timing_index)/1e6,
        frame->is_3d?(double)(timing_geometry-timing_sort)/1e6:0,
        frame->is_3d?(double)(timing_submit-timing_geometry)/1e6:0,
        (double)(SDL_GetTicksNS()-timing_submit)/1e6);
    return 1;
failed:
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
    free(renderer->lights);
    free(renderer->ordered);
    free(renderer->sort_scratch);
    free(renderer->shadows.nodes);
    free(renderer->shadows.objects);
    free(renderer->shadows.indices);
    free(renderer->shadows.previous_keys);
    free(renderer->shadows.previous_occluder_keys);
    free(renderer->shadows.moving_lights);
    free(renderer->shadows.light_slots);
    free(renderer->shadows.shadow_face_keys);
    free(renderer->shadows.shadow_results);
    free(renderer->shadows.face_colors);
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
    game_gpu_destroy(&renderer->gpu);
    if (renderer->renderer != NULL) SDL_DestroyRenderer(renderer->renderer);
    if(renderer->gpu.device)SDL_DestroyGPUDevice(renderer->gpu.device);
    free(renderer);
}

#endif
