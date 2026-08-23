#ifndef ZSHARP_PROVIDER_LOADER_H
#define ZSHARP_PROVIDER_LOADER_H

#include "zsharp_provider.h"

#include <stddef.h>

typedef struct ZSharpLoadedProvider {
    ZSharpProviderBinding binding;
    void *library;
} ZSharpLoadedProvider;

int zsharp_provider_load(const char *specification,
                         ZSharpLoadedProvider *loaded, char *error,
                         size_t error_size);
void zsharp_provider_unload(ZSharpLoadedProvider *loaded);

#endif
