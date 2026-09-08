#define _CRT_SECURE_NO_WARNINGS

#include "updater.h"

#include "zsharp.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZSHARP_UPDATE_MANIFEST_LIMIT (1024u * 1024u)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
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

static char *executable_path(void) {
#ifdef _WIN32
    WCHAR wide_path[32768];
    DWORD length = GetModuleFileNameW(NULL, wide_path,
                                     (DWORD)(sizeof(wide_path) /
                                             sizeof(wide_path[0])));
    int bytes;
    char *path;
    if (length == 0 ||
        length >= (DWORD)(sizeof(wide_path) / sizeof(wide_path[0])))
        return NULL;
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, NULL, 0,
                                NULL, NULL);
    path = bytes == 0 ? NULL : (char *)malloc((size_t)bytes);
    if (path == NULL || WideCharToMultiByte(CP_UTF8, 0, wide_path, -1,
                                             path, bytes, NULL, NULL) == 0) {
        free(path);
        return NULL;
    }
    return path;
#elif defined(__linux__)
    {
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
    }
    return NULL;
#else
    {
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
    }
    return NULL;
#endif
}

static char *parent_directory(const char *path) {
    const char *last = NULL;
    const char *cursor;
    char *result;
    for (cursor = path; *cursor != '\0'; cursor++)
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    if (last == NULL) return NULL;
    result = (char *)malloc((size_t)(last - path) + 1);
    if (result == NULL) return NULL;
    memcpy(result, path, (size_t)(last - path));
    result[last - path] = '\0';
    return result;
}

static void current_version(char *version, size_t capacity) {
    snprintf(version, capacity, "%d.%d.%d.%d",
             ZSHARP_VERSION_MAJOR, ZSHARP_VERSION_MINOR,
             ZSHARP_VERSION_PATCH, ZSHARP_VERSION_REVISION);
}

void zsharp_update_preferences_default(ZSharpUpdatePreferences *preferences) {
    if (preferences == NULL) return;
    preferences->automatic_updates = 1;
    preferences->beta_updates = 0;
    preferences->automatic_beta_updates = 0;
}

#ifdef _WIN32
static int preference_dword(HKEY key, const WCHAR *name, int fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    return RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) ==
               ERROR_SUCCESS && type == REG_DWORD
        ? value != 0 : fallback;
}

int zsharp_update_preferences_load(ZSharpUpdatePreferences *preferences,
                                   char *error, size_t error_size) {
    HKEY key = NULL;
    (void)error;
    (void)error_size;
    zsharp_update_preferences_default(preferences);
    if (preferences == NULL) return 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\ZombieOS\\ZSharp\\Updates", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return 1;
    preferences->automatic_updates = preference_dword(
        key, L"AutomaticUpdates", preferences->automatic_updates);
    preferences->beta_updates = preference_dword(
        key, L"BetaUpdates", preferences->beta_updates);
    preferences->automatic_beta_updates = preference_dword(
        key, L"AutomaticBetaUpdates", preferences->automatic_beta_updates);
    RegCloseKey(key);
    if (!preferences->beta_updates)
        preferences->automatic_beta_updates = 0;
    return 1;
}

int zsharp_update_preferences_save(
    const ZSharpUpdatePreferences *preferences,
    char *error, size_t error_size) {
    HKEY key = NULL;
    DWORD automatic;
    DWORD beta;
    DWORD automatic_beta;
    LONG result;
    if (preferences == NULL) return 0;
    automatic = preferences->automatic_updates != 0;
    beta = preferences->beta_updates != 0;
    automatic_beta = beta && preferences->automatic_beta_updates != 0;
    result = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\ZombieOS\\ZSharp\\Updates", 0, NULL, 0,
        KEY_SET_VALUE, NULL, &key, NULL);
    if (result == ERROR_SUCCESS)
        result = RegSetValueExW(key, L"AutomaticUpdates", 0, REG_DWORD,
                               (const BYTE *)&automatic, sizeof(automatic));
    if (result == ERROR_SUCCESS)
        result = RegSetValueExW(key, L"BetaUpdates", 0, REG_DWORD,
                               (const BYTE *)&beta, sizeof(beta));
    if (result == ERROR_SUCCESS)
        result = RegSetValueExW(key, L"AutomaticBetaUpdates", 0, REG_DWORD,
                               (const BYTE *)&automatic_beta,
                               sizeof(automatic_beta));
    if (key != NULL) RegCloseKey(key);
    if (result == ERROR_SUCCESS) return 1;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "could not save update preferences");
    return 0;
}
#else
static char *preferences_path(void) {
    const char *base = getenv("XDG_CONFIG_HOME");
    char *directory;
    char *path;
    if (base != NULL && base[0] != '\0')
        directory = join_path(base, "zsharp");
    else {
        base = getenv("HOME");
        if (base == NULL || base[0] == '\0') return NULL;
        directory = join_path(base, ".config/zsharp");
    }
    if (directory == NULL) return NULL;
    mkdir(directory, 0700);
    path = join_path(directory, "updates.conf");
    free(directory);
    return path;
}

int zsharp_update_preferences_load(ZSharpUpdatePreferences *preferences,
                                   char *error, size_t error_size) {
    char *path;
    FILE *file;
    char line[128];
    (void)error;
    (void)error_size;
    zsharp_update_preferences_default(preferences);
    if (preferences == NULL) return 0;
    path = preferences_path();
    file = path == NULL ? NULL : fopen(path, "rb");
    free(path);
    if (file == NULL) return 1;
    while (fgets(line, sizeof(line), file) != NULL) {
        int value;
        if (sscanf(line, "automatic_updates=%d", &value) == 1)
            preferences->automatic_updates = value != 0;
        else if (sscanf(line, "beta_updates=%d", &value) == 1)
            preferences->beta_updates = value != 0;
        else if (sscanf(line, "automatic_beta_updates=%d", &value) == 1)
            preferences->automatic_beta_updates = value != 0;
    }
    fclose(file);
    if (!preferences->beta_updates)
        preferences->automatic_beta_updates = 0;
    return 1;
}

int zsharp_update_preferences_save(
    const ZSharpUpdatePreferences *preferences,
    char *error, size_t error_size) {
    char *path = preferences_path();
    FILE *file = path == NULL ? NULL : fopen(path, "wb");
    free(path);
    if (file == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not save update preferences");
        return 0;
    }
    fprintf(file, "automatic_updates=%d\nbeta_updates=%d\n"
                  "automatic_beta_updates=%d\n",
            preferences->automatic_updates != 0,
            preferences->beta_updates != 0,
            preferences->beta_updates &&
                preferences->automatic_beta_updates != 0);
    if (fclose(file) == 0) return 1;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "could not save update preferences");
    return 0;
}
#endif

#ifdef _WIN32

static int version_parts(const char *version, unsigned long parts[4]) {
    const char *cursor = version;
    int index;
    for (index = 0; index < 4; index++) {
        char *end = NULL;
        unsigned long value;
        if (!isdigit((unsigned char)*cursor)) return 0;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno == ERANGE || end == cursor) return 0;
        parts[index] = value;
        if (index == 3) return *end == '\0';
        if (*end != '.') return 0;
        cursor = end + 1;
    }
    return 0;
}

static int compare_versions(const char *left, const char *right) {
    unsigned long left_parts[4];
    unsigned long right_parts[4];
    int index;
    if (!version_parts(left, left_parts) ||
        !version_parts(right, right_parts)) return 0;
    for (index = 0; index < 4; index++) {
        if (left_parts[index] < right_parts[index]) return -1;
        if (left_parts[index] > right_parts[index]) return 1;
    }
    return 0;
}

#define ZSHARP_AGENT_CLASS L"ZombieOS.ZSharp.UpdateAgent.Window"
#define ZSHARP_AGENT_MUTEX L"Local\\ZombieOS.ZSharp.UpdateAgent"
#define ZSHARP_AGENT_RUN_VALUE L"ZombieOS ZSharp Update Agent"
#define ZSHARP_AGENT_TRAY_MESSAGE (WM_APP + 1)
#define ZSHARP_AGENT_UPDATE_MESSAGE (WM_APP + 2)
#define ZSHARP_AGENT_CURRENT_MESSAGE (WM_APP + 3)
#define ZSHARP_AGENT_ERROR_MESSAGE (WM_APP + 4)
#define ZSHARP_AGENT_HOURLY_TIMER 1u
#define ZSHARP_AGENT_INSTALL_TIMER 2u
#define ZSHARP_AGENT_CHECK_COMMAND 1001u
#define ZSHARP_AGENT_EXIT_COMMAND 1002u
#define ZSHARP_AGENT_INTERVAL_MS (60u * 60u * 1000u)

typedef struct AgentCheck {
    HWND window;
    int manual;
    int beta;
} AgentCheck;

static NOTIFYICONDATAW agent_icon;
static volatile LONG agent_checking;
static int agent_update_pending;
static int agent_pending_beta;
static UINT agent_taskbar_created;

static WCHAR *wide_text(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *result = count == 0 ? NULL
        : (WCHAR *)malloc((size_t)count * sizeof(*result));
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, result, count) == 0) {
        free(result);
        return NULL;
    }
    return result;
}

static int parse_latest_version(const char *manifest, char *version,
                                size_t capacity) {
    const char *cursor = strstr(manifest, "\"latestVersion\"");
    const char *end;
    size_t length;
    if (cursor == NULL) return 0;
    cursor = strchr(cursor + 15, ':');
    if (cursor == NULL) return 0;
    cursor++;
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor++ != '"') return 0;
    end = strchr(cursor, '"');
    if (end == NULL) return 0;
    length = (size_t)(end - cursor);
    if (length == 0 || length >= capacity) return 0;
    memcpy(version, cursor, length);
    version[length] = '\0';
    return version_parts(version, (unsigned long[4]){0, 0, 0, 0});
}

static int fetch_latest_version(char *version, size_t capacity, int beta) {
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    WCHAR path[256];
    char installed[64];
    char *manifest = NULL;
    size_t length = 0;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    int ok = 0;
    current_version(installed, sizeof(installed));
    if (swprintf(path, sizeof(path) / sizeof(path[0]),
                 beta ? L"/beta.js?v=%hs-windows"
                      : L"/update.js?v=%hs-windows", installed) < 0) return 0;
    session = WinHttpOpen(L"ZSharp Update Agent/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) goto done;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    connection = WinHttpConnect(session, L"www.zsharp.zombieos.com",
                                INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (request == NULL ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) goto done;
    manifest = (char *)malloc(1);
    if (manifest == NULL) goto done;
    manifest[0] = '\0';
    for (;;) {
        DWORD available = 0;
        DWORD read = 0;
        char *grown;
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (available == 0) break;
        if (length + (size_t)available > ZSHARP_UPDATE_MANIFEST_LIMIT)
            goto done;
        grown = (char *)realloc(manifest, length + (size_t)available + 1);
        if (grown == NULL) goto done;
        manifest = grown;
        if (!WinHttpReadData(request, manifest + length, available, &read) ||
            read == 0) goto done;
        length += (size_t)read;
        manifest[length] = '\0';
    }
    ok = parse_latest_version(manifest, version, capacity);
done:
    free(manifest);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    return ok;
}

static void agent_notify(const WCHAR *title, const WCHAR *message,
                         DWORD flags) {
    agent_icon.uFlags = NIF_INFO;
    lstrcpynW(agent_icon.szInfoTitle, title,
              (int)(sizeof(agent_icon.szInfoTitle) /
                    sizeof(agent_icon.szInfoTitle[0])));
    lstrcpynW(agent_icon.szInfo, message,
              (int)(sizeof(agent_icon.szInfo) /
                    sizeof(agent_icon.szInfo[0])));
    agent_icon.dwInfoFlags = flags;
    agent_icon.uTimeout = 10000;
    Shell_NotifyIconW(NIM_MODIFY, &agent_icon);
}

static DWORD WINAPI agent_check_thread(LPVOID parameter) {
    AgentCheck *check = (AgentCheck *)parameter;
    char latest[64];
    char installed[64];
    int fetched = fetch_latest_version(latest, sizeof(latest), check->beta);
    current_version(installed, sizeof(installed));
    if (fetched && compare_versions(latest, installed) > 0) {
        char *message = _strdup(latest);
        if (message != NULL)
            PostMessageW(check->window, ZSHARP_AGENT_UPDATE_MESSAGE,
                         (WPARAM)check->beta,
                         (LPARAM)message);
    } else if (check->manual) {
        PostMessageW(check->window,
                     fetched ? ZSHARP_AGENT_CURRENT_MESSAGE
                             : ZSHARP_AGENT_ERROR_MESSAGE,
                     0, 0);
    }
    InterlockedExchange(&agent_checking, 0);
    free(check);
    return 0;
}

static void agent_begin_check(HWND window, int manual) {
    AgentCheck *check;
    HANDLE thread;
    ZSharpUpdatePreferences preferences;
    zsharp_update_preferences_load(&preferences, NULL, 0);
    if (!manual && !preferences.automatic_updates) return;
    if (InterlockedCompareExchange(&agent_checking, 1, 0) != 0) return;
    check = (AgentCheck *)malloc(sizeof(*check));
    if (check == NULL) {
        InterlockedExchange(&agent_checking, 0);
        return;
    }
    check->window = window;
    check->manual = manual;
    check->beta = manual ? preferences.beta_updates
                         : preferences.automatic_beta_updates;
    thread = CreateThread(NULL, 0, agent_check_thread, check, 0, NULL);
    if (thread == NULL) {
        free(check);
        InterlockedExchange(&agent_checking, 0);
        return;
    }
    CloseHandle(thread);
}

static int launch_installer_check(int beta) {
    char version[64];
    char process_id[32];
    char *runtime = executable_path();
    char *directory = runtime == NULL ? NULL : parent_directory(runtime);
    char *installer = directory == NULL ? NULL
        : join_path(directory, "zsharp-installer.exe");
    WCHAR *wide_installer = installer == NULL ? NULL : wide_text(installer);
    WCHAR command[65536];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int started = 0;
    free(directory);
    free(runtime);
    if (installer == NULL || wide_installer == NULL ||
        GetFileAttributesA(installer) == INVALID_FILE_ATTRIBUTES) goto done;
    current_version(version, sizeof(version));
    snprintf(process_id, sizeof(process_id), "%lu",
             (unsigned long)GetCurrentProcessId());
    if (beta)
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" --check --quiet --current-version %hs "
                 L"--notify-result --wait-pid %hs --manifest "
                 L"\"https://www.zsharp.zombieos.com/beta.js?v=%hs-windows\"",
                 wide_installer, version, process_id, version);
    else
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" --check --quiet --current-version %hs "
                 L"--notify-result --wait-pid %hs", wide_installer, version,
                 process_id);
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (CreateProcessW(wide_installer, command, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL,
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        started = 1;
    }
done:
    free(wide_installer);
    free(installer);
    return started;
}

static void agent_show_menu(HWND window) {
    HMENU menu = CreatePopupMenu();
    POINT cursor;
    if (menu == NULL) return;
    AppendMenuW(menu, MF_STRING, ZSHARP_AGENT_CHECK_COMMAND,
                L"Check for updates");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ZSHARP_AGENT_EXIT_COMMAND, L"Exit");
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   cursor.x, cursor.y, 0, window, NULL);
    DestroyMenu(menu);
}

static LRESULT CALLBACK agent_window_proc(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    if (agent_taskbar_created != 0 && message == agent_taskbar_created) {
        agent_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &agent_icon);
        return 0;
    }
    switch (message) {
        case ZSHARP_AGENT_TRAY_MESSAGE:
            if ((UINT)lparam == WM_RBUTTONUP ||
                (UINT)lparam == WM_CONTEXTMENU)
                agent_show_menu(window);
            else if ((UINT)lparam == WM_LBUTTONDBLCLK)
                agent_begin_check(window, 1);
            return 0;
        case ZSHARP_AGENT_UPDATE_MESSAGE: {
            char *version = (char *)lparam;
            WCHAR notice[256];
            swprintf(notice, sizeof(notice) / sizeof(notice[0]),
                     L"Z# %hs is available. The verified update is now "
                     L"being downloaded and installed.", version);
            free(version);
            agent_notify(L"Z# update is installing", notice, NIIF_INFO);
            if (!agent_update_pending) {
                agent_update_pending = 1;
                agent_pending_beta = wparam != 0;
                SetTimer(window, ZSHARP_AGENT_INSTALL_TIMER, 2500, NULL);
            }
            return 0;
        }
        case ZSHARP_AGENT_CURRENT_MESSAGE:
            agent_notify(L"Z#", L"Z# is up to date.", NIIF_INFO);
            return 0;
        case ZSHARP_AGENT_ERROR_MESSAGE:
            agent_notify(L"Z# could not check for updates",
                         L"The tray agent will try again automatically.",
                         NIIF_WARNING);
            return 0;
        case WM_TIMER:
            if (wparam == ZSHARP_AGENT_HOURLY_TIMER) {
                agent_begin_check(window, 0);
            } else if (wparam == ZSHARP_AGENT_INSTALL_TIMER) {
                KillTimer(window, ZSHARP_AGENT_INSTALL_TIMER);
                if (!launch_installer_check(agent_pending_beta)) {
                    agent_update_pending = 0;
                    agent_notify(L"Z# update could not start",
                                 L"The tray agent will try again automatically.",
                                 NIIF_WARNING);
                } else {
                    DestroyWindow(window);
                }
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == ZSHARP_AGENT_CHECK_COMMAND)
                agent_begin_check(window, 1);
            else if (LOWORD(wparam) == ZSHARP_AGENT_EXIT_COMMAND)
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &agent_icon);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

int zsharp_update_agent_register(char *error, size_t error_size) {
    WCHAR executable[32768];
    WCHAR command[65536];
    HKEY key = NULL;
    LONG result;
    DWORD length = GetModuleFileNameW(
        NULL, executable,
        (DWORD)(sizeof(executable) / sizeof(executable[0])));
    if (length == 0 || length >=
            (DWORD)(sizeof(executable) / sizeof(executable[0]))) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not locate zsharp.exe");
        return 0;
    }
    swprintf(command, sizeof(command) / sizeof(command[0]),
             L"\"%ls\" update-agent", executable);
    result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0,
        KEY_SET_VALUE, NULL, &key, NULL);
    if (result == ERROR_SUCCESS)
        result = RegSetValueExW(
            key, ZSHARP_AGENT_RUN_VALUE, 0, REG_SZ, (const BYTE *)command,
            ((DWORD)wcslen(command) + 1) * sizeof(WCHAR));
    if (key != NULL) RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                     "could not register the Z# update tray for startup");
        return 0;
    }
    return 1;
}

int zsharp_update_agent_start(void) {
    WCHAR executable[32768];
    WCHAR command[65536];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD length = GetModuleFileNameW(
        NULL, executable,
        (DWORD)(sizeof(executable) / sizeof(executable[0])));
    if (length == 0 || length >=
            (DWORD)(sizeof(executable) / sizeof(executable[0]))) return 0;
    swprintf(command, sizeof(command) / sizeof(command[0]),
             L"\"%ls\" update-agent", executable);
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL,
                        &startup, &process)) return 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

int zsharp_update_agent_run(void) {
    HANDLE mutex = CreateMutexW(NULL, TRUE, ZSHARP_AGENT_MUTEX);
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW window_class;
    HWND window;
    MSG message;
    if (mutex == NULL) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = agent_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = ZSHARP_AGENT_CLASS;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 1;
    }
    window = CreateWindowExW(0, ZSHARP_AGENT_CLASS,
                             L"Z# Update Agent", WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (window == NULL) {
        CloseHandle(mutex);
        return 1;
    }
    memset(&agent_icon, 0, sizeof(agent_icon));
    agent_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    agent_icon.cbSize = sizeof(agent_icon);
    agent_icon.hWnd = window;
    agent_icon.uID = 1;
    agent_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    agent_icon.uCallbackMessage = ZSHARP_AGENT_TRAY_MESSAGE;
    agent_icon.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    lstrcpynW(agent_icon.szTip, L"Z# ZVM update agent",
              (int)(sizeof(agent_icon.szTip) /
                    sizeof(agent_icon.szTip[0])));
    if (!Shell_NotifyIconW(NIM_ADD, &agent_icon)) {
        DestroyWindow(window);
        CloseHandle(mutex);
        return 1;
    }
    SetTimer(window, ZSHARP_AGENT_HOURLY_TIMER,
             ZSHARP_AGENT_INTERVAL_MS, NULL);
    PostMessageW(window, WM_TIMER, ZSHARP_AGENT_HOURLY_TIMER, 0);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(mutex);
    return 0;
}

#else

int zsharp_update_agent_register(char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    return 1;
}

int zsharp_update_agent_start(void) {
    return 1;
}

int zsharp_update_agent_run(void) {
    return 1;
}

#endif

void zsharp_update_check_start(void) {
#ifdef _WIN32
    return;
#else
    ZSharpUpdatePreferences preferences;
    char version[64];
    char process_id[32];
    char *runtime;
    char *directory;
    char *installer;
    zsharp_update_preferences_load(&preferences, NULL, 0);
    if (getenv("ZSHARP_SKIP_UPDATE_CHECK") != NULL ||
        !preferences.automatic_updates) return;
    runtime = executable_path();
    directory = runtime == NULL ? NULL : parent_directory(runtime);
    installer = directory == NULL ? NULL
        : join_path(directory, "zsharp-installer");
    free(directory);
    free(runtime);
    if (installer == NULL) return;
    if (access(installer, X_OK) != 0) {
        free(installer);
        return;
    }
    snprintf(process_id, sizeof(process_id), "%lu", (unsigned long)getpid());
    current_version(version, sizeof(version));
    {
        pid_t child = fork();
        if (child == 0) {
            pid_t grandchild = fork();
            if (grandchild < 0) _exit(127);
            if (grandchild > 0) _exit(0);
            setsid();
            {
                int null_file = open("/dev/null", O_RDWR);
                if (null_file >= 0) {
                    dup2(null_file, STDIN_FILENO);
                    dup2(null_file, STDOUT_FILENO);
                    dup2(null_file, STDERR_FILENO);
                    if (null_file > STDERR_FILENO) close(null_file);
                }
            }
            if (preferences.automatic_beta_updates)
                execl(installer, installer, "--check", "--quiet",
                      "--current-version", version, "--wait-pid", process_id,
                      "--manifest", "https://www.zsharp.zombieos.com/beta.js",
                      (char *)NULL);
            else
                execl(installer, installer, "--check", "--quiet",
                      "--current-version", version, "--wait-pid", process_id,
                      (char *)NULL);
            _exit(127);
        }
        if (child > 0) waitpid(child, NULL, 0);
    }
    free(installer);
#endif
}

int zsharp_update_now(char *error, size_t error_size) {
    char version[64];
    char process_id[32];
    ZSharpUpdatePreferences preferences;
    char *runtime = executable_path();
    char *directory = runtime == NULL ? NULL : parent_directory(runtime);
#ifdef _WIN32
    const char *override = getenv("ZSHARP_UPDATE_INSTALLER");
    char *installer = override != NULL && override[0] != '\0'
        ? _strdup(override)
        : (directory == NULL ? NULL
                             : join_path(directory, "zsharp-installer.exe"));
    WCHAR *wide_installer = installer == NULL ? NULL : wide_text(installer);
    WCHAR command[65536];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int started = 0;
    zsharp_update_preferences_load(&preferences, NULL, 0);
    current_version(version, sizeof(version));
    snprintf(process_id, sizeof(process_id), "%lu",
             (unsigned long)GetCurrentProcessId());
    if (installer == NULL || wide_installer == NULL ||
        GetFileAttributesA(installer) == INVALID_FILE_ATTRIBUTES) {
        snprintf(error, error_size,
                 "zsharp-installer.exe was not found beside the ZVM");
        goto windows_done;
    }
    if (preferences.beta_updates)
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" --check --quiet --notify-result "
                 L"--current-version %hs --wait-pid %hs --manifest "
                 L"\"https://www.zsharp.zombieos.com/beta.js?v=%hs-windows\"",
                 wide_installer, version, process_id, version);
    else
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" --check --quiet --notify-result "
                 L"--current-version %hs --wait-pid %hs",
                 wide_installer, version, process_id);
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(wide_installer, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS,
                        NULL, NULL, &startup, &process)) {
        DWORD launch_error = GetLastError();
        snprintf(error, error_size,
                 "could not start the Z# updater (Windows error %lu)",
                 (unsigned long)launch_error);
        goto windows_done;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    started = 1;
windows_done:
    free(wide_installer);
    free(installer);
    free(directory);
    free(runtime);
    return started;
#else
    const char *override = getenv("ZSHARP_UPDATE_INSTALLER");
    char *installer = override != NULL && override[0] != '\0'
        ? strdup(override)
        : (directory == NULL ? NULL : join_path(directory,
                                                 "zsharp-installer"));
    pid_t child;
    zsharp_update_preferences_load(&preferences, NULL, 0);
    current_version(version, sizeof(version));
    snprintf(process_id, sizeof(process_id), "%lu", (unsigned long)getpid());
    if (installer == NULL || access(installer, X_OK) != 0) {
        snprintf(error, error_size,
                 "zsharp-installer was not found beside the ZVM");
        free(installer);
        free(directory);
        free(runtime);
        return 0;
    }
    child = fork();
    if (child < 0) {
        snprintf(error, error_size, "could not start the Z# updater");
        free(installer);
        free(directory);
        free(runtime);
        return 0;
    }
    if (child == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);
        setsid();
        {
            int null_file = open("/dev/null", O_RDWR);
            if (null_file >= 0) {
                dup2(null_file, STDIN_FILENO);
                dup2(null_file, STDOUT_FILENO);
                dup2(null_file, STDERR_FILENO);
                if (null_file > STDERR_FILENO) close(null_file);
            }
        }
        if (preferences.beta_updates)
            execl(installer, installer, "--check", "--quiet",
                  "--notify-result", "--current-version", version,
                  "--wait-pid", process_id, "--manifest",
                  "https://www.zsharp.zombieos.com/beta.js",
                  (char *)NULL);
        else
            execl(installer, installer, "--check", "--quiet",
                  "--notify-result", "--current-version", version,
                  "--wait-pid", process_id, (char *)NULL);
        _exit(127);
    }
    waitpid(child, NULL, 0);
    free(installer);
    free(directory);
    free(runtime);
    return 1;
#endif
}
