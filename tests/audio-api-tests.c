/* Fixture-only tests: no SDL initialization, device access, recording or sound. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_api.h"
#include "audio_endpoint.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint32_t u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int cancelled(void *context) {(void)context; return 1;}

static void endpoint_signal(int16_t *samples, size_t frames, double amplitude,
                             int varied) {
    size_t index;
    for (index = 0; index < frames; index++) {
        double envelope = varied ? 0.55 + 0.45 * ((index / 320) % 3) / 2.0 : 1.0;
        samples[index] = (int16_t)(32767.0 * amplitude * envelope *
            sin((double)index * 6.283185307179586 * 200.0 / 16000.0));
    }
}

static void endpoint_tests(void) {
    const size_t rate = 16000;
    int16_t *samples = (int16_t *)calloc(rate * 36, sizeof(*samples));
    ZSharpAudioEndpoint endpoint, fragmented;
    size_t used, offset, end;
    assert(samples != NULL);
    /* Very quiet speech, far below the old fixed RMS >= .02 threshold. */
    endpoint_signal(samples + rate / 2, rate, 0.003, 1);
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 3);
    assert(endpoint.speaking && endpoint.done == 1);
    assert(endpoint.capture_start == rate * 3 / 10);
    assert(used == rate * 19 / 10); /* .5 idle + 1 speech + .4 silence. */
    /* Driver read boundaries cannot change onset/end decisions. */
    zsharp_audio_endpoint_init(&fragmented, rate * 30);
    offset = 0;
    while (!fragmented.done) {
        size_t chunk = offset % 1931 + 1;
        offset += zsharp_audio_endpoint_feed(&fragmented, samples + offset, chunk);
    }
    assert(fragmented.total_samples == endpoint.total_samples);
    assert(fragmented.capture_start == endpoint.capture_start && offset == used);
    /* A 200 ms pause inside a sentence must not submit a partial utterance. */
    memset(samples, 0, rate * 36 * sizeof(*samples));
    endpoint_signal(samples + rate / 2, rate / 2, 0.3, 1);
    endpoint_signal(samples + rate * 12 / 10, rate / 2, 0.3, 1);
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 3);
    assert(endpoint.done == 1 && used == rate * 21 / 10);
    /* A stable startup fan/hum is learned, never reported as speech. */
    endpoint_signal(samples, rate * 6, 0.025, 0);
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 6);
    assert(!endpoint.speaking && endpoint.done == 2 && used == rate * 5);
    /* Learned quieter background still allows evolving quiet speech. */
    endpoint_signal(samples, rate * 4, 0.001, 0);
    endpoint_signal(samples + rate, rate, 0.008, 1);
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 4);
    assert(endpoint.speaking && endpoint.done == 1 && used == rate * 24 / 10);
    /* No speech is bounded at five seconds rather than a busy retry loop. */
    memset(samples, 0, rate * 36 * sizeof(*samples));
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 6);
    assert(endpoint.done == 2 && used == rate * 5);
    /* A late start retains the full 30-second utterance budget. */
    endpoint_signal(samples + rate * 47 / 10, rate * 31, 0.01, 1);
    zsharp_audio_endpoint_init(&endpoint, rate * 30);
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate * 36);
    assert(endpoint.speaking && endpoint.done == 3);
    assert(endpoint.capture_start == rate * 45 / 10);
    assert(used - endpoint.capture_start == rate * 30);
    end = used;
    assert(zsharp_audio_endpoint_feed(&endpoint, samples, rate) == 0);
    assert(endpoint.total_samples == end);
    /* Short calls still honor their explicit upper/idle bound exactly. */
    zsharp_audio_endpoint_init(&endpoint, rate / 20);
    memset(samples, 0, rate * sizeof(*samples));
    used = zsharp_audio_endpoint_feed(&endpoint, samples, rate);
    assert(endpoint.done == 2 && used == rate / 20);
    free(samples);
}

static void safe_destination_tests(const char *fixture) {
    char root[2048], directory[2048], link[2048], relative[2048], error[512];
    const char *basename = strrchr(fixture, '/');
    int linked;
    int16_t sample = 123;
#ifdef _WIN32
    const char *back = strrchr(fixture, '\\');
    if (back != NULL && (basename == NULL || back > basename)) basename = back;
#endif
    assert(basename != NULL && strlen(fixture) < 1900);
    memcpy(root, fixture, (size_t)(basename - fixture)); root[basename - fixture] = '\0';
    basename++;
    assert(snprintf(directory, sizeof(directory), "%s.directory", fixture) > 0);
#ifdef _WIN32
    assert(CreateDirectoryA(directory, NULL));
#else
    assert(mkdir(directory, 0700) == 0);
#endif
    /* A directory can never replace a recording, including under cancellation. */
    assert(snprintf(relative, sizeof(relative), "%s.directory", basename) > 0);
    assert(!zsharp_audio_record(root, relative, 1, cancelled, NULL, error, sizeof(error)));
    assert(strstr(error, "regular file") != NULL);
    assert(!zsharp_audio_write_wav(directory, &sample, 1, 16000, error, sizeof(error)));
    assert(snprintf(link, sizeof(link), "%s.link", fixture) > 0);
#ifdef _WIN32
    linked = CreateSymbolicLinkA(link, fixture, 2) != 0;
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
        linked = CreateSymbolicLinkA(link, fixture, 0) != 0;
#else
    linked = symlink(fixture, link) == 0;
#endif
    if (linked) {
        assert(!zsharp_audio_write_wav(link, &sample, 1, 16000, error, sizeof(error)));
        assert(strstr(error, "regular file") != NULL);
        assert(snprintf(relative, sizeof(relative), "%s.link", basename) > 0);
        assert(!zsharp_audio_record(root, relative, 1, cancelled, NULL, error, sizeof(error)));
        assert(strstr(error, "regular file") != NULL);
#ifdef _WIN32
        assert(DeleteFileA(link));
#else
        assert(unlink(link) == 0);
#endif
    } else puts("SKIP: fixture file symlink creation is not permitted on this host");
    assert(snprintf(link, sizeof(link), "%s.linkdir", fixture) > 0);
#ifdef _WIN32
    linked = CreateSymbolicLinkA(link, directory, 1 | 2) != 0;
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
        linked = CreateSymbolicLinkA(link, directory, 1) != 0;
#else
    linked = symlink(directory, link) == 0;
#endif
    if (linked) {
        assert(snprintf(relative, sizeof(relative), "%s.linkdir/recording.wav", basename) > 0);
        assert(!zsharp_audio_record(root, relative, 1, cancelled, NULL, error, sizeof(error)));
        assert(strstr(error, "parent directory") != NULL);
#ifdef _WIN32
        assert(RemoveDirectoryA(link));
#else
        assert(unlink(link) == 0);
#endif
    } else puts("SKIP: fixture directory symlink creation is not permitted on this host");
#ifdef _WIN32
    assert(RemoveDirectoryA(directory));
#else
    assert(rmdir(directory) == 0);
#endif
}

int main(int argc, char **argv) {
    int16_t silence[4] = {0, 0, 0, 0};
    int16_t samples[4] = {-32768, 0, 32767, 16384};
    double peak, rms;
    char error[512], temporary[2048];
    unsigned char bytes[64];
    FILE *file;
    size_t length;
    ZSharpAudioDevice *devices;
    uint32_t selected;
    ZSharpAudioDevice fixture_devices[3] = {{1, "Microphone"}, {2, "Microphone"}, {3, "Headset"}};
    assert(argc == 2); /* The caller supplies its own temporary fixture path. */
    endpoint_tests();
    zsharp_audio_pcm16_levels(silence, 4, &peak, &rms);
    assert(peak == 0 && rms == 0);
    zsharp_audio_pcm16_levels(NULL, 0, &peak, &rms);
    assert(peak == 0 && rms == 0);
    zsharp_audio_pcm16_levels(samples, 4, &peak, &rms);
    assert(peak == 1.0);
    assert(fabs(rms - sqrt((1.0 + pow(32767.0 / 32768.0, 2) + 0.25) / 4)) < 1e-12);
    assert(zsharp_audio_write_wav(argv[1], samples, 4, 16000, error, sizeof(error)));
    file = fopen(argv[1], "rb"); assert(file != NULL);
    length = fread(bytes, 1, sizeof(bytes), file); fclose(file);
    assert(length == 52);
    assert(!memcmp(bytes, "RIFF", 4) && u32(bytes + 4) == 44);
    assert(!memcmp(bytes + 8, "WAVEfmt ", 8));
    assert(u32(bytes + 16) == 16 && bytes[20] == 1 && bytes[22] == 1);
    assert(u32(bytes + 24) == 16000 && u32(bytes + 28) == 32000);
    assert(bytes[32] == 2 && bytes[34] == 16);
    assert(!memcmp(bytes + 36, "data", 4) && u32(bytes + 40) == 8);
    assert(bytes[44] == 0 && bytes[45] == 128);
    assert(bytes[48] == 255 && bytes[49] == 127);
    assert(bytes[50] == 0 && bytes[51] == 64);
    assert(!zsharp_audio_write_wav(argv[1], NULL, 1, 16000, error, sizeof(error)));
    assert(strstr(error, "invalid") != NULL);
    assert(!zsharp_audio_write_wav(argv[1], samples, 4, 0, error, sizeof(error)));
    assert(!zsharp_audio_write_wav(NULL, samples, 4, 16000, error, sizeof(error)));
    assert(!zsharp_audio_write_wav(argv[1], samples, (size_t)33 * 1024 * 1024, 16000, error, sizeof(error)));
    assert(!zsharp_audio_record_path(argv[1], 1, cancelled, NULL, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL);
    /* Full-duration recording remains a separate path, including its 30 s
     * bound and cancellation guarantee; endpointing is recognition-only. */
    assert(!zsharp_audio_record_path(argv[1], 30, cancelled, NULL, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL);
    safe_destination_tests(argv[1]);
    /* Invalid input, cancellation and unsafe destinations leave the original
     * recording unchanged, not truncated or partially replaced. */
    file = fopen(argv[1], "rb"); assert(file != NULL);
    length = fread(bytes, 1, sizeof(bytes), file); fclose(file);
    assert(length == 52 && bytes[44] == 0 && bytes[45] == 128 && bytes[50] == 0 && bytes[51] == 64);
    assert(!zsharp_audio_record("project", "../outside.wav", 1, cancelled, NULL, error, sizeof(error)));
    assert(strstr(error, "traverse") != NULL);
    assert(!zsharp_audio_record("project", "sub/../../outside.wav", 1, cancelled, NULL, error, sizeof(error)));
    assert(!zsharp_audio_record("project", "/outside.wav", 1, cancelled, NULL, error, sizeof(error)));
    assert(!zsharp_audio_record("project", "C:/outside.wav", 1, cancelled, NULL, error, sizeof(error)));
    assert(!zsharp_audio_record("project", "sub//file.wav", 1, cancelled, NULL, error, sizeof(error)));
    assert(!zsharp_audio_capture_temp(5, cancelled, NULL, temporary, sizeof(temporary), error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL && temporary[0] == '\0');
    assert(!zsharp_audio_capture_temp(NAN, cancelled, NULL, temporary, sizeof(temporary), error, sizeof(error)));
    assert(strstr(error, "duration") != NULL);
    assert(!zsharp_audio_match_device(fixture_devices, 3, "Microphone", &selected, error, sizeof(error)));
    assert(strstr(error, "ambiguous") != NULL);
    assert(zsharp_audio_match_device(fixture_devices, 3, "Headset", &selected, error, sizeof(error)) && selected == 3);
    assert(zsharp_audio_match_device(fixture_devices, 3, "id:2", &selected, error, sizeof(error)) && selected == 2);
    assert(!zsharp_audio_match_device(fixture_devices, 3, "Missing", &selected, error, sizeof(error)));
    assert(strstr(error, "unavailable") != NULL);
    assert(!zsharp_audio_match_device(fixture_devices, 3, "id:-1", &selected, error, sizeof(error)));
    assert(!zsharp_audio_match_device(fixture_devices, 3, "id:0", &selected, error, sizeof(error)));
    assert(!zsharp_audio_match_device(fixture_devices, 3, "id:4294967295", &selected, error, sizeof(error)));
    assert(!zsharp_audio_match_device(fixture_devices, 3, "id:1suffix", &selected, error, sizeof(error)));
    assert(!zsharp_audio_match_device(NULL, 1, "Missing", &selected, error, sizeof(error)));
    assert(zsharp_audio_level() == 0.0); /* Must not initialize/open any device. */
    devices = (ZSharpAudioDevice *)calloc(2, sizeof(*devices)); assert(devices != NULL);
    devices[0].name = (char *)malloc(2); assert(devices[0].name != NULL);
    memcpy(devices[0].name, "A", 2);
    zsharp_audio_free_devices(devices, 2);
    zsharp_audio_free_devices(NULL, 0);
    assert(remove(argv[1]) == 0);
    puts("PASS: adaptive sample-window endpointing, PCM levels, atomic WAV fixtures, bounds, safe paths, symlink/directory rejection and cancellation; no microphone/speaker accessed");
    return 0;
}
