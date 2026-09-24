#define _CRT_SECURE_NO_WARNINGS

#include "game_vulkan.h"
#include "game_projection.h"

#ifdef ZSHARP_HAS_GAME_RUNTIME

#include <SDL3/SDL.h>

#include <math.h>
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
};

typedef struct ZSharpTextureCache {
    char *path;
    SDL_Texture *texture;
    struct ZSharpTextureCache *next;
} ZSharpTextureCache;

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
        SDL_FColor color = float_color_value(object->color);
        size_t index;
        color.r *= face->brightness;
        color.g *= face->brightness;
        color.b *= face->brightness;
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

static int render_before(const ZSharpGameRenderFrame *frame,
                         const ZSharpGameRenderObject *a,
                         const ZSharpGameRenderObject *b) {
    float a_depth, b_depth;
    if (a->layer != b->layer) return a->layer < b->layer;
    if (!frame->is_3d) return a->z > b->z;
    a_depth = zsharp_game_camera_depth(frame, a);
    b_depth = zsharp_game_camera_depth(frame, b);
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
    texture = SDL_CreateTextureFromSurface(renderer->renderer, surface);
    SDL_DestroySurface(surface);
    if (texture == NULL) {
        renderer_error(error, error_size, SDL_GetError());
        free(path);
        return NULL;
    }
    cached = (ZSharpTextureCache *)calloc(1, sizeof(*cached));
    if (cached == NULL) {
        SDL_DestroyTexture(texture);
        free(path);
        renderer_error(error, error_size, "out of memory");
        return NULL;
    }
    cached->path = path;
    cached->texture = texture;
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
    for (index = 0; index < frame->object_count; index++) {
        const ZSharpGameRenderObject *object = &ordered[index];
        int ok = 1;
        if (!object->visible) continue;
        if (object->shape == ZGAME_SHAPE_CUBE) {
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
    if (renderer == NULL) return;
    texture = renderer->textures;
    while (texture != NULL) {
        ZSharpTextureCache *next = texture->next;
        SDL_DestroyTexture(texture->texture);
        free(texture->path);
        free(texture);
        texture = next;
    }
    if (renderer->renderer != NULL) SDL_DestroyRenderer(renderer->renderer);
    free(renderer);
}

#endif
