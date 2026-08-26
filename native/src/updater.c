#define _CRT_SECURE_NO_WARNINGS

#include "updater.h"

#include "zsharp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

void zsharp_update_check_start(void) {
    char version[64];
    char process_id[32];
    char *runtime;
    char *directory;
    char *installer;
    if (getenv("ZSHARP_SKIP_UPDATE_CHECK") != NULL) return;
    runtime = executable_path();
    directory = runtime == NULL ? NULL : parent_directory(runtime);
#ifdef _WIN32
    installer = directory == NULL ? NULL
        : join_path(directory, "zsharp-installer.exe");
#else
    installer = directory == NULL ? NULL
        : join_path(directory, "zsharp-installer");
#endif
    free(directory);
    free(runtime);
    if (installer == NULL) return;
#ifdef _WIN32
    if (GetFileAttributesA(installer) == INVALID_FILE_ATTRIBUTES) {
        free(installer);
        return;
    }
    snprintf(process_id, sizeof(process_id), "%lu",
             (unsigned long)GetCurrentProcessId());
#else
    if (access(installer, X_OK) != 0) {
        free(installer);
        return;
    }
    snprintf(process_id, sizeof(process_id), "%lu", (unsigned long)getpid());
#endif
    snprintf(version, sizeof(version), "%d.%d.%d.%d",
             ZSHARP_VERSION_MAJOR, ZSHARP_VERSION_MINOR,
             ZSHARP_VERSION_PATCH, ZSHARP_VERSION_REVISION);
#ifdef _WIN32
    {
        int wide_count = MultiByteToWideChar(CP_UTF8, 0, installer, -1,
                                             NULL, 0);
        WCHAR *wide_installer = wide_count == 0 ? NULL
            : (WCHAR *)malloc((size_t)wide_count * sizeof(*wide_installer));
        WCHAR command[65536];
        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        if (wide_installer != NULL && MultiByteToWideChar(
                CP_UTF8, 0, installer, -1, wide_installer, wide_count) != 0) {
            swprintf(command, sizeof(command) / sizeof(command[0]),
                     L"\"%ls\" --check --quiet --current-version %hs "
                     L"--wait-pid %hs", wide_installer, version, process_id);
            memset(&startup, 0, sizeof(startup));
            memset(&process, 0, sizeof(process));
            startup.cb = sizeof(startup);
            if (CreateProcessW(wide_installer, command, NULL, NULL, FALSE,
                               CREATE_NO_WINDOW | DETACHED_PROCESS,
                               NULL, NULL, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
        free(wide_installer);
    }
#else
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
            execl(installer, installer, "--check", "--quiet",
                  "--current-version", version, "--wait-pid", process_id,
                  (char *)NULL);
            _exit(127);
        }
        if (child > 0) waitpid(child, NULL, 0);
    }
#endif
    free(installer);
}
