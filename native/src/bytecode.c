#define _CRT_SECURE_NO_WARNINGS

#include "bytecode.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char BYTECODE_MAGIC[4] = {'Z', 'S', 'B', 'C'};
static const uint16_t BYTECODE_MAJOR = 0;
static const uint16_t BYTECODE_MINOR = 17;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

char *zsharp_copy_text(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

void zsharp_program_init(ZSharpProgram *program) {
    memset(program, 0, sizeof(*program));
}

static void free_variable(ZSharpVariable *variable) {
    size_t index;
    size_t argument_index;
    free(variable->name);
    free(variable->number_text);
    free(variable->text_value);
    for (index = 0; index < variable->text_item_count; index++) {
        free(variable->text_items[index]);
    }
    free(variable->text_items);
    for (index = 0; index < variable->number_item_count; index++) {
        free(variable->number_items[index]);
    }
    free(variable->number_items);
    free(variable->array_object_type);
    for (index = 0; index < variable->object_item_count; index++) {
        for (argument_index = 0;
             argument_index <
                 variable->object_items[index].constructor_argument_count;
             argument_index++) {
            free(variable->object_items[index]
                     .constructor_arguments[argument_index].number_text);
            free(variable->object_items[index]
                     .constructor_arguments[argument_index].text_value);
        }
        free(variable->object_items[index].constructor_arguments);
    }
    free(variable->object_items);
    free(variable->object_type);
    for (index = 0; index < variable->constructor_argument_count; index++) {
        free(variable->constructor_arguments[index].number_text);
        free(variable->constructor_arguments[index].text_value);
    }
    free(variable->constructor_arguments);
}

static void free_instruction(ZSharpInstruction *instruction) {
    free(instruction->operand);
    free(instruction->call_file);
    free(instruction->call_room);
    free(instruction->call_function);
    free(instruction->call_outcome);
}

static void free_ui_property(ZSharpUIProperty *property) {
    size_t index;
    free(property->name);
    free(property->text_value);
    for (index = 0; index < property->item_count; index++) {
        free(property->items[index]);
    }
    free(property->items);
}

static void free_window(ZSharpWindow *window) {
    size_t index;
    for (index = 0; index < window->import_count; index++) {
        free(window->imports[index].path);
    }
    free(window->imports);
    for (index = 0; index < window->element_count; index++) {
        ZSharpUIElement *element = &window->elements[index];
        size_t property_index;
        free(element->variant);
        free(element->name);
        for (property_index = 0; property_index < element->property_count;
             property_index++) {
            free_ui_property(&element->properties[property_index]);
        }
        free(element->properties);
    }
    free(window->elements);
    free(window->name);
}

void zsharp_program_free(ZSharpProgram *program) {
    size_t room_index;
    free(program->project_id);
    free(program->source_name);
    free_window(&program->window);
    for (room_index = 0; room_index < program->room_count; room_index++) {
        ZSharpRoom *room = &program->rooms[room_index];
        size_t variable_index;
        size_t function_index;
        size_t import_index;
        free(room->name);
        free(room->parent_name);
        free(room->qualified_name);
        for (import_index = 0; import_index < room->import_count;
             import_index++) {
            free(room->imports[import_index].path);
        }
        free(room->imports);
        for (variable_index = 0; variable_index < room->variable_count;
             variable_index++) {
            free_variable(&room->variables[variable_index]);
        }
        free(room->variables);
        for (function_index = 0; function_index < room->function_count;
             function_index++) {
            ZSharpFunction *function = &room->functions[function_index];
            size_t parameter_index;
            size_t instruction_index;
            free(function->name);
            for (parameter_index = 0;
                 parameter_index < function->parameter_count;
                parameter_index++) {
                free(function->parameters[parameter_index].name);
                free(function->parameters[parameter_index].object_type);
            }
            free(function->parameters);
            for (parameter_index = 0;
                 parameter_index < function->outcome_count;
                 parameter_index++) {
                free(function->outcome_names[parameter_index]);
            }
            free(function->outcome_names);
            for (instruction_index = 0;
                 instruction_index < function->instruction_count;
                 instruction_index++) {
                free_instruction(&function->instructions[instruction_index]);
            }
            free(function->instructions);
        }
        free(room->functions);
    }
    free(program->rooms);
    zsharp_program_init(program);
}

#define ADD_ITEM(owner, field, count, type)                                      \
    do {                                                                         \
        type *resized = (type *)realloc(                                         \
            (owner)->field, ((owner)->count + 1) * sizeof(*(owner)->field));     \
        type *item;                                                              \
        if (resized == NULL) {                                                   \
            return NULL;                                                         \
        }                                                                        \
        (owner)->field = resized;                                                \
        item = &(owner)->field[(owner)->count++];                                \
        memset(item, 0, sizeof(*item));                                          \
        return item;                                                             \
    } while (0)

ZSharpRoom *zsharp_program_add_room(ZSharpProgram *program) {
    ADD_ITEM(program, rooms, room_count, ZSharpRoom);
}

ZSharpVariable *zsharp_room_add_variable(ZSharpRoom *room) {
    ADD_ITEM(room, variables, variable_count, ZSharpVariable);
}

ZSharpImport *zsharp_room_add_import(ZSharpRoom *room) {
    ADD_ITEM(room, imports, import_count, ZSharpImport);
}

ZSharpFunction *zsharp_room_add_function(ZSharpRoom *room) {
    ADD_ITEM(room, functions, function_count, ZSharpFunction);
}

ZSharpParameter *zsharp_function_add_parameter(ZSharpFunction *function) {
    ADD_ITEM(function, parameters, parameter_count, ZSharpParameter);
}

ZSharpInstruction *zsharp_function_add_instruction(ZSharpFunction *function) {
    ADD_ITEM(function, instructions, instruction_count, ZSharpInstruction);
}

ZSharpImport *zsharp_window_add_import(ZSharpWindow *window) {
    ADD_ITEM(window, imports, import_count, ZSharpImport);
}

ZSharpUIElement *zsharp_window_add_element(ZSharpWindow *window) {
    ADD_ITEM(window, elements, element_count, ZSharpUIElement);
}

ZSharpUIProperty *zsharp_ui_element_add_property(ZSharpUIElement *element) {
    ADD_ITEM(element, properties, property_count, ZSharpUIProperty);
}

#undef ADD_ITEM

static int write_bytes(FILE *file, const void *data, size_t size) {
    return fwrite(data, 1, size, file) == size;
}

static int write_u8(FILE *file, uint8_t value) {
    return write_bytes(file, &value, 1);
}

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_string(FILE *file, const char *text) {
    size_t length = text == NULL ? 0 : strlen(text);
    return length <= UINT32_MAX && write_u32(file, (uint32_t)length) &&
           (length == 0 || write_bytes(file, text, length));
}

static int write_instruction(FILE *file,
                             const ZSharpInstruction *instruction) {
    if (!write_u8(file, (uint8_t)instruction->op)) {
        return 0;
    }
    switch (instruction->op) {
        case ZOP_PUSH_NUMBER:
            return write_string(file, instruction->operand);
        case ZOP_PUSH_STATUS:
            return write_u32(file, (uint32_t)instruction->number_operand);
        case ZOP_PUSH_TEXT:
        case ZOP_LOAD_NAME:
        case ZOP_STORE_GLOBAL:
        case ZOP_STORE_LOCAL:
        case ZOP_STORE_LOCAL_TEXT:
        case ZOP_STORE_FIELD:
        case ZOP_GET_MEMBER:
        case ZOP_STORE_NAME:
        case ZOP_SET_MEMBER:
            return write_string(file, instruction->operand);
        case ZOP_LOAD_PATH:
        case ZOP_STORE_PATH:
            return write_string(file, instruction->operand) &&
                   write_u32(file, instruction->argument_count);
        case ZOP_SET_MEMBER_PATH:
            return write_string(file, instruction->operand) &&
                   write_u32(file, instruction->index_operand) &&
                   write_string(file, instruction->call_function);
        case ZOP_CALL_METHOD_PATH:
            return write_string(file, instruction->operand) &&
                   write_u32(file, instruction->index_operand) &&
                   write_string(file, instruction->call_function) &&
                   write_u32(file, instruction->argument_count);
        case ZOP_CALL_LOCAL:
        case ZOP_CALL_METHOD:
        case ZOP_ARRAY_ADD_OBJECT:
            return write_string(file, instruction->operand) &&
                   write_u32(file, instruction->argument_count);
        case ZOP_CALL_QUALIFIED:
        case ZOP_CALL_QUALIFIED_VALUE:
            return write_string(file, instruction->operand) &&
                   write_string(file, instruction->call_file) &&
                   write_string(file, instruction->call_room) &&
                   write_string(file, instruction->call_function) &&
                   write_string(file, instruction->call_outcome) &&
                   write_u32(file, instruction->argument_count);
        case ZOP_JUMP_IF_FALSE:
        case ZOP_JUMP:
            return write_u32(file, instruction->index_operand);
        case ZOP_NAMED_IF_START:
            return write_string(file, instruction->operand) &&
                   write_u32(file, instruction->index_operand);
        case ZOP_UI_SET:
            return write_string(file, instruction->operand) &&
                   write_u32(file, (uint32_t)instruction->number_operand) &&
                   write_u32(file, instruction->index_operand) &&
                   write_string(file, instruction->call_function);
        case ZOP_UI_SET_DYNAMIC:
            return write_u32(file, (uint32_t)instruction->number_operand) &&
                   write_u32(file, instruction->index_operand) &&
                   write_string(file, instruction->call_function);
        case ZOP_DELAY:
            return write_string(file, instruction->operand);
        case ZOP_ADD:
        case ZOP_SUBTRACT:
        case ZOP_MULTIPLY:
        case ZOP_DIVIDE:
        case ZOP_REMAINDER:
        case ZOP_NOT_EQUAL:
        case ZOP_NOT:
        case ZOP_NEGATE:
        case ZOP_SET_INDEX:
        case ZOP_ARRAY_LENGTH:
        case ZOP_GREATER_EQUAL:
        case ZOP_GREATER:
        case ZOP_AND:
        case ZOP_EQUAL:
        case ZOP_LESS:
        case ZOP_LESS_EQUAL:
        case ZOP_OR:
        case ZOP_GET_INDEX:
        case ZOP_PRINT:
        case ZOP_RETURN_VALUE:
        case ZOP_RETURN_VOID:
        case ZOP_RETURN_IF_FALSE:
        case ZOP_PUSH_NULL:
            return 1;
        default:
            return 0;
    }
}

static int write_literal(FILE *file, const ZSharpLiteral *literal) {
    if (!write_u8(file, (uint8_t)literal->type)) return 0;
    if (literal->type == ZVALUE_NUMBER) {
        return write_string(file, literal->number_text);
    }
    if (literal->type == ZVALUE_STATUS) {
        return write_u32(file, (uint32_t)literal->number_value);
    }
    if (literal->type == ZVALUE_TEXT) {
        return write_string(file, literal->text_value);
    }
    return 0;
}

static int write_variable(FILE *file, const ZSharpVariable *variable) {
    size_t item_index;
    int ok = write_u8(file, (uint8_t)variable->is_public) &&
             write_u8(file, (uint8_t)variable->is_horde) &&
             write_u8(file, (uint8_t)variable->type) &&
             write_string(file, variable->name);
    if (ok && variable->type == ZVALUE_NUMBER) {
        return write_string(file, variable->number_text);
    }
    if (ok && variable->type == ZVALUE_TEXT) {
        return write_string(file, variable->text_value);
    }
    if (ok && variable->type == ZVALUE_STATUS) {
        return write_u8(file, (uint8_t)(variable->number_value != 0));
    }
    if (ok && variable->type == ZVALUE_OBJECT) {
        if (!write_string(file, variable->object_type) ||
            variable->constructor_argument_count > UINT32_MAX ||
            !write_u32(file,
                       (uint32_t)variable->constructor_argument_count)) {
            return 0;
        }
        for (item_index = 0;
             item_index < variable->constructor_argument_count; item_index++) {
            if (!write_literal(file,
                               &variable->constructor_arguments[item_index])) {
                return 0;
            }
        }
        return 1;
    }
    if (ok && variable->type == ZVALUE_TEXT_ARRAY) {
        if (variable->text_item_count > UINT32_MAX ||
            !write_u32(file, (uint32_t)variable->text_item_count)) {
            return 0;
        }
        for (item_index = 0; item_index < variable->text_item_count;
             item_index++) {
            if (!write_string(file, variable->text_items[item_index])) {
                return 0;
            }
        }
        return 1;
    }
    if (ok && variable->type == ZVALUE_NUMBER_ARRAY) {
        if (variable->number_item_count > UINT32_MAX ||
            !write_u32(file, (uint32_t)variable->number_item_count)) {
            return 0;
        }
        for (item_index = 0; item_index < variable->number_item_count;
             item_index++) {
            if (!write_string(file, variable->number_items[item_index])) {
                return 0;
            }
        }
        return 1;
    }
    if (!ok || variable->type != ZVALUE_OBJECT_ARRAY ||
        !write_string(file, variable->array_object_type) ||
        variable->object_item_count > UINT32_MAX ||
        !write_u32(file, (uint32_t)variable->object_item_count)) return 0;
    for (item_index = 0; item_index < variable->object_item_count;
         item_index++) {
        const ZSharpObjectArrayItem *item = &variable->object_items[item_index];
        size_t argument_index;
        if (item->constructor_argument_count > UINT32_MAX ||
            !write_u32(file,
                       (uint32_t)item->constructor_argument_count)) return 0;
        for (argument_index = 0;
             argument_index < item->constructor_argument_count;
             argument_index++) {
            if (!write_literal(file,
                               &item->constructor_arguments[argument_index])) {
                return 0;
            }
        }
    }
    return 1;
}

static int write_ui_property(FILE *file,
                             const ZSharpUIProperty *property) {
    size_t index;
    if (!write_string(file, property->name) ||
        !write_u8(file, (uint8_t)property->type)) return 0;
    switch (property->type) {
        case ZUI_PROPERTY_TEXT:
        case ZUI_PROPERTY_COLOR:
        case ZUI_PROPERTY_IDENTIFIER:
        case ZUI_PROPERTY_CALLBACK:
            return write_string(file, property->text_value);
        case ZUI_PROPERTY_STATUS:
            return write_u8(file, (uint8_t)(property->status_value != 0));
        case ZUI_PROPERTY_MEASUREMENT:
            return write_string(file, property->text_value) &&
                   write_u8(file, (uint8_t)property->unit);
        case ZUI_PROPERTY_IDENTIFIER_ARRAY:
            if (property->item_count > UINT32_MAX ||
                !write_u32(file, (uint32_t)property->item_count)) return 0;
            for (index = 0; index < property->item_count; index++) {
                if (!write_string(file, property->items[index])) return 0;
            }
            return 1;
        case ZUI_PROPERTY_EMPTY_ARRAY:
            return 1;
        default:
            return 0;
    }
}

static int write_window(FILE *file, const ZSharpWindow *window) {
    size_t import_index;
    size_t element_index;
    if (!write_u8(file, (uint8_t)(window->is_public != 0)) ||
        !write_string(file, window->name) ||
        window->import_count > UINT32_MAX ||
        !write_u32(file, (uint32_t)window->import_count)) return 0;
    for (import_index = 0; import_index < window->import_count;
         import_index++) {
        if (!write_string(file, window->imports[import_index].path) ||
            !write_u32(file, window->imports[import_index].part_count)) {
            return 0;
        }
    }
    if (window->element_count > UINT32_MAX ||
        !write_u32(file, (uint32_t)window->element_count)) return 0;
    for (element_index = 0; element_index < window->element_count;
         element_index++) {
        const ZSharpUIElement *element = &window->elements[element_index];
        size_t property_index;
        if (!write_u8(file, (uint8_t)(element->is_public != 0)) ||
            !write_u8(file, (uint8_t)element->type) ||
            !write_string(file, element->variant) ||
            !write_string(file, element->name) ||
            element->property_count > UINT32_MAX ||
            !write_u32(file, (uint32_t)element->property_count)) return 0;
        for (property_index = 0; property_index < element->property_count;
             property_index++) {
            if (!write_ui_property(file,
                                   &element->properties[property_index])) {
                return 0;
            }
        }
    }
    return 1;
}

int zsharp_bytecode_write(
    const char *path, const ZSharpProgram *program, const char *project_id,
    unsigned char project_identity[ZSHARP_SHA256_SIZE],
    unsigned char build_hash[ZSHARP_SHA256_SIZE], char *error,
    size_t error_size) {
    FILE *file = fopen(path, "wb");
    unsigned char identity[ZSHARP_SHA256_SIZE];
    unsigned char hash[ZSHARP_SHA256_SIZE];
    unsigned char empty_hash[ZSHARP_SHA256_SIZE] = {0};
    long hash_offset = -1;
    size_t room_index;
    int ok;
    if (file == NULL) {
        set_error(error, error_size, "could not open the bytecode output file");
        return 0;
    }
    zsharp_project_identity(project_id, identity);
    ok = write_bytes(file, BYTECODE_MAGIC, sizeof(BYTECODE_MAGIC)) &&
         write_u16(file, BYTECODE_MAJOR) && write_u16(file, BYTECODE_MINOR) &&
         write_string(file, project_id) &&
         write_bytes(file, identity, sizeof(identity));
    if (ok) hash_offset = ftell(file);
    ok = ok && hash_offset >= 0 &&
         write_bytes(file, empty_hash, sizeof(empty_hash)) &&
         write_string(file, program->source_name) &&
         write_u8(file, (uint8_t)program->script_type) &&
         write_u8(file, (uint8_t)(program->has_window != 0));
    if (ok && program->has_window) {
        ok = write_window(file, &program->window);
    }
    ok = ok &&
         program->room_count <= UINT32_MAX &&
         write_u32(file, (uint32_t)program->room_count);
    for (room_index = 0; ok && room_index < program->room_count; room_index++) {
        const ZSharpRoom *room = &program->rooms[room_index];
        size_t import_index;
        size_t variable_index;
        size_t function_index;
        ok = write_u8(file, (uint8_t)room->visibility) &&
             write_u8(file, (uint8_t)room->is_horde) &&
             write_string(file, room->name) &&
             write_string(file, room->parent_name) &&
             write_string(file, room->qualified_name) &&
             room->import_count <= UINT32_MAX &&
             write_u32(file, (uint32_t)room->import_count);
        for (import_index = 0; ok && import_index < room->import_count;
             import_index++) {
            ok = write_string(file, room->imports[import_index].path) &&
                 write_u32(file, room->imports[import_index].part_count);
        }
        ok = ok &&
             room->variable_count <= UINT32_MAX &&
             write_u32(file, (uint32_t)room->variable_count);
        for (variable_index = 0; ok && variable_index < room->variable_count;
             variable_index++) {
            ok = write_variable(file, &room->variables[variable_index]);
        }
        ok = ok && room->function_count <= UINT32_MAX &&
             write_u32(file, (uint32_t)room->function_count);
        for (function_index = 0;
             ok && function_index < room->function_count; function_index++) {
            const ZSharpFunction *function = &room->functions[function_index];
            size_t parameter_index;
            size_t instruction_index;
            ok = write_u8(file, (uint8_t)function->is_public) &&
                 write_u8(file, (uint8_t)function->is_horde) &&
                 write_u8(file, (uint8_t)function->disable_auto_run) &&
                 write_u8(file, (uint8_t)function->return_type) &&
                 write_string(file, function->name) &&
                 function->parameter_count <= UINT32_MAX &&
                 write_u32(file, (uint32_t)function->parameter_count);
            for (parameter_index = 0;
                 ok && parameter_index < function->parameter_count;
                 parameter_index++) {
                ok = write_u8(file,
                              (uint8_t)function->parameters[parameter_index].type) &&
                     write_string(file,
                                  function->parameters[parameter_index].name);
                if (ok && function->parameters[parameter_index].type ==
                              ZVALUE_OBJECT) {
                    ok = write_string(
                        file,
                        function->parameters[parameter_index].object_type);
                }
            }
            ok = ok && function->outcome_count <= UINT32_MAX &&
                 write_u32(file, (uint32_t)function->outcome_count);
            for (parameter_index = 0;
                 ok && parameter_index < function->outcome_count;
                 parameter_index++) {
                ok = write_string(file,
                                  function->outcome_names[parameter_index]);
            }
            ok = ok && function->instruction_count <= UINT32_MAX &&
                 write_u32(file, (uint32_t)function->instruction_count);
            for (instruction_index = 0;
                 ok && instruction_index < function->instruction_count;
                 instruction_index++) {
                ok = write_instruction(
                    file, &function->instructions[instruction_index]);
            }
        }
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (ok) {
        ok = zsharp_sha256_file_except(path, hash_offset, sizeof(hash), hash);
    }
    if (ok) {
        file = fopen(path, "r+b");
        ok = file != NULL && fseek(file, hash_offset, SEEK_SET) == 0 &&
             write_bytes(file, hash, sizeof(hash));
        if (file != NULL && fclose(file) != 0) ok = 0;
    }
    if (ok) {
        if (project_identity != NULL) {
            memcpy(project_identity, identity, sizeof(identity));
        }
        if (build_hash != NULL) memcpy(build_hash, hash, sizeof(hash));
    }
    if (!ok) {
        set_error(error, error_size, "failed while writing Z# bytecode");
    }
    return ok;
}

static int read_bytes(FILE *file, void *data, size_t size) {
    return fread(data, 1, size, file) == size;
}

static int read_u8(FILE *file, uint8_t *value) {
    return read_bytes(file, value, 1);
}

static int read_u16(FILE *file, uint16_t *value) {
    unsigned char bytes[2];
    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return 0;
    }
    *value = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
    return 1;
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char bytes[4];
    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return 0;
    }
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 1;
}

static int read_string(FILE *file, char **text) {
    uint32_t length = 0;
    char *value;
    if (!read_u32(file, &length) || length > (16u * 1024u * 1024u)) {
        return 0;
    }
    value = (char *)malloc((size_t)length + 1);
    if (value == NULL) {
        return 0;
    }
    if (length > 0 && !read_bytes(file, value, length)) {
        free(value);
        return 0;
    }
    value[length] = '\0';
    *text = value;
    return 1;
}

static int read_instruction(FILE *file, ZSharpInstruction *instruction) {
    uint8_t op = 0;
    uint32_t value = 0;
    if (!read_u8(file, &op)) {
        return 0;
    }
    instruction->op = (ZSharpOpCode)op;
    switch (instruction->op) {
        case ZOP_PUSH_NUMBER:
            return read_string(file, &instruction->operand);
        case ZOP_PUSH_STATUS:
            if (!read_u32(file, &value)) return 0;
            instruction->number_operand = (int32_t)value;
            return 1;
        case ZOP_PUSH_TEXT:
        case ZOP_LOAD_NAME:
        case ZOP_STORE_GLOBAL:
        case ZOP_STORE_LOCAL:
        case ZOP_STORE_LOCAL_TEXT:
        case ZOP_STORE_FIELD:
        case ZOP_GET_MEMBER:
        case ZOP_STORE_NAME:
        case ZOP_SET_MEMBER:
            return read_string(file, &instruction->operand);
        case ZOP_LOAD_PATH:
        case ZOP_STORE_PATH:
            return read_string(file, &instruction->operand) &&
                   read_u32(file, &instruction->argument_count);
        case ZOP_SET_MEMBER_PATH:
            return read_string(file, &instruction->operand) &&
                   read_u32(file, &instruction->index_operand) &&
                   read_string(file, &instruction->call_function);
        case ZOP_CALL_METHOD_PATH:
            return read_string(file, &instruction->operand) &&
                   read_u32(file, &instruction->index_operand) &&
                   read_string(file, &instruction->call_function) &&
                   read_u32(file, &instruction->argument_count);
        case ZOP_CALL_LOCAL:
        case ZOP_CALL_METHOD:
        case ZOP_ARRAY_ADD_OBJECT:
            return read_string(file, &instruction->operand) &&
                   read_u32(file, &instruction->argument_count);
        case ZOP_CALL_QUALIFIED:
        case ZOP_CALL_QUALIFIED_VALUE:
            return read_string(file, &instruction->operand) &&
                   read_string(file, &instruction->call_file) &&
                   read_string(file, &instruction->call_room) &&
                   read_string(file, &instruction->call_function) &&
                   read_string(file, &instruction->call_outcome) &&
                   read_u32(file, &instruction->argument_count);
        case ZOP_JUMP_IF_FALSE:
        case ZOP_JUMP:
            return read_u32(file, &instruction->index_operand);
        case ZOP_NAMED_IF_START:
            return read_string(file, &instruction->operand) &&
                   read_u32(file, &instruction->index_operand);
        case ZOP_UI_SET:
            if (!read_string(file, &instruction->operand) ||
                !read_u32(file, &value)) return 0;
            instruction->number_operand = (int32_t)value;
            return read_u32(file, &instruction->index_operand) &&
                   read_string(file, &instruction->call_function);
        case ZOP_UI_SET_DYNAMIC:
            if (!read_u32(file, &value)) return 0;
            instruction->number_operand = (int32_t)value;
            return read_u32(file, &instruction->index_operand) &&
                   read_string(file, &instruction->call_function);
        case ZOP_DELAY:
            return read_string(file, &instruction->operand);
        case ZOP_ADD:
        case ZOP_SUBTRACT:
        case ZOP_MULTIPLY:
        case ZOP_DIVIDE:
        case ZOP_REMAINDER:
        case ZOP_NOT_EQUAL:
        case ZOP_NOT:
        case ZOP_NEGATE:
        case ZOP_SET_INDEX:
        case ZOP_ARRAY_LENGTH:
        case ZOP_GREATER_EQUAL:
        case ZOP_GREATER:
        case ZOP_AND:
        case ZOP_EQUAL:
        case ZOP_LESS:
        case ZOP_LESS_EQUAL:
        case ZOP_OR:
        case ZOP_GET_INDEX:
        case ZOP_PRINT:
        case ZOP_RETURN_VALUE:
        case ZOP_RETURN_VOID:
        case ZOP_RETURN_IF_FALSE:
        case ZOP_PUSH_NULL:
            return 1;
        default:
            return 0;
    }
}

static int read_literal(FILE *file, ZSharpLiteral *literal) {
    uint8_t type = 0;
    uint32_t value = 0;
    if (!read_u8(file, &type)) return 0;
    literal->type = (ZSharpValueType)type;
    if (literal->type == ZVALUE_NUMBER) {
        return read_string(file, &literal->number_text);
    }
    if (literal->type == ZVALUE_STATUS) {
        if (!read_u32(file, &value)) return 0;
        literal->number_value = (int32_t)value;
        return 1;
    }
    if (literal->type == ZVALUE_TEXT) {
        return read_string(file, &literal->text_value);
    }
    return 0;
}

static int read_variable(FILE *file, ZSharpVariable *variable) {
    uint8_t visibility = 0;
    uint8_t horde = 0;
    uint8_t type = 0;
    uint32_t count = 0;
    uint32_t index;
    if (!read_u8(file, &visibility) || !read_u8(file, &horde) ||
        !read_u8(file, &type) ||
        !read_string(file, &variable->name)) {
        return 0;
    }
    variable->is_public = visibility != 0;
    variable->is_horde = horde != 0;
    variable->type = (ZSharpValueType)type;
    if (variable->type == ZVALUE_NUMBER) {
        return read_string(file, &variable->number_text);
    }
    if (variable->type == ZVALUE_TEXT) {
        return read_string(file, &variable->text_value);
    }
    if (variable->type == ZVALUE_STATUS) {
        uint8_t status = 0;
        if (!read_u8(file, &status) || status > 1) return 0;
        variable->number_value = status;
        return 1;
    }
    if (variable->type == ZVALUE_OBJECT) {
        if (!read_string(file, &variable->object_type) ||
            !read_u32(file, &count) || count > 1000000u) {
            return 0;
        }
        if (count > 0) {
            variable->constructor_arguments = (ZSharpLiteral *)calloc(
                count, sizeof(*variable->constructor_arguments));
            if (variable->constructor_arguments == NULL) return 0;
        }
        variable->constructor_argument_count = count;
        for (index = 0; index < count; index++) {
            if (!read_literal(file,
                              &variable->constructor_arguments[index])) {
                return 0;
            }
        }
        return 1;
    }
    if (variable->type == ZVALUE_OBJECT_ARRAY) {
        if (!read_string(file, &variable->array_object_type) ||
            !read_u32(file, &count) || count > 1000000u) return 0;
        if (count > 0) {
            variable->object_items = (ZSharpObjectArrayItem *)calloc(
                count, sizeof(*variable->object_items));
            if (variable->object_items == NULL) return 0;
        }
        variable->object_item_count = count;
        for (index = 0; index < count; index++) {
            ZSharpObjectArrayItem *item = &variable->object_items[index];
            uint32_t argument_count;
            uint32_t argument_index;
            if (!read_u32(file, &argument_count) ||
                argument_count > 1000000u) return 0;
            if (argument_count > 0) {
                item->constructor_arguments = (ZSharpLiteral *)calloc(
                    argument_count, sizeof(*item->constructor_arguments));
                if (item->constructor_arguments == NULL) return 0;
            }
            item->constructor_argument_count = argument_count;
            for (argument_index = 0; argument_index < argument_count;
                 argument_index++) {
                if (!read_literal(
                        file, &item->constructor_arguments[argument_index])) {
                    return 0;
                }
            }
        }
        return 1;
    }
    if (variable->type != ZVALUE_TEXT_ARRAY &&
        variable->type != ZVALUE_NUMBER_ARRAY) {
        return 0;
    }
    if (!read_u32(file, &count) || count > 1000000u) return 0;
    if (variable->type == ZVALUE_NUMBER_ARRAY) {
        if (count > 0) {
            variable->number_items = (char **)calloc(
                count, sizeof(*variable->number_items));
            if (variable->number_items == NULL) return 0;
        }
        variable->number_item_count = count;
        for (index = 0; index < count; index++) {
            if (!read_string(file, &variable->number_items[index])) return 0;
        }
        return 1;
    }
    if (count > 0) {
        variable->text_items = (char **)calloc(count, sizeof(char *));
        if (variable->text_items == NULL) return 0;
    }
    variable->text_item_count = count;
    for (index = 0; index < count; index++) {
        if (!read_string(file, &variable->text_items[index])) return 0;
    }
    return 1;
}

static int read_ui_property(FILE *file, ZSharpUIProperty *property) {
    uint8_t type = 0;
    uint8_t value = 0;
    uint32_t count = 0;
    uint32_t index;
    if (!read_string(file, &property->name) || !read_u8(file, &type) ||
        type < (uint8_t)ZUI_PROPERTY_TEXT ||
        type > (uint8_t)ZUI_PROPERTY_EMPTY_ARRAY) return 0;
    property->type = (ZSharpUIPropertyType)type;
    switch (property->type) {
        case ZUI_PROPERTY_TEXT:
        case ZUI_PROPERTY_COLOR:
        case ZUI_PROPERTY_IDENTIFIER:
        case ZUI_PROPERTY_CALLBACK:
            return read_string(file, &property->text_value);
        case ZUI_PROPERTY_STATUS:
            if (!read_u8(file, &value) || value > 1) return 0;
            property->status_value = value != 0;
            return 1;
        case ZUI_PROPERTY_MEASUREMENT:
            if (!read_string(file, &property->text_value) ||
                !read_u8(file, &value) ||
                (value != (uint8_t)ZUI_UNIT_ZU &&
                 value != (uint8_t)ZUI_UNIT_PX)) return 0;
            property->unit = (ZSharpUIUnit)value;
            return 1;
        case ZUI_PROPERTY_IDENTIFIER_ARRAY:
            if (!read_u32(file, &count) || count > 1000000u) return 0;
            if (count > 0) {
                property->items =
                    (char **)calloc(count, sizeof(*property->items));
                if (property->items == NULL) return 0;
            }
            property->item_count = count;
            for (index = 0; index < count; index++) {
                if (!read_string(file, &property->items[index])) return 0;
            }
            return 1;
        case ZUI_PROPERTY_EMPTY_ARRAY:
            return 1;
        default:
            return 0;
    }
}

static int read_window(FILE *file, ZSharpWindow *window) {
    uint8_t visibility = 0;
    uint32_t import_count = 0;
    uint32_t import_index;
    uint32_t element_count = 0;
    uint32_t element_index;
    if (!read_u8(file, &visibility) || visibility > 1 ||
        !read_string(file, &window->name) ||
        !read_u32(file, &import_count) || import_count > 100000u) return 0;
    window->is_public = visibility != 0;
    for (import_index = 0; import_index < import_count; import_index++) {
        ZSharpImport *import = zsharp_window_add_import(window);
        if (import == NULL || !read_string(file, &import->path) ||
            !read_u32(file, &import->part_count) ||
            import->part_count < 2 || import->part_count > 64) return 0;
    }
    if (!read_u32(file, &element_count) || element_count > 1000000u) {
        return 0;
    }
    for (element_index = 0; element_index < element_count; element_index++) {
        ZSharpUIElement *element = zsharp_window_add_element(window);
        uint8_t type = 0;
        uint32_t property_count = 0;
        uint32_t property_index;
        if (element == NULL || !read_u8(file, &visibility) ||
            visibility > 1 || !read_u8(file, &type) ||
            type < (uint8_t)ZUI_DESIGN ||
            type > (uint8_t)ZUI_TEXT_INPUT ||
            !read_string(file, &element->variant) ||
            !read_string(file, &element->name) ||
            !read_u32(file, &property_count) ||
            property_count > 1000000u) return 0;
        element->is_public = visibility != 0;
        element->type = (ZSharpUIElementType)type;
        for (property_index = 0; property_index < property_count;
             property_index++) {
            ZSharpUIProperty *property =
                zsharp_ui_element_add_property(element);
            if (property == NULL || !read_ui_property(file, property)) {
                return 0;
            }
        }
    }
    return 1;
}

int zsharp_bytecode_read(const char *path, ZSharpProgram *program,
                         char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    unsigned char magic[4];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint32_t room_count = 0;
    uint32_t room_index;
    unsigned char expected_identity[ZSHARP_SHA256_SIZE];
    unsigned char actual_hash[ZSHARP_SHA256_SIZE];
    long hash_offset = -1;
    int ok;
    zsharp_program_init(program);
    if (file == NULL) {
        set_error(error, error_size, "could not open the bytecode file");
        return 0;
    }
    ok = read_bytes(file, magic, sizeof(magic)) &&
         memcmp(magic, BYTECODE_MAGIC, sizeof(magic)) == 0 &&
         read_u16(file, &major) && read_u16(file, &minor) &&
         major == BYTECODE_MAJOR &&
         (minor == 14 || minor == 15 || minor == 16 ||
          minor == BYTECODE_MINOR) &&
         read_string(file, &program->project_id) &&
         read_bytes(file, program->project_identity,
                    sizeof(program->project_identity));
    if (ok) hash_offset = ftell(file);
    ok = ok && hash_offset >= 0 &&
         read_bytes(file, program->build_hash, sizeof(program->build_hash));
    if (!ok) {
        fclose(file);
        set_error(error, error_size,
                  "invalid, unsupported, or truncated Z# bytecode");
        zsharp_program_free(program);
        return 0;
    }
    zsharp_project_identity(program->project_id, expected_identity);
    if (memcmp(program->project_identity, expected_identity,
               sizeof(expected_identity)) != 0) {
        fclose(file);
        set_error(error, error_size, "invalid Z# project identity");
        zsharp_program_free(program);
        return 0;
    }
    if (!zsharp_sha256_file_except(path, hash_offset,
                                   sizeof(program->build_hash), actual_hash) ||
        memcmp(program->build_hash, actual_hash, sizeof(actual_hash)) != 0) {
        fclose(file);
        set_error(error, error_size,
                  "Z# bytecode integrity check failed; the file was changed");
        zsharp_program_free(program);
        return 0;
    }
    ok = read_string(file, &program->source_name);
    if (ok && minor >= 15) {
        uint8_t script_type = 0;
        uint8_t has_window = 0;
        ok = read_u8(file, &script_type) &&
             script_type <= (uint8_t)ZSCRIPT_3D &&
             read_u8(file, &has_window) && has_window <= 1;
        if (ok) {
            program->script_type = (ZSharpScriptType)script_type;
            program->has_window = has_window != 0;
        }
        if (ok && program->has_window) {
            ok = program->script_type == ZSCRIPT_WINDOW &&
                 read_window(file, &program->window);
        }
    }
    ok = ok && read_u32(file, &room_count) && room_count <= 100000u;
    for (room_index = 0; ok && room_index < room_count; room_index++) {
        ZSharpRoom *room = zsharp_program_add_room(program);
        uint8_t visibility = 0;
        uint8_t horde_room = 0;
        uint32_t import_count = 0;
        uint32_t import_index;
        uint32_t variable_count = 0;
        uint32_t variable_index;
        uint32_t function_count = 0;
        uint32_t function_index;
        ok = room != NULL && read_u8(file, &visibility) &&
             read_u8(file, &horde_room) &&
             visibility <= (uint8_t)ZVISIBILITY_FILE &&
             read_string(file, &room->name) &&
             read_string(file, &room->parent_name) &&
             read_string(file, &room->qualified_name) &&
             read_u32(file, &import_count) && import_count <= 100000u;
        for (import_index = 0; ok && import_index < import_count;
             import_index++) {
            ZSharpImport *import = zsharp_room_add_import(room);
            ok = import != NULL && read_string(file, &import->path) &&
                 read_u32(file, &import->part_count) &&
                 import->part_count >= 2 && import->part_count <= 64;
        }
        ok = ok &&
             read_u32(file, &variable_count) && variable_count <= 1000000u;
        if (room != NULL) {
            room->visibility = (ZSharpVisibility)visibility;
            room->is_horde = horde_room != 0;
        }
        for (variable_index = 0;
             ok && variable_index < variable_count; variable_index++) {
            ZSharpVariable *variable = zsharp_room_add_variable(room);
            ok = variable != NULL && read_variable(file, variable);
        }
        if (ok) {
            ok = read_u32(file, &function_count) &&
                 function_count <= 1000000u;
        }
        for (function_index = 0;
             ok && function_index < function_count; function_index++) {
            ZSharpFunction *function = zsharp_room_add_function(room);
            uint8_t disabled = 0;
            uint8_t horde = 0;
            uint8_t return_type = 0;
            uint32_t parameter_count = 0;
            uint32_t parameter_index;
            uint32_t outcome_count = 0;
            uint32_t outcome_index;
            uint32_t instruction_count = 0;
            uint32_t instruction_index;
            ok = function != NULL && read_u8(file, &visibility) &&
                 read_u8(file, &horde) &&
                 read_u8(file, &disabled) && read_u8(file, &return_type) &&
                 read_string(file, &function->name) &&
                 read_u32(file, &parameter_count) &&
                 parameter_count <= 1000000u;
            if (function != NULL) {
                function->is_public = visibility != 0;
                function->is_horde = horde != 0;
                function->disable_auto_run = disabled != 0;
                function->return_type = (ZSharpReturnType)return_type;
            }
            for (parameter_index = 0;
                 ok && parameter_index < parameter_count; parameter_index++) {
                ZSharpParameter *parameter =
                    zsharp_function_add_parameter(function);
                uint8_t type = 0;
                ok = parameter != NULL && read_u8(file, &type) &&
                     read_string(file, &parameter->name);
                if (parameter != NULL) parameter->type = (ZSharpValueType)type;
                if (ok && parameter->type == ZVALUE_OBJECT) {
                    ok = read_string(file, &parameter->object_type);
                }
            }
            if (ok) {
                ok = read_u32(file, &outcome_count) &&
                     outcome_count <= 1000000u;
            }
            if (ok && outcome_count > 0) {
                function->outcome_names = (char **)calloc(
                    outcome_count, sizeof(*function->outcome_names));
                ok = function->outcome_names != NULL;
            }
            if (function != NULL) function->outcome_count = outcome_count;
            for (outcome_index = 0;
                 ok && outcome_index < outcome_count; outcome_index++) {
                ok = read_string(file,
                                 &function->outcome_names[outcome_index]);
            }
            if (ok) {
                ok = read_u32(file, &instruction_count) &&
                     instruction_count <= 10000000u;
            }
            for (instruction_index = 0;
                 ok && instruction_index < instruction_count;
                 instruction_index++) {
                ZSharpInstruction *instruction =
                    zsharp_function_add_instruction(function);
                ok = instruction != NULL && read_instruction(file, instruction);
            }
        }
    }
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        set_error(error, error_size,
                  "invalid, unsupported, or truncated Z# bytecode");
        zsharp_program_free(program);
    }
    return ok;
}
