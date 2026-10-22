#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "interop_process.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

#define INTEROP_FRAME_LIMIT (1024u * 1024u)
struct ZSharpInteropProcess {
#ifdef _WIN32
    HANDLE process, job, input, output;
#else
    pid_t process;
    int socket;
#endif
    int failed;
    char bytes[4096];
    size_t at, count;
};
static uint64_t interop_now(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
#endif
}
static int interop_fail(ZSharpInteropProcess *process, char *error, size_t size,
    const char *message) {
    if (process) process->failed = 1;
    snprintf(error, size, "%s", message);
    return 0;
}
static void interop_kill(ZSharpInteropProcess *process) {
#ifdef _WIN32
    if (process->job) TerminateJobObject(process->job, 1);
    else if (process->process) TerminateProcess(process->process, 1);
#else
    if (process->process > 0) kill(-process->process, SIGKILL);
#endif
}
#ifdef _WIN32
static WCHAR *interop_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    WCHAR *result = length ? malloc((size_t)length * sizeof(WCHAR)) : NULL;
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, length)) {
        free(result); return NULL;
    }
    return result;
}
static WCHAR *interop_command(const char *const *argv) {
    WCHAR *command = calloc(32768, sizeof(WCHAR));
    size_t at = 0, argument;
    if (!command) return NULL;
    for (argument = 0; argv[argument]; argument++) {
        WCHAR *wide = interop_wide(argv[argument]);
        size_t i = 0;
        if (!wide) { free(command); return NULL; }
        if (argument) command[at++] = L' ';
        command[at++] = L'"';
        while (wide[i]) {
            size_t slashes = 0;
            while (wide[i] == L'\\') { slashes++; i++; }
            if (at + slashes * 2 + 4 >= 32768) { free(wide); free(command); return NULL; }
            if (wide[i] == L'"' || !wide[i]) {
                size_t count = slashes * 2 + (wide[i] == L'"');
                while (count--) command[at++] = L'\\';
            } else while (slashes--) command[at++] = L'\\';
            if (wide[i]) command[at++] = wide[i++];
        }
        free(wide);
        if (at + 3 >= 32768) { free(command); return NULL; }
        command[at++] = L'"'; command[at] = 0;
    }
    return command;
}
typedef struct InteropWrite {
    HANDLE pipe;
    const char *bytes;
    size_t length;
    int success;
} InteropWrite;
static DWORD WINAPI interop_writer(void *opaque) {
    InteropWrite *request = opaque;
    size_t at = 0;
    while (at < request->length) {
        DWORD count = 0;
        if (!WriteFile(request->pipe, request->bytes + at,
                (DWORD)(request->length - at), &count, NULL) || !count) return 1;
        at += count;
    }
    request->success = 1;
    return 0;
}
#endif

ZSharpInteropProcess *zsharp_interop_process_start(const char *const *argv,
    const char *directory, char *error, size_t size) {
    ZSharpInteropProcess *process = calloc(1, sizeof(*process));
    if (!process || !argv || !argv[0]) {
        free(process); snprintf(error, size, "invalid foreign runtime launch"); return NULL;
    }
#ifdef _WIN32
    {
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        STARTUPINFOEXW startup;
        PROCESS_INFORMATION child;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
        HANDLE child_input = NULL, child_output = NULL, child_error = NULL;
        HANDLE inherited[3];
        SIZE_T attribute_size = 0;
        WCHAR *command = interop_command(argv), *cwd = directory ? interop_wide(directory) : NULL;
        int initialized = 0, created = 0;
        memset(&startup, 0, sizeof(startup)); memset(&child, 0, sizeof(child));
        memset(&limit, 0, sizeof(limit));
        if (!command || (directory && !cwd) ||
            !CreatePipe(&child_input, &process->input, &security, 65536) ||
            !CreatePipe(&process->output, &child_output, &security, 65536) ||
            !SetHandleInformation(process->input, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(process->output, HANDLE_FLAG_INHERIT, 0)) goto launch_done;
        /* User logs go to inherited stderr, never into the protocol pipe. */
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                GetCurrentProcess(), &child_error, 0, TRUE, DUPLICATE_SAME_ACCESS))
            child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security, OPEN_EXISTING, 0, NULL);
        if (!child_error || child_error == INVALID_HANDLE_VALUE) goto launch_done;
        inherited[0] = child_input; inherited[1] = child_output; inherited[2] = child_error;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
        startup.lpAttributeList = malloc(attribute_size);
        if (!startup.lpAttributeList ||
            !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) goto launch_done;
        initialized = 1;
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, sizeof(inherited), NULL, NULL)) goto launch_done;
        process->job = CreateJobObjectW(NULL, NULL);
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!process->job || !SetInformationJobObject(process->job,
                JobObjectExtendedLimitInformation, &limit, sizeof(limit))) goto launch_done;
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input;
        startup.StartupInfo.hStdOutput = child_output;
        startup.StartupInfo.hStdError = child_error;
        if (!CreateProcessW(NULL, command, NULL, NULL, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                NULL, cwd, &startup.StartupInfo, &child)) goto launch_done;
        process->process = child.hProcess;
        if (!AssignProcessToJobObject(process->job, child.hProcess) || ResumeThread(child.hThread) == (DWORD)-1) {
            TerminateProcess(child.hProcess, 1); CloseHandle(child.hThread); goto launch_done;
        }
        CloseHandle(child.hThread); created = 1;
launch_done:
        if (!created) snprintf(error, size,
            "could not start foreign runtime '%s' (Windows error %lu); install its runtime or configure its executable",
            argv[0], (unsigned long)GetLastError());
        if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList); free(command); free(cwd);
        if (child_input) CloseHandle(child_input);
        if (child_output) CloseHandle(child_output);
        if (child_error && child_error != INVALID_HANDLE_VALUE) CloseHandle(child_error);
        if (!created) { zsharp_interop_process_destroy(process); return NULL; }
    }
#else
    {
        int sockets[2] = {-1, -1}, status, index;
        int actions_ready = 0, attributes_ready = 0;
        pid_t child = 0;
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        process->socket = -1;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) goto posix_failed;
        /* Closed inherited stdin/stdout must not make the later addclose
         * actions close a descriptor that was just installed as child stdio. */
        for (index = 0; index < 2; ++index) {
            if (sockets[index] <= STDERR_FILENO) {
                int moved = fcntl(sockets[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (moved < 0) goto posix_failed;
                close(sockets[index]); sockets[index] = moved;
            } else if (fcntl(sockets[index], F_SETFD, FD_CLOEXEC) < 0) goto posix_failed;
        }
        status = posix_spawn_file_actions_init(&actions);
        if (status) { errno = status; goto posix_failed; }
        actions_ready = 1;
        status = posix_spawnattr_init(&attributes);
        if (status) { errno = status; goto posix_failed; }
        attributes_ready = 1;
#define INTEROP_SPAWN_CHECK(action) do { status = (action); if (status) { errno = status; goto posix_failed; } } while (0)
        INTEROP_SPAWN_CHECK(posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO));
        INTEROP_SPAWN_CHECK(posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO));
        INTEROP_SPAWN_CHECK(posix_spawn_file_actions_addclose(&actions, sockets[0]));
        INTEROP_SPAWN_CHECK(posix_spawn_file_actions_addclose(&actions, sockets[1]));
#if defined(__APPLE__) || defined(__linux__)
        if (directory) INTEROP_SPAWN_CHECK(posix_spawn_file_actions_addchdir_np(&actions, directory));
#else
        if (directory) { errno = ENOTSUP; goto posix_failed; }
#endif
        INTEROP_SPAWN_CHECK(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
        INTEROP_SPAWN_CHECK(posix_spawnattr_setpgroup(&attributes, 0));
#undef INTEROP_SPAWN_CHECK
        status = posix_spawnp(&child, argv[0], &actions, &attributes,
            (char *const *)argv, environ);
        posix_spawnattr_destroy(&attributes); attributes_ready = 0;
        posix_spawn_file_actions_destroy(&actions); actions_ready = 0;
        close(sockets[1]); sockets[1] = -1;
        if (status) { errno = status; goto posix_failed; }
        process->process = child;
        process->socket = sockets[0];
        status = fcntl(process->socket, F_GETFL);
        if (status < 0 || fcntl(process->socket, F_SETFL, status | O_NONBLOCK) < 0) goto posix_failed;
#ifdef SO_NOSIGPIPE
        {
            int enabled = 1;
            if (setsockopt(process->socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) goto posix_failed;
        }
#endif
        return process;
posix_failed:
        snprintf(error, size, "could not start foreign runtime '%s': %s", argv[0], strerror(errno));
        if (attributes_ready) posix_spawnattr_destroy(&attributes);
        if (actions_ready) posix_spawn_file_actions_destroy(&actions);
        if (sockets[0] >= 0) close(sockets[0]);
        if (sockets[1] >= 0) close(sockets[1]);
        if (process->process > 0) {
            interop_kill(process);
            while (waitpid(process->process, NULL, 0) < 0 && errno == EINTR) { }
        }
        free(process); return NULL;
    }
#endif
    return process;
}

int zsharp_interop_process_write(ZSharpInteropProcess *process, const char *line,
    unsigned timeout_ms, ZSharpInteropCancelled cancelled, void *context, char *error, size_t size) {
    size_t length = strlen(line);
    uint64_t deadline = interop_now() + timeout_ms;
    char *frame;
    if (process->failed) return interop_fail(process, error, size, "foreign runtime is no longer available; requests are not replayed");
    if (length > INTEROP_FRAME_LIMIT || strchr(line, '\n') || strchr(line, '\r'))
        return interop_fail(process, error, size, "foreign request exceeds its frame limit or contains a raw newline");
    frame = malloc(length + 1);
    if (!frame) return interop_fail(process, error, size, "out of memory preparing foreign request");
    memcpy(frame, line, length); frame[length++] = '\n';
#ifdef _WIN32
    {
        InteropWrite request = {process->input, frame, length, 0};
        HANDLE writer = CreateThread(NULL, 0, interop_writer, &request, 0, NULL);
        DWORD wait = WAIT_FAILED;
        if (writer) do {
            wait = WaitForSingleObject(writer, 10);
            if (wait != WAIT_TIMEOUT) break;
        } while (interop_now() < deadline && !(cancelled && cancelled(context)));
        if (!writer || wait != WAIT_OBJECT_0 || !request.success) {
            interop_kill(process);
            if (writer) { CancelSynchronousIo(writer); WaitForSingleObject(writer, INFINITE); CloseHandle(writer); }
            free(frame);
            return interop_fail(process, error, size, "foreign request could not be delivered or was cancelled; it was not replayed");
        }
        CloseHandle(writer);
    }
#else
    {
        size_t at = 0;
        while (at < length && interop_now() < deadline && !(cancelled && cancelled(context))) {
            struct pollfd descriptor = {process->socket, POLLOUT, 0};
            ssize_t count;
            int ready = poll(&descriptor, 1, 10);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            if (!(descriptor.revents & POLLOUT)) continue;
            count = send(process->socket, frame + at, length - at,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) break;
            at += (size_t)count;
        }
        if (at != length) {
            free(frame); interop_kill(process);
            return interop_fail(process, error, size, "foreign request delivery failed or was cancelled; it was not replayed");
        }
    }
#endif
    free(frame); return 1;
}

char *zsharp_interop_process_read(ZSharpInteropProcess *process, unsigned timeout_ms,
    ZSharpInteropCancelled cancelled, void *context, char *error, size_t size) {
    char *line = malloc(INTEROP_FRAME_LIMIT + 1);
    size_t used = 0;
    uint64_t deadline = interop_now() + timeout_ms;
    if (!line) { interop_fail(process, error, size, "out of memory reading foreign response"); return NULL; }
    while (!process->failed && interop_now() < deadline && !(cancelled && cancelled(context))) {
        while (process->at < process->count) {
            unsigned char byte = (unsigned char)process->bytes[process->at++];
            if (byte == '\n') {
                if (used && line[used - 1] == '\r') used--;
                line[used] = 0;
                if (memchr(line, '\r', used)) goto failed;
                return line;
            }
            if (!byte || used == INTEROP_FRAME_LIMIT) goto failed;
            line[used++] = (char)byte;
        }
        process->at = process->count = 0;
#ifdef _WIN32
        {
            DWORD available = 0, received = 0;
            if (!PeekNamedPipe(process->output, NULL, 0, NULL, &available, NULL)) goto failed;
            if (!available) { if (WaitForSingleObject(process->process, 10) != WAIT_TIMEOUT) goto failed; continue; }
            if (available > sizeof(process->bytes)) available = sizeof(process->bytes);
            if (!ReadFile(process->output, process->bytes, available, &received, NULL) || !received) goto failed;
            process->count = received;
        }
#else
        {
            struct pollfd descriptor = {process->socket, POLLIN, 0};
            ssize_t count;
            int ready = poll(&descriptor, 1, 10);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (descriptor.revents & POLLNVAL)) goto failed;
            if (!(descriptor.revents & (POLLIN | POLLHUP))) continue;
            count = recv(process->socket, process->bytes, sizeof(process->bytes), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) goto failed;
            process->count = (size_t)count;
        }
#endif
    }
failed:
    interop_kill(process); free(line);
    interop_fail(process, error, size, "foreign runtime exited, timed out, or returned an invalid frame; the call was not replayed");
    return NULL;
}

void zsharp_interop_process_destroy(ZSharpInteropProcess *process) {
    if (!process) return;
#ifdef _WIN32
    if (process->input) CloseHandle(process->input);
    if (process->process && WaitForSingleObject(process->process, 2000) == WAIT_TIMEOUT) interop_kill(process);
    if (process->job) CloseHandle(process->job);
    if (process->process) { WaitForSingleObject(process->process, 1000); CloseHandle(process->process); }
    if (process->output) CloseHandle(process->output);
#else
    if (process->socket >= 0) { shutdown(process->socket, SHUT_WR); close(process->socket); }
    if (process->process > 0) {
        struct timespec grace = {0, 10000000};
        uint64_t deadline = interop_now() + 2000;
        siginfo_t information;
        do {
            memset(&information, 0, sizeof(information));
            if (waitid(P_PID, process->process, &information, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                information.si_pid == process->process) break;
            nanosleep(&grace, NULL);
        } while (interop_now() < deadline);
        /* Keep ownership of the unreaped leader until the group is stopped. */
        kill(-process->process, SIGKILL);
        while (waitpid(process->process, NULL, 0) < 0 && errno == EINTR) { }
    }
#endif
    free(process);
}
