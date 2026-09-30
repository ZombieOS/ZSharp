#ifndef ZSHARP_PARSER_H
#define ZSHARP_PARSER_H

#include "bytecode.h"

#include <stddef.h>

typedef struct ZSharpDiagnostic {
    unsigned line;
    unsigned column;
    char message[256];
} ZSharpDiagnostic;

typedef struct ZSharpCustomSyntaxRule {
    char *pattern;
    char *function;
    char *module;
    int is_block;
} ZSharpCustomSyntaxRule;

int zsharp_parse_source_with_syntax(
    const char *source, const char *source_name, ZSharpProgram *program,
    ZSharpDiagnostic *diagnostic, const ZSharpCustomSyntaxRule *rules,
    size_t rule_count);

int zsharp_parse_source(const char *source, const char *source_name,
                        ZSharpProgram *program, ZSharpDiagnostic *diagnostic);

#endif
