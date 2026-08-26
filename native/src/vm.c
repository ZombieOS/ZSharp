#include "vm.h"

#include "decimal.h"
#include "project.h"
#include "window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#define ZSHARP_MAX_CALL_DEPTH 256

typedef struct RuntimeValue {
    ZSharpValueType type;
    int32_t number;
    const char *number_text;
    const char *text;
    ZSharpVariable *array;
    struct RuntimeObject *object;
} RuntimeValue;

typedef struct LocalValue {
    const char *name;
    RuntimeValue value;
} LocalValue;

typedef struct RuntimeField {
    const char *name;
    ZSharpValueType type;
    ZSharpVariable *definition;
    RuntimeValue value;
} RuntimeField;

typedef struct RuntimeObject {
    ZSharpRoom *room;
    RuntimeField *fields;
    size_t field_count;
} RuntimeObject;

typedef struct RuntimeHeap {
    char **texts;
    size_t text_count;
} RuntimeHeap;

typedef struct RuntimeModule {
    char *source_path;
    ZSharpProgram *program;
    int objects_initialized;
} RuntimeModule;

typedef struct RuntimeModuleCache {
    RuntimeModule *modules;
    size_t module_count;
    const ZSharpWindowRuntime *window_runtime;
} RuntimeModuleCache;

static int runtime_wait(const char *milliseconds, char *error,
                        size_t error_size) {
    char *end = NULL;
    double value = strtod(milliseconds == NULL ? "" : milliseconds, &end);
    if (end == milliseconds || end == NULL || *end != '\0' ||
        !isfinite(value) || value < 0.0 || value > 604800000.0) {
        snprintf(error, error_size,
                 "wait/delay must be between 0ms and 604800000ms");
        return 0;
    }
#ifdef _WIN32
    {
        unsigned long duration = (unsigned long)(value + 0.5);
        Sleep(duration);
    }
#else
    {
        struct timespec duration;
        duration.tv_sec = (time_t)(value / 1000.0);
        duration.tv_nsec = (long)((value -
            (double)duration.tv_sec * 1000.0) * 1000000.0 + 0.5);
        if (duration.tv_nsec >= 1000000000L) {
            duration.tv_sec++;
            duration.tv_nsec -= 1000000000L;
        }
        while (nanosleep(&duration, &duration) != 0) {
            if (errno != EINTR) {
                snprintf(error, error_size, "wait/delay failed");
                return 0;
            }
        }
    }
#endif
    return 1;
}

static RuntimeObject *create_object_from_values(
    ZSharpProgram *program, const char *object_type,
    const RuntimeValue *arguments, size_t argument_count, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, RuntimeModuleCache *module_cache, unsigned depth,
    char *error, size_t error_size);

static char *heap_add_text(RuntimeHeap *heap, char *text) {
    char **resized = (char **)realloc(
        heap->texts, (heap->text_count + 1) * sizeof(*heap->texts));
    if (resized == NULL) {
        free(text);
        return NULL;
    }
    heap->texts = resized;
    heap->texts[heap->text_count++] = text;
    return text;
}

static int coerce_number_to_text(RuntimeHeap *heap, RuntimeValue *value,
                                 char *error, size_t error_size) {
    char *copy;
    if (value->type != ZVALUE_NUMBER) return 1;
    copy = zsharp_copy_text(value->number_text, strlen(value->number_text));
    if (copy == NULL || heap_add_text(heap, copy) == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    value->type = ZVALUE_TEXT;
    value->text = copy;
    return 1;
}

static void heap_free(RuntimeHeap *heap) {
    size_t index;
    for (index = 0; index < heap->text_count; index++) {
        free(heap->texts[index]);
    }
    free(heap->texts);
}

static ZSharpRoom *find_room(ZSharpProgram *program, const char *name) {
    size_t index;
    for (index = 0; index < program->room_count; index++) {
        if ((program->rooms[index].qualified_name != NULL &&
             strcmp(program->rooms[index].qualified_name, name) == 0) ||
            strcmp(program->rooms[index].name, name) == 0) {
            return &program->rooms[index];
        }
    }
    return NULL;
}

static int room_is_visible(const ZSharpRoom *target,
                           const ZSharpRoom *caller, int cross_file) {
    if (target == caller) return 1;
    if (cross_file) return target->visibility == ZVISIBILITY_NOTICED;
    if (target->visibility != ZVISIBILITY_SILENT) return 1;
    return target->parent_name != NULL && target->parent_name[0] != '\0' &&
           caller != NULL && caller->qualified_name != NULL &&
           strcmp(target->parent_name, caller->qualified_name) == 0;
}

static ZSharpFunction *find_function(ZSharpRoom *room, const char *name) {
    size_t index;
    for (index = 0; index < room->function_count; index++) {
        if (strcmp(room->functions[index].name, name) == 0) {
            return &room->functions[index];
        }
    }
    return NULL;
}

static ZSharpVariable *find_variable(ZSharpRoom *room, const char *name) {
    size_t index;
    for (index = 0; index < room->variable_count; index++) {
        if (strcmp(room->variables[index].name, name) == 0) {
            return &room->variables[index];
        }
    }
    return NULL;
}

static RuntimeValue variable_value(ZSharpVariable *variable) {
    RuntimeValue value;
    memset(&value, 0, sizeof(value));
    value.type = variable->type;
    if (variable->type == ZVALUE_NUMBER) {
        value.number_text = variable->number_text;
    } else if (variable->type == ZVALUE_TEXT) {
        value.text = variable->text_value;
    } else if (variable->type == ZVALUE_TEXT_ARRAY) {
        value.array = variable;
    } else if (variable->type == ZVALUE_NUMBER_ARRAY) {
        value.array = variable;
    } else if (variable->type == ZVALUE_OBJECT_ARRAY) {
        value.array = variable;
    } else if (variable->type == ZVALUE_STATUS) {
        value.number = variable->number_value;
    } else if (variable->type == ZVALUE_OBJECT) {
        value.object = (RuntimeObject *)variable->runtime_object;
    }
    return value;
}

static int push(RuntimeValue *stack, size_t capacity, size_t *count,
                RuntimeValue value, char *error, size_t error_size) {
    if (*count >= capacity) {
        snprintf(error, error_size, "expression stack overflow");
        return 0;
    }
    stack[(*count)++] = value;
    return 1;
}

static int pop(RuntimeValue *stack, size_t *count, RuntimeValue *value,
               char *error, size_t error_size) {
    if (*count == 0) {
        snprintf(error, error_size, "invalid bytecode: expression stack underflow");
        return 0;
    }
    *value = stack[--(*count)];
    return 1;
}

static RuntimeField *find_field(RuntimeObject *object, const char *name) {
    size_t index;
    if (object == NULL) return NULL;
    for (index = 0; index < object->field_count; index++) {
        if (strcmp(object->fields[index].name, name) == 0) {
            return &object->fields[index];
        }
    }
    return NULL;
}

static RuntimeValue runtime_field_value(const RuntimeField *field) {
    if (field->definition != NULL && field->definition->is_horde) {
        return variable_value(field->definition);
    }
    return field->value;
}

static int runtime_field_assign(RuntimeField *field,
                                const RuntimeValue *value, char *error,
                                size_t error_size) {
    if (field->type != value->type) {
        fprintf(stderr,
                "runtime warning: assignment to '%s' has the wrong type; "
                "the assignment was skipped\n",
                field->name);
        return 1;
    }
    if (field->definition == NULL || !field->definition->is_horde) {
        field->value = *value;
        return 1;
    }
    if (value->type == ZVALUE_NUMBER) {
        char *copy = zsharp_copy_text(value->number_text,
                                      strlen(value->number_text));
        if (copy == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        free(field->definition->number_text);
        field->definition->number_text = copy;
        return 1;
    }
    if (value->type == ZVALUE_STATUS) {
        field->definition->number_value = value->number;
        return 1;
    }
    if (value->type == ZVALUE_TEXT) {
        char *copy = zsharp_copy_text(value->text, strlen(value->text));
        if (copy == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        free(field->definition->text_value);
        field->definition->text_value = copy;
        return 1;
    }
    snprintf(error, error_size,
             "horde assignment to '%s' is not defined for this type",
             field->name);
    return 0;
}

static int lookup_name(ZSharpRoom *room, RuntimeObject *current_object,
                       LocalValue *locals, size_t local_count, const char *name,
                       RuntimeValue *value) {
    size_t index = local_count;
    ZSharpVariable *variable;
    while (index > 0) {
        index--;
        if (strcmp(locals[index].name, name) == 0) {
            *value = locals[index].value;
            return 1;
        }
    }
    {
        RuntimeField *field = find_field(current_object, name);
        if (field != NULL) {
            *value = runtime_field_value(field);
            return 1;
        }
    }
    variable = find_variable(room, name);
    if (variable == NULL) return 0;
    *value = variable_value(variable);
    return 1;
}

static const ZSharpProviderBinding *find_provider(
    const ZSharpProviderBinding *providers, size_t provider_count,
    const char *project_name) {
    size_t index;
    for (index = 0; index < provider_count; index++) {
        if (strcmp(providers[index].project_name, project_name) == 0) {
            return &providers[index];
        }
    }
    return NULL;
}

static int copy_text_value(RuntimeHeap *heap, const char *text,
                           RuntimeValue *value, char *error,
                           size_t error_size) {
    size_t length;
    char *copy;
    if (text == NULL) {
        snprintf(error, error_size, "a text provider returned no text");
        return 0;
    }
    length = strlen(text);
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    memcpy(copy, text, length + 1);
    value->type = ZVALUE_TEXT;
    value->text = heap_add_text(heap, copy);
    if (value->text == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    return 1;
}

static int provider_to_runtime_value(RuntimeHeap *heap,
                                     const ZSharpProviderValue *provider,
                                     RuntimeValue *value, char *error,
                                     size_t error_size) {
    if (provider->type == ZSHARP_PROVIDER_NUMBER) {
        char buffer[32];
        char *copy;
        snprintf(buffer, sizeof(buffer), "%d", (int)provider->number);
        copy = zsharp_copy_text(buffer, strlen(buffer));
        if (copy == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        value->type = ZVALUE_NUMBER;
        value->number_text = heap_add_text(heap, copy);
        if (value->number_text == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    if (provider->type == ZSHARP_PROVIDER_STATUS) {
        value->type = ZVALUE_STATUS;
        value->number = provider->number != 0;
        return 1;
    }
    if (provider->type == ZSHARP_PROVIDER_TEXT) {
        return copy_text_value(heap, provider->text, value, error, error_size);
    }
    snprintf(error, error_size,
             "provider returned an unsupported value type");
    return 0;
}

static int room_imports_project_file(const ZSharpRoom *room,
                                     const char *project,
                                     const char *file) {
    size_t index;
    size_t project_length = strlen(project);
    size_t file_length = strlen(file);
    for (index = 0; index < room->import_count; index++) {
        const char *path = room->imports[index].path;
        size_t length = strlen(path);
        if (strncmp(path, project, project_length) == 0 &&
            path[project_length] == '.' && length > file_length &&
            path[length - file_length - 1] == '.' &&
            strcmp(path + length - file_length, file) == 0) {
            return 1;
        }
    }
    return 0;
}

static int split_value_path(const char *path, uint32_t expected_count,
                            char **storage, char **parts, size_t *part_count,
                            char *error, size_t error_size) {
    char *copy;
    char *cursor;
    size_t count = 1;
    copy = zsharp_copy_text(path, strlen(path));
    if (copy == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    parts[0] = copy;
    for (cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor != '.') continue;
        *cursor = '\0';
        if (count == 5 || cursor[1] == '\0') {
            free(copy);
            snprintf(error, error_size, "invalid qualified value path '%s'",
                     path);
            return 0;
        }
        parts[count++] = cursor + 1;
    }
    if (count != expected_count || count < 2 || count > 5) {
        free(copy);
        snprintf(error, error_size, "invalid qualified value path '%s'",
                 path);
        return 0;
    }
    *storage = copy;
    *part_count = count;
    return 1;
}

static int initialize_program_objects(
    ZSharpProgram *program, RuntimeHeap *heap, const char *project_root,
    const ZSharpProviderBinding *providers, size_t provider_count,
    RuntimeModuleCache *module_cache, unsigned depth, char *error,
    size_t error_size);
static void cleanup_program_objects(ZSharpProgram *program);

static ZSharpProgram *load_project_module_path(
    RuntimeModuleCache *cache, char *source_path, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, unsigned depth, char *error, size_t error_size) {
    size_t index;
    RuntimeModule *resized;
    RuntimeModule *module;
    ZSharpDiagnostic diagnostic;
    char load_error[512] = {0};
    for (index = 0; index < cache->module_count; index++) {
        if (strcmp(cache->modules[index].source_path, source_path) == 0) {
            free(source_path);
            return cache->modules[index].program;
        }
    }
    resized = (RuntimeModule *)realloc(
        cache->modules,
        (cache->module_count + 1) * sizeof(*cache->modules));
    if (resized == NULL) {
        free(source_path);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    cache->modules = resized;
    module = &cache->modules[cache->module_count++];
    memset(module, 0, sizeof(*module));
    module->source_path = source_path;
    module->program = (ZSharpProgram *)malloc(sizeof(*module->program));
    if (module->program == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (!zsharp_project_parse_file(source_path, module->program, &diagnostic,
                                   load_error, sizeof(load_error))) {
        if (diagnostic.message[0] != '\0') {
            snprintf(error, error_size, "%s:%u:%u: %s", source_path,
                     diagnostic.line, diagnostic.column, diagnostic.message);
        } else {
            snprintf(error, error_size, "%s", load_error);
        }
        return NULL;
    }
    module->objects_initialized = 1;
    if (!initialize_program_objects(
            module->program, heap, project_root, providers, provider_count,
            cache, depth + 1, error, error_size)) {
        return NULL;
    }
    return module->program;
}

static ZSharpProgram *load_project_module(
    RuntimeModuleCache *cache, const char *file_name, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, unsigned depth, char *error, size_t error_size) {
    char *source_path = NULL;
    if (!zsharp_project_find_source(project_root, file_name, &source_path,
                                    error, error_size)) {
        return NULL;
    }
    return load_project_module_path(
        cache, source_path, heap, project_root, providers, provider_count,
        depth, error, error_size);
}

static int resolve_room_variable(ZSharpProgram *target_program,
                                 ZSharpRoom *caller_room,
                                 const char *target_room_name,
                                 const char *variable_name, int cross_file,
                                 RuntimeHeap *heap, RuntimeValue *value,
                                 char *error, size_t error_size) {
    ZSharpRoom *target_room = find_room(target_program, target_room_name);
    ZSharpVariable *variable;
    (void)heap;
    if (target_room == NULL) {
        snprintf(error, error_size, "file '%s' has no room named '%s'",
                 target_program->source_name, target_room_name);
        return 0;
    }
    if (!room_is_visible(target_room, caller_room, cross_file)) {
        snprintf(error, error_size, "room '%s.%s' is not visible here",
                 target_program->source_name, target_room_name);
        return 0;
    }
    variable = find_variable(target_room, variable_name);
    if (variable == NULL) {
        snprintf(error, error_size, "room '%s.%s' has no value named '%s'",
                 target_program->source_name, target_room_name,
                 variable_name);
        return 0;
    }
    if (target_room != caller_room && !variable->is_public) {
        snprintf(error, error_size, "value '%s.%s' is silent outside its room",
                 target_room_name, variable_name);
        return 0;
    }
    *value = variable_value(variable);
    return 1;
}

static int resolve_value_path(
    ZSharpProgram *program, ZSharpRoom *room, RuntimeObject *current_object,
    LocalValue *locals, size_t local_count, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, RuntimeModuleCache *module_cache,
    unsigned depth, const ZSharpInstruction *instruction, RuntimeValue *value,
    char *error, size_t error_size) {
    char *storage = NULL;
    char *parts[5] = {0};
    size_t part_count = 0;
    int ok = 0;
    if (!split_value_path(instruction->operand, instruction->argument_count,
                          &storage, parts, &part_count, error, error_size)) {
        return 0;
    }
    if (part_count == 2) {
        RuntimeValue base;
        if (lookup_name(room, current_object, locals, local_count, parts[0],
                        &base) &&
            base.type == ZVALUE_OBJECT && base.object != NULL) {
            RuntimeField *field = find_field(base.object, parts[1]);
            if (field == NULL) {
                snprintf(error, error_size,
                         "room '%s' has no field named '%s'",
                         base.object->room->name, parts[1]);
            } else {
                *value = runtime_field_value(field);
                ok = 1;
            }
        } else {
            ok = resolve_room_variable(program, room, parts[0], parts[1], 0,
                                       heap, value, error, error_size);
        }
    } else if (part_count == 3) {
        ZSharpRoom *candidate_room = find_room(program, parts[0]);
        if (candidate_room != NULL) {
            RuntimeValue object_value;
            RuntimeField *field;
            if (resolve_room_variable(program, room, parts[0], parts[1], 0,
                                      heap, &object_value, error,
                                      error_size) &&
                object_value.type == ZVALUE_OBJECT &&
                object_value.object != NULL &&
                (field = find_field(object_value.object, parts[2])) != NULL) {
                *value = runtime_field_value(field);
                ok = 1;
            } else if (error[0] == '\0') {
                snprintf(error, error_size,
                         "'%s.%s' is not an object with field '%s'", parts[0],
                         parts[1], parts[2]);
            }
        } else if (strcmp(parts[0], program->source_name) == 0) {
            ok = resolve_room_variable(program, room, parts[1], parts[2], 0,
                                       heap, value, error, error_size);
        } else {
            ZSharpProgram *target_program = load_project_module(
                module_cache, parts[0], heap, project_root, providers,
                provider_count, depth, error, error_size);
            if (target_program != NULL) {
                ok = resolve_room_variable(target_program, room, parts[1],
                                           parts[2], 1, heap, value, error,
                                           error_size);
            }
        }
    } else if (part_count == 4 &&
               (find_provider(providers, provider_count, parts[0]) != NULL ||
                room_imports_project_file(room, parts[0], parts[1]))) {
        const ZSharpProviderBinding *binding =
            find_provider(providers, provider_count, parts[0]);
        ZSharpProviderValue provider_value;
        char provider_error[512] = {0};
        memset(&provider_value, 0, sizeof(provider_value));
        if (binding == NULL) {
            snprintf(error, error_size,
                     "external project '%s' has no registered Z# provider; "
                     "use --provider %s=path-to-library",
                     parts[0], parts[0]);
        } else if (binding->provider->get_variable == NULL) {
            snprintf(error, error_size,
                     "provider for project '%s' does not expose variables",
                     parts[0]);
        } else if (!binding->provider->get_variable(
                       binding->provider->user_data, parts[1], parts[2],
                       parts[3], &provider_value, provider_error,
                       sizeof(provider_error))) {
            snprintf(error, error_size, "provider '%s': %s", parts[0],
                     provider_error[0] == '\0' ? "variable lookup failed"
                                                : provider_error);
        } else {
            ok = provider_to_runtime_value(heap, &provider_value, value, error,
                                           error_size);
        }
    } else if (part_count == 4) {
        ZSharpProgram *target_program = program;
        RuntimeValue object_value;
        RuntimeField *field;
        int cross_file = strcmp(parts[0], program->source_name) != 0;
        if (cross_file) {
            target_program = load_project_module(
                module_cache, parts[0], heap, project_root, providers,
                provider_count, depth, error, error_size);
        }
        if (target_program != NULL &&
            resolve_room_variable(target_program, room, parts[1], parts[2],
                                  cross_file, heap, &object_value, error,
                                  error_size) &&
            object_value.type == ZVALUE_OBJECT &&
            object_value.object != NULL &&
            (field = find_field(object_value.object, parts[3])) != NULL) {
            *value = runtime_field_value(field);
            ok = 1;
        } else if (target_program != NULL && error[0] == '\0') {
            snprintf(error, error_size,
                     "'%s.%s.%s' is not an object with field '%s'", parts[0],
                     parts[1], parts[2], parts[3]);
        }
    } else if (part_count == 5) {
        const ZSharpProviderBinding *binding =
            find_provider(providers, provider_count, parts[0]);
        ZSharpProviderValue provider_value;
        char provider_error[512] = {0};
        memset(&provider_value, 0, sizeof(provider_value));
        if (binding == NULL) {
            snprintf(error, error_size,
                     "external project '%s' has no registered Z# provider",
                     parts[0]);
        } else if (binding->provider->get_member == NULL) {
            snprintf(error, error_size,
                     "provider for project '%s' does not expose object "
                     "fields",
                     parts[0]);
        } else if (!binding->provider->get_member(
                       binding->provider->user_data, parts[1], parts[2],
                       parts[3], parts[4], &provider_value, provider_error,
                       sizeof(provider_error))) {
            snprintf(error, error_size, "provider '%s': %s", parts[0],
                     provider_error[0] == '\0' ? "object field read failed"
                                                : provider_error);
        } else {
            ok = provider_to_runtime_value(heap, &provider_value, value, error,
                                           error_size);
        }
    }
    free(storage);
    return ok;
}

static int runtime_to_provider_value(const RuntimeValue *runtime,
                                     ZSharpProviderValue *provider,
                                     char *error, size_t error_size) {
    memset(provider, 0, sizeof(*provider));
    if (runtime->type == ZVALUE_NUMBER) {
        int32_t converted;
        if (!zsharp_decimal_to_int32(runtime->number_text, &converted)) {
            snprintf(error, error_size,
                     "provider ABI v1 numbers must be whole 32-bit values");
            return 0;
        }
        provider->type = ZSHARP_PROVIDER_NUMBER;
        provider->number = converted;
        return 1;
    }
    if (runtime->type == ZVALUE_STATUS) {
        provider->type = ZSHARP_PROVIDER_STATUS;
        provider->number = runtime->number != 0;
        return 1;
    }
    if (runtime->type == ZVALUE_TEXT) {
        provider->type = ZSHARP_PROVIDER_TEXT;
        provider->text = runtime->text;
        return 1;
    }
    snprintf(error, error_size,
             "external projects currently accept number, text, or status "
             "values");
    return 0;
}

static int assign_variable_value(ZSharpVariable *variable,
                                 const RuntimeValue *value, char *error,
                                 size_t error_size) {
    if (variable->type != value->type) {
        fprintf(stderr,
                "runtime warning: assignment to '%s' has the wrong type; "
                "the assignment was skipped\n",
                variable->name);
        return 1;
    }
    if (value->type == ZVALUE_NUMBER) {
        char *copy = zsharp_copy_text(value->number_text,
                                      strlen(value->number_text));
        if (copy == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        free(variable->number_text);
        variable->number_text = copy;
        return 1;
    }
    if (value->type == ZVALUE_STATUS) {
        variable->number_value = value->number;
        return 1;
    }
    if (value->type == ZVALUE_TEXT) {
        char *copy = zsharp_copy_text(value->text, strlen(value->text));
        if (copy == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        free(variable->text_value);
        variable->text_value = copy;
        return 1;
    }
    snprintf(error, error_size,
             "direct assignment to '%s' is not defined for this type",
             variable->name);
    return 0;
}

static int find_visible_variable(ZSharpProgram *target_program,
                                 ZSharpRoom *caller_room,
                                 const char *room_name,
                                 const char *variable_name, int cross_file,
                                 ZSharpVariable **variable_output,
                                 char *error, size_t error_size) {
    ZSharpRoom *target_room = find_room(target_program, room_name);
    ZSharpVariable *variable;
    if (target_room == NULL) {
        snprintf(error, error_size, "file '%s' has no room named '%s'",
                 target_program->source_name, room_name);
        return 0;
    }
    if (!room_is_visible(target_room, caller_room, cross_file)) {
        snprintf(error, error_size, "room '%s.%s' is not visible here",
                 target_program->source_name, room_name);
        return 0;
    }
    variable = find_variable(target_room, variable_name);
    if (variable == NULL) {
        snprintf(error, error_size, "room '%s.%s' has no value named '%s'",
                 target_program->source_name, room_name, variable_name);
        return 0;
    }
    if (target_room != caller_room && !variable->is_public) {
        snprintf(error, error_size, "value '%s.%s' is silent outside its room",
                 room_name, variable_name);
        return 0;
    }
    *variable_output = variable;
    return 1;
}

static int store_value_path(
    ZSharpProgram *program, ZSharpRoom *room, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, RuntimeModuleCache *module_cache, unsigned depth,
    const ZSharpInstruction *instruction, const RuntimeValue *value,
    char *error, size_t error_size) {
    char *storage = NULL;
    char *parts[5] = {0};
    size_t part_count = 0;
    ZSharpVariable *variable = NULL;
    int ok = 0;
    if (!split_value_path(instruction->operand, instruction->argument_count,
                          &storage, parts, &part_count, error, error_size)) {
        return 0;
    }
    if (part_count == 2) {
        if (find_visible_variable(program, room, parts[0], parts[1], 0,
                                  &variable, error, error_size)) {
            ok = assign_variable_value(variable, value, error, error_size);
        }
    } else if (part_count == 3) {
        ZSharpProgram *target_program = program;
        int cross_file = strcmp(parts[0], program->source_name) != 0;
        if (cross_file) {
            target_program = load_project_module(
                module_cache, parts[0], heap, project_root, providers,
                provider_count, depth, error, error_size);
        }
        if (target_program != NULL &&
            find_visible_variable(target_program, room, parts[1], parts[2],
                                  cross_file, &variable, error, error_size)) {
            ok = assign_variable_value(variable, value, error, error_size);
        }
    } else if (part_count == 4) {
        const ZSharpProviderBinding *binding =
            find_provider(providers, provider_count, parts[0]);
        ZSharpProviderValue provider_value;
        char provider_error[512] = {0};
        if (binding == NULL) {
            snprintf(error, error_size,
                     "external project '%s' has no registered Z# provider",
                     parts[0]);
        } else if (binding->provider->set_variable == NULL) {
            snprintf(error, error_size,
                     "provider for project '%s' does not allow variable "
                     "writes",
                     parts[0]);
        } else if (runtime_to_provider_value(value, &provider_value, error,
                                             error_size) &&
                   binding->provider->set_variable(
                       binding->provider->user_data, parts[1], parts[2],
                       parts[3], &provider_value, provider_error,
                       sizeof(provider_error))) {
            ok = 1;
        } else if (error[0] == '\0') {
            snprintf(error, error_size, "provider '%s': %s", parts[0],
                     provider_error[0] == '\0' ? "variable write failed"
                                                : provider_error);
        }
    } else {
        snprintf(error, error_size,
                 "object fields must be written with '.set.Field'");
    }
    free(storage);
    return ok;
}

typedef struct ResolvedObjectPath {
    ZSharpProgram *program;
    ZSharpRoom *room;
    RuntimeObject *object;
    int cross_file;
} ResolvedObjectPath;

static int resolve_object_path(
    ZSharpProgram *program, ZSharpRoom *caller_room, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, RuntimeModuleCache *module_cache, unsigned depth,
    const char *path, uint32_t expected_count, ResolvedObjectPath *resolved,
    char *error, size_t error_size) {
    char *storage = NULL;
    char *parts[5] = {0};
    size_t part_count = 0;
    ZSharpVariable *variable = NULL;
    ZSharpProgram *target_program = program;
    const char *room_name;
    const char *object_name;
    int cross_file = 0;
    if (!split_value_path(path, expected_count, &storage, parts, &part_count,
                          error, error_size)) {
        return 0;
    }
    if (part_count == 2) {
        room_name = parts[0];
        object_name = parts[1];
    } else if (part_count == 3) {
        room_name = parts[1];
        object_name = parts[2];
        cross_file = strcmp(parts[0], program->source_name) != 0;
        if (cross_file) {
            target_program = load_project_module(
                module_cache, parts[0], heap, project_root, providers,
                provider_count, depth, error, error_size);
        }
    } else {
        free(storage);
        snprintf(error, error_size,
                 "external object paths are handled by their project "
                 "provider");
        return 0;
    }
    if (target_program != NULL &&
        find_visible_variable(target_program, caller_room, room_name,
                              object_name, cross_file, &variable, error,
                              error_size)) {
        if (variable->type != ZVALUE_OBJECT ||
            variable->runtime_object == NULL) {
            snprintf(error, error_size, "'%s' is not an object", object_name);
        } else {
            resolved->program = target_program;
            resolved->room = find_room(target_program, room_name);
            resolved->object = (RuntimeObject *)variable->runtime_object;
            resolved->cross_file = cross_file;
            free(storage);
            return 1;
        }
    }
    free(storage);
    return 0;
}

static int execute_function(ZSharpProgram *program, ZSharpRoom *room,
                            ZSharpFunction *function,
                            RuntimeObject *current_object,
                            RuntimeHeap *heap,
                            const char *project_root,
                            const ZSharpProviderBinding *providers,
                            size_t provider_count,
                            RuntimeModuleCache *module_cache, unsigned depth,
                            const RuntimeValue *arguments,
                            size_t argument_count,
                            const char *selected_outcome,
                            RuntimeValue *return_value,
                            int *did_return, char *error, size_t error_size) {
    RuntimeValue *stack = NULL;
    LocalValue *locals = NULL;
    size_t stack_capacity = function->instruction_count + 8;
    size_t local_capacity = function->parameter_count +
                            function->instruction_count + 1;
    size_t stack_count = 0;
    size_t local_count = 0;
    size_t instruction_index = 0;
    size_t index;
    int ok = 1;

    *did_return = 0;
    if (depth > ZSHARP_MAX_CALL_DEPTH) {
        snprintf(error, error_size,
                 "Function.call exceeded the maximum call depth (%d)",
                 ZSHARP_MAX_CALL_DEPTH);
        return 0;
    }
    if (argument_count != function->parameter_count) {
        snprintf(error, error_size,
                 "function '%s' expects %zu argument(s), but received %zu",
                 function->name, function->parameter_count, argument_count);
        return 0;
    }
    stack = (RuntimeValue *)calloc(stack_capacity, sizeof(*stack));
    locals = (LocalValue *)calloc(local_capacity, sizeof(*locals));
    if (stack == NULL || locals == NULL) {
        snprintf(error, error_size, "out of memory");
        free(stack);
        free(locals);
        return 0;
    }
    for (index = 0; index < function->parameter_count; index++) {
        if (arguments[index].type != function->parameters[index].type ||
            (arguments[index].type == ZVALUE_OBJECT &&
             (arguments[index].object == NULL ||
              strcmp(arguments[index].object->room->name,
                     function->parameters[index].object_type) != 0))) {
            snprintf(error, error_size,
                     "argument %zu for '%s' has the wrong type", index + 1,
                     function->name);
            ok = 0;
            goto done;
        }
        locals[local_count].name = function->parameters[index].name;
        locals[local_count].value = arguments[index];
        local_count++;
    }

    while (instruction_index < function->instruction_count) {
        ZSharpInstruction *instruction =
            &function->instructions[instruction_index];
        RuntimeValue left;
        RuntimeValue right;
        RuntimeValue value;
        memset(&value, 0, sizeof(value));

        if (module_cache->window_runtime != NULL &&
            module_cache->window_runtime->is_cancelled != NULL &&
            module_cache->window_runtime->is_cancelled(
                module_cache->window_runtime->state)) {
            snprintf(error, error_size, "window task stopped");
            ok = 0;
            goto done;
        }

        switch (instruction->op) {
            case ZOP_PUSH_NUMBER:
                value.type = ZVALUE_NUMBER;
                value.number_text = instruction->operand;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_PUSH_STATUS:
                value.type = ZVALUE_STATUS;
                value.number = instruction->number_operand != 0;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_PUSH_TEXT:
                value.type = ZVALUE_TEXT;
                value.text = instruction->operand;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_PUSH_NULL:
                value.type = ZVALUE_NULL;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_UI_SET:
            case ZOP_UI_SET_DYNAMIC: {
                const char *property_path = instruction->operand;
                if (instruction->op == ZOP_UI_SET_DYNAMIC) {
                    if (!pop(stack, &stack_count, &value, error,
                             error_size)) {
                        ok = 0;
                        goto done;
                    }
                    if (value.type != ZVALUE_TEXT || value.text == NULL ||
                        value.text[0] == '\0') {
                        snprintf(error, error_size,
                                 "a window property path alias must contain "
                                 "text such as "
                                 "'Startup.Design.background'");
                        ok = 0;
                        goto done;
                    }
                    property_path = value.text;
                }
                if (module_cache->window_runtime == NULL ||
                    module_cache->window_runtime->set_property == NULL) {
                    snprintf(error, error_size,
                             "window properties can only be changed while "
                             "a window callback is running");
                    ok = 0;
                    goto done;
                }
                if (!module_cache->window_runtime->set_property(
                        module_cache->window_runtime->state,
                        property_path,
                        (ZSharpWindowValueType)instruction->number_operand,
                        instruction->call_function,
                        (ZSharpUIUnit)instruction->index_operand,
                        error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_DELAY:
                if (module_cache->window_runtime != NULL &&
                    module_cache->window_runtime->wait != NULL) {
                    if (!module_cache->window_runtime->wait(
                            module_cache->window_runtime->state,
                            instruction->operand, error, error_size)) {
                        ok = 0;
                        goto done;
                    }
                } else if (!runtime_wait(instruction->operand, error,
                                         error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_LOAD_NAME:
                if (!lookup_name(room, current_object, locals, local_count,
                                 instruction->operand, &value)) {
                    snprintf(error, error_size,
                             "room '%s' has no value named '%s'",
                             room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_LOAD_PATH:
                if (!resolve_value_path(
                        program, room, current_object, locals, local_count,
                        heap, project_root, providers, provider_count,
                        module_cache, depth, instruction, &value, error,
                        error_size) ||
                    !push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_STORE_PATH:
                if (!pop(stack, &stack_count, &value, error, error_size) ||
                    !store_value_path(
                        program, room, heap, project_root, providers,
                        provider_count, module_cache, depth, instruction,
                        &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_STORE_GLOBAL: {
                RuntimeField *field =
                    find_field(current_object, instruction->operand);
                ZSharpVariable *variable =
                    find_variable(room, instruction->operand);
                if (field == NULL && variable == NULL) {
                    snprintf(error, error_size,
                             "room '%s' has no number named '%s'",
                             room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if ((field != NULL && field->type != ZVALUE_NUMBER) ||
                    (field == NULL && variable->type != ZVALUE_NUMBER)) {
                    snprintf(error, error_size,
                             "number.set requires a number variable");
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_NUMBER) {
                    fprintf(stderr,
                            "runtime warning: number assignment to '%s' "
                            "received a non-number; the assignment was "
                            "skipped\n",
                            instruction->operand);
                    break;
                }
                if (field != NULL) {
                    if (!runtime_field_assign(field, &value, error,
                                              error_size)) {
                        ok = 0;
                        goto done;
                    }
                } else {
                    if (!assign_variable_value(variable, &value, error,
                                               error_size)) {
                        ok = 0;
                        goto done;
                    }
                }
                break;
            }
            case ZOP_STORE_FIELD: {
                RuntimeField *field =
                    find_field(current_object, instruction->operand);
                if (field == NULL) {
                    snprintf(error, error_size,
                             "the current object has no field named '%s'",
                             instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (field->type == ZVALUE_TEXT &&
                    !coerce_number_to_text(heap, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (!runtime_field_assign(field, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_STORE_LOCAL_TEXT: {
                size_t local_index;
                if (!pop(stack, &stack_count, &value, error, error_size) ||
                    !coerce_number_to_text(heap, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_TEXT) {
                    fprintf(stderr,
                            "runtime warning: local text '%s' received a "
                            "non-text value; the assignment was skipped\n",
                            instruction->operand);
                    memset(&value, 0, sizeof(value));
                    value.type = ZVALUE_TEXT;
                    value.text = "";
                }
                for (local_index = 0; local_index < local_count;
                     local_index++) {
                    if (strcmp(locals[local_index].name,
                               instruction->operand) == 0) {
                        locals[local_index].value = value;
                        break;
                    }
                }
                if (local_index == local_count) {
                    if (local_count >= local_capacity) {
                        snprintf(error, error_size, "too many local values");
                        ok = 0;
                        goto done;
                    }
                    locals[local_count].name = instruction->operand;
                    locals[local_count].value = value;
                    local_count++;
                }
                break;
            }
            case ZOP_STORE_LOCAL: {
                size_t local_index;
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_NUMBER) {
                    fprintf(stderr,
                            "runtime warning: local number '%s' received a "
                            "non-number; the assignment was skipped\n",
                            instruction->operand);
                    memset(&value, 0, sizeof(value));
                    value.type = ZVALUE_NUMBER;
                    value.number_text = "0";
                }
                for (local_index = 0; local_index < local_count; local_index++) {
                    if (strcmp(locals[local_index].name,
                               instruction->operand) == 0) {
                        locals[local_index].value = value;
                        break;
                    }
                }
                if (local_index == local_count) {
                    if (local_count >= local_capacity) {
                        snprintf(error, error_size, "too many local values");
                        ok = 0;
                        goto done;
                    }
                    locals[local_count].name = instruction->operand;
                    locals[local_count].value = value;
                    local_count++;
                }
                break;
            }
            case ZOP_STORE_NAME: {
                RuntimeField *field;
                ZSharpVariable *variable;
                size_t local_index;
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                for (local_index = local_count; local_index > 0; local_index--) {
                    if (strcmp(locals[local_index - 1].name,
                               instruction->operand) == 0) {
                        if (locals[local_index - 1].value.type != value.type) {
                            fprintf(stderr,
                                    "runtime warning: assignment to '%s' has "
                                    "the wrong type; the assignment was "
                                    "skipped\n",
                                    instruction->operand);
                            break;
                        }
                        locals[local_index - 1].value = value;
                        break;
                    }
                }
                if (local_index > 0) break;
                field = find_field(current_object, instruction->operand);
                if (field != NULL) {
                    if (field->type != value.type) {
                        snprintf(error, error_size,
                                 "assignment to '%s' has the wrong type",
                                 instruction->operand);
                        ok = 0;
                        goto done;
                    }
                    if (!runtime_field_assign(field, &value, error,
                                              error_size)) {
                        ok = 0;
                        goto done;
                    }
                    break;
                }
                variable = find_variable(room, instruction->operand);
                if (variable == NULL || variable->type != value.type) {
                    snprintf(error, error_size,
                             "room '%s' has no compatible value named '%s'",
                             room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (!assign_variable_value(variable, &value, error,
                                           error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_SET_MEMBER: {
                RuntimeField *field;
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (left.type != ZVALUE_OBJECT || left.object == NULL) {
                    snprintf(error, error_size,
                             "object field assignment requires an object");
                    ok = 0;
                    goto done;
                }
                field = find_field(left.object, instruction->operand);
                if (field == NULL || field->type != right.type) {
                    snprintf(error, error_size,
                             "room '%s' has no compatible field named '%s'",
                             left.object->room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (!runtime_field_assign(field, &right, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_SET_MEMBER_PATH: {
                char *storage = NULL;
                char *parts[5] = {0};
                size_t part_count = 0;
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !split_value_path(instruction->operand,
                                      instruction->index_operand, &storage,
                                      parts, &part_count, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (part_count == 4) {
                    const ZSharpProviderBinding *binding =
                        find_provider(providers, provider_count, parts[0]);
                    ZSharpProviderValue provider_value;
                    char provider_error[512] = {0};
                    if (binding == NULL) {
                        snprintf(error, error_size,
                                 "external project '%s' has no registered "
                                 "Z# provider",
                                 parts[0]);
                        ok = 0;
                    } else if (binding->provider->set_member == NULL) {
                        snprintf(error, error_size,
                                 "provider for project '%s' does not allow "
                                 "object field writes",
                                 parts[0]);
                        ok = 0;
                    } else if (!runtime_to_provider_value(
                                   &right, &provider_value, error,
                                   error_size) ||
                               !binding->provider->set_member(
                                   binding->provider->user_data, parts[1],
                                   parts[2], parts[3],
                                   instruction->call_function,
                                   &provider_value, provider_error,
                                   sizeof(provider_error))) {
                        if (error[0] == '\0') {
                            snprintf(error, error_size, "provider '%s': %s",
                                     parts[0],
                                     provider_error[0] == '\0'
                                         ? "object field write failed"
                                         : provider_error);
                        }
                        ok = 0;
                    }
                } else {
                    ResolvedObjectPath resolved;
                    RuntimeField *field;
                    memset(&resolved, 0, sizeof(resolved));
                    if (!resolve_object_path(
                            program, room, heap, project_root, providers,
                            provider_count, module_cache, depth,
                            instruction->operand, instruction->index_operand,
                            &resolved, error, error_size)) {
                        ok = 0;
                    } else {
                        field = find_field(resolved.object,
                                           instruction->call_function);
                        if (field == NULL || field->type != right.type) {
                            snprintf(error, error_size,
                                     "room '%s' has no compatible field "
                                     "named '%s'",
                                     resolved.object->room->name,
                                     instruction->call_function);
                            ok = 0;
                        } else {
                            if (!runtime_field_assign(field, &right, error,
                                                      error_size)) {
                                ok = 0;
                            }
                        }
                    }
                }
                free(storage);
                if (!ok) goto done;
                break;
            }
            case ZOP_ADD:
            {
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (left.type == ZVALUE_NUMBER &&
                    right.type == ZVALUE_NUMBER) {
                    char *sum = zsharp_decimal_add(
                        left.number_text, right.number_text, error,
                        error_size);
                    if (sum == NULL) {
                        ok = 0;
                        goto done;
                    }
                    value.type = ZVALUE_NUMBER;
                    value.number_text = heap_add_text(heap, sum);
                    if (value.number_text == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                } else if (left.type == ZVALUE_TEXT &&
                           right.type == ZVALUE_TEXT) {
                    size_t left_length = strlen(left.text);
                    size_t right_length = strlen(right.text);
                    char *combined;
                    if (left_length > SIZE_MAX - right_length - 1) {
                        snprintf(error, error_size,
                                 "combined text value is too large");
                        ok = 0;
                        goto done;
                    }
                    combined = (char *)malloc(left_length + right_length + 1);
                    if (combined == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                    memcpy(combined, left.text, left_length);
                    memcpy(combined + left_length, right.text,
                           right_length + 1);
                    value.type = ZVALUE_TEXT;
                    value.text = heap_add_text(heap, combined);
                    if (value.text == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                } else {
                    snprintf(error, error_size,
                             "'+' requires two numbers or two text values");
                    ok = 0;
                    goto done;
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_SUBTRACT:
            case ZOP_MULTIPLY:
            case ZOP_DIVIDE:
            case ZOP_REMAINDER: {
                char *result;
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (left.type != ZVALUE_NUMBER ||
                    right.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "number arithmetic requires two numbers");
                    ok = 0;
                    goto done;
                }
                if (instruction->op == ZOP_SUBTRACT) {
                    result = zsharp_decimal_subtract(
                        left.number_text, right.number_text, error,
                        error_size);
                } else if (instruction->op == ZOP_MULTIPLY) {
                    result = zsharp_decimal_multiply(
                        left.number_text, right.number_text, error,
                        error_size);
                } else if (instruction->op == ZOP_DIVIDE) {
                    result = zsharp_decimal_divide(
                        left.number_text, right.number_text, error,
                        error_size);
                } else {
                    result = zsharp_decimal_remainder(
                        left.number_text, right.number_text, error,
                        error_size);
                }
                if (result == NULL) {
                    ok = 0;
                    goto done;
                }
                value.type = ZVALUE_NUMBER;
                value.number_text = heap_add_text(heap, result);
                if (value.number_text == NULL) {
                    snprintf(error, error_size, "out of memory");
                    ok = 0;
                    goto done;
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_GREATER_EQUAL:
            case ZOP_GREATER:
            case ZOP_LESS_EQUAL:
            case ZOP_LESS:
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (left.type != ZVALUE_NUMBER ||
                    right.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "number comparison requires two numbers");
                    ok = 0;
                    goto done;
                }
                value.type = ZVALUE_STATUS;
                {
                    int comparison = zsharp_decimal_compare(
                        left.number_text, right.number_text);
                if (instruction->op == ZOP_GREATER_EQUAL) {
                    value.number = comparison >= 0;
                } else if (instruction->op == ZOP_GREATER) {
                    value.number = comparison > 0;
                } else if (instruction->op == ZOP_LESS_EQUAL) {
                    value.number = comparison <= 0;
                } else {
                    value.number = comparison < 0;
                }
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_EQUAL:
            case ZOP_NOT_EQUAL:
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                value.type = ZVALUE_STATUS;
                if (left.type != right.type) {
                    snprintf(error, error_size,
                             "comparison requires values of the same type");
                    ok = 0;
                    goto done;
                } else if (left.type == ZVALUE_NUMBER ||
                           left.type == ZVALUE_STATUS) {
                    value.number = left.type == ZVALUE_NUMBER
                        ? zsharp_decimal_compare(left.number_text,
                                                 right.number_text) == 0
                        : left.number == right.number;
                } else if (left.type == ZVALUE_TEXT) {
                    value.number = strcmp(left.text, right.text) == 0;
                } else if (left.type == ZVALUE_OBJECT) {
                    value.number = left.object == right.object;
                } else if (left.type == ZVALUE_NULL) {
                    value.number = 1;
                } else {
                    snprintf(error, error_size,
                             "'==' is not defined for this value type");
                    ok = 0;
                    goto done;
                }
                if (instruction->op == ZOP_NOT_EQUAL) {
                    value.number = !value.number;
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_NOT:
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_STATUS) {
                    snprintf(error, error_size,
                             "'not' requires a status value");
                    ok = 0;
                    goto done;
                }
                value.number = !value.number;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_NEGATE:
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "number negation requires a number");
                    ok = 0;
                    goto done;
                }
                {
                    char *negated = zsharp_decimal_negate(
                        value.number_text, error, error_size);
                    if (negated == NULL) {
                        ok = 0;
                        goto done;
                    }
                    value.number_text = heap_add_text(heap, negated);
                }
                if (value.number_text == NULL) {
                    snprintf(error, error_size, "out of memory");
                    ok = 0;
                    goto done;
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_AND:
            case ZOP_OR:
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (left.type != ZVALUE_STATUS ||
                    right.type != ZVALUE_STATUS) {
                    snprintf(error, error_size,
                             "logical operations require two status values");
                    ok = 0;
                    goto done;
                }
                value.type = ZVALUE_STATUS;
                value.number = instruction->op == ZOP_AND
                    ? left.number && right.number
                    : left.number || right.number;
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_GET_INDEX:
            {
                size_t array_index;
                if (!pop(stack, &stack_count, &right, error, error_size) ||
                    !pop(stack, &stack_count, &left, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if ((left.type != ZVALUE_TEXT_ARRAY &&
                     left.type != ZVALUE_NUMBER_ARRAY &&
                     left.type != ZVALUE_OBJECT_ARRAY) ||
                    right.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "array indexing requires an array and a number "
                             "index");
                    ok = 0;
                    goto done;
                }
                if (!zsharp_decimal_to_size(right.number_text, &array_index) ||
                    (left.type == ZVALUE_TEXT_ARRAY
                         ? array_index >= left.array->text_item_count
                         : left.type == ZVALUE_NUMBER_ARRAY
                               ? array_index >= left.array->number_item_count
                               : array_index >= left.array->object_item_count)) {
                    snprintf(error, error_size,
                             "index %s is outside array '%s'",
                             right.number_text, left.array->name);
                    ok = 0;
                    goto done;
                }
                if (left.type == ZVALUE_TEXT_ARRAY) {
                    value.type = ZVALUE_TEXT;
                    value.text = left.array->text_items[array_index];
                } else {
                    if (left.type == ZVALUE_NUMBER_ARRAY) {
                        value.type = ZVALUE_NUMBER;
                        value.number_text =
                            left.array->number_items[array_index];
                    } else {
                        value.type = ZVALUE_OBJECT;
                        value.object = (RuntimeObject *)left.array
                            ->object_items[array_index].runtime_object;
                    }
                }
                if (!push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_SET_INDEX:
            {
                RuntimeValue array_value;
                RuntimeValue index_value;
                size_t array_index;
                char *copy;
                if (!pop(stack, &stack_count, &value, error, error_size) ||
                    !pop(stack, &stack_count, &index_value, error,
                         error_size) ||
                    !pop(stack, &stack_count, &array_value, error,
                         error_size)) {
                    ok = 0;
                    goto done;
                }
                if ((array_value.type != ZVALUE_TEXT_ARRAY &&
                     array_value.type != ZVALUE_NUMBER_ARRAY) ||
                    index_value.type != ZVALUE_NUMBER ||
                    !zsharp_decimal_to_size(index_value.number_text,
                                             &array_index)) {
                    snprintf(error, error_size,
                             "array.set requires an array and a whole, "
                             "non-negative index");
                    ok = 0;
                    goto done;
                }
                if ((array_value.type == ZVALUE_TEXT_ARRAY &&
                     array_index >= array_value.array->text_item_count) ||
                    (array_value.type == ZVALUE_NUMBER_ARRAY &&
                     array_index >= array_value.array->number_item_count)) {
                    snprintf(error, error_size,
                             "index %s is outside array '%s'",
                             index_value.number_text,
                             array_value.array->name);
                    ok = 0;
                    goto done;
                }
                if (array_value.type == ZVALUE_TEXT_ARRAY &&
                    value.type == ZVALUE_NUMBER &&
                    !coerce_number_to_text(heap, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if ((array_value.type == ZVALUE_TEXT_ARRAY &&
                     value.type != ZVALUE_TEXT) ||
                    (array_value.type == ZVALUE_NUMBER_ARRAY &&
                     value.type != ZVALUE_NUMBER)) {
                    fprintf(stderr,
                            "runtime warning: array assignment to '%s' has "
                            "the wrong type; the assignment was skipped\n",
                            array_value.array->name);
                    break;
                }
                copy = zsharp_copy_text(
                    value.type == ZVALUE_TEXT ? value.text
                                              : value.number_text,
                    strlen(value.type == ZVALUE_TEXT ? value.text
                                                      : value.number_text));
                if (copy == NULL) {
                    snprintf(error, error_size, "out of memory");
                    ok = 0;
                    goto done;
                }
                if (array_value.type == ZVALUE_TEXT_ARRAY) {
                    free(array_value.array->text_items[array_index]);
                    array_value.array->text_items[array_index] = copy;
                } else {
                    free(array_value.array->number_items[array_index]);
                    array_value.array->number_items[array_index] = copy;
                }
                break;
            }
            case ZOP_ARRAY_LENGTH:
            {
                size_t count;
                char buffer[32];
                char *copy;
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type == ZVALUE_TEXT_ARRAY) {
                    count = value.array->text_item_count;
                } else if (value.type == ZVALUE_NUMBER_ARRAY) {
                    count = value.array->number_item_count;
                } else if (value.type == ZVALUE_OBJECT_ARRAY) {
                    count = value.array->object_item_count;
                } else {
                    snprintf(error, error_size,
                             "Length is only available on arrays");
                    ok = 0;
                    goto done;
                }
                snprintf(buffer, sizeof(buffer), "%zu", count);
                copy = zsharp_copy_text(buffer, strlen(buffer));
                if (copy == NULL) {
                    snprintf(error, error_size, "out of memory");
                    ok = 0;
                    goto done;
                }
                value.type = ZVALUE_NUMBER;
                value.number_text = heap_add_text(heap, copy);
                if (value.number_text == NULL ||
                    !push(stack, stack_capacity, &stack_count, value, error,
                          error_size)) {
                    if (value.number_text == NULL) {
                        snprintf(error, error_size, "out of memory");
                    }
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_ARRAY_ADD_OBJECT:
            {
                RuntimeValue array_value;
                RuntimeValue *constructor_arguments = NULL;
                RuntimeObject *created;
                ZSharpObjectArrayItem *resized;
                size_t argument_index = instruction->argument_count;
                if (argument_index > 0) {
                    constructor_arguments = (RuntimeValue *)calloc(
                        argument_index, sizeof(*constructor_arguments));
                    if (constructor_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                }
                while (argument_index > 0) {
                    argument_index--;
                    if (!pop(stack, &stack_count,
                             &constructor_arguments[argument_index], error,
                             error_size)) {
                        free(constructor_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                if (!pop(stack, &stack_count, &array_value, error,
                         error_size) ||
                    array_value.type != ZVALUE_OBJECT_ARRAY ||
                    strcmp(array_value.array->array_object_type,
                           instruction->operand) != 0) {
                    free(constructor_arguments);
                    if (error[0] == '\0') {
                        snprintf(error, error_size,
                                 "array.add object type does not match the "
                                 "array type");
                    }
                    ok = 0;
                    goto done;
                }
                created = create_object_from_values(
                    program, instruction->operand, constructor_arguments,
                    instruction->argument_count, heap, project_root,
                    providers, provider_count, module_cache, depth + 1, error,
                    error_size);
                free(constructor_arguments);
                if (created == NULL) {
                    ok = 0;
                    goto done;
                }
                resized = (ZSharpObjectArrayItem *)realloc(
                    array_value.array->object_items,
                    (array_value.array->object_item_count + 1) *
                        sizeof(*array_value.array->object_items));
                if (resized == NULL) {
                    free(created->fields);
                    free(created);
                    snprintf(error, error_size, "out of memory");
                    ok = 0;
                    goto done;
                }
                array_value.array->object_items = resized;
                memset(&array_value.array->object_items[
                           array_value.array->object_item_count],
                       0, sizeof(*array_value.array->object_items));
                array_value.array->object_items[
                    array_value.array->object_item_count].runtime_object =
                    created;
                array_value.array->object_item_count++;
                break;
            }
            case ZOP_GET_MEMBER: {
                RuntimeField *field;
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_OBJECT || value.object == NULL) {
                    snprintf(error, error_size,
                             "member access requires an object value");
                    ok = 0;
                    goto done;
                }
                field = find_field(value.object, instruction->operand);
                if (field == NULL) {
                    snprintf(error, error_size,
                             "room '%s' has no field named '%s'",
                             value.object->room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (field->definition != NULL &&
                    field->definition->is_horde) {
                    snprintf(error, error_size,
                             "horde field '%s.%s' must be accessed through "
                             "its room",
                             value.object->room->name, field->name);
                    ok = 0;
                    goto done;
                }
                value = runtime_field_value(field);
                if (!push(stack, stack_capacity, &stack_count, value,
                          error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_PRINT:
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type == ZVALUE_NUMBER) {
                    puts(value.number_text);
                } else if (value.type == ZVALUE_TEXT) {
                    puts(value.text);
                } else if (value.type == ZVALUE_STATUS) {
                    puts(value.number ? "alive" : "dead");
                } else if (value.type == ZVALUE_NULL) {
                    puts("null");
                } else {
                    snprintf(error, error_size,
                             "Print does not yet support this value type");
                    ok = 0;
                    goto done;
                }
                break;
            case ZOP_CALL_LOCAL: {
                ZSharpFunction *target =
                    find_function(room, instruction->operand);
                RuntimeValue *call_arguments = NULL;
                RuntimeValue call_return;
                int call_did_return = 0;
                size_t argument_index;
                if (target == NULL) {
                    snprintf(error, error_size,
                             "room '%s' has no function named '%s'",
                             room->name, instruction->operand);
                    ok = 0;
                    goto done;
                }
                if (instruction->argument_count > 0) {
                    call_arguments = (RuntimeValue *)calloc(
                        instruction->argument_count, sizeof(*call_arguments));
                    if (call_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                }
                argument_index = instruction->argument_count;
                while (argument_index > 0) {
                    argument_index--;
                    if (!pop(stack, &stack_count,
                             &call_arguments[argument_index], error,
                             error_size)) {
                        free(call_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                if (!execute_function(program, room, target, current_object,
                                      heap, project_root, providers,
                                      provider_count, module_cache, depth + 1,
                                      call_arguments,
                                      instruction->argument_count,
                                      NULL,
                                      &call_return, &call_did_return, error,
                                      error_size)) {
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                free(call_arguments);
                if (!call_did_return) {
                    snprintf(error, error_size,
                             "brain '%s' does not feed a value",
                             target->name);
                    ok = 0;
                    goto done;
                }
                if (!push(stack, stack_capacity, &stack_count, call_return,
                          error, error_size)) {
                    ok = 0;
                    goto done;
                }
                break;
            }
            case ZOP_CALL_METHOD: {
                RuntimeValue *call_arguments = NULL;
                RuntimeValue object_value;
                RuntimeValue ignored_return;
                int ignored_did_return = 0;
                size_t argument_index = instruction->argument_count;
                ZSharpFunction *target;
                if (instruction->argument_count > 0) {
                    call_arguments = (RuntimeValue *)calloc(
                        instruction->argument_count, sizeof(*call_arguments));
                    if (call_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                }
                while (argument_index > 0) {
                    argument_index--;
                    if (!pop(stack, &stack_count,
                             &call_arguments[argument_index], error,
                             error_size)) {
                        free(call_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                if (!pop(stack, &stack_count, &object_value, error,
                         error_size) || object_value.type != ZVALUE_OBJECT ||
                    object_value.object == NULL) {
                    free(call_arguments);
                    if (error[0] == '\0') {
                        snprintf(error, error_size,
                                 "method calls require an object value");
                    }
                    ok = 0;
                    goto done;
                }
                target = find_function(object_value.object->room,
                                       instruction->operand);
                if (target == NULL) {
                    snprintf(error, error_size,
                             "room '%s' has no method named '%s'",
                             object_value.object->room->name,
                             instruction->operand);
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                if (target->is_horde) {
                    snprintf(error, error_size,
                             "horde function '%s.%s' must be called with "
                             "Function.call",
                             object_value.object->room->name, target->name);
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                if (!execute_function(
                        program, object_value.object->room, target,
                        object_value.object, heap, project_root, providers,
                        provider_count, module_cache, depth + 1,
                        call_arguments, instruction->argument_count,
                        NULL,
                        &ignored_return, &ignored_did_return, error,
                        error_size)) {
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                free(call_arguments);
                break;
            }
            case ZOP_CALL_METHOD_PATH: {
                RuntimeValue *call_arguments = NULL;
                size_t argument_index = instruction->argument_count;
                char *storage = NULL;
                char *parts[5] = {0};
                size_t part_count = 0;
                if (instruction->argument_count > 0) {
                    call_arguments = (RuntimeValue *)calloc(
                        instruction->argument_count, sizeof(*call_arguments));
                    if (call_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                }
                while (argument_index > 0) {
                    argument_index--;
                    if (!pop(stack, &stack_count,
                             &call_arguments[argument_index], error,
                             error_size)) {
                        free(call_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                if (!split_value_path(instruction->operand,
                                      instruction->index_operand, &storage,
                                      parts, &part_count, error, error_size)) {
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                if (part_count == 4) {
                    const ZSharpProviderBinding *binding =
                        find_provider(providers, provider_count, parts[0]);
                    ZSharpProviderValue *provider_arguments = NULL;
                    char provider_error[512] = {0};
                    if (instruction->argument_count > 0) {
                        provider_arguments = (ZSharpProviderValue *)calloc(
                            instruction->argument_count,
                            sizeof(*provider_arguments));
                    }
                    if (binding == NULL) {
                        snprintf(error, error_size,
                                 "external project '%s' has no registered "
                                 "Z# provider",
                                 parts[0]);
                        ok = 0;
                    } else if (binding->provider->call_method == NULL) {
                        snprintf(error, error_size,
                                 "provider for project '%s' does not expose "
                                 "object methods",
                                 parts[0]);
                        ok = 0;
                    } else if (instruction->argument_count > 0 &&
                               provider_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                    } else {
                        for (argument_index = 0;
                             argument_index < instruction->argument_count;
                             argument_index++) {
                            if (!runtime_to_provider_value(
                                    &call_arguments[argument_index],
                                    &provider_arguments[argument_index], error,
                                    error_size)) {
                                ok = 0;
                                break;
                            }
                        }
                        if (ok && !binding->provider->call_method(
                                      binding->provider->user_data, parts[1],
                                      parts[2], parts[3],
                                      instruction->call_function,
                                      provider_arguments,
                                      instruction->argument_count,
                                      provider_error,
                                      sizeof(provider_error))) {
                            snprintf(error, error_size, "provider '%s': %s",
                                     parts[0],
                                     provider_error[0] == '\0'
                                         ? "object method call failed"
                                         : provider_error);
                            ok = 0;
                        }
                    }
                    free(provider_arguments);
                } else {
                    ResolvedObjectPath resolved;
                    ZSharpFunction *target;
                    RuntimeValue ignored_return;
                    int ignored_did_return = 0;
                    memset(&resolved, 0, sizeof(resolved));
                    if (!resolve_object_path(
                            program, room, heap, project_root, providers,
                            provider_count, module_cache, depth,
                            instruction->operand, instruction->index_operand,
                            &resolved, error, error_size)) {
                        ok = 0;
                    } else {
                        target = find_function(resolved.object->room,
                                               instruction->call_function);
                        if (target == NULL) {
                            snprintf(error, error_size,
                                     "room '%s' has no method named '%s'",
                                     resolved.object->room->name,
                                     instruction->call_function);
                            ok = 0;
                        } else if (target->is_horde) {
                            snprintf(error, error_size,
                                     "horde function '%s.%s' must be called "
                                     "with Function.call",
                                     resolved.object->room->name,
                                     target->name);
                            ok = 0;
                        } else if (!target->is_public &&
                                   resolved.object->room != room) {
                            snprintf(error, error_size,
                                     "method '%s' is silent outside room "
                                     "'%s'",
                                     target->name,
                                     resolved.object->room->name);
                            ok = 0;
                        } else if (!execute_function(
                                       resolved.program,
                                       resolved.object->room, target,
                                       resolved.object, heap, project_root,
                                       providers, provider_count, module_cache,
                                       depth + 1, call_arguments,
                                       instruction->argument_count,
                                       NULL,
                                       &ignored_return, &ignored_did_return,
                                       error, error_size)) {
                            ok = 0;
                        }
                    }
                }
                free(storage);
                free(call_arguments);
                if (!ok) goto done;
                break;
            }
            case ZOP_CALL_QUALIFIED:
            case ZOP_CALL_QUALIFIED_VALUE: {
                ZSharpProgram *call_program = program;
                ZSharpRoom *target_room;
                ZSharpFunction *target_function;
                RuntimeValue call_return;
                RuntimeValue *call_arguments = NULL;
                int call_did_return = 0;
                int external;
                size_t argument_index = instruction->argument_count;
                if (instruction->argument_count > 0) {
                    call_arguments = (RuntimeValue *)calloc(
                        instruction->argument_count,
                        sizeof(*call_arguments));
                    if (call_arguments == NULL) {
                        snprintf(error, error_size, "out of memory");
                        ok = 0;
                        goto done;
                    }
                }
                while (argument_index > 0) {
                    argument_index--;
                    if (!pop(stack, &stack_count,
                             &call_arguments[argument_index], error,
                             error_size)) {
                        free(call_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                if (instruction->operand != NULL &&
                    instruction->operand[0] != '\0') {
                    const ZSharpProviderBinding *binding = find_provider(
                        providers, provider_count, instruction->operand);
                    char provider_error[512] = {0};
                    if (binding == NULL) {
                        snprintf(error, error_size,
                                 "external project '%s' has no registered "
                                 "Z# provider; use --provider "
                                 "%s=path-to-library",
                                 instruction->operand, instruction->operand);
                        ok = 0;
                        free(call_arguments);
                        goto done;
                    }
                    if (instruction->argument_count != 0 ||
                        instruction->op == ZOP_CALL_QUALIFIED_VALUE) {
                        snprintf(error, error_size,
                                 "provider function arguments and returned "
                                 "values require the next provider ABI");
                        ok = 0;
                        free(call_arguments);
                        goto done;
                    }
                    if (binding->provider->call_function == NULL) {
                        snprintf(error, error_size,
                                 "provider for project '%s' does not expose "
                                 "functions",
                                 instruction->operand);
                        ok = 0;
                        free(call_arguments);
                        goto done;
                    }
                    if (!binding->provider->call_function(
                            binding->provider->user_data,
                            instruction->call_file, instruction->call_room,
                            instruction->call_function, provider_error,
                            sizeof(provider_error))) {
                        snprintf(error, error_size, "provider '%s': %s",
                                 instruction->operand,
                                 provider_error[0] == '\0'
                                     ? "function call failed"
                                     : provider_error);
                        ok = 0;
                        free(call_arguments);
                        goto done;
                    }
                    free(call_arguments);
                    break;
                }
                external = strcmp(instruction->call_file,
                                  program->source_name) != 0;
                if (external) {
                    call_program = load_project_module(
                        module_cache, instruction->call_file, heap,
                        project_root, providers, provider_count, depth, error,
                        error_size);
                    if (call_program == NULL) {
                        free(call_arguments);
                        ok = 0;
                        goto done;
                    }
                }
                target_room = find_room(call_program, instruction->call_room);
                target_function = target_room == NULL
                    ? NULL
                    : find_function(target_room,
                                    instruction->call_function);
                if (target_room == NULL || target_function == NULL) {
                    snprintf(error, error_size,
                             "Function.call could not resolve '%s:%s:%s'",
                             instruction->call_file, instruction->call_room,
                             instruction->call_function);
                    ok = 0;
                    free(call_arguments);
                    goto done;
                }
                if (!room_is_visible(target_room, room, external)) {
                    snprintf(error, error_size,
                             "room '%s:%s' is not visible to room '%s:%s'",
                             instruction->call_file, target_room->name,
                             program->source_name, room->name);
                    ok = 0;
                    free(call_arguments);
                    goto done;
                }
                if (!target_function->is_public && target_room != room) {
                    snprintf(error, error_size,
                             "function '%s' is silent outside room '%s'",
                             target_function->name, target_room->name);
                    ok = 0;
                    free(call_arguments);
                    goto done;
                }
                if (!execute_function(call_program, target_room,
                                      target_function, NULL, heap, project_root,
                                      providers, provider_count, module_cache,
                                      depth + 1,
                                      call_arguments,
                                      instruction->argument_count,
                                      instruction->call_outcome != NULL &&
                                              instruction->call_outcome[0] != '\0'
                                          ? instruction->call_outcome
                                          : NULL,
                                      &call_return, &call_did_return, error,
                                      error_size)) {
                    free(call_arguments);
                    ok = 0;
                    goto done;
                }
                free(call_arguments);
                if (instruction->op == ZOP_CALL_QUALIFIED_VALUE) {
                    if (!call_did_return) {
                        if (target_function->return_type == ZRETURN_VOID &&
                            instruction->call_outcome != NULL &&
                            instruction->call_outcome[0] != '\0') {
                            memset(&call_return, 0, sizeof(call_return));
                            call_return.type = ZVALUE_NULL;
                        } else {
                            snprintf(error, error_size,
                                     "brain '%s' does not feed a value",
                                     target_function->name);
                            ok = 0;
                            goto done;
                        }
                    }
                    if (!push(stack, stack_capacity, &stack_count, call_return,
                              error, error_size)) {
                        ok = 0;
                        goto done;
                    }
                }
                break;
            }
            case ZOP_RETURN_VALUE:
                if (!pop(stack, &stack_count, return_value, error,
                         error_size)) {
                    ok = 0;
                    goto done;
                }
                if (function->return_type == ZRETURN_NUMBER &&
                    return_value->type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "number function '%s' must feed a number",
                             function->name);
                    ok = 0;
                    goto done;
                }
                if (function->return_type == ZRETURN_TEXT &&
                    return_value->type != ZVALUE_TEXT) {
                    snprintf(error, error_size,
                             "text function '%s' must feed text",
                             function->name);
                    ok = 0;
                    goto done;
                }
                *did_return = 1;
                goto done;
            case ZOP_RETURN_VOID:
                *did_return = 0;
                goto done;
            case ZOP_NAMED_IF_START:
                if (selected_outcome != NULL &&
                    strcmp(selected_outcome, instruction->operand) != 0) {
                    if (instruction->index_operand >
                        function->instruction_count) {
                        snprintf(error, error_size,
                                 "invalid named if jump target");
                        ok = 0;
                        goto done;
                    }
                    instruction_index = instruction->index_operand;
                    continue;
                }
                break;
            case ZOP_RETURN_IF_FALSE:
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_STATUS &&
                    value.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "an if condition must produce a status");
                    ok = 0;
                    goto done;
                }
                if ((value.type == ZVALUE_STATUS && !value.number) ||
                    (value.type == ZVALUE_NUMBER &&
                     zsharp_decimal_is_zero(value.number_text))) {
                    *did_return = 0;
                    goto done;
                }
                break;
            case ZOP_JUMP_IF_FALSE:
                if (!pop(stack, &stack_count, &value, error, error_size)) {
                    ok = 0;
                    goto done;
                }
                if (value.type != ZVALUE_STATUS &&
                    value.type != ZVALUE_NUMBER) {
                    snprintf(error, error_size,
                             "an if condition must produce a status");
                    ok = 0;
                    goto done;
                }
                if ((value.type == ZVALUE_STATUS && !value.number) ||
                    (value.type == ZVALUE_NUMBER &&
                     zsharp_decimal_is_zero(value.number_text))) {
                    if (instruction->index_operand >
                        function->instruction_count) {
                        snprintf(error, error_size,
                                 "invalid bytecode jump target");
                        ok = 0;
                        goto done;
                    }
                    instruction_index = instruction->index_operand;
                    continue;
                }
                break;
            case ZOP_JUMP:
                if (instruction->index_operand > function->instruction_count) {
                    snprintf(error, error_size,
                             "invalid bytecode jump target");
                    ok = 0;
                    goto done;
                }
                instruction_index = instruction->index_operand;
                continue;
            default:
                snprintf(error, error_size, "unsupported bytecode operation %d",
                         (int)instruction->op);
                ok = 0;
                goto done;
        }
        instruction_index++;
    }
    if (function->return_type != ZRETURN_VOID) {
        snprintf(error, error_size,
                 "%s function '%s' ended without feed",
                 function->return_type == ZRETURN_TEXT ? "text" : "number",
                 function->name);
        ok = 0;
    }

done:
    free(stack);
    free(locals);
    return ok;
}

static RuntimeValue literal_value(const ZSharpLiteral *literal) {
    RuntimeValue value;
    memset(&value, 0, sizeof(value));
    value.type = literal->type;
    value.number = literal->number_value;
    value.number_text = literal->number_text;
    value.text = literal->text_value;
    return value;
}

static RuntimeObject *create_object_from_values(
    ZSharpProgram *program, const char *object_type,
    const RuntimeValue *arguments, size_t argument_count, RuntimeHeap *heap,
    const char *project_root, const ZSharpProviderBinding *providers,
    size_t provider_count, RuntimeModuleCache *module_cache, unsigned depth,
    char *error, size_t error_size) {
    ZSharpRoom *class_room = find_room(program, object_type);
    RuntimeObject *object;
    ZSharpFunction *constructor;
    RuntimeValue ignored_return;
    int ignored_did_return = 0;
    size_t index;
    if (class_room == NULL) {
        snprintf(error, error_size,
                 "new could not find room '%s'", object_type);
        return NULL;
    }
    if (class_room->is_horde) {
        snprintf(error, error_size,
                 "horde room '%s' cannot be created with new",
                 class_room->name);
        return NULL;
    }
    object = (RuntimeObject *)calloc(1, sizeof(*object));
    if (object == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    object->room = class_room;
    object->field_count = class_room->variable_count;
    if (object->field_count > 0) {
        object->fields = (RuntimeField *)calloc(object->field_count,
                                                sizeof(*object->fields));
        if (object->fields == NULL) {
            snprintf(error, error_size, "out of memory");
            free(object);
            return NULL;
        }
    }
    for (index = 0; index < object->field_count; index++) {
        object->fields[index].name = class_room->variables[index].name;
        object->fields[index].type = class_room->variables[index].type;
        object->fields[index].definition = &class_room->variables[index];
        object->fields[index].value =
            variable_value(&class_room->variables[index]);
    }
    constructor = find_function(class_room, object_type);
    if (constructor == NULL) {
        if (argument_count != 0) {
            snprintf(error, error_size,
                     "room '%s' has no matching constructor",
                     object_type);
            free(object->fields);
            free(object);
            return NULL;
        }
    } else if (!execute_function(
                   program, class_room, constructor, object, heap, project_root,
                   providers, provider_count, module_cache, depth + 1,
                   arguments, argument_count, NULL, &ignored_return,
                   &ignored_did_return, error, error_size)) {
        free(object->fields);
        free(object);
        return NULL;
    }
    return object;
}

static RuntimeObject *create_object(ZSharpProgram *program,
                                    ZSharpVariable *variable,
                                    RuntimeHeap *heap,
                                    const char *project_root,
                                    const ZSharpProviderBinding *providers,
                                    size_t provider_count,
                                    RuntimeModuleCache *module_cache,
                                    unsigned depth, char *error,
                                    size_t error_size) {
    RuntimeValue *arguments = NULL;
    RuntimeObject *object;
    size_t index;
    if (variable->constructor_argument_count > 0) {
        arguments = (RuntimeValue *)calloc(
            variable->constructor_argument_count, sizeof(*arguments));
        if (arguments == NULL) {
            snprintf(error, error_size, "out of memory");
            return NULL;
        }
        for (index = 0; index < variable->constructor_argument_count; index++) {
            arguments[index] =
                literal_value(&variable->constructor_arguments[index]);
        }
    }
    object = create_object_from_values(
        program, variable->object_type, arguments,
        variable->constructor_argument_count, heap, project_root, providers,
        provider_count, module_cache, depth, error, error_size);
    free(arguments);
    return object;
}

static int initialize_program_objects(ZSharpProgram *program,
                                      RuntimeHeap *heap,
                                      const char *project_root,
                                      const ZSharpProviderBinding *providers,
                                      size_t provider_count,
                                      RuntimeModuleCache *module_cache,
                                      unsigned depth, char *error,
                                      size_t error_size) {
    size_t room_index;
    for (room_index = 0; room_index < program->room_count; room_index++) {
        ZSharpRoom *room = &program->rooms[room_index];
        size_t variable_index;
        for (variable_index = 0; variable_index < room->variable_count;
             variable_index++) {
            ZSharpVariable *variable = &room->variables[variable_index];
            if (variable->type == ZVALUE_OBJECT &&
                variable->runtime_object == NULL) {
                variable->runtime_object = create_object(
                    program, variable, heap, project_root, providers,
                    provider_count, module_cache, depth, error, error_size);
                if (variable->runtime_object == NULL) {
                    cleanup_program_objects(program);
                    return 0;
                }
            } else if (variable->type == ZVALUE_OBJECT_ARRAY) {
                size_t item_index;
                for (item_index = 0;
                     item_index < variable->object_item_count; item_index++) {
                    ZSharpObjectArrayItem *item =
                        &variable->object_items[item_index];
                    ZSharpVariable temporary;
                    if (item->runtime_object != NULL) continue;
                    memset(&temporary, 0, sizeof(temporary));
                    temporary.type = ZVALUE_OBJECT;
                    temporary.object_type = variable->array_object_type;
                    temporary.constructor_arguments =
                        item->constructor_arguments;
                    temporary.constructor_argument_count =
                        item->constructor_argument_count;
                    item->runtime_object = create_object(
                        program, &temporary, heap, project_root, providers,
                        provider_count, module_cache, depth, error,
                        error_size);
                    if (item->runtime_object == NULL) {
                        cleanup_program_objects(program);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static void cleanup_program_objects(ZSharpProgram *program) {
    size_t room_index;
    for (room_index = 0; room_index < program->room_count; room_index++) {
        ZSharpRoom *room = &program->rooms[room_index];
        size_t variable_index;
        for (variable_index = 0; variable_index < room->variable_count;
             variable_index++) {
            ZSharpVariable *variable = &room->variables[variable_index];
            RuntimeObject *object =
                (RuntimeObject *)variable->runtime_object;
            if (object != NULL) {
                free(object->fields);
                free(object);
                variable->runtime_object = NULL;
            }
            if (variable->type == ZVALUE_OBJECT_ARRAY) {
                size_t item_index;
                for (item_index = 0;
                     item_index < variable->object_item_count; item_index++) {
                    object = (RuntimeObject *)
                        variable->object_items[item_index].runtime_object;
                    if (object != NULL) {
                        free(object->fields);
                        free(object);
                        variable->object_items[item_index].runtime_object =
                            NULL;
                    }
                }
            }
        }
    }
}

static void cleanup_module_cache(RuntimeModuleCache *cache) {
    size_t index;
    for (index = 0; index < cache->module_count; index++) {
        RuntimeModule *module = &cache->modules[index];
        if (module->program != NULL) {
            cleanup_program_objects(module->program);
            zsharp_program_free(module->program);
            free(module->program);
        }
        free(module->source_path);
    }
    free(cache->modules);
    memset(cache, 0, sizeof(*cache));
}

typedef struct WindowExecutionContext {
    ZSharpProgram *program;
    RuntimeHeap *heap;
    const char *project_root;
    const ZSharpProviderBinding *providers;
    size_t provider_count;
    RuntimeModuleCache *module_cache;
    struct WindowTask **tasks;
    size_t task_count;
    struct WindowRoomState **room_states;
    size_t room_state_count;
    int stopping;
} WindowExecutionContext;

typedef struct WindowSharedVariable {
    char *name;
    ZSharpValueType type;
    char *text;
    int32_t number;
} WindowSharedVariable;

typedef struct WindowRoomState {
    char *source_path;
    char *room_name;
    WindowSharedVariable *variables;
    size_t variable_count;
    int initialized;
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
} WindowRoomState;

typedef struct WindowTask {
    WindowExecutionContext *context;
    const ZSharpWindowRuntime *runtime;
    char *source_path;
    char *room_name;
    char *function_name;
    WindowRoomState *room_state;
    int failed;
    char error[512];
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
} WindowTask;

static void lock_window_room_state(WindowRoomState *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->mutex);
#else
    pthread_mutex_lock(&state->mutex);
#endif
}

static void unlock_window_room_state(WindowRoomState *state) {
#ifdef _WIN32
    LeaveCriticalSection(&state->mutex);
#else
    pthread_mutex_unlock(&state->mutex);
#endif
}

static WindowRoomState *find_or_add_window_room_state(
    WindowExecutionContext *context, const char *source_path,
    const char *room_name, char *error, size_t error_size) {
    size_t index;
    WindowRoomState *state;
    WindowRoomState **resized;
    for (index = 0; index < context->room_state_count; index++) {
        state = context->room_states[index];
        if (strcmp(state->source_path, source_path) == 0 &&
            strcmp(state->room_name, room_name) == 0) return state;
    }
    state = (WindowRoomState *)calloc(1, sizeof(*state));
    if (state == NULL) goto out_of_memory;
    state->source_path = zsharp_copy_text(source_path, strlen(source_path));
    state->room_name = zsharp_copy_text(room_name, strlen(room_name));
    if (state->source_path == NULL || state->room_name == NULL) {
        free(state->source_path);
        free(state->room_name);
        free(state);
        goto out_of_memory;
    }
#ifdef _WIN32
    InitializeCriticalSection(&state->mutex);
#else
    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        free(state->source_path);
        free(state->room_name);
        free(state);
        snprintf(error, error_size,
                 "could not initialize Z# window task state");
        return NULL;
    }
#endif
    resized = (WindowRoomState **)realloc(
        context->room_states,
        (context->room_state_count + 1) * sizeof(*context->room_states));
    if (resized == NULL) {
#ifdef _WIN32
        DeleteCriticalSection(&state->mutex);
#else
        pthread_mutex_destroy(&state->mutex);
#endif
        free(state->source_path);
        free(state->room_name);
        free(state);
        goto out_of_memory;
    }
    context->room_states = resized;
    context->room_states[context->room_state_count++] = state;
    return state;

out_of_memory:
    snprintf(error, error_size, "out of memory");
    return NULL;
}

static int restore_window_room_state(WindowRoomState *state,
                                     ZSharpRoom *room, char *error,
                                     size_t error_size) {
    size_t index;
    if (!state->initialized) return 1;
    for (index = 0; index < state->variable_count; index++) {
        WindowSharedVariable *saved = &state->variables[index];
        ZSharpVariable *variable = find_variable(room, saved->name);
        char *copy;
        if (variable == NULL || variable->type != saved->type) continue;
        if (saved->type == ZVALUE_STATUS) {
            variable->number_value = saved->number;
        } else if (saved->type == ZVALUE_NUMBER) {
            copy = zsharp_copy_text(saved->text, strlen(saved->text));
            if (copy == NULL) goto out_of_memory;
            free(variable->number_text);
            variable->number_text = copy;
        } else if (saved->type == ZVALUE_TEXT) {
            copy = zsharp_copy_text(saved->text, strlen(saved->text));
            if (copy == NULL) goto out_of_memory;
            free(variable->text_value);
            variable->text_value = copy;
        }
    }
    return 1;

out_of_memory:
    snprintf(error, error_size, "out of memory");
    return 0;
}

static int save_window_room_state(WindowRoomState *state, ZSharpRoom *room,
                                  char *error, size_t error_size) {
    WindowSharedVariable *variables = NULL;
    size_t count = 0;
    size_t index;
    for (index = 0; index < room->variable_count; index++) {
        ZSharpVariable *variable = &room->variables[index];
        WindowSharedVariable *resized;
        WindowSharedVariable *saved;
        const char *text = NULL;
        if (variable->type != ZVALUE_NUMBER && variable->type != ZVALUE_TEXT &&
            variable->type != ZVALUE_STATUS) continue;
        resized = (WindowSharedVariable *)realloc(
            variables, (count + 1) * sizeof(*variables));
        if (resized == NULL) goto out_of_memory;
        variables = resized;
        saved = &variables[count++];
        memset(saved, 0, sizeof(*saved));
        saved->name = zsharp_copy_text(variable->name, strlen(variable->name));
        saved->type = variable->type;
        saved->number = variable->number_value;
        if (variable->type == ZVALUE_NUMBER) text = variable->number_text;
        else if (variable->type == ZVALUE_TEXT) text = variable->text_value;
        if (saved->name == NULL) goto out_of_memory;
        if (text != NULL) {
            saved->text = zsharp_copy_text(text, strlen(text));
            if (saved->text == NULL) goto out_of_memory;
        }
    }
    for (index = 0; index < state->variable_count; index++) {
        free(state->variables[index].name);
        free(state->variables[index].text);
    }
    free(state->variables);
    state->variables = variables;
    state->variable_count = count;
    state->initialized = 1;
    return 1;

out_of_memory:
    for (index = 0; index < count; index++) {
        free(variables[index].name);
        free(variables[index].text);
    }
    free(variables);
    snprintf(error, error_size, "out of memory");
    return 0;
}

static void cleanup_window_room_states(WindowExecutionContext *context) {
    size_t index;
    for (index = 0; index < context->room_state_count; index++) {
        WindowRoomState *state = context->room_states[index];
        size_t variable_index;
        for (variable_index = 0; variable_index < state->variable_count;
             variable_index++) {
            free(state->variables[variable_index].name);
            free(state->variables[variable_index].text);
        }
        free(state->variables);
#ifdef _WIN32
        DeleteCriticalSection(&state->mutex);
#else
        pthread_mutex_destroy(&state->mutex);
#endif
        free(state->source_path);
        free(state->room_name);
        free(state);
    }
    free(context->room_states);
    context->room_states = NULL;
    context->room_state_count = 0;
}

static int window_task_cancelled(const WindowTask *task) {
    return task->runtime != NULL && task->runtime->is_cancelled != NULL &&
           task->runtime->is_cancelled(task->runtime->state);
}

static void run_window_task(WindowTask *task) {
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic;
    RuntimeHeap heap;
    RuntimeModuleCache module_cache;
    ZSharpRoom *room;
    ZSharpFunction *function;
    RuntimeValue ignored_return;
    int ignored_did_return = 0;
    char parse_error[512] = {0};
    int initialized = 0;
    int state_locked = 0;
    int ok = 0;
    memset(&program, 0, sizeof(program));
    memset(&diagnostic, 0, sizeof(diagnostic));
    memset(&heap, 0, sizeof(heap));
    memset(&module_cache, 0, sizeof(module_cache));
    module_cache.window_runtime = task->runtime;
    if (!zsharp_project_parse_file(task->source_path, &program, &diagnostic,
                                   parse_error, sizeof(parse_error))) {
        if (diagnostic.message[0] != '\0') {
            snprintf(task->error, sizeof(task->error), "%s:%u:%u: %s",
                     task->source_path, diagnostic.line, diagnostic.column,
                     diagnostic.message);
        } else {
            snprintf(task->error, sizeof(task->error), "%s",
                     parse_error[0] == '\0' ? "could not load window task"
                                             : parse_error);
        }
        goto done;
    }
    if (!initialize_program_objects(
            &program, &heap, task->context->project_root,
            task->context->providers, task->context->provider_count,
            &module_cache, 1, task->error, sizeof(task->error))) {
        goto done;
    }
    initialized = 1;
    room = find_room(&program, task->room_name);
    function = room == NULL ? NULL : find_function(room, task->function_name);
    if (room == NULL || function == NULL) {
        snprintf(task->error, sizeof(task->error),
                 "window task could not resolve '%s:%s'",
                 task->room_name, task->function_name);
        goto done;
    }
    lock_window_room_state(task->room_state);
    state_locked = 1;
    if (!restore_window_room_state(task->room_state, room, task->error,
                                   sizeof(task->error))) goto done;
    ok = execute_function(
        &program, room, function, NULL, &heap, task->context->project_root,
        task->context->providers, task->context->provider_count,
        &module_cache, 1, NULL, 0, NULL, &ignored_return,
        &ignored_did_return, task->error, sizeof(task->error));
    if (ok && !save_window_room_state(task->room_state, room, task->error,
                                      sizeof(task->error))) ok = 0;
done:
    if (state_locked) unlock_window_room_state(task->room_state);
    if (window_task_cancelled(task)) ok = 1;
    task->failed = !ok;
    cleanup_module_cache(&module_cache);
    if (initialized) cleanup_program_objects(&program);
    zsharp_program_free(&program);
    heap_free(&heap);
}

#ifdef _WIN32
static DWORD WINAPI window_task_entry(LPVOID data) {
    run_window_task((WindowTask *)data);
    return 0;
}
#else
static void *window_task_entry(void *data) {
    run_window_task((WindowTask *)data);
    return NULL;
}
#endif

static int start_window_task(WindowExecutionContext *context,
                             const ZSharpWindowRuntime *runtime,
                             const char *source_path, const char *room_name,
                             const char *function_name, char *error,
                             size_t error_size) {
    WindowTask *task;
    WindowTask **resized;
    if (context->stopping) {
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    task = (WindowTask *)calloc(1, sizeof(*task));
    if (task == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    task->context = context;
    task->runtime = runtime;
    task->source_path = zsharp_copy_text(source_path, strlen(source_path));
    task->room_name = zsharp_copy_text(room_name, strlen(room_name));
    task->function_name = zsharp_copy_text(function_name,
                                           strlen(function_name));
    task->room_state = find_or_add_window_room_state(
        context, source_path, room_name, error, error_size);
    if (task->source_path == NULL || task->room_name == NULL ||
        task->function_name == NULL || task->room_state == NULL) {
        free(task->source_path);
        free(task->room_name);
        free(task->function_name);
        free(task);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    resized = (WindowTask **)realloc(
        context->tasks, (context->task_count + 1) * sizeof(*context->tasks));
    if (resized == NULL) {
        free(task->source_path);
        free(task->room_name);
        free(task->function_name);
        free(task);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    context->tasks = resized;
    context->tasks[context->task_count++] = task;
#ifdef _WIN32
    task->thread = CreateThread(NULL, 0, window_task_entry, task, 0, NULL);
    if (task->thread == NULL) {
#else
    if (pthread_create(&task->thread, NULL, window_task_entry, task) != 0) {
#endif
        context->task_count--;
        free(task->source_path);
        free(task->room_name);
        free(task->function_name);
        free(task);
        snprintf(error, error_size, "could not start a Z# runtime task");
        return 0;
    }
    return 1;
}

static int stop_window_tasks(WindowExecutionContext *context, char *error,
                             size_t error_size) {
    size_t index;
    int ok = 1;
    context->stopping = 1;
    for (index = 0; index < context->task_count; index++) {
        WindowTask *task = context->tasks[index];
#ifdef _WIN32
        WaitForSingleObject(task->thread, INFINITE);
        CloseHandle(task->thread);
#else
        pthread_join(task->thread, NULL);
#endif
        if (ok && task->failed) {
            snprintf(error, error_size, "%s",
                     task->error[0] == '\0' ? "a Z# runtime task failed"
                                             : task->error);
            ok = 0;
        }
        free(task->source_path);
        free(task->room_name);
        free(task->function_name);
        free(task);
    }
    free(context->tasks);
    context->tasks = NULL;
    context->task_count = 0;
    cleanup_window_room_states(context);
    return ok;
}

static int split_window_target(const char *target, char **storage,
                               char **parts, size_t *count_output) {
    char *copy = zsharp_copy_text(target, strlen(target));
    char *cursor;
    size_t count = 1;
    if (copy == NULL) return 0;
    parts[0] = copy;
    for (cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor == ':') {
            *cursor = '\0';
            if (count == 4 || cursor[1] == '\0') {
                free(copy);
                return 0;
            }
            parts[count++] = cursor + 1;
        }
    }
    if (count != 3 && count != 4) {
        free(copy);
        return 0;
    }
    *storage = copy;
    *count_output = count;
    return 1;
}

static int execute_project_starts(WindowExecutionContext *context,
                                  const ZSharpWindowRuntime *runtime,
                                  char *error, size_t error_size) {
    ZSharpSourceList sources;
    size_t source_index;
    int ok = 1;
    if (!zsharp_project_list_sources(context->project_root, &sources,
                                     error, error_size)) return 0;
    for (source_index = 0; ok && source_index < sources.count;
         source_index++) {
        char *source_path = sources.items[source_index];
        ZSharpProgram *program;
        size_t room_index;
        sources.items[source_index] = NULL;
        program = load_project_module_path(
            context->module_cache, source_path, context->heap,
            context->project_root, context->providers,
            context->provider_count, 1, error, error_size);
        if (program == NULL) {
            ok = 0;
            break;
        }
        if (program->script_type == ZSCRIPT_WINDOW) continue;
        for (room_index = 0; ok && room_index < program->room_count;
             room_index++) {
            ZSharpRoom *room = &program->rooms[room_index];
            size_t function_index;
            for (function_index = 0;
                 function_index < room->function_count; function_index++) {
                ZSharpFunction *function = &room->functions[function_index];
                if (strcmp(function->name, "Start") != 0 ||
                    function->disable_auto_run) continue;
                if (!start_window_task(
                        context, runtime, source_path,
                        room->qualified_name == NULL ? room->name
                                                     : room->qualified_name,
                        function->name, error, error_size)) {
                    ok = 0;
                    break;
                }
            }
        }
    }
    zsharp_project_source_list_free(&sources);
    return ok;
}

static int execute_window_callback(void *user_data, const char *target,
                                   const ZSharpWindowRuntime *runtime,
                                   char *error, size_t error_size) {
    WindowExecutionContext *context = (WindowExecutionContext *)user_data;
    char *storage = NULL;
    char *parts[4] = {0};
    size_t count = 0;
    const char *file;
    const char *room_name;
    const char *function_name;
    ZSharpProgram *target_program;
    ZSharpRoom *room;
    ZSharpFunction *function;
    int ok;
    if (strcmp(target, ZSHARP_WINDOW_PROJECT_STARTS) == 0) {
        return execute_project_starts(context, runtime, error, error_size);
    }
    if (strcmp(target, ZSHARP_WINDOW_TASKS_STOP) == 0) {
        return stop_window_tasks(context, error, error_size);
    }
    if (!split_window_target(target, &storage, parts, &count)) {
        snprintf(error, error_size, "invalid window callback '%s'", target);
        return 0;
    }
    if (count == 4 &&
        (context->program->project_id == NULL ||
         strcmp(parts[0], context->program->project_id) != 0)) {
        const ZSharpProviderBinding *binding = find_provider(
            context->providers, context->provider_count, parts[0]);
        if (binding == NULL || binding->provider->call_function == NULL) {
            snprintf(error, error_size,
                     "external callback project '%s' has no function "
                     "provider", parts[0]);
            free(storage);
            return 0;
        }
        ok = binding->provider->call_function(
            binding->provider->user_data, parts[1], parts[2], parts[3],
            error, error_size);
        free(storage);
        return ok;
    }
    file = parts[count - 3];
    room_name = parts[count - 2];
    function_name = parts[count - 1];
    target_program = load_project_module(
        context->module_cache, file, context->heap, context->project_root,
        context->providers, context->provider_count, 1, error, error_size);
    if (target_program == NULL) {
        free(storage);
        return 0;
    }
    room = find_room(target_program, room_name);
    function = room == NULL ? NULL : find_function(room, function_name);
    if (room == NULL || function == NULL) {
        snprintf(error, error_size,
                 "window callback could not resolve '%s'", target);
        free(storage);
        return 0;
    }
    if (function->parameter_count != 0) {
        snprintf(error, error_size,
                 "window callback '%s' must not require arguments", target);
        free(storage);
        return 0;
    }
    {
        char *source_path = NULL;
        if (!zsharp_project_find_source(context->project_root, file,
                                        &source_path, error, error_size)) {
            free(storage);
            return 0;
        }
        ok = start_window_task(
            context, runtime, source_path,
            room->qualified_name == NULL ? room->name : room->qualified_name,
            function->name, error, error_size);
        free(source_path);
    }
    free(storage);
    return ok;
}

int zsharp_vm_run_with_providers(
    ZSharpProgram *program, const char *project_root,
    const ZSharpProviderBinding *providers, size_t provider_count,
    char *error, size_t error_size) {
    size_t room_index;
    RuntimeHeap heap;
    RuntimeModuleCache module_cache;
    int ok = 1;
    memset(&heap, 0, sizeof(heap));
    memset(&module_cache, 0, sizeof(module_cache));
    if (!initialize_program_objects(program, &heap, project_root, providers,
                                    provider_count, &module_cache, 1, error,
                                    error_size)) {
        cleanup_module_cache(&module_cache);
        heap_free(&heap);
        return 0;
    }
    if (program->script_type == ZSCRIPT_WINDOW) {
        WindowExecutionContext context;
        context.program = program;
        context.heap = &heap;
        context.project_root = project_root;
        context.providers = providers;
        context.provider_count = provider_count;
        context.module_cache = &module_cache;
        context.tasks = NULL;
        context.task_count = 0;
        context.room_states = NULL;
        context.room_state_count = 0;
        context.stopping = 0;
        ok = zsharp_window_run(program, project_root,
                               execute_window_callback, &context,
                               error, error_size);
        goto done;
    }
    for (room_index = 0; room_index < program->room_count; room_index++) {
        ZSharpRoom *room = &program->rooms[room_index];
        size_t function_index;
        for (function_index = 0; function_index < room->function_count;
             function_index++) {
            ZSharpFunction *function = &room->functions[function_index];
            if (strcmp(function->name, "Start") == 0 &&
                !function->disable_auto_run) {
                RuntimeValue ignored_return;
                int ignored_did_return = 0;
                if (!execute_function(program, room, function, NULL, &heap,
                                      project_root, providers, provider_count,
                                      &module_cache, 1, NULL, 0, NULL,
                                      &ignored_return,
                                      &ignored_did_return, error, error_size)) {
                    ok = 0;
                    goto done;
                }
            }
        }
    }
done:
    cleanup_module_cache(&module_cache);
    cleanup_program_objects(program);
    heap_free(&heap);
    return ok;
}

int zsharp_vm_run(ZSharpProgram *program, const char *project_root,
                  char *error, size_t error_size) {
    return zsharp_vm_run_with_providers(program, project_root, NULL, 0, error,
                                        error_size);
}
