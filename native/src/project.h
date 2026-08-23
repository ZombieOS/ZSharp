#ifndef ZSHARP_PROJECT_H
#define ZSHARP_PROJECT_H

#include "bytecode.h"
#include "parser.h"
#include "settings.h"

#include <stddef.h>

char *zsharp_project_current_directory(char *error, size_t error_size);

int zsharp_project_find_source(const char *project_root, const char *file_name,
                               char **source_path, char *error,
                               size_t error_size);

int zsharp_project_parse_file(const char *path, ZSharpProgram *program,
                              ZSharpDiagnostic *diagnostic, char *error,
                              size_t error_size);

int zsharp_project_validate(const ZSharpProgram *program,
                            const ZSharpSettings *settings,
                            const char *project_root, char *error,
                            size_t error_size);

#endif
