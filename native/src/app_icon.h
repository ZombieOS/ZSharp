#ifndef ZSHARP_APP_ICON_H
#define ZSHARP_APP_ICON_H

#include <stddef.h>

/* All returned paths/identifiers are allocated strings owned by the caller.
 * Paths identify existing PNG files and are absolute. No network is used. */
char *zsharp_app_icon_default_path(char *error, size_t error_size);
char *zsharp_app_icon_resolve(const char *project_root, const char *icon,
                             char *error, size_t error_size);
/* Project settings Icon > legacy window design icon > embedded official icon.
 * NULL, empty and "zsharp-hub" IDs never read project settings. */
char *zsharp_app_icon_for_window(const char *project_root,
                                const char *project_id,
                                const char *legacy_icon,
                                char *error, size_t error_size);
/* Hub uses "zsharp-hub"; projects use a stable ASCII ID derived from their PID. */
char *zsharp_app_desktop_id(const char *project_id);
/* Basic PNG signature/IHDR validation, without image decoder dependencies. */
int zsharp_app_icon_is_png(const char *path);

#endif
