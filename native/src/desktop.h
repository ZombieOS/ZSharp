#ifndef ZSHARP_DESKTOP_H
#define ZSHARP_DESKTOP_H

#include <stddef.h>

int zsharp_desktop_install_associations(char *error, size_t error_size);
int zsharp_desktop_create_hub_shortcut(char *error, size_t error_size);

/* Register the identity used by the Linux dock, independently of whether a
 * user requested a shortcut on the desktop. Other platforms need no entry. */
int zsharp_desktop_register_application(const char *display_name,
                                         const char *project_id,
                                         const char *entry_path,
                                         int entry_kind,
                                         const char *icon_path,
                                         char *error, size_t error_size);

enum {
    ZDESKTOP_APP_SOURCE = 0,
    ZDESKTOP_APP_BYTECODE = 1,
    ZDESKTOP_APP_PACKAGE = 2,
    ZDESKTOP_APP_HUB = 3
};

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
