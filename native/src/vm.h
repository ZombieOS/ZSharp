#ifndef ZSHARP_VM_H
#define ZSHARP_VM_H

#include "bytecode.h"
#include "zsharp_provider.h"

#include <stddef.h>

int zsharp_vm_run(ZSharpProgram *program, const char *project_root,
                  char *error, size_t error_size);

int zsharp_vm_run_with_providers(
    ZSharpProgram *program, const char *project_root,
    const ZSharpProviderBinding *providers, size_t provider_count,
    char *error, size_t error_size);

/* Invoke one noticed brain/function in a noticed room, without automatic
 * Start/Shutdown or a window/game event loop. Room initializers still run.
 * args_json is a tagged interop JSON array; result_json receives one allocated
 * tagged JSON value (free() it). Live object handles cannot cross this one-shot
 * boundary. The caller owns program and must not run it concurrently. */
int zsharp_vm_call_with_providers(
    ZSharpProgram *program, const char *project_root,
    const ZSharpProviderBinding *providers, size_t provider_count,
    const char *room_name, const char *function_name, const char *args_json,
    char **result_json, char *error, size_t error_size);

#endif
