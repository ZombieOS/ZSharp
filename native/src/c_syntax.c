#include "c_syntax.h"

#include "lexer.h"
#include "zsharp_c.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct SyntaxCollector {
    ZSharpCustomSyntaxRule *rules;
    size_t count;
    const char *module;
} SyntaxCollector;

static char *syntax_copy(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

void zsharp_c_syntax_free(ZSharpCustomSyntaxRule *rules, size_t count) {
    size_t index;
    for (index = 0; index < count; index++) {
        free(rules[index].pattern);
        free(rules[index].function);
        free(rules[index].module);
    }
    free(rules);
}

static int reserved_start(const ZSharpToken *token) {
    static const char *reserved[] = {
        "Print", "Function", "File", "number", "text", "JSON", "feed",
        "if", "loop", "continue", "wait", "delay", "status"
    };
    size_t index;
    for (index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index++)
        if (zsharp_token_equals(token, reserved[index])) return 1;
    return 0;
}

static int register_statement(void *context, const char *pattern,
                              const char *function, char *error,
                              size_t error_size) {
    SyntaxCollector *collector = (SyntaxCollector *)context;
    ZSharpLexer lexer;
    ZSharpToken first, token;
    ZSharpCustomSyntaxRule *resized;
    size_t index, captures = 0;
    if (pattern == NULL || function == NULL || pattern[0] == '\0' ||
        function[0] == '\0' || strlen(pattern) > 255 ||
        strlen(function) > 127 ||
        !(isalpha((unsigned char)function[0]) || function[0] == '_')) {
        snprintf(error, error_size, "invalid C syntax registration");
        return 0;
    }
    for (index = 1; function[index] != '\0'; index++)
        if (!isalnum((unsigned char)function[index]) && function[index] != '_') {
            snprintf(error, error_size, "invalid C syntax handler name");
            return 0;
        }
    zsharp_lexer_init(&lexer, pattern);
    first = zsharp_lexer_next(&lexer);
    if (first.type != ZTOKEN_IDENTIFIER || reserved_start(&first)) {
        snprintf(error, error_size,
                 "custom C syntax cannot replace a built-in statement");
        return 0;
    }
    for (index = 0; index < collector->count; index++) {
        ZSharpLexer previous;
        ZSharpToken start;
        zsharp_lexer_init(&previous, collector->rules[index].pattern);
        start = zsharp_lexer_next(&previous);
        if (start.length == first.length &&
            memcmp(start.start, first.start, first.length) == 0) {
            snprintf(error, error_size,
                     "custom C syntax conflicts with another registered statement");
            return 0;
        }
    }
    zsharp_lexer_init(&lexer, pattern);
    while ((token = zsharp_lexer_next(&lexer)).type != ZTOKEN_EOF) {
        if (token.type == ZTOKEN_ERROR || token.type == ZTOKEN_COLON ||
            token.type == ZTOKEN_EQUAL || token.type == ZTOKEN_LEFT_BRACKET ||
            token.type == ZTOKEN_RIGHT_BRACKET) {
            snprintf(error, error_size, "invalid C syntax pattern");
            return 0;
        }
        if (token.type == ZTOKEN_LEFT_BRACE) {
            ZSharpToken name = zsharp_lexer_next(&lexer);
            ZSharpToken close = zsharp_lexer_next(&lexer);
            if (name.type != ZTOKEN_IDENTIFIER ||
                close.type != ZTOKEN_RIGHT_BRACE || ++captures > 32) {
                snprintf(error, error_size,
                         "C syntax placeholders must be {names} (maximum 32)");
                return 0;
            }
        } else if (token.type == ZTOKEN_RIGHT_BRACE) {
            snprintf(error, error_size, "unmatched '}' in C syntax pattern");
            return 0;
        }
    }
    resized = (ZSharpCustomSyntaxRule *)realloc(
        collector->rules, (collector->count + 1) * sizeof(*resized));
    if (resized == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    collector->rules = resized;
    resized[collector->count].pattern = syntax_copy(pattern, strlen(pattern));
    resized[collector->count].function = syntax_copy(function, strlen(function));
    resized[collector->count].module = syntax_copy(collector->module,
                                                   strlen(collector->module));
    resized[collector->count].is_block = 0;
    if (resized[collector->count].pattern == NULL ||
        resized[collector->count].function == NULL ||
        resized[collector->count].module == NULL) {
        free(resized[collector->count].pattern);
        free(resized[collector->count].function);
        free(resized[collector->count].module);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    collector->count++;
    return 1;
}

static int register_block(void *context, const char *name,
                          const char *function, char *error,
                          size_t error_size) {
    SyntaxCollector *collector = (SyntaxCollector *)context;
    ZSharpLexer lexer;
    ZSharpToken first, end;
    if (name == NULL) {
        snprintf(error, error_size, "C block name is missing");
        return 0;
    }
    zsharp_lexer_init(&lexer, name);
    first = zsharp_lexer_next(&lexer);
    end = zsharp_lexer_next(&lexer);
    if (first.type != ZTOKEN_IDENTIFIER || end.type != ZTOKEN_EOF) {
        snprintf(error, error_size, "C block name must be one identifier");
        return 0;
    }
    if (!register_statement(context, name, function, error, error_size))
        return 0;
    collector->rules[collector->count - 1].is_block = 1;
    return 1;
}

static int register_module(SyntaxCollector *collector, const char *root,
                           const char *module, char *error,
                           size_t error_size) {
    char relative[1024], path[2048];
    size_t index;
    ZSharpCRegisterV1 entry;
    ZSharpCSyntaxRegistry registry;
    FILE *probe;
#ifdef _WIN32
    HMODULE library;
#else
    void *library;
#endif
    if (strlen(module) >= sizeof(relative) || strstr(module, "..") != NULL ||
        strchr(module, '/') != NULL || strchr(module, '\\') != NULL) {
        snprintf(error, error_size, "invalid C syntax module path");
        return 0;
    }
    memcpy(relative, module, strlen(module) + 1);
    for (index = 0; relative[index] != '\0'; index++)
        if (relative[index] == '.') relative[index] =
#ifdef _WIN32
            '\\';
#else
            '/';
#endif
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\%s.zc.dll", root, relative);
    probe = fopen(path, "rb");
    if (probe == NULL) return 1;
    fclose(probe);
    library = LoadLibraryA(path);
#elif defined(__APPLE__)
    snprintf(path, sizeof(path), "%s/%s.zc.dylib", root, relative);
    probe = fopen(path, "rb");
    if (probe == NULL) return 1;
    fclose(probe);
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#else
    snprintf(path, sizeof(path), "%s/%s.zc.so", root, relative);
    probe = fopen(path, "rb");
    if (probe == NULL) return 1;
    fclose(probe);
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    /* Ordinary C calls may be checked before the native module is built. */
    if (library == NULL) {
        snprintf(error, error_size,
                 "could not load compiled C syntax module '%s'", path);
        return 0;
    }
#ifdef _WIN32
    entry = (ZSharpCRegisterV1)(void *)GetProcAddress(
        library, ZSHARP_C_REGISTER_NAME);
#else
    entry = (ZSharpCRegisterV1)dlsym(library, ZSHARP_C_REGISTER_NAME);
#endif
    if (entry != NULL) {
        collector->module = module;
        registry.abi_version = ZSHARP_C_ABI_VERSION;
        registry.context = collector;
        registry.add_statement = register_statement;
        registry.add_block = register_block;
        if (!entry(&registry, error, error_size)) {
            if (error[0] == '\0')
                snprintf(error, error_size, "C syntax registration failed");
#ifdef _WIN32
            FreeLibrary(library);
#else
            dlclose(library);
#endif
            return 0;
        }
    }
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return 1;
}

int zsharp_c_syntax_collect(const char *source, const char *project_root,
                            const char *project_id,
                            ZSharpCustomSyntaxRule **rules, size_t *count,
                            char *error, size_t error_size) {
    SyntaxCollector collector = {0};
    ZSharpLexer lexer;
    ZSharpToken token;
    char **modules = NULL;
    size_t module_count = 0, index;
    *rules = NULL;
    *count = 0;
    zsharp_lexer_init(&lexer, source);
    while ((token = zsharp_lexer_next(&lexer)).type != ZTOKEN_EOF) {
        ZSharpLexer lookahead;
        ZSharpToken next, colon, project;
        char module[1024];
        size_t length = 0;
        int duplicate = 0;
        if (!zsharp_token_equals(&token, "import")) continue;
        lookahead = lexer;
        next = zsharp_lexer_next(&lookahead);
        colon = zsharp_lexer_next(&lookahead);
        project = zsharp_lexer_next(&lookahead);
        if (!zsharp_token_equals(&next, "c") ||
            colon.type != ZTOKEN_COLON ||
            project.type != ZTOKEN_IDENTIFIER ||
            strlen(project_id) != project.length ||
            memcmp(project.start, project_id, project.length) != 0) continue;
        next = zsharp_lexer_next(&lookahead);
        while (next.type == ZTOKEN_DOT) {
            ZSharpToken part = zsharp_lexer_next(&lookahead);
            if (part.type != ZTOKEN_IDENTIFIER ||
                length + part.length + 2 >= sizeof(module)) break;
            if (length != 0) module[length++] = '.';
            memcpy(module + length, part.start, part.length);
            length += part.length;
            next = zsharp_lexer_next(&lookahead);
        }
        if (length == 0 || next.type != ZTOKEN_LEFT_PAREN) continue;
        module[length] = '\0';
        for (index = 0; index < module_count; index++)
            if (strcmp(modules[index], module) == 0) duplicate = 1;
        if (duplicate) continue;
        {
            char **resized = (char **)realloc(
                modules, (module_count + 1) * sizeof(*modules));
            if (resized == NULL) goto memory_error;
            modules = resized;
            modules[module_count] = syntax_copy(module, length);
            if (modules[module_count] == NULL) goto memory_error;
            module_count++;
        }
        if (!register_module(&collector, project_root, module,
                             error, error_size)) goto failed;
    }
    for (index = 0; index < module_count; index++) free(modules[index]);
    free(modules);
    *rules = collector.rules;
    *count = collector.count;
    return 1;
memory_error:
    snprintf(error, error_size, "out of memory");
failed:
    for (index = 0; index < module_count; index++) free(modules[index]);
    free(modules);
    zsharp_c_syntax_free(collector.rules, collector.count);
    return 0;
}
