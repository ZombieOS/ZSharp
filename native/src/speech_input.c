#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "speech_input.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

#define STT_REQUEST_LIMIT 16384u
#define STT_RESPONSE_LIMIT 8192u
#define STT_LOG_LIMIT (1024u * 1024u)
#define STT_TAIL_LIMIT 1024u
#define SPEECH_STREAM_FRAME_LIMIT (256u * 1024u)
#define SPEECH_STREAM_TOTAL_LIMIT (128u * 1024u * 1024u)
#define SPEECH_STREAM_FRAME_COUNT 65536u

struct ZSharpSpeechWorker {
    char *python, *helper;
    char tail[STT_TAIL_LIMIT];
    size_t tail_count, log_count;
#ifdef _WIN32
    HANDLE process, job, input, output, diagnostic, reader;
    CRITICAL_SECTION diagnostic_lock;
    volatile LONG excessive_log, stopping, reader_finished, drain_requested, drain_completed;
#else
    pid_t process;
    int input, output, diagnostic;
    pthread_t reader;
    pthread_mutex_t diagnostic_lock;
    atomic_int excessive_log, stopping, reader_finished, drain_requested, drain_completed;
#endif
    int reader_started, diagnostic_initialized, recognition;
};
typedef ZSharpSpeechWorker SpeechInputWorker;

static SpeechInputWorker *stt_worker;
static int stt_atexit_registered;
#ifdef _WIN32
static SRWLOCK stt_lock = SRWLOCK_INIT;
static volatile LONG stt_closing;
#else
static pthread_mutex_t stt_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int stt_closing;
#endif

static int stt_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
    return 0;
}

static char *stt_copy(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static uint64_t stt_now(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static void stt_sleep(void) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec delay = {0, 10000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
#endif
}

static int stt_is_closing(void) {
#ifdef _WIN32
    return InterlockedCompareExchange(&stt_closing, 0, 0) != 0;
#else
    return atomic_load(&stt_closing) != 0;
#endif
}

static int stt_try_lock(void) {
#ifdef _WIN32
    return TryAcquireSRWLockExclusive(&stt_lock) != 0;
#else
    return pthread_mutex_trylock(&stt_lock) == 0;
#endif
}

static void stt_unlock(void) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&stt_lock);
#else
    pthread_mutex_unlock(&stt_lock);
#endif
}

static int stt_alive(SpeechInputWorker *worker) {
#ifdef _WIN32
    if (worker->process == NULL) return 0;
    return WaitForSingleObject(worker->process, 0) == WAIT_TIMEOUT;
#else
    siginfo_t information;
    int result;
    if (worker->process <= 0) return 0;
    memset(&information, 0, sizeof(information));
    /* Retain the owned leader (even when it is a zombie) until the entire
     * process group and diagnostic reader are finished. Reaping here would
     * allow PID/group reuse before the reader's possible group termination. */
    result = waitid(P_PID, (id_t)worker->process, &information, WEXITED | WNOHANG | WNOWAIT);
    return (result == 0 && information.si_pid == 0) || (result < 0 && errno == EINTR);
#endif
}

static void stt_kill(SpeechInputWorker *worker) {
#ifdef _WIN32
    if (worker->job != NULL) TerminateJobObject(worker->job, 1);
    else if (worker->process != NULL) TerminateProcess(worker->process, 1);
#else
    if (worker->process > 0) kill(-worker->process, SIGKILL);
#endif
}

static void stt_diagnostic_lock(SpeechInputWorker *worker) {
#ifdef _WIN32
    EnterCriticalSection(&worker->diagnostic_lock);
#else
    pthread_mutex_lock(&worker->diagnostic_lock);
#endif
}

static void stt_diagnostic_unlock(SpeechInputWorker *worker) {
#ifdef _WIN32
    LeaveCriticalSection(&worker->diagnostic_lock);
#else
    pthread_mutex_unlock(&worker->diagnostic_lock);
#endif
}

static int stt_diagnostic_generation(SpeechInputWorker *worker) {
#ifdef _WIN32
    return (int)InterlockedCompareExchange(&worker->drain_requested, 0, 0);
#else
    return atomic_load(&worker->drain_requested);
#endif
}

static void stt_diagnostic_drained(SpeechInputWorker *worker, int generation) {
#ifdef _WIN32
    InterlockedExchange(&worker->drain_completed, generation);
#else
    atomic_store(&worker->drain_completed, generation);
#endif
}

static void stt_flush_diagnostics(SpeechInputWorker *worker) {
    uint64_t started = stt_now();
#ifdef _WIN32
    LONG generation = InterlockedIncrement(&worker->drain_requested);
#else
    int generation = atomic_fetch_add(&worker->drain_requested, 1) + 1;
#endif
    if (!worker->reader_started) return;
    while (stt_now() - started < 100) {
#ifdef _WIN32
        if (InterlockedCompareExchange(&worker->drain_completed, 0, 0) == generation ||
            InterlockedCompareExchange(&worker->reader_finished, 0, 0) != 0) break;
#else
        if (atomic_load(&worker->drain_completed) == generation || atomic_load(&worker->reader_finished)) break;
#endif
        stt_sleep();
    }
}

/* Stderr is drained even while no request is executing, into bounded private
 * storage. No transcripts, paths, arguments, or logs are printed by this API. */
#ifdef _WIN32
static DWORD WINAPI stt_read_diagnostics(LPVOID argument) {
#else
static void *stt_read_diagnostics(void *argument) {
#endif
    SpeechInputWorker *worker = (SpeechInputWorker *)argument;
    char bytes[4096];
    for (;;) {
        size_t count;
        /* Observe the generation before checking pipe readiness: a request
         * arriving after that check must wait for the next empty observation. */
        int generation = stt_diagnostic_generation(worker);
#ifdef _WIN32
        DWORD received = 0, available = 0;
        if (InterlockedCompareExchange(&worker->stopping, 0, 0) != 0) break;
        if (!PeekNamedPipe(worker->diagnostic, NULL, 0, NULL, &available, NULL)) break;
        if (available == 0) { stt_diagnostic_drained(worker, generation); stt_sleep(); continue; }
        if (available > sizeof(bytes)) available = sizeof(bytes);
        if (!ReadFile(worker->diagnostic, bytes, available, &received, NULL) || received == 0) break;
        count = received;
#else
        struct pollfd ready;
        ssize_t received;
        if (atomic_load(&worker->stopping)) break;
        ready.fd = worker->diagnostic; ready.events = POLLIN; ready.revents = 0;
        if (poll(&ready, 1, 10) < 0) { if (errno == EINTR) continue; break; }
        if (!(ready.revents & POLLIN)) {
            stt_diagnostic_drained(worker, generation);
            if (ready.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            continue;
        }
        received = read(worker->diagnostic, bytes, sizeof(bytes));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;
        count = (size_t)received;
#endif
        stt_diagnostic_lock(worker);
        worker->log_count += count;
        if (count >= STT_TAIL_LIMIT) {
            memcpy(worker->tail, bytes + count - STT_TAIL_LIMIT, STT_TAIL_LIMIT);
            worker->tail_count = STT_TAIL_LIMIT;
        } else {
            size_t keep = worker->tail_count;
            if (keep + count > STT_TAIL_LIMIT) keep = STT_TAIL_LIMIT - count;
            memmove(worker->tail, worker->tail + worker->tail_count - keep, keep);
            memcpy(worker->tail + keep, bytes, count);
            worker->tail_count = keep + count;
        }
        if (worker->log_count > STT_LOG_LIMIT) {
#ifdef _WIN32
            InterlockedExchange(&worker->excessive_log, 1);
#else
            atomic_store(&worker->excessive_log, 1);
#endif
            stt_diagnostic_unlock(worker);
            stt_kill(worker);
            break;
        }
        stt_diagnostic_unlock(worker);
    }
#ifdef _WIN32
    InterlockedExchange(&worker->reader_finished, 1);
    return 0;
#else
    atomic_store(&worker->reader_finished, 1);
    return NULL;
#endif
}

static int stt_log_excessive(SpeechInputWorker *worker) {
#ifdef _WIN32
    return InterlockedCompareExchange(&worker->excessive_log, 0, 0) != 0;
#else
    return atomic_load(&worker->excessive_log) != 0;
#endif
}

static void stt_failure_diagnostic(SpeechInputWorker *worker, char *error, size_t size) {
    char tail[STT_TAIL_LIMIT + 1];
    size_t count, i, used;
    if (error == NULL || size == 0 || !worker->diagnostic_initialized) return;
    stt_diagnostic_lock(worker);
    count = worker->tail_count;
    memcpy(tail, worker->tail, count);
    stt_diagnostic_unlock(worker);
    tail[count] = '\0';
    for (i = 0; i < count; i++) if ((unsigned char)tail[i] < 32) tail[i] = ' ';
    used = strlen(error);
    if (count != 0 && used + 3 < size) snprintf(error + used, size - used, ": %s", tail);
}

static void stt_collect_failure(SpeechInputWorker *worker, char *error, size_t size) {
    stt_kill(worker);
    /* Give the independent drain a bounded opportunity to collect bytes that
     * were written immediately before a crash. Do not wait on escaped writers. */
    stt_flush_diagnostics(worker);
    stt_failure_diagnostic(worker, error, size);
}

static void stt_destroy(SpeechInputWorker *worker, int graceful) {
    uint64_t started;
    if (worker == NULL) return;
#ifdef _WIN32
    if (worker->input != NULL) { CloseHandle(worker->input); worker->input = NULL; }
#else
    if (worker->input >= 0) { close(worker->input); worker->input = -1; }
#endif
    started = stt_now();
    if (graceful) while (stt_alive(worker) && stt_now() - started < 200) stt_sleep();
    stt_kill(worker);
#ifdef _WIN32
    InterlockedExchange(&worker->stopping, 1);
    if (worker->process != NULL) WaitForSingleObject(worker->process, 2000);
    if (worker->reader_started) {
        /* The reader never blocks in a pipe read. If the OS cannot schedule it
         * within this bound, retain its private storage instead of freeing
         * memory that a late reader could still access. */
        if (WaitForSingleObject(worker->reader, 2000) != WAIT_OBJECT_0) return;
        CloseHandle(worker->reader);
    }
    if (worker->output != NULL) CloseHandle(worker->output);
    if (worker->diagnostic != NULL) CloseHandle(worker->diagnostic);
    if (worker->process != NULL) CloseHandle(worker->process);
    if (worker->job != NULL) CloseHandle(worker->job);
    if (worker->diagnostic_initialized) DeleteCriticalSection(&worker->diagnostic_lock);
#else
    atomic_store(&worker->stopping, 1);
    if (worker->reader_started) pthread_join(worker->reader, NULL);
    /* No remaining thread can send signals after this final reap. */
    if (worker->process > 0) {
        int status;
        while (waitpid(worker->process, &status, 0) < 0 && errno == EINTR) {}
    }
    if (worker->output >= 0) close(worker->output);
    if (worker->diagnostic >= 0) close(worker->diagnostic);
    if (worker->diagnostic_initialized) pthread_mutex_destroy(&worker->diagnostic_lock);
#endif
    free(worker->python); free(worker->helper); free(worker);
}

#ifdef _WIN32
static wchar_t *stt_wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = count > 0 && count < 32768 ? (wchar_t *)calloc((size_t)count, sizeof(*wide)) : NULL;
    if (wide != NULL && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count)) {
        free(wide); wide = NULL;
    }
    return wide;
}

/* Correct C-runtime argv quoting, including quotes and trailing backslashes. */
static wchar_t *stt_command(const char *const *arguments, size_t count) {
    wchar_t *command = (wchar_t *)calloc(32768, sizeof(*command));
    size_t used = 0, arg;
    if (command == NULL) return NULL;
    for (arg = 0; arg < count; arg++) {
        wchar_t *wide = stt_wide(arguments[arg]);
        size_t index = 0;
        if (wide == NULL) goto failed;
        if (used != 0) command[used++] = L' ';
        command[used++] = L'"';
        while (wide[index] != 0) {
            size_t slashes = 0, times;
            while (wide[index] == L'\\') { slashes++; index++; }
            times = wide[index] == L'"' || wide[index] == 0 ? slashes * 2 : slashes;
            if (used + times + 4 >= 32768) { free(wide); goto failed; }
            while (times-- != 0) command[used++] = L'\\';
            if (wide[index] == 0) break;
            if (wide[index] == L'"') command[used++] = L'\\';
            command[used++] = wide[index++];
        }
        free(wide);
        if (used + 3 >= 32768) goto failed;
        command[used++] = L'"';
    }
    return command;
failed:
    free(command); return NULL;
}

/* Parent end supports overlapped I/O, so stalled writes as well as reads are
 * cancellable. Only the child end is inheritable. */
static int stt_pipe(HANDLE *parent, HANDLE *child, int parent_reads) {
    wchar_t name[160];
    LARGE_INTEGER nonce;
    static volatile LONG sequence;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    QueryPerformanceCounter(&nonce);
    swprintf(name, sizeof(name) / sizeof(name[0]), L"\\\\.\\pipe\\zsharp-stt-%lu-%llu-%ld",
        (unsigned long)GetCurrentProcessId(), (unsigned long long)nonce.QuadPart,
        (long)InterlockedIncrement(&sequence));
    *parent = CreateNamedPipeW(name,
        (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) { *parent = NULL; return 0; }
    *child = CreateFileW(name, parent_reads ? GENERIC_WRITE : GENERIC_READ, 0,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (*child == INVALID_HANDLE_VALUE) { *child = NULL; CloseHandle(*parent); *parent = NULL; return 0; }
    {
        OVERLAPPED connection;
        int connected;
        memset(&connection, 0, sizeof(connection));
        connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (connection.hEvent == NULL) { CloseHandle(*child); CloseHandle(*parent); *child = *parent = NULL; return 0; }
        connected = ConnectNamedPipe(*parent, &connection) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
        CloseHandle(connection.hEvent);
        if (!connected) { CloseHandle(*child); CloseHandle(*parent); *child = *parent = NULL; return 0; }
    }
    return 1;
}
#endif

static SpeechInputWorker *stt_start(const char *python, const char *helper, char *error, size_t size) {
    SpeechInputWorker *worker = (SpeechInputWorker *)calloc(1, sizeof(*worker));
    const char *arguments[] = {python, "-I", helper, "worker", NULL};
    if (worker == NULL) { stt_error(error, size, "could not allocate local speech worker"); return NULL; }
#ifndef _WIN32
    worker->input = worker->output = worker->diagnostic = -1;
#endif
    worker->python = stt_copy(python); worker->helper = stt_copy(helper);
    if (worker->python == NULL || worker->helper == NULL) goto failed;
#ifdef _WIN32
    {
        HANDLE child_input = NULL, child_output = NULL, child_diagnostic = NULL;
        HANDLE inherited[3];
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        STARTUPINFOEXW startup;
        PROCESS_INFORMATION process;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        SIZE_T attribute_size = 0;
        int initialized = 0, ok = 0;
        wchar_t *executable = stt_wide(python), *command = stt_command(arguments, 4);
        memset(&startup, 0, sizeof(startup)); startup.StartupInfo.cb = sizeof(startup);
        memset(&process, 0, sizeof(process)); memset(&limits, 0, sizeof(limits));
        InitializeCriticalSection(&worker->diagnostic_lock); worker->diagnostic_initialized = 1;
        if (executable == NULL || command == NULL ||
            !stt_pipe(&worker->input, &child_input, 0) || !stt_pipe(&worker->output, &child_output, 1) ||
            !CreatePipe(&worker->diagnostic, &child_diagnostic, &security, 65536) ||
            !SetHandleInformation(worker->diagnostic, HANDLE_FLAG_INHERIT, 0)) goto windows_done;
        worker->job = CreateJobObjectW(NULL, NULL);
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (worker->job == NULL || !SetInformationJobObject(worker->job,
            JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto windows_done;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = child_input;
        startup.StartupInfo.hStdOutput = child_output;
        startup.StartupInfo.hStdError = child_diagnostic;
        inherited[0] = child_input; inherited[1] = child_output; inherited[2] = child_diagnostic;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
        startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
        if (startup.lpAttributeList == NULL ||
            !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) goto windows_done;
        initialized = 1;
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), NULL, NULL)) goto windows_done;
        if (!CreateProcessW(executable, command, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            NULL, NULL, &startup.StartupInfo, &process)) goto windows_done;
        worker->process = process.hProcess;
        if (!AssignProcessToJobObject(worker->job, process.hProcess)) {
            TerminateProcess(process.hProcess, 1); goto windows_done;
        }
        worker->reader = CreateThread(NULL, 0, stt_read_diagnostics, worker, 0, NULL);
        if (worker->reader == NULL) goto windows_done;
        worker->reader_started = 1;
        if (ResumeThread(process.hThread) == (DWORD)-1) goto windows_done;
        ok = 1;
windows_done:
        if (process.hThread != NULL) CloseHandle(process.hThread);
        if (child_input != NULL) CloseHandle(child_input);
        if (child_output != NULL) CloseHandle(child_output);
        if (child_diagnostic != NULL) CloseHandle(child_diagnostic);
        if (startup.lpAttributeList != NULL) {
            if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            free(startup.lpAttributeList);
        }
        free(executable); free(command);
        if (!ok) goto failed;
    }
#else
    {
        int input[2] = {-1, -1}, output[2] = {-1, -1}, diagnostic[2] = {-1, -1};
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        int actions_ready = 0, attributes_ready = 0, ok = 0, i;
        if (pthread_mutex_init(&worker->diagnostic_lock, NULL) != 0) goto posix_done;
        worker->diagnostic_initialized = 1;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, input) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, output) != 0 || pipe(diagnostic) != 0) goto posix_done;
        for (i = 0; i < 2; i++) {
            if (fcntl(input[i], F_SETFD, FD_CLOEXEC) < 0 || fcntl(output[i], F_SETFD, FD_CLOEXEC) < 0 ||
                fcntl(diagnostic[i], F_SETFD, FD_CLOEXEC) < 0) goto posix_done;
        }
#ifdef __APPLE__
        { int one = 1; if (setsockopt(input[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) goto posix_done; }
#endif
        if (posix_spawn_file_actions_init(&actions) != 0) goto posix_done;
        actions_ready = 1;
        if (posix_spawnattr_init(&attributes) != 0) goto posix_done;
        attributes_ready = 1;
        if (posix_spawn_file_actions_adddup2(&actions, input[1], STDIN_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, diagnostic[1], STDERR_FILENO) != 0 ||
            posix_spawnattr_setpgroup(&attributes, 0) != 0 ||
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0) goto posix_done;
        if (posix_spawnp(&worker->process, python, &actions, &attributes,
            (char *const *)arguments, environ) != 0) { worker->process = 0; goto posix_done; }
        worker->input = input[0]; input[0] = -1;
        worker->output = output[0]; output[0] = -1;
        worker->diagnostic = diagnostic[0]; diagnostic[0] = -1;
        if (fcntl(worker->input, F_SETFL, fcntl(worker->input, F_GETFL) | O_NONBLOCK) < 0 ||
            fcntl(worker->output, F_SETFL, fcntl(worker->output, F_GETFL) | O_NONBLOCK) < 0) goto posix_done;
        if (pthread_create(&worker->reader, NULL, stt_read_diagnostics, worker) != 0) goto posix_done;
        worker->reader_started = 1; ok = 1;
posix_done:
        if (actions_ready) posix_spawn_file_actions_destroy(&actions);
        if (attributes_ready) posix_spawnattr_destroy(&attributes);
        for (i = 0; i < 2; i++) {
            if (input[i] >= 0) close(input[i]);
            if (output[i] >= 0) close(output[i]);
            if (diagnostic[i] >= 0) close(diagnostic[i]);
        }
        if (!ok) goto failed;
    }
#endif
    return worker;
failed:
    stt_destroy(worker, 0);
    stt_error(error, size, "could not start the private local speech worker; check its Python installation and helper");
    return NULL;
}

static int stt_check(SpeechInputWorker *worker, uint64_t started, unsigned int timeout,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t size) {
    if ((worker->recognition && stt_is_closing()) || (cancelled != NULL && cancelled(context)))
        return stt_error(error, size, "local speech operation cancelled");
    if (stt_now() - started >= timeout) return stt_error(error, size, "local speech worker timed out");
    if (stt_log_excessive(worker)) return stt_error(error, size, "local speech worker exceeded its diagnostic output limit");
    return 1;
}

static int stt_transfer(SpeechInputWorker *worker, void *bytes, size_t count, int writing,
    uint64_t started, unsigned int timeout, ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t size) {
    size_t offset = 0;
    while (offset < count) {
        if (!stt_check(worker, started, timeout, cancelled, context, error, size)) return 0;
#ifdef _WIN32
        OVERLAPPED operation;
        DWORD transferred = 0;
        HANDLE pipe = writing ? worker->input : worker->output;
        int completed;
        memset(&operation, 0, sizeof(operation));
        operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (operation.hEvent == NULL) return stt_error(error, size, "could not create local speech I/O event");
        completed = writing ? WriteFile(pipe, (char *)bytes + offset, (DWORD)(count - offset), &transferred, &operation)
                            : ReadFile(pipe, (char *)bytes + offset, (DWORD)(count - offset), &transferred, &operation);
        if (!completed && GetLastError() == ERROR_IO_PENDING) {
            for (;;) {
                DWORD wait = WaitForSingleObject(operation.hEvent, 10);
                if (wait == WAIT_OBJECT_0) { completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE) != 0; break; }
                if (wait == WAIT_FAILED || !stt_check(worker, started, timeout, cancelled, context, error, size)) {
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                    CloseHandle(operation.hEvent);
                    if (wait == WAIT_FAILED) stt_error(error, size, "could not wait for local speech I/O");
                    return 0;
                }
                if (!stt_alive(worker)) {
                    CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                    CloseHandle(operation.hEvent);
                    return stt_error(error, size, "local speech worker exited before returning a result");
                }
            }
        }
        CloseHandle(operation.hEvent);
        if (!completed || transferred == 0)
            return stt_error(error, size, "local speech worker disconnected before returning a result");
        offset += transferred;
#else
        ssize_t transferred;
        struct pollfd ready;
        ready.fd = writing ? worker->input : worker->output;
        ready.events = writing ? POLLOUT : POLLIN; ready.revents = 0;
        if (poll(&ready, 1, 10) < 0) {
            if (errno == EINTR) continue;
            return stt_error(error, size, "could not wait for local speech I/O");
        }
        if (!(ready.revents & (writing ? POLLOUT : POLLIN))) {
            if (ready.revents & (POLLERR | POLLHUP | POLLNVAL) || !stt_alive(worker))
                return stt_error(error, size, "local speech worker exited before returning a result");
            continue;
        }
        if (writing) {
#ifdef __APPLE__
            transferred = send(worker->input, (char *)bytes + offset, count - offset, 0);
#else
            transferred = send(worker->input, (char *)bytes + offset, count - offset, MSG_NOSIGNAL);
#endif
        } else transferred = read(worker->output, (char *)bytes + offset, count - offset);
        if (transferred < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (transferred <= 0) return stt_error(error, size, "local speech worker disconnected before returning a result");
        offset += (size_t)transferred;
#endif
    }
    return 1;
}

static int stt_output_pending(SpeechInputWorker *worker) {
#ifdef _WIN32
    DWORD available = 0;
    return PeekNamedPipe(worker->output, NULL, 0, NULL, &available, NULL) && available != 0;
#else
    struct pollfd ready;
    ready.fd = worker->output; ready.events = POLLIN; ready.revents = 0;
    return poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN);
#endif
}

static int stt_utf8(const unsigned char *text) {
    while (*text != 0) {
        uint32_t code; unsigned int following, i;
        if (*text < 0x80) { text++; continue; }
        if (*text >= 0xc2 && *text <= 0xdf) { code = *text & 31u; following = 1; }
        else if (*text >= 0xe0 && *text <= 0xef) { code = *text & 15u; following = 2; }
        else if (*text >= 0xf0 && *text <= 0xf4) { code = *text & 7u; following = 3; }
        else return 0;
        text++;
        for (i = 0; i < following; i++) { if ((*text & 0xc0u) != 0x80u) return 0; code = (code << 6) | (*text++ & 63u); }
        if ((following == 1 && code < 0x80) || (following == 2 && code < 0x800) ||
            (following == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}

static int stt_request(const char *const *args, size_t count, char *output, size_t *length) {
    size_t used = 0, arg;
    static const char hex[] = "0123456789abcdef";
    output[used++] = '[';
    for (arg = 0; arg < count; arg++) {
        const unsigned char *text = (const unsigned char *)args[arg];
        if (text == NULL || !stt_utf8(text)) return 0;
        if (arg != 0) output[used++] = ',';
        output[used++] = '"';
        while (*text != 0) {
            unsigned char byte = *text++;
            if (used + 8 >= STT_REQUEST_LIMIT) return 0;
            if (byte == '"' || byte == '\\') { output[used++] = '\\'; output[used++] = (char)byte; }
            else if (byte < 32) {
                output[used++] = '\\'; output[used++] = 'u'; output[used++] = '0'; output[used++] = '0';
                output[used++] = hex[byte >> 4]; output[used++] = hex[byte & 15u];
            } else output[used++] = (char)byte;
        }
        output[used++] = '"';
    }
    output[used++] = ']'; output[used] = '\0'; *length = used;
    return 1;
}

static const char *stt_space(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    return text;
}

static int stt_hex(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static int stt_unicode(const char **cursor, uint32_t *code) {
    unsigned int i;
    *code = 0;
    for (i = 0; i < 4; i++) { int digit = stt_hex(**cursor); if (digit < 0) return 0; (*cursor)++; *code = (*code << 4) | (unsigned)digit; }
    return 1;
}

static int stt_json_string(const char **cursor, char *output, size_t capacity) {
    const char *text = *cursor;
    size_t used = 0;
    if (*text++ != '"') return 0;
    while (*text != '"') {
        uint32_t code;
        if ((unsigned char)*text < 32) return 0;
        if (*text != '\\') {
            if (used + 1 >= capacity) return 0;
            output[used++] = *text++; continue;
        }
        text++;
        switch (*text++) {
            case '"': code = '"'; break; case '\\': code = '\\'; break; case '/': code = '/'; break;
            case 'b': code = 8; break; case 'f': code = 12; break; case 'n': code = 10; break;
            case 'r': code = 13; break; case 't': code = 9; break;
            case 'u':
                if (!stt_unicode(&text, &code)) return 0;
                if (code >= 0xd800 && code <= 0xdbff) {
                    uint32_t second;
                    if (text[0] != '\\' || text[1] != 'u') return 0;
                    text += 2;
                    if (!stt_unicode(&text, &second) || second < 0xdc00 || second > 0xdfff) return 0;
                    code = 0x10000 + ((code - 0xd800) << 10) + second - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return 0;
                break;
            default: return 0;
        }
        if (code == 0 || used + 5 >= capacity) return 0;
        if (code < 0x80) output[used++] = (char)code;
        else if (code < 0x800) { output[used++] = (char)(0xc0 | (code >> 6)); output[used++] = (char)(0x80 | (code & 63)); }
        else if (code < 0x10000) { output[used++] = (char)(0xe0 | (code >> 12)); output[used++] = (char)(0x80 | ((code >> 6) & 63)); output[used++] = (char)(0x80 | (code & 63)); }
        else { output[used++] = (char)(0xf0 | (code >> 18)); output[used++] = (char)(0x80 | ((code >> 12) & 63)); output[used++] = (char)(0x80 | ((code >> 6) & 63)); output[used++] = (char)(0x80 | (code & 63)); }
    }
    output[used] = '\0'; *cursor = text + 1;
    return stt_utf8((const unsigned char *)output);
}

/* Protocol parser accepts exactly one boolean ok and optional string error;
 * malformed/duplicate fields and trailing data never become successful RPCs. */
static int stt_response(const char *text, char *message, size_t capacity, int *ok) {
    int has_ok = 0, has_error = 0;
    char key[32];
    text = stt_space(text);
    if (*text++ != '{') return 0;
    for (;;) {
        text = stt_space(text);
        if (!stt_json_string(&text, key, sizeof(key))) return 0;
        text = stt_space(text); if (*text++ != ':') return 0; text = stt_space(text);
        if (strcmp(key, "ok") == 0 && !has_ok) {
            if (strncmp(text, "true", 4) == 0) { *ok = 1; text += 4; }
            else if (strncmp(text, "false", 5) == 0) { *ok = 0; text += 5; }
            else return 0;
            has_ok = 1;
        } else if (strcmp(key, "error") == 0 && !has_error) {
            if (!stt_json_string(&text, message, capacity)) return 0;
            has_error = 1;
        } else return 0;
        text = stt_space(text);
        if (*text == '}') { text = stt_space(text + 1); break; }
        if (*text++ != ',') return 0;
    }
    return *text == '\0' && has_ok && (*ok || (has_error && message[0] != '\0'));
}

static int speech_worker_request(ZSharpSpeechWorker **handle,
    const char *python, const char *helper, const char *const *arguments,
    size_t argument_count, size_t frame_limit, int recognition,
    ZSharpSpeechWorkerFrame frame, void *frame_context,
    uint64_t started, unsigned int timeout_ms,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t error_size) {
    char request[STT_REQUEST_LIMIT + 1], *response = NULL;
    unsigned char header[4];
    size_t request_size, response_size, total = 0, frames = 0;
    SpeechInputWorker *worker;
    int ok = 0;
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (handle == NULL || python == NULL || python[0] == '\0' || helper == NULL || helper[0] == '\0' ||
        arguments == NULL || argument_count == 0 || argument_count > 3 || frame == NULL ||
        timeout_ms == 0 || timeout_ms > 600000 || frame_limit > SPEECH_STREAM_FRAME_LIMIT ||
        !stt_request(arguments, argument_count, request, &request_size))
        return stt_error(error, error_size, "invalid local speech worker arguments or timeout");
    if ((recognition && stt_is_closing()) || (cancelled != NULL && cancelled(context)))
        return stt_error(error, error_size, "local speech operation cancelled");
    if (*handle != NULL && (strcmp((*handle)->python, python) != 0 || strcmp((*handle)->helper, helper) != 0))
        return stt_error(error, error_size, "local speech worker configuration changed; shut down the current worker before changing its Python executable or helper");
    response = (char *)malloc(frame_limit + 1);
    if (response == NULL) return stt_error(error, error_size, "could not allocate local speech response buffer");
    if (*handle == NULL) *handle = stt_start(python, helper, error, error_size);
    worker = *handle;
    if (worker == NULL) goto done;
    worker->recognition = recognition;
    if (stt_log_excessive(worker)) { stt_error(error, error_size, "local speech worker exceeded its diagnostic output limit"); goto broken; }
    if (!stt_alive(worker)) { stt_error(error, error_size, "local speech worker exited between requests"); goto broken; }
    if (stt_output_pending(worker)) { stt_error(error, error_size, "local speech worker sent unsolicited protocol data"); goto broken; }
    stt_flush_diagnostics(worker);
    stt_diagnostic_lock(worker); worker->tail_count = 0; stt_diagnostic_unlock(worker);
    header[0] = (unsigned char)request_size; header[1] = (unsigned char)(request_size >> 8);
    header[2] = (unsigned char)(request_size >> 16); header[3] = (unsigned char)(request_size >> 24);
    if (!stt_transfer(worker, header, sizeof(header), 1, started, timeout_ms, cancelled, context, error, error_size) ||
        !stt_transfer(worker, request, request_size, 1, started, timeout_ms, cancelled, context, error, error_size)) goto broken;
    for (;;) {
        int complete = 0;
        if (!stt_transfer(worker, header, sizeof(header), 0, started, timeout_ms, cancelled, context, error, error_size)) goto broken;
        response_size = (size_t)header[0] | ((size_t)header[1] << 8) | ((size_t)header[2] << 16) | ((size_t)header[3] << 24);
        if (response_size == 0 || response_size > frame_limit || ++frames > SPEECH_STREAM_FRAME_COUNT ||
            response_size > SPEECH_STREAM_TOTAL_LIMIT - total) {
            stt_error(error, error_size, "local speech worker response exceeds its protocol limit"); goto broken;
        }
        total += response_size;
        if (!stt_transfer(worker, response, response_size, 0, started, timeout_ms, cancelled, context, error, error_size)) goto broken;
        if (memchr(response, '\0', response_size) != NULL) {
            stt_error(error, error_size, "local speech worker returned a malformed response"); goto broken;
        }
        response[response_size] = '\0';
        if (!stt_utf8((const unsigned char *)response)) {
            stt_error(error, error_size, "local speech worker response is not valid UTF-8"); goto broken;
        }
        ok = frame(response, response_size, frame_context, &complete, error, error_size);
        if (complete != 0 && complete != 1) { stt_error(error, error_size, "local speech callback returned an invalid completion state"); goto broken; }
        if (!ok && !complete) {
            if (error != NULL && error_size != 0 && error[0] == '\0') stt_error(error, error_size, "local speech worker returned a malformed response");
            goto broken;
        }
        if (complete) {
            if (stt_output_pending(worker)) { stt_error(error, error_size, "local speech worker returned unsolicited protocol data"); goto broken; }
            if (!ok) {
                if (error != NULL && error_size != 0 && error[0] == '\0') stt_error(error, error_size, "local speech worker request failed");
                stt_flush_diagnostics(worker); stt_failure_diagnostic(worker, error, error_size);
            }
            goto done;
        }
    }
broken:
    ok = 0;
    stt_collect_failure(worker, error, error_size);
    stt_destroy(worker, 0); *handle = NULL;
done:
    free(response);
    return ok;
}

static int stt_frame(const char *response, size_t length, void *context,
    int *complete, char *error, size_t error_size) {
    char message[STT_RESPONSE_LIMIT + 1];
    int ok = 0;
    size_t index;
    (void)length; (void)context;
    message[0] = '\0';
    if (!stt_response(response, message, sizeof(message), &ok))
        return stt_error(error, error_size, "speech recognition worker returned a malformed response");
    *complete = 1;
    if (!ok) {
        for (index = 0; message[index] != '\0'; index++) if ((unsigned char)message[index] < 32) message[index] = ' ';
        if (error != NULL && error_size != 0) snprintf(error, error_size, "speech recognition error: %s", message);
    }
    return ok;
}

int zsharp_speech_worker_run_stream(ZSharpSpeechWorker **worker,
    const char *python, const char *helper, const char *const *arguments,
    size_t argument_count, ZSharpSpeechWorkerFrame frame, void *frame_context,
    unsigned int timeout_ms, ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size) {
    if (arguments == NULL || argument_count < 2 || argument_count > 3 || arguments[0] == NULL ||
        !(((argument_count == 2 || (argument_count == 3 && arguments[2] != NULL && arguments[2][0] != '\0')) &&
           strcmp(arguments[0], "prepare") == 0 && arguments[1] != NULL && arguments[1][0] != '\0') ||
          (argument_count == 3 && strcmp(arguments[0], "synthesize-stream") == 0 &&
           arguments[1] != NULL && arguments[1][0] != '\0' && arguments[2] != NULL && arguments[2][0] != '\0')))
        return stt_error(error, error_size, "invalid local speech streaming arguments");
    return speech_worker_request(worker, python, helper, arguments, argument_count,
        SPEECH_STREAM_FRAME_LIMIT, 0, frame, frame_context, stt_now(),
        timeout_ms, cancelled, context, error, error_size);
}

void zsharp_speech_worker_destroy(ZSharpSpeechWorker *worker) {
    stt_destroy(worker, 1);
}

int zsharp_speech_input_run(const char *python, const char *helper,
    const char *const *arguments, size_t argument_count, unsigned int timeout_ms,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t error_size) {
    uint64_t started = stt_now();
    int ok = 0;
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (python == NULL || python[0] == '\0' || helper == NULL || helper[0] == '\0' ||
        arguments == NULL || timeout_ms == 0 || timeout_ms > 600000 ||
        !((argument_count == 1 && arguments[0] != NULL && strcmp(arguments[0], "preflight") == 0) ||
          (argument_count == 3 && arguments[0] != NULL && strcmp(arguments[0], "transcribe") == 0 &&
           arguments[1] != NULL && arguments[1][0] != '\0' && arguments[2] != NULL && arguments[2][0] != '\0')))
        return stt_error(error, error_size, "invalid speech recognition worker arguments or timeout");
    while (!stt_try_lock()) {
        if (stt_is_closing() || (cancelled != NULL && cancelled(context)))
            return stt_error(error, error_size, "speech recognition operation cancelled while waiting for the worker");
        if (stt_now() - started >= timeout_ms)
            return stt_error(error, error_size, "speech recognition worker timed out while waiting for another request");
        stt_sleep();
    }
    if (stt_is_closing() || (cancelled != NULL && cancelled(context))) {
        stt_error(error, error_size, "speech recognition operation cancelled"); goto done;
    }
    if (!stt_atexit_registered) {
        if (atexit(zsharp_speech_input_shutdown) != 0) { stt_error(error, error_size, "could not register speech recognition cleanup"); goto done; }
        stt_atexit_registered = 1;
    }
    ok = speech_worker_request(&stt_worker, python, helper, arguments, argument_count,
        STT_RESPONSE_LIMIT, 1, stt_frame, NULL, started, timeout_ms,
        cancelled, context, error, error_size);
done:
    /* A structured model/input error does not corrupt the transport or discard
     * the loaded model. Never replay a timed-out or interrupted command. */
    stt_unlock();
    return ok;
}

void zsharp_speech_input_shutdown(void) {
#ifdef _WIN32
    InterlockedExchange(&stt_closing, 1);
#else
    atomic_store(&stt_closing, 1);
#endif
    while (!stt_try_lock()) stt_sleep();
    stt_destroy(stt_worker, 1); stt_worker = NULL;
#ifdef _WIN32
    InterlockedExchange(&stt_closing, 0);
#else
    atomic_store(&stt_closing, 0);
#endif
    stt_unlock();
}
