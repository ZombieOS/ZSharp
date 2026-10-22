#include "interop_embedded.h"
#include "zsharp_interop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
    } \
} while (0)

static int callback(void *context, const char *op, const char *target,
                    const char *member, const char *handle, const char *arguments,
                    char **result, char *error, size_t error_size) {
    const char *value = "{\"$number\":\"123456789012345678901234567890\"}";
    (void)error; (void)error_size;
    CHECK(context == &checks);
    CHECK(strcmp(op, "call") == 0 && strcmp(target, "Main:Main:Callback") == 0);
    CHECK(!*member && !*handle && !strcmp(arguments, "[\"callback argument\"]"));
    *result = (char *)malloc(strlen(value) + 1);
    CHECK(*result != NULL);
    strcpy(*result, value);
    return 1;
}

static void expect(ZSharpEmbeddedInterop **cache, const char *language,
                   const char *path, const char *op, const char *target,
                   const char *member, const char *handle, const char *args,
                   const char *expected) {
    char *result = NULL, error[1024] = {0};
    int ok = zsharp_embedded_interop_request(cache, language, path, op, target,
        member, handle, args, callback, &checks, &result, error, sizeof(error));
    if (!ok) fprintf(stderr, "interop request failed: %s\n", error);
    CHECK(ok == 1 && result != NULL);
    CHECK(strcmp(result, expected) == 0);
    free(result);
}

int main(int argc, char **argv) {
    ZSharpEmbeddedInterop *cache = NULL;
    char *result = NULL, error[1024] = {0};
    char *large;
    const char *targets[] = {"badMissing", "badDouble", "fail"};
    unsigned index;
    CHECK(argc == 3);
    expect(&cache, "c", argv[1], "call", "echo", "", "", "[\"hello\"]", "[\"hello\"]");
    expect(&cache, "c", argv[1], "call", "callback", "", "", "[\"callback argument\"]",
        "{\"$number\":\"123456789012345678901234567890\"}");
    expect(&cache, "c", argv[1], "call", "wrongThread", "", "", "[]", "true");
    expect(&cache, "c", argv[1], "new", "Counter", "", "", "[\"retained\"]",
        "{\"$ref\":\"1\",\"$type\":\"Counter\"}");
    expect(&cache, "c", argv[1], "get", "", "value", "1", "[]", "\"retained\"");
    expect(&cache, "c", argv[1], "set", "", "value", "1", "[true]", "null");
    expect(&cache, "c", argv[1], "get", "", "value", "1", "[]", "true");
    expect(&cache, "c", argv[1], "release", "", "", "1", "[]", "null");
    for (index = 0; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        result = (char *)1;
        error[0] = '\0';
        CHECK(zsharp_embedded_interop_request(&cache, "c", argv[1], "call", targets[index],
            "", "", "[]", callback, &checks, &result, error, sizeof(error)) == 0);
        CHECK(result == NULL && error[0] != '\0');
    }
    CHECK(zsharp_embedded_interop_request(&cache, "c", argv[1], "get", "", "value", "1",
        "[]", callback, &checks, &result, error, sizeof(error)) == 0);
    CHECK(strstr(error, "handle") != NULL);
    /* A library containing only the original v1 symbol falls back, not fails. */
    CHECK(zsharp_embedded_interop_request(&cache, "c", argv[2], "call", "echo", "", "",
        "[]", callback, &checks, &result, error, sizeof(error)) == -1);
    CHECK(result == NULL);
    large = (char *)malloc(ZSHARP_INTEROP_MAX_JSON_BYTES + 2);
    CHECK(large != NULL);
    memset(large, ' ', ZSHARP_INTEROP_MAX_JSON_BYTES + 1);
    large[ZSHARP_INTEROP_MAX_JSON_BYTES + 1] = '\0';
    CHECK(zsharp_embedded_interop_request(&cache, "c", argv[1], "call", "echo", "", "",
        large, callback, &checks, &result, error, sizeof(error)) == 0);
    CHECK(strstr(error, "1 MiB") != NULL && result == NULL);
    free(large);
    zsharp_embedded_interop_free(cache);
    printf("native interop backend: %u checks passed\n", checks);
    return 0;
}
