#ifndef ZSHARP_CPP_H
#define ZSHARP_CPP_H

#include <stddef.h>
#include <stdint.h>

#define ZSHARP_CPP_ABI_VERSION 1u

#ifdef _WIN32
#define ZSHARP_CPP_EXPORT __declspec(dllexport)
#else
#define ZSHARP_CPP_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ZSharpCppValueType {
    ZSHARP_CPP_NULL = 0,
    ZSHARP_CPP_NUMBER = 1,
    ZSHARP_CPP_TEXT = 2,
    ZSHARP_CPP_STATUS = 3
} ZSharpCppValueType;

typedef struct ZSharpCppValue {
    ZSharpCppValueType type;
    double number;
    const char *text;
} ZSharpCppValue;

/* Text returned here only needs to remain valid until this function returns. */
typedef int (*ZSharpCppCallV1)(
    uint32_t abi_version, const char *function,
    const ZSharpCppValue *arguments, size_t argument_count,
    ZSharpCppValue *result, char *error, size_t error_size);

#define ZSHARP_CPP_ENTRY_NAME "zsharp_cpp_call_v1"

#ifdef __cplusplus
}
#endif

#endif
