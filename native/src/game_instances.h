#ifndef ZSHARP_GAME_INSTANCES_H
#define ZSHARP_GAME_INSTANCES_H
#include "game_model.h"
int zsharp_game_instances_build(ZSharpGameModel *model, char *error, size_t size);
void zsharp_game_instances_free(ZSharpGameModel *model);
/* scene NULL means current active scene; never fall back to another scene.
   ambiguous is set when a key exists but cannot identify one placement. */
ZSharpGameObject *zsharp_game_instance(const ZSharpGameModel *model,
    const char *scene, const char *key, int *ambiguous);
int zsharp_game_instance_identifier(const char *key);
#endif
