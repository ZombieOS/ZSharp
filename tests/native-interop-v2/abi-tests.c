#include "zsharp_c.h"
#include "zsharp_cpp.h"
#include "zsharp_rust.h"
#include "zsharp_kotlin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct LegacyLayout {
    ZSharpCValueType type;
    double number;
    const char *text;
} LegacyLayout;
_Static_assert(ZSHARP_C_ABI_VERSION == 1, "v1 C version changed");
_Static_assert(ZSHARP_CPP_ABI_VERSION == 1, "v1 C++ version changed");
_Static_assert(ZSHARP_RUST_ABI_VERSION == 1, "v1 Rust version changed");
_Static_assert(sizeof(ZSharpCValue) == sizeof(LegacyLayout), "v1 C layout changed");
_Static_assert(sizeof(ZSharpCppValue) == sizeof(LegacyLayout), "v1 C++ layout changed");
_Static_assert(offsetof(ZSharpCValue, number) == offsetof(LegacyLayout, number), "v1 number offset changed");
_Static_assert(offsetof(ZSharpCValue, text) == offsetof(LegacyLayout, text), "v1 text offset changed");

typedef struct Capture {
    char *json;
    size_t count;
    int failed;
} Capture;
static unsigned checks;
static unsigned callbacks;
#define CHECK(condition) do { \
    ++checks; if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
    } \
} while (0)

static int capture(void *context, const char *json, size_t length,
                   char *error, size_t error_size) {
    Capture *output = (Capture *)context;
    ++output->count;
    if (output->count != 1 || json == NULL || length > ZSHARP_INTEROP_MAX_JSON_BYTES) {
        output->failed = 1;
        if (error && error_size) snprintf(error, error_size, "invalid or repeated result");
        return 0;
    }
    output->json = (char *)malloc(length + 1);
    if (!output->json) return 0;
    memcpy(output->json, json, length);
    output->json[length] = '\0';
    return 1;
}

static int callback(void *context, const ZSharpInteropRequestV2 *request,
                    const ZSharpInteropResultV2 *sink, char *error, size_t size) {
    const char *result = "{\"$record\":{\"called\":true,\"exact\":{\"$number\":\"100000000000000000000000000000000000000000000000001\"}}}";
    CHECK(context == &callbacks);
    CHECK(request->abi_version == 2);
    CHECK(strcmp(request->operation, "call") == 0);
    CHECK(strcmp(request->target, "Main:Main:Callback") == 0);
    CHECK(strcmp(request->arguments_json, "[\"callback argument\"]") == 0);
    ++callbacks;
    return sink->write(sink->context, result, strlen(result), error, size);
}

static char *request(ZSharpInteropCallV2 entry, const ZSharpInteropHostV2 *host,
                     const char *op, const char *target, const char *member,
                     const char *handle, const char *arguments) {
    ZSharpInteropRequestV2 call = {2, sizeof(call), op, target, member, handle, arguments};
    Capture output = {0};
    ZSharpInteropResultV2 sink = {&output, capture};
    char error[1024] = {0};
    int ok = entry(host, &call, &sink, error, sizeof(error));
    if (!ok) fprintf(stderr, "interop failure: %s\n", error);
    CHECK(ok == 1);
    CHECK(output.count == 1 && !output.failed && output.json != NULL);
    return output.json;
}

static void expect(ZSharpInteropCallV2 entry, const ZSharpInteropHostV2 *host,
                   const char *op, const char *target, const char *member,
                   const char *handle, const char *arguments, const char *expected) {
    char *actual = request(entry, host, op, target, member, handle, arguments);
    CHECK(strcmp(actual, expected) == 0);
    free(actual);
}

static void suite(const char *path, const char *symbol) {
    ZSharpInteropCallV2 entry;
    ZSharpInteropHostV2 host = {2, sizeof(host), &callbacks, callback};
    const char *complex = "[null,true,false,\"hello \\u2728\",{\"$number\":\"123456789012345678901234567890.000000000000000000000001\"},[\"nested\"],{\"$record\":{\"title\":\"record\"}},{\"$zref\":\"7\",\"$type\":\"Main.Room\"}]";
    char handle[64], other_handle[64], error[1024];
    char *reference;
    void *library;
#ifdef _WIN32
    library = (void *)LoadLibraryExA(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) fprintf(stderr, "LoadLibrary error %lu for %s\n", (unsigned long)GetLastError(), path);
    CHECK(library != NULL);
    entry = (ZSharpInteropCallV2)(void *)GetProcAddress((HMODULE)library, symbol);
#else
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) fprintf(stderr, "%s\n", dlerror());
    CHECK(library != NULL);
    entry = (ZSharpInteropCallV2)dlsym(library, symbol);
#endif
    CHECK(entry != NULL);
    expect(entry, &host, "call", "echo", "", "", complex, complex);
    expect(entry, &host, "call", "callback", "", "", "[\"callback argument\"]",
        "{\"$record\":{\"called\":true,\"exact\":{\"$number\":\"100000000000000000000000000000000000000000000000001\"}}}");
    reference = request(entry, &host, "new", "Counter", "", "", "[{\"$number\":\"10\"}]");
    CHECK(sscanf(reference, "{\"$ref\":\"%63[0-9]", handle) == 1);
    CHECK(strstr(reference, "\"$type\":\"Counter\"") != NULL);
    free(reference);
    reference = request(entry, &host, "new", "Counter", "", "", "[\"second\"]");
    CHECK(sscanf(reference, "{\"$ref\":\"%63[0-9]", other_handle) == 1);
    CHECK(strcmp(handle, other_handle) != 0);
    free(reference);
    expect(entry, &host, "get", "", "value", handle, "[]", "{\"$number\":\"10\"}");
    expect(entry, &host, "set", "", "value", handle, "[{\"$number\":\"999999999999999999999999999999999999\"}]", "null");
    expect(entry, &host, "get", "", "value", handle, "[]", "{\"$number\":\"999999999999999999999999999999999999\"}");
    expect(entry, &host, "get", "", "value", other_handle, "[]", "\"second\"");
    expect(entry, &host, "invoke", "", "echo", handle, complex, complex);
    expect(entry, &host, "release", "", "", handle, "[]", "null");
    {
        ZSharpInteropRequestV2 call = {2, sizeof(call), "get", "", "value", handle, "[]"};
        Capture output = {0};
        ZSharpInteropResultV2 sink = {&output, capture};
        error[0] = '\0';
        CHECK(entry(&host, &call, &sink, error, sizeof(error)) == 0);
        CHECK(output.count == 0 && strstr(error, "handle") != NULL);
        call.abi_version = 999;
        CHECK(entry(&host, &call, &sink, error, sizeof(error)) == 0);
        CHECK(strstr(error, "ABI") != NULL);
    }
    expect(entry, &host, "release", "", "", other_handle, "[]", "null");
    /* v1 headers and entrypoints continue to work without any v2 types. */
    if (strcmp(symbol, ZSHARP_RUST_ENTRY_V2_NAME) != 0) {
        ZSharpCppCallV1 old_entry;
        ZSharpCppValue value = {0};
#ifdef _WIN32
        old_entry = (ZSharpCppCallV1)(void *)GetProcAddress((HMODULE)library,
            strcmp(symbol, ZSHARP_C_ENTRY_V2_NAME) == 0 ? ZSHARP_C_ENTRY_NAME : ZSHARP_CPP_ENTRY_NAME);
#else
        old_entry = (ZSharpCppCallV1)dlsym(library,
            strcmp(symbol, ZSHARP_C_ENTRY_V2_NAME) == 0 ? ZSHARP_C_ENTRY_NAME : ZSHARP_CPP_ENTRY_NAME);
#endif
        CHECK(old_entry != NULL);
        CHECK(old_entry(1, "legacy", NULL, 0, &value, error, sizeof(error)) == 1);
        CHECK(value.type == ZSHARP_CPP_NUMBER);
        CHECK(value.number == (strcmp(symbol, ZSHARP_C_ENTRY_V2_NAME) == 0 ? 99 : 88));
    }
    printf("native interop v2: %s passed\n", symbol);
    /* Retain through process exit, matching production module lifetime. */
}

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    suite(argv[1], ZSHARP_C_ENTRY_V2_NAME);
    suite(argv[2], ZSHARP_CPP_ENTRY_V2_NAME);
    if (argc == 4) suite(argv[3], ZSHARP_RUST_ENTRY_V2_NAME);
    CHECK(callbacks == (unsigned)(argc - 1));
    printf("native interop v2: %u checks passed\n", checks);
    return 0;
}
