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

#endif
