#define _CRT_SECURE_NO_WARNINGS

#include "hub.h"

#include "bytecode.h"
#include "package.h"
#include "settings.h"
#include "registry.h"
#include "updater.h"
#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

typedef struct HubState {
    ZSharpInstalledPackageList packages;
} HubState;

#define ZSHARP_DEFAULT_ICON_URL \
    "https://www.zsharp.zombieos.com/zsharp.png"

static int set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s", message);
    return 0;
}

static char *hub_copy(const char *value) {
    return value == NULL ? NULL : zsharp_copy_text(value, strlen(value));
}

static char *hub_join_path(const char *left, const char *right) {
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

static int hub_path_is_file(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
#endif
}

static int hub_make_directory(const char *path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static int hub_make_directories(const char *path) {
    char *copy = hub_copy(path);
    char *cursor;
    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
#ifdef _WIN32
            if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
            *cursor = '\0';
            if (copy[0] != '\0' && !hub_make_directory(copy)) {
                free(copy);
                return 0;
            }
#ifdef _WIN32
            *cursor = '\\';
#else
            *cursor = '/';
#endif
        }
    }
    if (!hub_make_directory(copy)) {
        free(copy);
        return 0;
    }
    free(copy);
    return 1;
}

static int valid_png_file(const char *path) {
    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    unsigned char actual[8];
    FILE *file = fopen(path, "rb");
    int valid = file != NULL && fread(actual, 1, sizeof(actual), file) ==
                sizeof(actual) && memcmp(actual, signature, sizeof(actual)) == 0;
    if (file != NULL) fclose(file);
    return valid;
}

static char *default_icon_cache_path(void) {
    const char *override = getenv("ZSHARP_HUB_DEFAULT_ICON");
    const char *base;
    char *directory;
    char *path;
    if (override != NULL && override[0] != '\0')
        return hub_path_is_file(override) ? hub_copy(override) : NULL;
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base == NULL || base[0] == '\0') return NULL;
    directory = hub_join_path(base, "ZombieOS\\ZSharp\\hub");
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0') return NULL;
    directory = hub_join_path(base,
                              "Library/Application Support/ZombieOS/ZSharp/hub");
#else
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] != '\0')
        directory = hub_join_path(base, "zsharp/hub");
    else {
        base = getenv("HOME");
        if (base == NULL || base[0] == '\0') return NULL;
        directory = hub_join_path(base, ".local/share/zsharp/hub");
    }
#endif
    if (directory == NULL || !hub_make_directories(directory)) {
        free(directory);
        return NULL;
    }
    path = hub_join_path(directory, "zsharp.png");
    free(directory);
    return path;
}

#ifdef _WIN32
static int download_default_icon(const char *destination) {
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    FILE *output = NULL;
    unsigned char buffer[16384];
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    DWORD read = 0;
    size_t total = 0;
    int ok = 0;
    session = WinHttpOpen(L"ZSharp-Hub/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) goto done;
    connection = WinHttpConnect(session, L"www.zsharp.zombieos.com",
                                INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) goto done;
    request = WinHttpOpenRequest(connection, L"GET", L"/zsharp.png", NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (request == NULL || !WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request,
             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
             WINHTTP_NO_HEADER_INDEX) || status != 200) goto done;
    output = fopen(destination, "wb");
    if (output == NULL) goto done;
    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) goto done;
        total += read;
        if (total > 8u * 1024u * 1024u ||
            (read != 0 && fwrite(buffer, 1, read, output) != read)) goto done;
    } while (read != 0);
    if (fclose(output) != 0) {
        output = NULL;
        goto done;
    }
    output = NULL;
    ok = valid_png_file(destination);
done:
    if (output != NULL) fclose(output);
    if (!ok) remove(destination);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    return ok;
}
#else
static int download_default_icon(const char *destination) {
    pid_t child = fork();
    int status = 0;
    if (child < 0) return 0;
    if (child == 0) {
        execl("/usr/bin/curl", "curl", "-fsSL", "--proto", "=https",
              "--tlsv1.2", "--max-filesize", "8388608", "-o", destination,
              ZSHARP_DEFAULT_ICON_URL, (char *)NULL);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || !valid_png_file(destination)) {
        remove(destination);
        return 0;
    }
    return 1;
}
#endif

static char *default_icon_path(void) {
    char *path = default_icon_cache_path();
    if (path == NULL) return NULL;
    if (!valid_png_file(path) && !download_default_icon(path)) {
        free(path);
        return NULL;
    }
    return path;
}

static void prepare_package_icons(ZSharpInstalledPackageList *packages) {
    char *fallback = NULL;
    size_t index;
    for (index = 0; index < packages->count; index++) {
        ZSharpInstalledPackage *item = &packages->items[index];
        ZSharpPackageInfo info;
        ZSharpSettings settings;
        ZSharpDiagnostic diagnostic;
        char local_error[512] = {0};
        char *root = NULL;
        int new_install = 0;
        memset(&info, 0, sizeof(info));
        memset(&settings, 0, sizeof(settings));
        memset(&diagnostic, 0, sizeof(diagnostic));
        if (zsharp_package_extract(item->package_path, &root, &info,
                                   &new_install, local_error,
                                   sizeof(local_error)) &&
            zsharp_settings_load(root, &settings, &diagnostic, local_error,
                                 sizeof(local_error)) &&
            settings.icon != NULL) {
            char *candidate = hub_join_path(root, settings.icon);
            if (candidate != NULL && hub_path_is_file(candidate))
                item->icon_path = candidate;
            else
                free(candidate);
        }
        if (settings.project_name != NULL)
            memcpy(item->zsharp_version, settings.zsharp_version,
                   sizeof(item->zsharp_version));
        zsharp_settings_free(&settings);
        zsharp_package_info_free(&info);
        free(root);
        (void)new_install;
    }
    for (index = 0; index < packages->count; index++) {
        if (packages->items[index].icon_path != NULL) continue;
        if (fallback == NULL) fallback = default_icon_path();
        if (fallback != NULL)
            packages->items[index].icon_path = hub_copy(fallback);
    }
    free(fallback);
}

static int add_property(ZSharpUIElement *element, const char *name,
                        ZSharpUIPropertyType type, const char *value,
                        int status, ZSharpUIUnit unit) {
    ZSharpUIProperty *property = zsharp_ui_element_add_property(element);
    if (property == NULL) return 0;
    property->name = zsharp_copy_text(name, strlen(name));
    property->type = type;
    property->status_value = status;
    property->unit = unit;
    if (value != NULL)
        property->text_value = zsharp_copy_text(value, strlen(value));
    return property->name != NULL &&
           (value == NULL || property->text_value != NULL);
}

static int add_measurement(ZSharpUIElement *element, const char *name,
                           int value) {
    char number[32];
    snprintf(number, sizeof(number), "%d", value);
    return add_property(element, name, ZUI_PROPERTY_MEASUREMENT, number, 0,
                        ZUI_UNIT_PX);
}

static ZSharpUIElement *add_element(ZSharpProgram *program,
                                    ZSharpUIElementType type,
                                    const char *name) {
    ZSharpUIElement *element = zsharp_window_add_element(&program->window);
    if (element == NULL) return NULL;
    element->is_public = 1;
    element->type = type;
    element->name = zsharp_copy_text(name, strlen(name));
    return element->name == NULL ? NULL : element;
}

static int add_text(ZSharpProgram *program, const char *name,
                    const char *content, const char *color,
                    int x, int y, int width, int height,
                    const char *variant, int font_size, int bold) {
    ZSharpUIElement *element = add_element(program, ZUI_TEXT, name);
    if (element == NULL) return 0;
    if (variant != NULL)
        element->variant = zsharp_copy_text(variant, strlen(variant));
    return (variant == NULL || element->variant != NULL) &&
           add_property(element, "content", ZUI_PROPERTY_TEXT, content, 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "color", ZUI_PROPERTY_COLOR, color, 0,
                        ZUI_UNIT_NONE) &&
           add_measurement(element, "locationX", x) &&
           add_measurement(element, "locationY", y) &&
           add_measurement(element, "width", width) &&
           add_measurement(element, "height", height) &&
           add_measurement(element, "fontSize", font_size) &&
           add_property(element, "fontWeight", ZUI_PROPERTY_IDENTIFIER,
                        bold ? "bold" : "normal", 0, ZUI_UNIT_NONE);
}

static int add_button(ZSharpProgram *program, const char *name,
                      const char *text, const char *target,
                      int x, int y, int width, const char *background) {
    ZSharpUIElement *element = add_element(program, ZUI_BUTTON, name);
    return element != NULL &&
           add_property(element, "text", ZUI_PROPERTY_TEXT, text, 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "textColor", ZUI_PROPERTY_COLOR, "#FFFFFF",
                        0, ZUI_UNIT_NONE) &&
           add_property(element, "buttonColor", ZUI_PROPERTY_COLOR,
                        background, 0, ZUI_UNIT_NONE) &&
           add_property(element, "hoverButtonColor", ZUI_PROPERTY_COLOR,
                         "#34344A", 0, ZUI_UNIT_NONE) &&
           add_measurement(element, "borderWidth", 0) &&
           add_measurement(element, "borderRadius", 6) &&
           add_measurement(element, "fontSize", 13) &&
           add_measurement(element, "locationX", x) &&
           add_measurement(element, "locationY", y) &&
           add_measurement(element, "width", width) &&
           add_measurement(element, "height", 34) &&
           add_property(element, "left", ZUI_PROPERTY_CALLBACK, target, 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "right", ZUI_PROPERTY_EMPTY_ARRAY, "", 0,
                        ZUI_UNIT_NONE);
}

static int add_image(ZSharpProgram *program, const char *name,
                     const char *file, const char *target,
                     int x, int y, int width, int height) {
    ZSharpUIElement *element = add_element(program, ZUI_IMAGE, name);
    return element != NULL &&
           add_property(element, "file", ZUI_PROPERTY_TEXT, file, 0,
                        ZUI_UNIT_NONE) &&
           add_measurement(element, "locationX", x) &&
           add_measurement(element, "locationY", y) &&
           add_measurement(element, "width", width) &&
           add_measurement(element, "height", height) &&
           add_property(element, "left", ZUI_PROPERTY_CALLBACK, target, 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "right", ZUI_PROPERTY_EMPTY_ARRAY, "", 0,
                        ZUI_UNIT_NONE);
}

static int add_package_input(ZSharpProgram *program) {
    ZSharpUIElement *element = add_element(program, ZUI_TEXT_INPUT,
                                           "PackagePath");
    return element != NULL &&
           add_property(element, "display", ZUI_PROPERTY_TEXT,
                        "Paste a .zapp or .zgame path here", 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "type", ZUI_PROPERTY_IDENTIFIER, "text", 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "contents", ZUI_PROPERTY_TEXT, "", 0,
                        ZUI_UNIT_NONE) &&
           add_property(element, "textColor", ZUI_PROPERTY_COLOR, "#F4F4FA",
                        0, ZUI_UNIT_NONE) &&
           add_property(element, "backgroundColor", ZUI_PROPERTY_COLOR,
                        "#15151F", 0, ZUI_UNIT_NONE) &&
           add_property(element, "borderColor", ZUI_PROPERTY_COLOR,
                        "#45455D", 0, ZUI_UNIT_NONE) &&
           add_measurement(element, "borderWidth", 1) &&
           add_measurement(element, "padding", 8) &&
           add_measurement(element, "locationX", -65) &&
           add_measurement(element, "locationY", 174) &&
           add_measurement(element, "width", 600) &&
           add_measurement(element, "height", 34);
}

static int build_hub_program(ZSharpProgram *program,
                             const ZSharpInstalledPackageList *packages,
                             char *error, size_t error_size) {
    ZSharpUIElement *design;
    char section[128];
    size_t index;
    zsharp_program_init(program);
    program->script_type = ZSCRIPT_WINDOW;
    program->has_window = 1;
    program->source_name = zsharp_copy_text("ZSharpHub", 9);
    program->window.name = zsharp_copy_text("Hub", 3);
    if (program->source_name == NULL || program->window.name == NULL)
        return set_error(error, error_size, "out of memory");
    design = add_element(program, ZUI_DESIGN, "Design");
    if (design == NULL ||
        !add_property(design, "title", ZUI_PROPERTY_TEXT, "Z# Hub", 0,
                      ZUI_UNIT_NONE) ||
        !add_property(design, "background", ZUI_PROPERTY_COLOR, "#0B0B10", 0,
                      ZUI_UNIT_NONE) ||
        !add_property(design, "scalable", ZUI_PROPERTY_STATUS, NULL, 1,
                      ZUI_UNIT_NONE) ||
        !add_measurement(design, "width", 1000) ||
        !add_measurement(design, "height", 650) ||
        !add_text(program, "Title", "Z# Hub", "#FFFFFF", -295, 280,
                  390, 58, "title", 38, 1) ||
        !add_text(program, "Status",
                  "Launch installed projects or add another package.",
                  "#A9A9BA", -80, 235, 810, 32, "paragraph", 14, 0) ||
        !add_package_input(program) ||
        !add_button(program, "AddPackage", "Add package", "@hub-add", 410,
                    174, 120, "#6E45E2") ||
        !add_button(program, "Update", "Update Z#", "@hub-update", 410,
                    280, 120, "#D92E49") ||
        !add_text(program, "DetailsHeading", "PROJECT DETAILS", "#A9A9BA",
                  260, 118, 390, 28, "subheader", 14, 1) ||
        !add_text(program, "DetailsTitle", "Select a project", "#FFFFFF",
                  260, 78, 390, 42, "header", 24, 1) ||
        !add_text(program, "DetailsKind", "", "#C8C8D6", 260, 38,
                  390, 28, "paragraph", 14, 0) ||
        !add_text(program, "DetailsVersion", "", "#C8C8D6", 260, 4,
                  390, 28, "paragraph", 14, 0) ||
        !add_text(program, "DetailsZSharpVersion", "", "#A9A9BA", 260,
                  -28, 390, 26, "paragraph", 13, 0) ||
        !add_text(program, "DetailsPlaytime", "Playtime: --", "#E4E4EC",
                  260, -72, 390, 28, "paragraph", 15, 0) ||
        !add_text(program, "DetailsLastPlayed", "Last played: --",
                  "#E4E4EC", 260, -106, 390, 28, "paragraph", 15, 0) ||
        !add_text(program, "DetailsAchievements",
                  "Achievements: Coming soon", "#9A9AAA", 260, -154,
                  390, 30, "paragraph", 15, 0))
        return set_error(error, error_size, "out of memory");
    snprintf(section, sizeof(section), "Installed apps and games (%lu)",
             (unsigned long)packages->count);
    if (!add_text(program, "Installed", section, "#F0F0F7", -315, 125,
                  310, 32, "header", 20, 1))
        return set_error(error, error_size, "out of memory");
    if (packages->count == 0 &&
        !add_text(program, "Empty",
                  "Nothing is installed yet. Open a package or paste its path above.",
                  "#858596", -180, 72, 460, 54, "paragraph", 14, 0))
        return set_error(error, error_size, "out of memory");
    for (index = 0; index < packages->count; index++) {
        const ZSharpInstalledPackage *item = &packages->items[index];
        char name[64];
        char launch_name[64];
        char remove_name[64];
        char launch_target[64];
        char remove_target[64];
        char details_target[64];
        char icon_name[64];
        char label[512];
        int y = 78 - (int)index * 52;
        snprintf(name, sizeof(name), "Entry%lu", (unsigned long)index);
        snprintf(launch_name, sizeof(launch_name), "Launch%lu",
                 (unsigned long)index);
        snprintf(remove_name, sizeof(remove_name), "Remove%lu",
                 (unsigned long)index);
        snprintf(launch_target, sizeof(launch_target), "@hub-launch:%lu",
                 (unsigned long)index);
        snprintf(remove_target, sizeof(remove_target), "@hub-remove:%lu",
                 (unsigned long)index);
        snprintf(details_target, sizeof(details_target), "@hub-details:%lu",
                 (unsigned long)index);
        snprintf(icon_name, sizeof(icon_name), "Icon%lu",
                 (unsigned long)index);
        snprintf(label, sizeof(label), "%s  -  %s %u.%u.%u.%u",
                 item->project_name,
                 item->kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
                 item->version[0], item->version[1], item->version[2],
                 item->version[3]);
        if ((item->icon_path != NULL
                 ? !add_image(program, icon_name, item->icon_path,
                              details_target, -450, y, 38, 38)
                 : !add_button(program, icon_name, "Z#", details_target,
                               -450, y, 38, "#D92E49")) ||
            !add_button(program, name, label, details_target, -285, y, 270,
                        "#15151F") ||
            !add_button(program, launch_name, "Launch", launch_target, -92,
                        y, 92, "#236B4D") ||
            !add_button(program, remove_name, "Forget", remove_target, 10,
                        y, 92, "#542633"))
            return set_error(error, error_size, "out of memory");
    }
    return 1;
}

static char *current_executable(void) {
#ifdef _WIN32
    DWORD capacity = 32768;
    char *path = (char *)malloc(capacity);
    DWORD length;
    if (path == NULL) return NULL;
    length = GetModuleFileNameA(NULL, path, capacity);
    if (length == 0 || length >= capacity) {
        free(path);
        return NULL;
    }
    return path;
#elif defined(__linux__)
    size_t capacity = 1024;
    while (capacity <= 65536) {
        char *path = (char *)malloc(capacity);
        ssize_t length;
        if (path == NULL) return NULL;
        length = readlink("/proc/self/exe", path, capacity - 1);
        if (length >= 0 && (size_t)length < capacity - 1) {
            path[length] = '\0';
            return path;
        }
        free(path);
        capacity *= 2;
    }
    return NULL;
#else
    uint32_t capacity = 1024;
    while (capacity <= 65536u) {
        char *path = (char *)malloc(capacity);
        uint32_t needed = capacity;
        if (path == NULL) return NULL;
        if (_NSGetExecutablePath(path, &needed) == 0) {
            char *resolved = realpath(path, NULL);
            free(path);
            return resolved;
        }
        free(path);
        capacity = needed;
    }
    return NULL;
#endif
}

static int launch_package(const char *package_path, char *error,
                          size_t error_size) {
    char *executable = current_executable();
    if (executable == NULL)
        return set_error(error, error_size, "could not locate the Z# runtime");
#ifdef _WIN32
    {
        size_t needed = strlen(executable) + strlen(package_path) + 40;
        char *command = (char *)malloc(needed);
        STARTUPINFOA startup;
        PROCESS_INFORMATION process;
        int ok = 0;
        if (command == NULL) {
            free(executable);
            return set_error(error, error_size, "out of memory");
        }
        snprintf(command, needed, "\"%s\" open-desktop \"%s\"",
                 executable, package_path);
        memset(&startup, 0, sizeof(startup));
        memset(&process, 0, sizeof(process));
        startup.cb = sizeof(startup);
        if (CreateProcessA(executable, command, NULL, NULL, FALSE,
                           CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                           &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            ok = 1;
        } else {
            set_error(error, error_size, "could not launch the package");
        }
        free(command);
        free(executable);
        return ok;
    }
#else
    {
        pid_t child = fork();
        if (child < 0) {
            free(executable);
            return set_error(error, error_size, "could not launch the package");
        }
        if (child == 0) {
            int null_file;
            setsid();
            null_file = open("/dev/null", O_RDWR);
            if (null_file >= 0) {
                dup2(null_file, STDIN_FILENO);
                dup2(null_file, STDOUT_FILENO);
                dup2(null_file, STDERR_FILENO);
                if (null_file > STDERR_FILENO) close(null_file);
            }
            execl(executable, executable, "open-desktop", package_path,
                  (char *)NULL);
            _exit(127);
        }
        free(executable);
        return 1;
    }
#endif
}

static void hub_set_status(const ZSharpWindowRuntime *runtime,
                           const char *message) {
    char ignored[256] = {0};
    if (runtime == NULL || runtime->set_property == NULL) return;
    runtime->set_property(runtime->state, "ZSharpHub.Status.content",
                          ZWINDOW_VALUE_TEXT, message, ZUI_UNIT_NONE,
                          ignored, sizeof(ignored));
}

static void hub_set_text(const ZSharpWindowRuntime *runtime,
                         const char *element, const char *message) {
    char path[128];
    char ignored[256] = {0};
    if (runtime == NULL || runtime->set_property == NULL) return;
    snprintf(path, sizeof(path), "ZSharpHub.%s.content", element);
    runtime->set_property(runtime->state, path, ZWINDOW_VALUE_TEXT,
                          message, ZUI_UNIT_NONE, ignored, sizeof(ignored));
}

static void format_playtime(uint64_t seconds, char *output,
                            size_t output_size) {
    if (seconds >= 86400u) {
        snprintf(output, output_size, "Playtime: %llu day%s, %llu hour%s",
                 (unsigned long long)(seconds / 86400u),
                 seconds / 86400u == 1 ? "" : "s",
                 (unsigned long long)((seconds % 86400u) / 3600u),
                 (seconds % 86400u) / 3600u == 1 ? "" : "s");
    } else if (seconds >= 3600u) {
        snprintf(output, output_size, "Playtime: %llu hour%s, %llu minute%s",
                 (unsigned long long)(seconds / 3600u),
                 seconds / 3600u == 1 ? "" : "s",
                 (unsigned long long)((seconds % 3600u) / 60u),
                 (seconds % 3600u) / 60u == 1 ? "" : "s");
    } else if (seconds >= 60u) {
        snprintf(output, output_size, "Playtime: %llu minute%s, %llu second%s",
                 (unsigned long long)(seconds / 60u),
                 seconds / 60u == 1 ? "" : "s",
                 (unsigned long long)(seconds % 60u),
                 seconds % 60u == 1 ? "" : "s");
    } else {
        snprintf(output, output_size, "Playtime: %llu second%s",
                 (unsigned long long)seconds, seconds == 1 ? "" : "s");
    }
}

static void format_last_played(int64_t timestamp, char *output,
                               size_t output_size) {
    time_t value = (time_t)timestamp;
    struct tm local_value;
    if (timestamp <= 0) {
        snprintf(output, output_size, "Last played: Never");
        return;
    }
#ifdef _WIN32
    if (localtime_s(&local_value, &value) != 0) {
#else
    if (localtime_r(&value, &local_value) == NULL) {
#endif
        snprintf(output, output_size, "Last played: Unknown");
        return;
    }
    if (strftime(output, output_size, "Last played: %b %d, %Y at %I:%M %p",
                 &local_value) == 0)
        snprintf(output, output_size, "Last played: Unknown");
}

static void show_package_details(const ZSharpWindowRuntime *runtime,
                                 const ZSharpInstalledPackage *item) {
    char kind[128];
    char version[128];
    char zsharp_version[128];
    char playtime[160];
    char last_played[160];
    snprintf(kind, sizeof(kind), "Type: %s   PID: %s",
             item->kind == ZSHARP_PACKAGE_GAME ? "Game" : "App",
             item->project_id);
    snprintf(version, sizeof(version), "Version: %u.%u.%u.%u",
             item->version[0], item->version[1], item->version[2],
             item->version[3]);
    if (item->zsharp_version[0] == 0)
        snprintf(zsharp_version, sizeof(zsharp_version),
                 "Z# version: Unknown");
    else
        snprintf(zsharp_version, sizeof(zsharp_version),
                 "Z# version: %u.%u.%u.%u", item->zsharp_version[0],
                 item->zsharp_version[1], item->zsharp_version[2],
                 item->zsharp_version[3]);
    format_playtime(item->total_play_seconds, playtime, sizeof(playtime));
    format_last_played(item->last_played, last_played, sizeof(last_played));
    hub_set_text(runtime, "DetailsTitle", item->project_name);
    hub_set_text(runtime, "DetailsKind", kind);
    hub_set_text(runtime, "DetailsVersion", version);
    hub_set_text(runtime, "DetailsZSharpVersion", zsharp_version);
    hub_set_text(runtime, "DetailsPlaytime", playtime);
    hub_set_text(runtime, "DetailsLastPlayed", last_played);
    hub_set_text(runtime, "DetailsAchievements",
                 "Achievements: Coming soon");
}

static char *trimmed_package_path(char *value) {
    char *start = value;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    if (end - start >= 2 && start[0] == '"' && end[-1] == '"') {
        start++;
        end[-1] = '\0';
    }
    return start;
}

static int hub_callback(void *user_data, const char *target,
                        const ZSharpWindowRuntime *runtime,
                        char *error, size_t error_size) {
    HubState *state = (HubState *)user_data;
    if (strcmp(target, ZSHARP_WINDOW_PROJECT_STARTS) == 0 ||
        strcmp(target, ZSHARP_WINDOW_TASKS_STOP) == 0) return 1;
    if (strcmp(target, "@hub-update") == 0) {
        if (!zsharp_update_now(error, error_size)) return 0;
        hub_set_status(runtime,
                       "Checking for updates. You will receive a notification.");
        return 1;
    }
    if (strcmp(target, "@hub-add") == 0) {
        ZSharpWindowReadType value_type;
        char *value = NULL;
        char *path;
        ZSharpPackageInfo info;
        char message[512];
        if (runtime == NULL || runtime->get_property == NULL ||
            !runtime->get_property(runtime->state,
                                   "ZSharpHub.PackagePath.contents", &value_type,
                                   &value, error, error_size)) return 0;
        path = trimmed_package_path(value);
        if (value_type != ZWINDOW_READ_TEXT || path[0] == '\0') {
            free(value);
            hub_set_status(runtime, "Enter a .zapp or .zgame package path.");
            return 1;
        }
        if (!zsharp_package_read_info(path, &info, error, error_size)) {
            free(value);
            return 0;
        }
        if (!zsharp_registry_remember_package(path, &info, error,
                                              error_size)) {
            zsharp_package_info_free(&info);
            free(value);
            return 0;
        }
        snprintf(message, sizeof(message),
                 "%s was added. Reopen the Hub to see it in the list.",
                 info.project_name);
        zsharp_package_info_free(&info);
        free(value);
        hub_set_status(runtime, message);
        return 1;
    }
    if (strncmp(target, "@hub-details:", 13) == 0) {
        char *end = NULL;
        unsigned long index = strtoul(target + 13, &end, 10);
        if (end == target + 13 || *end != '\0' ||
            index >= state->packages.count)
            return set_error(error, error_size, "invalid Hub project details");
        show_package_details(runtime, &state->packages.items[index]);
        return 1;
    }
    if (strncmp(target, "@hub-launch:", 12) == 0 ||
        strncmp(target, "@hub-remove:", 12) == 0) {
        char *end = NULL;
        unsigned long index = strtoul(target + 12, &end, 10);
        char message[512];
        ZSharpInstalledPackage *item;
        if (end == target + 12 || *end != '\0' ||
            index >= state->packages.count)
            return set_error(error, error_size, "invalid Hub package action");
        item = &state->packages.items[index];
        if (strncmp(target, "@hub-launch:", 12) == 0) {
            if (item->package_path[0] == '\0') {
                hub_set_status(runtime, "That package was removed from the Hub.");
                return 1;
            }
            if (!launch_package(item->package_path, error, error_size)) return 0;
            snprintf(message, sizeof(message), "Launching %s...",
                     item->project_name);
            hub_set_status(runtime, message);
            return 1;
        }
        if (!zsharp_registry_forget_package(item->project_id, error,
                                            error_size)) return 0;
        snprintf(message, sizeof(message),
                 "%s was removed from the Hub. Its package was not deleted.",
                 item->project_name);
        item->package_path[0] = '\0';
        hub_set_status(runtime, message);
        return 1;
    }
    return set_error(error, error_size, "unknown Hub action");
}

int zsharp_hub_add(const char *package_path, char *error, size_t error_size) {
    ZSharpPackageInfo info;
    int ok;
    if (!zsharp_package_read_info(package_path, &info, error, error_size))
        return 0;
    ok = zsharp_registry_remember_package(package_path, &info, error,
                                          error_size);
    if (ok) printf("added %s '%s' to the Z# Hub\n",
                   info.kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
                   info.project_name);
    zsharp_package_info_free(&info);
    return ok;
}

int zsharp_hub_remove(const char *project_id, char *error,
                      size_t error_size) {
    if (!zsharp_registry_forget_package(project_id, error, error_size))
        return 0;
    printf("removed PID '%s' from the Z# Hub (package kept)\n", project_id);
    return 1;
}

int zsharp_hub_list(char *error, size_t error_size) {
    ZSharpInstalledPackageList packages;
    size_t index;
    if (!zsharp_registry_list_packages(&packages, error, error_size)) return 0;
    printf("Z# Hub: %lu installed package%s\n", (unsigned long)packages.count,
           packages.count == 1 ? "" : "s");
    for (index = 0; index < packages.count; index++) {
        ZSharpInstalledPackage *item = &packages.items[index];
        printf("%s\t%s\t%s\t%u.%u.%u.%u\t%s\n",
               item->project_id,
               item->kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
               item->project_name, item->version[0], item->version[1],
               item->version[2], item->version[3], item->package_path);
    }
    zsharp_registry_package_list_free(&packages);
    return 1;
}

int zsharp_hub_show(char *error, size_t error_size) {
    HubState state;
    ZSharpProgram program;
    int result;
    memset(&state, 0, sizeof(state));
    if (!zsharp_registry_list_packages(&state.packages, error, error_size))
        return 0;
    if (getenv("ZSHARP_HUB_CONSOLE_ONLY") != NULL) {
        zsharp_registry_package_list_free(&state.packages);
        return zsharp_hub_list(error, error_size);
    }
    prepare_package_icons(&state.packages);
    if (!build_hub_program(&program, &state.packages, error, error_size)) {
        zsharp_registry_package_list_free(&state.packages);
        zsharp_program_free(&program);
        return 0;
    }
    result = zsharp_window_run(&program, ".", hub_callback, &state,
                               error, error_size);
    zsharp_program_free(&program);
    zsharp_registry_package_list_free(&state.packages);
    return result;
}
