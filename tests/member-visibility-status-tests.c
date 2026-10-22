#define _CRT_SECURE_NO_WARNINGS
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char source[] =
    "zsharp = type.script\n"
    "noticed room Public[] (\n"
    " number DefaultNumber = 1: text DefaultText = \"public\":\n"
    " status DefaultStatus = alive: text() DefaultArray = [\"one\"]:\n"
    " number() DefaultNumbers = [1, 2]:\n"
    " JSON DefaultSchema[] (name: text:)\n"
    " silent number Hidden = 2: noticed number Exposed = 3:\n"
    " brain DefaultBrain[] () silent brain HiddenBrain[] ()\n"
    " noticed brain ExposedBrain[] ()\n"
    " number DefaultNumberFunction[number N] (feed(N):)\n"
    " text DefaultTextFunction[text T] (feed(T):)\n"
    ")\n"
    "room FileOnly[] (\n"
    " number DefaultNumber = 4: text DefaultText = \"file\":\n"
    " status DefaultStatus = dead: text() DefaultArray = [\"two\"]:\n"
    " number() DefaultNumbers = [3, 4]:\n"
    " JSON DefaultSchema[] (name: text:)\n"
    " silent number Hidden = 5: noticed number Exposed = 6:\n"
    " brain DefaultBrain[] () silent brain HiddenBrain[] ()\n"
    " noticed brain ExposedBrain[] ()\n"
    ")\n"
    "silent room Private[] (\n"
    " number DefaultNumber = 7: text DefaultText = \"private\":\n"
    " status DefaultStatus = alive: text() DefaultArray = [\"three\"]:\n"
    " number() DefaultNumbers = [5, 6]:\n"
    " JSON DefaultSchema[] (name: text:)\n"
    " silent number Hidden = 8: noticed number Exposed = 9:\n"
    " brain Start[] (\n"
    "  status Enabled = alive: status Disabled = dead:\n"
    "  status Calculated = 2 < 3 and not Disabled:\n"
    "  Enabled = dead: if[Enabled == dead and Calculated] (Print(\"status\"):)\n"
    " )\n"
    " brain DefaultBrain[] () silent brain HiddenBrain[] ()\n"
    " noticed brain ExposedBrain[] ()\n"
    ")\n";

static void check_program(const ZSharpProgram *program) {
    size_t r, index;
    unsigned status_stores = 0;
    assert(program->room_count == 3);
    assert(program->rooms[0].visibility == ZVISIBILITY_NOTICED);
    assert(program->rooms[1].visibility == ZVISIBILITY_FILE);
    assert(program->rooms[2].visibility == ZVISIBILITY_SILENT);
    for (r = 0; r < program->room_count; r++) {
        const ZSharpRoom *room = &program->rooms[r];
        int inherited = r != 2;
        assert(room->variable_count == 7);
        for (index = 0; index < room->variable_count; index++) {
            const ZSharpVariable *variable = &room->variables[index];
            int expected = !strcmp(variable->name, "Hidden") ? 0 :
                           !strcmp(variable->name, "Exposed") ? 1 : inherited;
            assert(variable->is_public == expected);
        }
        assert(room->json_schema_count == 1);
        assert(room->json_schemas[0].is_public == inherited);
        for (index = 0; index < room->function_count; index++) {
            const ZSharpFunction *function = &room->functions[index];
            size_t instruction;
            int expected = !strcmp(function->name, "HiddenBrain") ? 0 :
                           !strcmp(function->name, "ExposedBrain") ? 1 : inherited;
            assert(function->is_public == expected);
            for (instruction = 0; instruction < function->instruction_count;
                 instruction++) {
                const ZSharpInstruction *in = &function->instructions[instruction];
                if (in->op == ZOP_STORE_LOCAL_STATUS) {
                    assert(in->operand != NULL && *in->operand != '\0');
                    status_stores++;
                }
            }
        }
    }
    assert(status_stores == 3);
    assert(ZOP_STORE_LOCAL_STATUS == 72);
}

int main(int argc, char **argv) {
    ZSharpProgram program, decoded;
    ZSharpDiagnostic diagnostic = {0};
    unsigned char identity[ZSHARP_SHA256_SIZE], hash[ZSHARP_SHA256_SIZE];
    char error[512] = {0};
    FILE *existing;
    assert(argc == 2);
    existing = fopen(argv[1], "rb");
    if (existing != NULL) {
        fclose(existing);
        fprintf(stderr, "Refusing to overwrite existing test file: %s\n", argv[1]);
        return 1;
    }
    if (!zsharp_parse_source(source, "Visibility", &program, &diagnostic)) {
        fprintf(stderr, "%u:%u: %s\n", diagnostic.line, diagnostic.column,
                diagnostic.message);
        return 1;
    }
    check_program(&program);
    assert(zsharp_bytecode_write(argv[1], &program, "visibility_status_test",
                                 identity, hash, error, sizeof(error)));
    assert(zsharp_bytecode_read(argv[1], &decoded, error, sizeof(error)));
    assert(remove(argv[1]) == 0);
    check_program(&decoded);
    zsharp_program_free(&decoded);
    zsharp_program_free(&program);

    assert(!zsharp_parse_source("zsharp = type.script room Main[] ("
        " brain Start[] (status Missing:))", "Invalid", &program, &diagnostic));
    assert(strstr(diagnostic.message, "'='") != NULL);
    zsharp_program_free(&program);
    puts("PASS: inherited public/file/private members, explicit overrides, local status opcode and bytecode round trip");
    return 0;
}
