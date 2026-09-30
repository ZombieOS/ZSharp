#ifndef ZSHARP_C_H
#define ZSHARP_C_H

#include <stddef.h>
#include <stdint.h>

#define ZSHARP_C_ABI_VERSION 1u
#define ZSHARP_C_ENTRY_NAME "zsharp_c_call_v1"

#ifdef _WIN32
#define ZSHARP_C_EXPORT __declspec(dllexport)
#else
#define ZSHARP_C_EXPORT __attribute__((visibility("default")))
#endif

typedef enum ZSharpCValueType {
    ZSHARP_C_NULL = 0,
    ZSHARP_C_NUMBER = 1,
    ZSHARP_C_TEXT = 2,
    ZSHARP_C_STATUS = 3
} ZSharpCValueType;

typedef struct ZSharpCValue {
    ZSharpCValueType type;
    double number;
    const char *text;
} ZSharpCValue;

/* Returned text must remain valid until the call returns. */
typedef int (*ZSharpCCallV1)(
    uint32_t abi_version, const char *function,
    const ZSharpCValue *arguments, size_t argument_count,
    ZSharpCValue *result, char *error, size_t error_size);

/* Optional compiler hook. Patterns consist of literal Z# tokens and
 * {placeholders}; each placeholder currently captures one expression atom.
 * Registration is rejected if it shadows a built-in statement or another
 * registered leading keyword. The compiler may call this function while
 * checking or packaging a project, so it must not have side effects. */
typedef struct ZSharpCSyntaxRegistry {
    uint32_t abi_version;
    void *context;
    int (*add_statement)(void *context, const char *pattern,
                         const char *function, char *error,
                         size_t error_size);
    /* Registers Name(key: value: ...) as a block. The handler receives
     * alternating key text and evaluated values in source order. */
    int (*add_block)(void *context, const char *name,
                     const char *function, char *error,
                     size_t error_size);
} ZSharpCSyntaxRegistry;

typedef int (*ZSharpCRegisterV1)(ZSharpCSyntaxRegistry *registry,
                                  char *error, size_t error_size);
#define ZSHARP_C_REGISTER_NAME "zsharp_c_register_v1"

#endif
