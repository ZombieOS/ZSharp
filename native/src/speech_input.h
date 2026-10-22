#ifndef ZSHARP_SPEECH_INPUT_H
#define ZSHARP_SPEECH_INPUT_H

#include "speech_api.h"

typedef struct ZSharpSpeechWorker ZSharpSpeechWorker;

/* Each frame is one UTF-8 JSON payload, not including its little-endian length.
 * Set complete=1 on the final ok/error frame. A false return with complete=1
 * reports an application error without losing the loaded model; false with
 * complete=0 aborts the transport and discards the worker. The callback must
 * enforce PCM format, sample-rate, and maximum five-minute audio duration. */
typedef int (*ZSharpSpeechWorkerFrame)(const char *payload, size_t length,
    void *context, int *complete, char *error, size_t error_size);

/* Caller owns and serializes its handle. Run on the service worker thread;
 * destroy only after that thread stops. Supports synthesize-stream requests
 * ["synthesize-stream", existing_voice_path, existing_text_path], or
 * ["prepare", existing_voice_path, optional_private_workspace] (one terminal
 * frame, no PCM). Payloads are
 * capped at 256 KiB per frame and 128 MiB / 65536 frames per utterance. No
 * failed command is replayed. This worker is independent from the STT singleton. */
int zsharp_speech_worker_run_stream(ZSharpSpeechWorker **worker,
    const char *python, const char *helper,
    const char *const *arguments, size_t argument_count,
    ZSharpSpeechWorkerFrame frame, void *frame_context,
    unsigned int timeout_ms, ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size);
void zsharp_speech_worker_destroy(ZSharpSpeechWorker *worker);

/* One private, serialized speech-recognition worker per ZVM process. This is
 * deliberately independent of the user-script Python bridge. */
int zsharp_speech_input_run(const char *python, const char *helper,
    const char *const *arguments, size_t argument_count,
    unsigned int timeout_ms, ZSharpSpeechCancelled cancelled, void *context,
    char *error, size_t error_size);

/* Call after script workers finish. Also registered as an atexit fallback. */
void zsharp_speech_input_shutdown(void);

#endif
