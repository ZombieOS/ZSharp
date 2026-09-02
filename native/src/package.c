#define _CRT_SECURE_NO_WARNINGS

#include "package.h"

#include "game_model.h"

#include "hash.h"
#include "project.h"
#include "settings.h"
#include "zsharp.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define PACKAGE_MAGIC "ZSPKG1\r\n"
#define PACKAGE_MAGIC_SIZE 8
#define PACKAGE_FORMAT 1u
#define PACKAGE_MAX_FILES 100000u
#define PACKAGE_MAX_TEXT (1024u * 1024u)
#define PACKAGE_MAX_TOTAL (32ull * 1024ull * 1024ull * 1024ull)
#define SOURCE_ZIP_COMMENT "ZSHARP-UNBYTECODED-1"

typedef struct PackageFile {
    char *relative;
    char *absolute;
    uint64_t size;
    unsigned char hash[ZSHARP_SHA256_SIZE];
    uint32_t zip_crc;
    uint32_t zip_offset;
} PackageFile;

typedef struct PackageFileList {
    PackageFile *items;
    size_t count;
    size_t capacity;
} PackageFileList;

static int make_directories(const char *path, char *error,
                            size_t error_size);
static int safe_relative_path(const char *path);

static void package_error(char *error, size_t error_size,
                          const char *format, ...) {
    va_list arguments;
    if (error == NULL || error_size == 0) return;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static char *join_path(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    int separator = left_length > 0 && left[left_length - 1] != '/' &&
                    left[left_length - 1] != '\\';
    char *result = (char *)malloc(left_length + (size_t)separator +
                                  right_length + 1);
    if (result == NULL) return NULL;
    memcpy(result, left, left_length);
    if (separator) {
#ifdef _WIN32
        result[left_length++] = '\\';
#else
        result[left_length++] = '/';
#endif
    }
    memcpy(result + left_length, right, right_length + 1);
    return result;
}

static char *append_relative(const char *left, const char *right) {
    size_t left_length = left == NULL ? 0 : strlen(left);
    size_t right_length = strlen(right);
    char *result = (char *)malloc(left_length + (left_length != 0) +
                                  right_length + 1);
    if (result == NULL) return NULL;
    if (left_length != 0) {
        memcpy(result, left, left_length);
        result[left_length++] = '/';
    }
    memcpy(result + left_length, right, right_length + 1);
    return result;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int ignored_directory(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "Packages") == 0 || strcmp(name, ".gradle") == 0 ||
           strcmp(name, ".idea") == 0;
}

static int ignored_file(const char *name) {
    return ends_with(name, ".zapp") || ends_with(name, ".zgame") ||
           ends_with(name, ".tmp");
}

static int get_file_size(const char *path, uint64_t *size) {
#ifdef _WIN32
    struct _stat64 status;
    if (_stat64(path, &status) != 0 || status.st_size < 0) return 0;
    *size = (uint64_t)status.st_size;
#else
    struct stat status;
    if (stat(path, &status) != 0 || status.st_size < 0) return 0;
    *size = (uint64_t)status.st_size;
#endif
    return 1;
}

static void file_list_free(PackageFileList *list) {
    size_t index;
    for (index = 0; index < list->count; index++) {
        free(list->items[index].relative);
        free(list->items[index].absolute);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int file_list_append(PackageFileList *list, char *relative,
                            char *absolute, char *error, size_t error_size) {
    PackageFile *resized;
    PackageFile *item;
    if (list->count >= PACKAGE_MAX_FILES) {
        package_error(error, error_size, "package has more than %u files",
                      PACKAGE_MAX_FILES);
        return 0;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 32 : list->capacity * 2;
        resized = (PackageFile *)realloc(list->items,
                                         capacity * sizeof(*resized));
        if (resized == NULL) {
            package_error(error, error_size, "out of memory");
            return 0;
        }
        list->items = resized;
        list->capacity = capacity;
    }
    item = &list->items[list->count];
    memset(item, 0, sizeof(*item));
    item->relative = relative;
    item->absolute = absolute;
    if (!get_file_size(absolute, &item->size) ||
        !zsharp_sha256_file(absolute, item->hash)) {
        package_error(error, error_size, "could not read '%s'", absolute);
        return 0;
    }
    list->count++;
    return 1;
}

#ifdef _WIN32
static int collect_files(const char *root, const char *relative,
                         PackageFileList *list, char *error,
                         size_t error_size) {
    char *directory = relative[0] == '\0' ? copy_text(root)
                                           : join_path(root, relative);
    char *pattern;
    WIN32_FIND_DATAA entry;
    HANDLE search;
    int ok = 1;
    if (directory == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    pattern = join_path(directory, "*");
    if (pattern == NULL) {
        free(directory);
        package_error(error, error_size, "out of memory");
        return 0;
    }
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        free(directory);
        package_error(error, error_size, "could not enumerate '%s'", root);
        return 0;
    }
    do {
        char *child_relative;
        char *child_absolute;
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) continue;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            ignored_directory(entry.cFileName)) continue;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            ignored_file(entry.cFileName)) continue;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;
        child_relative = append_relative(relative, entry.cFileName);
        child_absolute = join_path(root, child_relative == NULL ? "" : child_relative);
        if (child_relative == NULL || child_absolute == NULL) {
            free(child_relative);
            free(child_absolute);
            package_error(error, error_size, "out of memory");
            ok = 0;
            break;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = collect_files(root, child_relative, list, error, error_size);
            free(child_relative);
            free(child_absolute);
        } else if (!file_list_append(list, child_relative, child_absolute,
                                     error, error_size)) {
            free(child_relative);
            free(child_absolute);
            ok = 0;
        }
    } while (ok && FindNextFileA(search, &entry));
    FindClose(search);
    free(directory);
    return ok;
}
#else
static int collect_files(const char *root, const char *relative,
                         PackageFileList *list, char *error,
                         size_t error_size) {
    char *directory = relative[0] == '\0' ? copy_text(root)
                                           : join_path(root, relative);
    DIR *stream;
    struct dirent *entry;
    int ok = 1;
    if (directory == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    stream = opendir(directory);
    if (stream == NULL) {
        package_error(error, error_size, "could not enumerate '%s'", directory);
        free(directory);
        return 0;
    }
    while (ok && (entry = readdir(stream)) != NULL) {
        char *child_relative;
        char *child_absolute;
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        child_relative = append_relative(relative, entry->d_name);
        child_absolute = join_path(root, child_relative == NULL ? "" : child_relative);
        if (child_relative == NULL || child_absolute == NULL) {
            free(child_relative);
            free(child_absolute);
            package_error(error, error_size, "out of memory");
            ok = 0;
            break;
        }
        if (lstat(child_absolute, &status) != 0 || S_ISLNK(status.st_mode)) {
            free(child_relative);
            free(child_absolute);
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            if (!ignored_directory(entry->d_name))
                ok = collect_files(root, child_relative, list, error, error_size);
            free(child_relative);
            free(child_absolute);
        } else if (S_ISREG(status.st_mode) && !ignored_file(entry->d_name)) {
            if (!file_list_append(list, child_relative, child_absolute,
                                  error, error_size)) {
                free(child_relative);
                free(child_absolute);
                ok = 0;
            }
        } else {
            free(child_relative);
            free(child_absolute);
        }
    }
    closedir(stream);
    free(directory);
    return ok;
}
#endif

static int compare_files(const void *left, const void *right) {
    return strcmp(((const PackageFile *)left)->relative,
                  ((const PackageFile *)right)->relative);
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)value,
                              (unsigned char)(value >> 8),
                              (unsigned char)(value >> 16),
                              (unsigned char)(value >> 24)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)value,
                              (unsigned char)(value >> 8)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_u64(FILE *file, uint64_t value) {
    unsigned char bytes[8];
    size_t index;
    for (index = 0; index < 8; index++)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char bytes[4];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return 0;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 1;
}

static int read_u16(FILE *file, uint16_t *value) {
    unsigned char bytes[2];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return 0;
    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return 1;
}

static int read_u64(FILE *file, uint64_t *value) {
    unsigned char bytes[8];
    size_t index;
    uint64_t result = 0;
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return 0;
    for (index = 0; index < 8; index++) result |= (uint64_t)bytes[index] << (index * 8);
    *value = result;
    return 1;
}

static int write_text(FILE *file, const char *text) {
    size_t length = strlen(text);
    return length <= UINT32_MAX && write_u32(file, (uint32_t)length) &&
           fwrite(text, 1, length, file) == length;
}

static char *read_text(FILE *file) {
    uint32_t length;
    char *text;
    if (!read_u32(file, &length) || length > PACKAGE_MAX_TEXT) return NULL;
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) return NULL;
    if (fread(text, 1, length, file) != length) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    return text;
}

static int copy_file_to_stream(const char *path, FILE *output) {
    FILE *input = fopen(path, "rb");
    unsigned char buffer[64 * 1024];
    size_t count;
    if (input == NULL) return 0;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            fclose(input);
            return 0;
        }
    }
    if (ferror(input)) {
        fclose(input);
        return 0;
    }
    return fclose(input) == 0;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data,
                             size_t length) {
    size_t index;
    crc = ~crc;
    for (index = 0; index < length; index++) {
        unsigned bit;
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^
                  (0xedb88320u & (uint32_t)-(int)(crc & 1u));
    }
    return ~crc;
}

static int crc32_file(const char *path, uint32_t *crc) {
    FILE *file = fopen(path, "rb");
    unsigned char buffer[64 * 1024];
    size_t count;
    uint32_t result = 0;
    if (file == NULL) return 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        result = crc32_update(result, buffer, count);
    if (ferror(file) || fclose(file) != 0) return 0;
    *crc = result;
    return 1;
}

static int validate_sources(const PackageFileList *files,
                            const ZSharpSettings *settings,
                            const char *root, char *error,
                            size_t error_size) {
    size_t index;
    for (index = 0; index < files->count; index++) {
        ZSharpProgram program;
        ZSharpDiagnostic diagnostic;
        char parse_error[512] = {0};
        if (!ends_with(files->items[index].relative, ZSHARP_SOURCE_EXTENSION))
            continue;
        if (!zsharp_project_parse_file(files->items[index].absolute, &program,
                                       &diagnostic, parse_error,
                                       sizeof(parse_error))) {
            if (diagnostic.message[0] != '\0')
                package_error(error, error_size, "%s:%u:%u: %s",
                              files->items[index].relative, diagnostic.line,
                              diagnostic.column, diagnostic.message);
            else package_error(error, error_size, "%s", parse_error);
            return 0;
        }
        if (!zsharp_project_validate(&program, settings, root, error,
                                     error_size)) {
            zsharp_program_free(&program);
            return 0;
        }
        zsharp_program_free(&program);
    }
    return 1;
}

static char *find_game_startup(const PackageFileList *files, char *error,
                               size_t error_size) {
    char *first_2d = NULL;
    char *first_3d = NULL;
    size_t index;
    for (index = 0; index < files->count; index++) {
        ZSharpProgram program;
        ZSharpDiagnostic diagnostic;
        char parse_error[512] = {0};
        char **candidate;
        if (!ends_with(files->items[index].relative,
                       ZSHARP_SOURCE_EXTENSION))
            continue;
        if (!zsharp_project_parse_file(files->items[index].absolute, &program,
                                       &diagnostic, parse_error,
                                       sizeof(parse_error))) {
            free(first_2d);
            free(first_3d);
            package_error(error, error_size, "%s",
                          diagnostic.message[0] != '\0'
                              ? diagnostic.message
                              : parse_error);
            return NULL;
        }
        candidate = program.script_type == ZSCRIPT_3D
                        ? &first_3d
                        : program.script_type == ZSCRIPT_2D ? &first_2d
                                                            : NULL;
        if (candidate != NULL &&
            (*candidate == NULL ||
             strcmp(files->items[index].relative, *candidate) < 0)) {
            char *replacement = copy_text(files->items[index].relative);
            if (replacement == NULL) {
                zsharp_program_free(&program);
                free(first_2d);
                free(first_3d);
                package_error(error, error_size, "out of memory");
                return NULL;
            }
            free(*candidate);
            *candidate = replacement;
        }
        zsharp_program_free(&program);
    }
    if (first_3d != NULL) {
        free(first_2d);
        return first_3d;
    }
    if (first_2d != NULL) return first_2d;
    package_error(error, error_size,
                  "game packages require at least one "
                  "zsharp = type.script:2D or type.script:3D file");
    return NULL;
}

static int validate_game_objects(const PackageFileList *files,
                                 const char *root,
                                 const char *startup_relative,
                                 char *error, size_t error_size) {
    size_t index;
    for (index = 0; index < files->count; index++) {
        if (strcmp(files->items[index].relative, startup_relative) == 0) {
            ZSharpProgram program;
            ZSharpDiagnostic diagnostic;
            char parse_error[512] = {0};
            int is_3d;
            int ok;
            if (!zsharp_project_parse_file(files->items[index].absolute,
                                           &program, &diagnostic,
                                           parse_error,
                                           sizeof(parse_error))) {
                package_error(error, error_size, "%s",
                              diagnostic.message[0] != '\0'
                                  ? diagnostic.message : parse_error);
                return 0;
            }
            is_3d = program.script_type == ZSCRIPT_3D;
            zsharp_program_free(&program);
            ok = zsharp_game_model_validate(root, is_3d, error, error_size);
            return ok;
        }
    }
    package_error(error, error_size,
                  "could not find the game startup while validating objects");
    return 0;
}

static int append_startup_bytecode(PackageFileList *files,
                                   const ZSharpSettings *settings,
                                   const char *root,
                                   const char *startup_relative,
                                   const char *output_path,
                                   char **temporary_path, char *error,
                                   size_t error_size) {
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic;
    unsigned char identity[ZSHARP_SHA256_SIZE];
    unsigned char build_hash[ZSHARP_SHA256_SIZE];
    char *startup = NULL;
    char *temporary = NULL;
    char *relative = NULL;
    char *owned_absolute = NULL;
    size_t temporary_length;
    int ok = 0;
    if (temporary_path != NULL) *temporary_path = NULL;
    startup = join_path(root, startup_relative);
    if (startup == NULL) goto memory_error;
    if (!zsharp_project_parse_file(startup, &program, &diagnostic, error,
                                   error_size)) {
        if (diagnostic.message[0] != '\0')
            package_error(error, error_size, "%s:%u:%u: %s",
                          startup_relative, diagnostic.line,
                          diagnostic.column, diagnostic.message);
        goto done;
    }
    if (!zsharp_project_validate(&program, settings, root, error,
                                 error_size)) {
        zsharp_program_free(&program);
        goto done;
    }
    free(program.source_name);
    program.source_name = copy_text(startup_relative);
    if (program.source_name == NULL) {
        zsharp_program_free(&program);
        goto memory_error;
    }
    temporary_length = strlen(output_path) + 17;
    temporary = (char *)malloc(temporary_length);
    if (temporary == NULL) {
        zsharp_program_free(&program);
        goto memory_error;
    }
    snprintf(temporary, temporary_length, "%s.startup.zbc.tmp", output_path);
    if (!zsharp_bytecode_write(temporary, &program, settings->project_id,
                               identity, build_hash, error, error_size)) {
        zsharp_program_free(&program);
        goto done;
    }
    zsharp_program_free(&program);
    relative = copy_text(ZSHARP_PACKAGE_STARTUP_BYTECODE);
    owned_absolute = copy_text(temporary);
    if (relative == NULL || owned_absolute == NULL) goto memory_error;
    if (!file_list_append(files, relative, owned_absolute, error, error_size))
        goto done;
    relative = NULL;
    owned_absolute = NULL;
    if (temporary_path != NULL) {
        *temporary_path = temporary;
        temporary = NULL;
    }
    ok = 1;
    goto done;
memory_error:
    package_error(error, error_size, "out of memory");
done:
    if (!ok && temporary != NULL) remove(temporary);
    free(startup);
    free(temporary);
    free(relative);
    free(owned_absolute);
    return ok;
}

void zsharp_package_info_free(ZSharpPackageInfo *info) {
    if (info == NULL) return;
    free(info->project_id);
    free(info->project_name);
    memset(info, 0, sizeof(*info));
}

static int read_header(FILE *file, ZSharpPackageInfo *info, char *error,
                       size_t error_size) {
    unsigned char magic[PACKAGE_MAGIC_SIZE];
    uint32_t format;
    uint32_t kind;
    size_t index;
    memset(info, 0, sizeof(*info));
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, PACKAGE_MAGIC, sizeof(magic)) != 0 ||
        !read_u32(file, &format) || format != PACKAGE_FORMAT ||
        !read_u32(file, &kind) ||
        (kind != ZSHARP_PACKAGE_APP && kind != ZSHARP_PACKAGE_GAME)) {
        package_error(error, error_size, "not a supported Z# package");
        return 0;
    }
    info->kind = (ZSharpPackageKind)kind;
    for (index = 0; index < 4; index++) {
        if (!read_u32(file, &info->version[index])) goto truncated;
    }
    info->project_id = read_text(file);
    info->project_name = read_text(file);
    if (info->project_id == NULL || info->project_name == NULL ||
        !read_u32(file, &info->file_count) ||
        info->file_count == 0 || info->file_count > PACKAGE_MAX_FILES) {
        goto truncated;
    }
    return 1;
truncated:
    zsharp_package_info_free(info);
    package_error(error, error_size, "Z# package header is truncated or invalid");
    return 0;
}

static uint16_t zip_u16(const unsigned char *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t zip_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int zip_eocd(FILE *file, uint16_t *entry_count,
                    uint32_t *central_offset, char *error,
                    size_t error_size) {
    long file_size;
    size_t search_size;
    unsigned char *tail;
    size_t position;
    size_t comment_length = strlen(SOURCE_ZIP_COMMENT);
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 22) {
        package_error(error, error_size,
                      "unbytecoded package has no ZIP directory");
        return 0;
    }
    search_size = (size_t)file_size;
    if (search_size > 22u + UINT16_MAX) search_size = 22u + UINT16_MAX;
    tail = (unsigned char *)malloc(search_size);
    if (tail == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    if (fseek(file, file_size - (long)search_size, SEEK_SET) != 0 ||
        fread(tail, 1, search_size, file) != search_size) {
        free(tail);
        package_error(error, error_size,
                      "could not read the unbytecoded package directory");
        return 0;
    }
    position = search_size - 22;
    for (;;) {
        if (zip_u32(tail + position) == 0x06054b50u) {
            uint16_t comment_size = zip_u16(tail + position + 20);
            uint16_t entries = zip_u16(tail + position + 10);
            uint32_t central_size = zip_u32(tail + position + 12);
            uint32_t offset = zip_u32(tail + position + 16);
            uint64_t eocd_absolute = (uint64_t)file_size - search_size +
                                     position;
            if (zip_u16(tail + position + 4) == 0 &&
                zip_u16(tail + position + 6) == 0 &&
                zip_u16(tail + position + 8) == entries && entries > 0 &&
                position + 22u + comment_size == search_size &&
                comment_size == comment_length &&
                memcmp(tail + position + 22, SOURCE_ZIP_COMMENT,
                       comment_length) == 0 &&
                (uint64_t)offset + central_size == eocd_absolute) {
                *entry_count = entries;
                *central_offset = offset;
                free(tail);
                return 1;
            }
        }
        if (position == 0) break;
        position--;
    }
    free(tail);
    package_error(error, error_size,
                  "not a Z# unbytecoded source package");
    return 0;
}

static int zip_local_header(FILE *file, char **relative, uint32_t *size,
                            uint32_t *crc, char *error,
                            size_t error_size) {
    uint32_t signature;
    uint16_t version;
    uint16_t flags;
    uint16_t method;
    uint16_t ignored;
    uint32_t compressed_size;
    uint16_t name_length;
    uint16_t extra_length;
    char *name;
    if (!read_u32(file, &signature) || signature != 0x04034b50u ||
        !read_u16(file, &version) || !read_u16(file, &flags) ||
        !read_u16(file, &method) || !read_u16(file, &ignored) ||
        !read_u16(file, &ignored) || !read_u32(file, crc) ||
        !read_u32(file, &compressed_size) || !read_u32(file, size) ||
        !read_u16(file, &name_length) || !read_u16(file, &extra_length)) {
        package_error(error, error_size,
                      "unbytecoded package has a truncated ZIP entry");
        return 0;
    }
    if (version > 20 || (flags & ~0x0800u) != 0 || method != 0 ||
        compressed_size != *size || name_length == 0 ||
        *size > (uint32_t)LONG_MAX) {
        package_error(error, error_size,
                      "unbytecoded package uses unsupported ZIP features");
        return 0;
    }
    name = (char *)malloc((size_t)name_length + 1);
    if (name == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    if (fread(name, 1, name_length, file) != name_length ||
        fseek(file, extra_length, SEEK_CUR) != 0) {
        free(name);
        package_error(error, error_size,
                      "unbytecoded package has a truncated ZIP entry");
        return 0;
    }
    name[name_length] = '\0';
    if (!safe_relative_path(name) || name[name_length - 1] == '/') {
        free(name);
        package_error(error, error_size,
                      "unbytecoded package contains an unsafe ZIP path");
        return 0;
    }
    *relative = name;
    return 1;
}

static ZSharpPackageKind package_kind(const char *path) {
    if (ends_with(path, ".zapp")) return ZSHARP_PACKAGE_APP;
    if (ends_with(path, ".zgame")) return ZSHARP_PACKAGE_GAME;
    return 0;
}

static int read_source_zip_info(FILE *file, const char *package_path,
                                ZSharpPackageInfo *info, char *error,
                                size_t error_size) {
    uint16_t entry_count;
    uint32_t central_offset;
    uint16_t index;
    int settings_found = 0;
    ZSharpSettings settings;
    ZSharpPackageKind kind = package_kind(package_path);
    memset(info, 0, sizeof(*info));
    zsharp_settings_init(&settings);
    if (kind == 0 ||
        !zip_eocd(file, &entry_count, &central_offset, error, error_size) ||
        fseek(file, 0, SEEK_SET) != 0) return 0;
    for (index = 0; index < entry_count; index++) {
        char *relative = NULL;
        uint32_t size;
        uint32_t expected_crc;
        if (!zip_local_header(file, &relative, &size, &expected_crc, error,
                              error_size)) goto failed;
        if (strcmp(relative, ZSHARP_SETTINGS_FILE) == 0) {
            char *source;
            ZSharpDiagnostic diagnostic;
            uint32_t actual_crc;
            if (settings_found || size > PACKAGE_MAX_TEXT) {
                free(relative);
                package_error(error, error_size,
                              "unbytecoded package has invalid project settings");
                goto failed;
            }
            source = (char *)malloc((size_t)size + 1);
            if (source == NULL) {
                free(relative);
                package_error(error, error_size, "out of memory");
                goto failed;
            }
            if (fread(source, 1, size, file) != size) {
                free(source);
                free(relative);
                package_error(error, error_size,
                              "unbytecoded package data is truncated");
                goto failed;
            }
            source[size] = '\0';
            actual_crc = crc32_update(0, (const unsigned char *)source, size);
            if (actual_crc != expected_crc ||
                !zsharp_settings_parse_source(source, &settings, &diagnostic,
                                              error, error_size)) {
                if (actual_crc == expected_crc &&
                    diagnostic.message[0] != '\0')
                    package_error(error, error_size,
                                  "packaged project.zsettings:%u:%u: %s",
                                  diagnostic.line, diagnostic.column,
                                  diagnostic.message);
                free(source);
                free(relative);
                goto failed;
            }
            free(source);
            settings_found = 1;
        } else if (fseek(file, (long)size, SEEK_CUR) != 0) {
            free(relative);
            package_error(error, error_size,
                          "unbytecoded package data is truncated");
            goto failed;
        }
        free(relative);
    }
    if (!settings_found || ftell(file) != (long)central_offset) {
        package_error(error, error_size,
                      "unbytecoded package directory does not match its files");
        goto failed;
    }
    info->project_id = copy_text(settings.project_id);
    info->project_name = copy_text(settings.project_name);
    info->kind = kind;
    memcpy(info->version, settings.version, sizeof(info->version));
    info->file_count = entry_count;
    zsharp_settings_free(&settings);
    if (info->project_id == NULL || info->project_name == NULL) {
        zsharp_package_info_free(info);
        package_error(error, error_size, "out of memory");
        return 0;
    }
    return 1;
failed:
    zsharp_settings_free(&settings);
    zsharp_package_info_free(info);
    return 0;
}

int zsharp_package_read_info(const char *package_path, ZSharpPackageInfo *info,
                             char *error, size_t error_size) {
    FILE *file = fopen(package_path, "rb");
    unsigned char prefix[4];
    int ok;
    if (file == NULL) {
        package_error(error, error_size, "could not open '%s'", package_path);
        return 0;
    }
    if (fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix) ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        package_error(error, error_size, "package header is truncated");
        return 0;
    }
    ok = zip_u32(prefix) == 0x04034b50u
        ? read_source_zip_info(file, package_path, info, error, error_size)
        : read_header(file, info, error, error_size);
    fclose(file);
    return ok;
}

int zsharp_package_create(const char *project_path, const char *output_path,
                          char *error, size_t error_size) {
    ZSharpPackageKind kind = package_kind(output_path);
    char *root = NULL;
    char *temporary = NULL;
    char *bytecode_temporary = NULL;
    char *game_startup = NULL;
    const char *startup_relative = NULL;
    PackageFileList files = {0};
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    FILE *output = NULL;
    uint64_t total = 0;
    size_t index;
    int ok = 0;
    if (kind == 0) {
        package_error(error, error_size,
                      "package output must end in .zapp or .zgame");
        return 0;
    }
    root = zsharp_project_find_root(project_path, error, error_size);
    if (root == NULL) return 0;
    if (!zsharp_settings_load(root, &settings, &diagnostic, error, error_size)) {
        if (diagnostic.message[0] != '\0')
            package_error(error, error_size, "project.zsettings:%u:%u: %s",
                          diagnostic.line, diagnostic.column,
                          diagnostic.message);
        free(root);
        return 0;
    }
    if (!zsharp_project_validate_settings(&settings, root, error,
                                          error_size)) {
        zsharp_settings_free(&settings);
        free(root);
        return 0;
    }
    if (kind == ZSHARP_PACKAGE_APP &&
        (!settings.has_window || settings.window_startup == NULL)) {
        package_error(error, error_size,
                      "app packages require a Window Startup entry");
        zsharp_settings_free(&settings);
        free(root);
        return 0;
    }
    if (!collect_files(root, "", &files, error, error_size) ||
        !validate_sources(&files, &settings, root, error, error_size))
        goto done;
    if (kind == ZSHARP_PACKAGE_GAME) {
        game_startup = find_game_startup(&files, error, error_size);
        if (game_startup == NULL) goto done;
        if (!validate_game_objects(&files, root, game_startup, error,
                                   error_size)) goto done;
        startup_relative = game_startup;
    } else {
        startup_relative = settings.window_startup;
    }
    if (!append_startup_bytecode(&files, &settings, root, startup_relative,
                                 output_path,
                                 &bytecode_temporary, error, error_size))
        goto done;
    qsort(files.items, files.count, sizeof(*files.items), compare_files);
    for (index = 0; index < files.count; index++) {
        if (UINT64_MAX - total < files.items[index].size ||
            (total += files.items[index].size) > PACKAGE_MAX_TOTAL) {
            package_error(error, error_size, "package contents exceed 32 GiB");
            goto done;
        }
    }
    temporary = (char *)malloc(strlen(output_path) + 5);
    if (temporary == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    sprintf(temporary, "%s.tmp", output_path);
    output = fopen(temporary, "wb");
    if (output == NULL) {
        package_error(error, error_size, "could not create '%s'", temporary);
        goto done;
    }
    if (fwrite(PACKAGE_MAGIC, 1, PACKAGE_MAGIC_SIZE, output) !=
            PACKAGE_MAGIC_SIZE ||
        !write_u32(output, PACKAGE_FORMAT) || !write_u32(output, kind))
        goto write_failed;
    for (index = 0; index < 4; index++)
        if (!write_u32(output, settings.version[index])) goto write_failed;
    if (!write_text(output, settings.project_id) ||
        !write_text(output, settings.project_name) ||
        !write_u32(output, (uint32_t)files.count)) goto write_failed;
    for (index = 0; index < files.count; index++) {
        PackageFile *item = &files.items[index];
        if (!write_text(output, item->relative) ||
            !write_u64(output, item->size) ||
            fwrite(item->hash, 1, sizeof(item->hash), output) !=
                sizeof(item->hash) ||
            !copy_file_to_stream(item->absolute, output)) goto write_failed;
    }
    if (fclose(output) != 0) {
        output = NULL;
        goto write_failed_closed;
    }
    output = NULL;
#ifdef _WIN32
    if (!MoveFileExA(temporary, output_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(temporary, output_path) != 0) {
#endif
        package_error(error, error_size, "could not finalize '%s'", output_path);
        goto done;
    }
    ok = 1;
    goto done;
write_failed:
    fclose(output);
    output = NULL;
write_failed_closed:
    package_error(error, error_size, "could not write '%s'", output_path);
done:
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    if (bytecode_temporary != NULL) remove(bytecode_temporary);
    free(temporary);
    free(bytecode_temporary);
    free(game_startup);
    file_list_free(&files);
    zsharp_settings_free(&settings);
    free(root);
    return ok;
}

static int write_zip_local_header(FILE *output, const PackageFile *item) {
    size_t name_length = strlen(item->relative);
    return name_length <= UINT16_MAX &&
           write_u32(output, 0x04034b50u) && write_u16(output, 20) &&
           write_u16(output, 0x0800u) && write_u16(output, 0) &&
           write_u16(output, 0x6000u) && write_u16(output, 0x5d19u) &&
           write_u32(output, item->zip_crc) &&
           write_u32(output, (uint32_t)item->size) &&
           write_u32(output, (uint32_t)item->size) &&
           write_u16(output, (uint16_t)name_length) &&
           write_u16(output, 0) &&
           fwrite(item->relative, 1, name_length, output) == name_length;
}

static int write_zip_central_header(FILE *output, const PackageFile *item) {
    size_t name_length = strlen(item->relative);
    return name_length <= UINT16_MAX &&
           write_u32(output, 0x02014b50u) && write_u16(output, 20) &&
           write_u16(output, 20) && write_u16(output, 0x0800u) &&
           write_u16(output, 0) && write_u16(output, 0x6000u) &&
           write_u16(output, 0x5d19u) && write_u32(output, item->zip_crc) &&
           write_u32(output, (uint32_t)item->size) &&
           write_u32(output, (uint32_t)item->size) &&
           write_u16(output, (uint16_t)name_length) &&
           write_u16(output, 0) && write_u16(output, 0) &&
           write_u16(output, 0) && write_u16(output, 0) &&
           write_u32(output, 0) && write_u32(output, item->zip_offset) &&
           fwrite(item->relative, 1, name_length, output) == name_length;
}

static int create_source_zip(const char *project_path,
                             const char *output_path, char *error,
                             size_t error_size) {
    ZSharpPackageKind kind = package_kind(output_path);
    char *root = NULL;
    char *temporary = NULL;
    char *game_startup = NULL;
    PackageFileList files = {0};
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    FILE *output = NULL;
    size_t index;
    long central_offset;
    long central_end;
    size_t comment_length = strlen(SOURCE_ZIP_COMMENT);
    int ok = 0;
    root = zsharp_project_find_root(project_path, error, error_size);
    if (root == NULL) return 0;
    if (!zsharp_settings_load(root, &settings, &diagnostic, error,
                              error_size)) {
        if (diagnostic.message[0] != '\0')
            package_error(error, error_size, "project.zsettings:%u:%u: %s",
                          diagnostic.line, diagnostic.column,
                          diagnostic.message);
        free(root);
        return 0;
    }
    if (!zsharp_project_validate_settings(&settings, root, error,
                                          error_size)) {
        goto done;
    }
    if (kind == ZSHARP_PACKAGE_APP &&
        (!settings.has_window || settings.window_startup == NULL)) {
        package_error(error, error_size,
                      "app packages require a Window Startup entry");
        goto done;
    }
    if (!collect_files(root, "", &files, error, error_size) ||
        !validate_sources(&files, &settings, root, error, error_size))
        goto done;
    if (kind == ZSHARP_PACKAGE_GAME) {
        game_startup = find_game_startup(&files, error, error_size);
        if (game_startup == NULL) goto done;
        if (!validate_game_objects(&files, root, game_startup, error,
                                   error_size)) goto done;
    }
    if (files.count == 0 || files.count > UINT16_MAX) {
        package_error(error, error_size,
                      "unbytecoded packages support at most %u files",
                      UINT16_MAX);
        goto done;
    }
    qsort(files.items, files.count, sizeof(*files.items), compare_files);
    for (index = 0; index < files.count; index++) {
        if (files.items[index].size > UINT32_MAX ||
            strlen(files.items[index].relative) > UINT16_MAX ||
            !crc32_file(files.items[index].absolute,
                        &files.items[index].zip_crc)) {
            package_error(error, error_size,
                          "could not add '%s' to the unbytecoded package",
                          files.items[index].relative);
            goto done;
        }
    }
    temporary = (char *)malloc(strlen(output_path) + 5);
    if (temporary == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    sprintf(temporary, "%s.tmp", output_path);
    output = fopen(temporary, "wb");
    if (output == NULL) {
        package_error(error, error_size, "could not create '%s'", temporary);
        goto done;
    }
    for (index = 0; index < files.count; index++) {
        long offset = ftell(output);
        if (offset < 0 || (uint64_t)offset > UINT32_MAX) goto zip_too_large;
        files.items[index].zip_offset = (uint32_t)offset;
        if (!write_zip_local_header(output, &files.items[index]) ||
            !copy_file_to_stream(files.items[index].absolute, output))
            goto write_failed;
    }
    central_offset = ftell(output);
    if (central_offset < 0 || (uint64_t)central_offset > UINT32_MAX)
        goto zip_too_large;
    for (index = 0; index < files.count; index++)
        if (!write_zip_central_header(output, &files.items[index]))
            goto write_failed;
    central_end = ftell(output);
    if (central_end < central_offset ||
        (uint64_t)(central_end - central_offset) > UINT32_MAX ||
        comment_length > UINT16_MAX)
        goto zip_too_large;
    if (!write_u32(output, 0x06054b50u) || write_u16(output, 0) == 0 ||
        write_u16(output, 0) == 0 ||
        !write_u16(output, (uint16_t)files.count) ||
        !write_u16(output, (uint16_t)files.count) ||
        !write_u32(output, (uint32_t)(central_end - central_offset)) ||
        !write_u32(output, (uint32_t)central_offset) ||
        !write_u16(output, (uint16_t)comment_length) ||
        fwrite(SOURCE_ZIP_COMMENT, 1, comment_length, output) != comment_length)
        goto write_failed;
    if (fclose(output) != 0) {
        output = NULL;
        goto write_failed_closed;
    }
    output = NULL;
#ifdef _WIN32
    if (!MoveFileExA(temporary, output_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(temporary, output_path) != 0) {
#endif
        package_error(error, error_size, "could not finalize '%s'", output_path);
        goto done;
    }
    ok = 1;
    goto done;
zip_too_large:
    package_error(error, error_size,
                  "unbytecoded packages cannot exceed the classic ZIP limit");
    goto done;
write_failed:
    fclose(output);
    output = NULL;
write_failed_closed:
    package_error(error, error_size, "could not write '%s'", output_path);
done:
    if (output != NULL) fclose(output);
    if (!ok && temporary != NULL) remove(temporary);
    free(temporary);
    free(game_startup);
    file_list_free(&files);
    zsharp_settings_free(&settings);
    free(root);
    return ok;
}

static int valid_package_name(const char *name, char *error,
                              size_t error_size) {
    const unsigned char *cursor = (const unsigned char *)name;
    size_t length;
    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        package_error(error, error_size, "package filename cannot be empty");
        return 0;
    }
    for (; *cursor != '\0'; cursor++) {
        if (*cursor < 32 || strchr("/\\:*?\"<>|", (int)*cursor) != NULL) {
            package_error(error, error_size,
                          "package filename '%s' contains an invalid character",
                          name);
            return 0;
        }
    }
    length = strlen(name);
    if (name[length - 1] == ' ' || name[length - 1] == '.') {
        package_error(error, error_size,
                      "package filename cannot end with a space or period");
        return 0;
    }
    if (ends_with(name, ".zapp") || ends_with(name, ".zgame")) {
        package_error(error, error_size,
                      "use the filename without .zapp or .zgame");
        return 0;
    }
    return 1;
}

int zsharp_package_create_named(const char *project_path,
                                ZSharpPackageKind kind,
                                const char *package_name,
                                char **output_path, char *error,
                                size_t error_size) {
    const char *extension;
    char *root = NULL;
    char *packages = NULL;
    char *filename = NULL;
    char *destination = NULL;
    size_t filename_length;
    int ok = 0;
    if (output_path != NULL) *output_path = NULL;
    if (kind != ZSHARP_PACKAGE_APP && kind != ZSHARP_PACKAGE_GAME) {
        package_error(error, error_size, "package type must be app or game");
        return 0;
    }
    if (!valid_package_name(package_name, error, error_size)) return 0;
    root = zsharp_project_find_root(project_path, error, error_size);
    if (root == NULL) return 0;
    packages = join_path(root, "Packages");
    if (packages == NULL ||
        !make_directories(packages, error, error_size)) goto done;
    extension = kind == ZSHARP_PACKAGE_APP ? ".zapp" : ".zgame";
    filename_length = strlen(package_name) + strlen(extension);
    filename = (char *)malloc(filename_length + 1);
    if (filename == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    snprintf(filename, filename_length + 1, "%s%s", package_name, extension);
    destination = join_path(packages, filename);
    if (destination == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    if (!zsharp_package_create(root, destination, error, error_size)) goto done;
    if (output_path != NULL) {
        *output_path = destination;
        destination = NULL;
    }
    ok = 1;
done:
    free(root);
    free(packages);
    free(filename);
    free(destination);
    return ok;
}

int zsharp_package_create_unbytecoded_named(
    const char *project_path, ZSharpPackageKind kind,
    const char *package_name, char **output_path, char *error,
    size_t error_size) {
    const char *extension;
    static const char suffix[] = "-unbytecoded";
    char *root = NULL;
    char *packages = NULL;
    char *filename = NULL;
    char *destination = NULL;
    size_t filename_length;
    int ok = 0;
    if (output_path != NULL) *output_path = NULL;
    if (kind != ZSHARP_PACKAGE_APP && kind != ZSHARP_PACKAGE_GAME) {
        package_error(error, error_size, "package type must be app or game");
        return 0;
    }
    if (!valid_package_name(package_name, error, error_size)) return 0;
    root = zsharp_project_find_root(project_path, error, error_size);
    if (root == NULL) return 0;
    packages = join_path(root, "Packages");
    if (packages == NULL ||
        !make_directories(packages, error, error_size)) goto done;
    extension = kind == ZSHARP_PACKAGE_APP ? ".zapp" : ".zgame";
    filename_length = strlen(package_name) + sizeof(suffix) - 1 +
                      strlen(extension);
    filename = (char *)malloc(filename_length + 1);
    if (filename == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    snprintf(filename, filename_length + 1, "%s%s%s", package_name, suffix,
             extension);
    destination = join_path(packages, filename);
    if (destination == NULL) {
        package_error(error, error_size, "out of memory");
        goto done;
    }
    if (!create_source_zip(root, destination, error, error_size)) goto done;
    if (output_path != NULL) {
        *output_path = destination;
        destination = NULL;
    }
    ok = 1;
done:
    free(root);
    free(packages);
    free(filename);
    free(destination);
    return ok;
}

static int safe_project_id(const char *id) {
    const unsigned char *cursor = (const unsigned char *)id;
    if (*cursor == '\0') return 0;
    for (; *cursor != '\0'; cursor++) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_')) return 0;
    }
    return 1;
}

static int safe_relative_path(const char *path) {
    const char *segment = path;
    const char *cursor;
    if (path[0] == '\0' || path[0] == '/' || path[0] == '\\' ||
        strchr(path, ':') != NULL || strchr(path, '\\') != NULL) return 0;
    for (cursor = path;; cursor++) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - segment);
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return 0;
            if (*cursor == '\0') return 1;
            segment = cursor + 1;
        }
    }
}

static char *cache_base(char *error, size_t error_size) {
    const char *base = getenv("ZSHARP_PACKAGE_CACHE");
    char *first;
    char *result;
    if (base != NULL && base[0] != '\0') {
        result = copy_text(base);
        if (result == NULL) package_error(error, error_size, "out of memory");
        return result;
    }
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base == NULL || base[0] == '\0') base = getenv("TEMP");
    if (base == NULL || base[0] == '\0') {
        package_error(error, error_size, "LOCALAPPDATA is not available");
        return NULL;
    }
    first = join_path(base, "ZombieOS");
    result = first == NULL ? NULL : join_path(first, "ZSharp\\packages");
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0') {
        package_error(error, error_size, "HOME is not available");
        return NULL;
    }
    first = join_path(base, "Library/Caches");
    result = first == NULL ? NULL : join_path(first, "zsharp/packages");
#else
    base = getenv("XDG_CACHE_HOME");
    if (base != NULL && base[0] != '\0') {
        first = copy_text(base);
    } else {
        base = getenv("HOME");
        if (base == NULL || base[0] == '\0') {
            package_error(error, error_size, "HOME is not available");
            return NULL;
        }
        first = join_path(base, ".cache");
    }
    result = first == NULL ? NULL : join_path(first, "zsharp/packages");
#endif
    free(first);
    if (result == NULL) package_error(error, error_size, "out of memory");
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

static int make_directories(const char *path, char *error,
                            size_t error_size) {
    char *copy = copy_text(path);
    char *cursor;
    if (copy == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
#ifdef _WIN32
            if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
            *cursor = '\0';
            if (!make_directory(copy)) {
                package_error(error, error_size,
                              "could not create package cache '%s'", copy);
                free(copy);
                return 0;
            }
            *cursor = '/';
        }
    }
    if (!make_directory(copy)) {
        package_error(error, error_size,
                      "could not create package cache '%s'", copy);
        free(copy);
        return 0;
    }
    free(copy);
    return 1;
}

static int make_parent_directories(const char *path, char *error,
                                   size_t error_size) {
    char *copy = copy_text(path);
    char *last = NULL;
    char *cursor;
    int ok;
    if (copy == NULL) return 0;
    for (cursor = copy; *cursor != '\0'; cursor++)
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    if (last == NULL) {
        free(copy);
        return 1;
    }
    *last = '\0';
    ok = make_directories(copy, error, error_size);
    free(copy);
    return ok;
}

static char *package_cache_root(const char *package_path,
                                const ZSharpPackageInfo *info, char *error,
                                size_t error_size) {
    unsigned char package_hash[ZSHARP_SHA256_SIZE];
    char hash_hex[ZSHARP_SHA256_SIZE * 2 + 1];
    char version[64];
    char *base;
    char *pid;
    char *version_path;
    char *result;
    if (!safe_project_id(info->project_id)) {
        package_error(error, error_size, "package PID is invalid");
        return NULL;
    }
    if (!zsharp_sha256_file(package_path, package_hash)) {
        package_error(error, error_size, "could not hash '%s'", package_path);
        return NULL;
    }
    zsharp_hash_hex(package_hash, hash_hex);
    snprintf(version, sizeof(version), "%u.%u.%u.%u", info->version[0],
             info->version[1], info->version[2], info->version[3]);
    base = cache_base(error, error_size);
    pid = base == NULL ? NULL : join_path(base, info->project_id);
    version_path = pid == NULL ? NULL : join_path(pid, version);
    result = version_path == NULL ? NULL : join_path(version_path, hash_hex);
    free(base);
    free(pid);
    free(version_path);
    if (result == NULL) package_error(error, error_size, "out of memory");
    return result;
}

static int write_entry(FILE *package, const char *target, uint64_t size,
                       const unsigned char expected[ZSHARP_SHA256_SIZE],
                       char *error, size_t error_size) {
    FILE *output;
    unsigned char buffer[64 * 1024];
    uint64_t remaining = size;
    unsigned char actual[ZSHARP_SHA256_SIZE];
    if (!make_parent_directories(target, error, error_size)) return 0;
    output = fopen(target, "wb");
    if (output == NULL) {
        package_error(error, error_size, "could not extract '%s'", target);
        return 0;
    }
    while (remaining > 0) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer)
                                                    : (size_t)remaining;
        if (fread(buffer, 1, wanted, package) != wanted ||
            fwrite(buffer, 1, wanted, output) != wanted) {
            fclose(output);
            package_error(error, error_size, "package data is truncated");
            return 0;
        }
        remaining -= wanted;
    }
    if (fclose(output) != 0 || !zsharp_sha256_file(target, actual) ||
        memcmp(actual, expected, ZSHARP_SHA256_SIZE) != 0) {
        package_error(error, error_size,
                      "package entry '%s' failed SHA-256 verification", target);
        return 0;
    }
    return 1;
}

static int write_zip_entry(FILE *package, const char *target, uint32_t size,
                           uint32_t expected_crc, char *error,
                           size_t error_size) {
    FILE *output;
    unsigned char buffer[64 * 1024];
    uint32_t remaining = size;
    uint32_t actual_crc = 0;
    if (!make_parent_directories(target, error, error_size)) return 0;
    output = fopen(target, "wb");
    if (output == NULL) {
        package_error(error, error_size, "could not extract '%s'", target);
        return 0;
    }
    while (remaining > 0) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer)
                                                    : (size_t)remaining;
        if (fread(buffer, 1, wanted, package) != wanted ||
            fwrite(buffer, 1, wanted, output) != wanted) {
            fclose(output);
            package_error(error, error_size,
                          "unbytecoded package data is truncated");
            return 0;
        }
        actual_crc = crc32_update(actual_crc, buffer, wanted);
        remaining -= (uint32_t)wanted;
    }
    if (fclose(output) != 0 || actual_crc != expected_crc) {
        package_error(error, error_size,
                      "package entry '%s' failed ZIP CRC verification", target);
        return 0;
    }
    return 1;
}

static int extract_source_zip(const char *package_path, char **project_root,
                              ZSharpPackageInfo *info, int *new_install,
                              char *error, size_t error_size) {
    FILE *package = NULL;
    char *root = NULL;
    char *marker = NULL;
    uint16_t entry_count;
    uint32_t central_offset;
    uint16_t index;
    int marker_existed = 0;
    int ok = 0;
    if (!zsharp_package_read_info(package_path, info, error, error_size))
        return 0;
    package = fopen(package_path, "rb");
    if (package == NULL ||
        !zip_eocd(package, &entry_count, &central_offset, error, error_size) ||
        entry_count != info->file_count || fseek(package, 0, SEEK_SET) != 0)
        goto done;
    root = package_cache_root(package_path, info, error, error_size);
    marker = root == NULL ? NULL : join_path(root,
                                             ".zsharp-package-complete");
    if (root == NULL || marker == NULL) goto done;
#ifdef _WIN32
    marker_existed = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
#else
    {
        struct stat marker_status;
        marker_existed = stat(marker, &marker_status) == 0 &&
                         S_ISREG(marker_status.st_mode);
    }
#endif
    if (!make_directories(root, error, error_size)) goto done;
    for (index = 0; index < entry_count; index++) {
        char *relative = NULL;
        char *target;
        uint32_t size;
        uint32_t expected_crc;
        if (!zip_local_header(package, &relative, &size, &expected_crc, error,
                              error_size)) goto done;
        target = join_path(root, relative);
        free(relative);
        if (target == NULL) {
            package_error(error, error_size, "out of memory");
            goto done;
        }
        if (!write_zip_entry(package, target, size, expected_crc, error,
                             error_size)) {
            free(target);
            goto done;
        }
        free(target);
    }
    if (ftell(package) != (long)central_offset) {
        package_error(error, error_size,
                      "unbytecoded package directory does not match its files");
        goto done;
    }
    {
        ZSharpSettings settings;
        ZSharpDiagnostic diagnostic;
        if (!zsharp_settings_load(root, &settings, &diagnostic, error,
                                  error_size)) {
            if (diagnostic.message[0] != '\0')
                package_error(error, error_size,
                              "packaged project.zsettings:%u:%u: %s",
                              diagnostic.line, diagnostic.column,
                              diagnostic.message);
            goto done;
        }
        if (strcmp(settings.project_id, info->project_id) != 0 ||
            memcmp(settings.version, info->version,
                   sizeof(info->version)) != 0 ||
            !zsharp_project_validate_settings(&settings, root, error,
                                              error_size)) {
            if (error[0] == '\0')
                package_error(error, error_size,
                              "package metadata does not match project settings");
            zsharp_settings_free(&settings);
            goto done;
        }
        zsharp_settings_free(&settings);
    }
    {
        FILE *complete = fopen(marker, "wb");
        int complete_ok = complete != NULL;
        if (complete_ok && fputs("verified\n", complete) < 0)
            complete_ok = 0;
        if (complete != NULL && fclose(complete) != 0)
            complete_ok = 0;
        if (!complete_ok) {
            package_error(error, error_size,
                          "could not finish installing the package cache");
            goto done;
        }
    }
    *project_root = root;
    if (new_install != NULL) *new_install = !marker_existed;
    root = NULL;
    ok = 1;
done:
    if (package != NULL) fclose(package);
    free(marker);
    free(root);
    if (!ok) zsharp_package_info_free(info);
    return ok;
}

int zsharp_package_extract(const char *package_path, char **project_root,
                           ZSharpPackageInfo *info, int *new_install,
                           char *error,
                           size_t error_size) {
    FILE *package = fopen(package_path, "rb");
    char *root = NULL;
    char *marker = NULL;
    uint64_t total = 0;
    uint32_t index;
    int ok = 0;
    int marker_existed = 0;
    unsigned char prefix[4];
    if (project_root != NULL) *project_root = NULL;
    if (new_install != NULL) *new_install = 0;
    memset(info, 0, sizeof(*info));
    if (package == NULL) {
        package_error(error, error_size, "could not open '%s'", package_path);
        return 0;
    }
    if (fread(prefix, 1, sizeof(prefix), package) != sizeof(prefix) ||
        fseek(package, 0, SEEK_SET) != 0) {
        fclose(package);
        package_error(error, error_size, "package header is truncated");
        return 0;
    }
    if (zip_u32(prefix) == 0x04034b50u) {
        fclose(package);
        return extract_source_zip(package_path, project_root, info,
                                  new_install, error, error_size);
    }
    if (!read_header(package, info, error, error_size)) goto done;
    root = package_cache_root(package_path, info, error, error_size);
    marker = root == NULL ? NULL : join_path(root, ".zsharp-package-complete");
    if (root == NULL || marker == NULL) goto done;
#ifdef _WIN32
    marker_existed = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
#else
    {
        struct stat marker_status;
        marker_existed = stat(marker, &marker_status) == 0 &&
                         S_ISREG(marker_status.st_mode);
    }
#endif
    if (!make_directories(root, error, error_size)) goto done;
    for (index = 0; index < info->file_count; index++) {
        char *relative = read_text(package);
        char *target;
        uint64_t size;
        unsigned char expected[ZSHARP_SHA256_SIZE];
        if (relative == NULL || !safe_relative_path(relative) ||
            !read_u64(package, &size) ||
            fread(expected, 1, sizeof(expected), package) != sizeof(expected) ||
            UINT64_MAX - total < size || (total += size) > PACKAGE_MAX_TOTAL) {
            free(relative);
            package_error(error, error_size,
                          "package contains an invalid or unsafe entry");
            goto done;
        }
        target = join_path(root, relative);
        free(relative);
        if (target == NULL) {
            package_error(error, error_size, "out of memory");
            goto done;
        }
        if (!write_entry(package, target, size, expected, error, error_size)) {
            free(target);
            goto done;
        }
        free(target);
    }
    {
        ZSharpSettings settings;
        ZSharpDiagnostic diagnostic;
        if (!zsharp_settings_load(root, &settings, &diagnostic, error,
                                  error_size)) {
            if (diagnostic.message[0] != '\0')
                package_error(error, error_size,
                              "packaged project.zsettings:%u:%u: %s",
                              diagnostic.line, diagnostic.column,
                              diagnostic.message);
            goto done;
        }
        if (strcmp(settings.project_id, info->project_id) != 0 ||
            memcmp(settings.version, info->version,
                   sizeof(info->version)) != 0 ||
            !zsharp_project_validate_settings(&settings, root, error,
                                              error_size)) {
            if (error[0] == '\0')
                package_error(error, error_size,
                              "package metadata does not match project settings");
            zsharp_settings_free(&settings);
            goto done;
        }
        zsharp_settings_free(&settings);
    }
    {
        FILE *complete = fopen(marker, "wb");
        int complete_ok = complete != NULL;
        if (complete_ok && fputs("verified\n", complete) < 0)
            complete_ok = 0;
        if (complete != NULL && fclose(complete) != 0)
            complete_ok = 0;
        if (!complete_ok) {
            package_error(error, error_size,
                          "could not finish installing the package cache");
            goto done;
        }
    }
    *project_root = root;
    if (new_install != NULL) *new_install = !marker_existed;
    root = NULL;
    ok = 1;
done:
    fclose(package);
    free(marker);
    free(root);
    if (!ok) zsharp_package_info_free(info);
    return ok;
}

#ifdef _WIN32
static int remove_tree(const char *directory, char *error,
                       size_t error_size) {
    WIN32_FIND_DATAA entry;
    char *pattern = join_path(directory, "*");
    HANDLE search;
    int ok = 1;
    if (pattern == NULL) return 0;
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char *path;
            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) continue;
            path = join_path(directory, entry.cFileName);
            if (path == NULL) { ok = 0; break; }
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                ok = remove_tree(path, error, error_size);
            else if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                ok = RemoveDirectoryA(path) != 0;
            else
                ok = DeleteFileA(path) != 0;
            free(path);
        } while (ok && FindNextFileA(search, &entry));
        FindClose(search);
    }
    if (ok) ok = RemoveDirectoryA(directory) != 0;
    if (!ok) package_error(error, error_size,
                           "could not completely remove '%s'", directory);
    return ok;
}
#else
static int remove_tree(const char *directory, char *error,
                       size_t error_size) {
    DIR *stream = opendir(directory);
    struct dirent *entry;
    int ok = 1;
    if (stream == NULL) return errno == ENOENT;
    while (ok && (entry = readdir(stream)) != NULL) {
        char *path;
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        path = join_path(directory, entry->d_name);
        if (path == NULL) { ok = 0; break; }
        if (lstat(path, &status) != 0) ok = errno == ENOENT;
        else if (S_ISDIR(status.st_mode)) ok = remove_tree(path, error, error_size);
        else ok = unlink(path) == 0;
        free(path);
    }
    closedir(stream);
    if (ok) ok = rmdir(directory) == 0;
    if (!ok) package_error(error, error_size,
                           "could not completely remove '%s'", directory);
    return ok;
}
#endif

int zsharp_package_uninstall(const char *package_path, int delete_package,
                             char *error, size_t error_size) {
    ZSharpPackageInfo info;
    char *root;
    int ok = 1;
    if (!zsharp_package_read_info(package_path, &info, error, error_size))
        return 0;
    root = package_cache_root(package_path, &info, error, error_size);
    zsharp_package_info_free(&info);
    if (root == NULL) return 0;
#ifdef _WIN32
    if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES)
        ok = remove_tree(root, error, error_size);
#else
    {
        struct stat status;
        if (lstat(root, &status) == 0) ok = remove_tree(root, error, error_size);
    }
#endif
    free(root);
    if (ok && delete_package && remove(package_path) != 0) {
        package_error(error, error_size, "could not delete '%s'", package_path);
        ok = 0;
    }
    return ok;
}
