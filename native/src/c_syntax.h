#ifndef ZSHARP_C_SYNTAX_H
#define ZSHARP_C_SYNTAX_H

#include "parser.h"

int zsharp_c_syntax_collect(const char *source, const char *project_root,
                            const char *project_id,
                            ZSharpCustomSyntaxRule **rules, size_t *count,
                            char *error, size_t error_size);
void zsharp_c_syntax_free(ZSharpCustomSyntaxRule *rules, size_t count);

#endif
