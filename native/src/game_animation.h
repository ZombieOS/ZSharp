#ifndef ZSHARP_GAME_ANIMATION_H
#define ZSHARP_GAME_ANIMATION_H

#include <stddef.h>

struct ZSharpGameModel;

typedef struct ZSharpAnimationKey {
    double frame;
    float position[3];
    float rotation[3];
    unsigned char *target_mask;
} ZSharpAnimationKey;

typedef struct ZSharpAnimationPart {
    char *name;
    ZSharpAnimationKey *keys;
    size_t key_count;
} ZSharpAnimationPart;

typedef struct ZSharpAnimationClip {
    char *name;
    char **targets;
    size_t target_count;
    ZSharpAnimationPart *parts;
    size_t part_count;
    double fps;
    double last_frame;
    double elapsed;
    int tween;
    int loop;
    int state; /* 0 stopped, 1 playing, 2 paused */
    unsigned *selection;
    size_t selection_count;
    float *base_transforms; /* object_count * six values while playing */
} ZSharpAnimationClip;

typedef struct ZSharpAnimationFile {
    char *name;
    ZSharpAnimationClip *clips;
    size_t clip_count;
} ZSharpAnimationFile;

int zsharp_game_animation_load(struct ZSharpGameModel *model,
                               const char *path, char *error,
                               size_t error_size);
void zsharp_game_animation_free(struct ZSharpGameModel *model);
int zsharp_game_animation_has_command(const struct ZSharpGameModel *model,
                                      const char *path);
int zsharp_game_animation_command(struct ZSharpGameModel *model,
                                  const char *path, const char *selection,
                                  char *error, size_t error_size);
void zsharp_game_animation_update(struct ZSharpGameModel *model,
                                  double delta_seconds);

#endif
