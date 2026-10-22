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

typedef struct ZSharpSplash {
    char *path;
    double duration_seconds;
} ZSharpSplash;

typedef struct ZSharpNativeTarget {
    char *platform;
    char *start;
} ZSharpNativeTarget;

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
    char *game_start_scene;
    ZSharpSplash *splashes;
    size_t splash_count;
    ZSharpNativeTarget *native_targets;
    size_t native_target_count;
    int has_allowed_devices;
    unsigned allowed_devices; /* Windows=1, Linux=2, MacOS=4; XR is separate. */
    int xr_allowed;
    int xr_required;
    int has_launch_at_start;
    int launch_at_start;
} ZSharpSettings;

void zsharp_settings_init(ZSharpSettings *settings);
int zsharp_settings_host_allowed(const ZSharpSettings *settings, char *error, size_t capacity);
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

const ZSharpNativeTarget *zsharp_settings_native_target(
    const ZSharpSettings *settings, const char *platform);

#endif
