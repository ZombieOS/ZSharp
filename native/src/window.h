#ifndef ZSHARP_WINDOW_H
#define ZSHARP_WINDOW_H

#include "bytecode.h"

#include <stddef.h>

typedef enum ZSharpWindowValueType {
    ZWINDOW_VALUE_TEXT = ZUI_PROPERTY_TEXT,
    ZWINDOW_VALUE_STATUS = ZUI_PROPERTY_STATUS,
    ZWINDOW_VALUE_COLOR = ZUI_PROPERTY_COLOR,
    ZWINDOW_VALUE_MEASUREMENT = ZUI_PROPERTY_MEASUREMENT,
    ZWINDOW_VALUE_IDENTIFIER = ZUI_PROPERTY_IDENTIFIER
} ZSharpWindowValueType;

typedef struct ZSharpWindowRuntime {
    void *state;
    int (*set_property)(void *state, const char *path,
                        ZSharpWindowValueType value_type,
                        const char *text_value, ZSharpUIUnit unit,
                        char *error, size_t error_size);
    int (*wait)(void *state, const char *milliseconds,
                char *error, size_t error_size);
    int (*is_cancelled)(void *state);
} ZSharpWindowRuntime;

typedef int (*ZSharpWindowCallback)(void *user_data, const char *target,
                                    const ZSharpWindowRuntime *runtime,
                                    char *error, size_t error_size);

#define ZSHARP_WINDOW_PROJECT_STARTS "@zsharp-project-starts"
#define ZSHARP_WINDOW_TASKS_STOP "@zsharp-tasks-stop"

int zsharp_window_run(ZSharpProgram *program, const char *project_root,
                      ZSharpWindowCallback callback, void *user_data,
                      char *error, size_t error_size);

int zsharp_window_show_hub(const char *headline, const char *reason,
                           char *error, size_t error_size);

#endif
