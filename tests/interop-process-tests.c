#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "interop_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#define current_directory(path, size) _getcwd(path, (int)(size))
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0700)
#define remove_directory(path) rmdir(path)
#define current_directory(path, size) getcwd(path, size)
#endif

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
    } \
} while (0)

static void delay(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec duration = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000};
    nanosleep(&duration, NULL);
#endif
}

static void write_marker(const char *path, const char *text, const char *mode) {
    FILE *file = fopen(path, mode);
    if (!file) exit(2);
    if (fwrite(text, 1, strlen(text), file) != strlen(text) || fclose(file)) exit(2);
}

static int worker(int argc, char **argv) {
    char line[4096];
    const char *mode = argv[2], *marker = argv[3];
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcmp(mode, "stall")) {
            write_marker(marker, "X", "ab");
            delay(10000);
        } else if (!strcmp(mode, "bad")) {
            write_marker(marker, "X", "ab");
            fwrite("{\"bad\":\"\0\"}\n", 1, 12, stdout);
        } else if (!strcmp(mode, "embedded-cr")) {
            fwrite("{\"bad\":\r1}\n", 1, 11, stdout);
        } else if (!strcmp(mode, "large")) {
            char block[4096];
            size_t remaining = 1024u * 1024u + 1;
            memset(block, 'x', sizeof(block));
            while (remaining) {
                size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
                fwrite(block, 1, count, stdout);
                remaining -= count;
            }
            fputc('\n', stdout);
        } else if (!strcmp(mode, "crlf")) {
            fputs("{\"crlf\":true}\r\n", stdout);
        } else if (!strcmp(mode, "cwd")) {
            char path[4096];
            int same;
            if (!current_directory(path, sizeof(path))) return 3;
#ifdef _WIN32
            {
                char expected[4096];
                size_t index;
                if (!_fullpath(expected, marker, sizeof(expected))) return 3;
                /* CMake passes forward-slash paths; the CRT returns native
                 * separators. Compare destinations, not separator spelling. */
                for (index = 0; expected[index]; ++index)
                    if (expected[index] == '/') expected[index] = '\\';
                for (index = 0; path[index]; ++index)
                    if (path[index] == '/') path[index] = '\\';
                same = _stricmp(path, expected) == 0;
            }
#else
            same = strcmp(path, marker) == 0;
#endif
            if (argc != 5 || strcmp(argv[4], "quotes \" and a trailing slash\\") || !same) return 4;
            fputs("{\"cwdAndQuotedArguments\":true}\n", stdout);
        } else fputs(line, stdout);
    }
    if (!strcmp(mode, "exit")) {
        delay(100); /* Must receive a normal EOF grace period, not immediate kill. */
        write_marker(marker, "normal exit", "wb");
    }
    return 0;
}

static int cancelled(void *context) {
    return *(int *)context;
}

static ZSharpInteropProcess *start(const char *self, const char *mode,
    const char *marker, const char *directory) {
    const char *arguments[] = {self, "--worker", mode, marker,
        "quotes \" and a trailing slash\\", NULL};
    char error[1024] = {0};
    ZSharpInteropProcess *process = zsharp_interop_process_start(arguments, directory, error, sizeof(error));
    if (!process) fprintf(stderr, "Launch failed: %s\n", error);
    return process;
}

static void marker_equals(const char *path, const char *expected) {
    char text[128] = {0};
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fread(text, 1, sizeof(text) - 1, file) == strlen(expected));
    CHECK(fclose(file) == 0 && !strcmp(text, expected));
    CHECK(remove(path) == 0);
}

int main(int argc, char **argv) {
    ZSharpInteropProcess *process;
    char marker[4096], error[1024], *line;
    int stop = 1;
    const char *failure_modes[] = {"bad", "embedded-cr", "large"};
    size_t index;
    if (argc >= 4 && !strcmp(argv[1], "--worker")) return worker(argc, argv);
    CHECK(argc == 2);
    CHECK(make_directory(argv[1]) == 0); /* Never reuse somebody else's fixtures. */
    CHECK(snprintf(marker, sizeof(marker), "%s/owned-marker.txt", argv[1]) < (int)sizeof(marker));
    process = start(argv[0], "echo", marker, argv[1]);
    CHECK(process != NULL);
    CHECK(zsharp_interop_process_write(process, "{\"text\":\"hello\"}", 2000, NULL, NULL, error, sizeof(error)));
    line = zsharp_interop_process_read(process, 2000, NULL, NULL, error, sizeof(error));
    CHECK(line && !strcmp(line, "{\"text\":\"hello\"}")); free(line);
    CHECK(zsharp_interop_process_write(process, "{\"text\":\"second\"}", 2000, NULL, NULL, error, sizeof(error)));
    line = zsharp_interop_process_read(process, 2000, NULL, NULL, error, sizeof(error));
    CHECK(line && !strcmp(line, "{\"text\":\"second\"}")); free(line);
    zsharp_interop_process_destroy(process);
    process = start(argv[0], "cwd", argv[1], argv[1]);
    CHECK(process != NULL);
    CHECK(zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    line = zsharp_interop_process_read(process, 2000, NULL, NULL, error, sizeof(error));
    CHECK(line && !strcmp(line, "{\"cwdAndQuotedArguments\":true}")); free(line);
    zsharp_interop_process_destroy(process);
    process = start(argv[0], "crlf", marker, argv[1]);
    CHECK(process != NULL);
    CHECK(zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    line = zsharp_interop_process_read(process, 2000, NULL, NULL, error, sizeof(error));
    CHECK(line && !strcmp(line, "{\"crlf\":true}")); free(line);
    zsharp_interop_process_destroy(process);
    process = start(argv[0], "exit", marker, argv[1]);
    CHECK(process != NULL);
    zsharp_interop_process_destroy(process);
    marker_equals(marker, "normal exit");
    for (index = 0; index < sizeof(failure_modes) / sizeof(failure_modes[0]); ++index) {
        process = start(argv[0], failure_modes[index], marker, argv[1]);
        CHECK(process != NULL);
        CHECK(zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
        CHECK(!zsharp_interop_process_read(process, 5000, NULL, NULL, error, sizeof(error)));
        CHECK(!zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
        CHECK(strstr(error, "not replayed") || strstr(error, "no longer available"));
        zsharp_interop_process_destroy(process);
        if (!strcmp(failure_modes[index], "bad")) marker_equals(marker, "X");
    }
    process = start(argv[0], "stall", marker, argv[1]);
    CHECK(process != NULL);
    CHECK(zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    CHECK(!zsharp_interop_process_read(process, 100, NULL, NULL, error, sizeof(error)));
    CHECK(!zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    zsharp_interop_process_destroy(process);
    marker_equals(marker, "X");
    process = start(argv[0], "echo", marker, argv[1]);
    CHECK(process != NULL);
    CHECK(!zsharp_interop_process_read(process, 2000, cancelled, &stop, error, sizeof(error)));
    CHECK(!zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    zsharp_interop_process_destroy(process);
    process = start(argv[0], "echo", marker, argv[1]);
    CHECK(process != NULL);
    CHECK(!zsharp_interop_process_write(process, "{}\n{}", 2000, NULL, NULL, error, sizeof(error)));
    CHECK(!zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
    zsharp_interop_process_destroy(process);
    {
        const char *arguments[] = {argv[0], "--worker", "echo", marker, NULL};
        char nonexistent[4096];
        snprintf(nonexistent, sizeof(nonexistent), "%s/no such directory", argv[1]);
        CHECK(zsharp_interop_process_start(arguments, nonexistent, error, sizeof(error)) == NULL);
    }
#ifndef _WIN32
    {
        int input = dup(STDIN_FILENO), output = dup(STDOUT_FILENO);
        CHECK(input >= 0 && output >= 0);
        close(STDIN_FILENO); close(STDOUT_FILENO);
        process = start(argv[0], "echo", marker, argv[1]);
        dup2(input, STDIN_FILENO); dup2(output, STDOUT_FILENO);
        close(input); close(output);
        CHECK(process != NULL);
        CHECK(zsharp_interop_process_write(process, "{}", 2000, NULL, NULL, error, sizeof(error)));
        line = zsharp_interop_process_read(process, 2000, NULL, NULL, error, sizeof(error));
        CHECK(line && !strcmp(line, "{}")); free(line);
        zsharp_interop_process_destroy(process);
    }
#endif
    CHECK(remove_directory(argv[1]) == 0);
    printf("owned interop process transport: %u checks passed\n", checks);
    return 0;
}
