#define _CRT_SECURE_NO_WARNINGS
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LanguageCase {
    const char *prefix;
    const char *tag;
    const char *extension;
    const char *companion;
} LanguageCase;

static unsigned checks;
static const LanguageCase languages[] = {
    {"cs", "@cs", "cs", "zcs.dll"},
    {"java", "@java", "java", "zjava.jar"},
#ifdef _WIN32
    {"go", "@go", "go", "zgo.exe"},
#else
    {"go", "@go", "go", "zgo"},
#endif
    {"rb", "@rb", "rb", NULL}
};

static void parse(const char *source, ZSharpProgram *program) {
    ZSharpDiagnostic diagnostic = {0};
    if (!zsharp_parse_source(source, "Entry", program, &diagnostic)) {
        fprintf(stderr, "%u:%u: %s\n", diagnostic.line,
                diagnostic.column, diagnostic.message);
        abort();
    }
    checks++;
}

static void check_program(const ZSharpProgram *program,
                          const LanguageCase *language) {
    const ZSharpRoom *room = &program->rooms[0];
    const ZSharpFunction *function = &room->functions[0];
    size_t index, calls = 0, values = 0, references = 0;
    char exact[128], wildcard[128];
    snprintf(exact, sizeof(exact), "%s:bridge_fixture.Module", language->prefix);
    snprintf(wildcard, sizeof(wildcard), "%s:bridge_fixture.*", language->prefix);
    assert(room->import_count == 2);
    assert(strcmp(room->imports[0].path, exact) == 0);
    assert(strcmp(room->imports[1].path, wildcard) == 0);
    assert(room->variable_count == 1);
    assert(room->variables[0].type == ZVALUE_FUNCTION);
    for (index = 0; index < function->instruction_count; index++) {
        const ZSharpInstruction *instruction = &function->instructions[index];
        if (instruction->op == ZOP_PUSH_FUNCTION) references++;
        if (instruction->op != ZOP_CALL_QUALIFIED &&
            instruction->op != ZOP_CALL_QUALIFIED_VALUE) continue;
        if (strcmp(instruction->operand, "@callback") == 0) continue;
        assert(strcmp(instruction->operand, language->tag) == 0);
        assert(strcmp(instruction->call_room, "") == 0);
        assert(strcmp(instruction->call_function, "echo") == 0);
        assert(instruction->argument_count == 1);
        if (instruction->op == ZOP_CALL_QUALIFIED_VALUE) {
            assert(strcmp(instruction->call_file, "bridge_fixture.Module") == 0);
            values++;
        } else {
            assert(strcmp(instruction->call_file, "Module") == 0);
            calls++;
        }
    }
    assert(calls == 1 && values == 1 && references == 1);
    checks++;
}

static void check_decode(void) {
    static const char *prefixes[] = {
        "py", "js", "lua", "cpp", "rust", "c", "kt", "cs", "java", "go", "rb"
    };
    size_t index;
    for (index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); index++) {
        char text[128], tag[16], storage[256], error[256];
        ZSharpInstruction target = {0};
        snprintf(text, sizeof(text), "%s:Folder.Module:echo", prefixes[index]);
        snprintf(tag, sizeof(tag), "@%s", prefixes[index]);
        assert(zsharp_call_target_decode(text, &target, storage, sizeof(storage),
                                         error, sizeof(error)));
        assert(strcmp(target.operand, tag) == 0);
        assert(strcmp(target.call_file, "Folder.Module") == 0);
        assert(strcmp(target.call_room, "") == 0);
        assert(strcmp(target.call_function, "echo") == 0);
        checks++;
    }
}

static void check_language(const LanguageCase *language, const char *bytecode_path) {
    char source[4096], error[1024] = {0};
    unsigned char identity[ZSHARP_SHA256_SIZE] = {0}, hash[ZSHARP_SHA256_SIZE];
    ZSharpProgram program, decoded;
    snprintf(source, sizeof(source),
        "zsharp = type.script\n"
        "noticed room Main[] (\n"
        " import %s:bridge_fixture.Module():\n"
        " import %s:bridge_fixture.*():\n"
        " noticed function Forward = %s:Module:echo:\n"
        " noticed brain Start[] (\n"
        "  Function.call(%s:Module:echo [\"hello\"]):\n"
        "  text Result = Function.call(%s:bridge_fixture.Module:echo [\"value\"]):\n"
        "  function Local = %s:Module:echo:\n"
        "  Function.call(Local [\"callback\"]):\n"
        " )\n)\n", language->prefix, language->prefix, language->prefix,
        language->prefix, language->prefix, language->prefix);
    parse(source, &program);
    check_program(&program, language);
    assert(zsharp_bytecode_write(bytecode_path, &program, "foreign_import_test",
                                 identity, hash, error, sizeof(error)));
    assert(zsharp_bytecode_read(bytecode_path, &decoded, error, sizeof(error)));
    check_program(&decoded, language);
    assert(remove(bytecode_path) == 0);
    zsharp_program_free(&decoded);
    zsharp_program_free(&program);
}

static void check_invalid(void) {
    static const char *invalid[] = {
        "zsharp = type.script noticed room Main[] (import ruby:bridge_fixture.Module():)",
        "zsharp = type.script noticed room Main[] (noticed brain Start[] (Function.call(java:Module):))",
        "zsharp = type.script noticed room Main[] (import cs:bridge_fixture.Module(): import cs:bridge_fixture.Module():)"
    };
    size_t index;
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        ZSharpProgram program;
        ZSharpDiagnostic diagnostic = {0};
        assert(!zsharp_parse_source(invalid[index], "Invalid", &program, &diagnostic));
        assert(diagnostic.message[0] != '\0');
        zsharp_program_free(&program);
        checks++;
    }
}

int main(int argc, char **argv) {
    size_t index;
    FILE *existing;
    assert(argc == 2);
    existing = fopen(argv[1], "rb");
    assert(existing == NULL);
    check_decode();
    check_invalid();
    for (index = 0; index < sizeof(languages) / sizeof(languages[0]); index++)
        check_language(&languages[index], argv[1]);
    printf("PASS: %u foreign import, callback and bytecode checks\n", checks);
    return 0;
}
