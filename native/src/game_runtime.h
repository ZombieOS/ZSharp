#ifndef ZSHARP_GAME_RUNTIME_H
#define ZSHARP_GAME_RUNTIME_H

#include "window_runtime.h"

#include <stddef.h>

int zsharp_game_runtime_available(void);
const char *zsharp_game_runtime_backend(void);

int zsharp_game_run(const char *title, const char *project_root, int is_3d,
                    ZSharpWindowCallback callback, void *user_data,
                    char *error, size_t error_size);

#endif
