#include "zsharp_cpp.h"

#include <cstdio>
#include <cstring>

extern "C" ZSHARP_CPP_EXPORT int zsharp_cpp_call_v1(
    uint32_t abi_version, const char *function,
    const ZSharpCppValue *arguments, size_t argument_count,
    ZSharpCppValue *result, char *error, size_t error_size) {
    static char greeting[256];
    if (abi_version != ZSHARP_CPP_ABI_VERSION) {
        std::snprintf(error, error_size, "unsupported ABI");
        return 0;
    }
    if (std::strcmp(function, "greeting") == 0 && argument_count == 1 &&
        arguments[0].type == ZSHARP_CPP_TEXT) {
        std::snprintf(greeting, sizeof(greeting), "Hello from C++, %s!",
                      arguments[0].text);
        result->type = ZSHARP_CPP_TEXT;
        result->text = greeting;
        return 1;
    }
    if (std::strcmp(function, "add") == 0 && argument_count == 2) {
        result->type = ZSHARP_CPP_NUMBER;
        result->number = arguments[0].number + arguments[1].number;
        return 1;
    }
    if (std::strcmp(function, "ready") == 0 && argument_count == 0) {
        result->type = ZSHARP_CPP_STATUS;
        result->number = 1;
        return 1;
    }
    std::snprintf(error, error_size, "unknown function or arguments");
    return 0;
}
