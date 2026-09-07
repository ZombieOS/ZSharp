#define _CRT_SECURE_NO_WARNINGS
#include "achievement.h"
#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    long length;
    char *text;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL || (length != 0 &&
        fread(text, 1, (size_t)length, file) != (size_t)length)) {
        free(text); fclose(file); return NULL;
    }
    text[length] = '\0'; fclose(file); return text;
}

static char *json_text(const char *source, const char *key) {
    char pattern[96];
    const char *at;
    const char *start;
    const char *end;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    at = strstr(source, pattern);
    if (at == NULL || (at = strchr(at + strlen(pattern), ':')) == NULL)
        return NULL;
    start = strchr(at + 1, '"');
    if (start == NULL) return NULL;
    end = strchr(++start, '"');
    if (end == NULL) return NULL;
    return zsharp_copy_text(start, (size_t)(end - start));
}

void zsharp_achievement_free(ZSharpAchievement *achievement) {
    if (achievement == NULL) return;
    free(achievement->id); free(achievement->display);
    free(achievement->description); free(achievement->rarity);
    memset(achievement, 0, sizeof(*achievement));
}

int zsharp_achievement_find(const char *project_root, const char *id,
                            ZSharpAchievement *achievement,
                            char *error, size_t error_size) {
    ZSharpSourceList sources;
    size_t index;
    char marker[512];
    memset(achievement, 0, sizeof(*achievement));
    snprintf(marker, sizeof(marker), "achievement %s[JSON]", id);
    if (!zsharp_project_list_sources(project_root, &sources, error, error_size))
        return 0;
    for (index = 0; index < sources.count; index++) {
        char *text = read_all(sources.items[index]);
        if (text != NULL && strstr(text, "zsharp = type.script:achievement") != NULL &&
            strstr(text, marker) != NULL) {
            achievement->id = zsharp_copy_text(id, strlen(id));
            achievement->display = json_text(text, "display");
            achievement->description = json_text(text, "description");
            achievement->rarity = json_text(text, "rarity");
            free(text);
            zsharp_project_source_list_free(&sources);
            if (achievement->id == NULL || achievement->display == NULL ||
                achievement->description == NULL || achievement->rarity == NULL) {
                zsharp_achievement_free(achievement);
                snprintf(error, error_size, "achievement '%s' has invalid ZSON", id);
                return 0;
            }
            if (strcmp(achievement->rarity, "EASY") != 0 &&
                strcmp(achievement->rarity, "MEDIUM") != 0 &&
                strcmp(achievement->rarity, "HARD") != 0 &&
                strcmp(achievement->rarity, "IMPOSSIBLE") != 0) {
                zsharp_achievement_free(achievement);
                snprintf(error, error_size, "achievement '%s' has invalid rarity", id);
                return 0;
            }
            return 1;
        }
        free(text);
    }
    zsharp_project_source_list_free(&sources);
    snprintf(error, error_size, "unknown achievement '%s'", id);
    return 0;
}
