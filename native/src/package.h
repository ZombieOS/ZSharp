#ifndef ZSHARP_PACKAGE_H
#define ZSHARP_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#define ZSHARP_PACKAGE_STARTUP_BYTECODE ".zsharp-bytecode/startup.zbc"

typedef enum ZSharpPackageKind {
    ZSHARP_PACKAGE_APP = 1,
    ZSHARP_PACKAGE_GAME = 2
} ZSharpPackageKind;

typedef struct ZSharpPackageInfo {
    ZSharpPackageKind kind;
    char *project_id;
    char *project_name;
    uint32_t version[4];
    uint32_t file_count;
} ZSharpPackageInfo;

void zsharp_package_info_free(ZSharpPackageInfo *info);

int zsharp_package_read_info(const char *package_path, ZSharpPackageInfo *info,
                             char *error, size_t error_size);

int zsharp_package_create(const char *project_path, const char *output_path,
                          char *error, size_t error_size);

/* Creates Packages/<package_name>.zapp or .zgame below the project root and
 * returns the allocated absolute output path through output_path. */
int zsharp_package_create_named(const char *project_path,
                                ZSharpPackageKind kind,
                                const char *package_name,
                                char **output_path, char *error,
                                size_t error_size);

/* Creates the ZIP-compatible source companion used by --unbytecode. */
int zsharp_package_create_unbytecoded_named(
    const char *project_path, ZSharpPackageKind kind,
    const char *package_name, char **output_path, char *error,
    size_t error_size);

int zsharp_package_extract(const char *package_path, char **project_root,
                           ZSharpPackageInfo *info, int *new_install,
                           char *error,
                           size_t error_size);

int zsharp_package_uninstall(const char *package_path, int delete_package,
                             char *error, size_t error_size);

#endif
