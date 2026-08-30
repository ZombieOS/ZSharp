#ifndef ZSHARP_DESKTOP_H
#define ZSHARP_DESKTOP_H

#include <stddef.h>

int zsharp_desktop_install_associations(char *error, size_t error_size);

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
                                   char *error, size_t error_size);
int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size);
int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size);

#endif
