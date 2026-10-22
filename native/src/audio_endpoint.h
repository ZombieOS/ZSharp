#ifndef ZSHARP_AUDIO_ENDPOINT_H
#define ZSHARP_AUDIO_ENDPOINT_H

/* Internal, device-independent utterance endpoint. All decisions use complete
 * 20 ms PCM windows, never the size/timing of an audio driver's read chunks.
 * Energy endpointing is not semantic speech recognition: changing background
 * sounds can resemble speech. A stable background is learned as a noise floor. */
#include <stddef.h>
#include <stdint.h>

typedef struct ZSharpAudioEndpoint {
    size_t total_samples;
    size_t maximum_samples;
    size_t idle_samples;
    size_t capture_start;
    size_t candidate_start;
    size_t last_voice;
    unsigned window_samples;
    unsigned candidate_frames;
    unsigned candidate_gaps;
    double window_squares;
    double noise_floor;
    double candidate_low;
    double candidate_high;
    int speaking;
    int done; /* 1: trailing silence, 2: idle timeout, 3: maximum duration */
} ZSharpAudioEndpoint;

void zsharp_audio_endpoint_init(ZSharpAudioEndpoint *endpoint,
                                 size_t maximum_samples);
/* Returns consumed samples; may end in the middle of a driver's PCM chunk. */
size_t zsharp_audio_endpoint_feed(ZSharpAudioEndpoint *endpoint,
                                  const int16_t *samples, size_t count);

#endif
