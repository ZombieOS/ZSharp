#include "zsharp_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct Counter {
    unsigned long long handle;
    char value[4096];
} Counter;

static Counter counters[16];
static unsigned long long next_handle;

typedef struct WrongThread {
    const ZSharpInteropHostV2 *host;
    const ZSharpInteropRequestV2 *request;
    const ZSharpInteropResultV2 *result;
    int rejected;
} WrongThread;

#ifdef _WIN32
static DWORD WINAPI wrong_thread(void *context) {
#else
static void *wrong_thread(void *context) {
#endif
    WrongThread *call = (WrongThread *)context;
    char error[512] = {0};
    int ok = call->host->invoke(call->host->context, call->request, call->result,
                               error, sizeof(error));
    call->rejected = !ok && strstr(error, "task thread") != NULL;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int fail(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
    return 0;
}

static int emit(const ZSharpInteropResultV2 *result, const char *json,
                char *error, size_t size) {
    if (result == NULL || result->write == NULL)
        return fail(error, size, "missing result sink");
    return result->write(result->context, json, strlen(json), error, size);
}

/* A fixture accepts one complete wire value enclosed by the arguments array.
 * Production modules can use any full JSON parser without an ABI dependency.
 */
static int one_value(const char *arguments, char *value, size_t capacity) {
    size_t length = strlen(arguments);
    if (length < 3 || arguments[0] != '[' || arguments[length - 1] != ']'
        || length - 1 >= capacity) return 0;
    memcpy(value, arguments + 1, length - 2);
    value[length - 2] = '\0';
    return 1;
}

ZSHARP_C_EXPORT int zsharp_c_call_v2(
    const ZSharpInteropHostV2 *host, const ZSharpInteropRequestV2 *request,
    const ZSharpInteropResultV2 *result, char *error, size_t error_size) {
    size_t index;
    unsigned long long handle;
    char *end;
    if (request == NULL || request->abi_version != ZSHARP_INTEROP_ABI_V2
        || request->struct_size < sizeof(*request) || !request->operation
        || !request->target || !request->member || !request->handle
        || !request->arguments_json)
        return fail(error, error_size, "unsupported interop request ABI");
    if (strcmp(request->operation, "call") == 0) {
        if (strcmp(request->target, "badMissing") == 0) return 1;
        if (strcmp(request->target, "badDouble") == 0) {
            (void)emit(result, "null", error, error_size);
            (void)emit(result, "null", error, error_size);
            return 1;
        }
        if (strcmp(request->target, "wrongThread") == 0) {
            WrongThread call = {host, request, result, 0};
#ifdef _WIN32
            HANDLE thread = CreateThread(NULL, 0, wrong_thread, &call, 0, NULL);
            if (!thread) return fail(error, error_size, "could not start fixture worker");
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
#else
            pthread_t thread;
            if (pthread_create(&thread, NULL, wrong_thread, &call) != 0)
                return fail(error, error_size, "could not start fixture worker");
            pthread_join(thread, NULL);
#endif
            return emit(result, call.rejected ? "true" : "false", error, error_size);
        }
        if (strcmp(request->target, "echo") == 0)
            return emit(result, request->arguments_json, error, error_size);
        if (strcmp(request->target, "callback") == 0) {
            ZSharpInteropRequestV2 callback = *request;
            if (host == NULL || host->abi_version != ZSHARP_INTEROP_ABI_V2
                || host->struct_size < sizeof(*host) || host->invoke == NULL)
                return fail(error, error_size, "missing Z# callback host");
            callback.target = "Main:Main:Callback";
            return host->invoke(host->context, &callback, result, error, error_size);
        }
        if (strcmp(request->target, "fail") == 0)
            return fail(error, error_size, "C fixture requested failure");
        return fail(error, error_size, "unknown exported C function");
    }
    if (strcmp(request->operation, "new") == 0) {
        char reference[128];
        if (strcmp(request->target, "Counter") != 0)
            return fail(error, error_size, "unknown exported C class");
        for (index = 0; index < 16 && counters[index].handle != 0; ++index) {}
        if (index == 16) return fail(error, error_size, "C fixture instance limit");
        if (!one_value(request->arguments_json, counters[index].value,
                       sizeof(counters[index].value)))
            return fail(error, error_size, "Counter requires one wire value");
        counters[index].handle = ++next_handle;
        snprintf(reference, sizeof(reference), "{\"$ref\":\"%llu\",\"$type\":\"Counter\"}",
                 counters[index].handle);
        return emit(result, reference, error, error_size);
    }
    if (request->handle[0] == '\0') return fail(error, error_size, "missing object handle");
    handle = strtoull(request->handle, &end, 10);
    if (*end != '\0' || handle == 0) return fail(error, error_size, "invalid object handle");
    for (index = 0; index < 16 && counters[index].handle != handle; ++index) {}
    if (index == 16) return fail(error, error_size, "released or unknown object handle");
    if (strcmp(request->operation, "release") == 0) {
        counters[index].handle = 0;
        return emit(result, "null", error, error_size);
    }
    if (strcmp(request->operation, "get") == 0 && strcmp(request->member, "value") == 0)
        return emit(result, counters[index].value, error, error_size);
    if (strcmp(request->operation, "set") == 0 && strcmp(request->member, "value") == 0) {
        if (!one_value(request->arguments_json, counters[index].value,
                       sizeof(counters[index].value)))
            return fail(error, error_size, "value requires one wire value");
        return emit(result, "null", error, error_size);
    }
    if (strcmp(request->operation, "invoke") == 0 && strcmp(request->member, "echo") == 0)
        return emit(result, request->arguments_json, error, error_size);
    return fail(error, error_size, "unknown C object member or operation");
}

/* Both exports may coexist. The VM must prefer v2 when available. */
ZSHARP_C_EXPORT int zsharp_c_call_v1(
    uint32_t version, const char *function, const ZSharpCValue *arguments,
    size_t count, ZSharpCValue *result, char *error, size_t error_size) {
    (void)arguments;
    if (version != ZSHARP_C_ABI_VERSION || count != 0 || strcmp(function, "legacy") != 0)
        return fail(error, error_size, "unexpected legacy C call");
    result->type = ZSHARP_C_NUMBER;
    result->number = 99;
    result->text = NULL;
    return 1;
}
