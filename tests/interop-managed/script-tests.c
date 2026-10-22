#include "interop_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HostState {
    ZSharpScriptInterop **cache;
    const char *module;
    int value;
    int callbacks;
} HostState;

static unsigned checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        exit(1);
    }
}

static char *copy_text(const char *value) {
    size_t length = strlen(value);
    char *result = (char *)malloc(length + 1);
    if (result) memcpy(result, value, length + 1);
    return result;
}

static int host(void *opaque, const char *operation, const char *target,
                 const char *member, const char *handle, const char *arguments,
                 char **result, char *error, size_t size) {
    HostState *state = (HostState *)opaque;
    char buffer[128];
    state->callbacks++;
    if (strcmp(operation, "host.call") == 0) {
        if (strcmp(target, "Tests:Bridge:Reenter") == 0) {
            char *nested_result = NULL, nested_error[512] = {0};
            int nested = zsharp_script_interop_request(state->cache, "js", state->module,
                "call", "counted", "", "", "[]", host, state, &nested_result,
                nested_error, sizeof(nested_error));
            check(!nested && strstr(nested_error, "Reentrant"), "active-module reentry is rejected");
            free(nested_result);
            *result = copy_text("true");
            return *result != NULL;
        }
        check(strcmp(target, "Tests:Bridge:Double") == 0, "callback preserves target");
        check(strcmp(arguments, "[{\"$number\":\"21\"}]") == 0, "callback encodes arguments");
        *result = copy_text("{\"$number\":\"42\"}");
        return *result != NULL;
    }
    check(strcmp(handle, "z-object-1") == 0, "proxy preserves Z# identity");
    if (strcmp(operation, "host.get") == 0) {
        check(strcmp(member, "value") == 0, "proxy get member");
        snprintf(buffer, sizeof(buffer), "{\"$number\":\"%d\"}", state->value);
    } else if (strcmp(operation, "host.set") == 0) {
        check(strcmp(member, "value") == 0, "proxy set member");
        check(strcmp(arguments, "[{\"$number\":\"21\"}]") == 0, "proxy set value");
        state->value = 21;
        snprintf(buffer, sizeof(buffer), "null");
    } else if (strcmp(operation, "host.invoke") == 0) {
        check(strcmp(member, "increase") == 0, "proxy invoke member");
        check(strcmp(arguments, "[{\"$number\":\"7\"}]") == 0, "proxy invoke arguments");
        snprintf(buffer, sizeof(buffer), "{\"$number\":\"28\"}");
    } else {
        snprintf(error, size, "Unsupported callback");
        return 0;
    }
    *result = copy_text(buffer);
    return *result != NULL;
}

static void request(HostState *host_state, const char *path, const char *operation,
                     const char *target, const char *member, const char *reference,
                     const char *arguments, const char *expected) {
    char error[2048] = {0};
    char *result = NULL;
    int ok = zsharp_script_interop_request(host_state->cache, "js", path,
        operation, target, member, reference, arguments, host, host_state,
        &result, error, sizeof(error));
    if (!ok || !result || strcmp(result, expected) != 0) {
        fprintf(stderr, "%s %s: got %s; error %s; expected %s\n", operation,
            target, result ? result : "<null>", error, expected);
        check(0, "script request exact result");
    }
    check(ok, "script request succeeds");
    free(result);
}

static void failure(HostState *host_state, const char *operation,
                     const char *target, const char *member, const char *reference,
                     const char *arguments, const char *expected) {
    char error[2048] = {0};
    char *result = NULL;
    int ok = zsharp_script_interop_request(host_state->cache, "js", host_state->module,
        operation, target, member, reference, arguments, host, host_state,
        &result, error, sizeof(error));
    if (ok || !strstr(error, expected)) {
        fprintf(stderr, "Expected failure %s, got %s (%s)\n", expected, result ? result : "", error);
        check(0, "script request gives clear failure");
    }
    check(!ok && !result, "failed operation has no result");
    free(result);
}

int main(int argc, char **argv) {
    ZSharpScriptInterop *cache = NULL;
    HostState state = {&cache, argc > 1 ? argv[1] : "", 7, 0};
    if (argc != 4) { fputs("usage: script-tests module.js legacy.js default.js\n", stderr); return 2; }
    request(&state, argv[1], "call", "add", "", "", "[{\"$number\":\"20\"},{\"$number\":\"22\"}]", "{\"$number\":\"42\"}");
    request(&state, argv[1], "call", "counted", "", "", "[]", "{\"$number\":\"1\"}");
    request(&state, argv[1], "call", "counted", "", "", "[]", "{\"$number\":\"2\"}");
    request(&state, argv[1], "call", "exact", "", "", "[{\"$number\":\"123456789012345678901234567890.12345678901234567890\"}]", "{\"$number\":\"123456789012345678901234567890.12345678901234567890\"}");
    request(&state, argv[1], "call", "exact", "", "", "[{\"$number\":\"1e-400\"}]", "{\"$number\":\"1e-400\"}");
    request(&state, argv[1], "call", "array", "", "", "[[\"a\",\"b\"]]", "[\"a\",\"b\"]");
    request(&state, argv[1], "call", "record", "", "", "[]", "{\"$record\":{\"name\":\"bridge\",\"value\":{\"$number\":\"42\"}}}");
    request(&state, argv[1], "new", "Counter", "", "", "[{\"$number\":\"4\"}]", "{\"$ref\":\"1\",\"$type\":\"Counter\"}");
    request(&state, argv[1], "get", "", "value", "1", "[]", "{\"$number\":\"4\"}");
    request(&state, argv[1], "set", "", "value", "1", "[{\"$number\":\"10\"}]", "null");
    request(&state, argv[1], "invoke", "", "increase", "1", "[{\"$number\":\"5\"}]", "{\"$number\":\"15\"}");
    request(&state, argv[1], "invoke", "", "self", "1", "[]", "{\"$ref\":\"1\",\"$type\":\"Counter\"}");
    request(&state, argv[1], "call", "callback", "", "", "[{\"$number\":\"21\"}]", "{\"$number\":\"42\"}");
    request(&state, argv[1], "call", "proxy", "", "", "[{\"$zref\":\"z-object-1\",\"$type\":\"Tests.Bridge\"}]", "{\"$number\":\"28\"}");
    check(state.value == 21, "Z# proxy writes original object");
    request(&state, argv[1], "call", "reentrant", "", "", "[]", "true");
    request(&state, argv[1], "call", "promised", "", "", "[{\"$number\":\"41\"}]", "{\"$number\":\"42\"}");
    failure(&state, "call", "never", "", "", "[]", "external event loop");
    failure(&state, "call", "nonfinite", "", "", "[]", "Non-finite");
    failure(&state, "call", "constructor.constructor('bad')", "", "", "[]", "Invalid");
    failure(&state, "get", "", "__proto__", "1", "[]", "prototype");
    request(&state, argv[1], "release", "", "", "1", "[]", "null");
    failure(&state, "get", "", "value", "1", "[]", "released");
    request(&state, argv[2], "call", "greeting", "", "", "[\"Z#\"]", "\"Hello Z#\"");
    request(&state, argv[2], "call", "counted", "", "", "[]", "{\"$number\":\"1\"}");
    request(&state, argv[2], "call", "counted", "", "", "[]", "{\"$number\":\"2\"}");
    request(&state, argv[2], "new", "Counter", "", "", "[{\"$number\":\"99\"}]", "{\"$ref\":\"1\",\"$type\":\"Counter\"}");
    request(&state, argv[3], "call", "greeting", "", "", "[\"Z#\"]", "\"Hello Z#\"");
    failure(&state, "call", "timeout", "", "", "[]", "interrupted");
    request(&state, argv[1], "call", "counted", "", "", "[]", "{\"$number\":\"3\"}");
    zsharp_script_interop_free(cache);
    printf("Persistent JavaScript interop: %u checks passed\n", checks);
    return 0;
}
