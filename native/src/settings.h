#ifndef ZSHARP_SETTINGS_H
#define ZSHARP_SETTINGS_H

#include "parser.h"

#include <stddef.h>
#include <stdint.h>

#define ZSHARP_SETTINGS_FILE "project.zsettings"
#define ZSHARP_VERSION_PART_COUNT 4
#define ZSHARP_CURRENT_GENERATION 1u

typedef struct ZSharpDependency {
    char *project_id;
    uint32_t version[ZSHARP_VERSION_PART_COUNT];
} ZSharpDependency;

typedef struct ZSharpSettings {
    char *project_name;
    char *project_id;
    uint32_t version[ZSHARP_VERSION_PART_COUNT];
    char **authors;
    size_t author_count;
    char *description;
    char *icon;
    uint32_t zsharp_version[ZSHARP_VERSION_PART_COUNT];
    ZSharpDependency *dependencies;
    size_t dependency_count;
    int has_window;
    char *window_startup;
    char *window_uninstall;
} ZSharpSettings;

void zsharp_settings_init(ZSharpSettings *settings);
void zsharp_settings_free(ZSharpSettings *settings);

int zsharp_settings_parse_file(const char *path, ZSharpSettings *settings,
                               ZSharpDiagnostic *diagnostic, char *error,
                               size_t error_size);

int zsharp_settings_parse_source(const char *source, ZSharpSettings *settings,
                                 ZSharpDiagnostic *diagnostic, char *error,
                                 size_t error_size);

int zsharp_settings_load(const char *project_root, ZSharpSettings *settings,
                         ZSharpDiagnostic *diagnostic, char *error,
                         size_t error_size);

const ZSharpDependency *zsharp_settings_find_dependency(
    const ZSharpSettings *settings, const char *project_id);

#endif
