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
 * {placeholders}; placeholders capture expressions, including nested calls.
 * Registration is rejected if it shadows a built-in statement or another
 * overlapping registered pattern. Distinct patterns may share a namespace.
 * The compiler may call this function while
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
    /* Z# 1.2.1+: registers a value-producing expression (no trailing ':').
     * The ordinary C call handler returns NUMBER, TEXT, or STATUS. */
    int (*add_expression)(void *context, const char *pattern,
                          const char *function, char *error,
                          size_t error_size);
} ZSharpCSyntaxRegistry;

typedef int (*ZSharpCRegisterV1)(ZSharpCSyntaxRegistry *registry,
                                  char *error, size_t error_size);
#define ZSHARP_C_REGISTER_NAME "zsharp_c_register_v1"

/* Optional versioned extension: old v1 modules and registries are unchanged.
 * Function declarations use normal Z# parameters/bodies. Their binder receives
 * name, File:Room:Function, then alternating parameter name/type texts.
 * Named configuration declarations bind name followed by dotted field/value
 * pairs, evaluated once before Start. Neither hook runs at compile time.
 * FUNCTION_OPTION registers a qualified name accepting one or more literal
 * texts at the beginning of a custom function body. Its binder receives
 * File:Room:Function followed by the texts, after the declaration binder.
 * FUNCTION_DURATION_OPTION accepts one wait-style duration literal and passes
 * File:Room:Function followed by a NUMBER in milliseconds to the binder. */
typedef struct ZSharpCSyntaxRegistryV2 {
    ZSharpCSyntaxRegistry v1;
    int (*add_declaration)(void *context, const char *name, int kind,
                          const char *binder, char *error, size_t error_size);
} ZSharpCSyntaxRegistryV2;
#define ZSHARP_C_FUNCTION_DECLARATION 1
#define ZSHARP_C_NAMED_BLOCK_DECLARATION 2
#define ZSHARP_C_FUNCTION_OPTION 3
#define ZSHARP_C_FUNCTION_DURATION_OPTION 4
#define ZSHARP_C_REGISTER_V2_NAME "zsharp_c_register_v2"
typedef int (*ZSharpCRegisterV2)(ZSharpCSyntaxRegistryV2 *, char *, size_t);

/* Optional asynchronous service. Called only on a ZVM task thread, never on
 * the service's worker. ACTIVE/POLL return 0 when inactive/empty, 1 when
 * active/available, and -1 on failure. COMPLETE/SHUTDOWN return success (1)
 * or failure (0). POLL receives the owning source name in event->target;
 * supply only callbacks registered by that owner. COMPLETE's error buffer
 * contains a callback error, or an empty string on success.
 * poll supplies borrowed values valid until complete.
 * The target must be a registered function in the owning Z# source file. */
typedef struct ZSharpCEventV1 {
    uint64_t id;
    const char *target;
    const ZSharpCValue *arguments;
    size_t argument_count;
} ZSharpCEventV1;
typedef int (*ZSharpCServiceV1)(int operation, ZSharpCEventV1 *event,
                              char *error, size_t error_size);
#define ZSHARP_C_SERVICE_NAME "zsharp_c_service_v1"
#define ZSHARP_C_SERVICE_ACTIVE 0
#define ZSHARP_C_SERVICE_POLL 1
#define ZSHARP_C_SERVICE_COMPLETE 2
#define ZSHARP_C_SERVICE_SHUTDOWN 3

#endif
