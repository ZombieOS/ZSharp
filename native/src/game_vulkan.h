#ifndef ZSHARP_GAME_VULKAN_H
#define ZSHARP_GAME_VULKAN_H

#include <stddef.h>

typedef enum ZSharpGameShape {
    ZGAME_SHAPE_RECTANGLE = 1,
    ZGAME_SHAPE_CIRCLE = 2,
    ZGAME_SHAPE_TRIANGLE = 3,
    ZGAME_SHAPE_SPRITE = 4,
    ZGAME_SHAPE_CUBE = 5,
    ZGAME_SHAPE_TEXT = 6
} ZSharpGameShape;

typedef struct ZSharpGameRenderObject {
    ZSharpGameShape shape;
    float x;
    float y;
    float z;
    float width;
    float height;
    float depth;
    float rotation;
    float scale_x;
    float scale_y;
    float scale_z;
    unsigned color;
    int visible;
    int layer;
    const char *text;
    const char *asset_path;
} ZSharpGameRenderObject;

typedef struct ZSharpGameRenderFrame {
    int is_3d;
    unsigned background;
    float camera_x;
    float camera_y;
    float camera_z;
    float camera_fov;
    const char *project_root;
    const ZSharpGameRenderObject *objects;
    size_t object_count;
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
