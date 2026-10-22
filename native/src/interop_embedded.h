#ifndef ZSHARP_INTEROP_EMBEDDED_H
#define ZSHARP_INTEROP_EMBEDDED_H

#include <stddef.h>

typedef struct ZSharpEmbeddedInterop ZSharpEmbeddedInterop;

/* The dispatcher returns one malloc-owned wire JSON value in result_json.
 * It runs synchronously on the owning ZVM task thread, never a module worker.
 */
typedef int (*ZSharpInteropHostDispatch)(
    void *context, const char *operation, const char *target,
    const char *member, const char *handle, const char *arguments_json,
    char **result_json, char *error, size_t error_size);

/* 1: success with malloc-owned result_json; 0: error; -1: no v2 entry, so the
 * caller may use the existing v1 bridge. No failed operation is ever replayed.
 * Cache cleanup requires all calls to have ended. Native runtimes stay loaded
 * until process exit, as background native threads may still execute code.
 */
int zsharp_embedded_interop_request(
    ZSharpEmbeddedInterop **cache, const char *language, const char *module_path,
    const char *operation, const char *target, const char *member,
    const char *handle, const char *arguments_json,
    ZSharpInteropHostDispatch host_dispatch, void *host_context,
    char **result_json, char *error, size_t error_size);

void zsharp_embedded_interop_free(ZSharpEmbeddedInterop *cache);

#endif
