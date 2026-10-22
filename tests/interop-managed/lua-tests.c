#define _CRT_SECURE_NO_WARNINGS
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "interop_script_lua.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Host {
    ZSharpLuaInterop **cache;
    const char *module;
    int value;
} Host;
static unsigned checks;

static int dispatch(void *context, const char *operation, const char *target,
    const char *member, const char *handle, const char *arguments,
    char **result, char *error, size_t size) {
    Host *host = (Host *)context;
    const char *wire = "null";
    if (!strcmp(operation, "host.call") && !strcmp(target, "Tests:Bridge:Double"))
        wire = "{\"$number\":\"42\"}";
    else if (!strcmp(operation, "host.call") && !strcmp(target, "Tests:Bridge:Reentrant")) {
        char *nested = NULL;
        int ok = zsharp_lua_interop_request(host->cache, host->module, "call", "counted", "", "", "[]",
            dispatch, host, &nested, error, size);
        free(nested);
        assert(!ok && strstr(error, "reentrant"));
        wire = "true";
    } else if (!strcmp(operation, "host.get")) {
        assert(!strcmp(handle, "z1") && !strcmp(member, "value"));
        wire = "{\"$number\":\"10\"}";
    } else if (!strcmp(operation, "host.set")) {
        assert(!strcmp(arguments, "[{\"$number\":\"13\"}]"));
        host->value = 13;
    } else if (!strcmp(operation, "host.invoke")) {
        assert(!strcmp(handle, "z1") && !strcmp(member, "add"));
        wire = "{\"$number\":\"20\"}";
    } else { snprintf(error, size, "unexpected host callback"); return 0; }
    *result = (char *)malloc(strlen(wire) + 1);
    assert(*result);
    strcpy(*result, wire);
    checks++;
    return 1;
}

static void request(Host *host, const char *operation, const char *target,
    const char *member, const char *handle, const char *arguments,
    const char *expected) {
    char *result = NULL, error[1024] = {0};
    int ok = zsharp_lua_interop_request(host->cache, host->module, operation,
        target, member, handle, arguments, dispatch, host, &result, error, sizeof(error));
    if (!ok || !result || strcmp(result, expected)) {
        fprintf(stderr, "%s %s: expected %s; received %s (%s)\n", operation, target,
            expected, result ? result : "<null>", error);
        abort();
    }
    free(result);
    checks++;
}

static void failure(Host *host, const char *operation, const char *target,
    const char *member, const char *handle, const char *arguments,
    const char *expected) {
    char *result = NULL, error[1024] = {0};
    int ok = zsharp_lua_interop_request(host->cache, host->module, operation,
        target, member, handle, arguments, dispatch, host, &result, error, sizeof(error));
    if (ok || !strstr(error, expected)) {
        fprintf(stderr, "Expected %s; received %s / %s\n", expected, error, result ? result : "");
        abort();
    }
    free(result);
    checks++;
}

int main(int argc, char **argv) {
    ZSharpLuaInterop *cache = NULL;
    Host host = {&cache, NULL, 10};
    unsigned index;
    assert(argc == 2);
    host.module = argv[1];
    request(&host, "call", "counted", "", "", "[]", "{\"$number\":\"1\"}");
    request(&host, "call", "counted", "", "", "[]", "{\"$number\":\"2\"}");
    request(&host, "call", "echo", "", "", "[{\"$number\":\"9007199254740993\"}]", "{\"$number\":\"9007199254740993\"}");
    request(&host, "call", "echo", "", "", "[{\"$number\":\"0.12345678901234567890123456789\"}]", "{\"$number\":\"0.12345678901234567890123456789\"}");
    request(&host, "call", "echo", "", "", "[{\"$number\":\"1e-400\"}]", "{\"$number\":\"1e-400\"}");
    request(&host, "call", "echo", "", "", "[{\"$number\":\"0.1\"}]", "{\"$number\":\"0.1\"}");
    failure(&host, "call", "echo", "", "", "[{\"$number\":\"0x10\"}]", "decimal text");
    request(&host, "call", "echo", "", "", "[[\"a\",\"b\"]]", "[\"a\",\"b\"]");
    request(&host, "call", "echo", "", "", "[[]]", "[]");
    request(&host, "call", "echo", "", "", "[true]", "true");
    request(&host, "call", "echo", "", "", "[null]", "null");
    request(&host, "new", "Counter", "", "", "[{\"$number\":\"4\"}]", "{\"$ref\":\"1\",\"$type\":\"LuaTable\"}");
    request(&host, "get", "", "value", "1", "[]", "{\"$number\":\"4\"}");
    request(&host, "set", "", "value", "1", "[{\"$number\":\"10\"}]", "null");
    request(&host, "invoke", "", "add", "1", "[{\"$number\":\"5\"}]", "{\"$number\":\"15\"}");
    request(&host, "invoke", "", "self", "1", "[]", "{\"$ref\":\"1\",\"$type\":\"LuaTable\"}");
    request(&host, "call", "callback", "", "", "[{\"$number\":\"21\"}]", "{\"$number\":\"42\"}");
    request(&host, "call", "proxy", "", "", "[{\"$zref\":\"z1\",\"$type\":\"Counter\"}]", "{\"$number\":\"20\"}");
    assert(host.value == 13);
    request(&host, "call", "reentrant", "", "", "[]", "true");
    request(&host, "new", "Empty", "", "", "[]", "{\"$ref\":\"2\",\"$type\":\"LuaTable\"}");
    failure(&host, "call", "fail", "", "", "[]", "intentional Lua failure");
    for (index = 0; index < 40; index++) {
        failure(&host, "call", "nonfinite", "", "", "[]", "non-finite");
        failure(&host, "call", "echo", "", "", "[{\"$ref\":\"10000\"}]", "released");
        failure(&host, "call", "echo", "", "", "[{\"$record\":{\"x\":{\"$ref\":\"10000\"}}}]", "released");
    }
    request(&host, "release", "", "", "1", "[]", "null");
    failure(&host, "get", "", "value", "1", "[]", "released");
    failure(&host, "call", "timeout", "", "", "[]", "5 second");
    request(&host, "call", "counted", "", "", "[]", "{\"$number\":\"3\"}");
    zsharp_lua_interop_free(cache);
    printf("Persistent Lua interop: %u checks passed\n", checks);
    return 0;
}
