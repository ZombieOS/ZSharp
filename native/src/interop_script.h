#ifndef ZSHARP_INTEROP_SCRIPT_H
#define ZSHARP_INTEROP_SCRIPT_H

#include "interop_embedded.h"

typedef struct ZSharpScriptInterop ZSharpScriptInterop;

/* Persistent JS/Lua module instances. result_json is malloc-owned on success.
 * No operation is replayed after failure. Call free only after all tasks end. */
int zsharp_script_interop_request(
    ZSharpScriptInterop **cache, const char *language, const char *module_path,
    const char *operation, const char *target, const char *member,
    const char *handle, const char *arguments_json,
    ZSharpInteropHostDispatch host_dispatch, void *host_context,
    char **result_json, char *error, size_t error_size);

void zsharp_script_interop_free(ZSharpScriptInterop *cache);

#endif
