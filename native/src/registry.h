#ifndef ZSHARP_REGISTRY_H
#define ZSHARP_REGISTRY_H

#include "settings.h"

#include <stddef.h>

/* Registers a validated project for the current user. Registering the same
 * PID or project path again replaces the existing entry instead of creating a
 * duplicate. */
int zsharp_registry_register_project(const char *project_root,
                                     const ZSharpSettings *settings,
                                     char *error, size_t error_size);

#endif
