#ifndef ZSHARP_WINDOW_RUNTIME_H
#define ZSHARP_WINDOW_RUNTIME_H

#include "window.h"

int zsharp_window_model_set(ZSharpProgram *program, const char *path,
                            ZSharpWindowValueType value_type,
                            const char *text_value, ZSharpUIUnit unit,
                            ZSharpUIElement **element_output,
                            ZSharpUIProperty **property_output,
                            char *error, size_t error_size);

#endif
