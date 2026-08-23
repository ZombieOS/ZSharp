#ifndef ZSHARP_PROVIDER_H
#define ZSHARP_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#define ZSHARP_PROVIDER_ABI_VERSION 1u

#ifdef _WIN32
#define ZSHARP_PROVIDER_EXPORT __declspec(dllexport)
#else
#define ZSHARP_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif

typedef enum ZSharpProviderValueType {
    ZSHARP_PROVIDER_NUMBER = 1,
    ZSHARP_PROVIDER_TEXT = 2,
    ZSHARP_PROVIDER_STATUS = 3
} ZSharpProviderValueType;

typedef struct ZSharpProviderValue {
    ZSharpProviderValueType type;
    int32_t number;
    const char *text;
} ZSharpProviderValue;

typedef int (*ZSharpProviderGetVariableV1)(
    void *user_data, const char *file, const char *room,
    const char *variable, ZSharpProviderValue *value, char *error,
    size_t error_size);

typedef int (*ZSharpProviderCallFunctionV1)(
    void *user_data, const char *file, const char *room,
    const char *function, char *error, size_t error_size);

typedef int (*ZSharpProviderSetVariableV1)(
    void *user_data, const char *file, const char *room,
    const char *variable, const ZSharpProviderValue *value, char *error,
    size_t error_size);

typedef int (*ZSharpProviderSetMemberV1)(
    void *user_data, const char *file, const char *room,
    const char *object, const char *member,
    const ZSharpProviderValue *value, char *error, size_t error_size);

typedef int (*ZSharpProviderGetMemberV1)(
    void *user_data, const char *file, const char *room,
    const char *object, const char *member, ZSharpProviderValue *value,
    char *error, size_t error_size);

typedef int (*ZSharpProviderCallMethodV1)(
    void *user_data, const char *file, const char *room,
    const char *object, const char *method,
    const ZSharpProviderValue *arguments, size_t argument_count,
    char *error, size_t error_size);

typedef struct ZSharpProviderV1 {
    uint32_t abi_version;
    void *user_data;
    ZSharpProviderGetVariableV1 get_variable;
    ZSharpProviderCallFunctionV1 call_function;
    ZSharpProviderSetVariableV1 set_variable;
    ZSharpProviderGetMemberV1 get_member;
    ZSharpProviderSetMemberV1 set_member;
    ZSharpProviderCallMethodV1 call_method;
} ZSharpProviderV1;

typedef const ZSharpProviderV1 *(*ZSharpProviderEntryV1)(void);

typedef struct ZSharpProviderBinding {
    const char *project_name;
    const ZSharpProviderV1 *provider;
} ZSharpProviderBinding;

/* Every provider shared library exports a function with this name. */
#define ZSHARP_PROVIDER_ENTRY_NAME "zsharp_provider_v1"

#endif
