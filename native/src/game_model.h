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

typedef struct ZSharpGameScene {
    char *name;
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
    char *source_file;
    char *scene;
    ZSharpGameShape shape;
    ZSharpGameBodyType body;
    ZSharpGameColliderType collider;
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
    float velocity_x;
    float velocity_y;
    float velocity_z;
    float mass;
    float gravity_scale;
    float restitution;
    float friction;
    float control_x;
    float control_y;
    float jump_speed;
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
    float tone_frequency;
    float tone_duration;
    int audio_loop;
    int audio_autoplay;
    int audio_on_collision;
    void *audio_stream;
    void *audio_buffer;
    unsigned audio_length;
    int audio_started;
    int was_colliding;
} ZSharpGameObject;

typedef struct ZSharpGameInput {
    int left;
    int right;
    int up;
    int down;
    int space;
    int action;
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
    char *active_scene;
    char *project_root;
    ZSharpGameInput input;
    double elapsed;
    double delta;
} ZSharpGameModel;

int zsharp_game_model_load(const char *project_root, int is_3d,
                           ZSharpGameModel *model, char *error,
                           size_t error_size);
int zsharp_game_model_validate(const char *project_root, int is_3d,
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

#endif
