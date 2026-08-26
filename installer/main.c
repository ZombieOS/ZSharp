#define _CRT_SECURE_NO_WARNINGS

#include "hash.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#define INSTALLER_MANIFEST_LIMIT (1024u * 1024u)
#define INSTALLER_RUNTIME_LIMIT (256u * 1024u * 1024u)
#define INSTALLER_ARCHIVE_LIMIT (512u * 1024u * 1024u)
#define INSTALLER_UPDATE_ENDPOINT \
    "https://www.zsharp.zombieos.com/update.js?v="
#define INSTALLER_TEST_APP_URL \
    "https://www.zsharp.zombieos.com/assets/download/ZSharp-Test-App.zapp"

typedef struct ReleaseInfo {
    int update_available;
    char *version;
    char *archive_url;
    char *archive_sha256;
    uint64_t archive_size;
    char *runtime_path;
    char *runtime_sha256;
    uint64_t runtime_size;
} ReleaseInfo;

static void report_error(char *error, size_t error_size,
                         const char *format, ...) {
    va_list arguments;
    if (error == NULL || error_size == 0) return;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *result = (char *)malloc(length + 1);
    if (result != NULL) memcpy(result, text, length + 1);
    return result;
}

static char *join_path(const char *left, const char *right) {
    size_t a = strlen(left);
    size_t b = strlen(right);
    int separator = a != 0 && left[a - 1] != '/' && left[a - 1] != '\\';
    char *result = (char *)malloc(a + (size_t)separator + b + 1);
    if (result == NULL) return NULL;
    memcpy(result, left, a);
    if (separator) {
#ifdef _WIN32
        result[a++] = '\\';
#else
        result[a++] = '/';
#endif
    }
    memcpy(result + a, right, b + 1);
    return result;
}

static const char *platform_id(void) {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    return "windows-aarch64";
#elif defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-aarch64";
#elif defined(__APPLE__)
    return "macos-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-aarch64";
#elif defined(__linux__)
    return "linux-x86_64";
#else
    return NULL;
#endif
}

static const char *operating_system_id(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return NULL;
#endif
}

static int open_web_url(const char *url) {
#ifdef _WIN32
    return (INT_PTR)ShellExecuteA(NULL, "open", url, NULL, NULL,
                                  SW_SHOWNORMAL) > 32;
#else
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
#if defined(__APPLE__)
        execlp("open", "open", url, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
        _exit(127);
    }
    return 1;
#endif
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

static int make_directories(const char *path) {
    char *copy = copy_text(path);
    char *cursor;
    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
#ifdef _WIN32
            if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
            *cursor = '\0';
            if (copy[0] != '\0' && !make_directory(copy)) {
                free(copy);
                return 0;
            }
            *cursor =
#ifdef _WIN32
                '\\';
#else
                '/';
#endif
        }
    }
    if (!make_directory(copy)) {
        free(copy);
        return 0;
    }
    free(copy);
    return 1;
}

static int copy_file(const char *source, const char *destination,
                     uint64_t maximum, char *error, size_t error_size) {
    FILE *input = fopen(source, "rb");
    FILE *output;
    unsigned char buffer[64 * 1024];
    size_t count;
    uint64_t total = 0;
    int ok = 1;
    if (input == NULL) {
        report_error(error, error_size, "could not open '%s'", source);
        return 0;
    }
    output = fopen(destination, "wb");
    if (output == NULL) {
        fclose(input);
        report_error(error, error_size, "could not create '%s'", destination);
        return 0;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        total += (uint64_t)count;
        if (total > maximum || fwrite(buffer, 1, count, output) != count) {
            ok = 0;
            break;
        }
    }
    if (ferror(input)) ok = 0;
    if (fclose(input) != 0) ok = 0;
    if (fclose(output) != 0) ok = 0;
    if (!ok) {
        remove(destination);
        report_error(error, error_size,
                     "the downloaded file is invalid, too large, or incomplete");
    }
    return ok;
}

static char *read_text_file(const char *path, size_t maximum,
                            char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    long length;
    char *contents;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 || (uint64_t)length > maximum ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        report_error(error, error_size, "could not read the update manifest");
        return NULL;
    }
    contents = (char *)malloc((size_t)length + 1);
    if (contents == NULL) {
        fclose(file);
        report_error(error, error_size, "out of memory");
        return NULL;
    }
    if (fread(contents, 1, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(contents);
        report_error(error, error_size, "could not read the update manifest");
        return NULL;
    }
    contents[length] = '\0';
    return contents;
}

static uint16_t read_u16(const unsigned char *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int safe_zip_member(const char *path, const char *platform) {
    char expected[128];
    const char *cursor;
    if (snprintf(expected, sizeof(expected), "runtimes/%s/", platform) < 0 ||
        strncmp(path, expected, strlen(expected)) != 0 ||
        path[strlen(expected)] == '\0') return 0;
    if (path[0] == '/' || path[0] == '\\' || strchr(path, '\\') != NULL)
        return 0;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if ((cursor == path || cursor[-1] == '/') && cursor[0] == '.' &&
            cursor[1] == '.' && (cursor[2] == '/' || cursor[2] == '\0'))
            return 0;
    }
    return 1;
}

static int extract_stored_zip_member(const char *archive_path,
                                     const char *member,
                                     const char *destination,
                                     uint64_t expected_size,
                                     char *error, size_t error_size) {
    FILE *archive = fopen(archive_path, "rb");
    FILE *output = NULL;
    unsigned char *tail = NULL;
    unsigned char central[46];
    unsigned char local[30];
    unsigned char buffer[64 * 1024];
    long archive_length;
    long tail_start;
    size_t tail_size;
    long eocd = -1;
    uint32_t central_offset;
    uint32_t central_size;
    uint16_t entry_count;
    uint16_t entry_index;
    int ok = 0;
    if (archive == NULL || fseek(archive, 0, SEEK_END) != 0 ||
        (archive_length = ftell(archive)) < 22 ||
        (uint64_t)archive_length > INSTALLER_ARCHIVE_LIMIT) goto done;
    tail_start = archive_length > 65557 ? archive_length - 65557 : 0;
    tail_size = (size_t)(archive_length - tail_start);
    tail = (unsigned char *)malloc(tail_size);
    if (tail == NULL || fseek(archive, tail_start, SEEK_SET) != 0 ||
        fread(tail, 1, tail_size, archive) != tail_size) goto done;
    {
        long cursor;
        for (cursor = (long)tail_size - 22; cursor >= 0; cursor--) {
            if (read_u32(tail + cursor) == 0x06054b50u) {
                eocd = cursor;
                break;
            }
        }
    }
    if (eocd < 0 || read_u16(tail + eocd + 4) != 0 ||
        read_u16(tail + eocd + 6) != 0 ||
        read_u16(tail + eocd + 8) != read_u16(tail + eocd + 10)) goto done;
    entry_count = read_u16(tail + eocd + 10);
    central_size = read_u32(tail + eocd + 12);
    central_offset = read_u32(tail + eocd + 16);
    if ((uint64_t)central_offset + central_size > (uint64_t)archive_length ||
        fseek(archive, (long)central_offset, SEEK_SET) != 0) goto done;
    for (entry_index = 0; entry_index < entry_count; entry_index++) {
        uint16_t flags;
        uint16_t method;
        uint16_t name_length;
        uint16_t extra_length;
        uint16_t comment_length;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint32_t local_offset;
        char *name;
        long next_entry;
        if (fread(central, 1, sizeof(central), archive) != sizeof(central) ||
            read_u32(central) != 0x02014b50u) goto done;
        flags = read_u16(central + 8);
        method = read_u16(central + 10);
        compressed_size = read_u32(central + 20);
        uncompressed_size = read_u32(central + 24);
        name_length = read_u16(central + 28);
        extra_length = read_u16(central + 30);
        comment_length = read_u16(central + 32);
        local_offset = read_u32(central + 42);
        name = (char *)malloc((size_t)name_length + 1);
        if (name == NULL || fread(name, 1, name_length, archive) != name_length) {
            free(name);
            goto done;
        }
        name[name_length] = '\0';
        next_entry = ftell(archive) + extra_length + comment_length;
        if (strcmp(name, member) == 0) {
            uint16_t local_name;
            uint16_t local_extra;
            uint64_t remaining;
            free(name);
            if ((flags & 1u) != 0 || method != 0 ||
                compressed_size != uncompressed_size ||
                uncompressed_size != expected_size ||
                (uint64_t)local_offset + sizeof(local) >
                    (uint64_t)archive_length ||
                fseek(archive, (long)local_offset, SEEK_SET) != 0 ||
                fread(local, 1, sizeof(local), archive) != sizeof(local) ||
                read_u32(local) != 0x04034b50u) goto done;
            local_name = read_u16(local + 26);
            local_extra = read_u16(local + 28);
            if ((uint64_t)local_offset + sizeof(local) + local_name +
                    local_extra + uncompressed_size >
                (uint64_t)archive_length ||
                fseek(archive, local_name + local_extra, SEEK_CUR) != 0)
                goto done;
            output = fopen(destination, "wb");
            if (output == NULL) goto done;
            remaining = uncompressed_size;
            while (remaining != 0) {
                size_t wanted = remaining > sizeof(buffer)
                    ? sizeof(buffer) : (size_t)remaining;
                if (fread(buffer, 1, wanted, archive) != wanted ||
                    fwrite(buffer, 1, wanted, output) != wanted) goto done;
                remaining -= wanted;
            }
            if (fclose(output) != 0) {
                output = NULL;
                goto done;
            }
            output = NULL;
            ok = 1;
            goto done;
        }
        free(name);
        if (next_entry < 0 || fseek(archive, next_entry, SEEK_SET) != 0)
            goto done;
    }
done:
    if (output != NULL) fclose(output);
    if (archive != NULL) fclose(archive);
    free(tail);
    if (!ok) {
        remove(destination);
        report_error(error, error_size,
                     "the ZVM archive is invalid or is missing '%s'", member);
    }
    return ok;
}

#ifdef _WIN32

static WCHAR *wide_text(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *result;
    if (count == 0) return NULL;
    result = (WCHAR *)malloc((size_t)count * sizeof(*result));
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, result, count) == 0) {
        free(result);
        return NULL;
    }
    return result;
}

static int download_url(const char *url, const char *destination,
                        uint64_t maximum, char *error, size_t error_size) {
    WCHAR *wide_url = wide_text(url);
    URL_COMPONENTSW parts;
    WCHAR *host = NULL;
    WCHAR *path = NULL;
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    FILE *output = NULL;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    uint64_t total = 0;
    int ok = 0;
    if (wide_url == NULL) goto done;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0)
        goto done;
    host = (WCHAR *)calloc((size_t)parts.dwHostNameLength + 1,
                           sizeof(*host));
    path = (WCHAR *)calloc((size_t)parts.dwUrlPathLength +
                               (size_t)parts.dwExtraInfoLength + 1,
                           sizeof(*path));
    if (host == NULL || path == NULL) goto done;
    memcpy(host, parts.lpszHostName,
           (size_t)parts.dwHostNameLength * sizeof(*host));
    if (parts.dwUrlPathLength != 0)
        memcpy(path, parts.lpszUrlPath,
               (size_t)parts.dwUrlPathLength * sizeof(*path));
    if (parts.dwExtraInfoLength != 0)
        memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
               (size_t)parts.dwExtraInfoLength * sizeof(*path));
    session = WinHttpOpen(L"ZSharp-Installer/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) goto done;
    WinHttpSetTimeouts(session, 20000, 20000, 20000, 600000);
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (connection == NULL) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (request == NULL || !WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX) || status < 200 || status >= 300)
        goto done;
    output = fopen(destination, "wb");
    if (output == NULL) goto done;
    for (;;) {
        unsigned char buffer[64 * 1024];
        DWORD received = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &received))
            goto done;
        if (received == 0) break;
        total += received;
        if (total > maximum || fwrite(buffer, 1, received, output) != received)
            goto done;
    }
    if (fclose(output) != 0) {
        output = NULL;
        goto done;
    }
    output = NULL;
    ok = 1;
done:
    if (output != NULL) fclose(output);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    free(path);
    free(host);
    free(wide_url);
    if (!ok) {
        remove(destination);
        report_error(error, error_size,
                     "could not securely download '%s'", url);
    }
    return ok;
}

#else

static int download_url(const char *url, const char *destination,
                        uint64_t maximum, char *error, size_t error_size) {
    pid_t child;
    int status;
    char maximum_text[32];
    snprintf(maximum_text, sizeof(maximum_text), "%llu",
             (unsigned long long)maximum);
    child = fork();
    if (child == 0) {
        execlp("curl", "curl", "--fail", "--location", "--silent",
               "--show-error", "--proto", "=https", "--tlsv1.2",
               "--connect-timeout", "20", "--max-time", "600",
               "--max-filesize", maximum_text, "--output", destination,
               url, (char *)NULL);
        _exit(127);
    }
    if (child < 0 || waitpid(child, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove(destination);
        report_error(error, error_size,
                     "could not securely download '%s'; curl is required", url);
        return 0;
    }
    return 1;
}

#endif

static const char *skip_space(const char *cursor, const char *end) {
    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static const char *json_value(const char *begin, const char *end,
                              const char *key) {
    size_t key_length = strlen(key);
    const char *cursor = begin;
    while (cursor < end && (cursor = strchr(cursor, '"')) != NULL &&
           cursor < end) {
        const char *name = cursor + 1;
        const char *after;
        if ((size_t)(end - name) > key_length &&
            memcmp(name, key, key_length) == 0 && name[key_length] == '"') {
            after = skip_space(name + key_length + 1, end);
            if (after < end && *after == ':')
                return skip_space(after + 1, end);
        }
        cursor++;
    }
    return NULL;
}

static int json_object(const char *manifest, const char *key,
                       const char **object_begin, const char **object_end) {
    const char *end = manifest + strlen(manifest);
    const char *cursor = json_value(manifest, end, key);
    int depth = 0;
    int string = 0;
    int escape = 0;
    if (cursor == NULL || *cursor != '{') return 0;
    *object_begin = cursor + 1;
    for (; cursor < end; cursor++) {
        if (string) {
            if (escape) escape = 0;
            else if (*cursor == '\\') escape = 1;
            else if (*cursor == '"') string = 0;
            continue;
        }
        if (*cursor == '"') string = 1;
        else if (*cursor == '{') depth++;
        else if (*cursor == '}' && --depth == 0) {
            *object_end = cursor;
            return 1;
        }
    }
    return 0;
}

static char *json_string(const char *begin, const char *end,
                         const char *key) {
    const char *cursor = json_value(begin, end, key);
    const char *scan;
    char *result;
    char *output;
    if (cursor == NULL || cursor >= end || *cursor != '"') return NULL;
    scan = cursor + 1;
    while (scan < end && *scan != '"') {
        if (*scan == '\\') {
            scan++;
            if (scan >= end) return NULL;
        }
        scan++;
    }
    if (scan >= end) return NULL;
    result = (char *)malloc((size_t)(scan - cursor));
    if (result == NULL) return NULL;
    output = result;
    for (cursor++; cursor < scan; cursor++) {
        if (*cursor == '\\') {
            cursor++;
            if (*cursor == 'n') *output++ = '\n';
            else if (*cursor == 'r') *output++ = '\r';
            else if (*cursor == 't') *output++ = '\t';
            else if (*cursor == '"' || *cursor == '\\' || *cursor == '/')
                *output++ = *cursor;
            else {
                free(result);
                return NULL;
            }
        } else {
            *output++ = *cursor;
        }
    }
    *output = '\0';
    return result;
}

static int json_u64(const char *begin, const char *end, const char *key,
                    uint64_t *value) {
    const char *cursor = json_value(begin, end, key);
    uint64_t result = 0;
    int digits = 0;
    if (cursor == NULL) return 0;
    while (cursor < end && isdigit((unsigned char)*cursor)) {
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10u) return 0;
        result = result * 10u + digit;
        cursor++;
        digits++;
    }
    if (!digits) return 0;
    *value = result;
    return 1;
}

static int valid_version(const char *version) {
    const char *cursor = version;
    int parts = 0;
    for (;;) {
        int digits = 0;
        while (isdigit((unsigned char)*cursor)) {
            cursor++;
            digits++;
        }
        if (!digits) return 0;
        parts++;
        if (*cursor == '\0') return parts == 4;
        if (*cursor != '.' || parts >= 4) return 0;
        cursor++;
    }
}

static int valid_sha256(char *sha256) {
    size_t index;
    if (strlen(sha256) != 64) return 0;
    for (index = 0; index < 64; index++) {
        if (!isxdigit((unsigned char)sha256[index])) return 0;
        sha256[index] = (char)tolower((unsigned char)sha256[index]);
    }
    return 1;
}

static int parse_release(const char *manifest, const char *platform,
                         const char *current_version, ReleaseInfo *release,
                         char *error,
                         size_t error_size) {
    const char *begin;
    const char *end;
    const char *download_begin;
    const char *download_end;
    const char *manifest_end = manifest + strlen(manifest);
    uint64_t schema = 0;
    memset(release, 0, sizeof(*release));
    if (!json_u64(manifest, manifest_end, "schema", &schema) || schema != 1) {
        report_error(error, error_size,
                     "the update site returned an unsupported manifest");
        return 0;
    }
    release->version = json_string(manifest, manifest_end, "latestVersion");
    if (release->version == NULL || !valid_version(release->version)) {
        report_error(error, error_size,
                     "the update site returned invalid version metadata");
        return 0;
    }
    release->update_available = strcmp(release->version, current_version) != 0;
    if (!release->update_available) return 1;
    if (!json_object(manifest, "download", &download_begin, &download_end)) {
        report_error(error, error_size,
                     "the update site did not provide the ZVM archive");
        return 0;
    }
    if (!json_object(manifest, platform, &begin, &end)) {
        report_error(error, error_size,
                     "the update site has no runtime for %s", platform);
        return 0;
    }
    release->archive_url = json_string(download_begin, download_end, "url");
    release->archive_sha256 = json_string(download_begin, download_end,
                                           "sha256");
    release->runtime_path = json_string(begin, end, "path");
    release->runtime_sha256 = json_string(begin, end, "sha256");
    if (release->archive_url == NULL || release->archive_sha256 == NULL ||
        release->runtime_path == NULL || release->runtime_sha256 == NULL ||
        !json_u64(download_begin, download_end, "size",
                  &release->archive_size) ||
        !json_u64(begin, end, "size", &release->runtime_size) ||
        strncmp(release->archive_url, "https://", 8) != 0 ||
        !valid_sha256(release->archive_sha256) ||
        !valid_sha256(release->runtime_sha256) ||
        !safe_zip_member(release->runtime_path, platform) ||
        release->archive_size == 0 ||
        release->archive_size > INSTALLER_ARCHIVE_LIMIT ||
        release->runtime_size == 0 ||
        release->runtime_size > INSTALLER_RUNTIME_LIMIT) {
        report_error(error, error_size,
                     "the update site returned invalid release metadata");
        return 0;
    }
    return 1;
}

static void release_free(ReleaseInfo *release) {
    free(release->version);
    free(release->archive_url);
    free(release->archive_sha256);
    free(release->runtime_path);
    free(release->runtime_sha256);
    memset(release, 0, sizeof(*release));
}

static char *default_install_directory(char *error, size_t error_size) {
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if (local == NULL || local[0] == '\0') {
        report_error(error, error_size, "LOCALAPPDATA is not available");
        return NULL;
    }
    {
        char *root = join_path(local, "ZombieOS");
        char *product = root == NULL ? NULL : join_path(root, "ZSharp");
        char *bin = product == NULL ? NULL : join_path(product, "bin");
        free(product);
        free(root);
        return bin;
    }
#else
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        report_error(error, error_size, "HOME is not available");
        return NULL;
    }
    {
        char *local = join_path(home, ".local");
        char *bin = local == NULL ? NULL : join_path(local, "bin");
        free(local);
        return bin;
    }
#endif
}

static uint64_t file_size(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
        return UINT64_MAX;
    return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
#else
    struct stat data;
    if (stat(path, &data) != 0 || data.st_size < 0) return UINT64_MAX;
    return (uint64_t)data.st_size;
#endif
}

static int replace_file(const char *temporary, const char *destination,
                        const char *backup, const char *label,
                        char *error, size_t error_size) {
#ifdef _WIN32
    DWORD existing = GetFileAttributesA(destination);
    if (existing != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(backup);
        if (!MoveFileExA(destination, backup,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            report_error(error, error_size,
                         "could not back up the existing %s", label);
            return 0;
        }
    }
    if (!MoveFileExA(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (existing != INVALID_FILE_ATTRIBUTES)
            MoveFileExA(backup, destination,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        report_error(error, error_size, "could not install %s", label);
        return 0;
    }
#else
    struct stat status;
    int existing = lstat(destination, &status) == 0;
    if (existing) {
        unlink(backup);
        if (rename(destination, backup) != 0) {
            report_error(error, error_size,
                         "could not back up the existing %s", label);
            return 0;
        }
    }
    if (rename(temporary, destination) != 0) {
        if (existing) rename(backup, destination);
        report_error(error, error_size, "could not install %s", label);
        return 0;
    }
#endif
    return 1;
}

static char *current_executable(char *error, size_t error_size) {
#ifdef _WIN32
    WCHAR wide_path[32768];
    DWORD length = GetModuleFileNameW(NULL, wide_path,
                                     (DWORD)(sizeof(wide_path) /
                                             sizeof(wide_path[0])));
    int bytes;
    char *path;
    if (length == 0 ||
        length >= (DWORD)(sizeof(wide_path) / sizeof(wide_path[0])))
        goto failed;
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, NULL, 0,
                                NULL, NULL);
    path = bytes == 0 ? NULL : (char *)malloc((size_t)bytes);
    if (path == NULL || WideCharToMultiByte(CP_UTF8, 0, wide_path, -1,
                                             path, bytes, NULL, NULL) == 0) {
        free(path);
        goto failed;
    }
    return path;
#elif defined(__linux__)
    {
        size_t capacity = 1024;
        for (;;) {
            char *path = (char *)malloc(capacity);
            ssize_t length;
            if (path == NULL) break;
            length = readlink("/proc/self/exe", path, capacity - 1);
            if (length >= 0 && (size_t)length < capacity - 1) {
                path[length] = '\0';
                return path;
            }
            free(path);
            if (capacity >= 65536) break;
            capacity *= 2;
        }
    }
#else
    {
        uint32_t capacity = 1024;
        for (;;) {
            char *path = (char *)malloc(capacity);
            uint32_t needed = capacity;
            if (path == NULL) break;
            if (_NSGetExecutablePath(path, &needed) == 0) {
                char *resolved = realpath(path, NULL);
                free(path);
                if (resolved != NULL) return resolved;
                break;
            }
            free(path);
            if (needed > 65536u) break;
            capacity = needed;
        }
    }
#endif
#ifdef _WIN32
failed:
#endif
    report_error(error, error_size,
                 "could not locate the running Z# installer");
    return NULL;
}

static int same_path(const char *left, const char *right) {
#ifdef _WIN32
    char left_full[32768];
    char right_full[32768];
    DWORD a = GetFullPathNameA(left, sizeof(left_full), left_full, NULL);
    DWORD b = GetFullPathNameA(right, sizeof(right_full), right_full, NULL);
    return a != 0 && a < (DWORD)sizeof(left_full) && b != 0 &&
           b < (DWORD)sizeof(right_full) &&
           _stricmp(left_full, right_full) == 0;
#else
    char *a = realpath(left, NULL);
    char *b = realpath(right, NULL);
    int same = a != NULL && b != NULL && strcmp(a, b) == 0;
    free(a);
    free(b);
    return same;
#endif
}

static void wait_for_process(unsigned long process_id) {
    if (process_id == 0) return;
#ifdef _WIN32
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process_id);
        if (process != NULL) {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }
    }
#else
    while (kill((pid_t)process_id, 0) == 0 || errno == EPERM) sleep(1);
#endif
}

#ifdef _WIN32

static int path_contains(const WCHAR *path, const WCHAR *directory) {
    const WCHAR *cursor = path;
    size_t wanted = wcslen(directory);
    while (*cursor != L'\0') {
        const WCHAR *end = wcschr(cursor, L';');
        size_t length = end == NULL ? wcslen(cursor) : (size_t)(end - cursor);
        while (length > 0 && iswspace(cursor[0])) {
            cursor++;
            length--;
        }
        while (length > 0 && iswspace(cursor[length - 1])) length--;
        if (length == wanted && _wcsnicmp(cursor, directory, wanted) == 0)
            return 1;
        if (end == NULL) break;
        cursor = end + 1;
    }
    return 0;
}

static int add_to_user_path(const char *directory, char *error,
                            size_t error_size) {
    HKEY key;
    WCHAR *wide_directory = wide_text(directory);
    WCHAR *old_path = NULL;
    WCHAR *new_path = NULL;
    DWORD bytes = 0;
    DWORD type = REG_EXPAND_SZ;
    LONG result;
    int ok = 0;
    if (wide_directory == NULL) goto done;
    result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, NULL, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
    if (result != ERROR_SUCCESS) goto done;
    result = RegQueryValueExW(key, L"Path", NULL, &type, NULL, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) {
        bytes = sizeof(WCHAR);
        type = REG_EXPAND_SZ;
    } else if (result != ERROR_SUCCESS ||
               (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        goto done;
    }
    old_path = (WCHAR *)calloc((size_t)bytes / sizeof(WCHAR) + 2,
                               sizeof(*old_path));
    if (old_path == NULL) {
        RegCloseKey(key);
        goto done;
    }
    if (bytes > sizeof(WCHAR)) {
        result = RegQueryValueExW(key, L"Path", NULL, &type,
                                  (BYTE *)old_path, &bytes);
        if (result != ERROR_SUCCESS) {
            RegCloseKey(key);
            goto done;
        }
    }
    if (path_contains(old_path, wide_directory)) {
        RegCloseKey(key);
        ok = 1;
        goto done;
    }
    new_path = (WCHAR *)malloc((wcslen(old_path) + wcslen(wide_directory) + 3) *
                               sizeof(*new_path));
    if (new_path == NULL) {
        RegCloseKey(key);
        goto done;
    }
    if (old_path[0] == L'\0') wcscpy(new_path, wide_directory);
    else swprintf(new_path, wcslen(old_path) + wcslen(wide_directory) + 3,
                  L"%ls;%ls", old_path, wide_directory);
    result = RegSetValueExW(key, L"Path", 0, type, (BYTE *)new_path,
                            ((DWORD)wcslen(new_path) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    if (result == ERROR_SUCCESS) {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            (LPARAM)L"Environment", SMTO_ABORTIFHUNG,
                            5000, NULL);
        ok = 1;
    }
done:
    free(new_path);
    free(old_path);
    free(wide_directory);
    if (!ok) report_error(error, error_size,
                          "could not add the Z# bin directory to your PATH");
    return ok;
}

static int run_associate(const char *runtime) {
    WCHAR *wide_runtime = wide_text(runtime);
    WCHAR *command;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD code = 1;
    size_t capacity;
    int ok = 0;
    if (wide_runtime == NULL) return 0;
    capacity = wcslen(wide_runtime) + 16;
    command = (WCHAR *)malloc(capacity * sizeof(*command));
    if (command == NULL) { free(wide_runtime); return 0; }
    swprintf(command, capacity, L"\"%ls\" associate", wide_runtime);
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    SetEnvironmentVariableW(L"ZSHARP_SKIP_UPDATE_CHECK", L"1");
    if (CreateProcessW(wide_runtime, command, NULL, NULL, FALSE, 0, NULL, NULL,
                       &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ok = code == 0;
    }
    SetEnvironmentVariableW(L"ZSHARP_SKIP_UPDATE_CHECK", NULL);
    free(command);
    free(wide_runtime);
    return ok;
}

#else

static int run_associate(const char *runtime) {
    pid_t child = fork();
    int status;
    if (child == 0) {
        setenv("ZSHARP_SKIP_UPDATE_CHECK", "1", 1);
        execl(runtime, runtime, "associate", (char *)NULL);
        _exit(127);
    }
    return child > 0 && waitpid(child, &status, 0) >= 0 &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

static void print_help(void) {
    puts("Z# ZVM bootstrap installer\n");
    puts("Usage:");
    puts("  zsharp-installer [--yes] [--manifest URL] [--install-dir PATH]");
    puts("  zsharp-installer --check --current-version VERSION [--quiet]");
    puts("  zsharp-installer --manifest-file FILE [--yes] [--install-dir PATH]");
}

static void wait_on_windows(int automatic) {
#ifdef _WIN32
    if (!automatic) {
        char line[8];
        fputs("Press Enter to close...", stdout);
        fflush(stdout);
        fgets(line, sizeof(line), stdin);
    }
#else
    (void)automatic;
#endif
}

int main(int argc, char **argv) {
    const char *platform = platform_id();
    const char *operating_system = operating_system_id();
    const char *manifest_url = getenv("ZSHARP_INSTALLER_MANIFEST_URL");
    const char *manifest_file = getenv("ZSHARP_INSTALLER_MANIFEST_FILE");
    const char *artifact_override = getenv("ZSHARP_INSTALLER_ARTIFACT_FILE");
    const char *directory_argument = getenv("ZSHARP_INSTALLER_INSTALL_DIR");
    const char *current_version = "0.0.0.0";
    int automatic = 0;
    int check_mode = 0;
    int quiet = 0;
    int custom_manifest = manifest_url != NULL && manifest_url[0] != '\0';
    unsigned long wait_process_id = 0;
    int index;
    int result = 1;
    char error[512] = {0};
    char manifest_temp[512] = {0};
    char *manifest_url_owned = NULL;
    char *manifest = NULL;
    char *install_directory = NULL;
    char *runtime = NULL;
    char *runtime_backup = NULL;
    char *archive = NULL;
    char *runtime_temporary = NULL;
    char *installed_installer = NULL;
    char *installer_backup = NULL;
    char *installer_temporary = NULL;
    char *self = NULL;
    ReleaseInfo release;
    unsigned char digest[ZSHARP_SHA256_SIZE];
    char digest_hex[ZSHARP_SHA256_SIZE * 2 + 1];
#ifdef _WIN32
    HANDLE update_lock = NULL;
#else
    int update_lock = -1;
    char *update_lock_path = NULL;
#endif
    memset(&release, 0, sizeof(release));
    if (manifest_file != NULL && manifest_file[0] == '\0') manifest_file = NULL;
    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--yes") == 0) automatic = 1;
        else if (strcmp(argv[index], "--check") == 0) {
            check_mode = 1;
            automatic = 1;
        } else if (strcmp(argv[index], "--quiet") == 0) quiet = 1;
        else if (strcmp(argv[index], "--help") == 0 ||
                 strcmp(argv[index], "-h") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_url = argv[++index];
            custom_manifest = 1;
        } else if (strcmp(argv[index], "--manifest-file") == 0 &&
                   index + 1 < argc) {
            manifest_file = argv[++index];
        } else if (strcmp(argv[index], "--install-dir") == 0 &&
                   index + 1 < argc) {
            directory_argument = argv[++index];
        } else if (strcmp(argv[index], "--current-version") == 0 &&
                   index + 1 < argc) {
            current_version = argv[++index];
        } else if (strcmp(argv[index], "--wait-pid") == 0 &&
                   index + 1 < argc) {
            char *end = NULL;
            wait_process_id = strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0') {
                fprintf(stderr, "installer error: invalid process ID\n");
                return 2;
            }
        } else {
            fprintf(stderr, "installer error: unknown or incomplete option '%s'\n",
                    argv[index]);
            print_help();
            return 2;
        }
    }
    if (platform == NULL || operating_system == NULL) {
        strcpy(error, "this operating system or CPU is not supported");
        goto done;
    }
    if (!valid_version(current_version)) {
        strcpy(error, "the current Z# version must have four numeric parts");
        goto done;
    }
    if (!custom_manifest) {
        manifest_url_owned = (char *)malloc(strlen(INSTALLER_UPDATE_ENDPOINT) +
                                             strlen(current_version) + 1 +
                                             strlen(operating_system) + 1);
        if (manifest_url_owned == NULL) {
            strcpy(error, "out of memory");
            goto done;
        }
        sprintf(manifest_url_owned, "%s%s-%s", INSTALLER_UPDATE_ENDPOINT,
                current_version, operating_system);
        manifest_url = manifest_url_owned;
    }
    install_directory = directory_argument == NULL || directory_argument[0] == '\0'
        ? default_install_directory(error, sizeof(error))
        : copy_text(directory_argument);
    if (install_directory == NULL) goto done;
    if (check_mode) {
#ifdef _WIN32
        update_lock = CreateMutexW(NULL, FALSE,
                                   L"Local\\ZombieOS.ZSharp.UpdateCheck");
        if (update_lock == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
            result = 0;
            goto done;
        }
#else
        update_lock_path = join_path(install_directory, ".zsharp-update.lock");
        if (update_lock_path == NULL || !make_directories(install_directory))
            goto done;
        update_lock = open(update_lock_path, O_CREAT | O_RDWR, 0600);
        if (update_lock < 0 || flock(update_lock, LOCK_EX | LOCK_NB) != 0) {
            result = 0;
            goto done;
        }
#endif
    }
    if (!quiet) printf("Z# ZVM installer for %s\n", platform);
    if (manifest_file == NULL) {
#ifdef _WIN32
        snprintf(manifest_temp, sizeof(manifest_temp), "%s\\zsharp-manifest-%lu.tmp",
                 getenv("TEMP") == NULL ? "." : getenv("TEMP"),
                 (unsigned long)GetCurrentProcessId());
#else
        snprintf(manifest_temp, sizeof(manifest_temp), "%s/zsharp-manifest-%lu.tmp",
                 getenv("TMPDIR") == NULL ? "/tmp" : getenv("TMPDIR"),
                 (unsigned long)getpid());
#endif
        if (!quiet) printf("Checking %s\n", manifest_url);
        if (!download_url(manifest_url, manifest_temp,
                          INSTALLER_MANIFEST_LIMIT, error, sizeof(error)))
            goto done;
        manifest_file = manifest_temp;
    } else {
        manifest_temp[0] = '\0';
    }
    manifest = read_text_file(manifest_file, INSTALLER_MANIFEST_LIMIT,
                              error, sizeof(error));
    if (manifest == NULL ||
        !parse_release(manifest, platform, current_version, &release,
                       error, sizeof(error))) goto done;
    if (!release.update_available) {
        if (!quiet) printf("Z# %s is already current.\n", release.version);
        result = 0;
        goto done;
    }
    if (!quiet)
        printf("Latest Z# version: %s\nInstall location: %s\n",
               release.version, install_directory);
    if (!automatic) {
        char answer[16];
        fputs("Install the Z# Virtual Machine? [y/N]: ", stdout);
        fflush(stdout);
        if (fgets(answer, sizeof(answer), stdin) == NULL ||
            (strcmp(answer, "y\n") != 0 && strcmp(answer, "y\r\n") != 0 &&
             strcmp(answer, "yes\n") != 0 &&
             strcmp(answer, "yes\r\n") != 0)) {
            puts("Installation cancelled; nothing was changed.");
            result = 0;
            goto done;
        }
    }
    if (!make_directories(install_directory)) {
        report_error(error, sizeof(error), "could not create '%s'",
                     install_directory);
        goto done;
    }
#ifdef _WIN32
    runtime = join_path(install_directory, "zsharp.exe");
    runtime_backup = join_path(install_directory, "zsharp.previous.exe");
    installed_installer = join_path(install_directory, "zsharp-installer.exe");
    installer_backup = join_path(install_directory,
                                 "zsharp-installer.previous.exe");
    {
        char name[96];
        unsigned long pid = (unsigned long)GetCurrentProcessId();
        snprintf(name, sizeof(name), ".zsharp-archive-%lu.tmp", pid);
        archive = join_path(install_directory, name);
        snprintf(name, sizeof(name), ".zsharp-runtime-%lu.tmp", pid);
        runtime_temporary = join_path(install_directory, name);
        snprintf(name, sizeof(name), ".zsharp-installer-%lu.tmp", pid);
        installer_temporary = join_path(install_directory, name);
    }
#else
    runtime = join_path(install_directory, "zsharp");
    runtime_backup = join_path(install_directory, "zsharp.previous");
    installed_installer = join_path(install_directory, "zsharp-installer");
    installer_backup = join_path(install_directory,
                                 "zsharp-installer.previous");
    {
        char name[96];
        unsigned long pid = (unsigned long)getpid();
        snprintf(name, sizeof(name), ".zsharp-archive-%lu.tmp", pid);
        archive = join_path(install_directory, name);
        snprintf(name, sizeof(name), ".zsharp-runtime-%lu.tmp", pid);
        runtime_temporary = join_path(install_directory, name);
        snprintf(name, sizeof(name), ".zsharp-installer-%lu.tmp", pid);
        installer_temporary = join_path(install_directory, name);
    }
#endif
    if (runtime == NULL || runtime_backup == NULL || archive == NULL ||
        runtime_temporary == NULL || installed_installer == NULL ||
        installer_backup == NULL || installer_temporary == NULL) {
        strcpy(error, "out of memory");
        goto done;
    }
    if (!quiet) puts("Downloading the verified ZVM archive...");
    if (artifact_override != NULL && artifact_override[0] != '\0') {
        if (!copy_file(artifact_override, archive, INSTALLER_ARCHIVE_LIMIT,
                       error, sizeof(error))) goto done;
    } else if (!download_url(release.archive_url, archive,
                             release.archive_size,
                             error, sizeof(error))) goto done;
    if (file_size(archive) != release.archive_size ||
        !zsharp_sha256_file(archive, digest)) {
        strcpy(error, "the downloaded archive size or checksum could not be read");
        goto done;
    }
    zsharp_hash_hex(digest, digest_hex);
    if (strcmp(digest_hex, release.archive_sha256) != 0) {
        strcpy(error, "the downloaded ZVM archive failed SHA-256 verification");
        goto done;
    }
    if (!extract_stored_zip_member(archive, release.runtime_path,
                                   runtime_temporary,
                                   release.runtime_size, error,
                                   sizeof(error)) ||
        !zsharp_sha256_file(runtime_temporary, digest)) {
        if (error[0] == '\0')
            strcpy(error, "the extracted runtime checksum could not be read");
        goto done;
    }
    zsharp_hash_hex(digest, digest_hex);
    if (strcmp(digest_hex, release.runtime_sha256) != 0) {
        strcpy(error, "the extracted runtime failed SHA-256 verification");
        goto done;
    }
#ifndef _WIN32
    if (chmod(runtime_temporary, 0755) != 0) {
        strcpy(error, "could not make the downloaded ZVM executable");
        goto done;
    }
#endif
    self = current_executable(error, sizeof(error));
    if (self == NULL) goto done;
    if (!same_path(self, installed_installer)) {
        if (!copy_file(self, installer_temporary, 32u * 1024u * 1024u,
                       error, sizeof(error))) goto done;
#ifndef _WIN32
        if (chmod(installer_temporary, 0755) != 0) {
            strcpy(error, "could not make the installed updater executable");
            goto done;
        }
#endif
    }
    wait_for_process(wait_process_id);
    if (!same_path(self, installed_installer) &&
        !replace_file(installer_temporary, installed_installer,
                      installer_backup, "Z# updater", error,
                      sizeof(error))) goto done;
    if (!replace_file(runtime_temporary, runtime, runtime_backup,
                      "Z# runtime", error, sizeof(error)))
        goto done;
    if (!check_mode && getenv("ZSHARP_INSTALLER_SKIP_INTEGRATION") == NULL) {
#ifdef _WIN32
        char integration_error[512] = {0};
        if (!add_to_user_path(install_directory, integration_error,
                              sizeof(integration_error)))
            fprintf(stderr, "PATH warning: %s\n", integration_error);
#endif
        if (!run_associate(runtime))
            fputs("association warning: run 'zsharp associate' later\n", stderr);
    }
    if (!quiet)
        printf("\nZ# %s installed successfully.\nZVM: %s\n",
               release.version, runtime);
#ifndef _WIN32
    {
        const char *path = getenv("PATH");
        if (path == NULL || strstr(path, install_directory) == NULL)
            if (!quiet)
                printf("Add %s to PATH to use 'zsharp' in new terminals.\n",
                       install_directory);
    }
#else
    if (!quiet) puts("Open a new terminal before using the 'zsharp' command.");
#endif
    if (!automatic && !check_mode && !quiet) {
        char answer[16];
        puts("\nWant to download our test app?");
        puts("To test if it installed correctly!");
        fputs("Download the Z# Test App? [y/N]: ", stdout);
        fflush(stdout);
        if (fgets(answer, sizeof(answer), stdin) != NULL &&
            (strcmp(answer, "y\n") == 0 ||
             strcmp(answer, "y\r\n") == 0 ||
             strcmp(answer, "yes\n") == 0 ||
             strcmp(answer, "yes\r\n") == 0)) {
            if (!open_web_url(INSTALLER_TEST_APP_URL))
                fprintf(stderr, "download warning: open %s in your browser\n",
                        INSTALLER_TEST_APP_URL);
        }
    }
    result = 0;
done:
    if (result != 0 && error[0] != '\0' && !quiet)
        fprintf(stderr, "installer error: %s\n", error);
    if (archive != NULL) remove(archive);
    if (runtime_temporary != NULL) remove(runtime_temporary);
    if (installer_temporary != NULL) remove(installer_temporary);
    if (manifest_temp[0] != '\0') remove(manifest_temp);
#ifdef _WIN32
    if (update_lock != NULL) CloseHandle(update_lock);
#else
    if (update_lock >= 0) close(update_lock);
    free(update_lock_path);
#endif
    release_free(&release);
    free(self);
    free(installer_temporary);
    free(installer_backup);
    free(installed_installer);
    free(runtime_temporary);
    free(archive);
    free(runtime_backup);
    free(runtime);
    free(install_directory);
    free(manifest);
    free(manifest_url_owned);
    wait_on_windows(automatic);
    return result;
}
