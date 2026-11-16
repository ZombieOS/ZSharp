#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "app_icon.h"
#include "settings.h"
#include "zsharp_default_icon.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void icon_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
}

static char *copy_text(const char *source) {
    size_t size = strlen(source) + 1;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, source, size);
    return copy;
}

static int path_separator(char character) {
    return character == '/'
#ifdef _WIN32
        || character == '\\'
#endif
        ;
}

static char *join_path(const char *root, const char *name) {
    size_t length = strlen(root), extra = strlen(name);
    int slash = length != 0 && !path_separator(root[length-1]);
    char *path = (char *)malloc(length + (size_t)slash + extra + 1);
    if (path == NULL) return NULL;
    memcpy(path, root, length);
    if (slash) path[length++] = '/';
    memcpy(path + length, name, extra + 1);
    return path;
}

#ifdef _WIN32
static WCHAR *wide_text(const char *text) {
    UINT encoding = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(encoding, flags, text, -1, NULL, 0);
    WCHAR *wide;
    if (!count) {
        encoding = CP_ACP; flags = 0;
        count = MultiByteToWideChar(encoding, flags, text, -1, NULL, 0);
    }
    if (!count) return NULL;
    wide = (WCHAR *)malloc((size_t)count * sizeof(*wide));
    if (wide != NULL && !MultiByteToWideChar(encoding, flags, text, -1, wide, count)) {
        free(wide); wide = NULL;
    }
    return wide;
}

static char *utf8_text(const WCHAR *wide) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = count ? (char *)malloc((size_t)count) : NULL;
    if (text != NULL && !WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, count, NULL, NULL)) {
        free(text); text = NULL;
    }
    return text;
}

static FILE *open_file(const char *path, const WCHAR *mode) {
    WCHAR *wide = wide_text(path);
    FILE *file = wide == NULL ? NULL : _wfopen(wide, mode);
    free(wide);
    return file;
}

static char *absolute_path(const char *path) {
    WCHAR *wide = wide_text(path), *full;
    DWORD count = wide == NULL ? 0 : GetFullPathNameW(wide, 0, NULL, NULL);
    char *result = NULL;
    full = count ? (WCHAR *)malloc((size_t)count * sizeof(*full)) : NULL;
    if (full != NULL && GetFullPathNameW(wide, count, full, NULL)) result = utf8_text(full);
    free(full); free(wide);
    return result;
}

static int make_directory(const char *path) {
    WCHAR *wide = wide_text(path);
    DWORD attributes;
    int ok;
    if (wide == NULL) return 0;
    ok = CreateDirectoryW(wide, NULL) != 0;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        attributes = GetFileAttributesW(wide);
        ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    free(wide);
    return ok;
}
static int regular_file(const char *path) {
    WCHAR *wide = wide_text(path);
    DWORD attributes = wide == NULL ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0;
}
#define READ_FILE(path) open_file(path, L"rb")
#else
static char *absolute_path(const char *path) { return realpath(path, NULL); }
static int make_directory(const char *path) {
    struct stat status;
    return mkdir(path, 0700) == 0 ||
           (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode));
}
static int regular_file(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}
#define READ_FILE(path) fopen(path, "rb")
#endif

static int is_absolute(const char *path) {
#ifdef _WIN32
    return path[0] == '/' || path[0] == '\\' ||
           (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
#else
    return path[0] == '/';
#endif
}

static int make_directories(const char *path) {
    char *copy = copy_text(path), *cursor;
    int ok = 1;
    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (!path_separator(*cursor)) continue;
#ifdef _WIN32
        if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
        {
            char saved = *cursor;
            *cursor = '\0';
            if (!make_directory(copy)) ok = 0;
            *cursor = saved;
        }
        if (!ok) break;
    }
    if (ok) ok = make_directory(copy);
    free(copy);
    return ok;
}

static uint32_t big_u32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

int zsharp_app_icon_is_png(const char *path) {
    static const unsigned char signature[] = {137,80,78,71,13,10,26,10};
    unsigned char header[33];
    FILE *file;
    size_t count;
    unsigned depth, color;
    if (path == NULL || path[0] == '\0' || !regular_file(path)) return 0;
    file = READ_FILE(path);
    if (file == NULL) return 0;
    count = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (count != sizeof(header) || memcmp(header, signature, 8) ||
        big_u32(header+8) != 13 || memcmp(header+12, "IHDR", 4) ||
        !big_u32(header+16) || !big_u32(header+20) ||
        big_u32(header+16) > 0x7fffffffu || big_u32(header+20) > 0x7fffffffu ||
        header[26] != 0 || header[27] != 0 || header[28] > 1) return 0;
    depth = header[24]; color = header[25];
    if (color == 0) return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    if (color == 3) return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    return (color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16);
}

static char *candidate_path(const char *root, const char *icon) {
    char *candidate, *result = NULL;
    if (icon == NULL || icon[0] == '\0') return NULL;
    candidate = is_absolute(icon) ? copy_text(icon) : join_path(root != NULL ? root : ".", icon);
    if (candidate != NULL && zsharp_app_icon_is_png(candidate)) result = absolute_path(candidate);
    free(candidate);
    return result;
}

static char *cache_directory(void) {
    const char *base;
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    return base != NULL && base[0] ? join_path(base, "ZombieOS/ZSharp/icons") : NULL;
#elif defined(__APPLE__)
    base = getenv("HOME");
    return base != NULL && base[0] ? join_path(base, "Library/Application Support/ZombieOS/ZSharp/icons") : NULL;
#else
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && is_absolute(base)) return join_path(base, "zsharp/icons");
    base = getenv("HOME");
    return base != NULL && base[0] ? join_path(base, ".local/share/zsharp/icons") : NULL;
#endif
}

static int matches_embedded(const char *path) {
    FILE *file;
    unsigned char buffer[8192];
    size_t position = 0, count;
    int matches = 1;
    if (!regular_file(path)) return 0;
    file = READ_FILE(path);
    if (file == NULL) return 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        if (position + count > sizeof(zsharp_default_icon_png) ||
            memcmp(buffer, zsharp_default_icon_png + position, count)) { matches = 0; break; }
        position += count;
    }
    if (ferror(file) || position != sizeof(zsharp_default_icon_png)) matches = 0;
    fclose(file);
    return matches;
}

static int write_embedded(const char *directory, const char *destination) {
    FILE *file = NULL;
    int ok = 0;
#ifdef _WIN32
    WCHAR *wide_directory = wide_text(directory), *wide_destination = wide_text(destination);
    WCHAR temporary[MAX_PATH] = {0};
    if (wide_directory == NULL || wide_destination == NULL ||
        !GetTempFileNameW(wide_directory, L"zsi", 0, temporary)) goto done;
    file = _wfopen(temporary, L"wb");
#else
    char *temporary = join_path(directory, ".zsharp-icon-XXXXXX");
    int descriptor = temporary == NULL ? -1 : mkstemp(temporary);
    if (descriptor < 0) { free(temporary); return 0; }
    file = fdopen(descriptor, "wb");
    if (file == NULL) close(descriptor);
#endif
    if (file != NULL) {
        int written = fwrite(zsharp_default_icon_png, 1, sizeof(zsharp_default_icon_png), file) == sizeof(zsharp_default_icon_png);
        int closed = fclose(file) == 0;
        file = NULL;
        if (written && closed) {
#ifdef _WIN32
            ok = MoveFileExW(temporary, wide_destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            ok = rename(temporary, destination) == 0;
#endif
        }
    }
#ifdef _WIN32
done:
    if (temporary[0]) DeleteFileW(temporary);
    free(wide_directory); free(wide_destination);
#else
    unlink(temporary); free(temporary);
#endif
    return ok;
}

char *zsharp_app_icon_default_path(char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_HUB_DEFAULT_ICON");
    char *path, *directory, *absolute;
    icon_error(error, error_size, "");
    absolute = candidate_path(".", override);
    if (absolute != NULL) return absolute;
    directory = cache_directory();
    if (directory == NULL || !make_directories(directory)) {
        free(directory);
        icon_error(error, error_size, "could not create the Z# icon cache");
        return NULL;
    }
    path = join_path(directory, "zsharp.png");
    if (path == NULL || (!matches_embedded(path) && !write_embedded(directory, path))) {
        free(directory); free(path);
        icon_error(error, error_size, "could not write the official Z# icon cache");
        return NULL;
    }
    free(directory);
    absolute = absolute_path(path);
    free(path);
    if (absolute == NULL) icon_error(error, error_size, "could not resolve the Z# icon cache path");
    return absolute;
}

char *zsharp_app_icon_resolve(const char *root, const char *icon,
                             char *error, size_t error_size) {
    char *path;
    icon_error(error, error_size, "");
    path = candidate_path(root, icon);
    return path != NULL ? path : zsharp_app_icon_default_path(error, error_size);
}

char *zsharp_app_icon_for_window(const char *root, const char *project_id,
                                const char *legacy_icon,
                                char *error, size_t error_size) {
    char *path = NULL;
    icon_error(error, error_size, "");
    if (root != NULL && project_id != NULL && project_id[0] != '\0' && strcmp(project_id, "zsharp-hub")) {
        ZSharpSettings settings;
        ZSharpDiagnostic diagnostic;
        char local_error[512] = {0};
        memset(&settings, 0, sizeof(settings));
        memset(&diagnostic, 0, sizeof(diagnostic));
        if (zsharp_settings_load(root, &settings, &diagnostic, local_error, sizeof(local_error)))
            path = candidate_path(root, settings.icon);
        zsharp_settings_free(&settings);
    }
    if (path != NULL) return path;
    return zsharp_app_icon_resolve(root, legacy_icon, error, error_size);
}

char *zsharp_app_desktop_id(const char *project_id) {
    const unsigned char *cursor;
    uint64_t hash = UINT64_C(14695981039346656037);
    char name[48];
    if (project_id == NULL || project_id[0] == '\0' || !strcmp(project_id, "zsharp-hub"))
        return copy_text("zsharp-hub");
    for (cursor = (const unsigned char *)project_id; *cursor != '\0'; cursor++) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    snprintf(name, sizeof(name), "zsharp-app-%016" PRIx64, hash);
    return copy_text(name);
}
