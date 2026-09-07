#ifndef ZSHARP_ACHIEVEMENT_H
#define ZSHARP_ACHIEVEMENT_H

#include <stddef.h>

typedef struct ZSharpAchievement {
    char *id;
    char *display;
    char *description;
    char *rarity;
} ZSharpAchievement;

int zsharp_achievement_find(const char *project_root, const char *id,
                            ZSharpAchievement *achievement,
                            char *error, size_t error_size);
void zsharp_achievement_free(ZSharpAchievement *achievement);

#endif
