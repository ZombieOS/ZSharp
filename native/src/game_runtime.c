#define _CRT_SECURE_NO_WARNINGS

#include "game_runtime.h"

#include "game_model.h"
#include "game_vulkan.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZSHARP_HAS_GAME_RUNTIME

#include <SDL3/SDL.h>

typedef struct ZSharpGameState {
    SDL_Window *window;
    ZSharpGameVulkan *renderer;
    SDL_Gamepad *gamepad;
    ZSharpGameModel model;
    SDL_Mutex *model_mutex;
    int cancelled;
} ZSharpGameState;

static void game_error(char *error, size_t error_size, const char *message) {
    if (error == NULL || error_size == 0) return;
    snprintf(error, error_size, "%s", message == NULL ? "game error" : message);
}

static int game_owns_property(void *state, const char *path) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    int owns;
    SDL_LockMutex(game->model_mutex);
    owns = zsharp_game_model_owns_property(&game->model, path);
    SDL_UnlockMutex(game->model_mutex);
    return owns;
}

static int game_get_property(void *state, const char *path,
                             ZSharpWindowReadType *type, char **text,
                             char *error, size_t error_size) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    int ok;
    SDL_LockMutex(game->model_mutex);
    ok = zsharp_game_model_get_property(&game->model, path, type, text,
                                        error, error_size);
    SDL_UnlockMutex(game->model_mutex);
    return ok;
}

static int game_set_property(void *state, const char *path,
                             ZSharpWindowValueType value_type,
                             const char *value, ZSharpUIUnit unit,
                             char *error, size_t error_size) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    int ok;
    (void)unit;
    SDL_LockMutex(game->model_mutex);
    ok = zsharp_game_model_set_property(&game->model, path, value_type, value,
                                        error, error_size);
    SDL_UnlockMutex(game->model_mutex);
    return ok;
}

static int game_wait(void *state, const char *milliseconds_text,
                     char *error, size_t error_size) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    const char *input = milliseconds_text == NULL ? "" : milliseconds_text;
    char *end = NULL;
    double parsed = strtod(input, &end);
    unsigned long long milliseconds;
    unsigned long long remaining;
    if (end == input || end == NULL || *end != '\0' || !isfinite(parsed) ||
        parsed < 0.0 || parsed > 604800000.0) {
        game_error(error, error_size,
                   "game wait must be between 0ms and 604800000ms");
        return 0;
    }
    milliseconds = (unsigned long long)(parsed + 0.999);
    remaining = milliseconds;
    while (!game->cancelled && remaining != 0) {
        Uint32 slice = remaining > 10u ? 10u : (Uint32)remaining;
        SDL_Delay(slice);
        remaining -= slice;
    }
    return 1;
}

static int game_cancelled(void *state) {
    return ((ZSharpGameState *)state)->cancelled;
}

static unsigned long long auto_close_after(void) {
    const char *value = getenv("ZSHARP_GAME_AUTOCLOSE_MS");
    char *end = NULL;
    unsigned long long result;
    if (value == NULL || value[0] == '\0') return 0;
    result = strtoull(value, &end, 10);
    return end == value || *end != '\0' ? 0 : result;
}

static int safe_asset_path(const char *path) {
    return path != NULL && path[0] != '\0' && path[0] != '/' &&
           path[0] != '\\' && !(isalpha((unsigned char)path[0]) &&
                                  path[1] == ':') &&
           strstr(path, "..") == NULL;
}

static char *join_project_path(const char *root, const char *relative) {
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    int separator = root_length != 0 && root[root_length - 1] != '/' &&
                    root[root_length - 1] != '\\';
    char *path = (char *)malloc(root_length + (size_t)separator +
                               relative_length + 1);
    if (path == NULL) return NULL;
    memcpy(path, root, root_length);
    if (separator) path[root_length++] = '/';
    memcpy(path + root_length, relative, relative_length + 1);
    return path;
}

static int load_audio(ZSharpGameState *game, char *error,
                      size_t error_size) {
    size_t index;
    for (index = 0; index < game->model.object_count; index++) {
        ZSharpGameObject *object = &game->model.objects[index];
        SDL_AudioSpec spec;
        Uint8 *buffer = NULL;
        Uint32 length = 0;
        SDL_AudioStream *stream;
        char *path;
        if (object->audio_path == NULL && object->tone_frequency <= 0.0f)
            continue;
        if (object->audio_path == NULL) {
            size_t sample_count = (size_t)(48000.0f * object->tone_duration);
            Sint16 *samples;
            size_t sample_index;
            if (object->tone_duration <= 0.0f ||
                object->tone_duration > 10.0f || sample_count == 0) {
                if (error != NULL && error_size != 0)
                    snprintf(error, error_size,
                             "game object '%s' has an invalid toneDuration",
                             object->name);
                return 0;
            }
            samples = (Sint16 *)SDL_malloc(sample_count * sizeof(*samples));
            if (samples == NULL) {
                game_error(error, error_size, "out of memory");
                return 0;
            }
            for (sample_index = 0; sample_index < sample_count;
                 sample_index++) {
                double fade = 1.0 - (double)sample_index /
                                      (double)sample_count;
                samples[sample_index] = (Sint16)(
                    sin(6.283185307179586 * object->tone_frequency *
                        (double)sample_index / 48000.0) * 5000.0 * fade);
            }
            SDL_zero(spec);
            spec.format = SDL_AUDIO_S16;
            spec.channels = 1;
            spec.freq = 48000;
            stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (stream == NULL) {
                SDL_free(samples);
                game_error(error, error_size, SDL_GetError());
                return 0;
            }
            object->audio_stream = stream;
            object->audio_buffer = samples;
            object->audio_length =
                (unsigned)(sample_count * sizeof(*samples));
            SDL_SetAudioStreamGain(stream, object->audio_volume);
            if (object->audio_autoplay) object->audio_started = 1;
            continue;
        }
        if (!safe_asset_path(object->audio_path)) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' has an unsafe audio path",
                         object->name);
            return 0;
        }
        path = join_project_path(game->model.project_root,
                                 object->audio_path);
        if (path == NULL) {
            game_error(error, error_size, "out of memory");
            return 0;
        }
        if (!SDL_LoadWAV(path, &spec, &buffer, &length)) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "could not load audio for '%s': %s", object->name,
                         SDL_GetError());
            free(path);
            return 0;
        }
        free(path);
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &spec, NULL, NULL);
        if (stream == NULL) {
            SDL_free(buffer);
            game_error(error, error_size, SDL_GetError());
            return 0;
        }
        SDL_SetAudioStreamGain(stream, object->audio_volume);
        object->audio_stream = stream;
        object->audio_buffer = buffer;
        object->audio_length = length;
        if (object->audio_autoplay) object->audio_started = 1;
    }
    return 1;
}

static void unload_audio(ZSharpGameState *game) {
    size_t index;
    for (index = 0; index < game->model.object_count; index++) {
        ZSharpGameObject *object = &game->model.objects[index];
        if (object->audio_stream != NULL)
            SDL_DestroyAudioStream((SDL_AudioStream *)object->audio_stream);
        if (object->audio_buffer != NULL) SDL_free(object->audio_buffer);
        object->audio_stream = NULL;
        object->audio_buffer = NULL;
    }
}

static void update_audio(ZSharpGameState *game) {
    size_t index;
    for (index = 0; index < game->model.object_count; index++) {
        ZSharpGameObject *object = &game->model.objects[index];
        SDL_AudioStream *stream = (SDL_AudioStream *)object->audio_stream;
        int collision_started = object->audio_on_collision &&
                                object->colliding && !object->was_colliding;
        if (stream == NULL) continue;
        SDL_SetAudioStreamGain(stream, object->audio_volume);
        if (object->audio_started || collision_started) {
            SDL_ClearAudioStream(stream);
            SDL_PutAudioStreamData(stream, object->audio_buffer,
                                   (int)object->audio_length);
            SDL_ResumeAudioStreamDevice(stream);
            object->audio_started = 0;
        } else if (object->audio_loop &&
                   SDL_GetAudioStreamQueued(stream) <
                       (int)(object->audio_length / 3u)) {
            SDL_PutAudioStreamData(stream, object->audio_buffer,
                                   (int)object->audio_length);
            SDL_ResumeAudioStreamDevice(stream);
        }
        object->was_colliding = object->colliding;
    }
}

static void refresh_input(ZSharpGameState *game) {
    const bool *keyboard = SDL_GetKeyboardState(NULL);
    Sint16 horizontal = 0;
    Sint16 vertical = 0;
    int pad_left = 0;
    int pad_right = 0;
    int pad_up = 0;
    int pad_down = 0;
    if (game->gamepad != NULL) {
        horizontal = SDL_GetGamepadAxis(game->gamepad,
                                        SDL_GAMEPAD_AXIS_LEFTX);
        vertical = SDL_GetGamepadAxis(game->gamepad,
                                      SDL_GAMEPAD_AXIS_LEFTY);
        pad_left = horizontal < -8000 || SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        pad_right = horizontal > 8000 || SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        pad_up = vertical < -8000 || SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        pad_down = vertical > 8000 || SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    game->model.input.left = keyboard[SDL_SCANCODE_A] ||
                             keyboard[SDL_SCANCODE_LEFT] || pad_left;
    game->model.input.right = keyboard[SDL_SCANCODE_D] ||
                              keyboard[SDL_SCANCODE_RIGHT] || pad_right;
    game->model.input.up = keyboard[SDL_SCANCODE_W] ||
                           keyboard[SDL_SCANCODE_UP] || pad_up;
    game->model.input.down = keyboard[SDL_SCANCODE_S] ||
                             keyboard[SDL_SCANCODE_DOWN] || pad_down;
    game->model.input.space = keyboard[SDL_SCANCODE_SPACE] ||
        (game->gamepad != NULL && SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
    game->model.input.action = keyboard[SDL_SCANCODE_E] ||
                               keyboard[SDL_SCANCODE_RETURN] ||
        (game->gamepad != NULL && SDL_GetGamepadButton(
            game->gamepad, SDL_GAMEPAD_BUTTON_EAST));
}

int zsharp_game_runtime_available(void) {
    return 1;
}

const char *zsharp_game_runtime_backend(void) {
#if defined(__APPLE__)
    return "SDL3 + Vulkan (macOS path experimental; MoltenVK bundled)";
#else
    return "SDL3 + Vulkan";
#endif
}

int zsharp_game_run(const char *title, const char *project_root, int is_3d,
                    ZSharpWindowCallback callback, void *user_data,
                    char *error, size_t error_size) {
    ZSharpGameState game;
    ZSharpWindowRuntime runtime;
    SDL_Event event;
    Uint64 started;
    Uint64 previous;
    unsigned long long close_after = auto_close_after();
    int tasks_started = 0;
    int resized = 0;
    int ok = 0;
    double accumulator = 0.0;
    char window_title[512];
    memset(&game, 0, sizeof(game));
    memset(&runtime, 0, sizeof(runtime));
    if (getenv("ZSHARP_GAME_FORCE_FAILURE") != NULL) {
        game_error(error, error_size, "forced game launch failure");
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        game_error(error, error_size, SDL_GetError());
        return 0;
    }
    if (!zsharp_game_model_load(project_root, is_3d, &game.model, error,
                                error_size)) goto done;
    game.model_mutex = SDL_CreateMutex();
    if (game.model_mutex == NULL) {
        game_error(error, error_size, SDL_GetError());
        goto done;
    }
    snprintf(window_title, sizeof(window_title), "%s - Z# %s Game",
             title == NULL || title[0] == '\0' ? "Z# Game" : title,
             is_3d ? "3D" : "2D");
    game.window = SDL_CreateWindow(window_title, 1280, 720,
                                   SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (game.window == NULL) {
        game_error(error, error_size, SDL_GetError());
        goto done;
    }
    game.renderer = zsharp_game_vulkan_create(game.window, error, error_size);
    if (game.renderer == NULL || !load_audio(&game, error, error_size))
        goto done;
    {
        int gamepad_count = 0;
        SDL_JoystickID *gamepads = SDL_GetGamepads(&gamepad_count);
        if (gamepads != NULL && gamepad_count > 0)
            game.gamepad = SDL_OpenGamepad(gamepads[0]);
        SDL_free(gamepads);
    }
    runtime.state = &game;
    runtime.get_property = game_get_property;
    runtime.owns_property = game_owns_property;
    runtime.set_property = game_set_property;
    runtime.wait = game_wait;
    runtime.is_cancelled = game_cancelled;
    if (callback != NULL && !callback(user_data, ZSHARP_WINDOW_PROJECT_STARTS,
                                      &runtime, error, error_size)) goto done;
    tasks_started = callback != NULL;
    started = previous = SDL_GetTicks();
    while (!game.cancelled) {
        Uint64 now;
        double delta;
        ZSharpGameRenderFrame frame;
        ZSharpGameRenderObject *objects = NULL;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                game.cancelled = 1;
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                     event.type == SDL_EVENT_WINDOW_RESIZED)
                resized = 1;
            else if (event.type == SDL_EVENT_KEY_DOWN ||
                     event.type == SDL_EVENT_KEY_UP) {
                int pressed = event.type == SDL_EVENT_KEY_DOWN;
                if (pressed && event.key.key == SDLK_ESCAPE)
                    game.cancelled = 1;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                int width = 1280;
                int height = 720;
                SDL_GetWindowSize(game.window, &width, &height);
                SDL_LockMutex(game.model_mutex);
                game.model.input.mouse_x =
                    width > 0 ? event.motion.x * 1280.0f / (float)width - 640.0f
                              : 0.0f;
                game.model.input.mouse_y =
                    height > 0 ? 360.0f - event.motion.y * 720.0f /
                                              (float)height
                               : 0.0f;
                SDL_UnlockMutex(game.model_mutex);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                int pressed = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                SDL_LockMutex(game.model_mutex);
                if (event.button.button == SDL_BUTTON_LEFT)
                    game.model.input.mouse_left = pressed;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    game.model.input.mouse_right = pressed;
                SDL_UnlockMutex(game.model_mutex);
            }
        }
        now = SDL_GetTicks();
        delta = (double)(now - previous) / 1000.0;
        previous = now;
        if (close_after != 0 && now - started >= close_after)
            game.cancelled = 1;
        if (game.cancelled) break;
        SDL_LockMutex(game.model_mutex);
        refresh_input(&game);
        accumulator += fmin(delta, 0.25);
        while (accumulator >= 1.0 / 120.0) {
            zsharp_game_model_update(&game.model, 1.0 / 120.0);
            accumulator -= 1.0 / 120.0;
        }
        update_audio(&game);
        zsharp_game_model_frame(&game.model, &frame, &objects);
        SDL_UnlockMutex(game.model_mutex);
        if (frame.object_count != 0 && objects == NULL) {
            game_error(error, error_size, "out of memory");
            goto done;
        }
        if (!zsharp_game_vulkan_draw(game.renderer, resized, &frame,
                                     error, error_size)) {
            free(objects);
            goto done;
        }
        free(objects);
        resized = 0;
        SDL_Delay(1);
    }
    ok = 1;
done:
    game.cancelled = 1;
    if (tasks_started && callback != NULL) {
        char stop_error[512] = {0};
        if (!callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &runtime,
                      stop_error, sizeof(stop_error))) {
            if (ok)
                snprintf(error, error_size, "%s",
                         stop_error[0] != '\0'
                             ? stop_error
                             : "could not stop the game tasks");
            ok = 0;
        }
    }
    unload_audio(&game);
    if (game.gamepad != NULL) SDL_CloseGamepad(game.gamepad);
    zsharp_game_vulkan_destroy(game.renderer);
    if (game.window != NULL) SDL_DestroyWindow(game.window);
    if (game.model_mutex != NULL) SDL_DestroyMutex(game.model_mutex);
    zsharp_game_model_free(&game.model);
    SDL_Quit();
    return ok;
}

#else

int zsharp_game_runtime_available(void) {
    return 0;
}

const char *zsharp_game_runtime_backend(void) {
    return "unavailable";
}

int zsharp_game_run(const char *title, const char *project_root, int is_3d,
                    ZSharpWindowCallback callback, void *user_data,
                    char *error, size_t error_size) {
    (void)title;
    (void)project_root;
    (void)is_3d;
    (void)callback;
    (void)user_data;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size,
                 "this ZVM was built without the zsharpgame runtime");
    return 0;
}

#endif
