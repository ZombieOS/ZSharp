#ifndef ZSHARP_DESKTOP_API_H
#define ZSHARP_DESKTOP_API_H

#include "settings.h"
#include <stddef.h>

/* Arguments are already evaluated text, not command-language fragments.
 * Selectors are a decimal PID or an exact process/application name. */
int zsharp_desktop_action(const char *operation, const char *const *text_args,
                          size_t count, char *error, size_t error_size);

typedef enum ZSharpDesktopEntryKind {
    ZDESKTOP_ENTRY_SOURCE = 0,
    ZDESKTOP_ENTRY_PACKAGE = 1,
    ZDESKTOP_ENTRY_BYTECODE = 2
} ZSharpDesktopEntryKind;

/* Called only by actual launch paths, never by checking/building/packaging.
 * An omitted setting leaves registration unchanged. A present dead removes
 * only this PID's entry. ZSHARP_SKIP_DESKTOP_INTEGRATION disables all writes;
 * ZSHARP_STARTUP_DIRECTORY redirects writes to an existing absolute test dir. */
int zsharp_desktop_apply_startup(const ZSharpSettings *settings,
                                 const char *entry_path, int entry_kind,
                                 char *error, size_t error_size);
int zsharp_desktop_remove_startup(const char *project_id,
                                  char *error, size_t error_size);

/* Pure document-generation helper; caller owns the returned buffer.
 * platform is windows/linux/macos; executable and entry are absolute paths. */
char *zsharp_desktop_startup_document(const char *platform, const char *project_id,
                                      const char *display_name,
                                      const char *executable, const char *entry,
                                      int entry_kind, char *error, size_t error_size);

#endif
