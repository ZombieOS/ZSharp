#define _CRT_SECURE_NO_WARNINGS

#include "publisher.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifdef _WIN32
static char *quoted_argument(const char *text) {
    size_t length = strlen(text);
    char *result = (char *)malloc(length + 3);
    if (result == NULL) return NULL;
    result[0] = '"';
    memcpy(result + 1, text, length);
    result[length + 1] = '"';
    result[length + 2] = '\0';
    return result;
}
#endif

static int regular_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static char *absolute_path(const char *path) {
#ifdef _WIN32
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

static char *parent_path(const char *path) {
    char *result = copy_text(path);
    char *last = NULL;
    char *cursor;
    if (result == NULL) return NULL;
    for (cursor = result; *cursor != '\0'; cursor++)
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    if (last == NULL) {
        free(result);
        return NULL;
    }
#ifdef _WIN32
    if (last == result + 2 && result[1] == ':') last[1] = '\0';
    else
#endif
    if (last == result) last[1] = '\0';
    else *last = '\0';
    return result;
}

static char *find_repository(const char *requested) {
    char *current;
    if (requested != NULL) {
        current = absolute_path(requested);
    } else {
#ifdef _WIN32
        current = _getcwd(NULL, 0);
#else
        current = getcwd(NULL, 0);
#endif
    }
    while (current != NULL) {
        char *script = join_path(current, "scripts/publish.ps1");
        char *cmake = join_path(current, "CMakeLists.txt");
        int found = script != NULL && cmake != NULL &&
                    regular_file(script) && regular_file(cmake);
        free(script);
        free(cmake);
        if (found) return current;
        {
            char *parent = parent_path(current);
            if (parent == NULL || strcmp(parent, current) == 0) {
                free(parent);
                free(current);
                return NULL;
            }
            free(current);
            current = parent;
        }
    }
    return NULL;
}

int zsharp_publisher_run(const char *repository, char *error,
                         size_t error_size) {
    char *root = find_repository(repository);
    char *script;
    if (root == NULL) {
        snprintf(error, error_size,
                 "could not find a Z# language repository containing scripts/publish.ps1");
        return 0;
    }
    script = join_path(root, "scripts/publish.ps1");
    if (script == NULL) {
        free(root);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
#ifdef _WIN32
    {
        char *quoted_script = quoted_argument(script);
        char *quoted_root = quoted_argument(root);
        const char *arguments[] = {
            "pwsh.exe", "-NoProfile", "-File", quoted_script,
            "-ProjectRoot", quoted_root, NULL
        };
        intptr_t result;
        if (quoted_script == NULL || quoted_root == NULL) {
            free(quoted_script);
            free(quoted_root);
            free(script);
            free(root);
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        result = _spawnvp(_P_WAIT, arguments[0], arguments);
        if (result == -1) {
            const char *fallback[] = {
                "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", quoted_script, "-ProjectRoot", quoted_root, NULL
            };
            result = _spawnvp(_P_WAIT, fallback[0], fallback);
        }
        free(quoted_script);
        free(quoted_root);
        free(script);
        free(root);
        if (result == 0) return 1;
        snprintf(error, error_size,
                 result == -1 ? "PowerShell could not be started"
                              : "the local publishing build failed");
        return 0;
    }
#else
    {
        pid_t child = fork();
        int status;
        if (child == 0) {
            execlp("pwsh", "pwsh", "-NoProfile", "-File", script,
                   "-ProjectRoot", root, (char *)NULL);
            _exit(127);
        }
        free(script);
        free(root);
        if (child < 0 || waitpid(child, &status, 0) < 0) {
            snprintf(error, error_size, "PowerShell could not be started");
            return 0;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
        snprintf(error, error_size, "the local publishing build failed");
        return 0;
    }
#endif
}
