#include "zsharp_c.h"

#include <stdio.h>
#include <string.h>

ZSHARP_C_EXPORT int zsharp_c_call_v1(
    uint32_t abi_version, const char *function,
    const ZSharpCValue *arguments, size_t argument_count,
    ZSharpCValue *result, char *error, size_t error_size) {
    static char greeting[256];
    if (abi_version != ZSHARP_C_ABI_VERSION) {
        snprintf(error, error_size, "unsupported ABI");
        return 0;
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
                               "detections", error, error_size);
}
