#define _CRT_SECURE_NO_WARNINGS

#include "registry.h"

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static void registry_error(char *error, size_t error_size,
                           const char *message, const char *detail) {
    if (error == NULL || error_size == 0) return;
    if (detail == NULL)
        snprintf(error, error_size, "%s", message);
    else
        snprintf(error, error_size, "%s '%s'", message, detail);
}

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *result = (char *)malloc(length + 1);
    if (result != NULL) memcpy(result, text, length + 1);
    return result;
}

static char *join_path(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    int needs_separator = directory_length > 0 &&
                          directory[directory_length - 1] != '/' &&
                          directory[directory_length - 1] != '\\';
    char *path = (char *)malloc(directory_length + (size_t)needs_separator +
                               name_length + 1);
    if (path == NULL) return NULL;
    memcpy(path, directory, directory_length);
    if (needs_separator) {
#ifdef _WIN32
        path[directory_length++] = '\\';
#else
        path[directory_length++] = '/';
#endif
    }
    memcpy(path + directory_length, name, name_length + 1);
    return path;
}

static char *registry_path(char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_PROJECT_REGISTRY");
    const char *base;
    char *first = NULL;
    char *second = NULL;
    char *result = NULL;
    if (override != NULL && override[0] != '\0') return copy_text(override);
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base == NULL || base[0] == '\0') {
        registry_error(error, error_size, "LOCALAPPDATA is not available",
                       NULL);
        return NULL;
    }
    first = join_path(base, "ZombieOS");
    second = first == NULL ? NULL : join_path(first, "ZSharp");
    result = second == NULL ? NULL : join_path(second, "projects.registry");
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0') {
        registry_error(error, error_size, "HOME is not available", NULL);
        return NULL;
    }
    first = join_path(base, "Library/Application Support");
    second = first == NULL ? NULL : join_path(first, "ZSharp");
    result = second == NULL ? NULL : join_path(second, "projects.registry");
#else
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] != '\0') {
        first = copy_text(base);
    } else {
        base = getenv("HOME");
        if (base == NULL || base[0] == '\0') {
            registry_error(error, error_size, "HOME is not available", NULL);
            return NULL;
        }
        first = join_path(base, ".local/share");
    }
    second = first == NULL ? NULL : join_path(first, "zsharp");
    result = second == NULL ? NULL : join_path(second, "projects.registry");
#endif
    free(first);
    free(second);
    if (result == NULL)
        registry_error(error, error_size, "out of memory", NULL);
    return result;
}

static int make_directory(const char *path) {
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST;
#endif
}

static int make_parent_directories(const char *path, char *error,
                                   size_t error_size) {
    char *copy = copy_text(path);
    char *cursor;
    char *last = NULL;
    if (copy == NULL) {
        registry_error(error, error_size, "out of memory", NULL);
        return 0;
    }
    for (cursor = copy; *cursor != '\0'; cursor++)
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    if (last == NULL) {
        free(copy);
        return 1;
    }
    *last = '\0';
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
#ifdef _WIN32
            if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
            *cursor = '\0';
            if (!make_directory(copy)) {
                registry_error(error, error_size,
                               "could not create project registry directory",
                               copy);
                free(copy);
                return 0;
            }
            *cursor = '/';
        }
    }
    if (copy[0] != '\0' && !make_directory(copy)) {
        registry_error(error, error_size,
                       "could not create project registry directory", copy);
        free(copy);
        return 0;
    }
    free(copy);
    return 1;
}

/* Returns 1 for a line, 0 at EOF, and -1 on allocation failure. */
static int read_line(FILE *input, char **line) {
    size_t length = 0;
    size_t capacity = 256;
    int character;
    char *buffer = (char *)malloc(capacity);
    if (buffer == NULL) return -1;
    while ((character = fgetc(input)) != EOF) {
        if (length + 1 >= capacity) {
            char *grown;
            capacity *= 2;
            grown = (char *)realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return -1;
            }
            buffer = grown;
        }
        buffer[length++] = (char)character;
        if (character == '\n') break;
    }
    if (length == 0 && character == EOF) {
        free(buffer);
        return 0;
    }
    buffer[length] = '\0';
    *line = buffer;
    return 1;
}

static int same_entry(char *line, const char *project_id,
                      const char *settings_path) {
    char *first_tab;
    char *second_tab;
    char *old_path;
    size_t path_length;
    if (line[0] == '#') return 0;
    first_tab = strchr(line, '\t');
    if (first_tab == NULL) return 0;
    second_tab = strchr(first_tab + 1, '\t');
    if (second_tab == NULL) return 0;
    *first_tab = '\0';
    old_path = second_tab + 1;
    path_length = strlen(old_path);
    while (path_length > 0 &&
           (old_path[path_length - 1] == '\n' ||
            old_path[path_length - 1] == '\r'))
        old_path[--path_length] = '\0';
    return strcmp(line, project_id) == 0 ||
           strcmp(old_path, settings_path) == 0;
}

static int replace_file(const char *temporary, const char *destination,
                        char *error, size_t error_size) {
#ifdef _WIN32
    if (MoveFileExA(temporary, destination,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 1;
#else
    if (rename(temporary, destination) == 0) return 1;
#endif
    registry_error(error, error_size,
                   "could not finalize project registry", destination);
    return 0;
}

int zsharp_registry_register_project(const char *project_root,
                                     const ZSharpSettings *settings,
                                     char *error, size_t error_size) {
    char *path = NULL;
    char *temporary = NULL;
    char *settings_path = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    int read_result;
    int ok = 0;
    size_t path_length;
    if (strchr(project_root, '\t') != NULL ||
        strchr(project_root, '\n') != NULL ||
        strchr(project_root, '\r') != NULL) {
        registry_error(error, error_size,
                       "project paths cannot contain tabs or newlines", NULL);
        return 0;
    }
    path = registry_path(error, error_size);
    if (path == NULL || !make_parent_directories(path, error, error_size))
        goto done;
    settings_path = join_path(project_root, ZSHARP_SETTINGS_FILE);
    if (settings_path == NULL) {
        registry_error(error, error_size, "out of memory", NULL);
        goto done;
    }
    path_length = strlen(path);
    temporary = (char *)malloc(path_length + 5);
    if (temporary == NULL) {
        registry_error(error, error_size, "out of memory", NULL);
        goto done;
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, ".tmp", 5);
    input = fopen(path, "rb");
    output = fopen(temporary, "wb");
    if (output == NULL) {
        registry_error(error, error_size,
                       "could not write project registry", path);
        goto done;
    }
    fputs("# Z# project registry v1\n", output);
    if (input != NULL) {
        char *line = NULL;
        while ((read_result = read_line(input, &line)) == 1) {
            char *original = copy_text(line);
            if (original == NULL) {
                free(line);
                registry_error(error, error_size, "out of memory", NULL);
                goto done;
            }
            if (line[0] != '#' &&
                !same_entry(line, settings->project_id, settings_path)) {
                fputs(original, output);
                if (original[0] != '\0' &&
                    original[strlen(original) - 1] != '\n')
                    fputc('\n', output);
            }
            free(original);
            free(line);
            line = NULL;
        }
        if (read_result < 0) {
            registry_error(error, error_size, "out of memory", NULL);
            goto done;
        }
        if (ferror(input)) {
            registry_error(error, error_size,
                           "could not read project registry", path);
            goto done;
        }
        fclose(input);
        input = NULL;
    }
    fprintf(output, "%s\t%u.%u.%u.%u\t%s\n", settings->project_id,
            settings->version[0], settings->version[1], settings->version[2],
            settings->version[3], settings_path);
    if (fclose(output) != 0) {
        output = NULL;
        registry_error(error, error_size,
                       "could not finish writing project registry", path);
        goto done;
    }
    output = NULL;
    if (!replace_file(temporary, path, error, error_size)) goto done;
    ok = 1;
done:
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    free(path);
    free(temporary);
    free(settings_path);
    return ok;
}
