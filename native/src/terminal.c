#define _CRT_SECURE_NO_WARNINGS

#include "terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static FILE *terminal_output;
static char terminal_active_path[1200];
static volatile int terminal_detach;

static int make_directory(const char *path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static int terminal_paths(const char *project_id, char *directory,
                          size_t directory_size, char *active,
                          size_t active_size, char *log, size_t log_size,
                          char *error, size_t error_size) {
    const char *base;
    size_t index;
    if (project_id == NULL || project_id[0] == '\0') goto invalid;
    for (index = 0; project_id[index] != '\0'; index++)
        if (!((project_id[index] >= 'a' && project_id[index] <= 'z') ||
              (project_id[index] >= 'A' && project_id[index] <= 'Z') ||
              (project_id[index] >= '0' && project_id[index] <= '9') ||
              project_id[index] == '_' || project_id[index] == '-')) goto invalid;
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base == NULL) goto unavailable;
    snprintf(directory, directory_size, "%s\\ZombieOS\\ZSharp\\terminal", base);
    snprintf(active, active_size, "%s\\%s.active", directory, project_id);
    snprintf(log, log_size, "%s\\%s.log", directory, project_id);
#else
    base = getenv("HOME");
    if (base == NULL) goto unavailable;
#ifdef __APPLE__
    snprintf(directory, directory_size,
             "%s/Library/Application Support/ZombieOS/ZSharp/terminal", base);
#else
    snprintf(directory, directory_size, "%s/.local/share/zsharp/terminal", base);
#endif
    snprintf(active, active_size, "%s/%s.active", directory, project_id);
    snprintf(log, log_size, "%s/%s.log", directory, project_id);
#endif
    return 1;
invalid:
    snprintf(error, error_size, "invalid project ID for terminal connection");
    return 0;
unavailable:
    snprintf(error, error_size, "the user data directory is unavailable");
    return 0;
}

static int process_is_running(unsigned long process_id) {
#ifdef _WIN32
    DWORD code = 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 (DWORD)process_id);
    if (process == NULL) return 0;
    if (!GetExitCodeProcess(process, &code)) code = 0;
    CloseHandle(process);
    return code == STILL_ACTIVE;
#else
    return process_id != 0 &&
           (kill((pid_t)process_id, 0) == 0 || errno == EPERM);
#endif
}

int zsharp_terminal_start(const char *project_id, char *error,
                          size_t error_size) {
    char directory[1024], log[1200];
    FILE *active;
    if (!terminal_paths(project_id, directory, sizeof(directory),
                        terminal_active_path, sizeof(terminal_active_path),
                        log, sizeof(log), error, error_size)) return 0;
    if (!make_directory(directory)) {
        snprintf(error, error_size, "could not create terminal session directory");
        return 0;
    }
    terminal_output = fopen(log, "wb");
    active = fopen(terminal_active_path, "wb");
    if (terminal_output == NULL || active == NULL) {
        if (terminal_output != NULL) fclose(terminal_output);
        terminal_output = NULL;
        snprintf(error, error_size, "could not create terminal session");
        return 0;
    }
#ifdef _WIN32
    fprintf(active, "%lu\n", (unsigned long)GetCurrentProcessId());
#else
    fprintf(active, "%lu\n", (unsigned long)getpid());
#endif
    fclose(active);
    return 1;
}

void zsharp_terminal_write(const char *text) {
    if (terminal_output == NULL) return;
    fprintf(terminal_output, "%s\n", text == NULL ? "" : text);
    fflush(terminal_output);
}

void zsharp_terminal_stop(void) {
    if (terminal_output != NULL) fclose(terminal_output);
    terminal_output = NULL;
    if (terminal_active_path[0] != '\0') remove(terminal_active_path);
    terminal_active_path[0] = '\0';
}

#ifdef _WIN32
static unsigned __stdcall terminal_input_thread(void *unused) {
#else
static void *terminal_input_thread(void *unused) {
#endif
    char line[256];
    (void)unused;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "zsharp terminal exit") == 0) {
            terminal_detach = 1;
            break;
        }
        puts("This terminal connection is output-only. Use 'zsharp terminal exit' to detach.");
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int zsharp_terminal_attach(const char *project_id, char *error,
                           size_t error_size) {
    char directory[1024], active_path[1200], log_path[1200];
    FILE *active, *log;
    unsigned long process_id = 0;
    char line[2048];
    if (!terminal_paths(project_id, directory, sizeof(directory), active_path,
                        sizeof(active_path), log_path, sizeof(log_path), error,
                        error_size)) return 0;
    active = fopen(active_path, "rb");
    if (active == NULL || fscanf(active, "%lu", &process_id) != 1) {
        if (active != NULL) fclose(active);
        snprintf(error, error_size, "that Z# app or game is not currently running");
        return 0;
    }
    fclose(active);
    if (!process_is_running(process_id)) {
        remove(active_path);
        snprintf(error, error_size, "that Z# app or game is not currently running");
        return 0;
    }
    log = fopen(log_path, "rb");
    if (log == NULL) {
        snprintf(error, error_size, "could not open the terminal output stream");
        return 0;
    }
    terminal_detach = 0;
#ifdef _WIN32
    {
        uintptr_t thread = _beginthreadex(NULL, 0, terminal_input_thread,
                                          NULL, 0, NULL);
        if (thread != 0) CloseHandle((HANDLE)thread);
    }
#else
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, terminal_input_thread, NULL) == 0)
            pthread_detach(thread);
    }
#endif
    printf("Connected to %s. Type 'zsharp terminal exit' to detach.\n",
           project_id);
    while (!terminal_detach && process_is_running(process_id)) {
        int read_any = 0;
        while (fgets(line, sizeof(line), log) != NULL) {
            fputs(line, stdout);
            fflush(stdout);
            read_any = 1;
        }
        clearerr(log);
#ifdef _WIN32
        Sleep(read_any ? 25 : 100);
#else
        usleep(read_any ? 25000 : 100000);
#endif
    }
    while (fgets(line, sizeof(line), log) != NULL) fputs(line, stdout);
    fclose(log);
    puts(terminal_detach ? "Terminal detached." : "The Z# app or game has closed.");
    return 1;
}
