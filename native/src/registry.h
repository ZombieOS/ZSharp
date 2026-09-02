#ifndef ZSHARP_REGISTRY_H
#define ZSHARP_REGISTRY_H

#include "package.h"
#include "settings.h"

#include <stddef.h>
#include <stdint.h>

/* Registers a validated project for the current user. Registering the same
 * PID or project path again replaces the existing entry instead of creating a
 * duplicate. */
int zsharp_registry_register_project(const char *project_root,
                                     const ZSharpSettings *settings,
                                     char *error, size_t error_size);

typedef struct ZSharpInstalledPackage {
    ZSharpPackageKind kind;
    char *project_name;
    char *project_id;
    unsigned version[4];
    unsigned zsharp_version[4];
    char *package_path;
    char *icon_path;
    uint64_t total_play_seconds;
    int64_t last_played;
} ZSharpInstalledPackage;

typedef struct ZSharpInstalledPackageList {
    ZSharpInstalledPackage *items;
    size_t count;
} ZSharpInstalledPackageList;

/* Remembers packages opened by the current user so the Z# Hub can launch
 * them later. Registering the same PID or package path replaces its entry. */
int zsharp_registry_remember_package(const char *package_path,
                                     const ZSharpPackageInfo *info,
                                     char *error, size_t error_size);

/* Lists remembered packages whose .zapp/.zgame file still exists. */
int zsharp_registry_list_packages(ZSharpInstalledPackageList *packages,
                                  char *error, size_t error_size);

int zsharp_registry_forget_package(const char *project_id,
                                   char *error, size_t error_size);

int zsharp_registry_record_play(const char *project_id, uint64_t seconds,
                                int64_t started_at, char *error,
                                size_t error_size);

void zsharp_registry_package_list_free(ZSharpInstalledPackageList *packages);

#endif
