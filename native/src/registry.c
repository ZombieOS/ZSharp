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

static char *registry_file_path(const char *filename, char *error,
                                size_t error_size) {
    const char *base;
    char *first = NULL;
    char *second = NULL;
    char *result = NULL;
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base == NULL || base[0] == '\0') {
        registry_error(error, error_size, "LOCALAPPDATA is not available",
                       NULL);
        return NULL;
    }
    first = join_path(base, "ZombieOS");
    second = first == NULL ? NULL : join_path(first, "ZSharp");
    result = second == NULL ? NULL : join_path(second, filename);
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0') {
        registry_error(error, error_size, "HOME is not available", NULL);
        return NULL;
    }
    first = join_path(base, "Library/Application Support");
    second = first == NULL ? NULL : join_path(first, "ZSharp");
    result = second == NULL ? NULL : join_path(second, filename);
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
    result = second == NULL ? NULL : join_path(second, filename);
#endif
    free(first);
    free(second);
    if (result == NULL)
        registry_error(error, error_size, "out of memory", NULL);
    return result;
}

static char *project_registry_path(char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_PROJECT_REGISTRY");
    if (override != NULL && override[0] != '\0') return copy_text(override);
    return registry_file_path("projects.registry", error, error_size);
}

static char *package_registry_path(char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_PACKAGE_REGISTRY");
    if (override != NULL && override[0] != '\0') return copy_text(override);
    return registry_file_path("installed.registry", error, error_size);
}

static char *play_stats_registry_path(char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_PLAY_STATS_REGISTRY");
    if (override != NULL && override[0] != '\0') return copy_text(override);
    return registry_file_path("playtime.registry", error, error_size);
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
                   "could not finalize registry", destination);
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
    path = project_registry_path(error, error_size);
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

static int text_is_registry_safe(const char *text) {
    return text != NULL && strchr(text, '\t') == NULL &&
           strchr(text, '\n') == NULL && strchr(text, '\r') == NULL;
}

static int package_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static char *absolute_package_path(const char *path) {
#ifdef _WIN32
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

static int installed_line_matches(char *line, const char *project_id,
                                  const char *package_path) {
    char *fields[5];
    size_t index;
    char *cursor = line;
    if (line[0] == '#') return 0;
    for (index = 0; index < 5; index++) {
        fields[index] = cursor;
        if (index < 4) {
            cursor = strchr(cursor, '\t');
            if (cursor == NULL) return 0;
            *cursor++ = '\0';
        }
    }
    fields[4][strcspn(fields[4], "\r\n")] = '\0';
    return strcmp(fields[0], project_id) == 0 ||
           (package_path != NULL && strcmp(fields[4], package_path) == 0);
}

int zsharp_registry_remember_package(const char *package_path,
                                     const ZSharpPackageInfo *info,
                                     char *error, size_t error_size) {
    char *path = NULL;
    char *temporary = NULL;
    char *absolute = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    int read_result = 0;
    int ok = 0;
    size_t path_length;
    if (info == NULL || !text_is_registry_safe(info->project_id) ||
        !text_is_registry_safe(info->project_name) ||
        !text_is_registry_safe(package_path)) {
        registry_error(error, error_size,
                       "package metadata contains tabs or newlines", NULL);
        return 0;
    }
    absolute = absolute_package_path(package_path);
    if (absolute == NULL) {
        registry_error(error, error_size,
                       "could not resolve package path", package_path);
        return 0;
    }
    path = package_registry_path(error, error_size);
    if (path == NULL || !make_parent_directories(path, error, error_size))
        goto done;
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
                       "could not write installed package registry", path);
        goto done;
    }
    fputs("# Z# installed package registry v1\n", output);
    if (input != NULL) {
        char *line = NULL;
        while ((read_result = read_line(input, &line)) == 1) {
            char *original = copy_text(line);
            if (original == NULL) {
                free(line);
                registry_error(error, error_size, "out of memory", NULL);
                goto done;
            }
            if (line[0] != '#' && !installed_line_matches(
                    line, info->project_id, absolute)) {
                fputs(original, output);
                if (original[0] != '\0' &&
                    original[strlen(original) - 1] != '\n') fputc('\n', output);
            }
            free(original);
            free(line);
        }
        if (read_result < 0 || ferror(input)) {
            registry_error(error, error_size,
                           "could not read installed package registry", path);
            goto done;
        }
        fclose(input);
        input = NULL;
    }
    fprintf(output, "%s\t%s\t%s\t%u.%u.%u.%u\t%s\n",
            info->project_id,
            info->kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
            info->project_name, info->version[0], info->version[1],
            info->version[2], info->version[3], absolute);
    if (fclose(output) != 0) {
        output = NULL;
        registry_error(error, error_size,
                       "could not finish installed package registry", path);
        goto done;
    }
    output = NULL;
    if (!replace_file(temporary, path, error, error_size)) goto done;
    ok = 1;
done:
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    free(absolute);
    free(temporary);
    free(path);
    return ok;
}

static int append_installed_package(ZSharpInstalledPackageList *packages,
                                    const ZSharpInstalledPackage *item) {
    ZSharpInstalledPackage *grown = (ZSharpInstalledPackage *)realloc(
        packages->items, (packages->count + 1) * sizeof(*grown));
    if (grown == NULL) return 0;
    packages->items = grown;
    packages->items[packages->count++] = *item;
    return 1;
}

void zsharp_registry_package_list_free(ZSharpInstalledPackageList *packages) {
    size_t index;
    if (packages == NULL) return;
    for (index = 0; index < packages->count; index++) {
        free(packages->items[index].project_name);
        free(packages->items[index].project_id);
        free(packages->items[index].package_path);
        free(packages->items[index].icon_path);
    }
    free(packages->items);
    packages->items = NULL;
    packages->count = 0;
}

int zsharp_registry_list_packages(ZSharpInstalledPackageList *packages,
                                  char *error, size_t error_size) {
    char *path;
    FILE *input;
    int read_result = 0;
    memset(packages, 0, sizeof(*packages));
    path = package_registry_path(error, error_size);
    if (path == NULL) return 0;
    input = fopen(path, "rb");
    free(path);
    if (input == NULL) return 1;
    for (;;) {
        char *line = NULL;
        char *fields[5];
        char *cursor;
        size_t index;
        unsigned version[4];
        ZSharpInstalledPackage item;
        read_result = read_line(input, &line);
        if (read_result <= 0) break;
        if (line[0] == '#') {
            free(line);
            continue;
        }
        cursor = line;
        for (index = 0; index < 5; index++) {
            fields[index] = cursor;
            if (index < 4) {
                cursor = strchr(cursor, '\t');
                if (cursor == NULL) break;
                *cursor++ = '\0';
            }
        }
        if (index != 5) {
            free(line);
            continue;
        }
        fields[4][strcspn(fields[4], "\r\n")] = '\0';
        if ((strcmp(fields[1], "app") != 0 &&
             strcmp(fields[1], "game") != 0) ||
            sscanf(fields[3], "%u.%u.%u.%u", &version[0], &version[1],
                   &version[2], &version[3]) != 4 ||
            !package_file_exists(fields[4])) {
            free(line);
            continue;
        }
        memset(&item, 0, sizeof(item));
        item.kind = strcmp(fields[1], "game") == 0
                        ? ZSHARP_PACKAGE_GAME : ZSHARP_PACKAGE_APP;
        item.project_id = copy_text(fields[0]);
        item.project_name = copy_text(fields[2]);
        memcpy(item.version, version, sizeof(version));
        item.package_path = copy_text(fields[4]);
        free(line);
        if (item.project_id == NULL || item.project_name == NULL ||
            item.package_path == NULL ||
            !append_installed_package(packages, &item)) {
            free(item.project_id);
            free(item.project_name);
            free(item.package_path);
            zsharp_registry_package_list_free(packages);
            fclose(input);
            registry_error(error, error_size, "out of memory", NULL);
            return 0;
        }
    }
    if (read_result < 0 || ferror(input)) {
        fclose(input);
        zsharp_registry_package_list_free(packages);
        registry_error(error, error_size,
                       "could not read installed package registry", NULL);
        return 0;
    }
    fclose(input);
    {
        char stats_error[256] = {0};
        char *stats_path = play_stats_registry_path(stats_error,
                                                     sizeof(stats_error));
        FILE *stats = stats_path == NULL ? NULL : fopen(stats_path, "rb");
        free(stats_path);
        if (stats != NULL) {
            for (;;) {
                char *line = NULL;
                char *first;
                char *second;
                size_t item_index;
                unsigned long long seconds;
                long long last_played;
                int status = read_line(stats, &line);
                if (status <= 0) break;
                if (line[0] == '#') {
                    free(line);
                    continue;
                }
                first = strchr(line, '\t');
                second = first == NULL ? NULL : strchr(first + 1, '\t');
                if (first == NULL || second == NULL) {
                    free(line);
                    continue;
                }
                *first++ = '\0';
                *second++ = '\0';
                second[strcspn(second, "\r\n")] = '\0';
                if (sscanf(first, "%llu", &seconds) != 1 ||
                    sscanf(second, "%lld", &last_played) != 1) {
                    free(line);
                    continue;
                }
                for (item_index = 0; item_index < packages->count;
                     item_index++) {
                    if (strcmp(packages->items[item_index].project_id,
                               line) == 0) {
                        packages->items[item_index].total_play_seconds =
                            (uint64_t)seconds;
                        packages->items[item_index].last_played =
                            (int64_t)last_played;
                        break;
                    }
                }
                free(line);
            }
            fclose(stats);
        }
    }
    return 1;
}

int zsharp_registry_record_play(const char *project_id, uint64_t seconds,
                                int64_t started_at, char *error,
                                size_t error_size) {
    char *path = NULL;
    char *temporary = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    uint64_t previous = 0;
    int read_result = 0;
    int ok = 0;
    size_t length;
    if (!text_is_registry_safe(project_id)) {
        registry_error(error, error_size, "invalid project ID", project_id);
        return 0;
    }
    path = play_stats_registry_path(error, error_size);
    if (path == NULL || !make_parent_directories(path, error, error_size))
        goto done;
    length = strlen(path);
    temporary = (char *)malloc(length + 5);
    if (temporary == NULL) {
        registry_error(error, error_size, "out of memory", NULL);
        goto done;
    }
    memcpy(temporary, path, length);
    memcpy(temporary + length, ".tmp", 5);
    input = fopen(path, "rb");
    output = fopen(temporary, "wb");
    if (output == NULL) {
        registry_error(error, error_size,
                       "could not write playtime registry", path);
        goto done;
    }
    fputs("# Z# playtime registry v1\n", output);
    if (input != NULL) {
        char *line = NULL;
        while ((read_result = read_line(input, &line)) == 1) {
            char *original = copy_text(line);
            char *first = line[0] == '#' ? NULL : strchr(line, '\t');
            char *second = first == NULL ? NULL : strchr(first + 1, '\t');
            int matches = 0;
            if (original == NULL) {
                free(line);
                registry_error(error, error_size, "out of memory", NULL);
                goto done;
            }
            if (first != NULL && second != NULL) {
                unsigned long long stored = 0;
                *first++ = '\0';
                *second = '\0';
                matches = strcmp(line, project_id) == 0;
                if (matches && sscanf(first, "%llu", &stored) == 1)
                    previous = (uint64_t)stored;
            }
            if (line[0] != '#' && !matches) {
                fputs(original, output);
                if (original[0] != '\0' &&
                    original[strlen(original) - 1] != '\n') fputc('\n', output);
            }
            free(original);
            free(line);
        }
        if (read_result < 0 || ferror(input)) {
            registry_error(error, error_size,
                           "could not read playtime registry", path);
            goto done;
        }
        fclose(input);
        input = NULL;
    }
    if (UINT64_MAX - previous < seconds) previous = UINT64_MAX;
    else previous += seconds;
    fprintf(output, "%s\t%llu\t%lld\n", project_id,
            (unsigned long long)previous, (long long)started_at);
    if (fclose(output) != 0) {
        output = NULL;
        registry_error(error, error_size,
                       "could not finish playtime registry", path);
        goto done;
    }
    output = NULL;
    if (!replace_file(temporary, path, error, error_size)) goto done;
    ok = 1;
done:
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    free(temporary);
    free(path);
    return ok;
}

int zsharp_registry_forget_package(const char *project_id,
                                   char *error, size_t error_size) {
    char *path = package_registry_path(error, error_size);
    char *temporary = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    int read_result = 0;
    int ok = 0;
    size_t length;
    if (path == NULL) return 0;
    input = fopen(path, "rb");
    if (input == NULL) {
        free(path);
        return 1;
    }
    length = strlen(path);
    temporary = (char *)malloc(length + 5);
    if (temporary == NULL) {
        registry_error(error, error_size, "out of memory", NULL);
        goto done;
    }
    memcpy(temporary, path, length);
    memcpy(temporary + length, ".tmp", 5);
    output = fopen(temporary, "wb");
    if (output == NULL) {
        registry_error(error, error_size,
                       "could not update installed package registry", path);
        goto done;
    }
    fputs("# Z# installed package registry v1\n", output);
    for (;;) {
        char *line = NULL;
        char *original;
        read_result = read_line(input, &line);
        if (read_result <= 0) break;
        original = copy_text(line);
        if (original == NULL) {
            free(line);
            registry_error(error, error_size, "out of memory", NULL);
            goto done;
        }
        if (line[0] != '#' && !installed_line_matches(line, project_id, NULL)) {
            fputs(original, output);
            if (original[0] != '\0' &&
                original[strlen(original) - 1] != '\n') fputc('\n', output);
        }
        free(original);
        free(line);
    }
    if (read_result < 0 || ferror(input) || fclose(output) != 0) {
        output = NULL;
        registry_error(error, error_size,
                       "could not finish installed package registry", path);
        goto done;
    }
    output = NULL;
    fclose(input);
    input = NULL;
    if (!replace_file(temporary, path, error, error_size)) goto done;
    ok = 1;
done:
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    free(temporary);
    free(path);
    return ok;
}
