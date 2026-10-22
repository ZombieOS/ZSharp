#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "interop_embedded.h"
#include "zsharp_interop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION InteropMutex;
static SRWLOCK cache_mutex = SRWLOCK_INIT;
#define CACHE_LOCK() AcquireSRWLockExclusive(&cache_mutex)
#define CACHE_UNLOCK() ReleaseSRWLockExclusive(&cache_mutex)
static void mutex_initialize(InteropMutex *mutex) { InitializeCriticalSection(mutex); }
static void mutex_enter(InteropMutex *mutex) { EnterCriticalSection(mutex); }
static void mutex_leave(InteropMutex *mutex) { LeaveCriticalSection(mutex); }
static void mutex_destroy(InteropMutex *mutex) { DeleteCriticalSection(mutex); }
#else
#include <dlfcn.h>
#include <pthread.h>
typedef pthread_mutex_t InteropMutex;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#define CACHE_LOCK() pthread_mutex_lock(&cache_mutex)
#define CACHE_UNLOCK() pthread_mutex_unlock(&cache_mutex)
static void mutex_initialize(InteropMutex *mutex) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
static void mutex_enter(InteropMutex *mutex) { pthread_mutex_lock(mutex); }
static void mutex_leave(InteropMutex *mutex) { pthread_mutex_unlock(mutex); }
static void mutex_destroy(InteropMutex *mutex) { pthread_mutex_destroy(mutex); }
#endif

typedef struct InteropModule {
    char *path;
    char *language;
    void *library;
    ZSharpInteropCallV2 entry;
    InteropMutex mutex;
    struct InteropModule *next;
} InteropModule;

struct ZSharpEmbeddedInterop {
    InteropModule *modules;
};

typedef struct InteropCapture {
    char *json;
    unsigned count;
    int failed;
} InteropCapture;

typedef struct InteropHostCall {
    ZSharpInteropHostDispatch dispatch;
    void *context;
    int active;
#ifdef _WIN32
    DWORD thread;
#else
    pthread_t thread;
#endif
} InteropHostCall;

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return 0;
}

static char *copy_text(const char *value) {
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static size_t bounded_length(const char *text) {
    size_t length = 0;
    if (!text) return ZSHARP_INTEROP_MAX_JSON_BYTES + 1;
    while (length <= ZSHARP_INTEROP_MAX_JSON_BYTES && text[length]) ++length;
    return length;
}

static int capture_json(void *context, const char *json, size_t length,
                        char *error, size_t size) {
    InteropCapture *capture = (InteropCapture *)context;
    ++capture->count;
    if (capture->count != 1 || !json || length == 0
        || length > ZSHARP_INTEROP_MAX_JSON_BYTES || memchr(json, '\0', length)) {
        capture->failed = 1;
        return fail(error, size, "native interop must emit one JSON value of at most 1 MiB");
    }
    capture->json = (char *)malloc(length + 1);
    if (!capture->json) {
        capture->failed = 1;
        return fail(error, size, "out of memory receiving native interop result");
    }
    memcpy(capture->json, json, length);
    capture->json[length] = '\0';
    return 1;
}

static int host_invoke(void *context, const ZSharpInteropRequestV2 *request,
                       const ZSharpInteropResultV2 *result,
                       char *error, size_t error_size) {
    InteropHostCall *call = (InteropHostCall *)context;
    char *json = NULL;
    int ok;
    if (!call || !call->active)
        return fail(error, error_size, "Z# host callbacks are only valid during a synchronous native call");
#ifdef _WIN32
    if (call->thread != GetCurrentThreadId())
#else
    if (!pthread_equal(call->thread, pthread_self()))
#endif
        return fail(error, error_size, "Z# host callbacks must run on the calling ZVM task thread");
    if (!call->dispatch || !request || request->abi_version != ZSHARP_INTEROP_ABI_V2
        || request->struct_size < sizeof(*request) || !request->operation
        || !request->target || !request->member || !request->handle
        || !request->arguments_json || !result || !result->write)
        return fail(error, error_size, "invalid Z# native callback ABI or arguments");
    if (strcmp(request->operation, "call") && strcmp(request->operation, "get")
        && strcmp(request->operation, "set") && strcmp(request->operation, "invoke"))
        return fail(error, error_size, "Z# host callbacks support call, get, set, and invoke");
    if (bounded_length(request->arguments_json) > ZSHARP_INTEROP_MAX_JSON_BYTES)
        return fail(error, error_size, "Z# callback JSON exceeds 1 MiB");
    ok = call->dispatch(call->context, request->operation, request->target,
        request->member, request->handle, request->arguments_json,
        &json, error, error_size);
    if (ok) {
        size_t length = bounded_length(json);
        if (length == 0 || length > ZSHARP_INTEROP_MAX_JSON_BYTES)
            ok = fail(error, error_size, "Z# callback returned missing or oversized JSON");
        else ok = result->write(result->context, json, length, error, error_size);
    }
    free(json);
    return ok;
}

static const char *native_symbol(const char *language) {
    if (!strcmp(language, "c")) return "zsharp_c_call_v2";
    if (!strcmp(language, "cpp")) return "zsharp_cpp_call_v2";
    if (!strcmp(language, "rust")) return "zsharp_rust_call_v2";
    if (!strcmp(language, "kt") || !strcmp(language, "kotlin")) return "zsharp_kotlin_call_v2";
    return NULL;
}

static char *canonical_path(const char *path, char *error, size_t size) {
#ifdef _WIN32
    wchar_t *wide, *absolute;
    char *utf8;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    DWORD required;
    if (!count) { fail(error, size, "native module path is not valid UTF-8"); return NULL; }
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (!wide) { fail(error, size, "out of memory"); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count);
    required = GetFullPathNameW(wide, 0, NULL, NULL);
    absolute = required ? (wchar_t *)malloc((size_t)required * sizeof(*absolute)) : NULL;
    if (!absolute || !GetFullPathNameW(wide, required, absolute, NULL)) {
        free(wide); free(absolute); fail(error, size, "could not resolve native module path"); return NULL;
    }
    free(wide);
    count = WideCharToMultiByte(CP_UTF8, 0, absolute, -1, NULL, 0, NULL, NULL);
    utf8 = count ? (char *)malloc((size_t)count) : NULL;
    if (utf8) WideCharToMultiByte(CP_UTF8, 0, absolute, -1, utf8, count, NULL, NULL);
    free(absolute);
    if (!utf8) fail(error, size, "out of memory resolving native module path");
    return utf8;
#else
    char *absolute = realpath(path, NULL);
    if (!absolute) fail(error, size, "could not resolve native module path");
    return absolute;
#endif
}

static InteropModule *find_module(ZSharpEmbeddedInterop **cache,
    const char *language, const char *path, const char *symbol,
    char *error, size_t size) {
    InteropModule *module;
    char *absolute = canonical_path(path, error, size);
    if (!absolute) return NULL;
    CACHE_LOCK();
    if (!*cache) *cache = (ZSharpEmbeddedInterop *)calloc(1, sizeof(**cache));
    if (!*cache) {
        CACHE_UNLOCK(); free(absolute); fail(error, size, "out of memory"); return NULL;
    }
    for (module = (*cache)->modules; module; module = module->next) {
#ifdef _WIN32
        int same_path = _stricmp(module->path, absolute) == 0;
#else
        int same_path = strcmp(module->path, absolute) == 0;
#endif
        if (same_path && strcmp(module->language, language) == 0) {
            CACHE_UNLOCK(); free(absolute); return module;
        }
    }
    module = (InteropModule *)calloc(1, sizeof(*module));
    if (!module) {
        CACHE_UNLOCK(); free(absolute); fail(error, size, "out of memory"); return NULL;
    }
    module->path = absolute;
    module->language = copy_text(language);
    if (!module->language) {
        CACHE_UNLOCK(); free(module->path); free(module); fail(error, size, "out of memory"); return NULL;
    }
#ifdef _WIN32
    {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, absolute, -1, NULL, 0);
        wchar_t *wide = count ? (wchar_t *)malloc((size_t)count * sizeof(*wide)) : NULL;
        if (wide) {
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, absolute, -1, wide, count);
            module->library = (void *)LoadLibraryExW(wide, NULL,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            free(wide);
        }
        if (!module->library && error && size)
            snprintf(error, size, "could not load native interop module '%s' (Windows error %lu)",
                absolute, (unsigned long)GetLastError());
    }
    if (module->library) module->entry = (ZSharpInteropCallV2)(void *)GetProcAddress((HMODULE)module->library, symbol);
#else
    module->library = dlopen(absolute, RTLD_NOW | RTLD_LOCAL);
    if (!module->library && error && size)
        snprintf(error, size, "could not load native interop module '%s': %s", absolute, dlerror());
    if (module->library) module->entry = (ZSharpInteropCallV2)dlsym(module->library, symbol);
#endif
    if (!module->library) {
        CACHE_UNLOCK(); free(module->path); free(module->language); free(module); return NULL;
    }
    mutex_initialize(&module->mutex);
    module->next = (*cache)->modules;
    (*cache)->modules = module;
    CACHE_UNLOCK();
    return module;
}

int zsharp_embedded_interop_request(
    ZSharpEmbeddedInterop **cache, const char *language, const char *module_path,
    const char *operation, const char *target, const char *member,
    const char *handle, const char *arguments_json,
    ZSharpInteropHostDispatch host_dispatch, void *host_context,
    char **result_json, char *error, size_t error_size) {
    const char *symbol;
    InteropModule *module;
    InteropCapture capture = {0};
    InteropHostCall call;
    ZSharpInteropHostV2 host;
    ZSharpInteropRequestV2 request;
    ZSharpInteropResultV2 sink;
    int ok;
    if (result_json) *result_json = NULL;
    if (!cache || !language || !module_path || !operation || !target || !member
        || !handle || !arguments_json || !result_json)
        return fail(error, error_size, "invalid embedded interop request");
    symbol = native_symbol(language);
    if (!symbol) return -1;
    if (bounded_length(arguments_json) > ZSHARP_INTEROP_MAX_JSON_BYTES)
        return fail(error, error_size, "native interop arguments exceed 1 MiB");
    module = find_module(cache, language, module_path, symbol, error, error_size);
    if (!module) return 0;
    if (!module->entry) return -1;
    memset(&call, 0, sizeof(call));
    call.dispatch = host_dispatch;
    call.context = host_context;
    call.active = 1;
#ifdef _WIN32
    call.thread = GetCurrentThreadId();
#else
    call.thread = pthread_self();
#endif
    host.abi_version = ZSHARP_INTEROP_ABI_V2;
    host.struct_size = sizeof(host);
    host.context = &call;
    host.invoke = host_invoke;
    request.abi_version = ZSHARP_INTEROP_ABI_V2;
    request.struct_size = sizeof(request);
    request.operation = operation;
    request.target = target;
    request.member = member;
    request.handle = handle;
    request.arguments_json = arguments_json;
    sink.context = &capture;
    sink.write = capture_json;
    mutex_enter(&module->mutex);
    ok = module->entry(&host, &request, &sink, error, error_size);
    call.active = 0;
    mutex_leave(&module->mutex);
    if (!ok || capture.failed || capture.count != 1 || !capture.json) {
        free(capture.json);
        if (ok || (!error || !error_size || !error[0]))
            fail(error, error_size, "native interop failed or did not return exactly one JSON value");
        return 0;
    }
    *result_json = capture.json;
    return 1;
}

void zsharp_embedded_interop_free(ZSharpEmbeddedInterop *cache) {
    InteropModule *module;
    if (!cache) return;
    CACHE_LOCK();
    module = cache->modules;
    while (module) {
        InteropModule *next = module->next;
        /* Do not dlclose/FreeLibrary: a native SDK's background threads may
         * still execute module code. The OS reclaims libraries at exit. */
        mutex_destroy(&module->mutex);
        free(module->path);
        free(module->language);
        free(module);
        module = next;
    }
    free(cache);
    CACHE_UNLOCK();
}
