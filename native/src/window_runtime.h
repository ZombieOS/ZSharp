#ifndef ZSHARP_WINDOW_RUNTIME_H
#define ZSHARP_WINDOW_RUNTIME_H

#include "window.h"

int zsharp_window_model_set(ZSharpProgram *program, const char *path,
                            ZSharpWindowValueType value_type,
                            const char *text_value, ZSharpUIUnit unit,
                            ZSharpUIElement **element_output,
                            ZSharpUIProperty **property_output,
                            char *error, size_t error_size);

typedef enum ZSharpWindowInputField {
    ZWINDOW_INPUT_CONTENTS = 1,
    ZWINDOW_INPUT_TOTAL_CHARACTERS = 2,
    ZWINDOW_INPUT_CURRENT_COLUMN = 3,
    ZWINDOW_INPUT_TOTAL_LINES = 4,
    ZWINDOW_INPUT_CURRENT_LINE = 5
} ZSharpWindowInputField;

int zsharp_window_model_resolve_input(
    ZSharpProgram *program, const char *path, ZSharpUIElement **element_output,
    ZSharpWindowInputField *field_output, char *error, size_t error_size);

void zsharp_window_text_metrics(const char *text, size_t cursor_byte_offset,
                                size_t *total_characters,
                                size_t *total_lines, size_t *current_line,
                                size_t *current_column);

size_t zsharp_window_utf8_byte_offset(const char *text,
                                      size_t character_offset);

char *zsharp_window_copy_size(size_t value);

#endif
