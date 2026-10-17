#define _CRT_SECURE_NO_WARNINGS

#include "game_runtime.h"

#include "achievement.h"
#include "game_model.h"
#include "game_input_edges.h"
#include "gamepad_input.h"
#include "game_vulkan.h"
#include "registry.h"
#include "settings.h"
#include "terminal.h"
#include "xr_input.h"
#ifdef ZSHARP_HAS_OPENXR
#include "game_xr.h"
#include "xr_ui.h"
#endif

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
    ZSharpGamepadState gamepad_input;
    int window_focused;
    int gamepad_focused;
    uint64_t gamepad_vibration_serial;
    int gamepad_test;
    SDL_JoystickID test_gamepad_id;
    SDL_Joystick *test_gamepad_joystick;
    unsigned test_gamepad_rumbles;
    Uint16 test_gamepad_rumble_strength;
    ZSharpGameModel model;
    SDL_Mutex *model_mutex;
    const ZSharpSettings *settings;
    char achievement_title[256];
    char achievement_description[512];
    Uint64 achievement_until;
    SDL_AudioStream *achievement_audio;
    Sint16 *achievement_samples;
    int achievement_sample_bytes;
    char *window_icon_path;
    int mouse_capture_dirty;
    int cancelled;
    ZSharpMouseEdges mouse_edges;
    ZSharpXRState xr;
#ifdef ZSHARP_HAS_OPENXR
    ZSharpGameXR *xr_backend;
    ZSharpXRUIActivation xr_ui_activation[2];
    uint64_t xr_ui_generation;
    uint64_t xr_ui_trace_pressed[2];
    unsigned xr_ui_test_phase;
    int xr_ui_trace, xr_ui_test;
#endif
} ZSharpGameState;

#ifdef _MSC_VER
static __declspec(thread) ZSharpMouseCursor mouse_cursor;
static __declspec(thread) ZSharpXRCursor xr_cursor;
static __declspec(thread) ZSharpGamepadCursor gamepad_cursor;
#else
static _Thread_local ZSharpMouseCursor mouse_cursor;
static _Thread_local ZSharpXRCursor xr_cursor;
static _Thread_local ZSharpGamepadCursor gamepad_cursor;
#endif

static void game_begin_task(void *state) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    SDL_LockMutex(game->model_mutex);
    zsharp_mouse_cursor_begin(&mouse_cursor, &game->mouse_edges);
    zsharp_xr_cursor_begin(&xr_cursor, &game->xr);
    zsharp_gamepad_cursor_begin(&gamepad_cursor, &game->gamepad_input);
    SDL_UnlockMutex(game->model_mutex);
}

static void game_request_close(void *data) {
    ZSharpGameState *game = (ZSharpGameState *)data;
    if (game != NULL) game->cancelled = 1;
}

static void game_error(char *error, size_t error_size, const char *message);
static char *join_project_path(const char *root, const char *relative);
static void refresh_input(ZSharpGameState *game);

#ifdef ZSHARP_HAS_OPENXR
/* Use the exact button IDs selected by the just-rendered stereo frame. The
   snapshot-to-model mapping is O(1), independent of the scene's placement count.
   No script executes under model_mutex and no model pointer escapes the lock. */
static int dispatch_xr_buttons(ZSharpGameState *game,
                               const ZSharpGameRenderFrame *frame,
                               const ZSharpXRState *snapshot,
                               const size_t hovered[2],
                               uint64_t scene_generation,
                               ZSharpWindowCallback callback, void *user_data,
                               const ZSharpWindowRuntime *runtime,
                               char *error, size_t error_size) {
    char *targets[2] = {NULL, NULL};
    char trace[2][384] = {{0}};
    int hand, failed = 0;
    SDL_LockMutex(game->model_mutex);
    if (game->model.scene_generation != scene_generation ||
        game->xr_ui_generation != scene_generation) {
        for (hand = 0; hand < 2; ++hand)
            zsharp_xr_ui_activation_reset(&game->xr_ui_activation[hand],
                snapshot->hands[hand].pressed[ZXR_TRIGGER]);
        game->xr_ui_generation = game->model.scene_generation;
        game->model.xr_hovered_buttons[0] = game->model.xr_hovered_buttons[1] = 0;
        if (game->model.scene_generation != scene_generation) {
            SDL_UnlockMutex(game->model_mutex);
            return 1;
        }
    }
    for (hand = 0; hand < 2; ++hand) {
        const ZSharpXRHand *input = &snapshot->hands[hand];
        int valid = snapshot->active && snapshot->focused && input->aim_tracked;
        size_t render_id = valid ? hovered[hand] : 0;
        size_t activated;
        int armed = game->xr_ui_activation[hand].armed;
        const ZSharpGameObject *button = NULL;
        size_t model_index = 0;
        if (render_id > 0 && render_id <= frame->object_count) {
            const ZSharpGameRenderObject *rendered = &frame->objects[render_id - 1];
            model_index = rendered->model_index;
            if (rendered->render_id == render_id && model_index > 0 &&
                model_index <= game->model.object_count) {
                button = &game->model.objects[model_index - 1];
                if (button->shape != ZGAME_SHAPE_BUTTON || !button->visible ||
                    button->opacity <= 0.0f || !button->scene ||
                    strcmp(button->scene, game->model.active_scene) != 0)
                    button = NULL;
            }
        }
        if (button == NULL) render_id = model_index = 0;
        game->model.xr_hovered_buttons[hand] = model_index;
        activated = zsharp_xr_ui_activation_update(&game->xr_ui_activation[hand], valid,
                input->held[ZXR_TRIGGER], input->pressed[ZXR_TRIGGER], render_id);
        if (game->xr_ui_trace && input->pressed[ZXR_TRIGGER] != game->xr_ui_trace_pressed[hand]) {
            const char *name = button ? (button->instance_id ? button->instance_id :
                button->display_name ? button->display_name : button->name) : "none";
            snprintf(trace[hand],sizeof(trace[hand]),
                "[XR UI] %s trigger: focus=%d aim=%d hit=%.72s armed=%d fire=%d",
                hand ? "Right" : "Left",snapshot->focused,input->aim_tracked,
                name ? name : "none",armed,activated != 0);
        }
        game->xr_ui_trace_pressed[hand] = input->pressed[ZXR_TRIGGER];
        if (activated) {
            game->model.focused_button = model_index;
            if (button->click_left != NULL) {
                targets[hand] = zsharp_copy_text(button->click_left, strlen(button->click_left));
                if (targets[hand] == NULL) {
                    game_error(error, error_size, "out of memory scheduling XR game button");
                    failed = 1;
                }
            }
        }
    }
    SDL_UnlockMutex(game->model_mutex);
    for (hand = 0; hand < 2; ++hand) {
        int current_scene;
        if (trace[hand][0]) zsharp_terminal_print(trace[hand],0);
        if (targets[hand] == NULL) continue;
        SDL_LockMutex(game->model_mutex);
        current_scene = game->model.scene_generation == scene_generation;
        SDL_UnlockMutex(game->model_mutex);
        /* A first controller's callback may have switched scenes. Do not enqueue
           the other controller's old-scene callback in that case. */
        if (!failed && current_scene && callback != NULL &&
            !callback(user_data, targets[hand], runtime, error, error_size)) failed = 1;
        free(targets[hand]);
    }
    return !failed;
}

/* Opt-in headless input fixture exercises the real dispatcher and VM callbacks
   in desktop mode. Ordinary games never receive synthetic controller input. */
static int test_xr_buttons(ZSharpGameState *game,const ZSharpGameRenderFrame *frame,
    uint64_t generation,ZSharpWindowCallback callback,void *user_data,
    const ZSharpWindowRuntime *runtime,char *error,size_t error_size) {
    static const uint64_t left[]  ={0,1,1,1,1,1,2,3,3,3,3,3,4,4,4,5};
    static const uint64_t right[] ={0,1,1,1,1,1,2,3,3,3,4,4,5,5,5,5};
    static const int held_left[] ={0,1,1,1,1,0,1,1,1,0,0,0,1,1,0,1};
    static const int held_right[]={0,1,1,1,1,0,1,1,1,0,1,0,1,1,0,0};
    unsigned phase=game->xr_ui_test_phase++;
    size_t hovered[2]={0,0},found=0,index;
    ZSharpXRState snapshot={0};
    if(phase>=sizeof(left)/sizeof(left[0]))return 1;
    for(index=0;index<frame->object_count&&found<2;index++)
        if(frame->objects[index].shape==ZGAME_SHAPE_BUTTON)
            hovered[found++]=frame->objects[index].render_id;
    if(found!=2){game_error(error,error_size,"XR UI fixture needs two buttons");return 0;}
    snapshot.active=1;snapshot.focused=phase!=3;
    for(int hand=0;hand<2;hand++)snapshot.hands[hand].aim_tracked=1;
    snapshot.hands[0].pressed[ZXR_TRIGGER]=left[phase];
    snapshot.hands[1].pressed[ZXR_TRIGGER]=right[phase];
    snapshot.hands[0].held[ZXR_TRIGGER]=held_left[phase];
    snapshot.hands[1].held[ZXR_TRIGGER]=held_right[phase];
    if(phase==12)hovered[0]=hovered[1]=0; /* Hold and enter must not click. */
    if(phase==7)generation--; /* Stale scene result must not dispatch. */
    return dispatch_xr_buttons(game,frame,&snapshot,hovered,generation,
        callback,user_data,runtime,error,error_size);
}
#endif

static int set_game_icon(ZSharpGameState *game, const char *relative,
                         char *error, size_t error_size) {
    char *path;
    SDL_Surface *surface;
    if (relative == NULL || relative[0] == '\0') return 1;
    if (game->window_icon_path != NULL &&
        strcmp(game->window_icon_path, relative) == 0) return 1;
    path = join_project_path(game->model.project_root, relative);
    if (path == NULL) {
        game_error(error, error_size, "out of memory");
        return 0;
    }
    surface = SDL_LoadPNG(path);
    if (surface == NULL) surface = SDL_LoadBMP(path);
    free(path);
    if (surface == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not load game icon '%s': %s",
                     relative, SDL_GetError());
        return 0;
    }
    SDL_SetWindowIcon(game->window, surface);
    SDL_DestroySurface(surface);
    free(game->window_icon_path);
    game->window_icon_path = zsharp_copy_text(relative, strlen(relative));
    return game->window_icon_path != NULL;
}

static void play_achievement_sound(ZSharpGameState *game) {
    if (game->achievement_audio == NULL) {
        SDL_AudioSpec spec;
        size_t count = 48000u / 3u;
        size_t index;
        game->achievement_samples = (Sint16 *)SDL_malloc(
            count * sizeof(*game->achievement_samples));
        if (game->achievement_samples == NULL) return;
        for (index = 0; index < count; index++) {
            double frequency = index < count / 2 ? 659.25 : 987.77;
            double fade = 1.0 - (double)index / (double)count;
            game->achievement_samples[index] = (Sint16)(
                sin(6.283185307179586 * frequency * (double)index / 48000.0) *
                6000.0 * fade);
        }
        SDL_zero(spec);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = 48000;
        game->achievement_audio = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        game->achievement_sample_bytes = (int)(count * sizeof(Sint16));
        if (game->achievement_audio == NULL) {
            SDL_free(game->achievement_samples);
            game->achievement_samples = NULL;
            return;
        }
        SDL_SetAudioStreamGain(game->achievement_audio, 0.45f);
    }
    SDL_ClearAudioStream(game->achievement_audio);
    SDL_PutAudioStreamData(game->achievement_audio, game->achievement_samples,
                           game->achievement_sample_bytes);
    SDL_ResumeAudioStreamDevice(game->achievement_audio);
}

static void game_error(char *error, size_t error_size, const char *message) {
    if (error == NULL || error_size == 0) return;
    snprintf(error, error_size, "%s", message == NULL ? "game error" : message);
}

static int show_splashes(ZSharpGameState *game, const ZSharpSettings *settings,
                         char *error, size_t error_size) {
    size_t index;
    for (index = 0; index < settings->splash_count; index++) {
        Uint64 started = SDL_GetTicks();
        Uint64 duration = (Uint64)(settings->splashes[index].duration_seconds *
                                   1000.0);
        while (!game->cancelled && SDL_GetTicks() - started < duration) {
            SDL_Event event;
            int width = 1280;
            int height = 720;
            int resized = 0;
            ZSharpGameRenderObject object;
            ZSharpGameRenderFrame frame;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) game->cancelled = 1;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) resized = 1;
            }
            SDL_GetWindowSizeInPixels(game->window, &width, &height);
            memset(&object, 0, sizeof(object));
            object.shape = ZGAME_SHAPE_SPRITE;
            object.width = (float)width;
            object.height = (float)height;
            object.scale_x = object.scale_y = object.scale_z = 1.0f;
            object.visible = 1;
            object.asset_path = settings->splashes[index].path;
            memset(&frame, 0, sizeof(frame));
            frame.background = 0x000000u;
            frame.project_root = game->model.project_root;
            frame.objects = &object;
            frame.object_count = 1;
            if (!zsharp_game_vulkan_draw(game->renderer, resized, &frame,
                                         error, error_size)) return 0;
            SDL_Delay(10);
        }
    }
    return 1;
}

static int game_owns_property(void *state, const char *path) {
    ZSharpGameState *game = (ZSharpGameState *)state;
    int owns;
    if (zsharp_mouse_edge_index(path) >= 0) return 1;
    if (path && !strncmp(path,"XR.",3)) return 1;
    if (path && !strncmp(path, "Gamepad.", 8)) return 1;
    if (path != NULL &&
        strncmp(path, "ZSharp.Achievement.Award.", 25) == 0) return 1;
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
    if (path && !strncmp(path, "Gamepad.", 8)) {
        char value[256];
        int kind;
        /* As with mouse edges, allocation failure must not consume an edge. */
        *text = (char *)malloc(sizeof(value));
        if (*text == NULL) {
            game_error(error, error_size, "out of memory");
            SDL_UnlockMutex(game->model_mutex);
            return 0;
        }
        ok = zsharp_gamepad_read(&game->gamepad_input, &gamepad_cursor,
                                 path, value, sizeof(value), &kind);
        if (ok) {
            strcpy(*text, value);
            *type = kind == ZSHARP_GAMEPAD_STATUS ? ZWINDOW_READ_STATUS :
                    kind == ZSHARP_GAMEPAD_TEXT ? ZWINDOW_READ_TEXT : ZWINDOW_READ_NUMBER;
        } else {
            free(*text);
            *text = NULL;
            game_error(error, error_size, "unknown Gamepad property");
        }
        SDL_UnlockMutex(game->model_mutex);
        return ok;
    }
    if(path && !strncmp(path,"XR.",3)) {
        char value[128];int status;
        ok=zsharp_xr_read(&game->xr,&xr_cursor,path,value,sizeof(value),&status);
        if(ok){*text=zsharp_copy_text(value,strlen(value));*type=status?ZWINDOW_READ_STATUS:ZWINDOW_READ_NUMBER;ok=*text!=NULL;}
        else game_error(error,error_size,"unknown XR property");
        SDL_UnlockMutex(game->model_mutex);return ok;
    }
    {
        int edge = zsharp_mouse_edge_index(path);
        if (edge >= 0) {
            /* Allocate before consuming so allocation failure loses no event. */
            *text = zsharp_copy_text("alive", 5);
            if (*text == NULL) {
                SDL_UnlockMutex(game->model_mutex);
                game_error(error, error_size, "out of memory");
                return 0;
            }
            if (!zsharp_mouse_edge_read(&mouse_cursor, &game->mouse_edges, edge))
                strcpy(*text, "dead");
            *type = ZWINDOW_READ_STATUS;
            SDL_UnlockMutex(game->model_mutex);
            return 1;
        }
    }
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
    if (path && !strncmp(path, "Gamepad.", 8)) {
        SDL_LockMutex(game->model_mutex);
        ok = zsharp_gamepad_write(&game->gamepad_input, path, value,
                                  error, error_size);
        SDL_UnlockMutex(game->model_mutex);
        return ok;
    }
    if(path && !strncmp(path,"XR.",3)) {
        SDL_LockMutex(game->model_mutex);
        ok=zsharp_xr_write(&game->xr,path,value,error,error_size);
        SDL_UnlockMutex(game->model_mutex);return ok;
    }
    if (path != NULL &&
        strncmp(path, "ZSharp.Achievement.Award.", 25) == 0) {
        ZSharpAchievement achievement;
        int newly_awarded = 0;
        if (game->settings == NULL ||
            !zsharp_achievement_find(game->model.project_root, path + 25,
                                     &achievement, error, error_size)) return 0;
        ok = zsharp_registry_award_achievement(
            game->settings->project_id, achievement.id, &newly_awarded,
            error, error_size);
        if (ok && newly_awarded) {
            snprintf(game->achievement_title, sizeof(game->achievement_title),
                     "ACHIEVEMENT UNLOCKED - %s", achievement.display);
            snprintf(game->achievement_description,
                     sizeof(game->achievement_description), "%s [%s]",
                     achievement.description, achievement.rarity);
            game->achievement_until = SDL_GetTicks() + 5000;
            play_achievement_sound(game);
        }
        zsharp_achievement_free(&achievement);
        return ok;
    }
    SDL_LockMutex(game->model_mutex);
    ok = zsharp_game_model_set_property(&game->model, path, value_type, value,
                                        error, error_size);
    if (ok && path != NULL &&
        strcmp(path, "input.mouse.captured") == 0)
        game->mouse_capture_dirty = 1;
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
            SDL_SetAudioStreamFrequencyRatio(stream, object->audio_pitch);
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
        SDL_SetAudioStreamFrequencyRatio(stream, object->audio_pitch);
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
        if (object->scene == NULL || game->model.active_scene == NULL ||
            strcmp(object->scene, game->model.active_scene) != 0) {
            if (object->audio_playing) {
                SDL_ClearAudioStream(stream);
                SDL_PauseAudioStreamDevice(stream);
                object->audio_playing = 0;
            }
            continue;
        }
        SDL_SetAudioStreamGain(stream, object->audio_volume);
        SDL_SetAudioStreamFrequencyRatio(stream, object->audio_pitch);
        if (object->audio_started || collision_started) {
            SDL_ClearAudioStream(stream);
            SDL_PutAudioStreamData(stream, object->audio_buffer,
                                   (int)object->audio_length);
            SDL_ResumeAudioStreamDevice(stream);
            object->audio_started = 0;
            object->audio_playing = 1;
        } else if (object->audio_loop && object->audio_playing &&
                   SDL_GetAudioStreamQueued(stream) <
                       (int)(object->audio_length / 3u)) {
            SDL_PutAudioStreamData(stream, object->audio_buffer,
                                   (int)object->audio_length);
            SDL_ResumeAudioStreamDevice(stream);
        } else if (!object->audio_loop && object->audio_playing &&
                   SDL_GetAudioStreamQueued(stream) <= 0) {
            object->audio_playing = 0;
        }
        object->was_colliding = object->colliding;
    }
}

/* SDL calls live on the event/render thread. Z# worker threads only read or
   queue output through gamepad_input while holding model_mutex. */
typedef struct GamepadButtonMapping {
    SDL_GamepadButton sdl;
    int zsharp;
} GamepadButtonMapping;

static const GamepadButtonMapping gamepad_buttons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, ZGP_SOUTH},
    {SDL_GAMEPAD_BUTTON_EAST, ZGP_EAST},
    {SDL_GAMEPAD_BUTTON_WEST, ZGP_WEST},
    {SDL_GAMEPAD_BUTTON_NORTH, ZGP_NORTH},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ZGP_DPAD_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ZGP_DPAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ZGP_DPAD_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ZGP_DPAD_RIGHT},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, ZGP_LEFT_STICK},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, ZGP_RIGHT_STICK},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ZGP_LEFT_BUMPER},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ZGP_RIGHT_BUMPER},
    {SDL_GAMEPAD_BUTTON_START, ZGP_START},
    {SDL_GAMEPAD_BUTTON_BACK, ZGP_BACK},
    {SDL_GAMEPAD_BUTTON_GUIDE, ZGP_GUIDE},
    {SDL_GAMEPAD_BUTTON_MISC1, ZGP_MISC1},
    {SDL_GAMEPAD_BUTTON_MISC2, ZGP_MISC2},
    {SDL_GAMEPAD_BUTTON_MISC3, ZGP_MISC3},
    {SDL_GAMEPAD_BUTTON_MISC4, ZGP_MISC4},
    {SDL_GAMEPAD_BUTTON_MISC5, ZGP_MISC5},
    {SDL_GAMEPAD_BUTTON_MISC6, ZGP_MISC6},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, ZGP_LEFT_PADDLE1},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, ZGP_RIGHT_PADDLE1},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, ZGP_LEFT_PADDLE2},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, ZGP_RIGHT_PADDLE2},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, ZGP_TOUCHPAD}
};

static const char *gamepad_type_name(SDL_Gamepad *pad) {
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return "xbox";
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return "playstation";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_GAMEPAD_TYPE_GAMECUBE: return "nintendo";
        default: return "generic";
    }
}

static const char *gamepad_label(SDL_GamepadButtonLabel label) {
    switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: return "";
    }
}

/* Called under model_mutex. Capture held state on connection/focus regain,
   without reporting an input that began in another application as a press. */
static void sample_gamepad(ZSharpGameState *game, int baseline) {
    size_t index;
    uint64_t presses[ZGP_BUTTONS], releases[ZGP_BUTTONS];
    if (!game->gamepad || !game->gamepad_focused) return;
    if (baseline) {
        memcpy(presses, game->gamepad_input.pressed, sizeof(presses));
        memcpy(releases, game->gamepad_input.released, sizeof(releases));
    }
    zsharp_gamepad_stick(&game->gamepad_input, 0,
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_LEFTX),
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    zsharp_gamepad_stick(&game->gamepad_input, 1,
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    for (index = 0; index < SDL_arraysize(gamepad_buttons); ++index)
        zsharp_gamepad_button(&game->gamepad_input, gamepad_buttons[index].zsharp,
            SDL_GetGamepadButton(game->gamepad, gamepad_buttons[index].sdl));
    zsharp_gamepad_trigger(&game->gamepad_input, 0,
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    zsharp_gamepad_trigger(&game->gamepad_input, 1,
        SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (baseline) {
        memcpy(game->gamepad_input.pressed, presses, sizeof(presses));
        memcpy(game->gamepad_input.released, releases, sizeof(releases));
    }
}

static void stop_gamepad_rumble(ZSharpGameState *game) {
    if (game->gamepad) (void)SDL_RumbleGamepad(game->gamepad, 0, 0, 0);
}

static void refresh_gamepad_focus(ZSharpGameState *game) {
    int focused, changed;
    SDL_LockMutex(game->model_mutex);
    focused = game->xr.active ? game->xr.focused : game->window_focused;
    changed = focused != game->gamepad_focused;
    if (changed) {
        game->gamepad_focused = focused;
        if (!focused) zsharp_gamepad_reset(&game->gamepad_input, 0);
        else sample_gamepad(game, 1);
        game->gamepad_vibration_serial = game->gamepad_input.vibration_serial;
    }
    SDL_UnlockMutex(game->model_mutex);
    if (changed && !focused) stop_gamepad_rumble(game);
}

static void open_gamepad(ZSharpGameState *game, SDL_JoystickID id) {
    size_t index;
    const char *name;
    if (game->gamepad || (game->gamepad_test && id != game->test_gamepad_id)) return;
    game->gamepad = SDL_OpenGamepad(id);
    if (!game->gamepad) return;
    name = SDL_GetGamepadName(game->gamepad);
    SDL_LockMutex(game->model_mutex);
    zsharp_gamepad_connect(&game->gamepad_input, name ? name : "Gamepad",
        gamepad_type_name(game->gamepad), SDL_GetBooleanProperty(
            SDL_GetGamepadProperties(game->gamepad),
            SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false));
    for (index = 0; index < 4; ++index)
        snprintf(game->gamepad_input.labels[index],
            sizeof(game->gamepad_input.labels[index]), "%s",
            gamepad_label(SDL_GetGamepadButtonLabel(game->gamepad,
                gamepad_buttons[index].sdl)));
    sample_gamepad(game, 1);
    game->gamepad_vibration_serial = game->gamepad_input.vibration_serial;
    SDL_UnlockMutex(game->model_mutex);
}

static void select_gamepad(ZSharpGameState *game) {
    int count = 0, index;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (index = 0; ids && index < count && !game->gamepad; ++index)
        open_gamepad(game, ids[index]);
    SDL_free(ids);
}

static int gamepad_event(ZSharpGameState *game, const SDL_Event *event) {
    size_t index;
    SDL_JoystickID selected = game->gamepad ? SDL_GetGamepadID(game->gamepad) : 0;
    switch (event->type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!game->gamepad) open_gamepad(game, event->gdevice.which);
            return 1;
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_REMAPPED:
            if (event->gdevice.which == selected) {
                stop_gamepad_rumble(game);
                SDL_CloseGamepad(game->gamepad);
                game->gamepad = NULL;
                SDL_LockMutex(game->model_mutex);
                zsharp_gamepad_reset(&game->gamepad_input, 1);
                game->gamepad_vibration_serial = game->gamepad_input.vibration_serial;
                SDL_UnlockMutex(game->model_mutex);
                select_gamepad(game);
            }
            return 1;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (event->gbutton.which == selected && game->gamepad_focused) {
                for (index = 0; index < SDL_arraysize(gamepad_buttons); ++index) {
                    if (gamepad_buttons[index].sdl != event->gbutton.button) continue;
                    SDL_LockMutex(game->model_mutex);
                    zsharp_gamepad_button(&game->gamepad_input,
                        gamepad_buttons[index].zsharp,
                        event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                    SDL_UnlockMutex(game->model_mutex);
                    break;
                }
            }
            return 1;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (event->gaxis.which == selected && game->gamepad_focused &&
                (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                 event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
                SDL_LockMutex(game->model_mutex);
                zsharp_gamepad_trigger(&game->gamepad_input,
                    event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                    event->gaxis.value);
                SDL_UnlockMutex(game->model_mutex);
            }
            return 1;
        default: return 0;
    }
}

static void refresh_gamepad_output(ZSharpGameState *game) {
    float strength, milliseconds;
    unsigned serial;
    SDL_LockMutex(game->model_mutex);
    serial = game->gamepad_input.vibration_serial;
    strength = game->gamepad_focused ? game->gamepad_input.vibration : 0.0f;
    milliseconds = game->gamepad_input.vibration_ms;
    SDL_UnlockMutex(game->model_mutex);
    if (serial == game->gamepad_vibration_serial) return;
    game->gamepad_vibration_serial = serial;
    if (game->gamepad) {
        Uint16 amount = (Uint16)(strength * 65535.0f + 0.5f);
        /* Unsupported motors are optional capabilities, never launch errors. */
        (void)SDL_RumbleGamepad(game->gamepad, amount, amount,
            (Uint32)ceilf(milliseconds));
    }
}

/* Opt-in integration fixture. It uses SDL's real virtual driver and the same
   event path as hardware; ordinary games never attach a virtual device. */
static bool SDLCALL test_gamepad_rumble(void *data, Uint16 low, Uint16 high) {
    ZSharpGameState *game = (ZSharpGameState *)data;
    (void)high;
    ++game->test_gamepad_rumbles;
    game->test_gamepad_rumble_strength = low;
    return true;
}

static int attach_test_gamepad(ZSharpGameState *game) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045e;
    desc.product_id = 0x028e;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    desc.name = "Z# Virtual Gamepad";
    desc.userdata = game;
    desc.Rumble = test_gamepad_rumble;
    game->test_gamepad_id = SDL_AttachVirtualJoystick(&desc);
    if (!game->test_gamepad_id) return 0;
    game->test_gamepad_joystick = SDL_OpenJoystick(game->test_gamepad_id);
    if (!game->test_gamepad_joystick) return 0;
    SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick,
        SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick,
        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    SDL_UpdateGamepads();
    return 1;
}

static void pump_test_gamepad(ZSharpGameState *game) {
    SDL_Event event;
    SDL_PumpEvents();
    while (SDL_PollEvent(&event)) (void)gamepad_event(game, &event);
    SDL_LockMutex(game->model_mutex);
    sample_gamepad(game, 0);
    refresh_input(game);
    SDL_UnlockMutex(game->model_mutex);
}

static int gamepad_regression(ZSharpGameState *game, char *error, size_t capacity) {
    ZSharpGamepadCursor first, second;
    char value[256];
    int kind, index;
    SDL_Event foreign;
#define PAD_CHECK(condition, message) do { if (!(condition)) { \
    game_error(error, capacity, "SDL gamepad regression: " message); return 0; \
} } while (0)
    game->window_focused = 1;
    refresh_gamepad_focus(game);
    PAD_CHECK(attach_test_gamepad(game), "could not attach virtual driver");
    pump_test_gamepad(game);
    PAD_CHECK(game->gamepad && game->gamepad_input.connected,
        "hotplug did not select virtual controller");
    PAD_CHECK(game->gamepad_input.trigger[0] == 0 &&
        game->gamepad_input.trigger[1] == 0, "idle trigger was not zero");
    zsharp_gamepad_cursor_begin(&first, &game->gamepad_input);
    zsharp_gamepad_cursor_begin(&second, &game->gamepad_input);
    for (index = 0; index < 2; ++index) {
        SDL_SetJoystickVirtualButton(game->test_gamepad_joystick,
            SDL_GAMEPAD_BUTTON_SOUTH, true);
        SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick,
            SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
        SDL_UpdateGamepads();
        SDL_SetJoystickVirtualButton(game->test_gamepad_joystick,
            SDL_GAMEPAD_BUTTON_SOUTH, false);
        SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick,
            SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
        SDL_UpdateGamepads();
    }
    /* All four transitions happened before either script/task reads anything. */
    pump_test_gamepad(game);
    PAD_CHECK(!game->gamepad_input.held[ZGP_SOUTH] &&
        !game->gamepad_input.held[ZGP_RIGHT_TRIGGER], "quick tap left held input");
    for (index = 0; index < 2; ++index) {
        PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
            "Gamepad.south.pressed", value, sizeof(value), &kind) &&
            !strcmp(value, "alive"), "quick button press lost");
        PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
            "Gamepad.south.released", value, sizeof(value), &kind) &&
            !strcmp(value, "alive"), "quick button release lost");
        PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
            "Gamepad.right.trigger.pressed", value, sizeof(value), &kind) &&
            !strcmp(value, "alive"), "quick trigger press lost");
        PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
            "Gamepad.right.trigger.released", value, sizeof(value), &kind) &&
            !strcmp(value, "alive"), "quick trigger release lost");
    }
    PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
        "Gamepad.south.pressed", value, sizeof(value), &kind) &&
        !strcmp(value, "dead"), "button edge repeated");
    PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &second,
        "Gamepad.south.pressed", value, sizeof(value), &kind) &&
        !strcmp(value, "alive"), "one task consumed another task's input");
    SDL_zero(foreign);
    foreign.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    foreign.gbutton.which = game->test_gamepad_id + 100000;
    foreign.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    PAD_CHECK(SDL_PushEvent(&foreign), "could not enqueue foreign input");
    pump_test_gamepad(game);
    PAD_CHECK(!game->gamepad_input.held[ZGP_EAST], "other device stole input");
    SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    SDL_SetJoystickVirtualButton(game->test_gamepad_joystick,
        SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_UpdateGamepads();
    pump_test_gamepad(game);
    PAD_CHECK(game->gamepad_input.stick[0][0] > 0.70f &&
        game->gamepad_input.stick[0][1] > 0.70f &&
        game->gamepad_input.stick[0][0] < 0.71f,
        "radial stick normalization or forward sign incorrect");
    PAD_CHECK(game->model.input.keys[ZGAME_KEY_RARROW] &&
        game->model.input.keys[ZGAME_KEY_UARROW] &&
        game->model.input.keys[ZGAME_KEY_SPACE], "legacy input mapping changed");
    PAD_CHECK(zsharp_gamepad_write(&game->gamepad_input, "Gamepad.vibrate",
        "75|100", error, capacity), "rumble request rejected");
    refresh_gamepad_output(game);
    PAD_CHECK(game->test_gamepad_rumble_strength >= 49150,
        "rumble was not delivered on SDL thread");
    game->window_focused = 0;
    refresh_gamepad_focus(game);
    PAD_CHECK(!game->gamepad_input.held[ZGP_SOUTH] &&
        game->gamepad_input.stick[0][0] == 0 &&
        game->test_gamepad_rumble_strength == 0, "focus loss did not clear input/output");
    zsharp_gamepad_cursor_begin(&first, &game->gamepad_input);
    game->window_focused = 1;
    refresh_gamepad_focus(game);
    PAD_CHECK(game->gamepad_input.held[ZGP_SOUTH], "focus regain did not seed held input");
    PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &first,
        "Gamepad.south.pressed", value, sizeof(value), &kind) &&
        !strcmp(value, "dead"), "focus regain generated phantom press");
    SDL_CloseJoystick(game->test_gamepad_joystick);
    game->test_gamepad_joystick = NULL;
    PAD_CHECK(SDL_DetachVirtualJoystick(game->test_gamepad_id), "detach failed");
    game->test_gamepad_id = 0;
    pump_test_gamepad(game);
    PAD_CHECK(!game->gamepad_input.connected && !game->gamepad,
        "removal did not disconnect controller");
    PAD_CHECK(attach_test_gamepad(game), "could not reconnect controller");
    pump_test_gamepad(game);
    PAD_CHECK(game->gamepad_input.connected &&
        !game->gamepad_input.held[ZGP_SOUTH], "reconnect retained old held input");
    PAD_CHECK(zsharp_gamepad_read(&game->gamepad_input, &second,
        "Gamepad.south.pressed", value, sizeof(value), &kind) &&
        !strcmp(value, "dead"), "reconnect replayed old input");
    SDL_SetJoystickVirtualAxis(game->test_gamepad_joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_UpdateGamepads();
    pump_test_gamepad(game);
    puts("PASS: SDL gamepad hotplug, queued quick taps, independent cursors, sticks, focus, rumble and reconnect");
#undef PAD_CHECK
    return 1;
}

static void refresh_input(ZSharpGameState *game) {
    const bool *keyboard = SDL_GetKeyboardState(NULL);
    static const SDL_Scancode number_keys[10] = {
        SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
        SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
        SDL_SCANCODE_8, SDL_SCANCODE_9
    };
    int index;
    Sint16 horizontal = 0;
    Sint16 vertical = 0;
    int pad_left = 0;
    int pad_right = 0;
    int pad_up = 0;
    int pad_down = 0;
    if (game->gamepad && game->gamepad_input.connected && game->gamepad_focused) {
        /* Preserve the historical fallback threshold independently of the
           richer API's radial deadzone and normalized analogue values. */
        horizontal = SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        vertical = SDL_GetGamepadAxis(game->gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        pad_left = horizontal < -8000 || game->gamepad_input.held[ZGP_DPAD_LEFT];
        pad_right = horizontal > 8000 || game->gamepad_input.held[ZGP_DPAD_RIGHT];
        pad_up = vertical < -8000 || game->gamepad_input.held[ZGP_DPAD_UP];
        pad_down = vertical > 8000 || game->gamepad_input.held[ZGP_DPAD_DOWN];
    }
    memset(game->model.input.keys, 0, sizeof(game->model.input.keys));
    for (index = 0; index < 26; index++)
        game->model.input.keys[ZGAME_KEY_A + index] =
            keyboard[SDL_SCANCODE_A + index];
    for (index = 0; index < 10; index++)
        game->model.input.keys[ZGAME_KEY_0 + index] = keyboard[number_keys[index]];
    game->model.input.keys[ZGAME_KEY_LARROW] =
        keyboard[SDL_SCANCODE_LEFT] || pad_left;
    game->model.input.keys[ZGAME_KEY_RARROW] =
        keyboard[SDL_SCANCODE_RIGHT] || pad_right;
    game->model.input.keys[ZGAME_KEY_UARROW] =
        keyboard[SDL_SCANCODE_UP] || pad_up;
    game->model.input.keys[ZGAME_KEY_DARROW] =
        keyboard[SDL_SCANCODE_DOWN] || pad_down;
    for (index = 0; index < 12; index++)
        game->model.input.keys[ZGAME_KEY_FN1 + index] =
            keyboard[SDL_SCANCODE_F1 + index];
    for (index = 0; index < 12; index++)
        game->model.input.keys[ZGAME_KEY_FN13 + index] =
            keyboard[SDL_SCANCODE_F13 + index];
    game->model.input.keys[ZGAME_KEY_SPACE] = keyboard[SDL_SCANCODE_SPACE] ||
        game->gamepad_input.held[ZGP_SOUTH];
    game->model.input.keys[ZGAME_KEY_ENTER] = keyboard[SDL_SCANCODE_RETURN];
    game->model.input.keys[ZGAME_KEY_ESCAPE] = keyboard[SDL_SCANCODE_ESCAPE];
    game->model.input.keys[ZGAME_KEY_TAB] = keyboard[SDL_SCANCODE_TAB];
    game->model.input.keys[ZGAME_KEY_BACKSPACE] = keyboard[SDL_SCANCODE_BACKSPACE];
    game->model.input.keys[ZGAME_KEY_LSHIFT] = keyboard[SDL_SCANCODE_LSHIFT];
    game->model.input.keys[ZGAME_KEY_RSHIFT] = keyboard[SDL_SCANCODE_RSHIFT];
    game->model.input.keys[ZGAME_KEY_LCTRL] = keyboard[SDL_SCANCODE_LCTRL];
    game->model.input.keys[ZGAME_KEY_RCTRL] = keyboard[SDL_SCANCODE_RCTRL];
    game->model.input.keys[ZGAME_KEY_LALT] = keyboard[SDL_SCANCODE_LALT];
    game->model.input.keys[ZGAME_KEY_RALT] = keyboard[SDL_SCANCODE_RALT];
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

int zsharp_game_run(const char *title, const char *project_root,
                    ZSharpWindowCallback callback, void *user_data,
                    char *error, size_t error_size) {
    ZSharpGameState game;
    ZSharpWindowRuntime runtime;
    SDL_Event event;
    Uint64 started;
    Uint64 previous;
    Uint64 profile_started = 0, profile_physics = 0, profile_snapshot = 0;
    int profile = getenv("ZSHARP_GAME_PROFILE") != NULL;
    unsigned long long close_after = auto_close_after();
    int tasks_started = 0;
    int resized = 0;
    int ok = 0;
    double accumulator = 0.0;
    char window_title[512];
    ZSharpSettings settings;
    ZSharpDiagnostic settings_diagnostic;
    memset(&game, 0, sizeof(game));
#ifdef ZSHARP_HAS_OPENXR
    game.xr_ui_trace=getenv("ZSHARP_XR_UI_TRACE")!=NULL;
    game.xr_ui_test=getenv("ZSHARP_XR_UI_TEST")!=NULL;
#endif
    memset(&runtime, 0, sizeof(runtime));
    zsharp_gamepad_init(&game.gamepad_input);
    game.gamepad_test = getenv("ZSHARP_GAMEPAD_TEST") != NULL;
    zsharp_settings_init(&settings);
    if (getenv("ZSHARP_GAME_FORCE_FAILURE") != NULL) {
        game_error(error, error_size, "forced game launch failure");
        return 0;
    }
    /* Keep the physical snapshot fresh even while another application owns
       focus. Delivery to Z# is explicitly gated and baselined below. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        game_error(error, error_size, SDL_GetError());
        return 0;
    }
    if (!zsharp_settings_load(project_root, &settings, &settings_diagnostic,
                              error, error_size)) goto done;
    {
#ifdef _WIN32
        const unsigned device=1;
#elif defined(__APPLE__)
        const unsigned device=4;
#else
        const unsigned device=2;
#endif
        if(!(settings.allowed_devices&device)){game_error(error,error_size,"This project does not allow this operating system");goto done;}
        if(settings.xr_allowed){
            int mode=settings.xr_required?1:0;
            const char *override_mode=getenv("ZSHARP_XR_MODE");
            if(override_mode && !strcmp(override_mode,"vr"))mode=1;
            else if(override_mode && !strcmp(override_mode,"desktop")){
                if(settings.xr_required){game_error(error,error_size,"This game requires VR mode");goto done;}
                mode=2;
            }
            if(!mode){
                const SDL_MessageBoxButtonData buttons[]={{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Cancel"},{0,1,"Play in VR"},{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,2,"Play in Desktop Mode"}};
                SDL_MessageBoxData box={SDL_MESSAGEBOX_INFORMATION,NULL,"Z# - Launch mode","How would you like to play?",3,buttons,NULL};
                if(!SDL_ShowMessageBox(&box,&mode)){game_error(error,error_size,SDL_GetError());goto done;}
                if(mode<=0){ok=1;goto done;}
            }
            if(mode==1){
#ifdef ZSHARP_HAS_OPENXR
                game.xr_backend=zsharp_game_xr_create(error,error_size);
                if(game.xr_backend)game.xr.available=game.xr.active=1;
                else
#endif
                {
                    int choice=0;char message[1024];
#ifndef ZSHARP_HAS_OPENXR
                    game_error(error,error_size,"PCVR is not available in this platform/build yet");
#endif
                    snprintf(message,sizeof(message),"A usable headset connection was not found.\n\n%s",error);
                    const SDL_MessageBoxButtonData buttons[]={{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT|SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,0,"Close"},{0,2,"Play in Desktop Mode"}};
                    SDL_MessageBoxData box={SDL_MESSAGEBOX_ERROR,NULL,"Z# - Headset unavailable",message,settings.xr_required?1:2,buttons,NULL};
                    if(!SDL_ShowMessageBox(&box,&choice)){game_error(error,error_size,SDL_GetError());goto done;}
                    if(choice!=2){ok=1;goto done;}error[0]=0;
                }
            }
        }
    }
    if (!zsharp_game_model_load(project_root, &game.model, error,error_size)) goto done;
    if (getenv("ZSHARP_GAME_MODEL_UI_TEST") != NULL &&
        !zsharp_game_model_ui_regression(error, error_size)) goto done;
    game.settings = &settings;
    game.model_mutex = SDL_CreateMutex();
    if (game.model_mutex == NULL) {
        game_error(error, error_size, SDL_GetError());
        goto done;
    }
    {
        const char *scene_title = zsharp_game_model_scene_title(&game.model);
        snprintf(window_title, sizeof(window_title), "%s",
                 scene_title != NULL && scene_title[0] != '\0'
                     ? scene_title
                     : (title == NULL || title[0] == '\0' ? "Z# Game" : title));
    }
    game.window = SDL_CreateWindow(window_title, 1280, 720,
                                   SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (game.window == NULL) {
        game_error(error, error_size, SDL_GetError());
        goto done;
    }
    {
        const char *scene_icon = zsharp_game_model_scene_icon(&game.model);
        const char *icon = scene_icon != NULL ? scene_icon : settings.icon;
        if (!set_game_icon(&game, icon, error, error_size)) goto done;
    }
    game.renderer = zsharp_game_vulkan_create(game.window, error, error_size);
    if (game.renderer == NULL ||
        !show_splashes(&game, &settings, error, error_size)) goto done;
    if (game.cancelled) {
        ok = 1;
        goto done;
    }
    if (!load_audio(&game, error, error_size)) goto done;
    game.window_focused = (SDL_GetWindowFlags(game.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    refresh_gamepad_focus(&game);
    select_gamepad(&game);
    if (game.gamepad_test && !gamepad_regression(&game, error, error_size)) goto done;
    runtime.state = &game;
    runtime.get_property = game_get_property;
    runtime.owns_property = game_owns_property;
    runtime.set_property = game_set_property;
    runtime.wait = game_wait;
    runtime.is_cancelled = game_cancelled;
    runtime.request_close = game_request_close;
    runtime.begin_task = game_begin_task;
    if (callback != NULL && !callback(user_data, ZSHARP_WINDOW_PROJECT_STARTS,
                                      &runtime, error, error_size)) goto done;
    tasks_started = callback != NULL;
    started = SDL_GetTicks();
    previous = SDL_GetTicksNS();
    while (!game.cancelled) {
        Uint64 now;
        double delta;
        ZSharpGameRenderFrame frame;
        ZSharpGameRenderObject *objects = NULL;
#ifdef ZSHARP_HAS_OPENXR
        ZSharpXRState xr_snapshot;
        uint64_t xr_scene_generation = 0;
        if(game.xr_backend){
            int result;
            SDL_LockMutex(game.model_mutex);xr_snapshot=game.xr;SDL_UnlockMutex(game.model_mutex);
            result=zsharp_game_xr_begin(game.xr_backend,&xr_snapshot,error,error_size);
            if(!result)goto done;
            if(result==2){game.cancelled=1;break;}
            SDL_LockMutex(game.model_mutex);
            game.xr.available=xr_snapshot.available;game.xr.active=xr_snapshot.active;game.xr.focused=xr_snapshot.focused;
            game.xr.hand_tracking_available=xr_snapshot.hand_tracking_available;
            game.xr.head=xr_snapshot.head;memcpy(game.xr.hands,xr_snapshot.hands,sizeof(game.xr.hands));
            game.model.xr_ui_active = xr_snapshot.active;
            if (!xr_snapshot.active || !xr_snapshot.focused) {
                game.model.xr_hovered_buttons[0] = game.model.xr_hovered_buttons[1] = 0;
                game.model.focused_button = 0;
            }
            SDL_UnlockMutex(game.model_mutex);
        }
#endif
        refresh_gamepad_focus(&game);
        SDL_LockMutex(game.model_mutex);
        game.model.input.mouse_delta_x = 0.0f;
        game.model.input.mouse_delta_y = 0.0f;
        SDL_UnlockMutex(game.model_mutex);
        while (SDL_PollEvent(&event)) {
            if (gamepad_event(&game, &event)) continue;
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                game.cancelled = 1;
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                     event.type == SDL_EVENT_WINDOW_RESIZED)
                resized = 1;
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                game.window_focused = 0;
                refresh_gamepad_focus(&game);
                SDL_LockMutex(game.model_mutex);
                zsharp_mouse_transition(&game.mouse_edges, 0, 0);
                zsharp_mouse_transition(&game.mouse_edges, 1, 0);
                game.model.input.mouse_left = 0;
                game.model.input.mouse_right = 0;
                game.model.focused_button = 0;
                SDL_UnlockMutex(game.model_mutex);
            }
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                game.window_focused = 1;
                refresh_gamepad_focus(&game);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                char *target = NULL;
                SDL_LockMutex(game.model_mutex);
                if (!game.model.xr_ui_active && !game.model.input.mouse_captured) {
                    if (event.key.key == SDLK_TAB)
                        zsharp_game_model_focus_next(&game.model, (event.key.mod & SDL_KMOD_SHIFT) != 0);
                    else if ((event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE) &&
                        game.model.focused_button > 0 && game.model.focused_button <= game.model.object_count) {
                        const ZSharpGameObject *button = &game.model.objects[game.model.focused_button-1];
                        if (button->visible && button->opacity > 0 && button->scene &&
                            strcmp(button->scene,game.model.active_scene) == 0 && button->click_left)
                            target = zsharp_copy_text(button->click_left,strlen(button->click_left));
                    }
                }
                SDL_UnlockMutex(game.model_mutex);
                if (target != NULL) {
                    int called = callback == NULL || callback(user_data,target,&runtime,error,error_size);
                    free(target);
                    if (!called) goto done;
                }
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION) {
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
                game.model.input.mouse_delta_x += event.motion.xrel;
                game.model.input.mouse_delta_y += event.motion.yrel;
                SDL_UnlockMutex(game.model_mutex);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                int pressed = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                char *click_target = NULL;
                int click_failed = 0;
                SDL_LockMutex(game.model_mutex);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    zsharp_mouse_transition(&game.mouse_edges, 0, pressed);
                    game.model.input.mouse_left = pressed;
                }
                else if (event.button.button == SDL_BUTTON_RIGHT) {
                    zsharp_mouse_transition(&game.mouse_edges, 1, pressed);
                    game.model.input.mouse_right = pressed;
                }
                if (pressed && !game.model.xr_ui_active && !game.model.input.mouse_captured &&
                    (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
                    int width = 1280, height = 720;
                    const ZSharpGameObject *button;
                    const char *target;
                    SDL_GetWindowSize(game.window, &width, &height);
                    button = zsharp_game_model_button_at(&game.model,
                        width > 0 ? event.button.x * 1280.0f / width - 640.0f : 0,
                        height > 0 ? 360.0f - event.button.y * 720.0f / height : 0);
                    target = button == NULL ? NULL :
                        event.button.button == SDL_BUTTON_LEFT ? button->click_left : button->click_right;
                    game.model.focused_button = button == NULL ? 0 : (size_t)(button-game.model.objects)+1;
                    if (target != NULL) {
                        click_target = zsharp_copy_text(target, strlen(target));
                        if (click_target == NULL) {
                            game_error(error, error_size, "out of memory scheduling game button");
                            click_failed = 1;
                        }
                    }
                }
                SDL_UnlockMutex(game.model_mutex);
                if (click_target != NULL) {
                    if (callback != NULL && !callback(user_data, click_target, &runtime, error, error_size)) {
                        click_failed = 1;
                    }
                    free(click_target);
                }
                if (click_failed) goto done;
            }
        }
        refresh_gamepad_output(&game);
        SDL_LockMutex(game.model_mutex);
        if (game.mouse_capture_dirty) {
            int captured = game.model.input.mouse_captured;
            game.mouse_capture_dirty = 0;
            SDL_UnlockMutex(game.model_mutex);
            if (!SDL_SetWindowRelativeMouseMode(game.window, captured)) {
                fprintf(stderr, "game input warning: could not %s mouse capture: %s\n",
                        captured ? "enable" : "release", SDL_GetError());
                SDL_LockMutex(game.model_mutex);
                game.model.input.mouse_captured = !captured;
                SDL_UnlockMutex(game.model_mutex);
            }
        } else {
            SDL_UnlockMutex(game.model_mutex);
        }
        now = SDL_GetTicksNS();
        profile_started = now;
        delta = (double)(now - previous) / 1000000000.0;
        previous = now;
        if (close_after != 0 && now / 1000000 - started >= close_after)
            game.cancelled = 1;
        if (game.cancelled) break;
        SDL_LockMutex(game.model_mutex);
        game.model.render_delta = delta;
        sample_gamepad(&game, 0);
        refresh_input(&game);
        accumulator += fmin(delta, 0.25);
        while (accumulator >= 1.0 / 120.0) {
            zsharp_game_model_update(&game.model, 1.0 / 120.0);
            accumulator -= 1.0 / 120.0;
        }
        update_audio(&game);
        SDL_UnlockMutex(game.model_mutex);
        profile_physics = SDL_GetTicksNS();
        SDL_LockMutex(game.model_mutex);
        {
            const char *scene_title = zsharp_game_model_scene_title(&game.model);
            if (scene_title != NULL && scene_title[0] != '\0' &&
                strcmp(SDL_GetWindowTitle(game.window), scene_title) != 0)
                SDL_SetWindowTitle(game.window, scene_title);
            {
                const char *scene_icon =
                    zsharp_game_model_scene_icon(&game.model);
                const char *icon = scene_icon != NULL
                    ? scene_icon : settings.icon;
                if (!set_game_icon(&game, icon, error, error_size)) {
                    SDL_UnlockMutex(game.model_mutex);
                    goto done;
                }
            }
        }
        zsharp_game_model_frame(&game.model, &frame, &objects);
#ifdef ZSHARP_HAS_OPENXR
        xr_snapshot=game.xr;
        xr_scene_generation = game.model.scene_generation;
#endif
        SDL_UnlockMutex(game.model_mutex);
        if (frame.object_count != 0 && objects == NULL) {
            game_error(error, error_size, "out of memory");
            goto done;
        }
        if (game.achievement_until > SDL_GetTicks()) {
            ZSharpGameRenderObject *expanded = (ZSharpGameRenderObject *)realloc(
                objects, (frame.object_count + 3) * sizeof(*expanded));
            if (expanded == NULL) {
                free(objects);
                game_error(error, error_size, "out of memory");
                goto done;
            }
            objects = expanded;
            memset(&objects[frame.object_count], 0, 3 * sizeof(*objects));
            objects[frame.object_count].shape = ZGAME_SHAPE_RECTANGLE;
            objects[frame.object_count].x = 0;
            objects[frame.object_count].y = -300;
            objects[frame.object_count].width = 720;
            objects[frame.object_count].height = 92;
            objects[frame.object_count].scale_x = objects[frame.object_count].scale_y = 1;
            objects[frame.object_count].visible = 1;
            objects[frame.object_count].layer = 100000;
            objects[frame.object_count].color = 0x17171Fu;
            objects[frame.object_count + 1] = objects[frame.object_count];
            objects[frame.object_count + 1].shape = ZGAME_SHAPE_TEXT;
            objects[frame.object_count + 1].x = -330;
            objects[frame.object_count + 1].y = -278;
            objects[frame.object_count + 1].scale_x = 1.5f;
            objects[frame.object_count + 1].color = 0xFFFFFFu;
            objects[frame.object_count + 1].text = game.achievement_title;
            objects[frame.object_count + 2] = objects[frame.object_count + 1];
            objects[frame.object_count + 2].y = -312;
            objects[frame.object_count + 2].scale_x = 1.0f;
            objects[frame.object_count + 2].color = 0xA8A8B8u;
            objects[frame.object_count + 2].text = game.achievement_description;
            frame.objects = objects;
            frame.object_count += 3;
        }
        profile_snapshot = SDL_GetTicksNS();
#ifdef ZSHARP_HAS_OPENXR
        if(game.xr_backend && !zsharp_game_xr_draw(game.xr_backend,game.renderer,&xr_snapshot,&frame,error,error_size)){free(objects);goto done;}
        if(game.xr_backend) {
            size_t hovered[2]={zsharp_game_xr_hovered_button(game.xr_backend,0),
                zsharp_game_xr_hovered_button(game.xr_backend,1)};
            if(!dispatch_xr_buttons(&game,&frame,&xr_snapshot,hovered,
                xr_scene_generation,callback,user_data,&runtime,error,error_size)) {
                free(objects);goto done;
            }
        } else if(game.xr_ui_test && !test_xr_buttons(&game,&frame,xr_scene_generation,
            callback,user_data,&runtime,error,error_size)) {
            free(objects);goto done;
        }
#endif
        if (!zsharp_game_vulkan_draw(game.renderer, resized, &frame,
                                     error, error_size)) {
            free(objects);
            goto done;
        }
        free(objects);
        if (profile) fprintf(stderr,
            "[Z# frame] objects=%zu interval=%.3fms physics/input=%.3fms snapshot=%.3fms render/present=%.3fms\n",
            frame.object_count, delta * 1000.0,
            (profile_physics-profile_started)/1000000.0,
            (profile_snapshot-profile_physics)/1000000.0,
            (SDL_GetTicksNS()-profile_snapshot)/1000000.0);
        resized = 0;
        SDL_Delay(1);
    }
    ok = 1;
done:
    zsharp_settings_free(&settings);
    game.cancelled = 1;
    stop_gamepad_rumble(&game);
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
    if (game.achievement_audio != NULL)
        SDL_DestroyAudioStream(game.achievement_audio);
    if (game.achievement_samples != NULL)
        SDL_free(game.achievement_samples);
    free(game.window_icon_path);
    if (game.gamepad != NULL) SDL_CloseGamepad(game.gamepad);
    if (game.test_gamepad_joystick) SDL_CloseJoystick(game.test_gamepad_joystick);
    if (game.test_gamepad_id) SDL_DetachVirtualJoystick(game.test_gamepad_id);
#ifdef ZSHARP_HAS_OPENXR
    zsharp_game_xr_destroy(game.xr_backend);
#endif
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

int zsharp_game_run(const char *title, const char *project_root,
                    ZSharpWindowCallback callback, void *user_data,
                    char *error, size_t error_size) {
    (void)title;
    (void)project_root;
    (void)callback;
    (void)user_data;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size,
                 "this ZVM was built without the zsharpgame runtime");
    return 0;
}

#endif
