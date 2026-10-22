#ifndef ZSHARP_VM_ROOM_STATE_H
#define ZSHARP_VM_ROOM_STATE_H

/* Private, header-local scalar room-state machinery. Included after the VM's
 * RuntimeHeap/RuntimeModuleCache, find_variable and runtime_wait definitions.
 * Kept separate so fixture tests can exercise lock/alias/error contracts without
 * linking (or invoking) microphone, speech, window or other native integrations. */
#include "system_ops.h"

typedef struct WindowSharedVariable {
    char *name;
    ZSharpValueType type;
    char *text;
    int32_t number;
} WindowSharedVariable;

typedef struct WindowRoomState {
    char *source_path;
    char *room_name;
    WindowSharedVariable *variables;
    size_t variable_count;
    int initialized;
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
} WindowRoomState;

/* A task owns its parsed program, stack, locals and heap. Only the persistent
   scalar room state is shared with other callback tasks. Publish/reload that
   state at wait/blocking-operation boundaries instead of holding its mutex while
   asleep, recording, transcribing or building a voice. */
typedef struct WindowTaskYield {
    WindowRoomState *state;
    ZSharpRoom *room;
    int *state_locked;
} WindowTaskYield;

static void lock_window_room_state(WindowRoomState *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->mutex);
#else
    pthread_mutex_lock(&state->mutex);
#endif
}

static void unlock_window_room_state(WindowRoomState *state) {
#ifdef _WIN32
    LeaveCriticalSection(&state->mutex);
#else
    pthread_mutex_unlock(&state->mutex);
#endif
}

static int system_operation_blocks(const ZSharpSystemOperation *operation) {
    return !strcmp(operation->name, "Microphone.record") ||
        !strcmp(operation->name, "Microphone.getInput") ||
        !strcmp(operation->name, "Speech.createVoice") ||
        !strcmp(operation->name, "Speech.prepare") ||
        !strcmp(operation->name, "Desktop.typeText") ||
        !strcmp(operation->name, "Desktop.focusWindow") ||
        !strcmp(operation->name, "Desktop.sendKeys");
}

static int restore_window_room_state(WindowRoomState *state,
                                     ZSharpRoom *room, RuntimeHeap *suspended_values,
                                     char *error,
                                     size_t error_size) {
    size_t index;
    if (!state->initialized) return 1;
    for (index = 0; index < state->variable_count; index++) {
        WindowSharedVariable *saved = &state->variables[index];
        ZSharpVariable *variable = find_variable(room, saved->name);
        char *copy, **slot;
        const char *text;
        if (variable == NULL || variable->type != saved->type) continue;
        if (saved->type == ZVALUE_STATUS) {
            variable->number_value = saved->number;
        } else if (saved->type == ZVALUE_NUMBER || saved->type == ZVALUE_TEXT ||
                   saved->type == ZVALUE_FUNCTION) {
            slot = saved->type == ZVALUE_NUMBER ? &variable->number_text :
                                                  &variable->text_value;
            text = saved->text == NULL ? "" : saved->text;
            if (*slot != NULL && strcmp(*slot, text) == 0) continue;
            copy = zsharp_copy_text(text, strlen(text));
            if (copy == NULL) goto out_of_memory;
            if (suspended_values != NULL && *slot != NULL) {
                /* Parameters, locals or an outer expression may still refer to
                   the old scalar buffer across a nested wait. Transfer that
                   buffer to this task's heap instead of freeing live aliases.
                   Allocate first so an OOM leaves the old variable untouched. */
                char **resized = (char **)realloc(suspended_values->texts,
                    (suspended_values->text_count + 1) * sizeof(*resized));
                if (resized == NULL) { free(copy); goto out_of_memory; }
                suspended_values->texts = resized;
                suspended_values->texts[suspended_values->text_count++] = *slot;
            } else free(*slot);
            *slot = copy;
        }
    }
    return 1;

out_of_memory:
    snprintf(error, error_size, "out of memory");
    return 0;
}

static int save_window_room_state(WindowRoomState *state, ZSharpRoom *room,
                                  char *error, size_t error_size) {
    WindowSharedVariable *variables = NULL;
    size_t count = 0;
    size_t index;
    for (index = 0; index < room->variable_count; index++) {
        ZSharpVariable *variable = &room->variables[index];
        WindowSharedVariable *resized;
        WindowSharedVariable *saved;
        const char *text = NULL;
        if (variable->type != ZVALUE_NUMBER && variable->type != ZVALUE_TEXT &&
            variable->type != ZVALUE_FUNCTION && variable->type != ZVALUE_STATUS) continue;
        resized = (WindowSharedVariable *)realloc(
            variables, (count + 1) * sizeof(*variables));
        if (resized == NULL) goto out_of_memory;
        variables = resized;
        saved = &variables[count++];
        memset(saved, 0, sizeof(*saved));
        saved->name = zsharp_copy_text(variable->name, strlen(variable->name));
        saved->type = variable->type;
        saved->number = variable->number_value;
        if (variable->type == ZVALUE_NUMBER) text = variable->number_text;
        else if (variable->type == ZVALUE_TEXT || variable->type == ZVALUE_FUNCTION) text = variable->text_value;
        if (saved->name == NULL) goto out_of_memory;
        if (text != NULL) {
            saved->text = zsharp_copy_text(text, strlen(text));
            if (saved->text == NULL) goto out_of_memory;
        }
    }
    for (index = 0; index < state->variable_count; index++) {
        free(state->variables[index].name);
        free(state->variables[index].text);
    }
    free(state->variables);
    state->variables = variables;
    state->variable_count = count;
    state->initialized = 1;
    return 1;

out_of_memory:
    for (index = 0; index < count; index++) {
        free(variables[index].name);
        free(variables[index].text);
    }
    free(variables);
    snprintf(error, error_size, "out of memory");
    return 0;
}

static int cooperative_window_suspend(RuntimeModuleCache *cache, int *released,
    char *error, size_t error_size) {
    WindowTaskYield *yield = cache->window_yield;
    *released = 0;
    if (yield != NULL && *yield->state_locked) {
        if (!save_window_room_state(yield->state, yield->room, error, error_size))
            return 0;
        unlock_window_room_state(yield->state);
        *yield->state_locked = 0;
        *released = 1;
    }
    return 1;
}

static int cooperative_window_resume(RuntimeModuleCache *cache, RuntimeHeap *heap,
    int released, char *error, size_t error_size) {
    WindowTaskYield *yield = cache->window_yield;
    if (released) {
        /* Always reacquire, even on cancellation or wait failure: unwinding and
           the final state save must have exactly the same ownership contract
           as normal execution. Restoring keeps old scalar buffers in the task
           heap so an argument, outer expression or local alias stays valid.
           Never restore another task's locals or stack. */
        lock_window_room_state(yield->state);
        *yield->state_locked = 1;
        if (!restore_window_room_state(yield->state, yield->room, heap,
                                        error, error_size)) return 0;
    }
    return 1;
}

static int cooperative_window_wait(RuntimeModuleCache *cache, RuntimeHeap *heap,
    const char *milliseconds, char *error, size_t error_size) {
    int released, ok;
    if (!cooperative_window_suspend(cache, &released, error, error_size)) return 0;
    if (cache->window_runtime != NULL && cache->window_runtime->wait != NULL)
        ok = cache->window_runtime->wait(cache->window_runtime->state,
            milliseconds, error, error_size);
    else ok = runtime_wait(milliseconds, error, error_size);
    if (!cooperative_window_resume(cache, heap, released, error, error_size)) return 0;
    return ok;
}

#endif
