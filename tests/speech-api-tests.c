#define _POSIX_C_SOURCE 200809L
#define _CRT_SECURE_NO_WARNINGS
#include "speech_api.h"
#include "audio_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* No SDL/audio hardware is linked: synthesis is checked as a WAV file only. */
static int wave_seen, wave_valid;
static const char *wave_copy;
int zsharp_audio_play_wav(const char *path, double gain,
    int (*cancelled)(void *), void *context, char *error, size_t size) {
    unsigned char header[44];
    FILE *file = fopen(path, "rb");
    long length = 0;
    (void)error; (void)size;
    wave_seen++;
    if (file != NULL) {
        size_t count = fread(header, 1, sizeof(header), file);
        fseek(file, 0, SEEK_END); length = ftell(file);
        wave_valid = count == sizeof(header) && length > 44 &&
            memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0 && gain >= 0 && gain <= 1;
        if (wave_valid && wave_copy != NULL) {
            FILE *copy = fopen(wave_copy, "wbx");
            if (copy != NULL) {
                unsigned char bytes[8192];
                rewind(file);
                while ((count = fread(bytes, 1, sizeof(bytes), file)) != 0) fwrite(bytes, 1, count, copy);
                fclose(copy);
            }
        }
        fclose(file);
    }
    return wave_valid && !(cancelled != NULL && cancelled(context));
}

/* This native-preset suite never streams a cloned voice. Keep all newly linked
 * streaming symbols hardware-free; the dedicated stream suite supplies mocks. */
ZSharpAudioPlayback *zsharp_audio_playback_open(unsigned int rate, double gain,
    ZSharpAudioCancelled cancelled, void *context, char *error, size_t size) {
    (void)rate; (void)gain; (void)cancelled; (void)context;
    snprintf(error, size, "cloned playback is not enabled in the native-preset fixture");
    return NULL;
}
int zsharp_audio_playback_append(ZSharpAudioPlayback *playback, const int16_t *pcm,
    size_t count, char *error, size_t size) {
    (void)playback; (void)pcm; (void)count; (void)error; (void)size; return 0;
}
int zsharp_audio_playback_finish(ZSharpAudioPlayback *playback, char *error, size_t size) {
    (void)playback; (void)error; (void)size; return 0;
}
void zsharp_audio_playback_abort(ZSharpAudioPlayback *playback) {(void)playback;}

static int checks, failures;
static void check(int condition, const char *name) {
    checks++;
    if (!condition) { failures++; fprintf(stderr, "FAIL: %s\n", name); }
}
static void pause_short(void) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
#endif
}
static void fixture(const char *root, const char *name, const char *text) {
    char path[4096]; FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    file = fopen(path, "wb");
    if (file == NULL) { fprintf(stderr, "could not write test fixture\n"); exit(2); }
    fwrite(text, 1, strlen(text), file); fclose(file);
}
static int always_cancel(void *context) { (void)context; return 1; }

int main(int argc, char **argv) {
    char root[4096], path[4096], error[2048] = "";
    ZSharpSpeechService *service;
    int active = 1, ok;
    unsigned int attempts;
    const char *fixtures[] = {"normal.zvoice", "bad.zvoice", "reference.wav", "exists.zvoice", "runner.py", "result.txt"};
    size_t index;
    wave_copy = argc > 3 ? argv[3] : NULL;
#ifdef _WIN32
    wchar_t temporary[MAX_PATH], directory[MAX_PATH];
    GetTempPathW(MAX_PATH, temporary); GetTempFileNameW(temporary, L"zst", 0, directory);
    DeleteFileW(directory); CreateDirectoryW(directory, NULL);
    WideCharToMultiByte(CP_UTF8, 0, directory, -1, root, sizeof(root), NULL, NULL);
#else
    snprintf(root, sizeof(root), "/tmp/zsharp-speech-test-XXXXXX");
    if (mkdtemp(root) == NULL) return 2;
#endif
    service = zsharp_speech_create(root, argc > 1 ? argv[1] : NULL,
        argc > 2 ? argv[2] : NULL, error, sizeof(error));
    check(service != NULL, "create private service without Python for native voices");
    if (service == NULL) return 1;
    check(zsharp_speech_speak(service, "", NULL, error, sizeof(error)), "default voice accepts empty no-op without file");
    check(zsharp_speech_poll(service, &active, error, sizeof(error)) && !active, "empty utterance creates no work");
    fixture(root, "normal.zvoice", "{\"format\":\"ZVOICE\",\"version\":1,\"type\":\"preset\",\"backend\":\"system\",\"pitch\":110,\"speed\":110,\"volume\":20}");
    check(zsharp_speech_speak(service, "", "normal.zvoice", error, sizeof(error)), "manual pitch/speed/volume preset parses");
    fixture(root, "bad.zvoice", "{\"pitch\":\"100\"}");
    check(!zsharp_speech_speak(service, "", "bad.zvoice", error, sizeof(error)) && strstr(error, "invalid .zvoice"), "non-numeric pitch rejected");
    fixture(root, "bad.zvoice", "{\"version\":1.5}");
    check(!zsharp_speech_speak(service, "", "bad.zvoice", error, sizeof(error)), "fractional schema version rejected");
    fixture(root, "bad.zvoice", "{\"volume\":101}");
    check(!zsharp_speech_speak(service, "", "bad.zvoice", error, sizeof(error)), "volume range checked");
    fixture(root, "bad.zvoice", "{\"backend\":\"executable\"}");
    check(!zsharp_speech_speak(service, "", "bad.zvoice", error, sizeof(error)), "arbitrary executable voice backend rejected");
    check(!zsharp_speech_speak(service, "", "../normal.zvoice", error, sizeof(error)), "parent traversal rejected");
    check(!zsharp_speech_speak(service, "", "/normal.zvoice", error, sizeof(error)), "absolute voice path rejected");
    check(!zsharp_speech_speak(service, "", "missing.zvoice", error, sizeof(error)), "missing voice rejected");
    check(!zsharp_speech_speak(service, "", "normal.json", error, sizeof(error)), "voice extension required");
    check(!zsharp_speech_create_voice(service, "reference.wav", "created.zvoice", NULL, NULL, error, sizeof(error)), "missing reference rejected");
    fixture(root, "reference.wav", "not-wave-data");
    fixture(root, "exists.zvoice", "existing contents");
    check(!zsharp_speech_create_voice(service, "reference.wav", "exists.zvoice", NULL, NULL, error, sizeof(error)) && strstr(error, "overwrite"), "existing voice never overwritten");
    check(!zsharp_speech_create_voice(service, "reference.wav", "created.zvoice", always_cancel, NULL, error, sizeof(error)), "voice creation cancellation or missing clone runtime fails, never substitutes preset");
    snprintf(path, sizeof(path), "%s/created.zvoice", root);
    { FILE *file = fopen(path, "rb"); check(file == NULL, "failed cloning creates no substitute output"); if (file) fclose(file); }
    check(!zsharp_speech_run_python(NULL, NULL, NULL, 0, 100, NULL, NULL, error, sizeof(error)), "missing helper capability diagnosed");

    if (argc > 1) {
        char helper[4096], result[4096];
        const char *arguments[2];
        snprintf(helper, sizeof(helper), "%s/runner.py", root);
        snprintf(result, sizeof(result), "%s/result.txt", root);
        fixture(root, "runner.py", "import pathlib,sys,time\nif sys.argv[1]=='sleep': time.sleep(10)\nelse: pathlib.Path(sys.argv[1]).write_text(sys.argv[2],encoding='utf-8')\n");
        arguments[0] = result; arguments[1] = "spaces \"quotes\" and \\ trailing\\";
        check(zsharp_speech_run_python(argv[1], helper, arguments, 2, 5000, NULL, NULL, error, sizeof(error)), "helper argument vector preserves spaces/quotes/backslashes");
        { FILE *file = fopen(result, "rb"); char content[256] = ""; if (file) { fread(content, 1, sizeof(content) - 1, file); fclose(file); }
          check(strcmp(content, arguments[1]) == 0, "helper receives exact argument contents"); }
        arguments[0] = "sleep";
        check(!zsharp_speech_run_python(argv[1], helper, arguments, 1, 50, NULL, NULL, error, sizeof(error)) && strstr(error, "timed out"), "helper timeout terminates child");
        check(!zsharp_speech_run_python(argv[1], helper, arguments, 1, 5000, always_cancel, NULL, error, sizeof(error)) && strstr(error, "cancelled"), "helper cancellation terminates before launch");
    }

    /* All audio output is replaced above. This tests real native synthesis to
     * a temporary WAV without opening a speaker or recording a microphone. */
    check(zsharp_speech_speak(service, "A short Z sharp synthesis test.", NULL, error, sizeof(error)), "queue default native synthesis");
    ok = 1;
    for (attempts = 0; attempts < 3000; attempts++) {
        if (!zsharp_speech_poll(service, &active, error, sizeof(error))) { ok = 0; break; }
        if (!active) break;
        pause_short();
    }
#ifdef _WIN32
    if (!ok) fprintf(stderr, "SAPI diagnostic: %s\n", error);
    check(ok && !active && wave_seen == 1 && wave_valid, "Windows SAPI generated nonempty WAV; no hardware playback");
#else
    if (ok) check(!active && wave_seen == 1 && wave_valid, "native backend generated WAV without playback");
    else printf("SKIP optional native synthesizer: %s\n", error);
#endif
    zsharp_speech_destroy(service);
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        snprintf(path, sizeof(path), "%s/%s", root, fixtures[index]); remove(path);
    }
#ifdef _WIN32
    RemoveDirectoryW(directory);
#else
    rmdir(root);
#endif
    printf("Speech backend: %d checks, %d failures (no audible output).\n", checks, failures);
    return failures != 0;
}
