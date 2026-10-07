#include "zsharp_c.h"

#include <stdio.h>
#include <string.h>

static char event_target[1024];
static int event_pending;

ZSHARP_C_EXPORT int zsharp_c_call_v1(
    uint32_t abi_version, const char *function,
    const ZSharpCValue *arguments, size_t argument_count,
    ZSharpCValue *result, char *error, size_t error_size) {
    static char greeting[256];
    static int counter;
    if (abi_version != ZSHARP_C_ABI_VERSION) {
        snprintf(error, error_size, "unsupported ABI");
        return 0;
    }
    if (strcmp(function, "fixture_bind") == 0 && argument_count == 4) {
        if (arguments[0].type != ZSHARP_C_TEXT || arguments[1].type != ZSHARP_C_TEXT ||
            arguments[2].type != ZSHARP_C_TEXT || arguments[3].type != ZSHARP_C_TEXT ||
            strcmp(arguments[2].text, "Value") || strcmp(arguments[3].text, "number")) return 0;
        snprintf(event_target, sizeof(event_target), "%s", arguments[1].text);
        result->type = ZSHARP_C_NULL; return 1;
    }
    if (strcmp(function, "fixture_start") == 0 && argument_count == 0) {
        event_pending = 1; result->type = ZSHARP_C_NULL; return 1;
    }
    if (strcmp(function, "fixture_option") == 0 && argument_count == 4) {
        if (arguments[0].type != ZSHARP_C_TEXT || strcmp(arguments[0].text, event_target) ||
            arguments[1].type != ZSHARP_C_TEXT || strcmp(arguments[1].text, "?") ||
            arguments[2].type != ZSHARP_C_TEXT || strcmp(arguments[2].text, "!") ||
            arguments[3].type != ZSHARP_C_TEXT || strcmp(arguments[3].text, "*")) return 0;
        printf("FUNCTION OPTION BOUND\n"); result->type = ZSHARP_C_NULL; return 1;
    }
    if (strcmp(function, "fixture_duration") == 0 && argument_count == 2) {
        if (arguments[0].type != ZSHARP_C_TEXT || strcmp(arguments[0].text, event_target) ||
            arguments[1].type != ZSHARP_C_NUMBER || arguments[1].number != 5000) return 0;
        printf("FUNCTION DURATION BOUND\n"); result->type = ZSHARP_C_NULL; return 1;
    }
    if (strcmp(function, "fixture_config") == 0 && argument_count == 5) {
        if (arguments[0].type != ZSHARP_C_TEXT || strcmp(arguments[0].text, "Settings") ||
            arguments[1].type != ZSHARP_C_TEXT || strcmp(arguments[1].text, "label") ||
            arguments[2].type != ZSHARP_C_TEXT ||
            arguments[3].type != ZSHARP_C_TEXT || strcmp(arguments[3].text, "nested.value") ||
            arguments[4].type != ZSHARP_C_NUMBER || arguments[4].number != 8) return 0;
        printf("CONFIGURATION BOUND\n"); result->type = ZSHARP_C_NULL; return 1;
    }
    if (strcmp(function, "counter") == 0 && argument_count == 0) {
        result->type = ZSHARP_C_NUMBER;
        result->number = ++counter;
        return 1;
    }
    if (strcmp(function, "identity") == 0 && argument_count == 1) {
        *result = arguments[0]; return 1;
    }
    if (strcmp(function, "client") == 0 && (argument_count == 0 ||
        (argument_count == 1 && arguments[0].type == ZSHARP_C_TEXT))) {
        result->type = ZSHARP_C_STATUS;
        result->number = argument_count == 0 || strcmp(arguments[0].text, "main") == 0;
        return 1;
    }
    if (strcmp(function, "greeting") == 0 && argument_count == 1 &&
        arguments[0].type == ZSHARP_C_TEXT) {
        snprintf(greeting, sizeof(greeting), "Hello from C, %s!",
                 arguments[0].text);
        result->type = ZSHARP_C_TEXT;
        result->text = greeting;
        return 1;
    }
    if (strcmp(function, "add") == 0 && argument_count == 2 &&
        arguments[0].type == ZSHARP_C_NUMBER &&
        arguments[1].type == ZSHARP_C_NUMBER) {
        result->type = ZSHARP_C_NUMBER;
        result->number = arguments[0].number + arguments[1].number;
        return 1;
    }
    if (strcmp(function, "move") == 0 && argument_count == 2 &&
        arguments[0].type == ZSHARP_C_TEXT &&
        arguments[1].type == ZSHARP_C_TEXT) {
        printf("Custom C syntax moved %s to %s\n", arguments[0].text,
               arguments[1].text);
        result->type = ZSHARP_C_NULL;
        return 1;
    }
    if (strcmp(function, "detections") == 0 && argument_count == 6 &&
        arguments[0].type == ZSHARP_C_TEXT &&
        arguments[1].type == ZSHARP_C_STATUS &&
        arguments[2].type == ZSHARP_C_TEXT &&
        arguments[3].type == ZSHARP_C_STATUS &&
        arguments[4].type == ZSHARP_C_TEXT &&
        arguments[5].type == ZSHARP_C_STATUS) {
        printf("Detections: %s=%d %s=%d %s=%d\n",
               arguments[0].text, (int)arguments[1].number,
               arguments[2].text, (int)arguments[3].number,
               arguments[4].text, (int)arguments[5].number);
        result->type = ZSHARP_C_NULL;
        return 1;
    }
    snprintf(error, error_size, "unknown function or arguments");
    return 0;
}

ZSHARP_C_EXPORT int zsharp_c_register_v1(ZSharpCSyntaxRegistry *registry,
                                          char *error, size_t error_size) {
    if (registry == NULL || registry->abi_version != ZSHARP_C_ABI_VERSION)
        return 0;
    return registry->add_statement(registry->context,
                                   "move {who} to {where}", "move",
                                   error, error_size) &&
           registry->add_statement(registry->context,
                                   "Movement.move({who}, {where})", "move",
                                   error, error_size) &&
           registry->add_block(registry->context, "Detections",
                               "detections", error, error_size) &&
           registry->add_expression(registry->context, "Discord.Client",
                                    "client", error, error_size) &&
           registry->add_expression(registry->context, "Discord.Client({name})",
                                    "client", error, error_size) &&
           registry->add_expression(registry->context, "NativeSum({a}, {b})",
                                    "add", error, error_size) &&
           registry->add_expression(registry->context, "NativeCounter()",
                                    "counter", error, error_size) &&
           registry->add_expression(registry->context, "NativeAdjacent {a} {b}",
                                    "add", error, error_size) &&
           registry->add_expression(registry->context, "NativeSeparated({a} + {b})",
                                    "add", error, error_size);
}

ZSHARP_C_EXPORT int zsharp_c_register_v2(ZSharpCSyntaxRegistryV2 *registry,
    char *error, size_t size) {
    return zsharp_c_register_v1(&registry->v1, error, size) &&
        registry->v1.add_expression(registry->v1.context, "NativeOverload({a})", "identity", error, size) &&
        registry->v1.add_expression(registry->v1.context, "NativeOverload({a}, {b})", "add", error, size) &&
        registry->add_declaration(registry->v1.context, "fixture_event", 1, "fixture_bind", error, size) &&
        registry->add_declaration(registry->v1.context, "Fixture.Option", ZSHARP_C_FUNCTION_OPTION, "fixture_option", error, size) &&
        registry->add_declaration(registry->v1.context, "Fixture.Duration", ZSHARP_C_FUNCTION_DURATION_OPTION, "fixture_duration", error, size) &&
        registry->add_declaration(registry->v1.context, "FixtureConfig", 2, "fixture_config", error, size);
}

ZSHARP_C_EXPORT int zsharp_c_service_v1(int operation, ZSharpCEventV1 *event,
    char *error, size_t size) {
    static ZSharpCValue argument = {ZSHARP_C_NUMBER, 7, NULL};
    (void)error; (void)size;
    if (operation == ZSHARP_C_SERVICE_ACTIVE) return event_pending;
    if (operation == ZSHARP_C_SERVICE_POLL) {
        if (!event_pending) return 0;
        event->id = 1; event->target = event_target; event->arguments = &argument;
        event->argument_count = 1; return 1;
    }
    if (operation == ZSHARP_C_SERVICE_COMPLETE || operation == ZSHARP_C_SERVICE_SHUTDOWN) {
        event_pending = 0; return 1;
    }
    return -1;
}
