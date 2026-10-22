#ifndef ZSHARP_SPEECH_API_H
#define ZSHARP_SPEECH_API_H

#include <stddef.h>

typedef struct ZSharpSpeechService ZSharpSpeechService;
typedef int (*ZSharpSpeechCancelled)(void *context);

/* Shared by optional local speech-model helpers (including transcription).
 * Executes Python in isolated mode with argument-vector semantics, no shell.
 * Child output is captured, execution is bounded (1..600000 ms), and cancelling
 * terminates/reaps the owned process tree. Helpers exchange data through their
 * own caller-owned files; success here means only that the child exited zero. */
int zsharp_speech_run_python(const char *python, const char *helper,
    const char *const *arguments, size_t count, unsigned int timeout_ms,
    ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size);

/* One service belongs to one script task. Native preset voices do not require
 * Python. The optional clone backend uses the supplied private helper process,
 * never the VM's shared Python language worker. All paths are UTF-8. */
ZSharpSpeechService *zsharp_speech_create(const char *project_root,
    const char *python_executable, const char *voice_helper,
    char *error, size_t error_size);

/* Project-relative reference/output paths only. Actual speaker conditioning is
 * required: this never silently substitutes a system voice or pitch estimate.
 * Requires an explicitly configured local voice model. Completes before return,
 * does not overwrite an existing output, and accepts caller cancellation. */
int zsharp_speech_create_voice(ZSharpSpeechService *service,
    const char *reference, const char *output,
    ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size);

/* Validate/warm a project-relative voice without playback. Clone models stay
 * loaded in this service's isolated worker for later streaming utterances.
 * Completes before returning and accepts cancellation; presets only validate. */
int zsharp_speech_prepare(ZSharpSpeechService *service, const char *voice,
    ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size);

/* Validate and queue speech, returning without blocking the script/UI thread.
 * Playback uses the process-selected Speaker output device. Manual preset
 * .zvoice JSON supports pitch/speed/volume percentages (100 = unchanged). */
int zsharp_speech_speak(ZSharpSpeechService *service,
    const char *text, const char *voice, char *error, size_t error_size);

/* Returns zero with a diagnostic after a queued operation failed. active is
 * true until all queued synthesis and playback have completed. */
int zsharp_speech_poll(ZSharpSpeechService *service, int *active,
    char *error, size_t error_size);

/* Cancels synthesis/playback, joins the worker, and removes owned temporaries.
 * Call before shutting down the shared audio backend. */
void zsharp_speech_destroy(ZSharpSpeechService *service);

#endif
