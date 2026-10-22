#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define COBJMACROS
#include "speech_api.h"
#include "speech_input.h"
#include "audio_api.h"
#include <quickjs.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define SPEECH_TEXT_LIMIT 16384u
#define SPEECH_VOICE_LIMIT (64u * 1024u * 1024u)
#define SPEECH_REFERENCE_LIMIT (50u * 1024u * 1024u)
#define SPEECH_QUEUE_LIMIT 32u
#define SPEECH_PATH_LIMIT 32768u
#define SPEECH_MODEL_TIMEOUT 120000u
#define SPEECH_LOG_LIMIT (1024u * 1024u)

typedef struct SpeechVoice {
    int cloned;
    char *path;
    char *voice;
    double pitch, speed, volume;
} SpeechVoice;

typedef struct SpeechJob {
    char *text;
    SpeechVoice voice;
    struct SpeechJob *next;
} SpeechJob;

struct ZSharpSpeechService {
    char *root, *python, *helper;
    ZSharpSpeechWorker *voice_worker;
    SpeechJob *head, *tail;
    unsigned int queued;
    int busy;
    char failure[1024];
#ifdef _WIN32
    CRITICAL_SECTION lock;
    SRWLOCK voice_lock;
    CONDITION_VARIABLE condition;
    HANDLE thread;
    volatile LONG stopping;
#else
    pthread_mutex_t lock;
    pthread_mutex_t voice_lock;
    pthread_cond_t condition;
    pthread_t thread;
    atomic_int stopping;
#endif
};

static void speech_clear(char *error, size_t size) {
    if (error != NULL && size != 0) error[0] = '\0';
}

static int speech_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
    return 0;
}

static char *speech_copy(const char *value) {
    size_t count;
    char *copy;
    if (value == NULL) return NULL;
    count = strlen(value) + 1;
    copy = (char *)malloc(count);
    if (copy != NULL) memcpy(copy, value, count);
    return copy;
}

#ifdef _WIN32
static wchar_t *speech_wide(const char *text) {
    int count;
    wchar_t *wide;
    if (text == NULL) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (count <= 0 || count > (int)SPEECH_PATH_LIMIT * 8) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide != NULL && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             text, -1, wide, count)) {
        free(wide); wide = NULL;
    }
    return wide;
}

static char *speech_utf8(const wchar_t *wide) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = count > 0 ? (char *)malloc((size_t)count) : NULL;
    if (text != NULL && !WideCharToMultiByte(CP_UTF8, 0, wide, -1, text,
                                             count, NULL, NULL)) {
        free(text); text = NULL;
    }
    return text;
}
#endif

static FILE *speech_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wide = speech_wide(path);
    wchar_t *wide_mode = speech_wide(mode);
    FILE *file = wide == NULL || wide_mode == NULL ? NULL : _wfopen(wide, wide_mode);
    free(wide); free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

static void speech_remove(const char *path) {
    if (path == NULL) return;
#ifdef _WIN32
    wchar_t *wide = speech_wide(path);
    if (wide != NULL) DeleteFileW(wide);
    free(wide);
#else
    unlink(path);
#endif
}

static char *speech_join(const char *base, const char *name) {
    size_t left = strlen(base), right = strlen(name);
    char *path;
    if (left + right + 2 > SPEECH_PATH_LIMIT) return NULL;
    path = (char *)malloc(left + right + 2);
    if (path != NULL) {
        memcpy(path, base, left);
        path[left] = '/';
        memcpy(path + left + 1, name, right + 1);
#ifdef _WIN32
        /* GetFinalPathNameByHandle returns an extended-length path; Windows
         * intentionally does not normalize forward slashes after \\?\. */
        { size_t index; for (index = 0; index < left + right + 1; index++)
            if (path[index] == '/') path[index] = '\\'; }
#endif
    }
    return path;
}

static uint64_t speech_now(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
#endif
}

static void speech_sleep(void) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec delay = {0, 10000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
#endif
}

static char *speech_temp_directory(char *error, size_t size) {
#ifdef _WIN32
    wchar_t base[MAX_PATH + 1], path[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH, base);
    if (length == 0 || length >= MAX_PATH ||
        GetTempFileNameW(base, L"zsv", 0, path) == 0) {
        speech_error(error, size, "could not create a private speech temporary directory");
        return NULL;
    }
    DeleteFileW(path);
    if (!CreateDirectoryW(path, NULL)) {
        speech_error(error, size, "could not create a private speech temporary directory");
        return NULL;
    }
    return speech_utf8(path);
#else
    const char *base = getenv("TMPDIR");
    char *path;
    if (base == NULL || base[0] != '/') base = "/tmp";
    path = speech_join(base, "zsharp-speech-XXXXXX");
    if (path == NULL || mkdtemp(path) == NULL) {
        free(path);
        speech_error(error, size, "could not create a private speech temporary directory");
        return NULL;
    }
    return path;
#endif
}

/* Only called for directories returned by speech_temp_directory. Never follow
 * a reparse point/symlink planted inside an owned helper workspace. */
static void speech_remove_directory(const char *directory) {
    if (directory == NULL) return;
#ifdef _WIN32
    wchar_t *wide = speech_wide(directory);
    char *pattern = speech_join(directory, "*");
    wchar_t *wide_pattern = speech_wide(pattern);
    WIN32_FIND_DATAW entry;
    HANDLE find = wide_pattern == NULL ? INVALID_HANDLE_VALUE : FindFirstFileW(wide_pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char *name, *path;
            wchar_t *wide_path;
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            name = speech_utf8(entry.cFileName); path = name == NULL ? NULL : speech_join(directory, name);
            wide_path = speech_wide(path);
            if (path != NULL && (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) speech_remove_directory(path);
            else if (wide_path != NULL && (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) RemoveDirectoryW(wide_path);
            else if (wide_path != NULL) DeleteFileW(wide_path);
            free(name); free(path); free(wide_path);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    if (wide != NULL) RemoveDirectoryW(wide);
    free(pattern); free(wide_pattern);
    free(wide);
#else
    DIR *stream = opendir(directory);
    struct dirent *entry;
    if (stream != NULL) {
        while ((entry = readdir(stream)) != NULL) {
            struct stat attributes;
            char *path;
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            path = speech_join(directory, entry->d_name);
            if (path != NULL && lstat(path, &attributes) == 0) {
                if (S_ISDIR(attributes.st_mode)) speech_remove_directory(path);
                else unlink(path);
            }
            free(path);
        }
        closedir(stream);
    }
    rmdir(directory);
#endif
}

static char *speech_read(const char *path, size_t limit, size_t *length,
                          char *error, size_t size) {
    FILE *file = speech_open(path, "rb");
    long end;
    char *data = NULL;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) < 0 ||
        (size_t)end > limit || fseek(file, 0, SEEK_SET) != 0) {
        speech_error(error, size, "speech file is missing, unreadable, or exceeds its size limit");
        goto done;
    }
    data = (char *)malloc((size_t)end + 1);
    if (data == NULL || fread(data, 1, (size_t)end, file) != (size_t)end) {
        free(data); data = NULL;
        speech_error(error, size, "could not read speech file");
        goto done;
    }
    data[end] = '\0';
    if (length != NULL) *length = (size_t)end;
done:
    if (file != NULL) fclose(file);
    return data;
}

static int speech_write(const char *path, const char *text, char *error, size_t size) {
    FILE *file = speech_open(path, "wb");
    size_t length = strlen(text);
    int ok;
    if (file == NULL) return speech_error(error, size, "could not create speech text input");
    ok = fwrite(text, 1, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    return ok ? 1 : speech_error(error, size, "could not write speech text input");
}

static int speech_publish_voice(const char *staging, const char *destination,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t size) {
    char *temporary = NULL;
    FILE *input = NULL, *output = NULL;
    unsigned char buffer[65536];
    size_t total = 0, count;
    int ok = 0;
#ifdef _WIN32
    GUID id;
    size_t length = strlen(destination);
    if (FAILED(CoCreateGuid(&id))) goto done;
    temporary = (char *)malloc(length + 80);
    if (temporary == NULL) goto done;
    snprintf(temporary, length + 80, "%s.tmp-%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        destination, (unsigned long)id.Data1, (unsigned)id.Data2, (unsigned)id.Data3,
        (unsigned)id.Data4[0], (unsigned)id.Data4[1], (unsigned)id.Data4[2], (unsigned)id.Data4[3],
        (unsigned)id.Data4[4], (unsigned)id.Data4[5], (unsigned)id.Data4[6], (unsigned)id.Data4[7]);
    output = speech_open(temporary, "wbx");
#else
    size_t length = strlen(destination);
    int descriptor;
    temporary = (char *)malloc(length + 16);
    if (temporary == NULL) goto done;
    snprintf(temporary, length + 16, "%s.tmp-XXXXXX", destination);
    descriptor = mkstemp(temporary);
    if (descriptor >= 0) {
        output = fdopen(descriptor, "wb");
        if (output == NULL) close(descriptor);
    }
#endif
    input = speech_open(staging, "rb");
    if (input == NULL || output == NULL) goto done;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        total += count;
        if (total > SPEECH_VOICE_LIMIT || (cancelled != NULL && cancelled(context)) ||
            fwrite(buffer, 1, count, output) != count) goto done;
    }
    if (ferror(input) || total == 0 || fflush(output) != 0) goto done;
    if (fclose(output) != 0) { output = NULL; goto done; }
    output = NULL;
    if (cancelled != NULL && cancelled(context)) goto done;
#ifdef _WIN32
    {
        wchar_t *source = speech_wide(temporary), *target = speech_wide(destination);
        if (source != NULL && target != NULL) ok = MoveFileExW(source, target, MOVEFILE_WRITE_THROUGH) != 0;
        free(source); free(target);
    }
#else
    /* Same-directory link is atomic and refuses an existing destination. */
    ok = link(temporary, destination) == 0;
#endif
done:
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    speech_remove(temporary); free(temporary);
    if (!ok) speech_error(error, size, cancelled != NULL && cancelled(context)
        ? "voice creation cancelled" : "could not publish .zvoice atomically; its destination may already exist");
    return ok;
}

/* Canonical paths prevent lexical traversal and symlink/junction escapes. */
static char *speech_canonical(const char *path) {
#ifdef _WIN32
    wchar_t *wide = speech_wide(path), *resolved = NULL;
    HANDLE handle;
    DWORD count;
    char *result = NULL;
    if (wide == NULL) return NULL;
    handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    count = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (count != 0 && count < SPEECH_PATH_LIMIT) {
        resolved = (wchar_t *)calloc((size_t)count + 1, sizeof(*resolved));
        if (resolved != NULL && GetFinalPathNameByHandleW(handle, resolved,
                    count + 1, FILE_NAME_NORMALIZED) != 0) result = speech_utf8(resolved);
    }
    free(resolved); CloseHandle(handle);
    return result;
#else
    return realpath(path, NULL);
#endif
}

static int speech_inside(const char *root, const char *path) {
    size_t length = strlen(root);
    while (length != 0 && (root[length - 1] == '/' || root[length - 1] == '\\')) length--;
#ifdef _WIN32
    if (_strnicmp(root, path, length) != 0) return 0;
#else
    if (strncmp(root, path, length) != 0) return 0;
#endif
    return path[length] == '/' || path[length] == '\\';
}

static int speech_relative(const char *path) {
    const char *start = path, *cursor;
    if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\' ||
        strlen(path) >= SPEECH_PATH_LIMIT / 2) return 0;
    for (cursor = path;; cursor++) {
        if (*cursor == ':' || ((unsigned char)*cursor < 32 && *cursor != '\0')) return 0;
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t length = (size_t)(cursor - start);
            if (length == 0 || (length == 1 && start[0] == '.') ||
                (length == 2 && start[0] == '.' && start[1] == '.')) return 0;
            if (*cursor == '\0') break;
            start = cursor + 1;
        }
    }
    return 1;
}

static char *speech_project_path(ZSharpSpeechService *service, const char *relative,
                                  int output, char *error, size_t size) {
    char *joined = NULL, *canonical = NULL, *slash, *parent = NULL, *result = NULL;
    const char *leaf;
    if (!speech_relative(relative)) {
        speech_error(error, size, "Speech paths must be project-relative without '..', absolute paths, or empty segments");
        return NULL;
    }
    joined = speech_join(service->root, relative);
    if (joined == NULL) goto failed;
    canonical = speech_canonical(joined);
    if (!output) {
        if (canonical != NULL && speech_inside(service->root, canonical)) {
            result = canonical; canonical = NULL;
        }
        goto done;
    }
    if (canonical != NULL) {
        speech_error(error, size, "Speech.createVoice refuses to overwrite an existing .zvoice file");
        goto done;
    }
    slash = strrchr(joined, '/');
    {
        char *backslash = strrchr(joined, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    }
    if (slash == NULL) goto failed;
    leaf = slash + 1;
    *slash = '\0';
    parent = speech_canonical(joined);
    if (parent != NULL && (strcmp(service->root, parent) == 0 ||
                           speech_inside(service->root, parent))) result = speech_join(parent, leaf);
    goto done;
failed:
    speech_error(error, size, "could not resolve speech path");
done:
    if (result == NULL && (error == NULL || size == 0 || error[0] == '\0'))
        speech_error(error, size, "speech path is missing or resolves outside this project");
    free(joined); free(canonical); free(parent);
    return result;
}

static int speech_extension(const char *path, const char *extension) {
    size_t left = strlen(path), right = strlen(extension);
    if (left < right) return 0;
#ifdef _WIN32
    return _stricmp(path + left - right, extension) == 0;
#else
    return strcmp(path + left - right, extension) == 0;
#endif
}

static void speech_diagnostic(const char *log, char *error, size_t size,
                               const char *fallback) {
    FILE *file = speech_open(log, "rb");
    char buffer[768];
    size_t count = file == NULL ? 0 : fread(buffer, 1, sizeof(buffer) - 1, file);
    if (file != NULL) fclose(file);
    buffer[count] = '\0';
    /* Diagnostics only: never echo command arguments, input text, or tokens. */
    if (count != 0) {
        size_t i;
        for (i = 0; i < count; i++) if ((unsigned char)buffer[i] < 32) buffer[i] = ' ';
        if (error != NULL && size != 0) snprintf(error, size, "%s: %s", fallback, buffer);
    } else speech_error(error, size, fallback);
}

#ifdef _WIN32
/* Windows C runtime argv quoting. Every argument is quoted independently. */
static wchar_t *speech_command(const char *const *arguments, size_t count) {
    wchar_t *command = (wchar_t *)calloc(32768, sizeof(*command));
    size_t used = 0, argument;
    if (command == NULL) return NULL;
    for (argument = 0; argument < count; argument++) {
        wchar_t *wide = speech_wide(arguments[argument]);
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
    free(command);
    return NULL;
}
#endif

static int speech_process(const char *const *arguments, size_t count,
    unsigned int timeout_ms, ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t size) {
    char *directory = NULL, *log = NULL;
    uint64_t started;
    int ok = 0;
    speech_clear(error, size);
    if (count == 0 || count > 64 || timeout_ms < 1 || timeout_ms > 600000)
        return speech_error(error, size, "invalid speech helper arguments or timeout");
    if (cancelled != NULL && cancelled(context)) return speech_error(error, size, "speech operation cancelled");
    directory = speech_temp_directory(error, size);
    log = directory == NULL ? NULL : speech_join(directory, "process.log");
    if (log == NULL) goto done;
    started = speech_now();
#ifdef _WIN32
    {
        wchar_t *command = speech_command(arguments, count), *wide_log = speech_wide(log);
        wchar_t *executable = speech_wide(arguments[0]);
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        STARTUPINFOEXW startup;
        PROCESS_INFORMATION process;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE, job = NULL;
        HANDLE inherited[2];
        SIZE_T attribute_size = 0;
        int launched = 0, attributes_initialized = 0;
        memset(&startup, 0, sizeof(startup)); startup.StartupInfo.cb = sizeof(startup);
        memset(&process, 0, sizeof(process));
        memset(&limits, 0, sizeof(limits));
        if (command == NULL || executable == NULL || wide_log == NULL) goto windows_done;
        output = CreateFileW(wide_log, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        job = CreateJobObjectW(NULL, NULL);
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE || job == NULL ||
            !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto windows_done;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = output; startup.StartupInfo.hStdError = output;
        inherited[0] = input; inherited[1] = output;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
        startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
        if (startup.lpAttributeList == NULL ||
            !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) goto windows_done;
        attributes_initialized = 1;
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited, sizeof(inherited), NULL, NULL)) goto windows_done;
        if (!CreateProcessW(executable, command, NULL, NULL, TRUE,
                 CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                 &startup.StartupInfo, &process)) goto windows_done;
        launched = 1;
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            TerminateProcess(process.hProcess, 1);
            speech_error(error, size, "could not contain speech helper process safely");
        } else {
            ResumeThread(process.hThread);
            for (;;) {
                DWORD wait = WaitForSingleObject(process.hProcess, 10), exit_code = 1;
                LARGE_INTEGER log_size;
                int excessive_log = GetFileSizeEx(output, &log_size) && log_size.QuadPart > SPEECH_LOG_LIMIT;
                if (excessive_log) {
                    TerminateJobObject(job, 1);
                    speech_error(error, size, "speech helper exceeded its diagnostic output limit");
                    break;
                }
                if (wait == WAIT_OBJECT_0) {
                    GetExitCodeProcess(process.hProcess, &exit_code);
                    ok = exit_code == 0;
                    if (!ok) speech_diagnostic(log, error, size, "speech helper failed");
                    break;
                }
                if (wait == WAIT_FAILED || (cancelled != NULL && cancelled(context)) ||
                    speech_now() - started >= timeout_ms) {
                    TerminateJobObject(job, 1);
                    speech_error(error, size, wait == WAIT_FAILED ? "could not wait for speech helper" :
                        cancelled != NULL && cancelled(context) ? "speech operation cancelled" : "speech helper timed out");
                    break;
                }
            }
        }
        WaitForSingleObject(process.hProcess, 5000);
windows_done:
        if (!launched && (error == NULL || size == 0 || error[0] == '\0'))
            speech_error(error, size, "could not start speech helper; check the configured executable and dependencies");
        if (process.hThread != NULL) CloseHandle(process.hThread);
        if (process.hProcess != NULL) CloseHandle(process.hProcess);
        if (job != NULL) CloseHandle(job);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (startup.lpAttributeList != NULL) {
            if (attributes_initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            free(startup.lpAttributeList);
        }
        free(command); free(wide_log); free(executable);
    }
#else
    {
        char **argv = (char **)calloc(count + 1, sizeof(*argv));
        pid_t child;
        int status = 0, output;
        size_t index;
        if (argv == NULL) goto done;
        for (index = 0; index < count; index++) argv[index] = (char *)arguments[index];
        output = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0) { free(argv); goto done; }
        child = fork();
        if (child == 0) {
            int input;
            setpgid(0, 0);
            input = open("/dev/null", O_RDONLY);
            if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
            dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO); close(output);
            execvp(argv[0], argv);
            _exit(127);
        }
        close(output); free(argv);
        if (child < 0) { speech_error(error, size, "could not start speech helper"); goto done; }
        setpgid(child, child);
        for (;;) {
            siginfo_t information;
            int result;
            struct stat log_attributes;
            memset(&information, 0, sizeof(information));
            /* Keep the leader unreaped until its descendants are terminated;
             * otherwise PID/group reuse could target an unrelated process. */
            result = waitid(P_PID, (id_t)child, &information, WEXITED | WNOHANG | WNOWAIT);
            if (result < 0 && errno != EINTR) { speech_error(error, size, "could not wait for speech helper"); break; }
            if (stat(log, &log_attributes) == 0 && log_attributes.st_size > SPEECH_LOG_LIMIT) {
                kill(-child, SIGKILL); kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                speech_error(error, size, "speech helper exceeded its diagnostic output limit");
                break;
            }
            if (result == 0 && information.si_pid == child) {
                kill(-child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (!ok) speech_diagnostic(log, error, size, WIFEXITED(status) && WEXITSTATUS(status) == 127
                    ? "speech helper is not installed" : "speech helper failed");
                break;
            }
            if ((cancelled != NULL && cancelled(context)) || speech_now() - started >= timeout_ms) {
                kill(-child, SIGKILL); kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                speech_error(error, size, cancelled != NULL && cancelled(context)
                    ? "speech operation cancelled" : "speech helper timed out");
                break;
            }
            speech_sleep();
        }
    }
#endif
done:
    speech_remove(log); speech_remove_directory(directory);
    free(log); free(directory);
    if (!ok && (error == NULL || size == 0 || error[0] == '\0'))
        speech_error(error, size, "could not run speech helper");
    return ok;
}

int zsharp_speech_run_python(const char *python, const char *helper,
    const char *const *arguments, size_t count, unsigned int timeout_ms,
    ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t size) {
    const char **argv;
    size_t index;
    int ok;
    if (python == NULL || python[0] == '\0' || helper == NULL || helper[0] == '\0')
        return speech_error(error, size, "local speech models require the ZVM Python runtime and installed speech helper");
    if (count > 60 || (count != 0 && arguments == NULL))
        return speech_error(error, size, "invalid speech helper argument count");
    argv = (const char **)calloc(count + 3, sizeof(*argv));
    if (argv == NULL) return speech_error(error, size, "out of memory starting speech helper");
    argv[0] = python; argv[1] = "-I"; argv[2] = helper;
    for (index = 0; index < count; index++) {
        if (arguments[index] == NULL) { free(argv); return speech_error(error, size, "invalid speech helper argument"); }
        argv[index + 3] = arguments[index];
    }
    ok = speech_process(argv, count + 3, timeout_ms, cancelled, context, error, size);
    free(argv);
    return ok;
}

static int speech_json_text(JSContext *context, JSValueConst object,
    const char *key, const char *fallback, char **result) {
    JSValue field = JS_GetPropertyStr(context, object, key);
    const char *text = NULL;
    int ok = 0;
    if (JS_IsUndefined(field)) *result = speech_copy(fallback);
    else if (JS_IsString(field)) {
        size_t length;
        text = JS_ToCStringLen(context, &length, field);
        if (text != NULL && length <= 4096 && strlen(text) == length) *result = speech_copy(text);
    }
    if (text != NULL) JS_FreeCString(context, text);
    ok = *result != NULL;
    JS_FreeValue(context, field);
    return ok;
}

static int speech_json_number(JSContext *context, JSValueConst object,
    const char *key, double minimum, double maximum, double *result) {
    JSValue field = JS_GetPropertyStr(context, object, key);
    int ok;
    *result = 100;
    ok = JS_IsUndefined(field) || (JS_IsNumber(field) && JS_ToFloat64(context, result, field) == 0 &&
                                 isfinite(*result) && *result >= minimum && *result <= maximum);
    JS_FreeValue(context, field);
    return ok;
}

static void speech_voice_free(SpeechVoice *voice) {
    free(voice->path); free(voice->voice); memset(voice, 0, sizeof(*voice));
}

static int speech_voice_read(ZSharpSpeechService *service, const char *relative,
                             SpeechVoice *voice, char *error, size_t size) {
    char *data = NULL, *kind = NULL, *backend = NULL, *format = NULL;
    size_t length = 0;
    JSRuntime *runtime = NULL;
    JSContext *context = NULL;
    JSValue object = JS_UNDEFINED, version = JS_UNDEFINED;
    int ok = 0;
    double number = 1;
    memset(voice, 0, sizeof(*voice));
    if (relative == NULL || relative[0] == '\0') {
        voice->voice = speech_copy(""); voice->pitch = 100; voice->speed = 100; voice->volume = 100;
        return voice->voice != NULL ? 1 : speech_error(error, size, "out of memory selecting default voice");
    }
    if (!speech_extension(relative, ".zvoice"))
        return speech_error(error, size, "Speech.speak requires a project-relative .zvoice file");
    voice->path = speech_project_path(service, relative, 0, error, size);
    if (voice->path == NULL) goto done;
    data = speech_read(voice->path, SPEECH_VOICE_LIMIT, &length, error, size);
    if (data == NULL || strlen(data) != length) goto done;
    runtime = JS_NewRuntime();
    if (runtime == NULL) goto done;
    JS_SetMemoryLimit(runtime, 160u * 1024u * 1024u);
    JS_SetMaxStackSize(runtime, 512u * 1024u);
    context = JS_NewContext(runtime);
    if (context == NULL) goto done;
    object = JS_ParseJSON(context, data, length, voice->path);
    if (JS_IsException(object) || !JS_IsObject(object) || JS_IsArray(object)) goto done;
    version = JS_GetPropertyStr(context, object, "version");
    if (!JS_IsUndefined(version) && (!JS_IsNumber(version) || JS_ToFloat64(context, &number, version) != 0 || number != 1)) goto done;
    if (!speech_json_text(context, object, "format", "ZVOICE", &format) || strcmp(format, "ZVOICE") != 0 ||
        !speech_json_text(context, object, "type", "preset", &kind) ||
        !speech_json_text(context, object, "backend", "system", &backend) ||
        !speech_json_text(context, object, "voice", "", &voice->voice) ||
        !speech_json_number(context, object, "pitch", 50, 200, &voice->pitch) ||
        !speech_json_number(context, object, "speed", 25, 400, &voice->speed) ||
        !speech_json_number(context, object, "volume", 0, 100, &voice->volume)) goto done;
    if (strcmp(kind, "preset") == 0 && strcmp(backend, "system") == 0) ok = 1;
    else if (strcmp(kind, "clone") == 0 && strcmp(backend, "pocket-tts") == 0) { voice->cloned = 1; ok = 1; }
done:
    if (context != NULL) { JS_FreeValue(context, version); JS_FreeValue(context, object); JS_FreeContext(context); }
    if (runtime != NULL) JS_FreeRuntime(runtime);
    free(data); free(kind); free(backend); free(format);
    if (!ok) {
        speech_voice_free(voice);
        if (error == NULL || size == 0 || error[0] == '\0')
            speech_error(error, size, "invalid .zvoice JSON: use version 1, type preset/system or clone/pocket-tts; pitch 50..200, speed 25..400, volume 0..100");
    }
    return ok;
}

static void speech_lock(ZSharpSpeechService *service) {
#ifdef _WIN32
    EnterCriticalSection(&service->lock);
#else
    pthread_mutex_lock(&service->lock);
#endif
}
static void speech_unlock(ZSharpSpeechService *service) {
#ifdef _WIN32
    LeaveCriticalSection(&service->lock);
#else
    pthread_mutex_unlock(&service->lock);
#endif
}
static int speech_stopping(void *context) {
    ZSharpSpeechService *service = (ZSharpSpeechService *)context;
#ifdef _WIN32
    return InterlockedCompareExchange(&service->stopping, 0, 0) != 0;
#else
    return atomic_load(&service->stopping) != 0;
#endif
}

/* Preparation runs on the calling task while synthesis runs on the service
 * thread. Serialize only the clone worker, never the queue or VM room state. */
static int speech_voice_lock(ZSharpSpeechService *service,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t size) {
    uint64_t started = speech_now();
    for (;;) {
        if (speech_stopping(service) || (cancelled != NULL && cancelled(context)))
            return speech_error(error, size, "speech operation cancelled");
#ifdef _WIN32
        if (TryAcquireSRWLockExclusive(&service->voice_lock)) return 1;
#else
        if (pthread_mutex_trylock(&service->voice_lock) == 0) return 1;
#endif
        if (speech_now() - started >= SPEECH_MODEL_TIMEOUT)
            return speech_error(error, size, "timed out waiting for the cloned voice worker");
        speech_sleep();
    }
}

static void speech_voice_unlock(ZSharpSpeechService *service) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&service->voice_lock);
#else
    pthread_mutex_unlock(&service->voice_lock);
#endif
}

#ifdef _WIN32
static wchar_t *speech_xml(const char *text, double pitch) {
    wchar_t *wide = speech_wide(text), *xml;
    size_t length, index, used;
    int middle = (int)lround((pitch - 100) / 10);
    if (middle < -10) middle = -10;
    if (middle > 10) middle = 10;
    if (wide == NULL) return NULL;
    length = wcslen(wide);
    xml = (wchar_t *)calloc(length * 6 + 128, sizeof(*xml));
    if (xml == NULL) { free(wide); return NULL; }
    swprintf(xml, length * 6 + 128, L"<pitch absmiddle=\"%d\">", middle);
    used = wcslen(xml);
    for (index = 0; index < length; index++) {
        const wchar_t *escaped = wide[index] == L'&' ? L"&amp;" : wide[index] == L'<' ? L"&lt;" :
            wide[index] == L'>' ? L"&gt;" : NULL;
        if (escaped != NULL) { size_t count = wcslen(escaped); memcpy(xml + used, escaped, count * sizeof(*xml)); used += count; }
        else xml[used++] = wide[index];
    }
    memcpy(xml + used, L"</pitch>", sizeof(L"</pitch>"));
    free(wide);
    return xml;
}

static HRESULT speech_sapi_voice(ISpVoice *speaker, const char *name) {
    ISpObjectTokenCategory *category = NULL;
    IEnumSpObjectTokens *tokens = NULL;
    ISpObjectToken *token = NULL;
    wchar_t *wanted;
    HRESULT result;
    if (name[0] == '\0') return S_OK;
    wanted = speech_wide(name);
    if (wanted == NULL) return E_INVALIDARG;
    result = CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_ISpObjectTokenCategory, (void **)&category);
    if (SUCCEEDED(result)) result = ISpObjectTokenCategory_SetId(category, SPCAT_VOICES, FALSE);
    if (SUCCEEDED(result)) result = ISpObjectTokenCategory_EnumTokens(category, NULL, NULL, &tokens);
    if (SUCCEEDED(result)) {
        result = E_INVALIDARG;
        while (IEnumSpObjectTokens_Next(tokens, 1, &token, NULL) == S_OK) {
            wchar_t *description = NULL, *id = NULL;
            ISpObjectToken_GetStringValue(token, NULL, &description);
            ISpObjectToken_GetId(token, &id);
            if ((description != NULL && _wcsicmp(wanted, description) == 0) ||
                (id != NULL && _wcsicmp(wanted, id) == 0)) result = ISpVoice_SetVoice(speaker, token);
            CoTaskMemFree(description); CoTaskMemFree(id);
            ISpObjectToken_Release(token); token = NULL;
            if (SUCCEEDED(result)) break;
        }
    }
    if (tokens != NULL) IEnumSpObjectTokens_Release(tokens);
    if (category != NULL) ISpObjectTokenCategory_Release(category);
    free(wanted);
    return result;
}

static int speech_native(ZSharpSpeechService *service, const SpeechJob *job,
                          const char *wave_path, const char *text_path,
                          char *error, size_t size) {
    ISpVoice *speaker = NULL;
    ISpStream *stream = NULL;
    WAVEFORMATEX format;
    wchar_t *path = speech_wide(wave_path), *xml = speech_xml(job->text, job->voice.pitch);
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED), result = E_FAIL;
    uint64_t started = speech_now();
    int ok = 0, rate;
    (void)text_path;
    if (FAILED(initialized) || path == NULL || xml == NULL) goto done;
    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = 22050;
    format.wBitsPerSample = 16; format.nBlockAlign = 2; format.nAvgBytesPerSec = 44100;
    result = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_INPROC_SERVER, &IID_ISpVoice, (void **)&speaker);
    if (SUCCEEDED(result)) result = speech_sapi_voice(speaker, job->voice.voice);
    if (SUCCEEDED(result)) result = CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_INPROC_SERVER, &IID_ISpStream, (void **)&stream);
    if (SUCCEEDED(result)) result = ISpStream_BindToFile(stream, path, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &format, 0);
    if (SUCCEEDED(result)) result = ISpVoice_SetOutput(speaker, (IUnknown *)stream, TRUE);
    rate = (int)lround(log(job->voice.speed / 100.0) / log(2.0) * 5);
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    if (SUCCEEDED(result)) result = ISpVoice_SetRate(speaker, rate);
    if (SUCCEEDED(result)) result = ISpVoice_SetVolume(speaker, 100);
    if (SUCCEEDED(result)) result = ISpVoice_Speak(speaker, xml, SPF_ASYNC | SPF_IS_XML | SPF_PURGEBEFORESPEAK, NULL);
    if (SUCCEEDED(result)) {
        for (;;) {
            result = ISpVoice_WaitUntilDone(speaker, 25);
            if (result == S_OK) { ok = 1; break; }
            if (FAILED(result) || speech_stopping(service) || speech_now() - started >= 300000) {
                ISpVoice_Speak(speaker, NULL, SPF_PURGEBEFORESPEAK, NULL);
                break;
            }
        }
    }
done:
    if (speaker != NULL) ISpVoice_Release(speaker);
    if (stream != NULL) { ISpStream_Close(stream); ISpStream_Release(stream); }
    if (SUCCEEDED(initialized)) CoUninitialize();
    free(path); free(xml);
    if (!ok) {
        if (speech_stopping(service)) speech_error(error, size, "speech operation cancelled");
        else if (error != NULL && size != 0) snprintf(error, size,
            "Windows SAPI synthesis failed (0x%08lx); check that the requested system voice is installed", (unsigned long)result);
    }
    return ok;
}
#else
static int speech_native(ZSharpSpeechService *service, const SpeechJob *job,
                          const char *wave_path, const char *text_path,
                          char *error, size_t size) {
    const char *arguments[16];
    char rate[32], pitch[32];
    size_t count = 0;
#ifdef __APPLE__
    if (job->voice.pitch != 100)
        return speech_error(error, size, "system preset pitch adjustment is not supported by the macOS say backend; use pitch100 or a configured clone backend");
    arguments[count++] = "/usr/bin/say";
    arguments[count++] = "-o"; arguments[count++] = wave_path;
    arguments[count++] = "--file-format=WAVE";
    arguments[count++] = "--data-format=LEI16@22050";
    arguments[count++] = "-f"; arguments[count++] = text_path;
    snprintf(rate, sizeof(rate), "%d", (int)lround(175 * job->voice.speed / 100));
    arguments[count++] = "-r"; arguments[count++] = rate;
    if (job->voice.voice[0] != '\0') { arguments[count++] = "-v"; arguments[count++] = job->voice.voice; }
    (void)pitch;
    return speech_process(arguments, count, 300000, speech_stopping, service, error, size);
#else
    int ok;
    arguments[count++] = "espeak-ng";
    arguments[count++] = "-w"; arguments[count++] = wave_path;
    arguments[count++] = "-f"; arguments[count++] = text_path;
    snprintf(rate, sizeof(rate), "%d", (int)lround(175 * job->voice.speed / 100));
    snprintf(pitch, sizeof(pitch), "%d", (int)fmin(99, lround(job->voice.pitch / 2)));
    arguments[count++] = "-s"; arguments[count++] = rate;
    arguments[count++] = "-p"; arguments[count++] = pitch;
    if (job->voice.voice[0] != '\0') { arguments[count++] = "-v"; arguments[count++] = job->voice.voice; }
    ok = speech_process(arguments, count, 300000, speech_stopping, service, error, size);
    /* Only retry the legacy executable when espeak-ng was not present, rather
     * than repeating a cancelled or invalid-voice operation. */
    if (!ok && !speech_stopping(service) && error != NULL && strstr(error, "not installed") != NULL) {
        arguments[0] = "espeak";
        ok = speech_process(arguments, count, 300000, speech_stopping, service, error, size);
    }
    return ok;
#endif
}
#endif

static int speech_helper_result(const char *path, char *error, size_t size) {
    char *data = speech_read(path, 4096, NULL, error, size);
    int ok = data != NULL && strcmp(data, "OK\n") == 0;
    if (!ok && data != NULL) speech_error(error, size,
        strncmp(data, "ERROR\n", 6) == 0 ? data + 6 : "local speech helper returned an invalid result");
    free(data);
    return ok;
}

typedef struct SpeechStream {
    JSRuntime *runtime;
    JSContext *json;
    ZSharpAudioPlayback *playback;
    ZSharpSpeechCancelled cancelled;
    void *context;
    uint64_t samples;
    unsigned int rate;
    double gain;
    int prepare;
} SpeechStream;

static int speech_base64_digit(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    return value == '+' ? 62 : value == '/' ? 63 : -1;
}

static int speech_decode_pcm(const char *text, size_t length,
    int16_t *pcm, size_t *samples) {
    unsigned char bytes[65536];
    size_t i, used = 0;
    if (length == 0 || length % 4 != 0 || length > 87384) return 0;
    for (i = 0; i < length; i += 4) {
        int a = speech_base64_digit((unsigned char)text[i]);
        int b = speech_base64_digit((unsigned char)text[i + 1]);
        int c = text[i + 2] == '=' ? -2 : speech_base64_digit((unsigned char)text[i + 2]);
        int d = text[i + 3] == '=' ? -2 : speech_base64_digit((unsigned char)text[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) || ((c == -2 || d == -2) && i + 4 != length) ||
            (c == -2 && (b & 15)) || (d == -2 && c >= 0 && (c & 3))) return 0;
        if (used >= sizeof(bytes)) return 0;
        bytes[used++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) {
            if (used >= sizeof(bytes)) return 0;
            bytes[used++] = (unsigned char)((b << 4) | (c >> 2));
            if (d >= 0) {
                if (used >= sizeof(bytes)) return 0;
                bytes[used++] = (unsigned char)((c << 6) | d);
            }
        }
    }
    if (used == 0 || used % 2 != 0) return 0;
    for (i = 0; i < used / 2; i++)
        pcm[i] = (int16_t)((uint16_t)bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8));
    *samples = used / 2;
    return 1;
}

static int speech_frame_fields(JSContext *context, JSValue object, uint32_t expected) {
    JSPropertyEnum *fields = NULL;
    uint32_t count = 0, i;
    int ok = JS_GetOwnPropertyNames(context, &fields, &count, object,
        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 && count == expected;
    for (i = 0; i < count; i++) JS_FreeAtom(context, fields[i].atom);
    js_free(context, fields);
    return ok;
}

static int speech_stream_frame(const char *payload, size_t length, void *context,
    int *complete, char *error, size_t size) {
    SpeechStream *stream = (SpeechStream *)context;
    JSContext *json = stream->json;
    JSValue object = JS_ParseJSON(json, payload, length, "speech-worker-frame");
    JSValue ok = JS_UNDEFINED, pcm = JS_UNDEFINED, rate = JS_UNDEFINED, message = JS_UNDEFINED;
    const char *text = NULL;
    int result = 0;
    *complete = 0;
    if (JS_IsException(object) || !JS_IsObject(object) || JS_IsArray(object)) goto malformed;
    ok = JS_GetPropertyStr(json, object, "ok");
    if (!JS_IsUndefined(ok)) {
        if (!JS_IsBool(ok)) goto malformed;
        if (JS_ToBool(json, ok)) {
            if (!speech_frame_fields(json, object, 1) || (!stream->prepare && stream->samples == 0)) goto malformed;
            *complete = 1; result = 1; goto done;
        }
        message = JS_GetPropertyStr(json, object, "error");
        if (!JS_IsString(message) || !speech_frame_fields(json, object, 2)) goto malformed;
        {
            size_t message_size;
            text = JS_ToCStringLen(json, &message_size, message);
            if (text == NULL || message_size == 0 || strlen(text) != message_size) goto malformed;
            speech_error(error, size, text);
        }
        /* Pocket may still own generation threads after an exception. Do not
         * reuse a failed voice worker; the next explicit call can start fresh. */
        *complete = 0;
        goto done;
    }
    if (stream->prepare || !speech_frame_fields(json, object, 2)) goto malformed;
    pcm = JS_GetPropertyStr(json, object, "pcm");
    rate = JS_GetPropertyStr(json, object, "rate");
    if (!JS_IsString(pcm) || !JS_IsNumber(rate)) goto malformed;
    {
        double frequency;
        size_t encoded_size, count;
        int16_t samples[32768];
        if (JS_ToFloat64(json, &frequency, rate) != 0 || !isfinite(frequency) ||
            frequency != floor(frequency) || frequency < 8000 || frequency > 192000) goto malformed;
        if (stream->rate != 0 && stream->rate != (unsigned int)frequency) goto malformed;
        text = JS_ToCStringLen(json, &encoded_size, pcm);
        if (text == NULL || !speech_decode_pcm(text, encoded_size, samples, &count)) goto malformed;
        stream->rate = (unsigned int)frequency;
        if (count > (uint64_t)stream->rate * 300 - stream->samples) goto malformed;
        stream->samples += count;
        if (stream->playback == NULL) {
            stream->playback = zsharp_audio_playback_open(stream->rate, stream->gain,
                stream->cancelled, stream->context, error, size);
            if (stream->playback == NULL) goto done;
        }
        result = zsharp_audio_playback_append(stream->playback, samples, count, error, size);
    }
    goto done;
malformed:
    speech_error(error, size, "cloned voice worker returned an invalid or oversized PCM stream");
done:
    if (text != NULL) JS_FreeCString(json, text);
    JS_FreeValue(json, message); JS_FreeValue(json, rate); JS_FreeValue(json, pcm);
    JS_FreeValue(json, ok); JS_FreeValue(json, object);
    return result;
}

/* The helper stays alive between utterances. Playback starts on its first PCM
 * chunk; neither full synthesis nor a temporary WAV is on the response path. */
static int speech_stream_request(ZSharpSpeechService *service,
    const char *const *arguments, size_t count, double gain, int prepare,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t size) {
    SpeechStream stream = {0};
    int result = 0;
    if (!speech_voice_lock(service, cancelled, context, error, size)) return 0;
    stream.gain = gain; stream.prepare = prepare;
    stream.cancelled = cancelled; stream.context = context;
    stream.runtime = JS_NewRuntime();
    if (stream.runtime == NULL) goto done;
    JS_SetMemoryLimit(stream.runtime, 4u * 1024u * 1024u);
    JS_SetMaxStackSize(stream.runtime, 512u * 1024u);
    stream.json = JS_NewContext(stream.runtime);
    if (stream.json == NULL) goto done;
    result = zsharp_speech_worker_run_stream(&service->voice_worker,
        service->python, service->helper, arguments, count,
        speech_stream_frame, &stream, prepare ? SPEECH_MODEL_TIMEOUT : SPEECH_MODEL_TIMEOUT + 300000u,
        cancelled, context, error, size);
    if (result && stream.playback != NULL) {
        ZSharpAudioPlayback *playback = stream.playback;
        stream.playback = NULL;
        result = zsharp_audio_playback_finish(playback, error, size);
    }
done:
    if (stream.playback != NULL) zsharp_audio_playback_abort(stream.playback);
    if (stream.json != NULL) JS_FreeContext(stream.json);
    if (stream.runtime != NULL) JS_FreeRuntime(stream.runtime);
    speech_voice_unlock(service);
    if (!result && (error == NULL || size == 0 || error[0] == '\0'))
        speech_error(error, size, "out of memory creating the cloned voice stream");
    return result;
}

static void speech_job_free(SpeechJob *job) {
    if (job == NULL) return;
    speech_voice_free(&job->voice); free(job->text); free(job);
}

static int speech_job_run(ZSharpSpeechService *service, const SpeechJob *job,
                           char *error, size_t size) {
    char *directory = speech_temp_directory(error, size);
    char *wave = directory == NULL ? NULL : speech_join(directory, "speech.wav");
    char *text = directory == NULL ? NULL : speech_join(directory, "text.txt");
    char *result = directory == NULL ? NULL : speech_join(directory, "result.txt");
    int ok = 0;
    if (wave == NULL || text == NULL || result == NULL) goto done;
    if (!speech_write(text, job->text, error, size)) goto done;
    if (job->voice.cloned) {
        const char *arguments[] = {"synthesize-stream", job->voice.path, text};
        ok = speech_stream_request(service, arguments, 3, job->voice.volume / 100.0,
            0, speech_stopping, service, error, size);
    } else {
        ok = speech_native(service, job, wave, text, error, size);
        if (ok && !speech_stopping(service)) ok = zsharp_audio_play_wav(wave,
            job->voice.volume / 100.0, speech_stopping, service, error, size);
    }
done:
    speech_remove(wave); speech_remove(text); speech_remove(result);
    speech_remove_directory(directory);
    free(wave); free(text); free(result); free(directory);
    return ok;
}

#ifdef _WIN32
static DWORD WINAPI speech_worker(LPVOID context) {
#else
static void *speech_worker(void *context) {
#endif
    ZSharpSpeechService *service = (ZSharpSpeechService *)context;
    for (;;) {
        SpeechJob *job;
        char error[1024] = "";
        int ok;
        speech_lock(service);
        while (service->head == NULL && !speech_stopping(service)) {
#ifdef _WIN32
            SleepConditionVariableCS(&service->condition, &service->lock, INFINITE);
#else
            pthread_cond_wait(&service->condition, &service->lock);
#endif
        }
        if (speech_stopping(service)) { speech_unlock(service); break; }
        job = service->head; service->head = job->next;
        if (service->head == NULL) service->tail = NULL;
        service->queued--; service->busy = 1;
        speech_unlock(service);
        ok = speech_job_run(service, job, error, sizeof(error));
        speech_job_free(job);
        speech_lock(service);
        service->busy = 0;
        if (!ok && !speech_stopping(service) && service->failure[0] == '\0')
            snprintf(service->failure, sizeof(service->failure), "%s", error[0] == '\0' ? "speech synthesis failed" : error);
        speech_unlock(service);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

ZSharpSpeechService *zsharp_speech_create(const char *project_root,
    const char *python, const char *helper, char *error, size_t size) {
    ZSharpSpeechService *service;
    speech_clear(error, size);
    if (project_root == NULL || project_root[0] == '\0') {
        speech_error(error, size, "Speech requires a project root"); return NULL;
    }
    service = (ZSharpSpeechService *)calloc(1, sizeof(*service));
    if (service == NULL) { speech_error(error, size, "out of memory creating speech service"); return NULL; }
    service->root = speech_canonical(project_root);
    service->python = speech_copy(python == NULL ? "" : python);
    service->helper = speech_copy(helper == NULL ? "" : helper);
    if (service->root == NULL || service->python == NULL || service->helper == NULL) goto failed;
#ifdef _WIN32
    InitializeCriticalSection(&service->lock);
    InitializeSRWLock(&service->voice_lock);
    InitializeConditionVariable(&service->condition);
    service->thread = CreateThread(NULL, 0, speech_worker, service, 0, NULL);
    if (service->thread == NULL) { DeleteCriticalSection(&service->lock); goto failed; }
#else
    atomic_init(&service->stopping, 0);
    if (pthread_mutex_init(&service->lock, NULL) != 0) goto failed;
    if (pthread_mutex_init(&service->voice_lock, NULL) != 0) { pthread_mutex_destroy(&service->lock); goto failed; }
    if (pthread_cond_init(&service->condition, NULL) != 0) {
        pthread_mutex_destroy(&service->voice_lock); pthread_mutex_destroy(&service->lock); goto failed;
    }
    if (pthread_create(&service->thread, NULL, speech_worker, service) != 0) {
        pthread_cond_destroy(&service->condition); pthread_mutex_destroy(&service->voice_lock);
        pthread_mutex_destroy(&service->lock); goto failed;
    }
#endif
    return service;
failed:
    free(service->root); free(service->python); free(service->helper); free(service);
    speech_error(error, size, "could not create speech worker or resolve the project root");
    return NULL;
}

typedef struct SpeechCreateCancel {
    ZSharpSpeechService *service;
    ZSharpSpeechCancelled cancelled;
    void *context;
} SpeechCreateCancel;

static int speech_create_cancelled(void *context) {
    SpeechCreateCancel *cancel = (SpeechCreateCancel *)context;
    return speech_stopping(cancel->service) ||
        (cancel->cancelled != NULL && cancel->cancelled(cancel->context));
}

int zsharp_speech_prepare(ZSharpSpeechService *service, const char *relative,
    ZSharpSpeechCancelled cancelled, void *context, char *error, size_t size) {
    SpeechVoice voice;
    char *directory = NULL;
    SpeechCreateCancel cancel = {service, cancelled, context};
    int ok = 0;
    speech_clear(error, size);
    if (service == NULL || speech_create_cancelled(&cancel))
        return speech_error(error, size, "speech preparation cancelled or service unavailable");
    if (!speech_voice_read(service, relative, &voice, error, size)) return 0;
    if (!voice.cloned) ok = 1;
    else if (service->python[0] == '\0' || service->helper[0] == '\0')
        speech_error(error, size, "cloned voices require the ZVM Python runtime and local voice helper");
    else {
        directory = speech_temp_directory(error, size);
        if (directory != NULL) {
            const char *arguments[] = {"prepare", voice.path, directory};
            ok = speech_stream_request(service, arguments, 3, 0, 1,
                speech_create_cancelled, &cancel, error, size);
        }
    }
    speech_remove_directory(directory); free(directory);
    speech_voice_free(&voice);
    return ok;
}

int zsharp_speech_create_voice(ZSharpSpeechService *service,
    const char *reference, const char *output, ZSharpSpeechCancelled cancelled,
    void *context, char *error, size_t size) {
    char *input = NULL, *destination = NULL, *directory = NULL, *result = NULL, *staging = NULL;
    FILE *file = NULL;
    long length;
    int ok = 0;
    SpeechCreateCancel cancel = {service, cancelled, context};
    speech_clear(error, size);
    if (service == NULL) return speech_error(error, size, "speech service is unavailable");
    if (reference == NULL || output == NULL ||
        (!speech_extension(reference, ".mp3") && !speech_extension(reference, ".wav")) ||
        !speech_extension(output, ".zvoice"))
        return speech_error(error, size, "Speech.createVoice expects reference.mp3/reference.wav and output.zvoice");
    input = speech_project_path(service, reference, 0, error, size);
    if (input == NULL) goto done;
    destination = speech_project_path(service, output, 1, error, size);
    if (destination == NULL) goto done;
    file = speech_open(input, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (size_t)length > SPEECH_REFERENCE_LIMIT) {
        speech_error(error, size, "voice reference must be readable, nonempty, and at most 50 MiB"); goto done;
    }
    fclose(file); file = NULL;
    directory = speech_temp_directory(error, size);
    result = directory == NULL ? NULL : speech_join(directory, "result.txt");
    staging = directory == NULL ? NULL : speech_join(directory, "created.zvoice");
    if (result != NULL && staging != NULL) {
        const char *arguments[] = {"create", "--reference", input, "--output", staging, "--result", result};
        ok = zsharp_speech_run_python(service->python, service->helper, arguments,
            sizeof(arguments) / sizeof(arguments[0]), SPEECH_MODEL_TIMEOUT,
            speech_create_cancelled, &cancel, error, size);
        if (ok) ok = speech_helper_result(result, error, size);
        if (ok) ok = speech_publish_voice(staging, destination, speech_create_cancelled, &cancel, error, size);
    }
done:
    if (file != NULL) fclose(file);
    speech_remove(result); speech_remove(staging); speech_remove_directory(directory);
    free(input); free(destination); free(result); free(staging); free(directory);
    return ok;
}

int zsharp_speech_speak(ZSharpSpeechService *service, const char *text,
    const char *voice, char *error, size_t size) {
    SpeechJob *job;
    speech_clear(error, size);
    if (service == NULL || speech_stopping(service))
        return speech_error(error, size, "speech service is unavailable or shutting down");
    if (text == NULL || strlen(text) > SPEECH_TEXT_LIMIT)
        return speech_error(error, size, "Speech.speak accepts at most 16384 UTF-8 bytes per queued utterance");
    job = (SpeechJob *)calloc(1, sizeof(*job));
    if (job == NULL) return speech_error(error, size, "out of memory queuing speech");
    job->text = speech_copy(text);
    if (job->text == NULL || !speech_voice_read(service, voice, &job->voice, error, size)) {
        speech_job_free(job);
        if (error == NULL || size == 0 || error[0] == '\0') speech_error(error, size, "out of memory queuing speech");
        return 0;
    }
    if (job->voice.cloned && (service->python[0] == '\0' || service->helper[0] == '\0')) {
        speech_job_free(job); return speech_error(error, size, "cloned voices require the ZVM Python runtime and local voice helper");
    }
    if (text[0] == '\0') { speech_job_free(job); return 1; }
    speech_lock(service);
    if (service->queued >= SPEECH_QUEUE_LIMIT || speech_stopping(service)) {
        speech_unlock(service); speech_job_free(job);
        return speech_error(error, size, "speech queue is full or shutting down");
    }
    if (service->tail != NULL) service->tail->next = job;
    else service->head = job;
    service->tail = job; service->queued++;
#ifdef _WIN32
    WakeConditionVariable(&service->condition);
#else
    pthread_cond_signal(&service->condition);
#endif
    speech_unlock(service);
    return 1;
}

int zsharp_speech_poll(ZSharpSpeechService *service, int *active,
    char *error, size_t size) {
    int ok;
    speech_clear(error, size);
    if (active != NULL) *active = 0;
    if (service == NULL) return speech_error(error, size, "speech service is unavailable");
    speech_lock(service);
    if (active != NULL) *active = service->busy || service->head != NULL;
    ok = service->failure[0] == '\0';
    if (!ok) { speech_error(error, size, service->failure); service->failure[0] = '\0'; }
    speech_unlock(service);
    return ok;
}

void zsharp_speech_destroy(ZSharpSpeechService *service) {
    SpeechJob *job;
    if (service == NULL) return;
#ifdef _WIN32
    InterlockedExchange(&service->stopping, 1);
#else
    atomic_store(&service->stopping, 1);
#endif
    speech_lock(service);
#ifdef _WIN32
    WakeAllConditionVariable(&service->condition);
#else
    pthread_cond_broadcast(&service->condition);
#endif
    speech_unlock(service);
#ifdef _WIN32
    WaitForSingleObject(service->thread, INFINITE); CloseHandle(service->thread);
    DeleteCriticalSection(&service->lock);
#else
    pthread_join(service->thread, NULL);
    pthread_cond_destroy(&service->condition); pthread_mutex_destroy(&service->lock);
    pthread_mutex_destroy(&service->voice_lock);
#endif
    zsharp_speech_worker_destroy(service->voice_worker);
    job = service->head;
    while (job != NULL) { SpeechJob *next = job->next; speech_job_free(job); job = next; }
    free(service->root); free(service->python); free(service->helper); free(service);
}
