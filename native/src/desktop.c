#define _CRT_SECURE_NO_WARNINGS

#include "desktop.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#if defined(__APPLE__)
#include <dirent.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
extern int _NSGetExecutablePath(char *buffer, unsigned int *buffer_size);
#endif
#endif

static void desktop_error(char *error, size_t error_size,
                          const char *format, ...) {
    va_list arguments;
    if (error == NULL || error_size == 0) return;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

#ifndef _WIN32
static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}
#endif

static char *join_path(const char *left, const char *right) {
    size_t a = strlen(left), b = strlen(right);
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

static char *safe_name(const char *display_name) {
    size_t length = strlen(display_name);
    size_t capacity = length < 10 ? 10 : length;
    char *result = (char *)malloc(capacity + 1);
    size_t input;
    size_t output = 0;
    if (result == NULL) return NULL;
    for (input = 0; input < length; input++) {
        unsigned char value = (unsigned char)display_name[input];
        if (value < 32 || value == '/' || value == '\\' || value == ':' ||
            value == '*' || value == '?' || value == '\"' || value == '<' ||
            value == '>' || value == '|') continue;
        result[output++] = (char)value;
    }
    while (output > 0 && (result[output - 1] == ' ' ||
                           result[output - 1] == '.')) output--;
    if (output == 0) {
        memcpy(result, "ZSharp App", 10);
        output = 10;
    }
    result[output] = '\0';
    return result;
}

#ifdef _WIN32

static WCHAR *wide_text(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *result;
    if (count == 0) count = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (count == 0) return NULL;
    result = (WCHAR *)malloc((size_t)count * sizeof(*result));
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, result, count) == 0)
        MultiByteToWideChar(CP_ACP, 0, text, -1, result, count);
    return result;
}

static int set_registry_default(HKEY root, const WCHAR *path,
                                const WCHAR *value) {
    HKEY key;
    LONG result = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE,
                                  NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return 0;
    result = RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)value,
                            ((DWORD)wcslen(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static int windows_executable(WCHAR *path, DWORD capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    return length != 0 && length < capacity;
}

static int windows_desktop(WCHAR *path, DWORD capacity) {
    const char *override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    WCHAR *wide_override;
    size_t length;
    if (override == NULL || override[0] == '\0')
        return SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL,
                                SHGFP_TYPE_CURRENT, path) == S_OK;
    wide_override = wide_text(override);
    if (wide_override == NULL) return 0;
    length = wcslen(wide_override);
    if (length >= capacity) {
        free(wide_override);
        return 0;
    }
    memcpy(path, wide_override, (length + 1) * sizeof(*path));
    free(wide_override);
    return 1;
}

static void put_u16(unsigned char *bytes, unsigned value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, unsigned long value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

static int png_icon_file(const char *png_path, const char *ico_path) {
    unsigned char signature[24];
    unsigned char header[22] = {0};
    unsigned long width;
    unsigned long height;
    FILE *input = fopen(png_path, "rb");
    FILE *output;
    long size;
    unsigned char buffer[64 * 1024];
    size_t count;
    if (input == NULL || fread(signature, 1, sizeof(signature), input) !=
                             sizeof(signature) ||
        memcmp(signature, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        fseek(input, 0, SEEK_END) != 0 || (size = ftell(input)) <= 0 ||
        (unsigned long)size > 0xfffffffful || fseek(input, 0, SEEK_SET) != 0) {
        if (input != NULL) fclose(input);
        return 0;
    }
    width = ((unsigned long)signature[16] << 24) |
            ((unsigned long)signature[17] << 16) |
            ((unsigned long)signature[18] << 8) | signature[19];
    height = ((unsigned long)signature[20] << 24) |
             ((unsigned long)signature[21] << 16) |
             ((unsigned long)signature[22] << 8) | signature[23];
    if (width == 0 || height == 0) { fclose(input); return 0; }
    put_u16(header + 2, 1);
    put_u16(header + 4, 1);
    header[6] = width >= 256 ? 0 : (unsigned char)width;
    header[7] = height >= 256 ? 0 : (unsigned char)height;
    put_u16(header + 10, 1);
    put_u16(header + 12, 32);
    put_u32(header + 14, (unsigned long)size);
    put_u32(header + 18, 22);
    output = fopen(ico_path, "wb");
    if (output == NULL || fwrite(header, 1, sizeof(header), output) !=
                              sizeof(header)) {
        fclose(input);
        if (output != NULL) fclose(output);
        return 0;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0)
        if (fwrite(buffer, 1, count, output) != count) {
            fclose(input); fclose(output); return 0;
        }
    if (ferror(input) || fclose(input) != 0 || fclose(output) != 0) return 0;
    return 1;
}

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    WCHAR executable[32768];
    WCHAR command[65536];
    if (!windows_executable(executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0])))) {
        desktop_error(error, error_size, "could not locate zsharp.exe");
        return 0;
    }
    swprintf(command, sizeof(command) / sizeof(command[0]),
             L"\"%ls\" open-desktop \"%%1\"", executable);
    if (!set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\.zapp", L"ZSharp.Application") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\.zgame", L"ZSharp.Game") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application", L"Z# Application") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application\\DefaultIcon",
            executable) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application\\shell\\open\\command",
            command) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game", L"Z# Game") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game\\DefaultIcon", executable) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game\\shell\\open\\command",
            command)) {
        desktop_error(error, error_size,
                      "could not register Z# package associations for this user");
        return 0;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return 1;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
                                   char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR executable[32768];
    WCHAR package_absolute[32768];
    WCHAR arguments[65536];
    WCHAR shortcut_path[32768];
    WCHAR *wide_package = NULL;
    WCHAR *wide_name = NULL;
    WCHAR *wide_icon = NULL;
    char *name = NULL;
    char *icon_path = NULL;
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT initialized;
    HRESULT result;
    int ok = 0;
    if (!windows_desktop(desktop,
                         (DWORD)(sizeof(desktop) / sizeof(desktop[0]))) ||
        !windows_executable(executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0])))) {
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_package = wide_text(package_path);
    name = safe_name(display_name);
    wide_name = name == NULL ? NULL : wide_text(name);
    if (wide_package == NULL || wide_name == NULL ||
        GetFullPathNameW(wide_package,
                         (DWORD)(sizeof(package_absolute) /
                                 sizeof(package_absolute[0])),
                         package_absolute, NULL) == 0) {
        desktop_error(error, error_size, "could not resolve the shortcut paths");
        goto done;
    }
    swprintf(shortcut_path, sizeof(shortcut_path) / sizeof(shortcut_path[0]),
             L"%ls\\%ls.lnk", desktop, wide_name);
    swprintf(arguments, sizeof(arguments) / sizeof(arguments[0]),
             L"open-desktop \"%ls\"", package_absolute);
    initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        desktop_error(error, error_size, "could not initialize shortcut support");
        goto done;
    }
    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(result)) result = IShellLinkW_SetPath(link, executable);
    if (SUCCEEDED(result)) result = IShellLinkW_SetArguments(link, arguments);
    if (SUCCEEDED(result)) result = IShellLinkW_SetDescription(link, wide_name);
    if (icon_relative != NULL && strlen(icon_relative) > 4 &&
        _stricmp(icon_relative + strlen(icon_relative) - 4, ".ico") == 0) {
        icon_path = join_path(project_root, icon_relative);
        wide_icon = icon_path == NULL ? NULL : wide_text(icon_path);
    } else if (icon_relative != NULL && strlen(icon_relative) > 4 &&
               _stricmp(icon_relative + strlen(icon_relative) - 4,
                        ".png") == 0) {
        char *png_path = join_path(project_root, icon_relative);
        icon_path = join_path(project_root, ".zsharp-shortcut.ico");
        if (png_path != NULL && icon_path != NULL &&
            png_icon_file(png_path, icon_path))
            wide_icon = wide_text(icon_path);
        free(png_path);
    }
    if (SUCCEEDED(result))
        result = IShellLinkW_SetIconLocation(
            link, wide_icon == NULL ? executable : wide_icon, 0);
    if (SUCCEEDED(result))
        result = IShellLinkW_QueryInterface(link, &IID_IPersistFile,
                                            (void **)&persist);
    if (SUCCEEDED(result)) result = IPersistFile_Save(persist, shortcut_path, TRUE);
    if (persist != NULL) IPersistFile_Release(persist);
    if (link != NULL) IShellLinkW_Release(link);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(result)) {
        desktop_error(error, error_size, "could not create the desktop shortcut");
        goto done;
    }
    ok = 1;
done:
    free(icon_path);
    free(wide_icon);
    free(wide_name);
    free(name);
    free(wide_package);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR shortcut_path[32768];
    WCHAR *wide_name;
    char *name = safe_name(display_name);
    DWORD attributes;
    if (name == NULL || !windows_desktop(
            desktop, (DWORD)(sizeof(desktop) / sizeof(desktop[0])))) {
        free(name);
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_name = wide_text(name);
    free(name);
    if (wide_name == NULL) return 0;
    swprintf(shortcut_path,
             sizeof(shortcut_path) / sizeof(shortcut_path[0]),
             L"%ls\\%ls.lnk", desktop, wide_name);
    free(wide_name);
    attributes = GetFileAttributesW(shortcut_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR path[32768];
    WCHAR *wide_name;
    char *name = safe_name(display_name);
    DWORD failure;
    if (name == NULL ||
        !windows_desktop(desktop,
                         (DWORD)(sizeof(desktop) / sizeof(desktop[0])))) {
        free(name);
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_name = wide_text(name);
    free(name);
    if (wide_name == NULL) {
        desktop_error(error, error_size, "could not resolve the shortcut name");
        return 0;
    }
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\%ls.lnk",
             desktop, wide_name);
    free(wide_name);
    if (DeleteFileW(path)) return 1;
    failure = GetLastError();
    if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND)
        return 1;
    desktop_error(error, error_size, "could not remove the desktop shortcut");
    return 0;
}

#else

static int make_directory(const char *path) {
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST;
}

static int make_directories(const char *path) {
    char *copy = copy_text(path);
    char *cursor;
    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (!make_directory(copy)) { free(copy); return 0; }
            *cursor = '/';
        }
    }
    if (!make_directory(copy)) { free(copy); return 0; }
    free(copy);
    return 1;
}

static char *executable_path(char *error, size_t error_size) {
#if defined(__linux__)
    size_t capacity = 1024;
    for (;;) {
        char *path = (char *)malloc(capacity);
        ssize_t length;
        if (path == NULL) return NULL;
        length = readlink("/proc/self/exe", path, capacity - 1);
        if (length >= 0 && (size_t)length < capacity - 1) {
            path[length] = '\0';
            return path;
        }
        free(path);
        if (capacity >= 65536) break;
        capacity *= 2;
    }
#else
    unsigned int capacity = 1024;
    for (;;) {
        char *path = (char *)malloc(capacity);
        unsigned int needed = capacity;
        if (path == NULL) return NULL;
        if (_NSGetExecutablePath(path, &needed) == 0) {
            char *resolved = realpath(path, NULL);
            free(path);
            return resolved;
        }
        free(path);
        capacity = needed;
        if (capacity > 65536u) break;
    }
#endif
    desktop_error(error, error_size, "could not locate the Z# executable");
    return NULL;
}

#if defined(__linux__)
static char *desktop_exec_quote(const char *text) {
    size_t length = 3;
    const char *cursor;
    char *result;
    char *output;
    for (cursor = text; *cursor != '\0'; cursor++)
        length += (*cursor == '\\' || *cursor == '\"' ||
                   *cursor == '`' || *cursor == '$') ? 2 : 1;
    result = (char *)malloc(length);
    if (result == NULL) return NULL;
    output = result;
    *output++ = '\"';
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\\' || *cursor == '\"' ||
            *cursor == '`' || *cursor == '$') *output++ = '\\';
        *output++ = *cursor;
    }
    *output++ = '\"';
    *output = '\0';
    return result;
}
#endif

static int write_file(const char *path, const char *contents, int executable,
                      char *error, size_t error_size) {
    FILE *file;
    file = fopen(path, "wb");
    if (file == NULL) {
        desktop_error(error, error_size, "could not write '%s'", path);
        return 0;
    }
    if (fwrite(contents, 1, strlen(contents), file) != strlen(contents) ||
        fclose(file) != 0) {
        desktop_error(error, error_size, "could not write '%s'", path);
        return 0;
    }
    if (executable && chmod(path, 0755) != 0) {
        desktop_error(error, error_size, "could not make '%s' executable", path);
        return 0;
    }
    return 1;
}

static int run_process(const char *executable, char *const arguments[]) {
    pid_t child = fork();
    int status;
    if (child < 0) return 0;
    if (child == 0) {
        execv(executable, arguments);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#if defined(__linux__)

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    char *executable = NULL;
    char *applications = NULL;
    char *mime_packages = NULL;
    char *desktop_file = NULL;
    char *mime_file = NULL;
    char *quoted_executable = NULL;
    char *launcher = NULL;
    const char *mime =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
        " <mime-type type=\"application/x-zsharp-app\"><comment>Z# Application</comment><glob pattern=\"*.zapp\"/></mime-type>\n"
        " <mime-type type=\"application/x-zsharp-game\"><comment>Z# Game</comment><glob pattern=\"*.zgame\"/></mime-type>\n"
        "</mime-info>\n";
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    executable = executable_path(error, error_size);
    applications = join_path(home, ".local/share/applications");
    mime_packages = join_path(home, ".local/share/mime/packages");
    desktop_file = applications == NULL ? NULL
        : join_path(applications, "zsharp-package.desktop");
    mime_file = mime_packages == NULL ? NULL
        : join_path(mime_packages, "zsharp-package.xml");
    quoted_executable = executable == NULL ? NULL : desktop_exec_quote(executable);
    if (executable == NULL || applications == NULL || mime_packages == NULL ||
        desktop_file == NULL || mime_file == NULL || quoted_executable == NULL ||
        !make_directories(applications) || !make_directories(mime_packages)) {
        desktop_error(error, error_size, "could not prepare the user application directories");
        goto done;
    }
    launcher = (char *)malloc(strlen(quoted_executable) + 256);
    if (launcher == NULL) goto done;
    sprintf(launcher,
        "[Desktop Entry]\nType=Application\nName=Z# Package Launcher\n"
        "Exec=%s open %%f\nTerminal=false\nNoDisplay=true\n"
        "MimeType=application/x-zsharp-app;application/x-zsharp-game;\n",
        quoted_executable);
    if (!write_file(desktop_file, launcher, 1, error, error_size) ||
        !write_file(mime_file, mime, 0, error, error_size)) goto done;
    {
        char *mime_args[] = {"xdg-mime", "install", "--mode", "user",
                             mime_file, NULL};
        char *app_args[] = {"xdg-mime", "default", "zsharp-package.desktop",
                            "application/x-zsharp-app", NULL};
        char *game_args[] = {"xdg-mime", "default", "zsharp-package.desktop",
                             "application/x-zsharp-game", NULL};
        /* These helpers are optional; desktop databases also refresh on login. */
        run_process("/usr/bin/xdg-mime", mime_args);
        run_process("/usr/bin/xdg-mime", app_args);
        run_process("/usr/bin/xdg-mime", game_args);
    }
    ok = 1;
done:
    free(launcher);
    free(quoted_executable);
    free(mime_file);
    free(desktop_file);
    free(mime_packages);
    free(applications);
    free(executable);
    return ok;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *name = NULL;
    char *filename = NULL;
    char *path = NULL;
    char *executable = NULL;
    char *package_absolute = NULL;
    char *quoted_executable = NULL;
    char *quoted_package = NULL;
    char *icon = NULL;
    char *contents = NULL;
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL : join_path(desktop, filename);
    executable = executable_path(error, error_size);
    package_absolute = realpath(package_path, NULL);
    quoted_executable = executable == NULL ? NULL : desktop_exec_quote(executable);
    quoted_package = package_absolute == NULL ? NULL : desktop_exec_quote(package_absolute);
    icon = icon_relative == NULL ? NULL : join_path(project_root, icon_relative);
    if (desktop == NULL || name == NULL || path == NULL || executable == NULL ||
        package_absolute == NULL || quoted_executable == NULL ||
        quoted_package == NULL || !make_directories(desktop)) {
        desktop_error(error, error_size, "could not prepare the desktop shortcut");
        goto done;
    }
    contents = (char *)malloc(strlen(name) + strlen(quoted_executable) +
                              strlen(quoted_package) +
                              (icon == NULL ? 0 : strlen(icon)) + 256);
    if (contents == NULL) goto done;
    sprintf(contents,
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s open %s\n"
        "Terminal=false\nIcon=%s\nCategories=Utility;\n",
        name, quoted_executable, quoted_package, icon == NULL ? "zsharp" : icon);
    ok = write_file(path, contents, 1, error, error_size);
done:
    free(contents);
    free(icon);
    free(quoted_package);
    free(quoted_executable);
    free(package_absolute);
    free(executable);
    free(path);
    free(filename);
    free(name);
    free(desktop);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *filename;
    char *path;
    int exists;
    if (home == NULL || home[0] == '\0') return 1;
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL
        : join_path(desktop, filename);
    exists = path != NULL && access(path, F_OK) == 0;
    free(path);
    free(filename);
    free(name);
    free(desktop);
    if (!exists) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *filename;
    char *path;
    int ok;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL
        : join_path(desktop, filename);
    if (path == NULL) {
        desktop_error(error, error_size, "could not resolve the desktop shortcut");
        free(filename);
        free(name);
        free(desktop);
        return 0;
    }
    ok = unlink(path) == 0 || errno == ENOENT;
    if (!ok)
        desktop_error(error, error_size,
                      "could not remove the desktop shortcut");
    free(path);
    free(filename);
    free(name);
    free(desktop);
    return ok;
}

#else

static char *apple_quote(const char *text) {
    size_t length = 3;
    const char *cursor;
    char *result;
    char *output;
    for (cursor = text; *cursor != '\0'; cursor++)
        length += (*cursor == '\\' || *cursor == '\"') ? 2 : 1;
    result = (char *)malloc(length);
    if (result == NULL) return NULL;
    output = result;
    *output++ = '\"';
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\\' || *cursor == '\"') *output++ = '\\';
        *output++ = *cursor;
    }
    *output++ = '\"';
    *output = '\0';
    return result;
}

static char *apple_script_text(const char *executable, const char *package) {
    char *quoted_executable = apple_quote(executable);
    char *quoted_package = package == NULL ? NULL : apple_quote(package);
    char *script;
    size_t size;
    if (quoted_executable == NULL || (package != NULL && quoted_package == NULL)) {
        free(quoted_executable); free(quoted_package); return NULL;
    }
    if (package == NULL) {
        size = strlen(quoted_executable) + 512;
        script = (char *)malloc(size);
        if (script != NULL) snprintf(script, size,
            "on open droppedItems\n repeat with itemRef in droppedItems\n"
            "  set commandText to quoted form of %s & \" open \" & quoted form of POSIX path of itemRef\n"
            "  do shell script commandText & \" >/dev/null 2>&1 &\"\n"
            " end repeat\nend open\n", quoted_executable);
    } else {
        size = strlen(quoted_executable) + strlen(quoted_package) + 384;
        script = (char *)malloc(size);
        if (script != NULL) snprintf(script, size,
            "on run\n set commandText to quoted form of %s & \" open \" & quoted form of %s\n"
            " do shell script commandText & \" >/dev/null 2>&1 &\"\n"
            "end run\n", quoted_executable, quoted_package);
    }
    free(quoted_executable);
    free(quoted_package);
    return script;
}

static int compile_applet(const char *app_path, const char *script,
                          char *error, size_t error_size) {
    const char *home = getenv("HOME");
    char *cache = home == NULL ? NULL : join_path(home, ".cache/zsharp");
    char *source = cache == NULL ? NULL : join_path(cache, "launcher.applescript");
    int ok = 0;
    if (cache == NULL || source == NULL || !make_directories(cache) ||
        !write_file(source, script, 0, error, error_size)) goto done;
    {
        char *arguments[] = {"osacompile", "-o", (char *)app_path, source, NULL};
        if (!run_process("/usr/bin/osacompile", arguments)) {
            desktop_error(error, error_size, "could not create the macOS app shortcut");
            goto done;
        }
    }
    ok = 1;
done:
    free(source);
    free(cache);
    return ok;
}

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    char *applications = NULL;
    char *app = NULL;
    char *info = NULL;
    char *executable = NULL;
    char *script = NULL;
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    applications = join_path(home, "Applications");
    app = applications == NULL ? NULL
        : join_path(applications, "ZSharp Package Launcher.app");
    executable = executable_path(error, error_size);
    script = executable == NULL ? NULL : apple_script_text(executable, NULL);
    if (applications == NULL || app == NULL || executable == NULL ||
        script == NULL || !make_directories(applications) ||
        !compile_applet(app, script, error, error_size)) goto done;
    info = join_path(app, "Contents/Info.plist");
    if (info == NULL) goto done;
    {
        const char *buddy = "/usr/libexec/PlistBuddy";
        char *commands[] = {
            "Add :CFBundleDocumentTypes array",
            "Add :CFBundleDocumentTypes:0 dict",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeName string ZSharp Package",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Viewer",
            "Add :CFBundleDocumentTypes:0:LSHandlerRank string Owner",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions array",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:0 string zapp",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:1 string zgame"
        };
        size_t index;
        for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
            char *arguments[] = {"PlistBuddy", "-c", commands[index], info, NULL};
            run_process(buddy, arguments);
        }
    }
    {
        const char *lsregister =
            "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
        char *arguments[] = {"lsregister", "-f", app, NULL};
        run_process(lsregister, arguments);
    }
    ok = 1;
done:
    free(script);
    free(executable);
    free(info);
    free(app);
    free(applications);
    return ok;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *name = NULL;
    char *bundle_name = NULL;
    char *app = NULL;
    char *executable = NULL;
    char *package_absolute = NULL;
    char *script = NULL;
    char *icon_source = NULL;
    char *icon_target = NULL;
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle_name = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle_name != NULL) sprintf(bundle_name, "%s.app", name);
    app = desktop == NULL || bundle_name == NULL ? NULL
        : join_path(desktop, bundle_name);
    executable = executable_path(error, error_size);
    package_absolute = realpath(package_path, NULL);
    script = executable == NULL || package_absolute == NULL ? NULL
        : apple_script_text(executable, package_absolute);
    if (desktop == NULL || app == NULL || executable == NULL ||
        package_absolute == NULL || script == NULL || !make_directories(desktop)) {
        desktop_error(error, error_size, "could not prepare the macOS desktop shortcut");
        goto done;
    }
    ok = compile_applet(app, script, error, error_size);
    if (ok && icon_relative != NULL) {
        icon_source = join_path(project_root, icon_relative);
        icon_target = join_path(app, "Contents/Resources/applet.icns");
        if (icon_source != NULL && icon_target != NULL) {
            char *arguments[] = {"sips", "-s", "format", "icns",
                                 icon_source, "--out", icon_target, NULL};
            run_process("/usr/bin/sips", arguments);
        }
    }
done:
    free(icon_target);
    free(icon_source);
    free(script);
    free(package_absolute);
    free(executable);
    free(app);
    free(bundle_name);
    free(name);
    free(desktop);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *bundle;
    char *path;
    int exists;
    if (home == NULL || home[0] == '\0') return 1;
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle != NULL) sprintf(bundle, "%s.app", name);
    path = desktop == NULL || bundle == NULL ? NULL : join_path(desktop, bundle);
    exists = path != NULL && access(path, F_OK) == 0;
    free(path);
    free(bundle);
    free(name);
    free(desktop);
    if (!exists) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

static int remove_applet_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode)) return unlink(path) == 0;
    {
        DIR *directory = opendir(path);
        struct dirent *entry;
        int ok = directory != NULL;
        if (directory == NULL) return 0;
        while (ok && (entry = readdir(directory)) != NULL) {
            char *child;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) continue;
            child = join_path(path, entry->d_name);
            if (child == NULL || !remove_applet_tree(child)) ok = 0;
            free(child);
        }
        if (closedir(directory) != 0) ok = 0;
        if (ok && rmdir(path) != 0) ok = 0;
        return ok;
    }
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *bundle;
    char *path;
    int ok;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle != NULL) sprintf(bundle, "%s.app", name);
    path = desktop == NULL || bundle == NULL ? NULL : join_path(desktop, bundle);
    if (path == NULL) {
        desktop_error(error, error_size, "could not resolve the desktop shortcut");
        free(bundle);
        free(name);
        free(desktop);
        return 0;
    }
    ok = remove_applet_tree(path);
    if (!ok)
        desktop_error(error, error_size,
                      "could not remove the desktop shortcut");
    free(path);
    free(bundle);
    free(name);
    free(desktop);
    return ok;
}

#endif
#endif
