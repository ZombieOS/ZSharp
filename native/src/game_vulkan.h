#ifndef ZSHARP_GAME_VULKAN_H
#define ZSHARP_GAME_VULKAN_H

#include <stddef.h>

typedef enum ZSharpGameShape {
    ZGAME_SHAPE_RECTANGLE = 1,
    ZGAME_SHAPE_CIRCLE = 2,
    ZGAME_SHAPE_TRIANGLE = 3,
    ZGAME_SHAPE_SPRITE = 4,
    ZGAME_SHAPE_CUBE = 5,
    ZGAME_SHAPE_TEXT = 6,
    ZGAME_SHAPE_LIGHT = 7,
    ZGAME_SHAPE_MESH = 8,
    ZGAME_SHAPE_NAV = 9,
    ZGAME_SHAPE_BUTTON = 10
} ZSharpGameShape;

typedef struct ZSharpGamePartPose {
    const char *name;
    float position[3];
    float rotation[3];
} ZSharpGamePartPose;

typedef struct ZSharpGameRenderObject {
    size_t render_id; /* stable across the per-frame painter sort */
    ZSharpGameShape shape;
    float x;
    float y;
    float z;
    float width;
    float height;
    float depth;
    float rotation;
    float rotation_x;
    float rotation_y;
    float rotation_z;
    float scale_x;
    float scale_y;
    float scale_z;
    unsigned color;
    int transparent_background;
    float font_size;
    float opacity;
    float roughness;
    float emissive;
    float metallic;
    int light_type;
    float light_intensity;
    float light_range;
    float light_angle;
    int cast_shadows;
    int visible;
    int layer;
    const char *text;
    const char *asset_path;
    const char *mesh_path;
    const char *const *material_names;
    const char *const *material_textures;
    size_t material_count;
    const ZSharpGamePartPose *part_poses;
    size_t part_pose_count;
} ZSharpGameRenderObject;

typedef struct ZSharpGameRenderFrame {
    int is_3d;
    unsigned background;
    float camera_x;
    float camera_y;
    float camera_z;
    float camera_rotation_x;
    float camera_rotation_y;
    float camera_rotation_z;
    float camera_fov;
    const char *project_root;
    const ZSharpGameRenderObject *objects;
    size_t object_count;
    const ZSharpGameRenderObject **lights;
    size_t light_count;
    int indexed_lights;
    const void *shadow_index; /* renderer-owned, valid only during this draw */
    int projection_prepared;
    float view_basis[9], projection_focal, projection_tangent;
} ZSharpGameRenderFrame;

#ifdef ZSHARP_HAS_GAME_RUNTIME
#include <SDL3/SDL.h>

typedef struct ZSharpGameVulkan ZSharpGameVulkan;

ZSharpGameVulkan *zsharp_game_vulkan_create(SDL_Window *window, char *error,
                                             size_t error_size);
int zsharp_game_vulkan_draw(ZSharpGameVulkan *renderer, int resized,
                            const ZSharpGameRenderFrame *frame,
                            char *error, size_t error_size);
const char *zsharp_game_vulkan_driver(const ZSharpGameVulkan *renderer);
void zsharp_game_vulkan_destroy(ZSharpGameVulkan *renderer);
#endif

#endif
