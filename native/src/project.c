#include "project.h"

#include "zsharp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct SearchState {
    const char *file_name;
    char *match;
    int failed;
    char *error;
    size_t error_size;
} SearchState;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

static char *join_path(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    int needs_separator = directory_length > 0 &&
                          directory[directory_length - 1] != '/' &&
                          directory[directory_length - 1] != '\\';
    char *path = (char *)malloc(directory_length + (size_t)needs_separator +
                               name_length + 1);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, directory, directory_length);
    if (needs_separator) {
#ifdef _WIN32
        path[directory_length++] = '\\';
#else
        path[directory_length++] = '/';
#endif
    }
    memcpy(path + directory_length, name, name_length);
    path[directory_length + name_length] = '\0';
    return path;
}

static int source_name_matches(const char *entry_name, const char *file_name) {
    size_t entry_length = strlen(entry_name);
    size_t name_length = strlen(file_name);
    size_t extension_length = strlen(ZSHARP_SOURCE_EXTENSION);
    return entry_length == name_length + extension_length &&
           memcmp(entry_name, file_name, name_length) == 0 &&
           memcmp(entry_name + name_length, ZSHARP_SOURCE_EXTENSION,
                  extension_length) == 0;
}

static void accept_match(SearchState *state, char *path) {
    if (state->match != NULL) {
        if (state->error != NULL && state->error_size > 0) {
            snprintf(state->error, state->error_size,
                     "Z# file '%s' is ambiguous; found both '%s' "
                     "and '%s'",
                     state->file_name, state->match, path);
        }
        free(path);
        state->failed = 1;
        return;
    }
    state->match = path;
}

#ifdef _WIN32
static void search_directory(const char *directory, SearchState *state) {
    WIN32_FIND_DATAA entry;
    char *pattern;
    HANDLE search;
    if (state->failed) {
        return;
    }
    pattern = join_path(directory, "*");
    if (pattern == NULL) {
        set_error(state->error, state->error_size, "out of memory");
        state->failed = 1;
        return;
    }
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND &&
            code != ERROR_ACCESS_DENIED) {
            set_error(state->error, state->error_size,
                      "could not search the Z# project directory");
            state->failed = 1;
        }
        return;
    }
    do {
        char *path;
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        path = join_path(directory, entry.cFileName);
        if (path == NULL) {
            set_error(state->error, state->error_size, "out of memory");
            state->failed = 1;
            break;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                search_directory(path, state);
            }
            free(path);
        } else if (source_name_matches(entry.cFileName, state->file_name)) {
            accept_match(state, path);
        } else {
            free(path);
        }
    } while (!state->failed && FindNextFileA(search, &entry));
    FindClose(search);
}
#else
static void search_directory(const char *directory, SearchState *state) {
    DIR *stream;
    struct dirent *entry;
    if (state->failed) {
        return;
    }
    stream = opendir(directory);
    if (stream == NULL) {
        if (errno != EACCES && errno != ENOENT) {
            set_error(state->error, state->error_size,
                      "could not search the Z# project directory");
            state->failed = 1;
        }
        return;
    }
    while (!state->failed && (entry = readdir(stream)) != NULL) {
        char *path;
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        path = join_path(directory, entry->d_name);
        if (path == NULL) {
            set_error(state->error, state->error_size, "out of memory");
            state->failed = 1;
            break;
        }
        if (lstat(path, &status) != 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            search_directory(path, state);
            free(path);
        } else if (S_ISREG(status.st_mode) &&
                   source_name_matches(entry->d_name, state->file_name)) {
            accept_match(state, path);
        } else {
            free(path);
        }
    }
    closedir(stream);
}
#endif

char *zsharp_project_current_directory(char *error, size_t error_size) {
#ifdef _WIN32
    DWORD required = GetCurrentDirectoryA(0, NULL);
    char *directory;
    if (required == 0) {
        set_error(error, error_size, "could not determine the project directory");
        return NULL;
    }
    directory = (char *)malloc(required);
    if (directory == NULL) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    if (GetCurrentDirectoryA(required, directory) == 0) {
        free(directory);
        set_error(error, error_size, "could not determine the project directory");
        return NULL;
    }
    return directory;
#else
    size_t size = 256;
    for (;;) {
        char *directory = (char *)malloc(size);
        if (directory == NULL) {
            set_error(error, error_size, "out of memory");
            return NULL;
        }
        if (getcwd(directory, size) != NULL) {
            return directory;
        }
        free(directory);
        if (errno != ERANGE || size > ((size_t)1 << 20)) {
            set_error(error, error_size,
                      "could not determine the project directory");
            return NULL;
        }
        size *= 2;
    }
#endif
}

int zsharp_project_find_source(const char *project_root, const char *file_name,
                               char **source_path, char *error,
                               size_t error_size) {
    SearchState state;
    memset(&state, 0, sizeof(state));
    state.file_name = file_name;
    state.error = error;
    state.error_size = error_size;
    search_directory(project_root, &state);
    if (state.failed) {
        free(state.match);
        return 0;
    }
    if (state.match == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size,
                     "could not find Z# file '%s%s' anywhere under '%s'",
                     file_name, ZSHARP_SOURCE_EXTENSION, project_root);
        }
        return 0;
    }
    *source_path = state.match;
    return 1;
}

static char *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *source;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    source = (char *)malloc((size_t)size + 1);
    if (source == NULL) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source);
        fclose(file);
        return NULL;
    }
    source[size] = '\0';
    fclose(file);
    return source;
}

static char *source_stem(const char *path) {
    const char *name = path;
    const char *cursor;
    const char *dot;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            name = cursor + 1;
        }
    }
    dot = strrchr(name, '.');
    if (dot == NULL) {
        dot = name + strlen(name);
    }
    return zsharp_copy_text(name, (size_t)(dot - name));
}

int zsharp_project_parse_file(const char *path, ZSharpProgram *program,
                              ZSharpDiagnostic *diagnostic, char *error,
                              size_t error_size) {
    char *source = read_source(path);
    char *stem;
    int ok;
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (source == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "could not read '%s'", path);
        }
        return 0;
    }
    stem = source_stem(path);
    if (stem == NULL) {
        free(source);
        set_error(error, error_size, "out of memory");
        return 0;
    }
    ok = zsharp_parse_source(source, stem, program, diagnostic);
    free(stem);
    free(source);
    return ok;
}

static const ZSharpRoom *model_find_room(const ZSharpProgram *program,
                                         const char *name) {
    size_t index;
    for (index = 0; index < program->room_count; index++) {
        const ZSharpRoom *room = &program->rooms[index];
        if (strcmp(room->name, name) == 0 ||
            (room->qualified_name != NULL &&
             strcmp(room->qualified_name, name) == 0)) {
            return room;
        }
    }
    return NULL;
}

static const ZSharpVariable *model_find_variable(const ZSharpRoom *room,
                                                  const char *name) {
    size_t index;
    for (index = 0; index < room->variable_count; index++) {
        if (strcmp(room->variables[index].name, name) == 0) {
            return &room->variables[index];
        }
    }
    return NULL;
}

static const ZSharpFunction *model_find_function(const ZSharpRoom *room,
                                                  const char *name) {
    size_t index;
    for (index = 0; index < room->function_count; index++) {
        if (strcmp(room->functions[index].name, name) == 0) {
            return &room->functions[index];
        }
    }
    return NULL;
}

static int model_room_visible(const ZSharpRoom *target,
                              const ZSharpRoom *caller, int cross_file) {
    if (target == caller) return 1;
    if (cross_file) return target->visibility == ZVISIBILITY_NOTICED;
    if (target->visibility != ZVISIBILITY_SILENT) return 1;
    return target->parent_name != NULL && target->parent_name[0] != '\0' &&
           caller->qualified_name != NULL &&
           strcmp(target->parent_name, caller->qualified_name) == 0;
}

static int split_model_path(const char *path, char **storage, char **parts,
                            size_t maximum, size_t *count_output) {
    char *copy = zsharp_copy_text(path, strlen(path));
    char *cursor;
    size_t count = 1;
    if (copy == NULL) return 0;
    parts[0] = copy;
    for (cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor == '.') {
            *cursor = '\0';
            if (count == maximum || cursor[1] == '\0') {
                free(copy);
                return 0;
            }
            parts[count++] = cursor + 1;
        }
    }
    *storage = copy;
    *count_output = count;
    return 1;
}

static int import_matches_project_file(const ZSharpRoom *room,
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

static int require_file_import(const ZSharpProgram *program,
                               const ZSharpSettings *settings,
                               const ZSharpRoom *room, const char *file,
                               char *error, size_t error_size) {
    if (strcmp(file, program->source_name) == 0 ||
        import_matches_project_file(room, settings->project_id, file)) {
        return 1;
    }
    snprintf(error, error_size,
             "room '%s' uses file '%s' without importing it",
             room->qualified_name, file);
    return 0;
}

static int require_project_import(const ZSharpRoom *room,
                                  const ZSharpSettings *settings,
                                  const char *project, const char *file,
                                  char *error, size_t error_size) {
    if (strcmp(project, settings->project_id) != 0 &&
        zsharp_settings_find_dependency(settings, project) == NULL) {
        snprintf(error, error_size,
                 "project '%s' is not listed in Dependencies", project);
        return 0;
    }
    if (import_matches_project_file(room, project, file)) return 1;
    snprintf(error, error_size,
             "room '%s' uses '%s.%s' without importing that file",
             room->qualified_name, project, file);
    return 0;
}

static int validate_variable_access(const ZSharpProgram *target_program,
                                    const ZSharpRoom *caller,
                                    const char *room_name,
                                    const char *variable_name, int cross_file,
                                    const ZSharpVariable **variable_output,
                                    char *error, size_t error_size) {
    const ZSharpRoom *target_room = model_find_room(target_program, room_name);
    const ZSharpVariable *variable;
    if (target_room == NULL) {
        snprintf(error, error_size, "file '%s' has no room named '%s'",
                 target_program->source_name, room_name);
        return 0;
    }
    if (!model_room_visible(target_room, caller, cross_file)) {
        snprintf(error, error_size, "room '%s.%s' is not visible to '%s'",
                 target_program->source_name, room_name,
                 caller->qualified_name);
        return 0;
    }
    variable = model_find_variable(target_room, variable_name);
    if (variable == NULL) {
        snprintf(error, error_size, "room '%s.%s' has no value named '%s'",
                 target_program->source_name, room_name, variable_name);
        return 0;
    }
    if (target_room != caller && !variable->is_public) {
        snprintf(error, error_size,
                 "value '%s.%s' is silent outside its room", room_name,
                 variable_name);
        return 0;
    }
    if (variable_output != NULL) *variable_output = variable;
    return 1;
}

static int load_model_file(const char *project_root, const char *file,
                           ZSharpProgram *target, char **source_path,
                           char *error, size_t error_size) {
    ZSharpDiagnostic diagnostic;
    char load_error[512] = {0};
    if (!zsharp_project_find_source(project_root, file, source_path, error,
                                    error_size)) {
        return 0;
    }
    if (!zsharp_project_parse_file(*source_path, target, &diagnostic,
                                   load_error, sizeof(load_error))) {
        if (diagnostic.message[0] != '\0') {
            snprintf(error, error_size, "%s:%u:%u: %s", *source_path,
                     diagnostic.line, diagnostic.column, diagnostic.message);
        } else {
            snprintf(error, error_size, "%s", load_error);
        }
        free(*source_path);
        *source_path = NULL;
        return 0;
    }
    return 1;
}

static int validate_file_variable(const ZSharpProgram *program,
                                  const ZSharpSettings *settings,
                                  const ZSharpRoom *room,
                                  const char *project_root,
                                  const char *file, const char *target_room,
                                  const char *variable, char *error,
                                  size_t error_size) {
    ZSharpProgram target;
    char *source_path = NULL;
    int ok;
    if (!require_file_import(program, settings, room, file, error,
                             error_size)) return 0;
    if (strcmp(file, program->source_name) == 0) {
        return validate_variable_access(program, room, target_room, variable,
                                        0, NULL, error, error_size);
    }
    if (!load_model_file(project_root, file, &target, &source_path, error,
                         error_size)) {
        return 0;
    }
    ok = validate_variable_access(&target, room, target_room, variable, 1,
                                  NULL, error, error_size);
    zsharp_program_free(&target);
    free(source_path);
    return ok;
}

static int validate_object_member(const ZSharpProgram *target_program,
                                  const ZSharpRoom *caller,
                                  const char *holder_room,
                                  const char *object_name, int cross_file,
                                  const char *member_name, int is_method,
                                  char *error, size_t error_size) {
    const ZSharpVariable *object_variable = NULL;
    const ZSharpRoom *class_room;
    if (!validate_variable_access(target_program, caller, holder_room,
                                  object_name, cross_file, &object_variable,
                                  error, error_size)) {
        return 0;
    }
    if (object_variable->type != ZVALUE_OBJECT) {
        snprintf(error, error_size, "value '%s.%s' is not an object",
                 holder_room, object_name);
        return 0;
    }
    class_room = model_find_room(target_program, object_variable->object_type);
    if (class_room == NULL) {
        snprintf(error, error_size, "object type '%s' does not exist",
                 object_variable->object_type);
        return 0;
    }
    if (is_method) {
        const ZSharpFunction *method =
            model_find_function(class_room, member_name);
        if (method == NULL) {
            snprintf(error, error_size,
                     "room '%s' has no method named '%s'", class_room->name,
                     member_name);
            return 0;
        }
        if (method->is_horde) {
            snprintf(error, error_size,
                     "horde function '%s.%s' must be called with "
                     "Function.call", class_room->name, member_name);
            return 0;
        }
        if (!method->is_public && class_room != caller) {
            snprintf(error, error_size,
                     "method '%s.%s' is silent outside its room",
                     class_room->name, member_name);
            return 0;
        }
    } else {
        const ZSharpVariable *field =
            model_find_variable(class_room, member_name);
        if (field == NULL) {
            snprintf(error, error_size, "room '%s' has no field named '%s'",
                     class_room->name, member_name);
            return 0;
        }
        if (field->is_horde) {
            snprintf(error, error_size,
                     "horde field '%s.%s' must be accessed through its room",
                     class_room->name, member_name);
            return 0;
        }
        if (!field->is_public && class_room != caller) {
            snprintf(error, error_size,
                     "field '%s.%s' is silent outside its room",
                     class_room->name, member_name);
            return 0;
        }
    }
    return 1;
}

static int validate_file_object_member(
    const ZSharpProgram *program, const ZSharpSettings *settings,
    const ZSharpRoom *caller,
    const char *project_root, const char *file, const char *holder_room,
    const char *object_name, const char *member_name, int is_method,
    char *error, size_t error_size) {
    ZSharpProgram target;
    char *source_path = NULL;
    int ok;
    if (!require_file_import(program, settings, caller, file, error,
                             error_size)) {
        return 0;
    }
    if (strcmp(file, program->source_name) == 0) {
        return validate_object_member(program, caller, holder_room,
                                      object_name, 0, member_name, is_method,
                                      error, error_size);
    }
    if (!load_model_file(project_root, file, &target, &source_path, error,
                         error_size)) {
        return 0;
    }
    ok = validate_object_member(&target, caller, holder_room, object_name, 1,
                                member_name, is_method, error, error_size);
    zsharp_program_free(&target);
    free(source_path);
    return ok;
}

static int validate_file_function(const ZSharpProgram *program,
                                  const ZSharpSettings *settings,
                                  const ZSharpRoom *caller,
                                  const char *project_root,
                                  const char *file, const char *room_name,
                                   const char *function_name,
                                   uint32_t argument_count,
                                   int requires_value,
                                   const char *outcome_name, char *error,
                                   size_t error_size) {
    ZSharpProgram target;
    char *source_path = NULL;
    const ZSharpRoom *target_room;
    const ZSharpFunction *function;
    size_t outcome_index;
    int has_outcome = 0;
    int selects_outcome = outcome_name != NULL && outcome_name[0] != '\0';
    int cross_file = strcmp(file, program->source_name) != 0;
    int ok = 0;
    if (!require_file_import(program, settings, caller, file, error,
                             error_size)) {
        return 0;
    }
    if (!cross_file) {
        target_room = model_find_room(program, room_name);
    } else {
        if (!load_model_file(project_root, file, &target, &source_path, error,
                             error_size)) {
            return 0;
        }
        target_room = model_find_room(&target, room_name);
    }
    function = target_room == NULL
        ? NULL
        : model_find_function(target_room, function_name);
    if (target_room == NULL || function == NULL) {
        snprintf(error, error_size,
                 "Function.call could not resolve '%s.%s.%s'", file,
                 room_name, function_name);
    } else if (!model_room_visible(target_room, caller, cross_file) ||
               (!function->is_public && target_room != caller)) {
        snprintf(error, error_size,
                 "Function.call target '%s.%s' is not visible", room_name,
                 function_name);
    } else if (function->parameter_count != argument_count) {
        snprintf(error, error_size,
                 "function '%s.%s' expects %zu argument(s), but received %u",
                 room_name, function_name, function->parameter_count,
                 argument_count);
    } else if (selects_outcome &&
               function->return_type != ZRETURN_VOID) {
        snprintf(error, error_size,
                 "only a brain can select named outcome '%s'", outcome_name);
    } else {
        for (outcome_index = 0; outcome_index < function->outcome_count;
             outcome_index++) {
            if (strcmp(function->outcome_names[outcome_index],
                       selects_outcome ? outcome_name : "") == 0) {
                has_outcome = 1;
                break;
            }
        }
        if (selects_outcome && !has_outcome) {
            snprintf(error, error_size,
                     "brain '%s.%s' has no named if '%s'", room_name,
                     function_name, outcome_name);
        } else if (requires_value && function->return_type == ZRETURN_VOID &&
                   !selects_outcome) {
        snprintf(error, error_size,
                     "brain '%s.%s' requires a named if selector",
                     room_name, function_name);
        } else {
            ok = 1;
        }
    }
    if (cross_file) {
        zsharp_program_free(&target);
        free(source_path);
    }
    return ok;
}

static int validate_instruction(const ZSharpProgram *program,
                                const ZSharpSettings *settings,
                                const ZSharpRoom *room,
                                const ZSharpInstruction *instruction,
                                const char *project_root, char *error,
                                size_t error_size) {
    char *storage = NULL;
    char *parts[5] = {0};
    size_t count = 0;
    int ok = 1;
    if (instruction->op == ZOP_CALL_QUALIFIED ||
        instruction->op == ZOP_CALL_QUALIFIED_VALUE) {
        if (instruction->operand != NULL && instruction->operand[0] != '\0') {
            return require_project_import(room, settings, instruction->operand,
                                          instruction->call_file, error,
                                          error_size);
        }
        return validate_file_function(
            program, settings, room, project_root, instruction->call_file,
            instruction->call_room, instruction->call_function,
            instruction->argument_count,
            instruction->op == ZOP_CALL_QUALIFIED_VALUE,
            instruction->call_outcome, error, error_size);
    }
    if (instruction->op != ZOP_LOAD_PATH &&
        instruction->op != ZOP_STORE_PATH &&
        instruction->op != ZOP_SET_MEMBER_PATH &&
        instruction->op != ZOP_CALL_METHOD_PATH) {
        return 1;
    }
    if (!split_model_path(instruction->operand, &storage, parts, 5, &count)) {
        snprintf(error, error_size, "invalid qualified path '%s'",
                 instruction->operand);
        return 0;
    }
    if (instruction->op == ZOP_SET_MEMBER_PATH ||
        instruction->op == ZOP_CALL_METHOD_PATH) {
        int is_method = instruction->op == ZOP_CALL_METHOD_PATH;
        if (count == 2) {
            ok = validate_object_member(
                program, room, parts[0], parts[1], 0,
                instruction->call_function, is_method, error, error_size);
        } else if (count == 3) {
            ok = validate_file_object_member(
                program, settings, room, project_root, parts[0], parts[1], parts[2],
                instruction->call_function, is_method, error, error_size);
        } else if (count == 4) {
            ok = require_project_import(room, settings, parts[0], parts[1], error,
                                        error_size);
        }
        free(storage);
        return ok;
    }
    if (count == 2) {
        const ZSharpVariable *base = model_find_variable(room, parts[0]);
        if (base != NULL && base->type == ZVALUE_OBJECT) {
            const ZSharpRoom *class_room =
                model_find_room(program, base->object_type);
            const ZSharpVariable *field = class_room == NULL
                ? NULL
                : model_find_variable(class_room, parts[1]);
            if (field == NULL) {
                snprintf(error, error_size,
                         "object '%s' has no field named '%s'", parts[0],
                         parts[1]);
                ok = 0;
            }
        } else {
            ok = validate_variable_access(program, room, parts[0], parts[1], 0,
                                          NULL, error, error_size);
        }
    } else if (count == 3) {
        if (model_find_room(program, parts[0]) != NULL) {
            ok = validate_object_member(program, room, parts[0], parts[1], 0,
                                        parts[2], 0, error, error_size);
        } else {
            ok = validate_file_variable(program, settings, room, project_root, parts[0],
                                        parts[1], parts[2], error, error_size);
        }
    } else if (count == 4) {
        if (import_matches_project_file(room, parts[0], parts[1])) {
            ok = 1;
        } else {
            ok = validate_file_object_member(
                program, settings, room, project_root, parts[0], parts[1], parts[2],
                parts[3], 0, error, error_size);
        }
    } else if (count == 5) {
        ok = require_project_import(room, settings, parts[0], parts[1], error,
                                    error_size);
    }
    free(storage);
    return ok;
}

static int function_knows_name(const ZSharpRoom *room,
                               const ZSharpFunction *function,
                               size_t instruction_index, const char *name) {
    size_t index;
    if (model_find_variable(room, name) != NULL) return 1;
    for (index = 0; index < function->parameter_count; index++) {
        if (strcmp(function->parameters[index].name, name) == 0) return 1;
    }
    for (index = 0; index < instruction_index; index++) {
        const ZSharpInstruction *instruction =
            &function->instructions[index];
        if ((instruction->op == ZOP_STORE_LOCAL ||
             instruction->op == ZOP_STORE_LOCAL_TEXT) &&
            strcmp(instruction->operand, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int program_has_method(const ZSharpProgram *program,
                              const char *name) {
    size_t room_index;
    for (room_index = 0; room_index < program->room_count; room_index++) {
        if (model_find_function(&program->rooms[room_index], name) != NULL) {
            return 1;
        }
    }
    return 0;
}

static int validate_simple_name(const ZSharpProgram *program,
                                const ZSharpRoom *room,
                                const ZSharpFunction *function,
                                size_t instruction_index, char *error,
                                size_t error_size) {
    const ZSharpInstruction *instruction =
        &function->instructions[instruction_index];
    if (instruction->op == ZOP_LOAD_NAME ||
        instruction->op == ZOP_STORE_NAME) {
        if (!function_knows_name(room, function, instruction_index,
                                 instruction->operand)) {
            snprintf(error, error_size,
                     "brain '%s.%s' has no value named '%s'",
                     room->qualified_name, function->name,
                     instruction->operand);
            return 0;
        }
        if (function->is_horde) {
            const ZSharpVariable *variable =
                model_find_variable(room, instruction->operand);
            if (variable != NULL && !variable->is_horde) {
                snprintf(error, error_size,
                         "horde function '%s.%s' cannot access instance "
                         "field '%s'",
                         room->qualified_name, function->name,
                         instruction->operand);
                return 0;
            }
        }
    } else if (instruction->op == ZOP_STORE_GLOBAL ||
               instruction->op == ZOP_STORE_FIELD) {
        if (model_find_variable(room, instruction->operand) == NULL) {
            snprintf(error, error_size,
                     "room '%s' has no field named '%s'",
                     room->qualified_name, instruction->operand);
            return 0;
        }
    } else if (instruction->op == ZOP_CALL_METHOD &&
               !program_has_method(program, instruction->operand)) {
        snprintf(error, error_size,
                 "no room in file '%s' has a method named '%s'",
                 program->source_name, instruction->operand);
        return 0;
    }
    return 1;
}

int zsharp_project_validate(const ZSharpProgram *program,
                            const ZSharpSettings *settings,
                            const char *project_root, char *error,
                            size_t error_size) {
    size_t room_index;
    for (room_index = 0; room_index < program->room_count; room_index++) {
        const ZSharpRoom *room = &program->rooms[room_index];
        size_t variable_index;
        size_t function_index;
        for (variable_index = 0; variable_index < room->variable_count;
             variable_index++) {
            const ZSharpVariable *variable = &room->variables[variable_index];
            const ZSharpRoom *object_room;
            if (variable->type != ZVALUE_OBJECT &&
                variable->type != ZVALUE_OBJECT_ARRAY) continue;
            object_room = model_find_room(
                program, variable->type == ZVALUE_OBJECT
                             ? variable->object_type
                             : variable->array_object_type);
            if (object_room != NULL && object_room->is_horde) {
                snprintf(error, error_size,
                         "horde room '%s' cannot be created with new",
                         object_room->name);
                return 0;
            }
        }
        for (function_index = 0; function_index < room->function_count;
             function_index++) {
            const ZSharpFunction *function = &room->functions[function_index];
            size_t instruction_index;
            for (instruction_index = 0;
                 instruction_index < function->instruction_count;
                 instruction_index++) {
                if (!validate_simple_name(
                        program, room, function, instruction_index, error,
                        error_size) ||
                    !validate_instruction(
                        program, settings, room,
                        &function->instructions[instruction_index],
                        project_root, error, error_size)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}
