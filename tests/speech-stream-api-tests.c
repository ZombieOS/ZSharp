#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "speech_api.h"
#include "audio_api.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static unsigned int checks, failures;
static char project[4096], events_path[4096];
static const char *python, *helper;
static unsigned int opened, appended, finished, aborted, live, wave_called;
static int reject_open, reject_append, reject_finish, check_samples, early_audio;
static uint64_t total_samples;
#ifdef _WIN32
static volatile LONG append_observed;
static SRWLOCK check_lock = SRWLOCK_INIT;
#else
static atomic_int append_observed;
static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

struct ZSharpAudioPlayback {
    ZSharpAudioCancelled cancelled;
    void *context;
};

static void check(int result, const char *name) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&check_lock);
#else
    pthread_mutex_lock(&check_lock);
#endif
    checks++;
    if (!result) { failures++; fprintf(stderr, "FAIL: %s\n", name); }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&check_lock);
#else
    pthread_mutex_unlock(&check_lock);
#endif
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static void pause_short(void) {
#ifdef _WIN32
    Sleep(5);
#else
    struct timespec delay = {0, 5000000}; nanosleep(&delay, NULL);
#endif
}

static char *read_events(void) {
    static char contents[65536];
    FILE *file = fopen(events_path, "rb");
    size_t count = file == NULL ? 0 : fread(contents, 1, sizeof(contents) - 1, file);
    if (file != NULL) fclose(file);
    contents[count] = '\0'; return contents;
}

static void reset_mock(void) {
    FILE *file;
    check(live == 0, "previous playback handles were consumed");
    opened = appended = finished = aborted = wave_called = 0;
    reject_open = reject_append = reject_finish = check_samples = early_audio = 0;
    total_samples = 0;
#ifdef _WIN32
    InterlockedExchange(&append_observed, 0);
#else
    atomic_store(&append_observed, 0);
#endif
    file = fopen(events_path, "wb"); if (file != NULL) fclose(file);
}

int zsharp_audio_play_wav(const char *path, double gain, ZSharpAudioCancelled cancelled,
    void *context, char *error, size_t size) {
    (void)path; (void)gain; (void)cancelled; (void)context;
    wave_called++; snprintf(error, size, "unexpected complete-WAV path"); return 0;
}

ZSharpAudioPlayback *zsharp_audio_playback_open(unsigned int rate, double gain,
    ZSharpAudioCancelled cancelled, void *context, char *error, size_t size) {
    ZSharpAudioPlayback *playback;
    opened++;
    check(rate >= 8000 && rate <= 192000, "playback receives validated sample rate");
    check(gain > 0.419 && gain < 0.421, "clone volume reaches PCM playback");
    if (reject_open) { snprintf(error, size, "fixture output device unavailable"); return NULL; }
    playback = (ZSharpAudioPlayback *)calloc(1, sizeof(*playback));
    if (playback == NULL) return NULL;
    playback->cancelled = cancelled; playback->context = context; live++;
    return playback;
}

int zsharp_audio_playback_append(ZSharpAudioPlayback *playback, const int16_t *samples,
    size_t count, char *error, size_t size) {
    appended++; total_samples += count;
    if (check_samples) {
        if (appended == 1) {
            check(count == 3 && samples[0] == 1 && samples[1] == 2 && samples[2] == 3,
                "first streamed PCM bytes decoded little-endian");
            early_audio = strstr(read_events(), "\"event\": \"generated\"") == NULL;
        } else {
            check(count == 3 && samples[0] == -1 && samples[1] == 1000 && samples[2] == -32768,
                "later signed PCM decoded without WAV buffering");
        }
    }
#ifdef _WIN32
    InterlockedExchange(&append_observed, 1);
#else
    atomic_store(&append_observed, 1);
#endif
    if (reject_append) { snprintf(error, size, "fixture append failed"); return 0; }
    if (playback->cancelled != NULL && playback->cancelled(playback->context)) {
        snprintf(error, size, "fixture playback cancelled"); return 0;
    }
    return 1;
}

int zsharp_audio_playback_finish(ZSharpAudioPlayback *playback, char *error, size_t size) {
    finished++; live--; free(playback);
    if (reject_finish) { snprintf(error, size, "fixture drain failed"); return 0; }
    return 1;
}

void zsharp_audio_playback_abort(ZSharpAudioPlayback *playback) {
    aborted++; live--; free(playback);
}

static void voice_fixture(const char *mode) {
    char path[4096]; FILE *file;
    snprintf(path, sizeof(path), "%s/clone.zvoice", project);
    file = fopen(path, "wb");
    if (file == NULL) { fprintf(stderr, "could not write voice fixture\n"); exit(2); }
    fprintf(file, "{\"format\":\"ZVOICE\",\"version\":1,\"type\":\"clone\",\"backend\":\"pocket-tts\",\"volume\":42,\"fixtureMode\":\"%s\"}", mode);
    fclose(file);
}

static ZSharpSpeechService *service_create(void) {
    char error[2048];
    ZSharpSpeechService *service = zsharp_speech_create(project, python, helper, error, sizeof(error));
    check(service != NULL, "create isolated streaming service");
    if (service == NULL) { fprintf(stderr, "%s\n", error); exit(2); }
    return service;
}

static int await_idle(ZSharpSpeechService *service, char *error, size_t size) {
    uint64_t started = now_ms();
    int active = 1, result = 1;
    while (now_ms() - started < 12000) {
        if (!zsharp_speech_poll(service, &active, error, size)) result = 0;
        if (!active) return result;
        pause_short();
    }
    snprintf(error, size, "fixture worker did not finish within 12 seconds"); return 0;
}

static unsigned long event_pid(void) {
    const char *pid = strstr(read_events(), "\"pid\": ");
    unsigned long value = 0;
    check(pid != NULL && sscanf(pid + 7, "%lu", &value) == 1, "fixture records worker PID");
    return value;
}

static void check_worker_stopped(unsigned long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    check(process == NULL || WaitForSingleObject(process, 1000) == WAIT_OBJECT_0,
        "owned model process exits after service destruction");
    if (process != NULL) CloseHandle(process);
#else
    check(kill((pid_t)pid, 0) != 0 && errno == ESRCH, "owned model process exits and is reaped");
#endif
}

static void check_workspaces_clean(void) {
    char path[4096], directory[4096];
    FILE *file;
    snprintf(path, sizeof(path), "%s/workspaces.txt", project);
    file = fopen(path, "rb");
    check(file != NULL, "fixture records owned helper workspaces");
    if (file == NULL) return;
    while (fgets(directory, sizeof(directory), file) != NULL) {
        size_t length = strlen(directory);
        while (length != 0 && (directory[length - 1] == '\n' || directory[length - 1] == '\r')) directory[--length] = '\0';
#ifdef _WIN32
        check(GetFileAttributesA(directory) == INVALID_FILE_ATTRIBUTES,
            "native service removed owned workspace after success/error/cancellation");
#else
        { struct stat status; check(lstat(directory, &status) != 0 && errno == ENOENT,
            "native service removed owned workspace after success/error/cancellation"); }
#endif
    }
    fclose(file); remove(path);
}

static int cancel_after(void *context) { return now_ms() - *(uint64_t *)context >= 80; }

int main(int argc, char **argv) {
    ZSharpSpeechService *service;
    char path[4096], error[2048];
    unsigned long first_pid;
    uint64_t started;
    size_t index;
    const char *invalid[] = {"invalid-base64", "noncanonical-base64", "odd-pcm", "empty-pcm",
        "oversized-pcm", "string-pcm", "missing-rate", "unknown-field", "rate-low", "rate-high",
        "rate-fraction", "rate-bool", "rate-text", "rate-null", "rate-change", "too-long", "empty-success"};
    if (argc != 3) { fprintf(stderr, "expected Python executable and fixture helper\n"); return 2; }
    python = argv[1]; helper = argv[2];
#ifdef _WIN32
    { char base[MAX_PATH + 1]; GetTempPathA(MAX_PATH, base); GetTempFileNameA(base, "zvs", 0, project);
      DeleteFileA(project); if (!CreateDirectoryA(project, NULL)) return 2; }
#else
    snprintf(project, sizeof(project), "/tmp/zsharp-stream-test-XXXXXX"); if (mkdtemp(project) == NULL) return 2;
#endif
    snprintf(events_path, sizeof(events_path), "%s/events.jsonl", project);
    reset_mock(); voice_fixture("valid"); service = service_create();
    check(zsharp_speech_prepare(service, "clone.zvoice", NULL, NULL, error, sizeof(error)), "prepare cloned voice without PCM playback");
    check(opened == 0 && live == 0, "prepare never opens an output device");
    first_pid = event_pid();
    check_samples = 1;
    check(zsharp_speech_speak(service, "delayed", "clone.zvoice", error, sizeof(error)), "queue streamed cloned utterance");
    check(await_idle(service, error, sizeof(error)), "finish first streamed utterance");
    if (error[0] != '\0') fprintf(stderr, "stream diagnostic: %s\n", error);
    check(early_audio, "first PCM chunk is delivered before all audio is generated");
    check(opened == 1 && appended == 2 && finished == 1 && aborted == 0 && live == 0 && wave_called == 0,
        "stream uses incremental playback and consumes handle exactly once");
    check(event_pid() == first_pid, "prepare and first utterance reuse model process");
    check_samples = 0;
    check(zsharp_speech_speak(service, "valid", "clone.zvoice", error, sizeof(error)), "queue second cloned utterance");
    check(await_idle(service, error, sizeof(error)), "finish second streamed utterance");
    check(strstr(read_events(), "\"request\": 3") != NULL && event_pid() == first_pid,
        "prepare plus two utterances use one persistent worker");
    zsharp_speech_destroy(service); check_worker_stopped(first_pid);

    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        reset_mock(); voice_fixture("valid"); service = service_create();
        check(zsharp_speech_speak(service, invalid[index], "clone.zvoice", error, sizeof(error)), "queue invalid-protocol fixture");
        check(!await_idle(service, error, sizeof(error)), invalid[index]);
        check(strstr(error, "invalid") != NULL || strstr(error, "oversized") != NULL, "invalid stream reports strict PCM diagnostic");
        check(finished == 0 && live == 0, "invalid stream never drains or leaks playback");
        if (opened != 0) check(aborted == 1, "partial invalid stream aborts its playback handle");
        zsharp_speech_destroy(service);
    }
    {
        const char *errors[] = {"error-before", "error-after", "die-after", "open-error", "append-error", "finish-error"};
        for (index = 0; index < sizeof(errors) / sizeof(errors[0]); index++) {
            reset_mock(); voice_fixture("valid"); service = service_create();
            reject_open = strcmp(errors[index], "open-error") == 0;
            reject_append = strcmp(errors[index], "append-error") == 0;
            reject_finish = strcmp(errors[index], "finish-error") == 0;
            check(zsharp_speech_speak(service, errors[index], "clone.zvoice", error, sizeof(error)), "queue controlled synthesis/playback failure");
            check(!await_idle(service, error, sizeof(error)), errors[index]);
            check(error[0] != '\0', "failed synthesis/playback produces an error");
            check(live == 0 && finished + aborted <= 1, "failure consumes opened playback exactly once");
            first_pid = event_pid();
            reset_mock();
            check(zsharp_speech_speak(service, "valid", "clone.zvoice", error, sizeof(error)), "queue recovery after failed synthesis/playback");
            check(await_idle(service, error, sizeof(error)), "service recovers without replaying failed utterance");
            if (strcmp(errors[index], "finish-error") != 0)
                check(event_pid() != first_pid, "partial model/playback failure discards potentially busy helper");
            zsharp_speech_destroy(service);
        }
    }
    reset_mock(); voice_fixture("prepare-pcm"); service = service_create();
    check(!zsharp_speech_prepare(service, "clone.zvoice", NULL, NULL, error, sizeof(error)), "prepare rejects unexpected PCM");
    check(opened == 0 && live == 0, "invalid prepare never starts playback"); zsharp_speech_destroy(service);
    reset_mock(); voice_fixture("prepare-sleep"); service = service_create(); started = now_ms();
    check(!zsharp_speech_prepare(service, "clone.zvoice", cancel_after, &started, error, sizeof(error)), "cancel preparation while model request is active");
    check(strstr(error, "cancelled") != NULL && now_ms() - started < 2000, "prepare cancellation returns promptly");
    zsharp_speech_destroy(service);
    reset_mock(); voice_fixture("valid"); service = service_create();
    check(zsharp_speech_speak(service, "hang-after", "clone.zvoice", error, sizeof(error)), "start partial stream for shutdown test");
    started = now_ms();
    while (now_ms() - started < 3000) {
#ifdef _WIN32
        if (InterlockedCompareExchange(&append_observed, 0, 0)) break;
#else
        if (atomic_load(&append_observed)) break;
#endif
        pause_short();
    }
    check(appended == 1, "shutdown fixture delivered initial PCM"); first_pid = event_pid();
    started = now_ms(); zsharp_speech_destroy(service);
    check(now_ms() - started < 2000, "shutdown cancels pending synthesis promptly");
    check(live == 0 && aborted == 1 && finished == 0, "shutdown aborts partial playback without leaking");
    check_worker_stopped(first_pid);
    check_workspaces_clean();
    snprintf(path, sizeof(path), "%s/clone.zvoice", project); remove(path); remove(events_path);
#ifdef _WIN32
    RemoveDirectoryA(project);
#else
    rmdir(project);
#endif
    printf("Speech streaming API: %u checks, %u failures (mock output, no models/audio hardware).\n", checks, failures);
    return failures != 0;
}
