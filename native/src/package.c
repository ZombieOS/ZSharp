#define _CRT_SECURE_NO_WARNINGS

#include "package.h"
#include "package_ignore.h"
#include "package_executable.h"

#include "game_model.h"

#include "hash.h"
#include "project.h"
#include "settings.h"
#include "zsharp.h"
#include "updater.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
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
static int remove_tree(const char *directory, char *error, size_t error_size);

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
    return strstr(name, ".install-") != NULL || strcmp(name, ".git") == 0 || strcmp(name, ".godot") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "Packages") == 0 || strcmp(name, ".gradle") == 0 ||
           strcmp(name, ".idea") == 0;
}

static int ignored_file(const char *name) {
    return strcmp(name, ".zignore") == 0 || ends_with(name, ".zapp") || ends_with(name, ".zgame") ||
           ends_with(name, ".zpackage") || ends_with(name, ".zbc") ||
           strncmp(name, ".zsharp-dependency-lock-", 24) == 0 || ends_with(name, ".tmp");
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
                         const ZSharpPackageIgnore *ignore, PackageFileList *list, char *error,
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
        if (relative[0] == '\0' && strcmp(entry.cFileName, ".zsharp-dependency-complete") == 0) continue;
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
        {
            int excluded = zsharp_package_ignore_match(ignore, child_relative,
                (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            if (excluded) {
                free(child_relative); free(child_absolute);
                if (excluded < 0) { package_error(error, error_size, "out of memory applying .zignore"); ok = 0; break; }
                continue;
            }
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = collect_files(root, child_relative, ignore, list, error, error_size);
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
                         const ZSharpPackageIgnore *ignore, PackageFileList *list, char *error,
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
        if (relative[0] == '\0' && strcmp(entry->d_name, ".zsharp-dependency-complete") == 0) continue;
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
        {
            int excluded = zsharp_package_ignore_match(ignore, child_relative, S_ISDIR(status.st_mode));
            if (excluded) {
                free(child_relative); free(child_absolute);
                if (excluded < 0) { package_error(error, error_size, "out of memory applying .zignore"); ok = 0; break; }
                continue;
            }
        }
        if (S_ISDIR(status.st_mode)) {
            if (!ignored_directory(entry->d_name))
                ok = collect_files(root, child_relative, ignore, list, error, error_size);
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

static int require_packaged_file(const PackageFileList *files, const char *relative,
                                  char *error, size_t error_size) {
    size_t i;
    for (i = 0; i < files->count; i++)
        if (!strcmp(files->items[i].relative, relative)) return 1;
    package_error(error, error_size, "required package file '%s' is missing or excluded by .zignore", relative);
    return 0;
}

static int collect_package_files(const char *root, const ZSharpSettings *settings,
                                 PackageFileList *files, char *error, size_t error_size) {
    ZSharpPackageIgnore ignore = {0};
    size_t i;
    int ok = zsharp_package_ignore_load(&ignore, root, error, error_size) &&
        collect_files(root, "", &ignore, files, error, error_size);
    zsharp_package_ignore_free(&ignore);
    if (!ok || !require_packaged_file(files, "project.zsettings", error, error_size)) return 0;
    if (settings->has_window && settings->window_startup &&
        !require_packaged_file(files, settings->window_startup, error, error_size)) return 0;
    if (settings->has_window && settings->window_uninstall &&
        !require_packaged_file(files, settings->window_uninstall, error, error_size)) return 0;
    if (settings->game_start_scene &&
        !require_packaged_file(files, settings->game_start_scene, error, error_size)) return 0;
    for (i = 0; i < settings->native_target_count; i++)
        if (!require_packaged_file(files, settings->native_targets[i].start, error, error_size)) return 0;
    return 1;
}

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
    /* Reflected IEEE CRC-32; crc is the finalized checksum of prior chunks. */
    static const uint32_t table[256] = {
        0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
        0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
        0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
        0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
        0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
        0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
        0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
        0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
        0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
        0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
        0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
        0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
        0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
        0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
        0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
        0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
        0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
        0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
        0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
        0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
        0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
        0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
        0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
        0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
        0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
        0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
        0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
        0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
        0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
        0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
        0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
        0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du
    };
    size_t index;
    crc = ~crc;
    for (index = 0; index < length; index++)
        crc = (crc >> 8) ^ table[(crc ^ data[index]) & 0xffu];
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
                            const char *root, int force_game,
                            ZSharpValidationContext *validation, char *error,
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
        if (strncmp(files->items[index].relative, "Dependencies/", 13) == 0) {
            ZSharpSettings dependency_settings;
            char *dependency_root = zsharp_project_find_root(files->items[index].absolute, error, error_size);
            int valid = dependency_root && zsharp_settings_load(dependency_root, &dependency_settings, &diagnostic, error, error_size);
            if (valid) {
                valid = zsharp_project_validate(&program, &dependency_settings, dependency_root, error, error_size);
                zsharp_settings_free(&dependency_settings);
            }
            free(dependency_root);
            zsharp_program_free(&program);
            if (!valid) return 0;
            continue;
        }
        if (force_game && program.script_type == ZSCRIPT_NORMAL)
            program.script_type = ZSCRIPT_GAME;
        if (!zsharp_project_validate_with_context(&program, settings, root, validation, error,
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
    char *first_script = NULL;
    size_t index;
    for (index = 0; index < files->count; index++) {
        ZSharpProgram program;
        ZSharpDiagnostic diagnostic;
        char parse_error[512] = {0};
        if (strncmp(files->items[index].relative, "Dependencies/", 13) == 0) continue;
        if (!ends_with(files->items[index].relative,
                       ZSHARP_SOURCE_EXTENSION))
            continue;
        if (!zsharp_project_parse_file(files->items[index].absolute, &program,
                                       &diagnostic, parse_error,
                                       sizeof(parse_error))) {
            free(first_script);
            package_error(error, error_size, "%s",
                          diagnostic.message[0] != '\0'
                              ? diagnostic.message
                              : parse_error);
            return NULL;
        }
        if (program.script_type == ZSCRIPT_NORMAL &&
            (first_script == NULL ||
             strcmp(files->items[index].relative, first_script) < 0)) {
            char *replacement = copy_text(files->items[index].relative);
            if (replacement == NULL) {
                zsharp_program_free(&program);
                free(first_script);
                package_error(error, error_size, "out of memory");
                return NULL;
            }
            free(first_script);
            first_script = replacement;
        }
        zsharp_program_free(&program);
    }
    if (first_script != NULL) return first_script;
    package_error(error, error_size,
                  "game packages require at least one zsharp = type.script file");
    return NULL;
}

static int validate_game_objects(const PackageFileList *files,
                                 const ZSharpSettings *settings,
                                 const char *root,
                                 const char *startup_relative,
                                 ZSharpValidationContext *validation,
                                 char *error, size_t error_size) {
    size_t index;
    if (!zsharp_project_validation_game(validation, root, error, error_size)) return 0;
    for (index = 0; index < files->count; index++) {
        if (strcmp(files->items[index].relative, startup_relative) == 0) {
            ZSharpProgram program;
            ZSharpDiagnostic diagnostic;
            char parse_error[512] = {0};
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
            program.script_type = ZSCRIPT_GAME;
            ok = zsharp_project_validate_with_context(&program, settings, root, validation, error,
                                         error_size);
            zsharp_program_free(&program);
            return ok;
        }
    }
    package_error(error, error_size, "could not validate the game startup");
    return 0;
}

static int append_startup_bytecode(PackageFileList *files,
                                   const ZSharpSettings *settings,
                                   const char *root,
                                   const char *startup_relative,
                                   int force_game,
                                   ZSharpValidationContext *validation,
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
    if (force_game) program.script_type = ZSCRIPT_GAME;
    if (!zsharp_project_validate_with_context(&program, settings, root, validation, error,
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
    if (memchr(name, '\0', name_length) != NULL || !safe_relative_path(name) || name[name_length - 1] == '/') {
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
    if (ends_with(path, ".zpackage")) return ZSHARP_PACKAGE_PACK;
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
    ZSharpValidationContext validation = {0};
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    FILE *output = NULL;
    uint64_t total = 0;
    size_t index;
    int ok = 0;
    if (kind == 0 || kind == ZSHARP_PACKAGE_PACK) {
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
        ((!settings.has_window || settings.window_startup == NULL) &&
         settings.native_target_count == 0)) {
        package_error(error, error_size,
                      "app packages require a Window Startup entry or Native target");
        zsharp_settings_free(&settings);
        free(root);
        return 0;
    }
    if (!zsharp_package_prepare_dependencies(root, NULL, 0, error, error_size) ||
        !collect_package_files(root, &settings, &files, error, error_size) ||
        !validate_sources(&files, &settings, root,
                          kind == ZSHARP_PACKAGE_GAME, &validation, error, error_size))
        goto done;
    if (kind == ZSHARP_PACKAGE_GAME) {
        game_startup = find_game_startup(&files, error, error_size);
        if (game_startup == NULL) goto done;
        if (!validate_game_objects(&files, &settings, root, game_startup, &validation, error,
                                   error_size)) goto done;
        startup_relative = game_startup;
    } else if (settings.native_target_count == 0) {
        startup_relative = settings.window_startup;
    }
    if (startup_relative != NULL &&
        !append_startup_bytecode(&files, &settings, root, startup_relative,
                                 kind == ZSHARP_PACKAGE_GAME, &validation, output_path,
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
    if (getenv("ZSHARP_PACKAGE_PROFILE") != NULL)
        fprintf(stderr, "package validation: game model loads=%zu, placements=%zu\n",
                validation.game_model_loads, validation.game == NULL ? 0 : validation.game->object_count);
    zsharp_project_validation_free(&validation);
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
    ZSharpValidationContext validation = {0};
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
        ((!settings.has_window || settings.window_startup == NULL) &&
         settings.native_target_count == 0)) {
        package_error(error, error_size,
                      "app packages require a Window Startup entry or Native target");
        goto done;
    }
    if (!zsharp_package_prepare_dependencies(root, NULL, 0, error, error_size) ||
        !collect_package_files(root, &settings, &files, error, error_size) ||
        !validate_sources(&files, &settings, root,
                          kind == ZSHARP_PACKAGE_GAME, &validation, error, error_size))
        goto done;
    if (kind == ZSHARP_PACKAGE_GAME) {
        game_startup = find_game_startup(&files, error, error_size);
        if (game_startup == NULL) goto done;
        if (!validate_game_objects(&files, &settings, root, game_startup, &validation, error,
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
    if (getenv("ZSHARP_PACKAGE_PROFILE") != NULL)
        fprintf(stderr, "package validation: game model loads=%zu, placements=%zu\n",
                validation.game_model_loads, validation.game == NULL ? 0 : validation.game->object_count);
    zsharp_project_validation_free(&validation);
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
    if (ends_with(name, ".zapp") || ends_with(name, ".zgame") || ends_with(name, ".zpackage")) {
        package_error(error, error_size,
                      "use the filename without .zapp or .zgame (or .zpackage)");
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
    if (kind != ZSHARP_PACKAGE_APP && kind != ZSHARP_PACKAGE_GAME && kind != ZSHARP_PACKAGE_PACK) {
        package_error(error, error_size, "package type must be app, game or pack");
        return 0;
    }
    if (!valid_package_name(package_name, error, error_size)) return 0;
    root = zsharp_project_find_root(project_path, error, error_size);
    if (root == NULL) return 0;
    packages = join_path(root, "Packages");
    if (packages == NULL ||
        !make_directories(packages, error, error_size)) goto done;
    extension = kind == ZSHARP_PACKAGE_APP ? ".zapp" : kind == ZSHARP_PACKAGE_GAME ? ".zgame" : ".zpackage";
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
    if (!(kind == ZSHARP_PACKAGE_PACK
          ? create_source_zip(root, destination, error, error_size)
          : zsharp_package_create(root, destination, error, error_size))) goto done;
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
        strpbrk(path, ":\\*?\"<>|") != NULL) return 0;
    for (cursor = path;; cursor++) {
        if (*cursor != '\0' && (unsigned char)*cursor < 32) return 0;
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - segment);
            char device[10] = {0};
            size_t device_length = 0, index;
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.') ||
                segment[length - 1] == '.' || segment[length - 1] == ' ')
                return 0;
            while (device_length < length && segment[device_length] != '.') device_length++;
            if (device_length < sizeof(device)) {
                for (index = 0; index < device_length; index++) {
                    char c = segment[index];
                    device[index] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
                }
                if (strcmp(device, "CON") == 0 || strcmp(device, "PRN") == 0 ||
                    strcmp(device, "AUX") == 0 || strcmp(device, "NUL") == 0 ||
                    strcmp(device, "CONIN$") == 0 || strcmp(device, "CONOUT$") == 0 ||
                    (device_length == 4 && (memcmp(device, "COM", 3) == 0 || memcmp(device, "LPT", 3) == 0) &&
                     device[3] >= '1' && device[3] <= '9')) return 0;
            }
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
    return zsharp_package_enable_go_companion(target, error, error_size);
}

static int verify_zip_entry(FILE *package, FILE *cached, FILE *output,
                            const char *target, uint32_t size,
                            uint32_t expected_crc, int *matches,
                            char *error, size_t error_size) {
    unsigned char buffer[64 * 1024];
    unsigned char cached_buffer[64 * 1024];
    uint32_t remaining = size;
    uint32_t actual_crc = 0;
    int same = cached != NULL;
    while (remaining > 0) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer)
                                                    : (size_t)remaining;
        if (fread(buffer, 1, wanted, package) != wanted ||
            (output != NULL && fwrite(buffer, 1, wanted, output) != wanted)) {
            package_error(error, error_size,
                          "unbytecoded package data is truncated");
            return 0;
        }
        if (same && (fread(cached_buffer, 1, wanted, cached) != wanted ||
                     memcmp(buffer, cached_buffer, wanted) != 0)) same = 0;
        actual_crc = crc32_update(actual_crc, buffer, wanted);
        remaining -= (uint32_t)wanted;
    }
    if (same && (fgetc(cached) != EOF || ferror(cached))) same = 0;
    if (actual_crc != expected_crc) {
        package_error(error, error_size,
                      "package entry '%s' failed ZIP CRC verification", target);
        return 0;
    }
    if (matches != NULL) *matches = same;
    return 1;
}

static int write_zip_entry(FILE *package, const char *target, uint32_t size,
                           uint32_t expected_crc, char *error,
                           size_t error_size) {
    FILE *output;
    int ok;
    if (!make_parent_directories(target, error, error_size)) return 0;
    output = fopen(target, "wb");
    if (output == NULL) {
        package_error(error, error_size, "could not extract '%s'", target);
        return 0;
    }
    ok = verify_zip_entry(package, NULL, output, target, size, expected_crc,
                          NULL, error, error_size);
    if (fclose(output) != 0) {
        package_error(error, error_size, "could not finish extracting '%s'", target);
        ok = 0;
    }
    if (!ok) return 0;
    return zsharp_package_enable_go_companion(target, error, error_size);
}

/* The configured cache base is user-controlled and may legitimately resolve
 * through HOME/XDG symlinks. Below it, package-owned paths must never redirect
 * reads or replacement writes through symlinks/reparse points. */
static int source_cache_path(const char *base, const char *path, int directory,
                              int *exists, char *error, size_t error_size) {
    size_t base_length = strlen(base);
    char *copy = copy_text(path), *cursor;
    int ok = 1;
    if (exists != NULL) *exists = 0;
    if (copy == NULL) {
        package_error(error, error_size, "out of memory");
        return 0;
    }
    if (strncmp(base, path, base_length) != 0 || strlen(path) <= base_length) {
        free(copy);
        package_error(error, error_size, "invalid package cache path");
        return 0;
    }
    for (cursor = copy + base_length; ; cursor++) {
        int leaf, wanted_directory, present;
        char saved;
        if (*cursor != '\0' && *cursor != '/' && *cursor != '\\') continue;
        if (cursor == copy + base_length) continue;
        leaf = *cursor == '\0';
        wanted_directory = !leaf || directory;
        saved = *cursor;
        *cursor = '\0';
#ifdef _WIN32
        {
            DWORD attributes = GetFileAttributesA(copy);
            present = attributes != INVALID_FILE_ATTRIBUTES;
            if (!present && wanted_directory &&
                (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
                if (CreateDirectoryA(copy, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
                    attributes = GetFileAttributesA(copy);
                present = attributes != INVALID_FILE_ATTRIBUTES;
            }
            if ((present && ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                 (attributes & FILE_ATTRIBUTE_DEVICE) ||
                 !!(attributes & FILE_ATTRIBUTE_DIRECTORY) != wanted_directory)) ||
                (!present && (wanted_directory ||
                 (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND))))
                ok = 0;
        }
#else
        {
            struct stat status;
            present = lstat(copy, &status) == 0;
            if (!present && errno == ENOENT && wanted_directory) {
                if (mkdir(copy, 0700) == 0 || errno == EEXIST)
                    present = lstat(copy, &status) == 0;
            }
            if ((present && (S_ISLNK(status.st_mode) ||
                 (wanted_directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)))) ||
                (!present && (wanted_directory || errno != ENOENT))) ok = 0;
        }
#endif
        if (ok && leaf && exists != NULL) *exists = present;
        if (!ok) package_error(error, error_size, "unsafe or unavailable package cache path '%s'", copy);
        *cursor = saved;
        if (!ok || leaf) break;
    }
    free(copy);
    return ok;
}

static FILE *source_cache_open(const char *path) {
    FILE *file;
    int descriptor;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
          FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))) {
        CloseHandle(handle);
        return NULL;
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (descriptor < 0) { CloseHandle(handle); return NULL; }
    file = _fdopen(descriptor, "rb");
    if (file == NULL) _close(descriptor);
#else
    struct stat status;
    descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return NULL;
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(descriptor);
        return NULL;
    }
    file = fdopen(descriptor, "rb");
    if (file == NULL) close(descriptor);
#endif
    return file;
}

static FILE *source_cache_temporary(const char *target, char **path) {
    size_t capacity = strlen(target) + 64;
    FILE *file = NULL;
    int descriptor = -1;
    *path = (char *)malloc(capacity);
    if (*path == NULL) return NULL;
#ifdef _WIN32
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        snprintf(*path, capacity, "%s.zsharp-tmp-%lu-%u", target,
                 (unsigned long)GetCurrentProcessId(), attempt);
        descriptor = _open(*path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY |
                            _O_NOINHERIT, _S_IREAD | _S_IWRITE);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor >= 0) file = _fdopen(descriptor, "wb");
    if (descriptor >= 0 && file == NULL) _close(descriptor);
#else
    snprintf(*path, capacity, "%s.zsharp-tmp-XXXXXX", target);
    descriptor = mkstemp(*path);
    if (descriptor >= 0) file = fdopen(descriptor, "wb");
    if (descriptor >= 0 && file == NULL) close(descriptor);
#endif
    if (file == NULL) {
        if (descriptor >= 0) remove(*path);
        free(*path);
        *path = NULL;
    }
    return file;
}

static int source_cache_replace(const char *temporary, const char *target,
                                 char *error, size_t error_size) {
#ifdef _WIN32
    int ok = MoveFileExA(temporary, target,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    int ok = rename(temporary, target) == 0;
#endif
    if (!ok) package_error(error, error_size, "could not replace cached package file '%s'", target);
    return ok;
}

static int source_cache_write(FILE *package, const char *base, const char *target,
                               uint32_t size, uint32_t expected_crc,
                               char *error, size_t error_size) {
    char *temporary = NULL;
    FILE *output = source_cache_temporary(target, &temporary);
    int ok;
    if (output == NULL) {
        package_error(error, error_size, "could not stage cached package file '%s'", target);
        return 0;
    }
    ok = verify_zip_entry(package, NULL, output, target, size, expected_crc,
                          NULL, error, error_size);
    if (fclose(output) != 0) {
        package_error(error, error_size, "could not finish extracting '%s'", target);
        ok = 0;
    }
    if (ok) ok = source_cache_path(base, target, 0, NULL, error, error_size) &&
                 source_cache_replace(temporary, target, error, error_size);
    if (!ok) remove(temporary);
    free(temporary);
    return ok && zsharp_package_enable_go_companion(target, error, error_size);
}

static int source_cache_reserved_path(const char *relative) {
    static const char marker[] = ".zsharp-package-complete";
    size_t index;
    for (index = 0; index < sizeof(marker) - 1; index++) {
        unsigned char character = (unsigned char)relative[index];
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character != (unsigned char)marker[index]) return 0;
    }
    return relative[index] == '\0' || relative[index] == '/';
}

static int extract_source_zip(const char *package_path, char **project_root,
                              ZSharpPackageInfo *info, int *new_install,
                              char *error, size_t error_size) {
    FILE *package = NULL;
    char *base = NULL;
    char *root = NULL;
    char *marker = NULL;
    uint16_t entry_count;
    uint32_t central_offset;
    uint16_t index;
    int marker_existed = 0;
    int marker_valid = 0;
    int cache_changed = 0;
    int ok = 0;
    if (!zsharp_package_read_info(package_path, info, error, error_size))
        return 0;
    package = fopen(package_path, "rb");
    if (package == NULL ||
        !zip_eocd(package, &entry_count, &central_offset, error, error_size) ||
        entry_count != info->file_count || fseek(package, 0, SEEK_SET) != 0)
        goto done;
    root = package_cache_root(package_path, info, error, error_size);
    base = cache_base(error, error_size);
    marker = root == NULL ? NULL : join_path(root,
                                             ".zsharp-package-complete");
    if (base == NULL || root == NULL || marker == NULL) goto done;
    if (!make_directories(base, error, error_size) ||
        !source_cache_path(base, root, 1, NULL, error, error_size) ||
        !source_cache_path(base, marker, 0, &marker_existed, error, error_size)) goto done;
    {
        static const char complete_text[] = "verified\n";
        char contents[sizeof(complete_text) - 1];
        FILE *complete = source_cache_open(marker);
        if (complete != NULL) {
            marker_valid = fread(contents, 1, sizeof(contents), complete) == sizeof(contents) &&
                           memcmp(contents, complete_text, sizeof(contents)) == 0 &&
                           fgetc(complete) == EOF && !ferror(complete);
            if (fclose(complete) != 0) marker_valid = 0;
        }
    }
    for (index = 0; index < entry_count; index++) {
        char *relative = NULL;
        char *target;
        FILE *cached;
        long data_offset;
        int matches = 0;
        uint32_t size;
        uint32_t expected_crc;
        if (!zip_local_header(package, &relative, &size, &expected_crc, error,
                              error_size)) goto done;
        if (source_cache_reserved_path(relative)) {
            free(relative);
            package_error(error, error_size, "source package contains a reserved cache marker");
            goto done;
        }
        target = join_path(root, relative);
        free(relative);
        if (target == NULL) {
            package_error(error, error_size, "out of memory");
            goto done;
        }
        if (!source_cache_path(base, target, 0, NULL, error, error_size) ||
            (data_offset = ftell(package)) < 0) {
            free(target);
            goto done;
        }
        cached = source_cache_open(target);
        if (cached != NULL) {
            int verified = verify_zip_entry(package, cached, NULL, target, size,
                                             expected_crc, &matches, error, error_size);
            if (fclose(cached) != 0) matches = 0;
            if (!verified || (!matches && fseek(package, data_offset, SEEK_SET) != 0)) {
                free(target);
                goto done;
            }
        }
        if (!matches) {
            if (!source_cache_write(package, base, target, size, expected_crc,
                                     error, error_size)) {
                free(target);
                goto done;
            }
            cache_changed = 1;
        }
#ifndef _WIN32
        else if (ends_with(target, ".zgo")) {
            struct stat status;
            if (lstat(target, &status) != 0 ||
                (!(status.st_mode & S_IXUSR) &&
                 !zsharp_package_enable_go_companion(target, error, error_size))) {
                free(target);
                goto done;
            }
        }
#endif
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
    if (!marker_valid || cache_changed) {
        char *temporary = NULL;
        FILE *complete = source_cache_temporary(marker, &temporary);
        int complete_ok = complete != NULL;
        if (complete_ok && fputs("verified\n", complete) < 0)
            complete_ok = 0;
        if (complete != NULL && fclose(complete) != 0)
            complete_ok = 0;
        if (complete_ok)
            complete_ok = source_cache_path(base, marker, 0, NULL, error, error_size) &&
                          source_cache_replace(temporary, marker, error, error_size);
        if (!complete_ok && temporary != NULL) remove(temporary);
        free(temporary);
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
    free(base);
    free(marker);
    free(root);
    if (!ok) zsharp_package_info_free(info);
    return ok;
}

/* Dependencies are installed transactionally below their owning project.
 * Metadata is inspected before any dependency native code can be loaded. */
static int dependency_directory_safe(const char *path, char *error, size_t error_size) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))) {
#else
    struct stat status;
    if (lstat(path, &status) == 0 && (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))) {
#endif
        package_error(error, error_size, "dependency directory is not a real directory: '%s'", path);
        return 0;
    }
    return 1;
}

static int dependency_matches(const ZSharpPackageInfo *info, const ZSharpDependency *dependency) {
    return info->kind == ZSHARP_PACKAGE_PACK &&
        strcmp(info->project_id, dependency->project_id) == 0 &&
        memcmp(info->version, dependency->version, sizeof(info->version)) == 0;
}

static int dependency_archive_candidate(const char *directory, const char *name,
    const ZSharpDependency *dependency, char **selected, char *error, size_t error_size) {
    ZSharpPackageInfo info = {0};
    char *path;
    int matches;
    if (!ends_with(name, ".zpackage")) return 1;
    path = join_path(directory, name);
    if (!path) { package_error(error, error_size, "out of memory"); return 0; }
#ifdef _WIN32
    if (GetFileAttributesA(path) & FILE_ATTRIBUTE_REPARSE_POINT) {
#else
    { struct stat status; if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
#endif
        package_error(error, error_size, "unsafe dependency archive '%s'", path);
        free(path); return 0;
    }
#ifndef _WIN32
    }
#endif
    if (!zsharp_package_read_info(path, &info, error, error_size)) {
        free(path); return 0;
    }
    if (info.kind != ZSHARP_PACKAGE_PACK) {
        package_error(error, error_size, "dependency archive '%s' is not a source .zpackage", path);
        zsharp_package_info_free(&info); free(path); return 0;
    }
    matches = dependency_matches(&info, dependency);
    zsharp_package_info_free(&info);
    if (matches && *selected != NULL) {
        package_error(error, error_size, "multiple .zpackage files match dependency '%s'", dependency->project_id);
        free(path); return 0;
    }
    if (matches) *selected = path; else free(path);
    return 1;
}

static int find_dependency_archive(const char *directory, const ZSharpDependency *dependency,
    char **selected, char *error, size_t error_size) {
    int ok = 1;
    *selected = NULL;
#ifdef _WIN32
    char *pattern = join_path(directory, "*");
    WIN32_FIND_DATAA entry;
    HANDLE search;
    if (!pattern) return 0;
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return 1;
        package_error(error, error_size, "could not scan dependency folder '%s'", directory); return 0;
    }
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !dependency_archive_candidate(directory, entry.cFileName, dependency, selected, error, error_size)) {
            ok = 0; break;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    DIR *search = opendir(directory);
    struct dirent *entry;
    if (!search) {
        if (errno == ENOENT) return 1;
        package_error(error, error_size, "could not scan dependency folder '%s'", directory); return 0;
    }
    while ((entry = readdir(search)) != NULL)
        if (!dependency_archive_candidate(directory, entry->d_name, dependency, selected, error, error_size)) {
            ok = 0; break;
        }
    closedir(search);
#endif
    return ok;
}

static int dependency_root_matches(const char *root, const ZSharpDependency *dependency,
    char *error, size_t error_size) {
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    int ok;
    if (!zsharp_settings_load(root, &settings, &diagnostic, error, error_size)) return 0;
    ok = strcmp(settings.project_id, dependency->project_id) == 0 &&
        memcmp(settings.version, dependency->version, sizeof(settings.version)) == 0;
    zsharp_settings_free(&settings);
    if (!ok) package_error(error, error_size, "installed dependency '%s' has incorrect PID/version", dependency->project_id);
    return ok;
}

static int compare_dependency_names(const void *a, const void *b) {
#ifdef _WIN32
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
#else
    return strcmp(*(const char *const *)a, *(const char *const *)b);
#endif
}

typedef struct DependencyLock {
#ifdef _WIN32
    HANDLE handle;
#else
    int descriptor;
#endif
} DependencyLock;

static int dependency_lock_acquire(const char *directory, const char *id, const char *version,
    DependencyLock *lock, char *error, size_t error_size) {
    char name[256];
    char *path;
    int ok = 0;
    snprintf(name, sizeof(name), ".zsharp-dependency-lock-%s-%s", id, version);
    path = join_path(directory, name);
    if (!path || !make_directories(directory, error, error_size)) { free(path); return 0; }
#ifdef _WIN32
    lock->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (lock->handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION information;
        OVERLAPPED overlapped = {0};
        if (GetFileInformationByHandle(lock->handle, &information) &&
            !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            ok = LockFileEx(lock->handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped) != 0;
        if (!ok) { CloseHandle(lock->handle); lock->handle = INVALID_HANDLE_VALUE; }
    }
#else
    lock->descriptor = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock->descriptor >= 0) {
        struct flock request = {0};
        request.l_type = F_WRLCK; request.l_whence = SEEK_SET; request.l_len = 1;
        do { ok = fcntl(lock->descriptor, F_SETLKW, &request) == 0; } while (!ok && errno == EINTR);
        if (!ok) { close(lock->descriptor); lock->descriptor = -1; }
    }
#endif
    if (!ok) package_error(error, error_size, "could not lock dependency '%s'", id);
    free(path);
    return ok;
}

static void dependency_lock_release(DependencyLock *lock) {
#ifdef _WIN32
    if (lock->handle != INVALID_HANDLE_VALUE) {
        OVERLAPPED overlapped = {0};
        UnlockFileEx(lock->handle, 0, 1, 0, &overlapped);
        CloseHandle(lock->handle);
    }
#else
    if (lock->descriptor >= 0) close(lock->descriptor);
#endif
}

static int install_dependency_archive(const char *archive, const char *destination,
    const ZSharpDependency *dependency, char *error, size_t error_size) {
    ZSharpPackageInfo info = {0};
    unsigned char digest[ZSHARP_SHA256_SIZE];
    char hex[ZSHARP_SHA256_SIZE * 2 + 1], existing[66] = {0};
    char *marker = NULL, *stage = NULL;
    FILE *file = NULL, *complete = NULL;
    char **names = NULL;
    uint16_t count = 0, index;
    uint32_t central_offset;
    int ok = 0;
    unsigned attempt;
    int stage_created = 0;
    if (!zsharp_package_read_info(archive, &info, error, error_size)) return 0;
    if (!dependency_matches(&info, dependency)) {
        package_error(error, error_size, "downloaded package does not match dependency '%s' and its requested release", dependency->project_id);
        goto done;
    }
    if (!zsharp_sha256_file(archive, digest)) goto done;
    zsharp_hash_hex(digest, hex);
    marker = join_path(destination, ".zsharp-dependency-complete");
    if (!marker) goto done;
    complete = fopen(marker, "rb");
    if (complete) {
        size_t bytes = fread(existing, 1, 64, complete);
        fclose(complete); complete = NULL;
        if (bytes != 64 || strcmp(existing, hex) != 0) {
            package_error(error, error_size, "dependency '%s' release already installed from different contents; move its installed directory aside before replacing it", dependency->project_id);
            goto done;
        }
        ok = dependency_root_matches(destination, dependency, error, error_size);
        goto done;
    }
    stage = (char *)malloc(strlen(destination) + 112);
    if (!stage) goto done;
    for (attempt = 0; attempt < 100; attempt++) {
#ifdef _WIN32
        snprintf(stage, strlen(destination) + 112, "%s.install-%s-%lu-%u", destination, hex, (unsigned long)GetCurrentProcessId(), attempt);
        if (CreateDirectoryA(stage, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS) goto done;
#else
        snprintf(stage, strlen(destination) + 112, "%s.install-%s-%lu-%u", destination, hex, (unsigned long)getpid(), attempt);
        if (mkdir(stage, 0700) == 0) break;
        if (errno != EEXIST) goto done;
#endif
    }
    if (attempt == 100) goto done;
    stage_created = 1;
    file = fopen(archive, "rb");
    if (!file || !zip_eocd(file, &count, &central_offset, error, error_size) ||
        fseek(file, 0, SEEK_SET) != 0) goto done;
    names = (char **)calloc(count, sizeof(*names));
    if (!names) goto done;
    for (index = 0; index < count; index++) {
        uint32_t size, crc;
        char *target;
        if (!zip_local_header(file, &names[index], &size, &crc, error, error_size)) goto done;
        if (ends_with(names[index], ".zbc") || ends_with(names[index], ".zpackage") ||
            strcmp(names[index], ".zsharp-dependency-complete") == 0) {
            package_error(error, error_size, "source dependency contains forbidden entry '%s'", names[index]); goto done;
        }
        target = join_path(stage, names[index]);
        if (!target) goto done;
        ok = write_zip_entry(file, target, size, crc, error, error_size);
        free(target);
        if (!ok) goto done;
        ok = 0;
    }
    qsort(names, count, sizeof(*names), compare_dependency_names);
    for (index = 1; index < count; index++)
        if (compare_dependency_names(&names[index - 1], &names[index]) == 0) {
            package_error(error, error_size, "duplicate dependency archive entry '%s'", names[index]); goto done;
        }
    if (ftell(file) != (long)central_offset || !dependency_root_matches(stage, dependency, error, error_size)) goto done;
    fclose(file); file = NULL;
    {
        char *stage_marker = join_path(stage, ".zsharp-dependency-complete");
        complete = stage_marker ? fopen(stage_marker, "wb") : NULL;
        free(stage_marker);
        if (!complete) goto done;
        ok = fputs(hex, complete) >= 0;
        if (fclose(complete) != 0) ok = 0;
        complete = NULL;
        if (!ok) goto done;
        ok = 0;
    }
#ifdef _WIN32
    ok = MoveFileExA(stage, destination, MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = rename(stage, destination) == 0;
#endif
    if (!ok) package_error(error, error_size, "could not finalize dependency directory '%s' (it may already exist)", destination);
done:
    if (complete) fclose(complete);
    if (file) fclose(file);
    if (!ok && stage_created) {
        char cleanup_error[256] = {0};
        remove_tree(stage, cleanup_error, sizeof(cleanup_error));
    }
    if (names) { for (index = 0; index < count; index++) free(names[index]); free(names); }
    free(stage); free(marker);
    zsharp_package_info_free(&info);
    if (!ok && error[0] == '\0') package_error(error, error_size, "could not install dependency '%s'", dependency->project_id);
    return ok;
}

static int prepare_dependencies(const char *root, const char *const *providers, size_t provider_count,
    const char **stack, unsigned depth, unsigned *visits, char *error, size_t error_size) {
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    char *directory = NULL;
    size_t index;
    int ok = 0;
    if (depth >= 32 || ++*visits > 512) {
        package_error(error, error_size, "dependency graph exceeds 32 levels or 512 projects"); return 0;
    }
    if (!zsharp_settings_load(root, &settings, &diagnostic, error, error_size)) {
        if (diagnostic.message[0]) package_error(error, error_size, "project.zsettings:%u:%u: %s", diagnostic.line, diagnostic.column, diagnostic.message);
        return 0;
    }
    for (index = 0; index < depth; index++) {
        if (strcmp(stack[index], settings.project_id) == 0) {
            package_error(error, error_size, "cyclic dependency involving '%s'", settings.project_id); goto done;
        }
    }
    stack[depth] = settings.project_id;
    directory = join_path(root, "Dependencies");
    if (!directory || !dependency_directory_safe(directory, error, error_size)) goto done;
    for (index = 0; index < settings.dependency_count; index++) {
        const ZSharpDependency *dependency = &settings.dependencies[index];
        char version[64], filename[256], url[512];
        char *parent = NULL, *destination = NULL, *archive = NULL, *temporary = NULL, *marker = NULL;
        size_t provider;
        int installed = 0, step_ok = 0;
        DependencyLock lock;
        int locked = 0;
#ifdef _WIN32
        lock.handle = INVALID_HANDLE_VALUE;
#else
        lock.descriptor = -1;
#endif
        if (strcmp(dependency->project_id, "zsharpwindow") == 0 || strcmp(dependency->project_id, "zsharpgame") == 0 ||
            strcmp(dependency->project_id, "zsharp") == 0 || strcmp(dependency->project_id, "zos") == 0) continue;
        for (provider = 0; provider < provider_count; provider++)
            if (strcmp(providers[provider], dependency->project_id) == 0) break;
        if (provider != provider_count) continue;
        if (!safe_project_id(dependency->project_id) || strlen(dependency->project_id) > 128) {
            package_error(error, error_size, "invalid dependency project ID"); goto done;
        }
        snprintf(version, sizeof(version), "%u.%u.%u.%u", dependency->version[0], dependency->version[1], dependency->version[2], dependency->version[3]);
        if (!dependency_lock_acquire(directory, dependency->project_id, version, &lock, error, error_size)) goto dependency_done;
        locked = 1;
        parent = join_path(directory, dependency->project_id);
        destination = parent ? join_path(parent, version) : NULL;
        if (!destination || !dependency_directory_safe(parent, error, error_size) ||
            !dependency_directory_safe(destination, error, error_size)) goto dependency_done;
        if (!find_dependency_archive(directory, dependency, &archive, error, error_size)) goto dependency_done;
        marker = join_path(destination, ".zsharp-dependency-complete");
        if (marker) { FILE *probe = fopen(marker, "rb"); if (probe) { installed = 1; fclose(probe); } }
        if (archive == NULL && installed) {
            /* A packaged application contains installed sources, not duplicate archives. */
            if (!dependency_root_matches(destination, dependency, error, error_size)) goto dependency_done;
        } else {
            if (!make_directories(parent, error, error_size)) goto dependency_done;
            if (archive == NULL) {
                snprintf(filename, sizeof(filename), "%s-%s.zpackage", dependency->project_id, version);
                archive = join_path(directory, filename);
                if (!archive) goto dependency_done;
                temporary = (char *)malloc(strlen(archive) + 5);
                if (!temporary) goto dependency_done;
                sprintf(temporary, "%s.tmp", archive);
                snprintf(url, sizeof(url), "https://zos-store-api.zos-store-api.workers.dev/?action=download&project=%s&release=%s", dependency->project_id, version);
                fprintf(stderr, "downloading dependency %s:%s\n", dependency->project_id, version);
                if (!zsharp_update_download_component(url, temporary, (size_t)1024 * 1024 * 1024, error, error_size)) goto dependency_done;
                /* The reader selects the source format from its final extension. */
                {
                    char *validated = (char *)malloc(strlen(archive) + 32);
                    if (!validated) goto dependency_done;
                    sprintf(validated, "%s.download.zpackage", archive);
                    if (rename(temporary, validated) != 0) { free(validated); goto dependency_done; }
                    if (!install_dependency_archive(validated, destination, dependency, error, error_size)) {
                        remove(validated); free(validated); goto dependency_done;
                    }
                    if (rename(validated, archive) != 0) {
                        package_error(error, error_size, "could not finalize downloaded dependency '%s'", archive);
                        remove(validated);
                        free(validated); goto dependency_done;
                    }
                    free(validated);
                }
            } else if (!install_dependency_archive(archive, destination, dependency, error, error_size)) goto dependency_done;
        }
        step_ok = prepare_dependencies(destination, providers, provider_count, stack, depth + 1, visits, error, error_size);
dependency_done:
        if (locked) dependency_lock_release(&lock);
        if (temporary) remove(temporary);
        free(parent); free(destination); free(archive); free(temporary); free(marker);
        if (!step_ok) goto done;
    }
    ok = 1;
done:
    free(directory);
    zsharp_settings_free(&settings);
    return ok;
}

int zsharp_package_prepare_dependencies(const char *root,
    const char *const *providers, size_t provider_count, char *error, size_t error_size) {
    const char *stack[32];
    unsigned visits = 0;
    return prepare_dependencies(root, providers, provider_count, stack, 0, &visits, error, error_size);
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
