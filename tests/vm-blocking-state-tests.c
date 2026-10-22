/* Private VM boundary regression, with minimal heap/cache adapters. The same
 * header-local implementation is used by the VM, without linking any microphone,
 * speech, window or other integration. No audio, app or UI work is performed. */
#ifndef _WIN32
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "bytecode.h"
#include "window.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct RuntimeHeap {
    char **texts;
    size_t text_count;
} RuntimeHeap;

typedef struct RuntimeModuleCache {
    const ZSharpWindowRuntime *window_runtime;
    struct WindowTaskYield *window_yield;
} RuntimeModuleCache;

static ZSharpVariable *find_variable(ZSharpRoom *room, const char *name) {
    size_t index;
    for (index = 0; index < room->variable_count; index++)
        if (!strcmp(room->variables[index].name, name)) return &room->variables[index];
    return NULL;
}

static int runtime_wait(const char *milliseconds, char *error, size_t error_size) {
    (void)milliseconds; (void)error; (void)error_size;
    assert(!"fixture wait callback must be used instead of sleeping");
    return 0;
}

#include "vm_room_state.h"

static int fail_copy;

/* Only the retained scalar-state helpers need this core allocator. */
char *zsharp_copy_text(const char *text, size_t length) {
    char *copy;
    if (fail_copy) {fail_copy = 0; return NULL;}
    copy = (char *)malloc(length + 1);
    if (copy != NULL) {memcpy(copy, text, length); copy[length] = '\0';}
    return copy;
}

typedef struct Fixture {
    WindowRoomState state;
    ZSharpRoom room;
    ZSharpVariable variables[4];
    WindowTaskYield yield;
    RuntimeModuleCache cache;
    RuntimeHeap heap;
    int locked;
    int acquired;
} Fixture;

static void fixture_init(Fixture *fixture) {
    size_t index;
    static char *names[] = {"Text", "Number", "Enabled", "Callback"};
    memset(fixture, 0, sizeof(*fixture));
#ifdef _WIN32
    InitializeCriticalSection(&fixture->state.mutex);
#else
    assert(pthread_mutex_init(&fixture->state.mutex, NULL) == 0);
#endif
    fixture->room.variables = fixture->variables;
    fixture->room.variable_count = 4;
    for (index = 0; index < 4; index++) fixture->variables[index].name = names[index];
    fixture->variables[0].type = ZVALUE_TEXT;
    fixture->variables[0].text_value = zsharp_copy_text("before", 6);
    fixture->variables[1].type = ZVALUE_NUMBER;
    fixture->variables[1].number_text = zsharp_copy_text("1", 1);
    fixture->variables[2].type = ZVALUE_STATUS;
    fixture->variables[2].number_value = 1;
    fixture->variables[3].type = ZVALUE_FUNCTION;
    fixture->variables[3].text_value = zsharp_copy_text("Main:Main:Before", 16);
    fixture->yield.state = &fixture->state;
    fixture->yield.room = &fixture->room;
    fixture->yield.state_locked = &fixture->locked;
    fixture->cache.window_yield = &fixture->yield;
    lock_window_room_state(&fixture->state);
    fixture->locked = 1;
}

static void fixture_free(Fixture *fixture) {
    size_t index;
    if (fixture->locked) unlock_window_room_state(&fixture->state);
    for (index = 0; index < 4; index++) {
        free(fixture->variables[index].text_value);
        free(fixture->variables[index].number_text);
    }
    for (index = 0; index < fixture->state.variable_count; index++) {
        free(fixture->state.variables[index].name);
        free(fixture->state.variables[index].text);
    }
    free(fixture->state.variables);
    for (index = 0; index < fixture->heap.text_count; index++) free(fixture->heap.texts[index]);
    free(fixture->heap.texts);
#ifdef _WIN32
    DeleteCriticalSection(&fixture->state.mutex);
#else
    assert(pthread_mutex_destroy(&fixture->state.mutex) == 0);
#endif
}

#ifdef _WIN32
static DWORD WINAPI sibling(void *context) {
#else
static void *sibling(void *context) {
#endif
    Fixture *fixture = (Fixture *)context;
    size_t index;
#ifdef _WIN32
    fixture->acquired = TryEnterCriticalSection(&fixture->state.mutex) != 0;
#else
    fixture->acquired = pthread_mutex_trylock(&fixture->state.mutex) == 0;
#endif
    if (fixture->acquired) {
        for (index = 0; index < fixture->state.variable_count; index++) {
            WindowSharedVariable *saved = &fixture->state.variables[index];
            free(saved->text); saved->text = NULL;
            if (saved->type == ZVALUE_STATUS) saved->number = 0;
            else if (saved->type == ZVALUE_TEXT) saved->text = zsharp_copy_text("after", 5);
            else if (saved->type == ZVALUE_NUMBER) saved->text = zsharp_copy_text("2", 1);
            else saved->text = zsharp_copy_text("Main:Main:After", 15);
        }
        unlock_window_room_state(&fixture->state);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void run_sibling(Fixture *fixture) {
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, sibling, fixture, 0, NULL);
    assert(thread != NULL);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, sibling, fixture) == 0);
    assert(pthread_join(thread, NULL) == 0);
#endif
    assert(fixture->acquired);
}

static int failing_wait(void *context, const char *milliseconds,
                        char *error, size_t error_size) {
    Fixture *fixture = (Fixture *)context;
    assert(!fixture->locked && !strcmp(milliseconds, "100"));
    run_sibling(fixture);
    snprintf(error, error_size, "fixture cancellation");
    return 0;
}

int main(void) {
    Fixture fixture;
    RuntimeModuleCache no_window = {0};
    ZSharpLiteral local = {0};
    ZSharpWindowRuntime runtime = {0};
    const char *text_alias, *number_alias, *function_alias;
    int released, nested;
    char error[512] = {0};
    const ZSharpSystemOperation *operation;
    operation = zsharp_system_operation("Microphone.record"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Microphone.getInput"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Speech.createVoice"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Speech.prepare"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Desktop.typeText"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Desktop.sendKeys"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Desktop.focusWindow"); assert(operation && system_operation_blocks(operation));
    operation = zsharp_system_operation("Desktop.openApp"); assert(operation && !system_operation_blocks(operation));
    operation = zsharp_system_operation("Microphone.level"); assert(operation && !system_operation_blocks(operation));
    operation = zsharp_system_operation("Speech.speak"); assert(operation && !system_operation_blocks(operation));

    assert(cooperative_window_suspend(&no_window, &released, error, sizeof(error)) && !released);
    assert(cooperative_window_resume(&no_window, NULL, released, error, sizeof(error)));
    fixture_init(&fixture);
    text_alias = fixture.variables[0].text_value;
    number_alias = fixture.variables[1].number_text;
    function_alias = fixture.variables[3].text_value;
    local.type = ZVALUE_STATUS; local.number_value = 1;
    assert(cooperative_window_suspend(&fixture.cache, &released, error, sizeof(error)));
    assert(released && !fixture.locked && fixture.state.variable_count == 4);
    assert(cooperative_window_suspend(&fixture.cache, &nested, error, sizeof(error)) && !nested);
    assert(cooperative_window_resume(&fixture.cache, &fixture.heap, nested, error, sizeof(error)) && !fixture.locked);
    run_sibling(&fixture);
    snprintf(error, sizeof(error), "fixture backend failure");
    assert(cooperative_window_resume(&fixture.cache, &fixture.heap, released, error, sizeof(error)));
    assert(fixture.locked && !strcmp(error, "fixture backend failure"));
    assert(!strcmp(fixture.variables[0].text_value, "after"));
    assert(!strcmp(fixture.variables[1].number_text, "2"));
    assert(!fixture.variables[2].number_value);
    assert(!strcmp(fixture.variables[3].text_value, "Main:Main:After"));
    assert(local.type == ZVALUE_STATUS && local.number_value == 1);
    assert(!strcmp(text_alias, "before") && !strcmp(number_alias, "1") && !strcmp(function_alias, "Main:Main:Before"));
    assert(fixture.heap.text_count == 3);
    fixture_free(&fixture);

    /* Publishing failure never unlocks; restoring failure reacquires first. */
    fixture_init(&fixture);
    fail_copy = 1;
    assert(!cooperative_window_suspend(&fixture.cache, &released, error, sizeof(error)));
    assert(fixture.locked && !released && !fixture.state.initialized);
    assert(cooperative_window_suspend(&fixture.cache, &released, error, sizeof(error)));
    run_sibling(&fixture);
    fail_copy = 1;
    assert(!cooperative_window_resume(&fixture.cache, &fixture.heap, released, error, sizeof(error)));
    assert(fixture.locked && strstr(error, "out of memory"));
    fixture_free(&fixture);

    /* Existing waits use the identical boundary, including failed/canceled
     * waits: state is reloaded and the unwind still owns the room lock. */
    fixture_init(&fixture);
    runtime.state = &fixture; runtime.wait = failing_wait;
    fixture.cache.window_runtime = &runtime;
    assert(!cooperative_window_wait(&fixture.cache, &fixture.heap, "100", error, sizeof(error)));
    assert(fixture.locked && !fixture.variables[2].number_value);
    assert(!strcmp(fixture.variables[0].text_value, "after") && !strcmp(error, "fixture cancellation"));
    fixture_free(&fixture);
    puts("PASS: blocking-operation policy, sibling progress, scalar/status state, private locals, live aliases, nested boundaries, allocation failures and cancellation; no audio/UI accessed");
    return 0;
}
