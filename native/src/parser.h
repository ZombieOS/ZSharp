#ifndef ZSHARP_PARSER_H
#define ZSHARP_PARSER_H

#include "bytecode.h"

#include <stddef.h>

typedef struct ZSharpDiagnostic {
    unsigned line;
    unsigned column;
    char message[256];
} ZSharpDiagnostic;

int zsharp_parse_source(const char *source, const char *source_name,
                        ZSharpProgram *program, ZSharpDiagnostic *diagnostic);

#endif
