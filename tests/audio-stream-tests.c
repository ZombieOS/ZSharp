/* Synthetic silence through SDL's explicit dummy driver only: never physical
 * recording or audible output. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_api.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int always_cancelled(void *context) {(void)context; return 1;}
static int flag_cancelled(void *context) {return *(int *)context;}
static int eventually_cancelled(void *context) {return ++*(int *)context >= 28;}

int main(void) {
    char error[512];
    const unsigned rate = 16000;
    int16_t *samples = (int16_t *)calloc(rate * 3, sizeof(*samples));
    ZSharpAudioPlayback *playback;
    Uint64 started, elapsed;
    int cancel = 0, polls = 0;
    ZSharpAudioDevice *devices = NULL;
    size_t device_count = 0;
    char selection[64];
    assert(samples != NULL);
    /* Validation/cancellation never initializes a device. */
    assert(!zsharp_audio_playback_open(7999, 1, NULL, NULL, error, sizeof(error)));
    assert(strstr(error, "sample rate") != NULL);
    assert(!zsharp_audio_playback_open(192001, 1, NULL, NULL, error, sizeof(error)));
    assert(!zsharp_audio_playback_open(rate, NAN, NULL, NULL, error, sizeof(error)));
    assert(!zsharp_audio_playback_open(rate, 1.1, NULL, NULL, error, sizeof(error)));
    assert(!zsharp_audio_playback_open(rate, 1, always_cancelled, NULL, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL);
    assert((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    assert(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE));
    assert(zsharp_audio_prepare(error, sizeof(error)));
    assert(SDL_GetCurrentAudioDriver() != NULL && !strcmp(SDL_GetCurrentAudioDriver(), "dummy"));
    assert(zsharp_audio_list_devices(0, &devices, &device_count, error, sizeof(error)));
    assert(device_count > 0);
    snprintf(selection, sizeof(selection), "id:%u", devices[0].id);
    assert(zsharp_audio_select_output(selection, error, sizeof(error)));
    zsharp_audio_free_devices(devices, device_count);
    playback = zsharp_audio_playback_open(rate, 0.5, NULL, NULL, error, sizeof(error));
    assert(playback != NULL);
    assert(zsharp_audio_playback_append(playback, NULL, 0, error, sizeof(error)));
    assert(zsharp_audio_playback_append(playback, samples, rate / 10, error, sizeof(error)));
    assert(zsharp_audio_playback_append(playback, samples, rate / 10, error, sizeof(error)));
    assert(zsharp_audio_playback_finish(playback, error, sizeof(error)));
    /* More than the two-second queue applies backpressure instead of
     * accumulating unbounded PCM; finish consumes its remaining device tail. */
    playback = zsharp_audio_playback_open(rate, 1, NULL, NULL, error, sizeof(error));
    assert(playback != NULL);
    started = SDL_GetTicks();
    assert(zsharp_audio_playback_append(playback, samples, rate * 3, error, sizeof(error)));
    elapsed = SDL_GetTicks() - started;
    assert(elapsed >= 500 && elapsed < 10000);
    assert(zsharp_audio_playback_finish(playback, error, sizeof(error)));
    playback = zsharp_audio_playback_open(rate, 1, NULL, NULL, error, sizeof(error));
    assert(playback != NULL);
    assert(!zsharp_audio_playback_append(playback, NULL, 1, error, sizeof(error)));
    assert(strstr(error, "invalid") != NULL);
    assert(!zsharp_audio_playback_append(playback, samples, (size_t)rate * 600 + 1, error, sizeof(error)));
    zsharp_audio_playback_abort(playback);
    /* Cancellation remains responsive while append is waiting for its bounded
     * queue, not just at the entry to a large PCM append. */
    playback = zsharp_audio_playback_open(rate, 1, eventually_cancelled, &polls, error, sizeof(error));
    assert(playback != NULL);
    assert(!zsharp_audio_playback_append(playback, samples, rate * 3, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL && polls >= 28);
    zsharp_audio_playback_abort(playback);
    /* finish consumes the handle even on cancellation. */
    playback = zsharp_audio_playback_open(rate, 1, flag_cancelled, &cancel, error, sizeof(error));
    assert(playback != NULL);
    assert(zsharp_audio_playback_append(playback, samples, rate / 10, error, sizeof(error)));
    cancel = 1;
    assert(!zsharp_audio_playback_finish(playback, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL);
    assert(!zsharp_audio_playback_append(NULL, samples, 1, error, sizeof(error)));
    assert(!zsharp_audio_playback_finish(NULL, error, sizeof(error)));
    zsharp_audio_playback_abort(NULL);
    playback = zsharp_audio_playback_open(rate, 1, NULL, NULL, error, sizeof(error));
    assert(playback != NULL);
    zsharp_audio_cancel_all();
    assert(!zsharp_audio_playback_append(playback, samples, 1, error, sizeof(error)));
    assert(strstr(error, "cancelled") != NULL);
    zsharp_audio_playback_abort(playback);
    zsharp_audio_shutdown();
    assert((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    /* No leaked active-user count: after error/cancellation the subsystem can
     * shut down, reinitialize and open another dummy stream cleanly. */
    assert(zsharp_audio_prepare(error, sizeof(error)));
    assert(!strcmp(SDL_GetCurrentAudioDriver(), "dummy"));
    playback = zsharp_audio_playback_open(24000, 1, NULL, NULL, error, sizeof(error));
    assert(playback != NULL);
    assert(zsharp_audio_playback_finish(playback, error, sizeof(error)));
    zsharp_audio_shutdown();
    assert((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    free(samples);
    puts("PASS: incremental PCM, bounded backpressure, selected dummy output, validation, cancellation, draining and shutdown; no physical audio device accessed");
    return 0;
}
