#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "speech_input.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

static unsigned int checks;
static const char *python, *helper;
static char output_path[4096];

#define CHECK(expression) do { checks++; if (!(expression)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); abort(); } } while (0)

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static void delay_ms(unsigned int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec wait = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000};
    nanosleep(&wait, NULL);
#endif
}

static int call(const char *mode, unsigned int timeout, char *error, size_t size) {
    const char *arguments[] = {"transcribe", mode, output_path};
    return zsharp_speech_input_run(python, helper, arguments, 3, timeout, NULL, NULL, error, size);
}

static unsigned long process_id(unsigned int *requests, char *contents, size_t capacity) {
    FILE *file = fopen(output_path, "rb");
    size_t length;
    unsigned long pid;
    CHECK(file != NULL);
    length = fread(contents, 1, capacity - 1, file); contents[length] = '\0'; fclose(file);
    CHECK(sscanf(contents, "%lu:%u:", &pid, requests) == 2);
    return pid;
}

static int timed_cancel(void *argument) {
    return now_ms() - *(uint64_t *)argument >= 60;
}

static void clear_output(void) {
    FILE *file = fopen(output_path, "wb"); CHECK(file != NULL); fclose(file);
}

static void await_started(void) {
    uint64_t started = now_ms();
    for (;;) {
        char bytes[8] = {0};
        FILE *file = fopen(output_path, "rb");
        if (file != NULL) { (void)fread(bytes, 1, 7, file); fclose(file); }
        if (strcmp(bytes, "began") == 0) { CHECK(1); return; }
        if (now_ms() - started >= 2000) { fprintf(stderr, "fixture request did not start\n"); abort(); }
        delay_ms(5);
    }
}

typedef struct ConcurrentCall { int result; char error[2048]; } ConcurrentCall;

typedef struct StreamFrames {
    unsigned int chunks;
    uint64_t first_frame;
    int reject_audio;
} StreamFrames;

static int stream_frame(const char *payload, size_t length, void *context,
    int *complete, char *error, size_t error_size) {
    StreamFrames *frames = (StreamFrames *)context;
    CHECK(strlen(payload) == length);
    if (strstr(payload, "\"pcm\"") != NULL && strstr(payload, "\"rate\": 24000") != NULL) {
        if (frames->chunks++ == 0) frames->first_frame = now_ms();
        if (frames->reject_audio) { snprintf(error, error_size, "fixture audio callback rejected a frame"); return 0; }
        return 1;
    }
    if (strcmp(payload, "{\"ok\": true}") == 0) { *complete = 1; return 1; }
    if (strstr(payload, "\"ok\": false") != NULL) {
        *complete = 1; snprintf(error, error_size, "fixture voice unavailable"); return 0;
    }
    snprintf(error, error_size, "fixture malformed stream response"); return 0;
}

static int stream_call(ZSharpSpeechWorker **worker, const char *mode,
    StreamFrames *frames, unsigned int timeout, char *error, size_t error_size) {
    const char *arguments[] = {"synthesize-stream", mode, output_path};
    return zsharp_speech_worker_run_stream(worker, python, helper, arguments, 3,
        stream_frame, frames, timeout, NULL, NULL, error, error_size);
}

static void stream_tests(void) {
    ZSharpSpeechWorker *one = NULL, *two = NULL;
    StreamFrames frames = {0, 0, 0};
    unsigned int requests;
    unsigned long first, second;
    char error[2048], contents[8192];
    uint64_t started, finished;
    {
        const char *prepare[] = {"prepare", "fixture.zvoice"};
        CHECK(zsharp_speech_worker_run_stream(&one, python, helper, prepare, 2, stream_frame, &frames,
            3000, NULL, NULL, error, sizeof(error)));
        CHECK(frames.chunks == 0);
    }
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(frames.chunks == 2);
    first = process_id(&requests, contents, sizeof(contents)); CHECK(requests == 2);
    frames.chunks = 0;
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == first); CHECK(requests == 3);
    CHECK(!stream_call(&one, "stream-error", &frames, 3000, error, sizeof(error)));
    CHECK(one != NULL); CHECK(strstr(error, "fixture voice unavailable") != NULL);
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == first); CHECK(requests == 5);
    CHECK(stream_call(&two, "stream-ok", &frames, 3000, error, sizeof(error)));
    second = process_id(&requests, contents, sizeof(contents)); CHECK(first != second); CHECK(requests == 1);
    zsharp_speech_input_shutdown();
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == first);
    frames.chunks = 0; started = now_ms();
    CHECK(stream_call(&one, "stream-delay", &frames, 3000, error, sizeof(error)));
    finished = now_ms(); CHECK(frames.chunks == 2);
    CHECK(frames.first_frame - started < finished - started - 200);
    CHECK(!stream_call(&one, "stream-oversize", &frames, 3000, error, sizeof(error)));
    CHECK(one == NULL); CHECK(strstr(error, "protocol limit") != NULL);
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(!stream_call(&one, "stream-bad-utf8", &frames, 3000, error, sizeof(error)));
    CHECK(one == NULL); CHECK(strstr(error, "UTF-8") != NULL);
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(!stream_call(&one, "stream-malformed", &frames, 3000, error, sizeof(error)));
    CHECK(one == NULL); CHECK(strstr(error, "malformed") != NULL);
    frames.chunks = 0;
    CHECK(!stream_call(&one, "stream-die", &frames, 3000, error, sizeof(error)));
    CHECK(one == NULL); CHECK(frames.chunks == 1);
    /* Measure the stalled stream, not cold CPython process startup. Heavy
     * parallel build/test load can legitimately exceed the 120 ms deadline. */
    frames.chunks = 0;
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(frames.chunks == 2);
    frames.chunks = 0;
    CHECK(!stream_call(&one, "stream-timeout", &frames, 120, error, sizeof(error)));
    CHECK(one == NULL); CHECK(frames.chunks == 1); CHECK(strstr(error, "timed out") != NULL);
    frames.reject_audio = 1;
    CHECK(!stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(one == NULL); CHECK(strstr(error, "callback rejected") != NULL);
    frames.reject_audio = 0;
    CHECK(stream_call(&one, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) != second); CHECK(requests == 1);
    CHECK(stream_call(&two, "stream-ok", &frames, 3000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == second); CHECK(requests == 2);
    zsharp_speech_worker_destroy(one); zsharp_speech_worker_destroy(two);
}
#ifdef _WIN32
static DWORD WINAPI concurrent_request(LPVOID argument) {
#else
static void *concurrent_request(void *argument) {
#endif
    ConcurrentCall *request = (ConcurrentCall *)argument;
    request->result = call("delay", 3000, request->error, sizeof(request->error));
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char **argv) {
    char error[2048], contents[8192];
    const char *preflight[] = {"preflight"};
    const char *invalid[] = {"unknown"};
    const char *cancel_arguments[] = {"transcribe", "sleep", output_path};
    unsigned long first, next;
    unsigned int requests;
    uint64_t started;
    CHECK(argc == 3); python = argv[1]; helper = argv[2];
#ifdef _WIN32
    { char base[MAX_PATH + 1]; CHECK(GetTempPathA(MAX_PATH, base) > 0); CHECK(GetTempFileNameA(base, "zsi", 0, output_path) != 0); }
#else
    { int descriptor; snprintf(output_path, sizeof(output_path), "/tmp/zsharp-stt-test-XXXXXX"); descriptor = mkstemp(output_path); CHECK(descriptor >= 0); close(descriptor); }
#endif
    CHECK(!zsharp_speech_input_run(NULL, helper, preflight, 1, 2000, NULL, NULL, error, sizeof(error)));
    CHECK(strstr(error, "invalid") != NULL);
    CHECK(!zsharp_speech_input_run(python, helper, invalid, 1, 2000, NULL, NULL, error, sizeof(error)));
    CHECK(!zsharp_speech_input_run(python, helper, preflight, 1, 0, NULL, NULL, error, sizeof(error)));
    CHECK(zsharp_speech_input_run(python, helper, preflight, 1, 3000, NULL, NULL, error, sizeof(error)));
    CHECK(call("ok", 2000, error, sizeof(error)));
    first = process_id(&requests, contents, sizeof(contents)); CHECK(requests == 2);
    CHECK(call("echo:\"quoted\"\\\xe8\xb7\xaf\xe5\xbe\x84\\", 2000, error, sizeof(error)));
    next = process_id(&requests, contents, sizeof(contents)); CHECK(first == next); CHECK(requests == 3);
    CHECK(strstr(contents, "\\u8def\\u5f84") != NULL);
    CHECK(strstr(contents, "\\\"quoted\\\"") != NULL);
    CHECK(!call("structured-error", 2000, error, sizeof(error)));
    CHECK(strstr(error, "fixture model unavailable") != NULL);
    CHECK(strstr(error, "\xc3\xa9") != NULL);
    CHECK(strstr(error, "\xf0\x9f\x94\x8a") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == first); CHECK(requests == 5);
    CHECK(!call("structured-stderr", 2000, error, sizeof(error)));
    CHECK(strstr(error, "fixture native model loader diagnostic") != NULL);
    CHECK(!call("structured-error", 2000, error, sizeof(error)));
    CHECK(strstr(error, "fixture native model loader diagnostic") == NULL);
    CHECK(!zsharp_speech_input_run(python, "different-helper.py", preflight, 1, 2000, NULL, NULL, error, sizeof(error)));
    CHECK(strstr(error, "configuration changed") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) == first);

    /* A caller waiting on another request can time out without interrupting or
     * replaying that request. */
    {
        ConcurrentCall concurrent;
        clear_output();
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL, 0, concurrent_request, &concurrent, 0, NULL);
        CHECK(thread != NULL);
#else
        pthread_t thread; CHECK(pthread_create(&thread, NULL, concurrent_request, &concurrent) == 0);
#endif
        await_started();
        CHECK(!zsharp_speech_input_run(python, helper, preflight, 1, 30, NULL, NULL, error, sizeof(error)));
        CHECK(strstr(error, "waiting for another request") != NULL);
#ifdef _WIN32
        CHECK(WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0); CloseHandle(thread);
#else
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(concurrent.result);
        CHECK(process_id(&requests, contents, sizeof(contents)) == first);
    }
    CHECK(!call("die", 2000, error, sizeof(error)));
    CHECK(strstr(error, "worker") != NULL);
    CHECK(strstr(error, "fixture child crashed") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) != first); CHECK(requests == 1);
    CHECK(!call("malformed", 2000, error, sizeof(error)));
    CHECK(strstr(error, "malformed") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(!call("oversize", 2000, error, sizeof(error)));
    CHECK(strstr(error, "protocol limit") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(!call("partial", 2000, error, sizeof(error)));
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(!call("unsolicited", 2000, error, sizeof(error)));
    CHECK(strstr(error, "malformed") != NULL || strstr(error, "unsolicited") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    started = now_ms();
    CHECK(!call("sleep", 100, error, sizeof(error)));
    CHECK(strstr(error, "timed out") != NULL); CHECK(now_ms() - started < 1500);
    CHECK(call("ok", 2000, error, sizeof(error)));
    first = process_id(&requests, contents, sizeof(contents)); CHECK(requests == 1);
    started = now_ms();
    CHECK(!zsharp_speech_input_run(python, helper, cancel_arguments, 3, 2000, timed_cancel, &started, error, sizeof(error)));
    CHECK(strstr(error, "cancelled") != NULL); CHECK(now_ms() - started < 1500);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) != first); CHECK(requests == 1);
    CHECK(!call("stderr", 3000, error, sizeof(error)));
    CHECK(strstr(error, "diagnostic output limit") != NULL || strstr(error, "worker") != NULL);
    CHECK(call("ok", 2000, error, sizeof(error)));
    first = process_id(&requests, contents, sizeof(contents));
    started = now_ms(); zsharp_speech_input_shutdown(); CHECK(now_ms() - started < 1500);
    CHECK(call("ok", 2000, error, sizeof(error)));
    CHECK(process_id(&requests, contents, sizeof(contents)) != first); CHECK(requests == 1);
    /* Shutdown interrupts an active RPC without freeing storage still used by
     * its caller, and a later request can start a clean worker. */
    {
        ConcurrentCall concurrent;
        clear_output();
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL, 0, concurrent_request, &concurrent, 0, NULL);
        CHECK(thread != NULL);
#else
        pthread_t thread; CHECK(pthread_create(&thread, NULL, concurrent_request, &concurrent) == 0);
#endif
        await_started(); started = now_ms(); zsharp_speech_input_shutdown();
        CHECK(now_ms() - started < 1500);
#ifdef _WIN32
        CHECK(WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0); CloseHandle(thread);
#else
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(!concurrent.result); CHECK(strstr(concurrent.error, "cancelled") != NULL);
        CHECK(call("ok", 2000, error, sizeof(error)));
        (void)process_id(&requests, contents, sizeof(contents)); CHECK(requests == 1);
    }
    zsharp_speech_input_shutdown();
    stream_tests();
    CHECK(remove(output_path) == 0);
    printf("speech input worker: %u checks passed (no microphone, models, or playback)\n", checks);
    return 0;
}
