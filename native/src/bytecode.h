#ifndef ZSHARP_BYTECODE_H
#define ZSHARP_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

#include "hash.h"

typedef enum ZSharpValueType {
    ZVALUE_NUMBER = 1,
    ZVALUE_TEXT = 2,
    ZVALUE_TEXT_ARRAY = 3,
    ZVALUE_STATUS = 4,
    ZVALUE_OBJECT = 5,
    ZVALUE_NUMBER_ARRAY = 6,
    ZVALUE_OBJECT_ARRAY = 7,
    ZVALUE_NULL = 8
} ZSharpValueType;

typedef enum ZSharpVisibility {
    ZVISIBILITY_SILENT = 0,
    ZVISIBILITY_NOTICED = 1,
    ZVISIBILITY_FILE = 2
} ZSharpVisibility;

typedef struct ZSharpLiteral {
    ZSharpValueType type;
    int32_t number_value;
    char *number_text;
    char *text_value;
} ZSharpLiteral;

typedef struct ZSharpObjectArrayItem {
    ZSharpLiteral *constructor_arguments;
    size_t constructor_argument_count;
    void *runtime_object;
} ZSharpObjectArrayItem;

typedef struct ZSharpVariable {
    int is_public;
    int is_horde;
    ZSharpValueType type;
    char *name;
    int32_t number_value;
    char *number_text;
    char *text_value;
    char **text_items;
    size_t text_item_count;
    char **number_items;
    size_t number_item_count;
    char *array_object_type;
    ZSharpObjectArrayItem *object_items;
    size_t object_item_count;
    char *object_type;
    ZSharpLiteral *constructor_arguments;
    size_t constructor_argument_count;
    void *runtime_object;
} ZSharpVariable;

typedef enum ZSharpReturnType {
    ZRETURN_VOID = 0,
    ZRETURN_NUMBER = 1,
    ZRETURN_TEXT = 2
} ZSharpReturnType;

typedef struct ZSharpParameter {
    ZSharpValueType type;
    char *name;
    char *object_type;
} ZSharpParameter;

typedef struct ZSharpImport {
    char *path;
    uint32_t part_count;
} ZSharpImport;

typedef enum ZSharpScriptType {
    ZSCRIPT_NORMAL = 0,
    ZSCRIPT_WINDOW = 1,
    ZSCRIPT_2D = 2,
    ZSCRIPT_3D = 3
} ZSharpScriptType;

typedef enum ZSharpUIElementType {
    ZUI_DESIGN = 1,
    ZUI_TEXT = 2,
    ZUI_BUTTON = 3,
    ZUI_IMAGE = 4,
    ZUI_TEXT_INPUT = 5
} ZSharpUIElementType;

typedef enum ZSharpUIPropertyType {
    ZUI_PROPERTY_TEXT = 1,
    ZUI_PROPERTY_STATUS = 2,
    ZUI_PROPERTY_COLOR = 3,
    ZUI_PROPERTY_MEASUREMENT = 4,
    ZUI_PROPERTY_IDENTIFIER = 5,
    ZUI_PROPERTY_IDENTIFIER_ARRAY = 6,
    ZUI_PROPERTY_CALLBACK = 7,
    ZUI_PROPERTY_EMPTY_ARRAY = 8
} ZSharpUIPropertyType;

typedef enum ZSharpUIUnit {
    ZUI_UNIT_NONE = 0,
    ZUI_UNIT_ZU = 1,
    ZUI_UNIT_PX = 2
} ZSharpUIUnit;

typedef struct ZSharpUIProperty {
    char *name;
    ZSharpUIPropertyType type;
    char *text_value;
    int status_value;
    ZSharpUIUnit unit;
    char **items;
    size_t item_count;
} ZSharpUIProperty;

typedef struct ZSharpUIElement {
    int is_public;
    ZSharpUIElementType type;
    char *variant;
    char *name;
    ZSharpUIProperty *properties;
    size_t property_count;
} ZSharpUIElement;

typedef struct ZSharpWindow {
    int is_public;
    char *name;
    ZSharpImport *imports;
    size_t import_count;
    ZSharpUIElement *elements;
    size_t element_count;
} ZSharpWindow;

typedef enum ZSharpOpCode {
    ZOP_PUSH_NUMBER = 1,
    ZOP_PUSH_TEXT = 2,
    ZOP_LOAD_NAME = 3,
    ZOP_STORE_GLOBAL = 4,
    ZOP_STORE_LOCAL = 5,
    ZOP_ADD = 6,
    ZOP_GREATER_EQUAL = 7,
    ZOP_GET_INDEX = 8,
    ZOP_PRINT = 9,
    ZOP_CALL_QUALIFIED = 10,
    ZOP_CALL_LOCAL = 11,
    ZOP_RETURN_VALUE = 12,
    ZOP_JUMP_IF_FALSE = 13,
    ZOP_JUMP = 14,
    ZOP_PUSH_STATUS = 15,
    ZOP_GREATER = 16,
    ZOP_AND = 17,
    ZOP_STORE_FIELD = 18,
    ZOP_GET_MEMBER = 19,
    ZOP_CALL_METHOD = 20,
    ZOP_STORE_NAME = 21,
    ZOP_SET_MEMBER = 22,
    ZOP_EQUAL = 23,
    ZOP_LESS = 24,
    ZOP_LESS_EQUAL = 25,
    ZOP_OR = 26,
    ZOP_RETURN_VOID = 27,
    ZOP_RETURN_IF_FALSE = 28,
    ZOP_LOAD_PATH = 29,
    ZOP_STORE_PATH = 30,
    ZOP_SET_MEMBER_PATH = 31,
    ZOP_CALL_METHOD_PATH = 32,
    ZOP_SUBTRACT = 33,
    ZOP_MULTIPLY = 34,
    ZOP_DIVIDE = 35,
    ZOP_REMAINDER = 36,
    ZOP_NOT_EQUAL = 37,
    ZOP_NOT = 38,
    ZOP_NEGATE = 39,
    ZOP_CALL_QUALIFIED_VALUE = 40,
    ZOP_STORE_LOCAL_TEXT = 41,
    ZOP_SET_INDEX = 42,
    ZOP_ARRAY_LENGTH = 43,
    ZOP_ARRAY_ADD_OBJECT = 44,
    ZOP_NAMED_IF_START = 45,
    ZOP_PUSH_NULL = 46,
    ZOP_UI_SET = 47,
    ZOP_DELAY = 48,
    ZOP_UI_SET_DYNAMIC = 49
} ZSharpOpCode;

typedef struct ZSharpInstruction {
    ZSharpOpCode op;
    int32_t number_operand;
    uint32_t index_operand;
    uint32_t argument_count;
    char *operand;
    char *call_file;
    char *call_room;
    char *call_function;
    char *call_outcome;
} ZSharpInstruction;

typedef struct ZSharpFunction {
    int is_public;
    int is_horde;
    int disable_auto_run;
    ZSharpReturnType return_type;
    char *name;
    ZSharpParameter *parameters;
    size_t parameter_count;
    char **outcome_names;
    size_t outcome_count;
    ZSharpInstruction *instructions;
    size_t instruction_count;
} ZSharpFunction;

typedef struct ZSharpRoom {
    ZSharpVisibility visibility;
    int is_horde;
    char *name;
    char *parent_name;
    char *qualified_name;
    ZSharpImport *imports;
    size_t import_count;
    ZSharpVariable *variables;
    size_t variable_count;
    ZSharpFunction *functions;
    size_t function_count;
} ZSharpRoom;

typedef struct ZSharpProgram {
    char *project_id;
    unsigned char project_identity[ZSHARP_SHA256_SIZE];
    unsigned char build_hash[ZSHARP_SHA256_SIZE];
    char *source_name;
    ZSharpScriptType script_type;
    int has_window;
    ZSharpWindow window;
    ZSharpRoom *rooms;
    size_t room_count;
} ZSharpProgram;

void zsharp_program_init(ZSharpProgram *program);
void zsharp_program_free(ZSharpProgram *program);
char *zsharp_copy_text(const char *text, size_t length);

ZSharpRoom *zsharp_program_add_room(ZSharpProgram *program);
ZSharpImport *zsharp_room_add_import(ZSharpRoom *room);
ZSharpVariable *zsharp_room_add_variable(ZSharpRoom *room);
ZSharpFunction *zsharp_room_add_function(ZSharpRoom *room);
ZSharpParameter *zsharp_function_add_parameter(ZSharpFunction *function);
ZSharpInstruction *zsharp_function_add_instruction(ZSharpFunction *function);
ZSharpImport *zsharp_window_add_import(ZSharpWindow *window);
ZSharpUIElement *zsharp_window_add_element(ZSharpWindow *window);
ZSharpUIProperty *zsharp_ui_element_add_property(ZSharpUIElement *element);

int zsharp_bytecode_write(
    const char *path, const ZSharpProgram *program, const char *project_id,
    unsigned char project_identity[ZSHARP_SHA256_SIZE],
    unsigned char build_hash[ZSHARP_SHA256_SIZE], char *error,
    size_t error_size);
int zsharp_bytecode_read(const char *path, ZSharpProgram *program,
                         char *error, size_t error_size);

#endif
