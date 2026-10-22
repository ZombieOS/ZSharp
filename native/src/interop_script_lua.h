#ifndef ZSHARP_INTEROP_SCRIPT_LUA_H
#define ZSHARP_INTEROP_SCRIPT_LUA_H

#include "interop_embedded.h"

typedef struct ZSharpLuaInterop ZSharpLuaInterop;

int zsharp_lua_interop_request(
    ZSharpLuaInterop **cache, const char *module_path, const char *operation,
    const char *target, const char *member, const char *handle,
    const char *arguments_json, ZSharpInteropHostDispatch host,
    void *host_context, char **result_json, char *error, size_t error_size);

void zsharp_lua_interop_free(ZSharpLuaInterop *cache);

#endif
