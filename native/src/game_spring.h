#ifndef ZSHARP_GAME_SPRING_H
#define ZSHARP_GAME_SPRING_H

#include <stddef.h>

#define ZSHARP_SPRING_MAX_CHAINS 32
#define ZSHARP_SPRING_MAX_BONES 64
#define ZSHARP_SPRING_MAX_COLLIDERS 64
#define ZSHARP_SPRING_MAX_TOTAL_BONES 256
#define ZSHARP_SPRING_MAX_STIFFNESS 10000.0f
#define ZSHARP_SPRING_MAX_DAMPING 1000.0f
#define ZSHARP_SPRING_MAX_GRAVITY 10000.0f
#define ZSHARP_SPRING_MAX_RADIUS 1000.0f
#define ZSHARP_SPRING_MAX_OFFSET 1000000.0f

typedef struct ZSharpSpringChain {
    char *name;
    char **bones; /* Ordered direct-parent bone names, including the tip marker. */
    size_t bone_count;
    float stiffness; /* Restoring acceleration, in inverse seconds squared. */
    float damping; /* Velocity decay, in inverse seconds. */
    float gravity[3]; /* World game units per second squared. */
    float radius; /* Tip collision radius in game units before instance scaling. */
    float max_angle; /* Maximum bend away from the animated pose, in degrees. */
} ZSharpSpringChain;

typedef struct ZSharpSpringCollider {
    char *name;
    char *bone;
    float offset[3]; /* Offsets in the anchor bone's local FBX coordinates. */
    float tail[3]; /* Capsule's other local endpoint; absent means sphere. */
    float radius; /* Game units before instance scaling. */
    int capsule;
} ZSharpSpringCollider;

typedef struct ZSharpSpringConfig {
    ZSharpSpringChain *chains;
    size_t chain_count;
    ZSharpSpringCollider *colliders;
    size_t collider_count;
} ZSharpSpringConfig;

typedef struct ZSharpSpringBonePose {
    const char *name;
    float rotation[4]; /* Local quaternion delta after the authored bone pose: x,y,z,w. */
} ZSharpSpringBonePose;

typedef struct ZSharpGameSpringState ZSharpGameSpringState;
typedef struct ZSharpGameSpringSystem ZSharpGameSpringSystem;
struct ZSharpGameModel;
struct ufbx_scene;

int zsharp_game_spring_config_copy(ZSharpSpringConfig **destination,
                                  const ZSharpSpringConfig *source);
void zsharp_game_spring_config_free(ZSharpSpringConfig *config);
int zsharp_game_spring_validate(const ZSharpSpringConfig *config,
                               const struct ufbx_scene *scene,
                               char *error, size_t error_size);
int zsharp_game_springs_prepare(struct ZSharpGameModel *model,
                               char *error, size_t error_size);
void zsharp_game_springs_update(struct ZSharpGameModel *model, double delta_seconds);
void zsharp_game_springs_reset(struct ZSharpGameModel *model);
void zsharp_game_springs_free(struct ZSharpGameModel *model);
const ZSharpSpringBonePose *zsharp_game_spring_poses(
    const ZSharpGameSpringState *state, size_t *count);

#endif
