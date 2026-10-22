#define _CRT_SECURE_NO_WARNINGS
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "desktop_api.h"
#include "desktop_input.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

static int desktop_fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    if (error != NULL && size != 0) {
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return 0;
}

#if defined(__linux__) && !defined(__ANDROID__)
static char *desktop_copy(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}
#endif

static int valid_project_id(const char *id) {
    size_t i;
    if (id == NULL || !id[0] || strlen(id) > 128) return 0;
    for (i = 0; id[i]; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '_')) return 0;
    return 1;
}

/* This deliberately rejects newlines/control characters in launch paths,
 * rather than allowing them to become another desktop-file/plist field. */
static int valid_argument(const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    if (value == NULL || !value[0] || strlen(value) > 32767) return 0;
    for (; *p; p++) if (*p < 32 || *p == 127) return 0;
    return 1;
}

static char *escape_xml(const char *value) {
    size_t size = strlen(value), i, at = 0;
    char *out;
    if (size > (SIZE_MAX - 1) / 6) return NULL;
    out = (char *)malloc(size * 6 + 1);
    if (out == NULL) return NULL;
    for (i = 0; i < size; i++) {
        const char *replacement = value[i] == '&' ? "&amp;" :
            value[i] == '<' ? "&lt;" : value[i] == '>' ? "&gt;" :
            value[i] == '"' ? "&quot;" : value[i] == '\'' ? "&apos;" : NULL;
        if (replacement) { size_t n = strlen(replacement); memcpy(out + at, replacement, n); at += n; }
        else out[at++] = value[i];
    }
    out[at] = '\0';
    return out;
}

static char *quote_windows(const char *value) {
    size_t i, at = 0, slashes = 0, n = strlen(value);
    char *out = n > (SIZE_MAX - 3) / 2 ? NULL : (char *)malloc(n * 2 + 3);
    if (!out) return NULL;
    out[at++] = '"';
    for (i = 0; i <= n; i++) {
        if (value[i] == '\\') { slashes++; continue; }
        if (value[i] == '"' || value[i] == '\0') {
            while (slashes) { out[at++] = '\\'; out[at++] = '\\'; slashes--; }
            if (value[i] == '"') out[at++] = '\\';
        } else while (slashes) { out[at++] = '\\'; slashes--; }
        if (value[i]) out[at++] = value[i];
    }
    out[at++] = '"'; out[at] = '\0'; return out;
}

/* Desktop Entry Exec quoting has two levels: the string-value parser first
 * unescapes backslashes, then the Exec parser unquotes arguments. Percent is
 * doubled to prevent desktop field-code expansion. No shell is involved. */
static char *quote_desktop_exec(const char *value) {
    size_t i, at = 0, n = strlen(value);
    char *out = n > (SIZE_MAX - 3) / 4 ? NULL : (char *)malloc(n * 4 + 3);
    if (!out) return NULL;
    out[at++] = '"';
    for (i = 0; i < n; i++) {
        if (value[i] == '\\') {
            out[at++] = '\\'; out[at++] = '\\'; out[at++] = '\\'; out[at++] = '\\';
        } else if (value[i] == '"' || value[i] == '`' || value[i] == '$') {
            out[at++] = '\\'; out[at++] = '\\'; out[at++] = value[i];
        } else if (value[i] == '%') { out[at++] = '%'; out[at++] = '%'; }
        else out[at++] = value[i];
    }
    out[at++] = '"'; out[at] = '\0'; return out;
}

static char *escape_desktop_value(const char *value) {
    size_t i, at = 0, n = strlen(value);
    char *out = n > (SIZE_MAX - 1) / 2 ? NULL : (char *)malloc(n * 2 + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (value[i] == '\\') out[at++] = '\\';
        out[at++] = value[i];
    }
    out[at] = '\0'; return out;
}

char *zsharp_desktop_startup_document(const char *platform, const char *project_id,
                                      const char *display_name,
                                      const char *executable, const char *entry,
                                      int entry_kind, char *error, size_t size) {
    char *exe = NULL, *file = NULL, *name = NULL, *out = NULL;
    const char *command = entry_kind == ZDESKTOP_ENTRY_BYTECODE ? "run-bytecode" :
        entry_kind == ZDESKTOP_ENTRY_PACKAGE ? "open" : "run";
    size_t capacity;
    if (!valid_project_id(project_id) || !valid_argument(display_name) ||
        !valid_argument(executable) || !valid_argument(entry) || !platform ||
        entry_kind < ZDESKTOP_ENTRY_SOURCE || entry_kind > ZDESKTOP_ENTRY_BYTECODE) {
        desktop_fail(error, size, "invalid startup project identity, name or path"); return NULL;
    }
    if (!strcmp(platform, "windows")) { exe = quote_windows(executable); file = quote_windows(entry); }
    else if (!strcmp(platform, "linux")) {
        exe = quote_desktop_exec(executable); file = quote_desktop_exec(entry);
        name = escape_desktop_value(display_name);
    } else if (!strcmp(platform, "macos")) { exe = escape_xml(executable); file = escape_xml(entry); }
    else { desktop_fail(error, size, "startup is unsupported on this platform"); return NULL; }
    if (!exe || !file || (!strcmp(platform, "linux") && !name)) goto memory;
    capacity = strlen(exe) + strlen(file) + strlen(project_id) +
        (name ? strlen(name) : 0) + 768;
    out = (char *)malloc(capacity);
    if (!out) goto memory;
    if (!strcmp(platform, "windows")) snprintf(out, capacity, "%s %s %s", exe, command, file);
    else if (!strcmp(platform, "linux"))
        snprintf(out, capacity,
                 "[Desktop Entry]\nType=Application\nName=%s\nExec=%s %s %s\n"
                 "Terminal=false\nX-GNOME-Autostart-enabled=true\n", name, exe, command, file);
    else snprintf(out, capacity,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n<key>Label</key><string>com.zombieos.zsharp.%s</string>\n"
        "<key>ProgramArguments</key><array><string>%s</string><string>%s</string><string>%s</string></array>\n"
        "<key>RunAtLoad</key><true/>\n</dict></plist>\n", project_id, exe, command, file);
    free(exe); free(file); free(name); return out;
memory:
    free(exe); free(file); free(name); free(out);
    desktop_fail(error, size, "out of memory preparing startup registration"); return NULL;
}

#ifdef _WIN32
static WCHAR *desktop_wide(const char *value) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    WCHAR *out = count ? (WCHAR *)malloc((size_t)count * sizeof(WCHAR)) : NULL;
    if (out) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, out, count);
    return out;
}
static char *desktop_utf8(const WCHAR *value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    char *out = count ? (char *)malloc((size_t)count) : NULL;
    if (out) WideCharToMultiByte(CP_UTF8, 0, value, -1, out, count, NULL, NULL);
    return out;
}
static char *desktop_absolute(const char *path) {
    WCHAR *wide = desktop_wide(path), resolved[32768];
    DWORD n = wide ? GetFullPathNameW(wide, 32768, resolved, NULL) : 0;
    DWORD attributes = n && n < 32768 ? GetFileAttributesW(resolved) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? NULL : desktop_utf8(resolved);
}
static char *desktop_executable(void) {
    WCHAR path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    return n && n < 32768 ? desktop_utf8(path) : NULL;
}
#else
static char *desktop_absolute(const char *path) { return realpath(path, NULL); }
static char *desktop_executable(void) {
    char path[PATH_MAX + 1];
#ifdef __APPLE__
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return NULL;
    return realpath(path, NULL);
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n < 0 || n >= (ssize_t)sizeof(path) - 1) return NULL;
    path[n] = '\0'; return desktop_copy(path);
#else
    return NULL;
#endif
}
#endif

static const char *desktop_platform(void) {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__) && !defined(__ANDROID__)
    return "linux";
#else
    return "unsupported";
#endif
}

static int startup_file(const char *directory, const char *id, const char *extension,
                          const char *document, char *error, size_t size) {
    size_t capacity = strlen(directory) + strlen(id) + strlen(extension) + 72;
    char *path = (char *)malloc(capacity), *temporary = (char *)malloc(capacity + 32);
    FILE *file = NULL;
    int ok = 0;
    if (!path || !temporary) { desktop_fail(error, size, "out of memory"); goto done; }
    snprintf(path, capacity, "%s/com.zombieos.zsharp.%s.%s", directory, id, extension);
#ifdef _WIN32
    {
        WCHAR *wide = desktop_wide(path);
        if (!wide) { desktop_fail(error, size, "invalid startup path"); goto done; }
        if (!document) {
            ok = DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
            free(wide); goto done;
        }
        free(wide);
    }
    snprintf(temporary, capacity + 32, "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId());
    {
        WCHAR *wide = desktop_wide(temporary);
        file = wide ? _wfopen(wide, L"wbx") : NULL;
        free(wide);
    }
#else
    {
        struct stat stat_value;
        if (lstat(path, &stat_value) == 0 && !S_ISREG(stat_value.st_mode)) {
            desktop_fail(error, size, "startup target is not a regular file"); goto done;
        }
        if (!document) { ok = unlink(path) == 0 || errno == ENOENT; goto done; }
    }
    snprintf(temporary, capacity + 32, "%s.tmp.%ld", path, (long)getpid());
    {
        int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0) { file = fdopen(descriptor, "wb"); if (!file) close(descriptor); }
    }
#endif
    if (!file) { desktop_fail(error, size, "could not create private startup registration: %s", strerror(errno)); goto done; }
    ok = fwrite(document, 1, strlen(document), file) == strlen(document);
    if (fclose(file) != 0) ok = 0;
    file = NULL;
    if (ok) {
#ifdef _WIN32
        WCHAR *from = desktop_wide(temporary), *to = desktop_wide(path);
        ok = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        free(from); free(to);
#else
        ok = rename(temporary, path) == 0;
#endif
    }
    if (!ok) desktop_fail(error, size, "could not save startup registration");
    /* The temporary filename belongs only to this exact registration attempt. */
#ifdef _WIN32
    { WCHAR *wide = desktop_wide(temporary); if (wide) DeleteFileW(wide); free(wide); }
#else
    unlink(temporary);
#endif
done:
    if (file) fclose(file);
    if (!ok && error && size && !error[0]) desktop_fail(error, size, "could not remove startup registration");
    free(path); free(temporary); return ok;
}

int zsharp_desktop_apply_startup(const ZSharpSettings *settings,
                                 const char *entry_path, int entry_kind,
                                 char *error, size_t size) {
    const char *override = getenv("ZSHARP_STARTUP_DIRECTORY");
    const char *platform = desktop_platform();
    char *exe = NULL, *entry = NULL, *document = NULL;
    int ok = 0;
    if (error != NULL && size != 0) error[0] = '\0';
    if (!settings || !settings->has_launch_at_start || getenv("ZSHARP_SKIP_DESKTOP_INTEGRATION")) return 1;
    if (!strcmp(platform, "unsupported")) return desktop_fail(error, size, "login startup is unsupported on this platform");
    if (!valid_project_id(settings->project_id)) return desktop_fail(error, size, "invalid startup project ID");
    if (settings->launch_at_start) {
        exe = desktop_executable(); entry = entry_path ? desktop_absolute(entry_path) : NULL;
        if (!exe || !entry) { desktop_fail(error, size, "could not resolve the startup executable or entry file"); goto done; }
        document = zsharp_desktop_startup_document(platform, settings->project_id,
            settings->project_name ? settings->project_name : settings->project_id,
            exe, entry, entry_kind, error, size);
        if (!document) goto done;
    }
    if (override && override[0]) {
#ifdef _WIN32
        WCHAR *wide = desktop_wide(override);
        DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        int absolute = (isalpha((unsigned char)override[0]) && override[1] == ':' &&
                         (override[2] == '\\' || override[2] == '/')) ||
                        (override[0] == '\\' && override[1] == '\\');
        free(wide);
        if (!absolute || attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            desktop_fail(error, size, "ZSHARP_STARTUP_DIRECTORY must be an existing absolute directory"); goto done;
        }
#else
        struct stat st;
        if (override[0] != '/' || stat(override, &st) != 0 || !S_ISDIR(st.st_mode)) {
            desktop_fail(error, size, "ZSHARP_STARTUP_DIRECTORY must be an existing absolute directory"); goto done;
        }
#endif
        ok = startup_file(override, settings->project_id,
                          !strcmp(platform, "windows") ? "windows-command.txt" :
                          !strcmp(platform, "macos") ? "plist" : "desktop",
                          document, error, size);
        goto done;
    }
#ifdef _WIN32
    {
        WCHAR name[192], *command = document ? desktop_wide(document) : NULL;
        WCHAR *id = desktop_wide(settings->project_id);
        HKEY key = NULL;
        LONG result;
        if (!id || (document && !command)) { free(id); free(command); desktop_fail(error, size, "invalid startup text"); goto done; }
        swprintf(name, 192, L"ZombieOS.ZSharp.Project.%ls", id);
        free(id);
        if (command && wcslen(command) > 259) {
            free(command); desktop_fail(error, size, "Windows login command exceeds 259 characters; use shorter installation/project paths"); goto done;
        }
        if (document) result = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
        else result = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key);
        if (!document && result == ERROR_FILE_NOT_FOUND) ok = 1;
        else if (result == ERROR_SUCCESS) {
            result = document ? RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)command,
                (DWORD)((wcslen(command) + 1) * sizeof(WCHAR))) : RegDeleteValueW(key, name);
            ok = result == ERROR_SUCCESS || (!document && result == ERROR_FILE_NOT_FOUND);
        }
        if (key) RegCloseKey(key);
        free(command);
        if (!ok) desktop_fail(error, size, "could not update this project's login registration (Windows error %ld)", result);
    }
#elif defined(__linux__) || defined(__APPLE__)
    {
        const char *home = getenv("HOME");
#ifndef __APPLE__
        const char *config = getenv("XDG_CONFIG_HOME");
#endif
        char directory[PATH_MAX], parent[PATH_MAX];
        int n;
        if (!home || home[0] != '/') { desktop_fail(error, size, "an absolute HOME is required for login registration"); goto done; }
#ifdef __APPLE__
        n = snprintf(parent, sizeof(parent), "%s/Library", home);
        if (n < 0 || n >= (int)sizeof(parent)) goto path_error;
        n = snprintf(directory, sizeof(directory), "%s/LaunchAgents", parent);
#else
        if (config && config[0] == '/') n = snprintf(parent, sizeof(parent), "%s", config);
        else n = snprintf(parent, sizeof(parent), "%s/.config", home);
        if (n < 0 || n >= (int)sizeof(parent)) goto path_error;
        n = snprintf(directory, sizeof(directory), "%s/autostart", parent);
#endif
        if (n < 0 || n >= (int)sizeof(directory)) goto path_error;
        if (document && ((mkdir(parent, 0700) != 0 && errno != EEXIST) ||
                         (mkdir(directory, 0700) != 0 && errno != EEXIST))) {
            desktop_fail(error, size, "could not create the current user's startup directory: %s", strerror(errno)); goto done;
        }
        ok = startup_file(directory, settings->project_id,
#ifdef __APPLE__
                          "plist",
#else
                          "desktop",
#endif
                          document, error, size);
        goto done;
path_error:
        desktop_fail(error, size, "startup directory path is too long");
    }
#else
    desktop_fail(error, size, "login startup is unsupported on this platform");
#endif
done:
    free(exe); free(entry); free(document); return ok;
}

int zsharp_desktop_remove_startup(const char *id, char *error, size_t size) {
    ZSharpSettings settings;
    memset(&settings, 0, sizeof(settings));
    settings.project_id = (char *)id;
    settings.has_launch_at_start = 1;
    return zsharp_desktop_apply_startup(&settings, NULL, 0, error, size);
}

static int desktop_pixels(const char *text, int *value) {
    char *end;
    double number;
    errno = 0;
    number = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(number) || number < 1 ||
        number > 32768 || floor(number) != number) return 0;
    *value = (int)number; return 1;
}

/* 0 means a name, 1 a valid numeric PID/index, -1 an invalid numeric value. */
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
static int desktop_decimal(const char *text, unsigned long *value) {
    const unsigned char *p = (const unsigned char *)text;
    char *end;
    if (!p[0]) return -1;
    for (; *p; p++) if (!isdigit(*p)) return 0;
    errno = 0; *value = strtoul(text, &end, 10);
    return errno || *end || *value > 0xffffffffUL ? -1 : 1;
}
#endif

#ifdef _WIN32
typedef struct DesktopWindowSearch { DWORD pid; HWND window; unsigned count; } DesktopWindowSearch;

static BOOL CALLBACK desktop_find_window(HWND window, LPARAM data) {
    DesktopWindowSearch *search = (DesktopWindowSearch *)data;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == NULL) {
        search->window = window; search->count++;
    }
    return TRUE;
}

static int desktop_process_name_matches(const WCHAR *name, const WCHAR *selector) {
    size_t length = wcslen(name);
    if (!_wcsicmp(name, selector)) return 1;
    return length > 4 && !_wcsicmp(name + length - 4, L".exe") &&
        wcslen(selector) == length - 4 && !_wcsnicmp(name, selector, length - 4);
}

static int desktop_select_window(const char *selector, HWND *window, DWORD *selected_pid,
                                  char *error, size_t size) {
    unsigned long number = 0;
    int numeric = desktop_decimal(selector, &number);
    DWORD pid = 0;
    DesktopWindowSearch search;
    if (numeric < 0 || (numeric && number == 0)) return desktop_fail(error, size, "invalid application PID");
    if (numeric) pid = (DWORD)number;
    else {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W process;
        WCHAR *name = desktop_wide(selector);
        unsigned matches = 0;
        if (snapshot == INVALID_HANDLE_VALUE || !name) {
            if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
            free(name); return desktop_fail(error, size, "could not enumerate applications");
        }
        memset(&process, 0, sizeof(process)); process.dwSize = sizeof(process);
        if (Process32FirstW(snapshot, &process)) do {
            if (desktop_process_name_matches(process.szExeFile, name)) {
                matches++; pid = process.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &process));
        CloseHandle(snapshot); free(name);
        if (!matches) return desktop_fail(error, size, "application process '%s' was not found", selector);
        if (matches > 1) return desktop_fail(error, size, "application name '%s' is ambiguous; use its PID", selector);
    }
    memset(&search, 0, sizeof(search)); search.pid = pid;
    if (!EnumWindows(desktop_find_window, (LPARAM)&search))
        return desktop_fail(error, size, "could not enumerate application windows (Windows error %lu)", (unsigned long)GetLastError());
    if (!search.count) return desktop_fail(error, size, "application PID %lu has no visible top-level window", (unsigned long)pid);
    if (search.count > 1) return desktop_fail(error, size, "application PID %lu has multiple top-level windows; target is ambiguous", (unsigned long)pid);
    *window = search.window;
    if (selected_pid) *selected_pid = pid;
    return 1;
}

/* Closing an application is different from manipulating one of its windows:
 * browsers and other multi-process applications can have several root windows
 * in several processes. Keep the old single-window selector for move/resize/
 * minimize, but resolve one process family before requesting graceful closure.
 * A Toolhelp parent PID alone is not an identity: retain process handles and
 * compare creation times before accepting each parent/child relationship. */
#define DESKTOP_CLOSE_PROCESS_LIMIT 32768
#define DESKTOP_CLOSE_WINDOW_LIMIT 8192
#define DESKTOP_CLOSE_NONE ((size_t)-1)

typedef struct DesktopCloseProcess {
    DWORD pid, parent_pid, open_error;
    HANDLE handle;
    FILETIME created;
    size_t parent, first_child, next_child, matching_root;
    unsigned char state, matches, resolved, selected;
} DesktopCloseProcess;

typedef struct DesktopCloseWindow { HWND window; size_t process; } DesktopCloseWindow;
typedef struct DesktopCloseFamily {
    DesktopCloseProcess *processes;
    DesktopCloseWindow *windows;
    size_t process_count, window_count;
    int overflow;
} DesktopCloseFamily;

static int desktop_close_process_order(const void *left, const void *right) {
    DWORD a = ((const DesktopCloseProcess *)left)->pid;
    DWORD b = ((const DesktopCloseProcess *)right)->pid;
    return a < b ? -1 : a > b;
}

static size_t desktop_close_process_index(const DesktopCloseFamily *family, DWORD pid) {
    size_t low = 0, high = family->process_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        DWORD candidate = family->processes[middle].pid;
        if (candidate < pid) low = middle + 1;
        else if (candidate > pid) high = middle;
        else return middle;
    }
    return DESKTOP_CLOSE_NONE;
}

static void desktop_close_family_free(DesktopCloseFamily *family) {
    size_t i;
    for (i = 0; i < family->process_count; i++)
        if (family->processes[i].handle) CloseHandle(family->processes[i].handle);
    free(family->processes); free(family->windows);
}

static int desktop_close_snapshot(DesktopCloseFamily *family, const WCHAR *name,
                                    char *error, size_t size) {
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    FILETIME snapshot_start;
    size_t capacity = 0, i;
    DWORD last;
    GetSystemTimeAsFileTime(&snapshot_start);
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return desktop_fail(error, size, "could not enumerate application processes (Windows error %lu)",
                            (unsigned long)GetLastError());
    memset(&entry, 0, sizeof(entry)); entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        last = GetLastError(); CloseHandle(snapshot);
        return desktop_fail(error, size, "could not enumerate application processes (Windows error %lu)", (unsigned long)last);
    }
    do {
        DesktopCloseProcess *process;
        if (family->process_count == DESKTOP_CLOSE_PROCESS_LIMIT) {
            CloseHandle(snapshot);
            return desktop_fail(error, size, "too many application processes to select a close target safely");
        }
        if (family->process_count == capacity) {
            size_t next = capacity ? capacity * 2 : 256;
            DesktopCloseProcess *grown = (DesktopCloseProcess *)realloc(family->processes, next * sizeof(*grown));
            if (!grown) { CloseHandle(snapshot); return desktop_fail(error, size, "out of memory selecting application processes"); }
            family->processes = grown; capacity = next;
        }
        process = &family->processes[family->process_count++];
        memset(process, 0, sizeof(*process));
        process->pid = entry.th32ProcessID; process->parent_pid = entry.th32ParentProcessID;
        process->parent = process->first_child = process->next_child = process->matching_root = DESKTOP_CLOSE_NONE;
        process->matches = (unsigned char)(name && desktop_process_name_matches(entry.szExeFile, name));
    } while (Process32NextW(snapshot, &entry));
    last = GetLastError(); CloseHandle(snapshot);
    if (last != ERROR_NO_MORE_FILES)
        return desktop_fail(error, size, "could not finish enumerating application processes (Windows error %lu)", (unsigned long)last);
    qsort(family->processes, family->process_count, sizeof(family->processes[0]), desktop_close_process_order);
    for (i = 0; i < family->process_count; i++) {
        DesktopCloseProcess *process = &family->processes[i];
        FILETIME exited, kernel, user;
        if (process->pid) process->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process->pid);
        if (!process->handle) {
            process->open_error = process->pid ? GetLastError() : ERROR_ACCESS_DENIED;
            process->state = process->open_error == ERROR_INVALID_PARAMETER ? 2 : 3;
        } else if (!GetProcessTimes(process->handle, &process->created, &exited, &kernel, &user)) {
            process->open_error = GetLastError(); process->state = 3;
        } else if (WaitForSingleObject(process->handle, 0) != WAIT_TIMEOUT ||
                   CompareFileTime(&process->created, &snapshot_start) > 0) {
            /* A newly recycled PID cannot describe the snapshot's process. */
            process->state = 2;
        } else process->state = 1;
        if (process->parent_pid && process->parent_pid != process->pid)
            process->parent = desktop_close_process_index(family, process->parent_pid);
        if (process->parent != DESKTOP_CLOSE_NONE) {
            process->next_child = family->processes[process->parent].first_child;
            family->processes[process->parent].first_child = i;
        }
    }
    return 1;
}

static int desktop_close_verified_parent(const DesktopCloseFamily *family, size_t child) {
    const DesktopCloseProcess *process = &family->processes[child];
    return process->parent != DESKTOP_CLOSE_NONE && process->state == 1 &&
        family->processes[process->parent].state == 1 &&
        CompareFileTime(&family->processes[process->parent].created, &process->created) <= 0;
}

static int desktop_close_select_root(DesktopCloseFamily *family, const char *selector,
                                       int numeric, DWORD pid, size_t *root,
                                       size_t *path, char *error, size_t size) {
    size_t i;
    *root = DESKTOP_CLOSE_NONE;
    if (numeric) {
        *root = desktop_close_process_index(family, pid);
        if (*root == DESKTOP_CLOSE_NONE || family->processes[*root].state == 2)
            return desktop_fail(error, size, "application PID %lu was not found", (unsigned long)pid);
        if (family->processes[*root].state != 1)
            return desktop_fail(error, size, "could not access application PID %lu to request graceful closure (Windows error %lu)",
                                (unsigned long)pid, (unsigned long)family->processes[*root].open_error);
        return 1;
    }
    for (i = 0; i < family->process_count; i++) {
        DesktopCloseProcess *process = &family->processes[i];
        size_t at = i, depth = 0, ancestor = DESKTOP_CLOSE_NONE;
        if (!process->matches || process->state == 2) continue;
        if (process->state != 1)
            return desktop_fail(error, size, "could not access matching application PID %lu (Windows error %lu); use an accessible PID",
                                (unsigned long)process->pid, (unsigned long)process->open_error);
        while (at != DESKTOP_CLOSE_NONE && family->processes[at].resolved != 2) {
            DesktopCloseProcess *current = &family->processes[at];
            if (current->resolved == 1)
                return desktop_fail(error, size, "could not verify application process ancestry safely; use its PID");
            current->resolved = 1; path[depth++] = at;
            at = desktop_close_verified_parent(family, at) ? current->parent : DESKTOP_CLOSE_NONE;
        }
        if (at != DESKTOP_CLOSE_NONE) ancestor = family->processes[at].matching_root;
        while (depth) {
            size_t index = path[--depth];
            DesktopCloseProcess *current = &family->processes[index];
            if (ancestor == DESKTOP_CLOSE_NONE && current->matches) ancestor = index;
            current->matching_root = ancestor; current->resolved = 2;
        }
        if (*root == DESKTOP_CLOSE_NONE) *root = process->matching_root;
        else if (*root != process->matching_root)
            return desktop_fail(error, size, "application name '%s' is ambiguous between separate process families; use its PID", selector);
    }
    return *root != DESKTOP_CLOSE_NONE || desktop_fail(error, size, "application process '%s' was not found", selector);
}

static BOOL CALLBACK desktop_close_find_window(HWND window, LPARAM data) {
    DesktopCloseFamily *family = (DesktopCloseFamily *)data;
    DWORD pid = 0;
    size_t index;
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL) return TRUE;
    GetWindowThreadProcessId(window, &pid);
    index = desktop_close_process_index(family, pid);
    if (index == DESKTOP_CLOSE_NONE || !family->processes[index].selected) return TRUE;
    if (family->window_count == DESKTOP_CLOSE_WINDOW_LIMIT) { family->overflow = 1; return FALSE; }
    family->windows[family->window_count].window = window;
    family->windows[family->window_count++].process = index;
    return TRUE;
}

static int desktop_close_application(const char *selector, char *error, size_t size) {
    DesktopCloseFamily family;
    WCHAR *name = NULL;
    size_t *queue = NULL, root = DESKTOP_CLOSE_NONE, head = 0, tail = 0, i, posted = 0;
    unsigned long number = 0;
    DWORD first_error = 0;
    int numeric = desktop_decimal(selector, &number), ok = 0;
    memset(&family, 0, sizeof(family));
    if (numeric < 0 || (numeric && !number)) return desktop_fail(error, size, "invalid application PID");
    if (!numeric && !(name = desktop_wide(selector))) return desktop_fail(error, size, "invalid application name");
    if (!desktop_close_snapshot(&family, name, error, size)) goto done;
    queue = (size_t *)malloc(family.process_count * sizeof(*queue));
    family.windows = (DesktopCloseWindow *)malloc(DESKTOP_CLOSE_WINDOW_LIMIT * sizeof(*family.windows));
    if (!queue || !family.windows) { desktop_fail(error, size, "out of memory selecting application windows"); goto done; }
    if (!desktop_close_select_root(&family, selector, numeric, (DWORD)number, &root, queue, error, size)) goto done;
    family.processes[root].selected = 1; queue[tail++] = root;
    while (head < tail) {
        size_t child = family.processes[queue[head++]].first_child;
        for (; child != DESKTOP_CLOSE_NONE; child = family.processes[child].next_child) {
            DesktopCloseProcess *process = &family.processes[child];
            if (process->state == 3) {
                desktop_fail(error, size, "could not access application subprocess PID %lu to verify graceful closure (Windows error %lu)",
                             (unsigned long)process->pid, (unsigned long)process->open_error); goto done;
            }
            if (!process->selected && desktop_close_verified_parent(&family, child)) {
                process->selected = 1; queue[tail++] = child;
            }
        }
    }
    SetLastError(0);
    if (!EnumWindows(desktop_close_find_window, (LPARAM)&family)) {
        if (family.overflow) desktop_fail(error, size, "too many application windows to request closure safely");
        else desktop_fail(error, size, "could not enumerate application windows (Windows error %lu)", (unsigned long)GetLastError());
        goto done;
    }
    if (!family.window_count) {
        desktop_fail(error, size, "application PID %lu and its subprocesses have no visible top-level windows; graceful closure cannot terminate background workers",
                     (unsigned long)family.processes[root].pid); goto done;
    }
    for (i = 0; i < family.window_count; i++) {
        DesktopCloseWindow *target = &family.windows[i];
        DesktopCloseProcess *process = &family.processes[target->process];
        DWORD current_pid = 0;
        /* The app may close/recreate windows while we enumerate. Never post to
         * a recycled HWND now belonging to a different process, or a dialog. */
        if (WaitForSingleObject(process->handle, 0) != WAIT_TIMEOUT ||
            !IsWindowVisible(target->window) || GetWindow(target->window, GW_OWNER) != NULL) continue;
        GetWindowThreadProcessId(target->window, &current_pid);
        if (current_pid != process->pid) continue;
        if (PostMessageW(target->window, WM_CLOSE, 0, 0)) posted++;
        else if (!first_error) first_error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    }
    if (first_error) desktop_fail(error, size, "could not request graceful closure of every application window (Windows error %lu); some windows may already have received the request",
                                  (unsigned long)first_error);
    else if (!posted) desktop_fail(error, size, "application windows changed before graceful closure could be requested; try again");
    else ok = 1;
done:
    free(name); free(queue); desktop_close_family_free(&family); return ok;
}

/* Serialize this process's automation so two script tasks cannot interleave
 * modifier changes. This does not lock out the user: a changed foreground
 * target causes an error rather than typing into the newly selected app. */
static SRWLOCK desktop_windows_input_lock = SRWLOCK_INIT;

static HANDLE desktop_windows_pin_window(HWND window, DWORD expected_pid,
                                           const FILETIME *started,
                                           char *error, size_t size) {
    HANDLE process;
    FILETIME created, exited, kernel, user;
    DWORD pid = 0, last;
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || !IsWindowEnabled(window) ||
        !GetWindowThreadProcessId(window, &pid) || !pid || (expected_pid && pid != expected_pid)) {
        desktop_fail(error, size, "application window is unavailable, disabled by a dialog, or changed"); return NULL;
    }
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        desktop_fail(error, size, "could not access application window process (Windows error %lu)", (unsigned long)GetLastError());
        return NULL;
    }
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        last = GetLastError(); CloseHandle(process);
        desktop_fail(error, size, "could not verify application window process (Windows error %lu)", (unsigned long)last);
        return NULL;
    }
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT || CompareFileTime(&created, started) > 0) {
        CloseHandle(process); desktop_fail(error, size, "application window process exited or changed"); return NULL;
    }
    return process;
}

static int desktop_windows_target_current(HWND window, HANDLE process, DWORD pid,
                                            int foreground, char *error, size_t size) {
    DWORD current_pid = 0;
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT || !IsWindow(window) ||
        !IsWindowVisible(window) || !IsWindowEnabled(window) ||
        !GetWindowThreadProcessId(window, &current_pid) || current_pid != pid ||
        (foreground && GetForegroundWindow() != window))
        return desktop_fail(error, size, "application focus or window identity changed; no further input was sent");
    return 1;
}

/* SetForegroundWindow's cross-thread activation is asynchronous. A harmless
 * bounded message waits for that activation request without joining the
 * target's input queue or changing Windows' foreground-stealing protection.
 * Return -1 only for a responsive, valid window that did not gain focus. */
static int desktop_windows_activate(HWND window, HANDLE process, DWORD pid,
                                     char *error, size_t size) {
    DWORD_PTR ignored = 0;
    DWORD last;
    if (!desktop_windows_target_current(window, process, pid, 0, error, size)) return 0;
    if (IsIconic(window) && !ShowWindowAsync(window, SW_RESTORE)) {
        return desktop_fail(error, size, "could not restore application window (Windows error %lu)", (unsigned long)GetLastError());
    }
    if (GetForegroundWindow() != window) {
        /* A zero return can race with the target becoming foreground, so the
         * authoritative result is checked after the target processes WM_NULL. */
        (void)SetForegroundWindow(window);
    }
    SetLastError(ERROR_SUCCESS);
    if (!SendMessageTimeoutW(window, WM_NULL, 0, 0,
                             SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &ignored)) {
        last = GetLastError();
        return desktop_fail(error, size, "application window did not respond while switching focus (Windows error %lu); no input was sent",
                            (unsigned long)last);
    }
    if (!desktop_windows_target_current(window, process, pid, 0, error, size)) return 0;
    if (GetForegroundWindow() != window) return -1;
    if (IsIconic(window))
        return desktop_fail(error, size, "application window restoration is still pending; wait for it to restore and try again");
    return 1;
}

static int desktop_windows_focus(const char *selector, char *error, size_t size) {
    HWND window;
    DWORD pid = 0;
    FILETIME started;
    HANDLE process = NULL;
    WCHAR *target_name = NULL, prompt[1024];
    size_t target_length;
    int truncated = 0;
    int activation, choice, ok = 0;
    AcquireSRWLockExclusive(&desktop_windows_input_lock);
    GetSystemTimeAsFileTime(&started);
    if (!desktop_select_window(selector, &window, &pid, error, size)) goto done;
    process = desktop_windows_pin_window(window, pid, &started, error, size);
    if (!process) goto done;
    activation = desktop_windows_activate(window, process, pid, error, size);
    if (activation >= 0) { ok = activation; goto done; }
    /* A background process cannot grant itself foreground permission. An
     * explicit click on our confirmation is normal user input, allowing one
     * fresh attempt. Never report success unless the intended window really
     * is foreground: later typeText/sendKeys must not reach some other app. */
    target_name = desktop_wide(selector);
    if (!target_name) {
        desktop_fail(error, size, "could not prepare the application name for focus confirmation; no input was sent"); goto done;
    }
    target_length = wcslen(target_name);
    if (target_length > 256) {
        target_length = 256;
        /* Keep a supplementary Unicode character intact at the display
         * boundary instead of leaving half of its UTF-16 surrogate pair. */
        if (target_name[target_length - 1] >= 0xD800 && target_name[target_length - 1] <= 0xDBFF)
            target_length--;
        target_name[target_length] = 0;
        truncated = 1;
    }
    if (swprintf(prompt, sizeof(prompt) / sizeof(prompt[0]),
        L"Windows blocked automatic switching to the selected application.\n\n"
        L"Target: %ls%ls\n\n"
        L"Click OK to switch to that application, or Cancel to leave your current window unchanged.\n\n"
        L"No text or keyboard shortcut has been sent.",
        target_name, truncated ? L"..." : L"") < 0) {
        desktop_fail(error, size, "could not prepare the application focus confirmation; no input was sent"); goto done;
    }
    free(target_name); target_name = NULL;
    choice = MessageBoxW(NULL, prompt,
        L"Z# Desktop - switch application",
        MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (choice != IDOK) {
        desktop_fail(error, size, "application focus switch was cancelled; no input was sent"); goto done;
    }
    activation = desktop_windows_activate(window, process, pid, error, size);
    if (activation < 0)
        desktop_fail(error, size, "Windows denied bringing the application to the foreground after confirmation; activate it manually and try again; no input was sent");
    else ok = activation;
done:
    free(target_name);
    if (process) CloseHandle(process);
    ReleaseSRWLockExclusive(&desktop_windows_input_lock);
    return ok;
}

static unsigned desktop_windows_modifiers(void) {
    unsigned held = 0;
    if ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000)) held |= DESKTOP_INPUT_CTRL;
    if ((GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000)) held |= DESKTOP_INPUT_ALT;
    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000)) held |= DESKTOP_INPUT_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) held |= DESKTOP_INPUT_SUPER;
    return held;
}

static int desktop_windows_mouse_held(void) {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000) || (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) ||
        (GetAsyncKeyState(VK_XBUTTON2) & 0x8000);
}

static void desktop_windows_key_input(INPUT *input, WORD key, WORD character, DWORD flags) {
    memset(input, 0, sizeof(*input)); input->type = INPUT_KEYBOARD;
    input->ki.wVk = key; input->ki.wScan = character; input->ki.dwFlags = flags;
}

/* SendInput can accept only part of a batch. Never replay text/a shortcut:
 * best-effort release only downs inserted by that accepted prefix. Requested
 * modifiers already held by the user are absent from the batch entirely. */
static int desktop_windows_inject(INPUT *inputs, UINT count, char *error, size_t size) {
    INPUT releases[16];
    UINT sent, i, pending = 0;
    DWORD last;
    SetLastError(0);
    sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent == count) return 1;
    last = GetLastError();
    if (sent > count) sent = count;
    for (i = 0; i < sent; i++) {
        INPUT *input = &inputs[i];
        if (input->type == INPUT_KEYBOARD) {
            UINT j;
            if (!(input->ki.dwFlags & KEYEVENTF_KEYUP)) {
                if (pending < sizeof(releases) / sizeof(releases[0])) {
                    releases[pending] = *input;
                    releases[pending++].ki.dwFlags |= KEYEVENTF_KEYUP;
                }
            } else for (j = 0; j < pending; j++) {
                if (releases[j].type == INPUT_KEYBOARD && releases[j].ki.wVk == input->ki.wVk &&
                    releases[j].ki.wScan == input->ki.wScan) {
                    releases[j] = releases[--pending]; break;
                }
            }
        } else if (input->type == INPUT_MOUSE) {
            if ((input->mi.dwFlags & MOUSEEVENTF_LEFTDOWN) && pending < sizeof(releases) / sizeof(releases[0])) {
                memset(&releases[pending], 0, sizeof(releases[pending]));
                releases[pending].type = INPUT_MOUSE;
                releases[pending++].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            } else if (input->mi.dwFlags & MOUSEEVENTF_LEFTUP) {
                UINT j;
                for (j = 0; j < pending; j++) if (releases[j].type == INPUT_MOUSE) {
                    releases[j] = releases[--pending]; break;
                }
            }
        }
    }
    for (i = 0; i < pending / 2; i++) {
        INPUT swap = releases[i]; releases[i] = releases[pending - i - 1]; releases[pending - i - 1] = swap;
    }
    if (pending) (void)SendInput(pending, releases, sizeof(INPUT));
    return desktop_fail(error, size,
        "Windows accepted %u of %u input events (Windows error %lu); input may be blocked by permissions or a higher-integrity app, and partial input was not replayed",
        sent, count, (unsigned long)last);
}

static int desktop_windows_shortcut_key(int key, WORD *virtual_key, DWORD *flags) {
    *flags = 0;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) *virtual_key = (WORD)key;
    else if (key >= DESKTOP_KEY_F1 && key <= DESKTOP_KEY_F24) *virtual_key = (WORD)(VK_F1 + key - DESKTOP_KEY_F1);
    else switch (key) {
        case DESKTOP_KEY_ENTER: *virtual_key = VK_RETURN; break;
        case DESKTOP_KEY_TAB: *virtual_key = VK_TAB; break;
        case DESKTOP_KEY_SPACE: *virtual_key = VK_SPACE; break;
        case DESKTOP_KEY_ESCAPE: *virtual_key = VK_ESCAPE; break;
        case DESKTOP_KEY_BACKSPACE: *virtual_key = VK_BACK; break;
        case DESKTOP_KEY_DELETE: *virtual_key = VK_DELETE; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_INSERT: *virtual_key = VK_INSERT; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_HOME: *virtual_key = VK_HOME; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_END: *virtual_key = VK_END; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_PAGEUP: *virtual_key = VK_PRIOR; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_PAGEDOWN: *virtual_key = VK_NEXT; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_LEFT: *virtual_key = VK_LEFT; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_RIGHT: *virtual_key = VK_RIGHT; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_UP: *virtual_key = VK_UP; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_DOWN: *virtual_key = VK_DOWN; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_MEDIA_PLAY_PAUSE: *virtual_key = VK_MEDIA_PLAY_PAUSE; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_MEDIA_NEXT: *virtual_key = VK_MEDIA_NEXT_TRACK; *flags = KEYEVENTF_EXTENDEDKEY; break;
        case DESKTOP_KEY_MEDIA_PREVIOUS: *virtual_key = VK_MEDIA_PREV_TRACK; *flags = KEYEVENTF_EXTENDEDKEY; break;
        default: return 0;
    }
    return 1;
}

static int desktop_windows_send_keys(const char *text, char *error, size_t size) {
    static const unsigned modifiers[] = {DESKTOP_INPUT_CTRL, DESKTOP_INPUT_ALT, DESKTOP_INPUT_SHIFT, DESKTOP_INPUT_SUPER};
    static const WORD modifier_keys[] = {VK_LCONTROL, VK_LMENU, VK_LSHIFT, VK_LWIN};
    DesktopInputChord chord;
    INPUT inputs[10];
    UINT count = 0;
    unsigned held, inserted;
    size_t i;
    WORD key;
    DWORD flags, pid = 0;
    HWND window;
    HANDLE process = NULL;
    FILETIME started;
    int ok = 0;
    if (!desktop_input_chord(text, &chord) || !desktop_windows_shortcut_key(chord.key, &key, &flags))
        return desktop_fail(error, size, "invalid desktop key chord");
    AcquireSRWLockExclusive(&desktop_windows_input_lock);
    GetSystemTimeAsFileTime(&started);
    window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &pid);
    process = desktop_windows_pin_window(window, pid, &started, error, size);
    if (!process) goto done;
    held = desktop_windows_modifiers();
    if ((held & ~chord.modifiers) || (GetAsyncKeyState(key) & 0x8000)) {
        desktop_fail(error, size, "release unrelated held modifiers and the shortcut key before Desktop.sendKeys"); goto done;
    }
    inserted = chord.modifiers & ~held;
    for (i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++) if (inserted & modifiers[i])
        desktop_windows_key_input(&inputs[count++], modifier_keys[i], 0, modifiers[i] == DESKTOP_INPUT_SUPER ? KEYEVENTF_EXTENDEDKEY : 0);
    desktop_windows_key_input(&inputs[count++], key, 0, flags);
    desktop_windows_key_input(&inputs[count++], key, 0, flags | KEYEVENTF_KEYUP);
    for (i = sizeof(modifiers) / sizeof(modifiers[0]); i > 0; i--) if (inserted & modifiers[i - 1])
        desktop_windows_key_input(&inputs[count++], modifier_keys[i - 1], 0,
            KEYEVENTF_KEYUP | (modifiers[i - 1] == DESKTOP_INPUT_SUPER ? KEYEVENTF_EXTENDEDKEY : 0));
    if (!desktop_windows_target_current(window, process, pid, 1, error, size)) goto done;
    if (desktop_windows_modifiers() != held || (GetAsyncKeyState(key) & 0x8000)) {
        desktop_fail(error, size, "keyboard state changed before the shortcut; no input was sent"); goto done;
    }
    ok = desktop_windows_inject(inputs, count, error, size);
done:
    if (process) CloseHandle(process);
    ReleaseSRWLockExclusive(&desktop_windows_input_lock);
    return ok;
}

typedef HANDLE (WINAPI *DesktopThreadDpiContext)(HANDLE context);

/* A successful SendInput only queues events; it does not mean the receiver has
 * translated its VK_PACKET messages. Feeding an entire paragraph at once can
 * overwhelm applications' text-input paths. Pace Unicode scalars instead,
 * keeping their down/up events (and both halves of a surrogate pair) together.
 * The wait is deliberately a typing cadence, not an input-consumption ACK. */
static int desktop_windows_text_wait(HANDLE timer, HANDLE process, DWORD milliseconds,
                                     char *error, size_t size) {
    HANDLE waits[2] = {process, timer};
    LARGE_INTEGER due;
    DWORD result;
    due.QuadPart = -(LONGLONG)milliseconds * 10000;
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        return desktop_fail(error, size, "could not schedule desktop typing (Windows error %lu); no further input was sent",
                            (unsigned long)GetLastError());
    result = WaitForMultipleObjects(2, waits, FALSE, milliseconds + 1000);
    if (result == WAIT_OBJECT_0 + 1) return 1;
    if (result == WAIT_OBJECT_0)
        return desktop_fail(error, size, "application window process exited; no further input was sent");
    if (result == WAIT_TIMEOUT)
        return desktop_fail(error, size, "desktop typing timer did not complete; no further input was sent");
    return desktop_fail(error, size, "could not wait for desktop typing (Windows error %lu); no further input was sent",
                        (unsigned long)GetLastError());
}

static int desktop_windows_text_focus(HWND window, DWORD thread, HWND expected,
                                      DWORD expected_thread, DWORD expected_pid,
                                      HWND *focused, char *error, size_t size) {
    GUITHREADINFO info;
    DWORD pid = 0, focus_thread;
    memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
    if (GetWindowThreadProcessId(window, NULL) != thread || !GetGUIThreadInfo(thread, &info) ||
        !info.hwndFocus || !IsWindow(info.hwndFocus) || !IsWindowVisible(info.hwndFocus) ||
        !IsWindowEnabled(info.hwndFocus) || GetAncestor(info.hwndFocus, GA_ROOT) != window ||
        (expected && info.hwndFocus != expected))
        return desktop_fail(error, size, "text input focus is unavailable or changed; no further input was sent");
    focus_thread = GetWindowThreadProcessId(info.hwndFocus, &pid);
    if (!focus_thread || !pid || (expected_thread && focus_thread != expected_thread) ||
        (expected_pid && pid != expected_pid))
        return desktop_fail(error, size, "text input window identity changed; no further input was sent");
    *focused = info.hwndFocus;
    return 1;
}

/* Native Unicode edit controls expose an insertion interface. EM_REPLACESEL
 * changes only the current selection (or inserts at its caret), preserves
 * undo, and handles paragraph endings without VK_PACKET/WM_CHAR translation.
 * Only use pointer-bearing insertion across processes, where Windows marshals
 * this system message and owns the remote copy even after a timeout. A same-
 * process recipient could retain our pointer after a timeout. Custom/browser
 * and same-process widgets therefore keep the existing keyboard path. */
static int desktop_windows_text_native_edit(HWND focused) {
    WCHAR name[64];
    if (!IsWindowUnicode(focused) || !GetClassNameW(focused, name, 64)) return 0;
    return !_wcsicmp(name, L"Edit") || !_wcsnicmp(name, L"RichEdit", 8);
}

static int desktop_windows_text_edit_writable(HWND focused, char *error, size_t size) {
    LONG_PTR style;
    DWORD code;
    SetLastError(ERROR_SUCCESS);
    style = GetWindowLongPtrW(focused, GWL_STYLE);
    code = GetLastError();
    if ((!style && code != ERROR_SUCCESS) || (style & ES_READONLY))
        return desktop_fail(error, size, "the selected native text input is read-only or unavailable; no text was entered");
    return 1;
}

static WCHAR *desktop_windows_text_edit_string(const WCHAR *text) {
    size_t length = wcslen(text), i, at = 0;
    WCHAR *normalized;
    if (length > (SIZE_MAX / sizeof(WCHAR) - 1) / 2) return NULL;
    normalized = (WCHAR *)malloc((length * 2 + 1) * sizeof(WCHAR));
    if (!normalized) return NULL;
    for (i = 0; i < length; i++) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            if (text[i] == L'\r' && i + 1 < length && text[i + 1] == L'\n') i++;
            normalized[at++] = L'\r'; normalized[at++] = L'\n';
        } else normalized[at++] = text[i];
    }
    normalized[at] = 0;
    return normalized;
}

static int desktop_windows_text_insert(HWND focused, const WCHAR *text,
                                       char *error, size_t size) {
    DWORD_PTR result = 0;
    DWORD code;
    if (!desktop_windows_text_edit_writable(focused, error, size)) return 0;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutW(focused, EM_REPLACESEL, TRUE, (LPARAM)text,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                            1000, &result)) return 1;
    code = GetLastError();
    /* A timeout may happen after the insertion was accepted. Never retry or
     * fall back to keys, since that could duplicate all or part of the text. */
    return desktop_fail(error, size,
        "text input control did not acknowledge insertion (Windows error %lu); "
        "text may have been entered; no further input was sent",
        (unsigned long)code);
}

static int desktop_windows_text_edit_current(HWND window, HWND edit, DWORD thread,
                                              DWORD expected_pid, char *error, size_t size) {
    DWORD pid = 0;
    if (!IsWindow(edit) || !IsWindowVisible(edit) || !IsWindowEnabled(edit) ||
        GetAncestor(edit, GA_ROOT) != window ||
        GetWindowThreadProcessId(edit, &pid) != thread || pid != expected_pid ||
        !desktop_windows_text_native_edit(edit))
        return desktop_fail(error, size, "native text input identity changed; no further input was sent");
    return 1;
}

static int desktop_windows_text_click(HWND edit, UINT message, LPARAM position,
                                      char *error, size_t size) {
    DWORD_PTR result = 0;
    DWORD code;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutW(edit, message, message == WM_LBUTTONDOWN ? MK_LBUTTON : 0,
        position, SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &result)) return 1;
    code = GetLastError();
    return desktop_fail(error, size,
        "native text input did not acknowledge the click (Windows error %lu); no text was entered",
        (unsigned long)code);
}

static void desktop_windows_text_cancel(HWND window, HANDLE process, DWORD pid,
    HWND edit, DWORD edit_thread, DWORD edit_pid, HANDLE edit_process) {
    DWORD_PTR result = 0;
    char ignored[128];
    if (!desktop_windows_target_current(window, process, pid, 1, ignored, sizeof(ignored)) ||
        !desktop_windows_text_edit_current(window, edit, edit_thread, edit_pid, ignored, sizeof(ignored)) ||
        (edit_process && WaitForSingleObject(edit_process, 0) != WAIT_TIMEOUT)) return;
    /* Cancel only capture on the still-verified original control. This is
     * cleanup, not a replayed mouse-up, click, or insertion. */
    (void)SendMessageTimeoutW(edit, WM_CANCELMODE, 0, 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &result);
}

static void desktop_windows_text_failure_stage(const char *stage, char *error, size_t size) {
    char detail[512];
    snprintf(detail, sizeof(detail), "%s", error && size && error[0] ? error : "native input validation failed");
    desktop_fail(error, size, "%s: %s", stage, detail);
}

static int desktop_windows_type_text(const char *text, const char *region_name,
                                       char *error, size_t size) {
    DesktopInputRegion region;
    DesktopThreadDpiContext set_dpi = NULL;
    HANDLE previous_dpi = NULL, process = NULL, focus_process = NULL, timer = NULL;
    HMODULE user32;
    WCHAR *wide = NULL, *edit_text = NULL;
    INPUT inputs[4];
    HWND window, hit, focused = NULL, current_focus;
    DWORD pid = 0, thread = 0, focus_pid = 0, focus_thread = 0;
    FILETIME started;
    RECT client, edit_client;
    POINT point, edit_point;
    int left, top, width, height, native_edit = 0, ok = 0;
    size_t length, i;
    UINT count;
    if (!desktop_input_text_valid(text) || !desktop_input_region(region_name, &region))
        return desktop_fail(error, size, "Desktop.typeText requires valid UTF-8 text and a valid window region");
    if (!text[0]) return 1;
    wide = desktop_wide(text);
    if (!wide) return desktop_fail(error, size, "could not prepare Unicode desktop text");
    length = wcslen(wide);
    AcquireSRWLockExclusive(&desktop_windows_input_lock);
    GetSystemTimeAsFileTime(&started);
    window = GetForegroundWindow();
    if (window) thread = GetWindowThreadProcessId(window, &pid);
    process = desktop_windows_pin_window(window, pid, &started, error, size);
    if (!process) goto done;
    if (IsIconic(window)) { desktop_fail(error, size, "focused application window is minimized"); goto done; }
    if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
        desktop_fail(error, size, "release held modifiers and mouse buttons before Desktop.typeText"); goto done;
    }
    /* Client/screen coordinates and absolute mouse input must use the same
     * physical pixel space, including monitors with different DPI scales. */
    user32 = GetModuleHandleW(L"user32.dll");
    if (user32) set_dpi = (DesktopThreadDpiContext)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
    if (set_dpi) {
        previous_dpi = set_dpi((HANDLE)(INT_PTR)-4);
        if (!previous_dpi) previous_dpi = set_dpi((HANDLE)(INT_PTR)-3);
        if (!previous_dpi) { desktop_fail(error, size, "could not establish physical desktop coordinates"); goto done; }
    }
    if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top) {
        desktop_fail(error, size, "could not read the focused window client area"); goto done;
    }
    point.x = client.left + (LONG)(((long long)client.right - client.left) * (region.column * 2 + 1) / 6);
    point.y = client.top + (LONG)(((long long)client.bottom - client.top) * (region.row * 2 + 1) / 6);
    if (!ClientToScreen(window, &point)) { desktop_fail(error, size, "could not locate the focused window region"); goto done; }
    left = GetSystemMetrics(SM_XVIRTUALSCREEN); top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    width = GetSystemMetrics(SM_CXVIRTUALSCREEN); height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width < 2 || height < 2 || (long long)point.x < left || (long long)point.y < top ||
        (long long)point.x >= (long long)left + width || (long long)point.y >= (long long)top + height ||
        !MonitorFromPoint(point, MONITOR_DEFAULTTONULL)) {
        desktop_fail(error, size, "the requested window region is outside the visible desktop"); goto done;
    }
    hit = WindowFromPoint(point);
    if (!hit || GetAncestor(hit, GA_ROOT) != window) {
        desktop_fail(error, size, "the requested window region is covered by another application or dialog"); goto done;
    }
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[0].mi.dx = (LONG)(((long long)point.x - left) * 65535 / (width - 1));
    inputs[0].mi.dy = (LONG)(((long long)point.y - top) * 65535 / (height - 1));
    native_edit = desktop_windows_text_native_edit(hit);
    if (native_edit) {
        focus_thread = GetWindowThreadProcessId(hit, &focus_pid);
        if (focus_pid == GetCurrentProcessId()) native_edit = 0;
    }
    if (native_edit) {
        LPARAM position;
        focused = hit;
        if (focus_pid != pid) {
            focus_process = desktop_windows_pin_window(hit, focus_pid, &started, error, size);
            if (!focus_process) goto done;
        }
        if (!desktop_windows_text_edit_current(window, hit, focus_thread, focus_pid, error, size) ||
            !desktop_windows_text_edit_writable(hit, error, size)) goto done;
        edit_point = point;
        if (!ScreenToClient(hit, &edit_point) || !GetClientRect(hit, &edit_client) ||
            edit_point.x < edit_client.left || edit_point.y < edit_client.top ||
            edit_point.x >= edit_client.right || edit_point.y >= edit_client.bottom ||
            edit_point.x < SHRT_MIN || edit_point.x > SHRT_MAX ||
            edit_point.y < SHRT_MIN || edit_point.y > SHRT_MAX) {
            desktop_fail(error, size, "the requested native text point is outside its supported client coordinates; no input was sent"); goto done;
        }
        edit_text = desktop_windows_text_edit_string(wide);
        if (!edit_text) { desktop_fail(error, size, "could not prepare native text insertion; no input was sent"); goto done; }
        position = MAKELPARAM((WORD)edit_point.x, (WORD)edit_point.y);
        if (!desktop_windows_target_current(window, process, pid, 1, error, size)) goto done;
        if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
            desktop_fail(error, size, "keyboard or mouse state changed before typing; no input was sent"); goto done;
        }
        if (!desktop_windows_inject(inputs, 1, error, size)) goto done;
        if (!desktop_windows_target_current(window, process, pid, 1, error, size) ||
            !desktop_windows_text_edit_current(window, hit, focus_thread, focus_pid, error, size)) goto done;
        if (focus_process && WaitForSingleObject(focus_process, 0) != WAIT_TIMEOUT) {
            desktop_fail(error, size, "text input window process exited before clicking; no text was entered"); goto done;
        }
        if (WindowFromPoint(point) != hit) {
            desktop_fail(error, size, "the native text input became covered before clicking; no text was entered"); goto done;
        }
        if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
            desktop_fail(error, size, "keyboard or mouse state changed before clicking; no text was entered"); goto done;
        }
        if (!desktop_windows_text_click(hit, WM_LBUTTONDOWN, position, error, size)) {
            desktop_windows_text_cancel(window, process, pid, hit, focus_thread, focus_pid, focus_process);
            goto done;
        }
        if (!desktop_windows_target_current(window, process, pid, 1, error, size) ||
            !desktop_windows_text_focus(window, thread, hit, focus_thread, focus_pid, &current_focus, error, size)) {
            desktop_windows_text_failure_stage("after native mouse-down", error, size);
            desktop_windows_text_cancel(window, process, pid, hit, focus_thread, focus_pid, focus_process);
            goto done;
        }
        if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
            desktop_fail(error, size, "keyboard or mouse state changed while clicking; no text was entered");
            desktop_windows_text_cancel(window, process, pid, hit, focus_thread, focus_pid, focus_process);
            goto done;
        }
        if (!desktop_windows_text_click(hit, WM_LBUTTONUP, position, error, size)) {
            desktop_windows_text_cancel(window, process, pid, hit, focus_thread, focus_pid, focus_process);
            goto done;
        }
        if (!desktop_windows_target_current(window, process, pid, 1, error, size) ||
            !desktop_windows_text_focus(window, thread, hit, focus_thread, focus_pid, &current_focus, error, size) ||
            !desktop_windows_text_edit_current(window, hit, focus_thread, focus_pid, error, size)) {
            desktop_windows_text_failure_stage("after native mouse-up", error, size);
            goto done;
        }
        if (focus_process && WaitForSingleObject(focus_process, 0) != WAIT_TIMEOUT) {
            desktop_fail(error, size, "text input window process exited; no further input was sent"); goto done;
        }
        if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
            desktop_fail(error, size, "keyboard or mouse state changed before native insertion; no text was entered"); goto done;
        }
        ok = desktop_windows_text_insert(hit, edit_text, error, size);
        goto done;
    }
    /* Windows 10 1803+ supports the high-resolution flag (0x2). Older Windows
     * may reject it, in which case a regular waitable timer preserves pacing.
     * Prepare and exercise the timer before even clicking the target. */
    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) {
        desktop_fail(error, size, "could not create desktop typing timer (Windows error %lu); no input was sent",
                     (unsigned long)GetLastError()); goto done;
    }
    if (!desktop_windows_text_wait(timer, process, 1, error, size)) goto done;
    inputs[1].type = INPUT_MOUSE; inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].type = INPUT_MOUSE; inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (!desktop_windows_target_current(window, process, pid, 1, error, size)) goto done;
    if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
        desktop_fail(error, size, "keyboard or mouse state changed before typing; no input was sent"); goto done;
    }
    if (!desktop_windows_inject(inputs, 3, error, size) ||
        !desktop_windows_text_wait(timer, process, 10, error, size)) goto done;
    if (!desktop_windows_target_current(window, process, pid, 1, error, size) ||
        !desktop_windows_text_focus(window, thread, NULL, 0, 0, &focused, error, size)) goto done;
    focus_thread = GetWindowThreadProcessId(focused, &focus_pid);
    if (focus_pid != pid) {
        focus_process = desktop_windows_pin_window(focused, focus_pid, &started, error, size);
        if (!focus_process) goto done;
    }
    for (i = 0; i < length; i++) {
        WCHAR character = wide[i];
        if (!desktop_windows_target_current(window, process, pid, 1, error, size) ||
            !desktop_windows_text_focus(window, thread, focused, focus_thread, focus_pid,
                                        &current_focus, error, size)) goto done;
        if (focus_process && WaitForSingleObject(focus_process, 0) != WAIT_TIMEOUT) {
            desktop_fail(error, size, "text input window process exited; no further input was sent"); goto done;
        }
        if (desktop_windows_modifiers() || desktop_windows_mouse_held()) {
            desktop_fail(error, size, "keyboard or mouse state changed while typing; no further input was sent"); goto done;
        }
        count = 0;
        if (character == L'\r' && i + 1 < length && wide[i + 1] == L'\n') i++;
        if (character == L'\n') character = L'\r';
        desktop_windows_key_input(&inputs[count++], 0, character, KEYEVENTF_UNICODE);
        desktop_windows_key_input(&inputs[count++], 0, character, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        if (character >= 0xd800 && character <= 0xdbff && i + 1 < length &&
            wide[i + 1] >= 0xdc00 && wide[i + 1] <= 0xdfff) {
            character = wide[++i];
            desktop_windows_key_input(&inputs[count++], 0, character, KEYEVENTF_UNICODE);
            desktop_windows_key_input(&inputs[count++], 0, character, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        }
        if (!desktop_windows_inject(inputs, count, error, size) ||
            !desktop_windows_text_wait(timer, process, 2, error, size)) goto done;
    }
    ok = 1;
done:
    if (previous_dpi) (void)set_dpi(previous_dpi);
    if (timer) CloseHandle(timer);
    if (focus_process) CloseHandle(focus_process);
    if (process) CloseHandle(process);
    ReleaseSRWLockExclusive(&desktop_windows_input_lock);
    free(edit_text); free(wide); return ok;
}

typedef struct DesktopMonitor { RECT work; WCHAR device[32]; int primary; } DesktopMonitor;
typedef struct DesktopMonitors { DesktopMonitor monitors[64]; size_t count; int overflow; } DesktopMonitors;
static BOOL CALLBACK desktop_find_monitor(HMONITOR monitor, HDC dc, LPRECT area, LPARAM data) {
    DesktopMonitors *list = (DesktopMonitors *)data;
    MONITORINFOEXW info;
    DesktopMonitor *entry;
    (void)dc; (void)area;
    if (list->count == 64) { list->overflow = 1; return FALSE; }
    memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) return TRUE;
    entry = &list->monitors[list->count++];
    entry->work = info.rcWork; entry->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    lstrcpynW(entry->device, info.szDevice, 32); return TRUE;
}
static int desktop_monitor_order(const void *left, const void *right) {
    const DesktopMonitor *a = (const DesktopMonitor *)left, *b = (const DesktopMonitor *)right;
    if (a->primary != b->primary) return b->primary - a->primary;
    return _wcsicmp(a->device, b->device);
}
static int desktop_move_window(HWND window, const char *selector, char *error, size_t size) {
    DesktopMonitors list;
    DesktopMonitor *target = NULL;
    unsigned long index = 0;
    int numeric = desktop_decimal(selector, &index);
    RECT bounds;
    int x, y;
    memset(&list, 0, sizeof(list));
    if (!EnumDisplayMonitors(NULL, NULL, desktop_find_monitor, (LPARAM)&list) || list.overflow)
        return desktop_fail(error, size, "could not enumerate display monitors");
    qsort(list.monitors, list.count, sizeof(list.monitors[0]), desktop_monitor_order);
    if (numeric < 0) return desktop_fail(error, size, "invalid display index");
    if (numeric) {
        if (index >= list.count) return desktop_fail(error, size, "display index %lu was not found", index);
        target = &list.monitors[index];
    } else {
        WCHAR *name = desktop_wide(selector);
        size_t i;
        if (!name) return desktop_fail(error, size, "invalid display name");
        for (i = 0; i < list.count; i++) if (!_wcsicmp(name, list.monitors[i].device)) target = &list.monitors[i];
        free(name);
        if (!target) return desktop_fail(error, size, "display '%s' was not found; use an index or Windows device name", selector);
    }
    if (!GetWindowRect(window, &bounds)) return desktop_fail(error, size, "could not read application window bounds");
    x = target->work.left + (target->work.right - target->work.left - (bounds.right - bounds.left)) / 2;
    y = target->work.top + (target->work.bottom - target->work.top - (bounds.bottom - bounds.top)) / 2;
    if (x < target->work.left) x = target->work.left;
    if (y < target->work.top) y = target->work.top;
    return SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ||
        desktop_fail(error, size, "could not move application window (Windows error %lu)", (unsigned long)GetLastError());
}

static int desktop_windows_action(const char *operation, const char *const *args,
                                    char *error, size_t size) {
    HWND window;
    if (!strcmp(operation, "openApp")) {
        WCHAR *name = desktop_wide(args[0]), resolved[32768];
        WCHAR *command;
        char *resolved_text, *quoted;
        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        DWORD n;
        int ok;
        if (!name) return desktop_fail(error, size, "invalid executable path");
        n = SearchPathW(NULL, name, L".exe", 32768, resolved, NULL); free(name);
        if (!n || n >= 32768) return desktop_fail(error, size, "application executable '%s' was not found", args[0]);
        resolved_text = desktop_utf8(resolved);
        quoted = resolved_text ? quote_windows(resolved_text) : NULL;
        command = quoted ? desktop_wide(quoted) : NULL;
        free(resolved_text); free(quoted);
        if (!command) return desktop_fail(error, size, "out of memory preparing application launch");
        memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
        memset(&process, 0, sizeof(process));
        ok = CreateProcessW(resolved, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
        free(command);
        if (!ok) return desktop_fail(error, size, "could not open application (Windows error %lu)", (unsigned long)GetLastError());
        CloseHandle(process.hThread); CloseHandle(process.hProcess); return 1;
    }
    if (!strcmp(operation, "closeApp"))
        return desktop_close_application(args[0], error, size);
    if (!strcmp(operation, "focusWindow")) return desktop_windows_focus(args[0], error, size);
    if (!strcmp(operation, "typeText")) return desktop_windows_type_text(args[0], args[1], error, size);
    if (!strcmp(operation, "sendKeys")) return desktop_windows_send_keys(args[0], error, size);
    if (!desktop_select_window(args[0], &window, NULL, error, size)) return 0;
    if (!strcmp(operation, "minimizeApp") || !strcmp(operation, "maximizeApp"))
        return ShowWindowAsync(window, !strcmp(operation, "minimizeApp") ? SW_MINIMIZE : SW_MAXIMIZE) || desktop_fail(error, size, "could not change application window state (Windows error %lu)", (unsigned long)GetLastError());
    if (!strcmp(operation, "scaleApp")) {
        int width, height;
        if (!desktop_pixels(args[1], &width) || !desktop_pixels(args[2], &height))
            return desktop_fail(error, size, "application width/height must be whole pixels from 1 to 32768");
        return SetWindowPos(window, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
            desktop_fail(error, size, "could not resize application window (Windows error %lu)", (unsigned long)GetLastError());
    }
    return desktop_move_window(window, args[1], error, size);
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
#include <pthread.h>
static pthread_mutex_t desktop_posix_input_mutex = PTHREAD_MUTEX_INITIALIZER;

static int desktop_posix_is_input(const char *operation) {
    return !strcmp(operation, "focusWindow") || !strcmp(operation, "typeText") || !strcmp(operation, "sendKeys");
}

/* Fixed executables and argv arrays only: app names/IDs never enter a shell.
 * Bound helper output and lifetime so a denied/hung automation service does
 * not leave an unbounded worker behind. This kills only our own helper child. */
static int desktop_command(char *const arguments[], char *output, size_t capacity,
                             char *error, size_t size) {
    int descriptors[2], status = 0, done = 0, overflow = 0;
    pid_t child;
    size_t used = 0;
    struct timespec start, now;
    if (pipe(descriptors) != 0) return desktop_fail(error, size, "could not create desktop helper pipe");
    child = fork();
    if (child == 0) {
        dup2(descriptors[1], STDOUT_FILENO); dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[0]); close(descriptors[1]);
        execvp(arguments[0], arguments);
        { const char failure[] = "desktop helper executable is unavailable\n"; write(STDERR_FILENO, failure, sizeof(failure) - 1); }
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); return desktop_fail(error, size, "could not start desktop helper"); }
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!done) {
        char buffer[4096];
        ssize_t count;
        unsigned reads = 0;
        pid_t waited;
        struct pollfd poller;
        poller.fd = descriptors[0]; poller.events = POLLIN | POLLHUP; poller.revents = 0;
        (void)poll(&poller, 1, 100);
        while ((count = read(descriptors[0], buffer, sizeof(buffer))) > 0) {
            size_t copy = (size_t)count;
            if (capacity && used < capacity - 1) {
                if (copy > capacity - 1 - used) copy = capacity - 1 - used;
                memcpy(output + used, buffer, copy); used += copy;
            } else copy = 0;
            if (copy != (size_t)count) overflow = 1;
            if (++reads == 32) break;
        }
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child) done = 1;
        else if (waited < 0 && errno != EINTR) {
            close(descriptors[0]);
            return desktop_fail(error, size, "could not observe desktop helper completion");
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!done && now.tv_sec - start.tv_sec >= 20) {
            kill(child, SIGKILL); waitpid(child, &status, 0); close(descriptors[0]);
            if (capacity) output[used] = '\0';
            return desktop_fail(error, size, "desktop helper timed out; check desktop/automation permissions");
        }
    }
    /* Drain bytes already buffered when waitpid observed process exit. */
    for (;;) {
        char buffer[4096]; ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        size_t copy;
        if (count <= 0) break;
        copy = (size_t)count;
        if (capacity && used < capacity - 1) {
            if (copy > capacity - 1 - used) copy = capacity - 1 - used;
            memcpy(output + used, buffer, copy); used += copy;
        } else copy = 0;
        if (copy != (size_t)count) overflow = 1;
    }
    close(descriptors[0]);
    if (capacity) output[used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return desktop_fail(error, size, "desktop operation failed: %.420s", used ? output : "permission denied or desktop helper unavailable");
    if (overflow) return desktop_fail(error, size, "desktop helper output exceeds the bounded enumeration limit");
    return 1;
}

#ifdef __linux__
static int desktop_spawn_app(const char *program, char *error, size_t size) {
    int descriptors[2], status;
    pid_t child;
    ssize_t count;
    int launch_error = 0;
    if (pipe(descriptors) != 0) return desktop_fail(error, size, "could not create application launch pipe");
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child == 0) {
        pid_t app;
        close(descriptors[0]);
        app = fork();
        if (app > 0) _exit(0);
        if (app < 0) { int e = errno; write(descriptors[1], &e, sizeof(e)); _exit(1); }
        setsid();
        { char *const args[] = {(char *)program, NULL}; execvp(program, args); }
        { int e = errno; write(descriptors[1], &e, sizeof(e)); }
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); return desktop_fail(error, size, "could not start application"); }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    do { count = read(descriptors[0], &launch_error, sizeof(launch_error)); } while (count < 0 && errno == EINTR);
    close(descriptors[0]);
    if (count > 0) return desktop_fail(error, size, "could not open application: %s", strerror(launch_error));
    return count == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 :
        desktop_fail(error, size, "could not confirm application launch");
}
#endif
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
typedef struct DesktopXDisplay DesktopXDisplay;

/* X11 is optional and resolved at the point of use. Observe physical state;
 * never release/repress the user's held keys or buttons to "clear" modifiers.
 * No X11 headers or build-time libX11 dependency are needed. */
static int desktop_linux_input_ready(char *error, size_t size) {
    void *library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    DesktopXDisplay *display = NULL;
    int (*init_threads)(void);
    DesktopXDisplay *(*open_display)(const char *);
    int (*close_display)(DesktopXDisplay *);
    int (*query_keymap)(DesktopXDisplay *, char *);
    int (*screen_count)(DesktopXDisplay *);
    unsigned long (*root_window)(DesktopXDisplay *, int);
    int (*query_pointer)(DesktopXDisplay *, unsigned long, unsigned long *, unsigned long *, int *, int *, int *, int *, unsigned *);
    char map[32];
    size_t i;
    int screen, ok = 0, have_pointer = 0;
    unsigned mask = 0;
    if (!library) return desktop_fail(error, size, "desktop input requires the optional X11 library libX11.so.6");
#define DESKTOP_X11_LOAD(destination, symbol) do { \
    void *address = dlsym(library, symbol); \
    if (!address || sizeof(destination) != sizeof(address)) { \
        desktop_fail(error, size, "the optional X11 library does not provide the required desktop input APIs"); goto done; \
    } \
    memcpy(&(destination), &address, sizeof(destination)); \
} while (0)
    DESKTOP_X11_LOAD(init_threads, "XInitThreads");
    DESKTOP_X11_LOAD(open_display, "XOpenDisplay");
    DESKTOP_X11_LOAD(close_display, "XCloseDisplay");
    DESKTOP_X11_LOAD(query_keymap, "XQueryKeymap");
    DESKTOP_X11_LOAD(screen_count, "XScreenCount");
    DESKTOP_X11_LOAD(root_window, "XRootWindow");
    DESKTOP_X11_LOAD(query_pointer, "XQueryPointer");
#undef DESKTOP_X11_LOAD
    if (!init_threads() || !(display = open_display(NULL))) {
        desktop_fail(error, size, "desktop input could not connect to the X11 DISPLAY; native Wayland input is unsupported"); goto done;
    }
    if (!query_keymap(display, map)) { desktop_fail(error, size, "could not inspect X11 keyboard state safely"); goto done; }
    for (i = 0; i < sizeof(map); i++) if ((unsigned char)map[i]) {
        desktop_fail(error, size, "release held keyboard keys before desktop input"); goto done;
    }
    for (screen = 0; screen < screen_count(display); screen++) {
        unsigned long root, child;
        int root_x, root_y, window_x, window_y;
        if (query_pointer(display, root_window(display, screen), &root, &child,
                          &root_x, &root_y, &window_x, &window_y, &mask)) { have_pointer = 1; break; }
    }
    if (!have_pointer) { desktop_fail(error, size, "could not inspect X11 pointer state safely"); goto done; }
    if (mask & 0x1f00u) { desktop_fail(error, size, "release held mouse buttons before desktop input"); goto done; }
    if (mask & 2u) { desktop_fail(error, size, "turn Caps Lock off before desktop input"); goto done; }
    ok = 1;
done:
    if (display) close_display(display);
    dlclose(library); return ok;
}

static int desktop_linux_name_matches(unsigned long pid, const char *name) {
    char path[128], executable[PATH_MAX + 1];
    ssize_t length;
    const char *base;
    snprintf(path, sizeof(path), "/proc/%lu/exe", pid);
    length = readlink(path, executable, sizeof(executable) - 1);
    if (length > 0 && length < (ssize_t)sizeof(executable) - 1) {
        executable[length] = '\0'; base = strrchr(executable, '/');
        if (!strcmp(base ? base + 1 : executable, name)) return 1;
    }
    snprintf(path, sizeof(path), "/proc/%lu/comm", pid);
    {
        FILE *file = fopen(path, "rb");
        if (!file) return 0;
        if (!fgets(executable, sizeof(executable), file)) { fclose(file); return 0; }
        fclose(file); executable[strcspn(executable, "\r\n")] = '\0';
    }
    return !strcmp(executable, name);
}

static int desktop_linux_active_window(unsigned long *window, char *error, size_t size) {
    char output[256], *end;
    char *command[] = {"xdotool", "getactivewindow", NULL};
    if (!desktop_command(command, output, sizeof(output), error, size)) return 0;
    errno = 0; *window = strtoul(output, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (errno || !*window || end == output || *end)
        return desktop_fail(error, size, "could not identify the active X11 window");
    return 1;
}

static int desktop_linux_same_active_window(unsigned long window, char *error, size_t size) {
    unsigned long active;
    return desktop_linux_active_window(&active, error, size) &&
        (active == window || desktop_fail(error, size, "the focused application window changed before desktop input; no further input was sent"));
}

static const char *desktop_linux_key_name(int key, char name[16]) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        name[0] = (char)(key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key);
        name[1] = '\0'; return name;
    }
    if (key >= DESKTOP_KEY_F1 && key <= DESKTOP_KEY_F24) {
        snprintf(name, 16, "F%d", key - DESKTOP_KEY_F1 + 1); return name;
    }
    switch (key) {
    case DESKTOP_KEY_ENTER: return "Return";
    case DESKTOP_KEY_TAB: return "Tab";
    case DESKTOP_KEY_SPACE: return "space";
    case DESKTOP_KEY_ESCAPE: return "Escape";
    case DESKTOP_KEY_BACKSPACE: return "BackSpace";
    case DESKTOP_KEY_DELETE: return "Delete";
    case DESKTOP_KEY_INSERT: return "Insert";
    case DESKTOP_KEY_HOME: return "Home";
    case DESKTOP_KEY_END: return "End";
    case DESKTOP_KEY_PAGEUP: return "Prior";
    case DESKTOP_KEY_PAGEDOWN: return "Next";
    case DESKTOP_KEY_LEFT: return "Left";
    case DESKTOP_KEY_RIGHT: return "Right";
    case DESKTOP_KEY_UP: return "Up";
    case DESKTOP_KEY_DOWN: return "Down";
    case DESKTOP_KEY_MEDIA_PLAY_PAUSE: return "XF86AudioPlay";
    case DESKTOP_KEY_MEDIA_NEXT: return "XF86AudioNext";
    case DESKTOP_KEY_MEDIA_PREVIOUS: return "XF86AudioPrev";
    default: return NULL;
    }
}

/* Only literal argv reaches xdotool. In particular, type --args 1 -- makes a
 * string such as "--file /etc/passwd" text, never another xdotool operation.
 * Held modifier/button state is rejected, never cleared. No clipboard changes. */
static int desktop_linux_input(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    char output[2048], id[32];
    unsigned long window;
    if (!strcmp(operation, "typeText") && !args[0][0]) return 1;
    if (!desktop_linux_input_ready(error, size)) return 0;
    if (!desktop_linux_active_window(&window, error, size)) return 0;
    snprintf(id, sizeof(id), "%lu", window);
    if (!strcmp(operation, "sendKeys")) {
        DesktopInputChord parsed;
        char key_buffer[16], sequence[96];
        const char *key;
        char *command[] = {"xdotool", "key", sequence, NULL};
        if (!desktop_input_chord(args[0], &parsed) || !(key = desktop_linux_key_name(parsed.key, key_buffer)))
            return desktop_fail(error, size, "unsupported desktop key chord");
        snprintf(sequence, sizeof(sequence), "%s%s%s%s%s",
            parsed.modifiers & DESKTOP_INPUT_CTRL ? "ctrl+" : "",
            parsed.modifiers & DESKTOP_INPUT_ALT ? "alt+" : "",
            parsed.modifiers & DESKTOP_INPUT_SHIFT ? "shift+" : "",
            parsed.modifiers & DESKTOP_INPUT_SUPER ? "super+" : "", key);
        if (!desktop_linux_same_active_window(window, error, size) || !desktop_linux_input_ready(error, size)) return 0;
        return desktop_command(command, output, sizeof(output), error, size);
    } else {
        DesktopInputRegion region;
        char *cursor = NULL, *line;
        char x_text[32], y_text[32];
        long width = 0, height = 0;
        unsigned fields = 0;
        char *geometry[] = {"xdotool", "getwindowgeometry", "--shell", id, NULL};
        if (!desktop_input_region(args[1], &region) || !desktop_input_text_valid(args[0]))
            return desktop_fail(error, size, "invalid desktop text or region");
        if (!desktop_command(geometry, output, sizeof(output), error, size)) return 0;
        /* Parse named numeric values, never evaluate xdotool's shell output. */
        for (line = strtok_r(output, "\n", &cursor); line; line = strtok_r(NULL, "\n", &cursor)) {
            char *end;
            long value;
            if (!strncmp(line, "WIDTH=", 6)) {
                errno = 0; value = strtol(line + 6, &end, 10);
                if (errno || *end || value < 1 || value > 1048576 || (fields & 1))
                    return desktop_fail(error, size, "invalid active X11 window dimensions");
                width = value; fields |= 1;
            } else if (!strncmp(line, "HEIGHT=", 7)) {
                errno = 0; value = strtol(line + 7, &end, 10);
                if (errno || *end || value < 1 || value > 1048576 || (fields & 2))
                    return desktop_fail(error, size, "invalid active X11 window dimensions");
                height = value; fields |= 2;
            }
        }
        if (fields != 3) return desktop_fail(error, size, "could not read the active X11 window dimensions");
        snprintf(x_text, sizeof(x_text), "%ld", (width - 1) * (2 * region.column + 1) / 6);
        snprintf(y_text, sizeof(y_text), "%ld", (height - 1) * (2 * region.row + 1) / 6);
        if (!desktop_linux_same_active_window(window, error, size) || !desktop_linux_input_ready(error, size)) return 0;
        {
            char *click[] = {"xdotool", "mousemove", "--sync", "--window", id,
                x_text, y_text, "click", "1", NULL};
            if (!desktop_command(click, output, sizeof(output), error, size)) return 0;
        }
        if (!desktop_linux_same_active_window(window, error, size) || !desktop_linux_input_ready(error, size)) return 0;
        if (!args[0][0]) return 1;
        {
            char *type[] = {"xdotool", "type", "--delay", "0", "--args", "1", "--", (char *)args[0], NULL};
            return desktop_command(type, output, sizeof(output), error, size);
        }
    }
}

static int desktop_linux_action_impl(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    char *listing, *line, *cursor = NULL;
    char id_text[32], geometry[128], small_output[1024];
    unsigned long selected = 0, number = 0;
    int numeric, width = 0, height = 0;
    unsigned matches = 0;
    char *list_arguments[] = {"wmctrl", "-lpG", NULL};
    int ok;
    if (!strcmp(operation, "openApp")) return desktop_spawn_app(args[0], error, size);
    if (!strcmp(operation, "typeText") && !args[0][0]) return 1;
    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0])
        return desktop_fail(error, size, "desktop window management requires an X11 DISPLAY and wmctrl; native Wayland windows are not supported");
    if (!strcmp(operation, "typeText") || !strcmp(operation, "sendKeys"))
        return desktop_linux_input(operation, args, error, size);
    numeric = desktop_decimal(args[0], &number);
    if (numeric < 0 || (numeric && !number)) return desktop_fail(error, size, "invalid application PID");
    listing = (char *)malloc(262144);
    if (!listing) return desktop_fail(error, size, "out of memory");
    if (!desktop_command(list_arguments, listing, 262144, error, size)) { free(listing); return 0; }
    for (line = strtok_r(listing, "\n", &cursor); line; line = strtok_r(NULL, "\n", &cursor)) {
        unsigned long id, pid;
        long desktop;
        int x, y, w, h;
        if (sscanf(line, "%lx %ld %lu %d %d %d %d", &id, &desktop, &pid, &x, &y, &w, &h) != 7) continue;
        if ((numeric && pid == number) || (!numeric && desktop_linux_name_matches(pid, args[0]))) {
            selected = id; matches++; width = w; height = h;
        }
    }
    free(listing);
    if (!matches) return desktop_fail(error, size, "application '%s' has no matching X11 window (or is a native Wayland app)", args[0]);
    if (matches > 1) return desktop_fail(error, size, "application '%s' matches multiple windows; target is ambiguous", args[0]);
    snprintf(id_text, sizeof(id_text), "0x%lx", selected);
    if (!strcmp(operation, "focusWindow")) {
        char *command[] = {"xdotool", "windowactivate", "--sync", id_text, NULL};
        if (!desktop_command(command, small_output, sizeof(small_output), error, size)) return 0;
        return desktop_linux_same_active_window(selected, error, size);
    }
    if (!strcmp(operation, "closeApp")) {
        char *command[] = {"wmctrl", "-ic", id_text, NULL};
        return desktop_command(command, small_output, sizeof(small_output), error, size);
    }
    if (!strcmp(operation, "minimizeApp")) {
        char *command[] = {"xdotool", "windowminimize", id_text, NULL};
        return desktop_command(command, small_output, sizeof(small_output), error, size);
    }
    if (!strcmp(operation, "maximizeApp")) {
        char *command[] = {"wmctrl", "-ir", id_text, "-b", "add,maximized_vert,maximized_horz", NULL};
        return desktop_command(command, small_output, sizeof(small_output), error, size);
    }
    if (!strcmp(operation, "scaleApp")) {
        if (!desktop_pixels(args[1], &width) || !desktop_pixels(args[2], &height))
            return desktop_fail(error, size, "application width/height must be whole pixels from 1 to 32768");
        snprintf(geometry, sizeof(geometry), "0,-1,-1,%d,%d", width, height);
    } else {
        char monitors[65536];
        char *command[] = {"xrandr", "--listactivemonitors", NULL};
        unsigned long display = 0;
        int display_numeric = desktop_decimal(args[1], &display);
        unsigned found = 0;
        if (display_numeric < 0) return desktop_fail(error, size, "invalid display index");
        if (!desktop_command(command, monitors, sizeof(monitors), error, size)) return 0;
        cursor = NULL;
        for (line = strtok_r(monitors, "\n", &cursor); line; line = strtok_r(NULL, "\n", &cursor)) {
            unsigned long index;
            char flags[128], bounds[128], name[128];
            int mw, mh, mx, my;
            if (sscanf(line, " %lu: %127s %127s %127s", &index, flags, bounds, name) != 4 ||
                sscanf(bounds, "%d/%*dx%d/%*d+%d+%d", &mw, &mh, &mx, &my) != 4) continue;
            if ((display_numeric && index == display) || (!display_numeric && !strcmp(name, args[1]))) {
                int x = mx + (mw > width ? (mw - width) / 2 : 0);
                int y = my + (mh > height ? (mh - height) / 2 : 0);
                snprintf(geometry, sizeof(geometry), "0,%d,%d,-1,-1", x, y); found++;
            }
        }
        if (found != 1) return desktop_fail(error, size, "display '%s' is missing or ambiguous", args[1]);
    }
    {
        char *command[] = {"wmctrl", "-ir", id_text, "-e", geometry, NULL};
        ok = desktop_command(command, small_output, sizeof(small_output), error, size);
    }
    return ok;
}

static int desktop_linux_action(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    int input = desktop_posix_is_input(operation), ok;
    if (input && pthread_mutex_lock(&desktop_posix_input_mutex) != 0)
        return desktop_fail(error, size, "could not serialize desktop input");
    ok = desktop_linux_action_impl(operation, args, error, size);
    if (input) pthread_mutex_unlock(&desktop_posix_input_mutex);
    return ok;
}
#endif

#ifdef __APPLE__
static int desktop_macos_key_code(int key) {
    static const unsigned char letters[] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    static const unsigned char digits[] = {29,18,19,20,21,23,22,26,28,25};
    static const unsigned char functions[] = {122,120,99,118,96,97,98,100,101,109,103,111,105,107,113,106,64,79,80,90};
    if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
    if (key >= '0' && key <= '9') return digits[key - '0'];
    if (key >= DESKTOP_KEY_F1 && key < DESKTOP_KEY_F1 + 20) return functions[key - DESKTOP_KEY_F1];
    switch (key) {
    case DESKTOP_KEY_ENTER: return 36;
    case DESKTOP_KEY_TAB: return 48;
    case DESKTOP_KEY_SPACE: return 49;
    case DESKTOP_KEY_ESCAPE: return 53;
    case DESKTOP_KEY_BACKSPACE: return 51;
    case DESKTOP_KEY_DELETE: return 117;
    case DESKTOP_KEY_HOME: return 115;
    case DESKTOP_KEY_END: return 119;
    case DESKTOP_KEY_PAGEUP: return 116;
    case DESKTOP_KEY_PAGEDOWN: return 121;
    case DESKTOP_KEY_LEFT: return 123;
    case DESKTOP_KEY_RIGHT: return 124;
    case DESKTOP_KEY_UP: return 126;
    case DESKTOP_KEY_DOWN: return 125;
    /* Separate from hardware keyboard key codes: NX_KEYTYPE_* consumer
     * events use the system-defined NSEvent path below. */
    case DESKTOP_KEY_MEDIA_PLAY_PAUSE: return 1016;
    case DESKTOP_KEY_MEDIA_NEXT: return 1017;
    case DESKTOP_KEY_MEDIA_PREVIOUS: return 1018;
    default: return -1;
    }
}

static int desktop_macos_input(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    char output[4096], row[16] = "0", column[16] = "0", key[16] = "0", modifiers[16] = "0";
    if (!strcmp(operation, "typeText") && !args[0][0]) return 1;
    /* This source is constant. Text, selectors and numeric values are separate
     * argv values, never evaluated as JavaScript or sent through a clipboard. */
    static const char script[] =
        "ObjC.import('AppKit');ObjC.import('ApplicationServices');\n"
        "function run(a){\n"
        " if(!$.AXIsProcessTrusted())throw Error('Desktop input requires Accessibility permission for the ZVM or its launching app in macOS settings.');\n"
        " var se=Application('System Events'),op=a[0],p,ws,w;\n"
        " if(op==='focusWindow'){\n"
        "  var numeric=/^[0-9]+$/.test(a[1]);\n"
        "  var ps=(numeric?se.applicationProcesses.whose({unixId:Number(a[1])}):se.applicationProcesses.whose({name:a[1]}))();\n"
        "  if(ps.length!==1)throw Error('Application is missing or ambiguous; use a unique PID/name.');\n"
        "  p=ps[0];ws=p.windows();if(ws.length!==1)throw Error('Application window is missing or ambiguous.');w=ws[0];\n"
        "  w.attributes.byName('AXMinimized').value=false;p.frontmost=true;w.actions.byName('AXRaise').perform();\n"
        "  if(!p.frontmost())throw Error('macOS did not allow the application to become focused.');return;\n"
        " }\n"
        " var ps=se.applicationProcesses.whose({frontmost:true})();\n"
        " if(ps.length!==1)throw Error('There is no unique focused application.');p=ps[0];\n"
        " var pid=Number(p.unixId());\n"
        " var axApp=$.AXUIElementCreateApplication(pid),saved=Ref();\n"
        " if(!axApp)throw Error('Could not identify the focused macOS application.');\n"
        " if($.AXUIElementCopyAttributeValue(axApp,$('AXFocusedWindow'),saved)!==0||!saved[0]){if(saved[0])$.CFRelease(saved[0]);$.CFRelease(axApp);throw Error('The focused application has no accessible focused window.');}\n"
        " try{\n"
        " function active(){\n"
        "  if(Number($.NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier)!==pid)throw Error('The focused application changed before desktop input; no further input was sent.');\n"
        "  var current=Ref();try{if($.AXUIElementCopyAttributeValue(axApp,$('AXFocusedWindow'),current)!==0||!current[0]||!$.CFEqual(current[0],saved[0]))throw Error('The focused window changed before desktop input; no further input was sent.');}finally{if(current[0])$.CFRelease(current[0]);}\n"
        " }\n"
        " var held=Number($.CGEventSourceFlagsState(1));\n"
        " if(held&1966080)throw Error('Release held Control, Option, Shift and Command keys before desktop input.');\n"
        " function keyboard(code,flags,text){\n"
        "  var down=$.CGEventCreateKeyboardEvent(null,code,true),up=$.CGEventCreateKeyboardEvent(null,code,false);\n"
        "  if(!down||!up){if(down)$.CFRelease(down);if(up)$.CFRelease(up);throw Error('Could not allocate a macOS keyboard event.');}\n"
        "  try{$.CGEventSetFlags(down,flags);$.CGEventSetFlags(up,flags);\n"
        "   if(text!==null){var data=$(text).dataUsingEncoding($.NSUTF16LittleEndianStringEncoding);$.CGEventKeyboardSetUnicodeString(down,text.length,data.bytes);$.CGEventKeyboardSetUnicodeString(up,text.length,data.bytes);}\n"
        "   active();$.CGEventPost(0,down);$.CGEventPost(0,up);\n"
        "  }finally{$.CFRelease(down);$.CFRelease(up);}\n"
        " }\n"
        " if(op==='sendKeys'){var m=Number(a[5]),code=Number(a[4]),flags=0;\n"
        "  if(code>=1016&&code<=1018){\n"
        "   if(m)throw Error('Media keys cannot be combined with modifiers.');\n"
        "   var nx=code-1000,point=$.NSMakePoint(0,0);\n"
        "   var md=$.NSEvent.otherEventWithTypeLocationModifierFlagsTimestampWindowNumberContextSubtypeData1Data2(14,point,2560,0,0,null,8,(nx<<16)|2560,-1);\n"
        "   var mu=$.NSEvent.otherEventWithTypeLocationModifierFlagsTimestampWindowNumberContextSubtypeData1Data2(14,point,2816,0,0,null,8,(nx<<16)|2816,-1);\n"
        "   if(!md||!mu)throw Error('Could not allocate a macOS media key event.');\n"
        "   var cd=md.CGEvent,cu=mu.CGEvent;if(!cd||!cu)throw Error('Could not convert a macOS media key event.');\n"
        "   active();$.CGEventPost(0,cd);$.CGEventPost(0,cu);return;\n"
        "  }\n"
        "  if(m&1)flags|=262144;if(m&2)flags|=524288;if(m&4)flags|=131072;if(m&8)flags|=1048576;keyboard(code,flags,null);return;\n"
        " }\n"
        " for(var button=0;button<5;button++)if($.CGEventSourceButtonState(1,button))throw Error('Release held mouse buttons before desktop typing.');\n"
        " w=p.attributes.byName('AXFocusedWindow').value();if(!w)throw Error('The focused application has no accessible focused window.');\n"
        " var xy=w.position(),sz=w.size(),r=Number(a[2]),c=Number(a[3]);\n"
        " if(sz.length!==2||!(sz[0]>0&&sz[1]>0))throw Error('Could not read the focused window dimensions.');\n"
        " var point=$.CGPointMake(Math.floor(xy[0]+(sz[0]-1)*(2*c+1)/6),Math.floor(xy[1]+(sz[1]-1)*(2*r+1)/6));\n"
        " var move=$.CGEventCreateMouseEvent(null,5,point,0),down=$.CGEventCreateMouseEvent(null,1,point,0),up=$.CGEventCreateMouseEvent(null,2,point,0);\n"
        " if(!move||!down||!up){if(move)$.CFRelease(move);if(down)$.CFRelease(down);if(up)$.CFRelease(up);throw Error('Could not allocate a macOS mouse event.');}\n"
        " try{active();$.CGEventPost(0,move);$.CGEventPost(0,down);$.CGEventPost(0,up);}finally{$.CFRelease(move);$.CFRelease(down);$.CFRelease(up);}\n"
        " active();var text=a[1];\n"
        " for(var i=0;i<text.length;i++){var ch=text.charCodeAt(i),piece=text.charAt(i);if(ch>=55296&&ch<=56319&&i+1<text.length)piece+=text.charAt(++i);\n"
        "  if(piece==='\\r'){if(text.charAt(i+1)==='\\n')i++;keyboard(36,0,null);}else if(piece==='\\n')keyboard(36,0,null);else if(piece==='\\t')keyboard(48,0,null);else keyboard(0,0,piece);\n"
        " }\n"
        " }finally{$.CFRelease(saved[0]);$.CFRelease(axApp);}\n"
        "}\n";
    if (!strcmp(operation, "typeText")) {
        DesktopInputRegion region;
        if (!desktop_input_region(args[1], &region) || !desktop_input_text_valid(args[0]))
            return desktop_fail(error, size, "invalid desktop text or region");
        snprintf(row, sizeof(row), "%d", region.row); snprintf(column, sizeof(column), "%d", region.column);
    } else if (!strcmp(operation, "sendKeys")) {
        DesktopInputChord chord;
        int code;
        if (!desktop_input_chord(args[0], &chord) || (code = desktop_macos_key_code(chord.key)) < 0)
            return desktop_fail(error, size, "this key is unavailable through the macOS keyboard backend (Insert and F21-F24 are unsupported)");
        snprintf(key, sizeof(key), "%d", code); snprintf(modifiers, sizeof(modifiers), "%u", chord.modifiers);
    }
    {
        char *command[] = {"/usr/bin/osascript", "-l", "JavaScript", "-e", (char *)script, "--",
            (char *)operation, (char *)args[0], row, column, key, modifiers, NULL};
        return desktop_command(command, output, sizeof(output), error, size);
    }
}

static int desktop_macos_action_impl(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    char output[4096];
    /* Fixed JXA source. All caller data is argv, never evaluated as code. */
    static const char script[] =
        "ObjC.import('AppKit');\n"
        "function run(a) {\n"
        " var se=Application('System Events'), numeric=/^[0-9]+$/.test(a[0]);\n"
        " var ps=(numeric?se.applicationProcesses.whose({unixId:Number(a[0])}):se.applicationProcesses.whose({name:a[0]}))();\n"
        " if(ps.length!==1) throw Error('Application is missing or ambiguous; use a unique PID/name.');\n"
        " var p=ps[0], op=a[1];\n"
        " if(op==='closeApp'){var id=p.bundleIdentifier();if(!id)throw Error('Application has no bundle ID for graceful quit.');Application(id).quit();return;}\n"
        " var ws=p.windows();if(ws.length!==1)throw Error('Application window is missing or ambiguous.');var w=ws[0];\n"
        " if(op==='minimizeApp'){w.attributes.byName('AXMinimized').value=true;return;}\n"
        " if(op==='scaleApp'){w.size=[Number(a[2]),Number(a[3])];return;}\n"
        " var screens=$.NSScreen.screens, target=null, total=Number(screens.count);\n"
        " if(op==='maximizeApp')target=$.NSScreen.mainScreen;\n"
        " else if(/^[0-9]+$/.test(a[2])){var n=Number(a[2]);if(n<total)target=screens.objectAtIndex(n);}\n"
        " else {for(var i=0;i<total;i++){var s=screens.objectAtIndex(i);if(ObjC.unwrap(s.localizedName)===a[2]){if(target)throw Error('Display name is ambiguous.');target=s;}}}\n"
        " if(!target)throw Error('Display was not found.');\n"
        " var r=target.visibleFrame, primary=screens.objectAtIndex(0).frame, top=primary.origin.y+primary.size.height;\n"
        " var sz=op==='maximizeApp'?[r.size.width,r.size.height]:w.size();\n"
        " w.position=[r.origin.x+Math.max(0,(r.size.width-sz[0])/2),top-r.origin.y-r.size.height+Math.max(0,(r.size.height-sz[1])/2)];\n"
        " if(op==='maximizeApp')w.size=sz;\n"
        "}\n";
    if (!strcmp(operation, "openApp")) {
        char *command[] = {"/usr/bin/open", "-a", (char *)args[0], NULL};
        return desktop_command(command, output, sizeof(output), error, size);
    }
    if (!strcmp(operation, "focusWindow") || !strcmp(operation, "typeText") || !strcmp(operation, "sendKeys"))
        return desktop_macos_input(operation, args, error, size);
    if (!strcmp(operation, "scaleApp")) {
        int width, height;
        if (!desktop_pixels(args[1], &width) || !desktop_pixels(args[2], &height))
            return desktop_fail(error, size, "application width/height must be whole pixels from 1 to 32768");
    }
    {
        char *command[] = {"/usr/bin/osascript", "-l", "JavaScript", "-e", (char *)script, "--",
            (char *)args[0], (char *)operation,
            (!strcmp(operation, "moveApp") || !strcmp(operation, "scaleApp")) ? (char *)args[1] : "",
            !strcmp(operation, "scaleApp") ? (char *)args[2] : "", NULL};
        return desktop_command(command, output, sizeof(output), error, size);
    }
}

static int desktop_macos_action(const char *operation, const char *const *args,
                                  char *error, size_t size) {
    int input = desktop_posix_is_input(operation), ok;
    if (input && pthread_mutex_lock(&desktop_posix_input_mutex) != 0)
        return desktop_fail(error, size, "could not serialize desktop input");
    ok = desktop_macos_action_impl(operation, args, error, size);
    if (input) pthread_mutex_unlock(&desktop_posix_input_mutex);
    return ok;
}
#endif

int zsharp_desktop_action(const char *operation, const char *const *args,
                          size_t count, char *error, size_t size) {
    size_t expected, i;
    if (error != NULL && size != 0) error[0] = '\0';
    if (!operation) return desktop_fail(error, size, "missing desktop operation");
    if (!strcmp(operation, "closeApp") || !strcmp(operation, "openApp") ||
        !strcmp(operation, "minimizeApp") || !strcmp(operation, "maximizeApp") ||
        !strcmp(operation, "focusWindow") || !strcmp(operation, "sendKeys")) expected = 1;
    else if (!strcmp(operation, "scaleApp")) expected = 3;
    else if (!strcmp(operation, "moveApp") || !strcmp(operation, "typeText")) expected = 2;
    else return desktop_fail(error, size, "unknown desktop operation '%s'", operation);
    if (!args || count != expected) return desktop_fail(error, size, "%s expects %zu argument(s)", operation, expected);
    for (i = 0; i < count; i++) {
        if (!strcmp(operation, "typeText") && i == 0) {
            if (!desktop_input_text_valid(args[i]))
                return desktop_fail(error, size, "typeText requires valid UTF-8 text of at most 65536 bytes; only newline and tab control characters are supported");
        } else if (!valid_argument(args[i])) return desktop_fail(error, size, "desktop arguments must be nonempty text without control characters");
    }
    if (!strcmp(operation, "typeText")) {
        DesktopInputRegion region;
        if (!desktop_input_region(args[1], &region))
            return desktop_fail(error, size, "unknown typeText region; use topleft, topmiddle, topright, middleleft, center, middleright, bottomleft, bottommiddle or bottomright");
    }
    if (!strcmp(operation, "sendKeys")) {
        DesktopInputChord chord;
        if (!desktop_input_chord(args[0], &chord))
            return desktop_fail(error, size, "invalid keyboard shortcut; use modifiers followed by one supported key, such as CTRL+S");
    }
    if (!strcmp(operation, "scaleApp")) {
        int width, height;
        if (!desktop_pixels(args[1], &width) || !desktop_pixels(args[2], &height))
            return desktop_fail(error, size, "application width/height must be whole pixels from 1 to 32768");
    }
    if (getenv("ZSHARP_DESKTOP_DISABLE_ACTIONS")) return desktop_fail(error, size, "desktop actions are disabled for this test session");
#ifdef _WIN32
    return desktop_windows_action(operation, args, error, size);
#elif defined(__APPLE__)
    return desktop_macos_action(operation, args, error, size);
#elif defined(__linux__) && !defined(__ANDROID__)
    return desktop_linux_action(operation, args, error, size);
#else
    return desktop_fail(error, size, "desktop management is unavailable on this platform");
#endif
}
