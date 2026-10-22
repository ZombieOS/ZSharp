#ifdef NDEBUG
#undef NDEBUG
#endif
#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;

static void check(const ZSharpProgram *program, const char *prefix) {
    const ZSharpFunction *function = &program->rooms[0].functions[0];
    size_t index;
    unsigned constructors = 0, locals = 0, methods = 0, releases = 0;
    char tag[32];
    snprintf(tag, sizeof(tag), "@%s", prefix);
    for (index = 0; index < function->instruction_count; ++index) {
        const ZSharpInstruction *instruction = &function->instructions[index];
        if (instruction->op == ZOP_CALL_QUALIFIED_VALUE && instruction->call_room &&
            strcmp(instruction->call_room, "$new") == 0) {
            assert(strcmp(instruction->operand, tag) == 0);
            assert(strcmp(instruction->call_file, "Models.Person") == 0);
            assert(strcmp(instruction->call_function, "Person") == 0);
            assert(instruction->argument_count == 2);
            ++constructors;
        }
        if (instruction->op == ZOP_STORE_LOCAL_VALUE) {
            if (instruction->call_room) assert(!strcmp(instruction->call_room, "$object"));
            ++locals;
        }
        if (instruction->op == ZOP_CALL_METHOD_PATH && !strcmp(instruction->operand, "Person")) {
            assert(!strcmp(instruction->call_function, "greet") && instruction->index_operand == 1);
            ++methods;
        }
        if (instruction->op == ZOP_CALL_METHOD && !strcmp(instruction->operand, "$foreign.release")) {
            assert(instruction->argument_count == 0);
            ++releases;
        }
    }
    assert(constructors == 1 && locals == 2 && methods == 1 && releases == 1);
    assert(program->rooms[0].functions[1].parameters[0].type == ZVALUE_OBJECT);
    assert(!strcmp(program->rooms[0].functions[1].parameters[0].object_type, "object"));
    ++checks;
}

int main(int argc, char **argv) {
    static const char *languages[] = {"py", "js", "lua", "c", "cpp", "rust", "kt", "cs", "java", "go", "rb"};
    static const char *invalid[] = {
        "object Person = Foreign.new(unknown:Models.Person []):",
        "object Person = Foreign.new(java:Models.Person):",
        "object Person = Foreign.new(java:Models.Person [\"Ada\"]:",
        "Foreign.release():",
        "Foreign.release(Person, Person):",
        "Foreign.release(Person)"
    };
    ZSharpProgram program, decoded;
    ZSharpDiagnostic diagnostic;
    unsigned char identity[ZSHARP_SHA256_SIZE] = {0}, hash[ZSHARP_SHA256_SIZE];
    char source[4096], error[1024];
    size_t index;
    assert(argc == 2);
    for (index = 0; index < sizeof(languages) / sizeof(languages[0]); ++index) {
        snprintf(source, sizeof(source),
            "zsharp = type.script noticed room Main[] ("
            " noticed brain Start[] ("
            " object Person = Foreign.new(%s:Models.Person [\"Ada\",42]):"
            " Print(Person.name): Person.set.name = \"Grace\": Person.name = \"Katherine\":"
            " Person.greet[\"!\"]:"
            " object Returned = Function.call(%s:Models.Person:factory []):"
            " Returned.Child.set.name = \"Nested\": Returned.Child.greet[\"!\"]:"
            " Foreign.release(Person):)"
            " noticed brain Accept[object Person] (Print(Person.name):))", languages[index], languages[index]);
        if (!zsharp_parse_source(source, "Main", &program, &diagnostic)) {
            fprintf(stderr, "%u:%u: %s\n", diagnostic.line, diagnostic.column, diagnostic.message);
            assert(0);
        }
        check(&program, languages[index]);
        assert(zsharp_bytecode_write(argv[1], &program, "foreign_objects", identity, hash, error, sizeof(error)));
        assert(zsharp_bytecode_read(argv[1], &decoded, error, sizeof(error)));
        check(&decoded, languages[index]);
        assert(remove(argv[1]) == 0);
        zsharp_program_free(&decoded);
        zsharp_program_free(&program);
    }
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        snprintf(source, sizeof(source), "zsharp = type.script noticed room Main[] (noticed brain Start[] (%s))", invalid[index]);
        assert(!zsharp_parse_source(source, "Invalid", &program, &diagnostic));
        assert(diagnostic.message[0]);
        zsharp_program_free(&program);
        ++checks;
    }
    puts("foreign object syntax and bytecode: 28 checks passed");
    return 0;
}
