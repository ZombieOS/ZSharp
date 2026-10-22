#define _CRT_SECURE_NO_WARNINGS
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "parser.h"
#include "window.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static ZSharpFunction *startup(ZSharpProgram *program) {
    assert(program->room_count == 1);
    assert(program->rooms[0].function_count == 1);
    return &program->rooms[0].functions[0];
}

static void parse_ok(const char *source, ZSharpProgram *program) {
    ZSharpDiagnostic diagnostic = {0};
    if (!zsharp_parse_source(source, "ApplicationQuit", program, &diagnostic)) {
        fprintf(stderr, "%u:%u: %s\n", diagnostic.line,
                diagnostic.column, diagnostic.message);
        abort();
    }
    checks++;
}

static void parse_error(const char *source, const char *expected) {
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic = {0};
    int ok = zsharp_parse_source(source, "ApplicationQuit", &program,
                                 &diagnostic);
    if (ok || strstr(diagnostic.message, expected) == NULL) {
        fprintf(stderr, "Expected parse error containing '%s'; got %s\n",
                expected, ok ? "success" : diagnostic.message);
        abort();
    }
    assert(diagnostic.line > 0 && diagnostic.column > 0);
    zsharp_program_free(&program);
    checks++;
}

static void mode_program(const char *body, ZSharpProgram *program) {
    char source[4096];
    int size = snprintf(source, sizeof(source),
        "zsharp = type.script\n"
        "noticed room Main[] (\n"
        " import ZSharp.Application():\n"
        " noticed brain Start[] ( %s )\n"
        ")\n", body);
    assert(size > 0 && (size_t)size < sizeof(source));
    parse_ok(source, program);
}

static void assert_mode_instruction(ZSharpProgram *program,
                                    const char *literal) {
    ZSharpFunction *function = startup(program);
    size_t index, found = 0;
    for (index = 0; index < function->instruction_count; index++) {
        ZSharpInstruction *instruction = &function->instructions[index];
        assert(instruction->op != ZOP_APPLICATION_QUIT);
        if (instruction->op != ZOP_APPLICATION_QUIT_MODE) continue;
        found++;
        assert(index > 0);
        assert(instruction->operand == NULL);
        assert(instruction->argument_count == 0);
        if (literal != NULL) {
            assert(function->instructions[index - 1].op == ZOP_PUSH_TEXT);
            assert(strcmp(function->instructions[index - 1].operand,
                          literal) == 0);
        }
    }
    assert(found == 1);
    checks++;
}

static void test_source_contract(void) {
    ZSharpProgram program;
    assert(ZOP_APPLICATION_QUIT == 60);
    assert(ZOP_APPLICATION_QUIT_MODE == 71);
    assert(ZAPPLICATION_QUIT_REGULAR == 0);
    assert(ZAPPLICATION_QUIT_TRAY == 1);

    parse_error("zsharp = type.script noticed room Main[] ("
                " noticed brain Start[] (Application.Quit(\"regular\"):))",
                "import ZSharp.Application()");
    parse_error("zsharp = type.script noticed room Main[] ("
                " import ZSharp.Window.Application():"
                " noticed brain Start[] (Application.Quit(\"regular\"):))",
                "import ZSharp.Application()");
    parse_error("zsharp = type.script noticed room Other[] ("
                " import ZSharp.Application():)"
                " noticed room Main[] (noticed brain Start[] ("
                " Application.Quit(\"regular\"):))",
                "import ZSharp.Application()");
    parse_error("zsharp = type.script noticed room Main[] ("
                " noticed brain Start[] (Application.Quit:))",
                "explicit mode");
    parse_error("zsharp = type.script noticed room Main[] ("
                " import ZSharp.Application():"
                " noticed brain Start[] (Application.Quit:))",
                "explicit mode");
    parse_error("zsharp = type.script noticed room Main[] ("
                " import ZSharp.Application():"
                " noticed brain Start[] (Application.Quit():))",
                "expected");
    parse_error("zsharp = type.script noticed room Main[] ("
                " import ZSharp.Application():"
                " noticed brain Start[] (Application.Quit(\"regular\", \"tray\"):))",
                "single Application.Quit mode");

    mode_program("Application.Quit(\"regular\"): Print(\"after\"):", &program);
    assert_mode_instruction(&program, "regular");
    zsharp_program_free(&program);
    mode_program("Application.Quit(\"tray\"): Print(\"after\"):", &program);
    assert_mode_instruction(&program, "tray");
    zsharp_program_free(&program);
    mode_program("text Mode = \"tr\" + \"ay\": Application.Quit(Mode):", &program);
    assert_mode_instruction(&program, NULL);
    zsharp_program_free(&program);
    mode_program("Application.Quit(\"reg\" + \"ular\"):", &program);
    assert_mode_instruction(&program, NULL);
    zsharp_program_free(&program);

    /* Non-text and invalid text values must reach the runtime's validation,
       including values calculated dynamically rather than literal tokens. */
    mode_program("Application.Quit(123):", &program);
    assert_mode_instruction(&program, NULL);
    zsharp_program_free(&program);
    mode_program("Application.Quit(alive):", &program);
    assert_mode_instruction(&program, NULL);
    zsharp_program_free(&program);
    mode_program("Application.Quit(\"invalid\"):", &program);
    assert_mode_instruction(&program, "invalid");
    zsharp_program_free(&program);
    puts("PASS: Application.Quit parser, imports, arity, expressions and opcode identity");
}

static void roundtrip(const char *path, ZSharpProgram *program, unsigned minor) {
    unsigned char identity[ZSHARP_SHA256_SIZE], hash[ZSHARP_SHA256_SIZE];
    char error[512] = {0};
    ZSharpProgram read;
    ZSharpFunction *before = startup(program), *after;
    size_t index;
    if (!zsharp_bytecode_write(path, program, "application_quit_tests",
                               identity, hash, error, sizeof(error))) {
        fprintf(stderr, "Bytecode write failed: %s\n", error);
        abort();
    }
    {
        FILE *file = fopen(path, "r+b");
        assert(file != NULL);
        assert(fseek(file, 6, SEEK_SET) == 0);
        assert(fgetc(file) == 26 && fgetc(file) == 0);
        if (minor != 26) {
            /* 0.23 through 0.26 have identical envelopes. Rebuild a genuine,
               integrity-checked older header around only legacy opcodes. */
            const long hash_offset = 12L +
                (long)strlen("application_quit_tests") + ZSHARP_SHA256_SIZE;
            assert(minor == 23 || minor == 24 || minor == 25);
            assert(fseek(file, 6, SEEK_SET) == 0);
            assert(fputc((int)minor, file) != EOF && fputc(0, file) != EOF);
            assert(fclose(file) == 0);
            assert(zsharp_sha256_file_except(path, hash_offset, sizeof(hash), hash));
            file = fopen(path, "r+b");
            assert(file != NULL && fseek(file, hash_offset, SEEK_SET) == 0);
            assert(fwrite(hash, 1, sizeof(hash), file) == sizeof(hash));
        }
        assert(fclose(file) == 0);
    }
    if (!zsharp_bytecode_read(path, &read, error, sizeof(error))) {
        fprintf(stderr, "Bytecode read failed: %s\n", error);
        remove(path);
        abort();
    }
    assert(remove(path) == 0);
    after = startup(&read);
    assert(before->instruction_count == after->instruction_count);
    assert(!strcmp(read.project_id, "application_quit_tests"));
    assert(!memcmp(read.project_identity, identity, sizeof(identity)));
    assert(!memcmp(read.build_hash, hash, sizeof(hash)));
    for (index = 0; index < before->instruction_count; index++) {
        assert(before->instructions[index].op == after->instructions[index].op);
        if (before->instructions[index].operand != NULL)
            assert(after->instructions[index].operand != NULL &&
                   !strcmp(before->instructions[index].operand,
                           after->instructions[index].operand));
    }
    zsharp_program_free(&read);
    checks++;
}

static void test_bytecode_contract(const char *path) {
    ZSharpProgram program;
    ZSharpFunction *function;
    size_t old_count;
    mode_program("Application.Quit(\"regular\"): Print(\"after regular\"):", &program);
    roundtrip(path, &program, 26);
    zsharp_program_free(&program);
    mode_program("Application.Quit(\"tray\"): Print(\"after tray\"):", &program);
    roundtrip(path, &program, 26);
    zsharp_program_free(&program);

    /* Construct legacy bytecode directly: old source is intentionally no
       longer accepted, but opcode 60 must remain readable and operandless. */
    parse_ok("zsharp = type.script noticed room Main[] ("
             " noticed brain Start[] (Print(\"after legacy\"):))", &program);
    function = startup(&program);
    old_count = function->instruction_count;
    assert(zsharp_function_add_instruction(function) != NULL);
    memmove(&function->instructions[1], &function->instructions[0],
            old_count * sizeof(function->instructions[0]));
    memset(&function->instructions[0], 0, sizeof(function->instructions[0]));
    function->instructions[0].op = ZOP_APPLICATION_QUIT;
    roundtrip(path, &program, 26);
    roundtrip(path, &program, 25);
    roundtrip(path, &program, 24);
    roundtrip(path, &program, 23);
    assert(function->instructions[0].op == 60);
    assert(function->instructions[1].op == ZOP_PUSH_TEXT);
    zsharp_program_free(&program);
    puts("PASS: mode opcode 71 and legacy opcode 60 bytecode round trips");
}

int main(int argc, char **argv) {
    FILE *existing;
    if (argc != 2) {
        fprintf(stderr, "Usage: application-quit-tests <temporary-bytecode-path>\n");
        return 1;
    }
    existing = fopen(argv[1], "rb");
    if (existing != NULL) {
        fclose(existing);
        fprintf(stderr, "Refusing to overwrite existing test file: %s\n", argv[1]);
        return 1;
    }
    test_source_contract();
    test_bytecode_contract(argv[1]);
    printf("PASS: %u Application.Quit checks\n", checks);
    return 0;
}
