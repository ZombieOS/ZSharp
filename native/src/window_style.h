#ifndef ZSHARP_WINDOW_STYLE_H
#define ZSHARP_WINDOW_STYLE_H

#include "bytecode.h"

#include <stddef.h>

int zsharp_window_styles_validate(const ZSharpProgram *program,
                                  const char *project_root,
                                  char *error, size_t error_size);

int zsharp_window_styles_apply(ZSharpProgram *program,
                               const char *project_root,
                               char *error, size_t error_size);

#endif
