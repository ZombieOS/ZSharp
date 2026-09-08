#ifndef ZSHARP_GAME_MODEL_H
#define ZSHARP_GAME_MODEL_H

#include "game_vulkan.h"
#include "window.h"

#include <stddef.h>

typedef enum ZSharpGameBodyType {
    ZGAME_BODY_STATIC = 1,
    ZGAME_BODY_DYNAMIC = 2,
    ZGAME_BODY_KINEMATIC = 3
} ZSharpGameBodyType;

typedef enum ZSharpGameColliderType {
    ZGAME_COLLIDER_NONE = 0,
    ZGAME_COLLIDER_BOX = 1,
    ZGAME_COLLIDER_CIRCLE = 2
} ZSharpGameColliderType;

typedef enum ZSharpGameKey {
    ZGAME_KEY_A = 0,
    ZGAME_KEY_B, ZGAME_KEY_C, ZGAME_KEY_D, ZGAME_KEY_E, ZGAME_KEY_F,
    ZGAME_KEY_G, ZGAME_KEY_H, ZGAME_KEY_I, ZGAME_KEY_J, ZGAME_KEY_K,
    ZGAME_KEY_L, ZGAME_KEY_M, ZGAME_KEY_N, ZGAME_KEY_O, ZGAME_KEY_P,
    ZGAME_KEY_Q, ZGAME_KEY_R, ZGAME_KEY_S, ZGAME_KEY_T, ZGAME_KEY_U,
    ZGAME_KEY_V, ZGAME_KEY_W, ZGAME_KEY_X, ZGAME_KEY_Y, ZGAME_KEY_Z,
    ZGAME_KEY_0, ZGAME_KEY_1, ZGAME_KEY_2, ZGAME_KEY_3, ZGAME_KEY_4,
    ZGAME_KEY_5, ZGAME_KEY_6, ZGAME_KEY_7, ZGAME_KEY_8, ZGAME_KEY_9,
    ZGAME_KEY_LARROW, ZGAME_KEY_RARROW, ZGAME_KEY_UARROW,
    ZGAME_KEY_DARROW,
    ZGAME_KEY_FN1, ZGAME_KEY_FN2, ZGAME_KEY_FN3, ZGAME_KEY_FN4,
    ZGAME_KEY_FN5, ZGAME_KEY_FN6, ZGAME_KEY_FN7, ZGAME_KEY_FN8,
    ZGAME_KEY_FN9, ZGAME_KEY_FN10, ZGAME_KEY_FN11, ZGAME_KEY_FN12,
    ZGAME_KEY_FN13, ZGAME_KEY_FN14, ZGAME_KEY_FN15, ZGAME_KEY_FN16,
    ZGAME_KEY_FN17, ZGAME_KEY_FN18, ZGAME_KEY_FN19, ZGAME_KEY_FN20,
    ZGAME_KEY_FN21, ZGAME_KEY_FN22, ZGAME_KEY_FN23, ZGAME_KEY_FN24,
    ZGAME_KEY_SPACE, ZGAME_KEY_ENTER, ZGAME_KEY_ESCAPE, ZGAME_KEY_TAB,
    ZGAME_KEY_BACKSPACE, ZGAME_KEY_LSHIFT, ZGAME_KEY_RSHIFT,
    ZGAME_KEY_LCTRL, ZGAME_KEY_RCTRL, ZGAME_KEY_LALT, ZGAME_KEY_RALT,
    ZGAME_KEY_COUNT
} ZSharpGameKey;

typedef struct ZSharpGameScene {
    char *name;
    char *title;
    char *icon;
    char *source_file;
    unsigned background;
    float gravity_x;
    float gravity_y;
    float gravity_z;
    float camera_x;
    float camera_y;
    float camera_z;
    float camera_fov;
} ZSharpGameScene;

typedef struct ZSharpGameObject {
    char *name;
    char *display_name;
    char *source_file;
    char *scene;
    ZSharpGameShape shape;
    ZSharpGameBodyType body;
    ZSharpGameColliderType collider;
    float x;
    float y;
    float z;
    float spawn_x;
    float spawn_y;
    float spawn_z;
    float width;
    float height;
    float depth;
    float rotation;
    float scale_x;
    float scale_y;
    float scale_z;
    float velocity_x;
    float velocity_y;
    float velocity_z;
    float mass;
    float gravity_scale;
    float restitution;
    float friction;
    unsigned color;
    int visible;
    int layer;
    int trigger;
    int grounded;
    int colliding;
    char *text;
    char *asset_path;
    char *audio_path;
    float audio_volume;
    float audio_pitch;
    float tone_frequency;
    float tone_duration;
    int audio_loop;
    int audio_autoplay;
    int audio_on_collision;
    void *audio_stream;
    void *audio_buffer;
    unsigned audio_length;
    int audio_started;
    int audio_playing;
    int was_colliding;
    int spawn_initialized;
    int is_audio_source;
    char **attribute_ids;
    int *attribute_active;
    size_t attribute_count;
} ZSharpGameObject;

typedef struct ZSharpGameInput {
    int keys[ZGAME_KEY_COUNT];
    int mouse_left;
    int mouse_right;
    float mouse_x;
    float mouse_y;
} ZSharpGameInput;

typedef struct ZSharpGameModel {
    int is_3d;
    ZSharpGameScene *scenes;
    size_t scene_count;
    ZSharpGameObject *objects;
    size_t object_count;
    ZSharpGameObject *definitions;
    size_t definition_count;
    char *active_scene;
    char *project_root;
    ZSharpGameInput input;
    double elapsed;
    double delta;
} ZSharpGameModel;

int zsharp_game_model_load(const char *project_root,
                           ZSharpGameModel *model, char *error,
                           size_t error_size);
int zsharp_game_model_validate(const char *project_root,
                               char *error, size_t error_size);
void zsharp_game_model_free(ZSharpGameModel *model);
void zsharp_game_model_update(ZSharpGameModel *model, double delta_seconds);
int zsharp_game_model_owns_property(const ZSharpGameModel *model,
                                    const char *path);
int zsharp_game_model_get_property(const ZSharpGameModel *model,
                                   const char *path,
                                   ZSharpWindowReadType *type, char **text,
                                   char *error, size_t error_size);
int zsharp_game_model_set_property(ZSharpGameModel *model, const char *path,
                                   ZSharpWindowValueType value_type,
                                   const char *value, char *error,
                                   size_t error_size);
void zsharp_game_model_frame(const ZSharpGameModel *model,
                             ZSharpGameRenderFrame *frame,
                             ZSharpGameRenderObject **objects);
const char *zsharp_game_model_scene_title(const ZSharpGameModel *model);
const char *zsharp_game_model_scene_icon(const ZSharpGameModel *model);

#endif
