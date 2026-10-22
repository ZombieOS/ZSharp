#ifndef ZSHARP_AUDIO_API_H
#define ZSHARP_AUDIO_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ZSharpAudioCancelled)(void *context);

typedef struct ZSharpAudioDevice {
    uint32_t id;
    char *name;
} ZSharpAudioDevice;

/* Initialize on the main/UI thread before starting script workers. This only
 * initializes/enumerates audio; a recording device is opened only by record.
 * Selection applies to this process and never changes the OS default device. */
int zsharp_audio_prepare(char *error, size_t error_size);
void zsharp_audio_cancel_all(void);
/* Call on the main/UI thread after all recording/playback workers have joined. */
void zsharp_audio_shutdown(void);

int zsharp_audio_list_devices(int recording, ZSharpAudioDevice **devices,
                              size_t *count, char *error, size_t error_size);
void zsharp_audio_free_devices(ZSharpAudioDevice *devices, size_t count);
/* Returned names are malloc-owned; NULL with an empty error means no device.
 * Select by unique exact name or "id:<physical SDL device ID>". NULL/empty resets
 * to the system default for future streams; existing streams are not rerouted. */
char *zsharp_audio_selected_input(char *error, size_t error_size);
char *zsharp_audio_selected_output(char *error, size_t error_size);
int zsharp_audio_select_input(const char *name, char *error, size_t error_size);
int zsharp_audio_select_output(const char *name, char *error, size_t error_size);

/* 16 kHz mono PCM16 WAV. Bounds: 0.05..30 seconds, always the full requested
 * sample duration. Cancellation/error preserves any existing output file.
 * The caller controls/validates the absolute destination (normally a unique
 * private temporary path). The relative wrapper rejects traversal, absolute
 * paths, symlink/reparse parents and non-regular destinations. Output is replaced
 * atomically only after successful recording; parent directories must exist. */
int zsharp_audio_record_path(const char *path, double seconds,
                             ZSharpAudioCancelled cancelled, void *context,
                             char *error, size_t error_size);
int zsharp_audio_record(const char *project_root, const char *relative_path,
                        double seconds, ZSharpAudioCancelled cancelled,
                        void *context, char *error, size_t error_size);
/* Creates a unique owner-controlled OS-temp WAV. Recognition uses adaptive
 * 20 ms energy windows, keeps 200 ms before speech onset, and ends after 400 ms
 * of trailing silence. The initial idle wait is at most 5 seconds; seconds is
 * the utterance bound (up to 30 s), not a mandatory fixed recording duration.
 * On success the caller owns the path and must remove it after transcription. */
int zsharp_audio_capture_temp(double seconds, ZSharpAudioCancelled cancelled,
                              void *context, char *output_path,
                              size_t output_capacity, char *error,
                              size_t error_size);
/* Current recording RMS amplitude, normalized 0..1; zero while not recording.
 * Reading it never starts recording or asks for microphone access. */
double zsharp_audio_level(void);

/* Used by synthesized speech: WAV conversion/playback uses the selected process
 * output device. Gain is 0..1. This blocks only its calling worker and can cancel. */
int zsharp_audio_play_wav(const char *path, double gain,
                         ZSharpAudioCancelled cancelled, void *context,
                         char *error, size_t error_size);

/* Incremental PCM16 mono output for low-latency synthesis. Opening does not
 * wait for a complete WAV: each append is playable immediately. Rate is
 * 8000..192000 Hz, gain 0..1, with at most two seconds of queued input and a
 * ten-minute total stream limit. A stalled/disconnected device fails clearly.
 * One calling worker owns the handle; do not use it concurrently. append keeps
 * ownership on both success/error. finish drains and CONSUMES/FREES the handle
 * on both success/error; abort also consumes it without draining. */
typedef struct ZSharpAudioPlayback ZSharpAudioPlayback;
ZSharpAudioPlayback *zsharp_audio_playback_open(unsigned int sample_rate,
                            double gain, ZSharpAudioCancelled cancelled,
                            void *context, char *error, size_t error_size);
int zsharp_audio_playback_append(ZSharpAudioPlayback *playback,
                                 const int16_t *samples, size_t count,
                                 char *error, size_t error_size);
int zsharp_audio_playback_finish(ZSharpAudioPlayback *playback,
                                 char *error, size_t error_size);
void zsharp_audio_playback_abort(ZSharpAudioPlayback *playback);

/* Pure helpers, also used by fixture-only unit tests. */
int zsharp_audio_match_device(const ZSharpAudioDevice *devices, size_t count,
                              const char *selection, uint32_t *id,
                              char *error, size_t error_size);
void zsharp_audio_pcm16_levels(const int16_t *samples, size_t count,
                               double *peak, double *rms);
int zsharp_audio_write_wav(const char *path, const int16_t *samples,
                           size_t count, unsigned int sample_rate,
                           char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
