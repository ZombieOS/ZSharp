#ifndef ZSHARP_GAME_NAVIGATION_H
#define ZSHARP_GAME_NAVIGATION_H

#include <stddef.h>

struct ZSharpGameModel;

int zsharp_game_navigation_has_command(const struct ZSharpGameModel *model,
                                        const char *path);
int zsharp_game_navigation_has_sight_query(const struct ZSharpGameModel *model,
                                            const char *path);
int zsharp_game_navigation_can_see(const struct ZSharpGameModel *model,
                                   const char *path);
int zsharp_game_navigation_command(struct ZSharpGameModel *model,
                                    const char *path, const char *value,
                                    char *error, size_t error_size);
void zsharp_game_navigation_update(struct ZSharpGameModel *model,
                                    double delta_seconds);

#endif
