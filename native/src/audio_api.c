#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "audio_api.h"
#include "audio_endpoint.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(ZSHARP_HAS_GAME_RUNTIME) || defined(ZSHARP_HAS_SDL_AUDIO)
#include <SDL3/SDL.h>
#define ZSHARP_AUDIO_SDL 1
#endif

#define AUDIO_RATE 16000u
#define AUDIO_MAX_SECONDS 30.0
#define AUDIO_WAV_LIMIT (64u * 1024u * 1024u)

static void clear_error(char *error, size_t size) {
    if (error != NULL && size != 0) error[0] = '\0';
}

static int audio_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
    return 0;
}

#if defined(ZSHARP_HAS_GAME_RUNTIME) || defined(ZSHARP_HAS_SDL_AUDIO)
static char *audio_copy(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}
#endif

#ifdef _WIN32
static wchar_t *audio_wide_path(const char *path) {
    int count;
    wchar_t *wide;
    if (path == NULL) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                 NULL, 0);
    if (count <= 0 || count > 32768) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide != NULL && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             path, -1, wide, count)) {
        free(wide); wide = NULL;
    }
    return wide;
}
#endif

#if defined(ZSHARP_HAS_GAME_RUNTIME) || defined(ZSHARP_HAS_SDL_AUDIO)
static FILE *audio_open_file(const char *path, int writing) {
#ifdef _WIN32
    wchar_t *wide = audio_wide_path(path);
    FILE *file = wide == NULL ? NULL : _wfopen(wide, writing ? L"wb" : L"rb");
    free(wide);
    return file;
#else
    return path == NULL ? NULL : fopen(path, writing ? "wb" : "rb");
#endif
}
#endif

static void audio_remove_file(const char *path) {
#ifdef _WIN32
    wchar_t *wide = audio_wide_path(path);
    if (wide != NULL) _wremove(wide);
    free(wide);
#else
    if (path != NULL) remove(path);
#endif
}

void zsharp_audio_pcm16_levels(const int16_t *samples, size_t count,
                               double *peak, double *rms) {
    size_t index;
    double maximum = 0.0, sum = 0.0;
    if (samples != NULL) for (index = 0; index < count; index++) {
        double value = (double)samples[index] / 32768.0;
        double absolute = fabs(value);
        if (absolute > maximum) maximum = absolute;
        sum += value * value;
    }
    if (peak != NULL) *peak = maximum;
    if (rms != NULL) *rms = samples == NULL || count == 0
        ? 0.0 : sqrt(sum / (double)count);
}

void zsharp_audio_endpoint_init(ZSharpAudioEndpoint *endpoint,
                                 size_t maximum_samples) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->maximum_samples = maximum_samples;
    endpoint->idle_samples = maximum_samples < AUDIO_RATE * 5u
        ? maximum_samples : AUDIO_RATE * 5u;
    endpoint->noise_floor = 0.00015;
}

static void endpoint_noise(ZSharpAudioEndpoint *endpoint, double rms) {
    double rate = rms < endpoint->noise_floor ? 0.15 : 0.04;
    endpoint->noise_floor += (rms - endpoint->noise_floor) * rate;
    if (endpoint->noise_floor < 0.00005) endpoint->noise_floor = 0.00005;
}

static void endpoint_window(ZSharpAudioEndpoint *endpoint) {
    double rms = sqrt(endpoint->window_squares / endpoint->window_samples);
    double onset = fmax(0.0006, endpoint->noise_floor * 2.7 + 0.0001);
    double offset = fmax(0.00035, endpoint->noise_floor * 1.65 + 0.00005);
    size_t window_start = endpoint->total_samples - endpoint->window_samples;
    if (endpoint->speaking) {
        if (rms >= offset) endpoint->last_voice = endpoint->total_samples;
        else endpoint_noise(endpoint, rms);
        if (endpoint->total_samples - endpoint->last_voice >= AUDIO_RATE * 4u / 10u)
            endpoint->done = 1;
    } else if (rms >= onset) {
        if (endpoint->candidate_frames == 0) {
            endpoint->candidate_start = window_start;
            endpoint->candidate_low = endpoint->candidate_high = rms;
        }
        if (rms < endpoint->candidate_low) endpoint->candidate_low = rms;
        if (rms > endpoint->candidate_high) endpoint->candidate_high = rms;
        endpoint->candidate_frames++;
        endpoint->candidate_gaps = 0;
        /* Require 100 ms of above-floor energy and an evolving envelope.
         * A constant fan/hum present when recording opens must not count as
         * speech merely because it exceeds a fixed absolute threshold. */
        if (endpoint->candidate_frames >= 5 &&
            endpoint->candidate_high >= endpoint->candidate_low * 1.25) {
            endpoint->speaking = 1;
            endpoint->capture_start = endpoint->candidate_start > AUDIO_RATE / 5u
                ? endpoint->candidate_start - AUDIO_RATE / 5u : 0;
            endpoint->last_voice = endpoint->total_samples;
        } else if (endpoint->candidate_frames >= 10) {
            endpoint->noise_floor += (rms - endpoint->noise_floor) * 0.5;
            endpoint->candidate_frames = 0;
        }
    } else {
        endpoint_noise(endpoint, rms);
        if (++endpoint->candidate_gaps >= 3) endpoint->candidate_frames = 0;
    }
    endpoint->window_samples = 0;
    endpoint->window_squares = 0;
}

size_t zsharp_audio_endpoint_feed(ZSharpAudioEndpoint *endpoint,
                                  const int16_t *samples, size_t count) {
    size_t index = 0;
    if (endpoint == NULL || samples == NULL) return 0;
    while (index < count && !endpoint->done) {
        double sample = (double)samples[index++] / 32768.0;
        endpoint->window_squares += sample * sample;
        endpoint->window_samples++;
        endpoint->total_samples++;
        if (endpoint->window_samples == AUDIO_RATE / 50u) endpoint_window(endpoint);
        if (!endpoint->done && endpoint->speaking &&
            endpoint->total_samples - endpoint->capture_start >= endpoint->maximum_samples)
            endpoint->done = 3;
        if (!endpoint->done && !endpoint->speaking &&
            endpoint->total_samples >= endpoint->idle_samples) endpoint->done = 2;
    }
    return index;
}

static int write_u16(FILE *file, unsigned int value) {
    unsigned char bytes[2] = {(unsigned char)value,
                              (unsigned char)(value >> 8)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)value,
        (unsigned char)(value >> 8), (unsigned char)(value >> 16),
        (unsigned char)(value >> 24)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

typedef struct AudioDestination {
#ifdef _WIN32
    wchar_t *directory;
    wchar_t *target;
    HANDLE *parents;
    size_t parent_count;
#else
    int directory;
    char *name;
#endif
} AudioDestination;

static void destination_close(AudioDestination *destination) {
#ifdef _WIN32
    size_t index;
    for (index = 0; index < destination->parent_count; index++)
        CloseHandle(destination->parents[index]);
    free(destination->parents); free(destination->directory); free(destination->target);
    memset(destination, 0, sizeof(*destination));
#else
    if (destination->directory >= 0) close(destination->directory);
    free(destination->name); destination->name = NULL; destination->directory = -1;
#endif
}

static int destination_regular(AudioDestination *destination,
                                 char *error, size_t size) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(destination->target);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD reason = GetLastError();
        if (reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND) return 1;
    } else if (!(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return 1;
#else
    struct stat st;
    if (fstatat(destination->directory, destination->name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISREG(st.st_mode)) return 1;
    } else if (errno == ENOENT) return 1;
#endif
    return audio_error(error, size,
        "recording destination must be a regular file, not a symbolic link, reparse point, directory or device");
}

static int safe_relative(const char *path, char *error, size_t size) {
    size_t length, index, segment = 0;
    if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\')
        return audio_error(error, size, "recording needs a safe project-relative file path");
    length = strlen(path);
    if (length > 32768) return audio_error(error, size, "recording file path is too long");
    for (index = 0; index <= length; index++) {
        unsigned char value = (unsigned char)path[index];
        if (value == ':' || (value != 0 && value < 32))
            return audio_error(error, size, "recording file path contains invalid characters");
        if (value == '/' || value == '\\' || value == 0) {
            size_t count = index - segment;
            if (count == 0 || (count == 1 && path[segment] == '.') ||
                (count == 2 && !strncmp(path + segment, "..", 2)))
                return audio_error(error, size, "recording file path cannot traverse outside the project");
            segment = index + 1;
        }
    }
    return 1;
}

/* Resolve a trusted root, then walk every untrusted relative parent without
 * following symlinks/reparse points. The final directory stays open through
 * recording and atomic replacement. Windows also holds non-delete-sharing
 * handles to every ancestor so junction/parent swaps cannot redirect writes. */
static int destination_open(const char *root, const char *relative,
                              AudioDestination *destination,
                              char *error, size_t size) {
    char *parts = NULL, *cursor, *separator;
    size_t length, index;
    memset(destination, 0, sizeof(*destination));
#ifndef _WIN32
    destination->directory = -1;
#endif
    if (root == NULL || !*root || strlen(root) > 32768)
        return audio_error(error, size, "recording needs an accessible project directory");
    if (!safe_relative(relative, error, size)) return 0;
    length = strlen(relative);
    parts = (char *)malloc(length + 1);
    if (parts == NULL) return audio_error(error, size, "out of memory");
    memcpy(parts, relative, length + 1);
    for (index = 0; index < length; index++) if (parts[index] == '\\') parts[index] = '/';
    cursor = parts;
#ifdef _WIN32
    {
        wchar_t *wide_root = audio_wide_path(root);
        wchar_t canonical[32768];
        HANDLE parent;
        BY_HANDLE_FILE_INFORMATION info;
        DWORD needed;
        destination->parents = (HANDLE *)calloc(257, sizeof(HANDLE));
        if (wide_root == NULL || destination->parents == NULL) {free(wide_root); goto failed;}
        parent = CreateFileW(wide_root, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
        free(wide_root);
        if (parent == INVALID_HANDLE_VALUE) goto failed;
        destination->parents[destination->parent_count++] = parent;
        if (!GetFileInformationByHandle(parent, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) goto failed;
        needed = GetFinalPathNameByHandleW(parent, canonical, 32768, FILE_NAME_NORMALIZED);
        if (needed == 0 || needed >= 32768) goto failed;
        while ((separator = strchr(cursor, '/')) != NULL) {
            wchar_t *component, *joined;
            size_t a, b;
            *separator = '\0'; component = audio_wide_path(cursor);
            if (component == NULL || destination->parent_count >= 257) {free(component); goto failed;}
            a = wcslen(canonical); b = wcslen(component);
            if (a + b + 2 >= 32768) {free(component); goto failed;}
            joined = (wchar_t *)malloc((a + b + 2) * sizeof(*joined));
            if (joined == NULL) {free(component); goto failed;}
            memcpy(joined, canonical, a * sizeof(*joined)); joined[a] = L'\\';
            memcpy(joined + a + 1, component, (b + 1) * sizeof(*joined)); free(component);
            parent = CreateFileW(joined, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (parent == INVALID_HANDLE_VALUE) {free(joined); goto failed;}
            destination->parents[destination->parent_count++] = parent;
            if (!GetFileInformationByHandle(parent, &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {free(joined); goto failed;}
            memcpy(canonical, joined, (a + b + 2) * sizeof(*joined)); free(joined);
            cursor = separator + 1;
        }
        {
            wchar_t *name = audio_wide_path(cursor);
            size_t a = wcslen(canonical), b;
            if (name == NULL) goto failed;
            b = wcslen(name);
            destination->directory = (wchar_t *)malloc((a + 1) * sizeof(wchar_t));
            destination->target = (wchar_t *)malloc((a + b + 2) * sizeof(wchar_t));
            if (a + b + 2 >= 32768 || destination->directory == NULL || destination->target == NULL) {free(name); goto failed;}
            memcpy(destination->directory, canonical, (a + 1) * sizeof(wchar_t));
            memcpy(destination->target, canonical, a * sizeof(wchar_t)); destination->target[a] = L'\\';
            memcpy(destination->target + a + 1, name, (b + 1) * sizeof(wchar_t)); free(name);
        }
    }
#else
    destination->directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (destination->directory < 0) goto failed;
    while ((separator = strchr(cursor, '/')) != NULL) {
        int next;
        *separator = '\0';
        next = openat(destination->directory, cursor,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) goto failed;
        close(destination->directory); destination->directory = next;
        cursor = separator + 1;
    }
    length = strlen(cursor);
    destination->name = (char *)malloc(length + 1);
    if (destination->name == NULL) goto failed;
    memcpy(destination->name, cursor, length + 1);
#endif
    free(parts);
    if (destination_regular(destination, error, size)) return 1;
    destination_close(destination); return 0;
failed:
    free(parts); destination_close(destination);
    return audio_error(error, size,
        "recording parent directory is unavailable, too deep, or contains a symbolic link/reparse point");
}

static int destination_path(const char *path, AudioDestination *destination,
                              char *error, size_t size) {
    char *root;
    const char *last;
    size_t length;
    int result;
    if (path == NULL || !*path) return audio_error(error, size, "recording needs a file path");
    last = strrchr(path, '/');
#ifdef _WIN32
    {const char *back = strrchr(path, '\\'); if (back != NULL && (last == NULL || back > last)) last = back;}
#endif
    if (last == NULL) return destination_open(".", path, destination, error, size);
    length = (size_t)(last - path);
    if (length == 0) length = 1;
#ifdef _WIN32
    if (length == 2 && path[1] == ':') length = 3;
#endif
    root = (char *)malloc(length + 1);
    if (root == NULL) return audio_error(error, size, "out of memory");
    memcpy(root, path, length); root[length] = '\0';
    result = destination_open(root, last + 1, destination, error, size);
    free(root); return result;
}

static int destination_write_wav(AudioDestination *destination,
                                  const int16_t *samples, size_t count,
                                  unsigned int rate, char *error, size_t size) {
    FILE *file = NULL;
    size_t index;
    int ok = 0, attempt;
#ifdef _WIN32
    static volatile LONG sequence;
    wchar_t temporary[32768];
    HANDLE handle = INVALID_HANDLE_VALUE;
    int descriptor;
    for (attempt = 0; attempt < 128; attempt++) {
        int length = swprintf(temporary, 32768, L"%ls\\.zsharp-audio-%lu-%lu.tmp",
            destination->directory, (unsigned long)GetCurrentProcessId(),
            (unsigned long)InterlockedIncrement(&sequence));
        if (length < 0 || length >= 32768) break;
        handle = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (handle == INVALID_HANDLE_VALUE) return audio_error(error, size, "could not reserve an atomic recording output file");
    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
    if (descriptor < 0) {CloseHandle(handle); DeleteFileW(temporary); return audio_error(error, size, "could not open the recording output stream");}
    file = _fdopen(descriptor, "wb");
    if (file == NULL) {_close(descriptor); DeleteFileW(temporary); return audio_error(error, size, "could not open the recording output stream");}
#else
    static unsigned long sequence;
    char temporary[96];
    int descriptor = -1;
    for (attempt = 0; attempt < 128; attempt++) {
        snprintf(temporary, sizeof(temporary), ".zsharp-audio-%lu-%lu.tmp",
            (unsigned long)getpid(), __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED));
        descriptor = openat(destination->directory, temporary,
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) return audio_error(error, size, "could not reserve an atomic recording output file");
    file = fdopen(descriptor, "wb");
    if (file == NULL) {close(descriptor); unlinkat(destination->directory, temporary, 0); return audio_error(error, size, "could not open the recording output stream");}
#endif
    ok = fwrite("RIFF", 1, 4, file) == 4 &&
        write_u32(file, 36u + (uint32_t)count * 2u) &&
        fwrite("WAVEfmt ", 1, 8, file) == 8 && write_u32(file, 16) &&
        write_u16(file, 1) && write_u16(file, 1) && write_u32(file, rate) &&
        write_u32(file, rate * 2u) && write_u16(file, 2) &&
        write_u16(file, 16) && fwrite("data", 1, 4, file) == 4 &&
        write_u32(file, (uint32_t)count * 2u);
    for (index = 0; ok && index < count; index++)
        ok = write_u16(file, (unsigned int)(uint16_t)samples[index]);
    if (fflush(file) != 0) ok = 0;
#ifndef _WIN32
    if (ok && fsync(fileno(file)) != 0) ok = 0;
#endif
    if (fclose(file) != 0) ok = 0;
    if (ok) ok = destination_regular(destination, error, size);
#ifdef _WIN32
    if (ok && !MoveFileExW(temporary, destination->target,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD reason = GetLastError();
        ok = 0;
        if (error != NULL && size != 0) snprintf(error, size,
            "could not atomically finish the recording WAV file (Windows error %lu); existing output was preserved",
            (unsigned long)reason);
    }
    if (!ok) DeleteFileW(temporary);
#else
    if (ok) ok = renameat(destination->directory, temporary,
                           destination->directory, destination->name) == 0;
    if (!ok) unlinkat(destination->directory, temporary, 0);
#endif
    if (!ok && (error == NULL || size == 0 || error[0] == '\0'))
        audio_error(error, size, "could not atomically finish the recording WAV file; existing output was preserved");
    return ok;
}

int zsharp_audio_write_wav(const char *path, const int16_t *samples,
                           size_t count, unsigned int rate,
                           char *error, size_t error_size) {
    AudioDestination destination;
    int result;
    clear_error(error, error_size);
    if (path == NULL || path[0] == '\0' || (count != 0 && samples == NULL) ||
        count > AUDIO_WAV_LIMIT / 2 || rate < 8000 || rate > 192000)
        return audio_error(error, error_size, "invalid or oversized mono PCM16 WAV data");
    if (!destination_path(path, &destination, error, error_size)) return 0;
    result = destination_write_wav(&destination, samples, count, rate, error, error_size);
    destination_close(&destination); return result;
}

void zsharp_audio_free_devices(ZSharpAudioDevice *devices, size_t count) {
    size_t index;
    for (index = 0; index < count; index++) free(devices[index].name);
    free(devices);
}

int zsharp_audio_match_device(const ZSharpAudioDevice *devices, size_t count,
                              const char *selection, uint32_t *id,
                              char *error, size_t error_size) {
    size_t index, matches = 0;
    unsigned long requested = 0;
    uint32_t selected = 0;
    int by_id;
    clear_error(error, error_size);
    if (id == NULL || selection == NULL || selection[0] == '\0' ||
        (count != 0 && devices == NULL) || count > 4096)
        return audio_error(error, error_size, "invalid explicit audio device selection");
    by_id = strncmp(selection, "id:", 3) == 0;
    if (by_id) {
        const char *digits = selection + 3;
        char *end;
        if (*digits < '0' || *digits > '9')
            return audio_error(error, error_size, "audio device ID must contain decimal digits after id:");
        errno = 0; requested = strtoul(digits, &end, 10);
        if (errno || *end != '\0' || requested == 0 || requested >= 0xfffffffeu)
            return audio_error(error, error_size, "invalid physical audio device ID");
    }
    for (index = 0; index < count; index++) {
        if (by_id ? devices[index].id == requested :
            devices[index].name != NULL && !strcmp(devices[index].name, selection)) {
            matches++; selected = devices[index].id;
        }
    }
    if (matches != 1)
        return audio_error(error, error_size, matches == 0
            ? "the selected audio device is unavailable or disconnected"
            : "audio device name is ambiguous; select its explicit id:<number> instead");
    *id = selected;
    return 1;
}

#ifdef ZSHARP_AUDIO_SDL
static SDL_AtomicInt audio_ready;
static SDL_Mutex *audio_mutex;
static char *input_selection, *output_selection;
static int active_users, recording_active, stopping;
static double current_level;

int zsharp_audio_prepare(char *error, size_t error_size) {
    clear_error(error, error_size);
    if (SDL_GetAtomicInt(&audio_ready)) return 1;
    if (!SDL_IsMainThread()) return audio_error(error, error_size,
        "audio must be initialized on the main thread before script workers start");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not initialize audio: %s", SDL_GetError());
        return 0;
    }
    audio_mutex = SDL_CreateMutex();
    if (audio_mutex == NULL) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return audio_error(error, error_size, "could not initialize audio synchronization");
    }
    stopping = 0;
    SDL_SetAtomicInt(&audio_ready, 1);
    return 1;
}

static int audio_available(char *error, size_t error_size) {
    if (SDL_GetAtomicInt(&audio_ready) &&
        (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) return 1;
    return audio_error(error, error_size,
        "audio is unavailable; initialize it on the main thread before starting script tasks");
}

void zsharp_audio_cancel_all(void) {
    if (!SDL_GetAtomicInt(&audio_ready)) return;
    SDL_LockMutex(audio_mutex); stopping = 1; SDL_UnlockMutex(audio_mutex);
}

void zsharp_audio_shutdown(void) {
    if (!SDL_GetAtomicInt(&audio_ready)) return;
    SDL_LockMutex(audio_mutex);
    stopping = 1;
    /* Shutdown is called after worker join. Never destroy a live stream or its
     * lock out from under a worker if a caller violates that ordering. */
    if (active_users != 0 || !SDL_IsMainThread()) {
        SDL_UnlockMutex(audio_mutex); return;
    }
    free(input_selection); input_selection = NULL;
    free(output_selection); output_selection = NULL;
    current_level = 0.0;
    SDL_SetAtomicInt(&audio_ready, 0);
    SDL_UnlockMutex(audio_mutex);
    SDL_DestroyMutex(audio_mutex); audio_mutex = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int zsharp_audio_list_devices(int recording, ZSharpAudioDevice **devices,
                              size_t *count, char *error, size_t error_size) {
    SDL_AudioDeviceID *ids;
    ZSharpAudioDevice *items;
    int found = 0, index;
    clear_error(error, error_size);
    if (devices == NULL || count == NULL || (recording != 0 && recording != 1))
        return audio_error(error, error_size, "invalid audio device list arguments");
    *devices = NULL; *count = 0;
    if (!audio_available(error, error_size)) return 0;
    ids = recording ? SDL_GetAudioRecordingDevices(&found)
                    : SDL_GetAudioPlaybackDevices(&found);
    if (ids == NULL || found < 0 || found > 4096) {
        SDL_free(ids);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not enumerate audio devices: %s", SDL_GetError());
        return 0;
    }
    if (found == 0) {SDL_free(ids); return 1;}
    items = (ZSharpAudioDevice *)calloc((size_t)found, sizeof(*items));
    if (items == NULL) {SDL_free(ids); return audio_error(error, error_size, "out of memory");}
    for (index = 0; index < found; index++) {
        const char *name = SDL_GetAudioDeviceName(ids[index]);
        if (name == NULL || (items[index].name = audio_copy(name)) == NULL) {
            zsharp_audio_free_devices(items, (size_t)found); SDL_free(ids);
            return audio_error(error, error_size,
                "an audio device disappeared during enumeration; try again");
        }
        items[index].id = ids[index];
    }
    SDL_free(ids); *devices = items; *count = (size_t)found;
    return 1;
}

/* Called while holding audio_mutex. Device enumeration does not open a mic. */
static int resolve_device(int recording, const char *selection,
                           SDL_AudioDeviceID *id, char **name,
                           char *error, size_t error_size) {
    ZSharpAudioDevice *devices = NULL;
    size_t count = 0, index;
    uint32_t selected;
    *id = recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (name != NULL) *name = NULL;
    if (!zsharp_audio_list_devices(recording, &devices, &count, error, error_size)) return 0;
    if (selection == NULL || selection[0] == '\0') {
        const char *current;
        if (count == 0) {zsharp_audio_free_devices(devices, count); return 1;}
        current = SDL_GetAudioDeviceName(*id);
        if (current == NULL) {
            zsharp_audio_free_devices(devices, count);
            return audio_error(error, error_size, "the system default audio device is unavailable");
        }
        if (name != NULL) *name = audio_copy(current);
        zsharp_audio_free_devices(devices, count);
        if (name != NULL && *name == NULL) return audio_error(error, error_size, "out of memory");
        return 1;
    }
    if (!zsharp_audio_match_device(devices, count, selection, &selected,
                                    error, error_size)) {
        zsharp_audio_free_devices(devices, count);
        return 0;
    }
    *id = selected;
    for (index = 0; index < count; index++) if (devices[index].id == selected) break;
    if (name != NULL) *name = audio_copy(devices[index].name);
    zsharp_audio_free_devices(devices, count);
    return name == NULL || *name != NULL ? 1 : audio_error(error, error_size, "out of memory");
}

static char *selected_device(int recording, char *error, size_t error_size) {
    SDL_AudioDeviceID id;
    char *name = NULL;
    clear_error(error, error_size);
    if (!audio_available(error, error_size)) return NULL;
    SDL_LockMutex(audio_mutex);
    resolve_device(recording, recording ? input_selection : output_selection,
                     &id, &name, error, error_size);
    SDL_UnlockMutex(audio_mutex);
    return name;
}

static int select_device(int recording, const char *name,
                          char *error, size_t error_size) {
    SDL_AudioDeviceID id;
    char *copy = NULL;
    int ok = 1;
    clear_error(error, error_size);
    if (!audio_available(error, error_size)) return 0;
    SDL_LockMutex(audio_mutex);
    if (stopping) ok = audio_error(error, error_size, "audio is shutting down");
    else if (name != NULL && name[0] != '\0') {
        ok = resolve_device(recording, name, &id, NULL, error, error_size);
        if (ok && (copy = audio_copy(name)) == NULL) ok = audio_error(error, error_size, "out of memory");
    }
    if (ok) {
        char **slot = recording ? &input_selection : &output_selection;
        free(*slot); *slot = copy;
    }
    SDL_UnlockMutex(audio_mutex);
    return ok;
}

char *zsharp_audio_selected_input(char *error, size_t size) {return selected_device(1, error, size);}
char *zsharp_audio_selected_output(char *error, size_t size) {return selected_device(0, error, size);}
int zsharp_audio_select_input(const char *name, char *error, size_t size) {return select_device(1, name, error, size);}
int zsharp_audio_select_output(const char *name, char *error, size_t size) {return select_device(0, name, error, size);}

static int begin_stream(int recording, SDL_AudioDeviceID *id,
                         char *error, size_t size) {
    char *name = NULL;
    int ok;
    if (!audio_available(error, size)) return 0;
    SDL_LockMutex(audio_mutex);
    if (stopping || (recording && recording_active)) {
        SDL_UnlockMutex(audio_mutex);
        return audio_error(error, size, stopping ? "audio is shutting down"
            : "a microphone recording is already active in this process");
    }
    ok = resolve_device(recording, recording ? input_selection : output_selection,
                          id, &name, error, size);
    if (ok && name == NULL) ok = audio_error(error, size,
        recording ? "no microphone input device is available" : "no speaker output device is available");
    free(name);
    if (ok) {active_users++; if (recording) {recording_active = 1; current_level = 0.0;}}
    SDL_UnlockMutex(audio_mutex);
    return ok;
}

static int stream_cancelled(ZSharpAudioCancelled callback, void *context) {
    int result;
    SDL_LockMutex(audio_mutex); result = stopping; SDL_UnlockMutex(audio_mutex);
    return result || (callback != NULL && callback(context));
}

static void end_stream(int recording) {
    SDL_LockMutex(audio_mutex);
    active_users--;
    if (recording) {recording_active = 0; current_level = 0.0;}
    SDL_UnlockMutex(audio_mutex);
}

double zsharp_audio_level(void) {
    double value;
    if (!SDL_GetAtomicInt(&audio_ready)) return 0.0;
    SDL_LockMutex(audio_mutex); value = current_level; SDL_UnlockMutex(audio_mutex);
    return value;
}

static int record_to_destination(AudioDestination *destination, double seconds,
                                   int utterance, ZSharpAudioCancelled cancelled,
                                   void *context, char *error, size_t error_size) {
    SDL_AudioDeviceID device;
    SDL_AudioSpec spec;
    SDL_AudioStream *stream = NULL;
    int16_t *samples;
    size_t capacity, count = 0, first = 0;
    Uint64 start, last_device_check = 0;
    ZSharpAudioEndpoint endpoint;
    int result = 0;
    clear_error(error, error_size);
    if (!isfinite(seconds) || seconds < 0.05 || seconds > AUDIO_MAX_SECONDS)
        return audio_error(error, error_size, "recording duration must be 0.05..30 seconds");
    if (cancelled != NULL && cancelled(context))
        return audio_error(error, error_size, "microphone recording was cancelled");
    if (!begin_stream(1, &device, error, error_size)) return 0;
    capacity = (size_t)ceil(seconds * AUDIO_RATE);
    zsharp_audio_endpoint_init(&endpoint, capacity);
    /* Idle audio is bounded separately and trimmed after onset, so a person
     * starting near the idle deadline still gets the full utterance bound. */
    if (utterance) capacity += endpoint.idle_samples;
    samples = (int16_t *)malloc(capacity * sizeof(*samples));
    if (samples == NULL) {audio_error(error, error_size, "out of memory"); goto done;}
    SDL_zero(spec); spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = AUDIO_RATE;
    stream = SDL_OpenAudioDeviceStream(device, &spec, NULL, NULL);
    if (stream == NULL || !SDL_ResumeAudioStreamDevice(stream)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                "could not open the microphone; check OS microphone permission and device availability: %s", SDL_GetError());
        goto done;
    }
    start = SDL_GetTicks();
    /* Capture the requested sample duration, allowing a bounded two-second
     * device-start/conversion grace period. A stalled stream must not produce
     * a misleading successful short recording or overwrite an existing file. */
    while (count < capacity && SDL_GetTicks() - start <
            (Uint64)ceil(seconds * 1000.0) + (utterance ? 5000u : 0u) + 2000u) {
        int available, received;
        Uint64 now = SDL_GetTicks();
        size_t wanted;
        double rms;
        if (stream_cancelled(cancelled, context)) {
            audio_error(error, error_size, "microphone recording was cancelled"); goto done;
        }
        if (now - last_device_check >= 100) {
            last_device_check = now;
            if (SDL_GetAudioDeviceName(device) == NULL) {
                audio_error(error, error_size, "the microphone was disconnected during recording"); goto done;
            }
        }
        available = SDL_GetAudioStreamAvailable(stream);
        if (available < 0) {audio_error(error, error_size, "could not read microphone recording data"); goto done;}
        wanted = capacity - count;
        if (wanted > 1600) wanted = 1600;
        if (wanted > (size_t)available / 2) wanted = (size_t)available / 2;
        if (wanted == 0) {SDL_Delay(10); continue;}
        received = SDL_GetAudioStreamData(stream, samples + count, (int)(wanted * 2));
        if (received < 0 || (received & 1)) {
            audio_error(error, error_size, "microphone returned invalid PCM16 recording data"); goto done;
        }
        if (received == 0) {SDL_Delay(10); continue;}
        zsharp_audio_pcm16_levels(samples + count, (size_t)received / 2, NULL, &rms);
        SDL_LockMutex(audio_mutex); current_level = rms; SDL_UnlockMutex(audio_mutex);
        if (utterance) {
            count += zsharp_audio_endpoint_feed(&endpoint, samples + count,
                                                  (size_t)received / 2);
            if (endpoint.done) break;
        } else count += (size_t)received / 2;
    }
    if (stream_cancelled(cancelled, context)) {
        audio_error(error, error_size, "microphone recording was cancelled"); goto done;
    }
    if (count == 0) {
        audio_error(error, error_size,
            "the microphone supplied no audio; check OS permission, mute state, and device connection"); goto done;
    }
    if (!utterance && count != capacity) {
        audio_error(error, error_size,
            "the microphone did not supply the full requested recording duration; existing output was preserved"); goto done;
    }
    if (utterance && !endpoint.done) {
        audio_error(error, error_size,
            "the microphone stalled before the speech recording completed"); goto done;
    }
    if (utterance && endpoint.speaking) first = endpoint.capture_start;
    /* Close the capture device before disk I/O/transcription. */
    SDL_DestroyAudioStream(stream); stream = NULL;
    result = destination_write_wav(destination, samples + first, count - first,
                                     AUDIO_RATE, error, error_size);
done:
    if (stream != NULL) SDL_DestroyAudioStream(stream);
    free(samples); end_stream(1); return result;
}

int zsharp_audio_play_wav(const char *path, double gain,
                         ZSharpAudioCancelled cancelled, void *context,
                         char *error, size_t error_size) {
    SDL_AudioDeviceID device;
    SDL_AudioSpec spec, actual;
    SDL_AudioStream *stream = NULL;
    Uint8 *buffer = NULL;
    Uint32 length = 0;
    FILE *file;
    long file_size;
    Uint64 start, drained = 0, drain_ms = 100, deadline;
    int result = 0, frames = 0;
    clear_error(error, error_size);
    if (path == NULL || path[0] == '\0' || !isfinite(gain) || gain < 0 || gain > 1)
        return audio_error(error, error_size, "speech playback needs a WAV path and gain between 0 and 1");
    if (cancelled != NULL && cancelled(context)) return audio_error(error, error_size, "audio playback was cancelled");
    file = audio_open_file(path, 0);
    if (file == NULL) return audio_error(error, error_size, "could not read the speech WAV file");
    file_size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    fclose(file);
    if (file_size < 44 || (unsigned long)file_size > AUDIO_WAV_LIMIT)
        return audio_error(error, error_size, "speech WAV file is invalid or exceeds 64 MiB");
    if (!begin_stream(0, &device, error, error_size)) return 0;
    if (!SDL_LoadWAV(path, &spec, &buffer, &length) || spec.freq <= 0 || spec.channels <= 0 ||
        SDL_AUDIO_FRAMESIZE(spec) <= 0 || length > AUDIO_WAV_LIMIT) {
        audio_error(error, error_size, "could not decode the speech WAV file"); goto done;
    }
    deadline = (Uint64)((double)length * 1000.0 / (SDL_AUDIO_FRAMESIZE(spec) * (double)spec.freq)) + 3000;
    if (deadline > 603000) {audio_error(error, error_size, "speech WAV playback exceeds ten minutes"); goto done;}
    stream = SDL_OpenAudioDeviceStream(device, &spec, NULL, NULL);
    if (stream == NULL || !SDL_SetAudioStreamGain(stream, (float)gain) ||
        !SDL_PutAudioStreamData(stream, buffer, (int)length) || !SDL_FlushAudioStream(stream) ||
        !SDL_ResumeAudioStreamDevice(stream)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not start selected speaker output: %s", SDL_GetError());
        goto done;
    }
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &actual, &frames) && actual.freq > 0 && frames > 0) {
        drain_ms = (Uint64)((double)frames * 2000.0 / actual.freq) + 20;
        if (drain_ms < 50) drain_ms = 50;
        if (drain_ms > 500) drain_ms = 500;
    }
    start = SDL_GetTicks();
    while (SDL_GetTicks() - start < deadline) {
        int queued;
        if (stream_cancelled(cancelled, context)) {audio_error(error, error_size, "audio playback was cancelled"); goto done;}
        if (SDL_GetAudioDeviceName(device) == NULL) {
            audio_error(error, error_size, "the selected speaker was disconnected during playback"); goto done;
        }
        queued = SDL_GetAudioStreamQueued(stream);
        if (queued < 0) {audio_error(error, error_size, "could not query speech playback progress"); goto done;}
        if (queued == 0) {
            if (drained == 0) drained = SDL_GetTicks();
            if (SDL_GetTicks() - drained >= drain_ms) {result = 1; break;}
        } else drained = 0;
        SDL_Delay(10);
    }
    if (!result && error != NULL && error_size != 0 && error[0] == '\0')
        audio_error(error, error_size, "speech audio playback timed out");
done:
    if (stream != NULL) SDL_DestroyAudioStream(stream);
    SDL_free(buffer); end_stream(0); return result;
}

struct ZSharpAudioPlayback {
    SDL_AudioStream *stream;
    SDL_AudioDeviceID device;
    ZSharpAudioCancelled cancelled;
    void *context;
    unsigned int sample_rate;
    size_t submitted;
    Uint64 drain_ms;
};

static int playback_ready(ZSharpAudioPlayback *playback,
                            char *error, size_t error_size) {
    if (playback == NULL || playback->stream == NULL)
        return audio_error(error, error_size, "speech audio stream is not open");
    if (stream_cancelled(playback->cancelled, playback->context))
        return audio_error(error, error_size, "audio playback was cancelled");
    if (SDL_GetAudioDeviceName(playback->device) == NULL)
        return audio_error(error, error_size,
            "the selected speaker was disconnected during playback");
    return 1;
}

ZSharpAudioPlayback *zsharp_audio_playback_open(unsigned int sample_rate,
                            double gain, ZSharpAudioCancelled cancelled,
                            void *context, char *error, size_t error_size) {
    ZSharpAudioPlayback *playback;
    SDL_AudioSpec spec, actual;
    int frames = 0;
    clear_error(error, error_size);
    if (sample_rate < 8000 || sample_rate > 192000 ||
        !isfinite(gain) || gain < 0 || gain > 1) {
        audio_error(error, error_size,
            "speech audio stream needs a sample rate of 8000..192000 Hz and gain 0..1");
        return NULL;
    }
    if (cancelled != NULL && cancelled(context)) {
        audio_error(error, error_size, "audio playback was cancelled"); return NULL;
    }
    playback = (ZSharpAudioPlayback *)calloc(1, sizeof(*playback));
    if (playback == NULL) {audio_error(error, error_size, "out of memory"); return NULL;}
    if (!begin_stream(0, &playback->device, error, error_size)) {
        free(playback); return NULL;
    }
    playback->cancelled = cancelled;
    playback->context = context;
    playback->sample_rate = sample_rate;
    playback->drain_ms = 100;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = (int)sample_rate;
    playback->stream = SDL_OpenAudioDeviceStream(playback->device, &spec, NULL, NULL);
    if (playback->stream == NULL ||
        !SDL_SetAudioStreamGain(playback->stream, (float)gain) ||
        !SDL_ResumeAudioStreamDevice(playback->stream)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not start selected speaker output: %s", SDL_GetError());
        zsharp_audio_playback_abort(playback); return NULL;
    }
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(playback->stream), &actual, &frames) &&
        actual.freq > 0 && frames > 0) {
        playback->drain_ms = (Uint64)((double)frames * 2000.0 / actual.freq) + 20;
        if (playback->drain_ms < 50) playback->drain_ms = 50;
        if (playback->drain_ms > 500) playback->drain_ms = 500;
    }
    return playback;
}

int zsharp_audio_playback_append(ZSharpAudioPlayback *playback,
                                 const int16_t *samples, size_t count,
                                 char *error, size_t error_size) {
    size_t offset = 0;
    clear_error(error, error_size);
    if (!playback_ready(playback, error, error_size)) return 0;
    if ((samples == NULL && count != 0) ||
        count > (size_t)playback->sample_rate * 600u - playback->submitted)
        return audio_error(error, error_size,
            "speech PCM is invalid or exceeds the ten-minute playback limit");
    while (offset < count) {
        size_t chunk = count - offset;
        Uint64 progress_at = SDL_GetTicks();
        int previous = -1;
        if (chunk > playback->sample_rate / 10u) chunk = playback->sample_rate / 10u;
        for (;;) {
            int queued;
            if (!playback_ready(playback, error, error_size)) return 0;
            queued = SDL_GetAudioStreamQueued(playback->stream);
            if (queued < 0)
                return audio_error(error, error_size, "could not query speech playback progress");
            if ((size_t)queued + chunk * sizeof(*samples) <=
                (size_t)playback->sample_rate * sizeof(*samples) * 2u) break;
            if (previous != queued) {progress_at = SDL_GetTicks(); previous = queued;}
            if (SDL_GetTicks() - progress_at >= 3000)
                return audio_error(error, error_size, "speech audio stream stopped making progress");
            SDL_Delay(5);
        }
        if (!SDL_PutAudioStreamData(playback->stream, samples + offset,
                                     (int)(chunk * sizeof(*samples))))
            return audio_error(error, error_size, "could not append synthesized speech audio");
        offset += chunk;
        playback->submitted += chunk;
    }
    return 1;
}

int zsharp_audio_playback_finish(ZSharpAudioPlayback *playback,
                                 char *error, size_t error_size) {
    Uint64 started, drained = 0;
    int result = 0;
    clear_error(error, error_size);
    if (!playback_ready(playback, error, error_size)) goto done;
    if (!SDL_FlushAudioStream(playback->stream)) {
        audio_error(error, error_size, "could not flush synthesized speech audio"); goto done;
    }
    started = SDL_GetTicks();
    /* append limits pending audio to two seconds. This deadline includes a
     * bounded output-device tail and grace, never the whole text duration. */
    while (SDL_GetTicks() - started < 5000) {
        int queued;
        if (!playback_ready(playback, error, error_size)) goto done;
        queued = SDL_GetAudioStreamQueued(playback->stream);
        if (queued < 0) {
            audio_error(error, error_size, "could not query speech playback progress"); goto done;
        }
        if (queued == 0) {
            if (drained == 0) drained = SDL_GetTicks();
            if (SDL_GetTicks() - drained >= playback->drain_ms) {result = 1; break;}
        } else drained = 0;
        SDL_Delay(5);
    }
    if (!result) audio_error(error, error_size, "speech audio stream drain timed out");
done:
    zsharp_audio_playback_abort(playback);
    return result;
}

void zsharp_audio_playback_abort(ZSharpAudioPlayback *playback) {
    if (playback == NULL) return;
    if (playback->stream != NULL) SDL_DestroyAudioStream(playback->stream);
    end_stream(0);
    free(playback);
}

#else
static int unavailable(char *error, size_t size) {
    return audio_error(error, size, "audio device/capture support was not included in this ZVM build (SDL3 is required)");
}
int zsharp_audio_prepare(char *error, size_t size) {return unavailable(error, size);}
void zsharp_audio_cancel_all(void) {}
void zsharp_audio_shutdown(void) {}
int zsharp_audio_list_devices(int recording, ZSharpAudioDevice **devices, size_t *count, char *error, size_t size) {
    (void)recording; if (devices != NULL) *devices = NULL; if (count != NULL) *count = 0;
    return unavailable(error, size);
}
char *zsharp_audio_selected_input(char *error, size_t size) {unavailable(error, size); return NULL;}
char *zsharp_audio_selected_output(char *error, size_t size) {unavailable(error, size); return NULL;}
int zsharp_audio_select_input(const char *name, char *error, size_t size) {(void)name; return unavailable(error, size);}
int zsharp_audio_select_output(const char *name, char *error, size_t size) {(void)name; return unavailable(error, size);}
double zsharp_audio_level(void) {return 0.0;}
static int record_to_destination(AudioDestination *destination, double seconds, int utterance,
                                   ZSharpAudioCancelled cancelled, void *context, char *error, size_t size) {
    (void)destination; (void)seconds; (void)utterance;
    if (cancelled != NULL && cancelled(context))
        return audio_error(error, size, "microphone recording was cancelled");
    return unavailable(error, size);
}
int zsharp_audio_play_wav(const char *path, double gain, ZSharpAudioCancelled cancelled, void *context, char *error, size_t size) {
    (void)path; (void)gain; (void)cancelled; (void)context; return unavailable(error, size);
}
ZSharpAudioPlayback *zsharp_audio_playback_open(unsigned int sample_rate,
                            double gain, ZSharpAudioCancelled cancelled,
                            void *context, char *error, size_t size) {
    (void)context;
    clear_error(error, size);
    if (sample_rate < 8000 || sample_rate > 192000 || !isfinite(gain) || gain < 0 || gain > 1) {
        audio_error(error, size, "speech audio stream needs a sample rate of 8000..192000 Hz and gain 0..1");
        return NULL;
    }
    if (cancelled != NULL && cancelled(context))
        audio_error(error, size, "audio playback was cancelled");
    else unavailable(error, size);
    return NULL;
}
int zsharp_audio_playback_append(ZSharpAudioPlayback *playback, const int16_t *samples,
                                 size_t count, char *error, size_t size) {
    (void)playback; (void)samples; (void)count; return unavailable(error, size);
}
int zsharp_audio_playback_finish(ZSharpAudioPlayback *playback, char *error, size_t size) {
    (void)playback; return unavailable(error, size);
}
void zsharp_audio_playback_abort(ZSharpAudioPlayback *playback) {(void)playback;}
#endif

static int record_mode_path(const char *path, double seconds, int utterance,
                              ZSharpAudioCancelled cancelled, void *context,
                              char *error, size_t error_size) {
    AudioDestination destination;
    int result;
    clear_error(error, error_size);
    if (!isfinite(seconds) || seconds < 0.05 || seconds > AUDIO_MAX_SECONDS)
        return audio_error(error, error_size, "recording duration must be 0.05..30 seconds");
    if (cancelled != NULL && cancelled(context))
        return audio_error(error, error_size, "microphone recording was cancelled");
    if (!destination_path(path, &destination, error, error_size)) return 0;
    result = record_to_destination(&destination, seconds, utterance, cancelled,
                                     context, error, error_size);
    destination_close(&destination); return result;
}

int zsharp_audio_record_path(const char *path, double seconds,
                             ZSharpAudioCancelled cancelled, void *context,
                             char *error, size_t error_size) {
    return record_mode_path(path, seconds, 0, cancelled, context, error, error_size);
}

int zsharp_audio_record(const char *project_root, const char *relative_path,
                        double seconds, ZSharpAudioCancelled cancelled,
                        void *context, char *error, size_t error_size) {
    AudioDestination destination;
    int result;
    clear_error(error, error_size);
    if (!isfinite(seconds) || seconds < 0.05 || seconds > AUDIO_MAX_SECONDS)
        return audio_error(error, error_size, "recording duration must be 0.05..30 seconds");
    if (!destination_open(project_root, relative_path, &destination, error, error_size)) return 0;
    result = record_to_destination(&destination, seconds, 0, cancelled,
                                     context, error, error_size);
    destination_close(&destination); return result;
}

int zsharp_audio_capture_temp(double seconds, ZSharpAudioCancelled cancelled,
                              void *context, char *output_path,
                              size_t output_capacity, char *error,
                              size_t error_size) {
    char *path = NULL;
    int result = 0;
    clear_error(error, error_size);
    if (output_path == NULL || output_capacity == 0)
        return audio_error(error, error_size, "speech capture needs an output path buffer");
    output_path[0] = '\0';
    if (!isfinite(seconds) || seconds < 0.05 || seconds > AUDIO_MAX_SECONDS)
        return audio_error(error, error_size, "recording duration must be 0.05..30 seconds");
    if (cancelled != NULL && cancelled(context))
        return audio_error(error, error_size, "microphone recording was cancelled");
#ifdef _WIN32
    {
        wchar_t directory[32768], filename[32768];
        DWORD length = GetTempPathW((DWORD)(sizeof(directory) / sizeof(*directory)), directory);
        int needed;
        if (length == 0 || length >= sizeof(directory) / sizeof(*directory) ||
            GetTempFileNameW(directory, L"zsp", 0, filename) == 0)
            return audio_error(error, error_size, "could not reserve a unique speech recording temporary file");
        needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename,
                                       -1, NULL, 0, NULL, NULL);
        if (needed <= 0 || (size_t)needed > output_capacity ||
            (path = (char *)malloc((size_t)needed)) == NULL) {
            DeleteFileW(filename);
            return audio_error(error, error_size, "speech recording temporary path exceeds the output buffer");
        }
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename, -1,
                                  path, needed, NULL, NULL)) {
            DeleteFileW(filename); free(path);
            return audio_error(error, error_size, "could not encode the speech recording temporary path");
        }
    }
#else
    {
        const char *directory = getenv("TMPDIR");
        size_t length;
        int file;
        if (directory == NULL || directory[0] != '/') directory = "/tmp";
        length = strlen(directory);
        if (length > 32768 || length + 32 > output_capacity)
            return audio_error(error, error_size, "speech recording temporary path exceeds the output buffer");
        path = (char *)malloc(length + 32);
        if (path == NULL) return audio_error(error, error_size, "out of memory");
        snprintf(path, length + 32, "%s/zsharp-speech-XXXXXX", directory);
        file = mkstemp(path);
        if (file < 0) {free(path); return audio_error(error, error_size, "could not reserve a unique private speech recording file");}
        result = fchmod(file, S_IRUSR | S_IWUSR);
        if (close(file) != 0) result = -1;
        if (result != 0) {
            audio_remove_file(path); free(path);
            return audio_error(error, error_size, "could not secure the speech recording temporary file");
        }
        result = 0;
    }
#endif
    if (record_mode_path(path, seconds, 1, cancelled, context, error, error_size)) {
        size_t length = strlen(path) + 1;
        if (length <= output_capacity) {memcpy(output_path, path, length); result = 1;}
        else audio_error(error, error_size, "speech recording temporary path exceeds the output buffer");
    }
    if (!result) audio_remove_file(path);
    free(path); return result;
}
