#include "provider_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static char *copy_text(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void close_library(void *library) {
    if (library == NULL) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)library);
#else
    dlclose(library);
#endif
}

int zsharp_provider_load(const char *specification,
                         ZSharpLoadedProvider *loaded, char *error,
                         size_t error_size) {
    const char *equals = strchr(specification, '=');
    char *project_name;
    const char *library_path;
    void *library;
    ZSharpProviderEntryV1 entry;
    const ZSharpProviderV1 *provider;
    memset(loaded, 0, sizeof(*loaded));
    if (equals == NULL || equals == specification || equals[1] == '\0') {
        snprintf(error, error_size,
                 "provider must use Project=path-to-library");
        return 0;
    }
    project_name = copy_text(specification,
                             (size_t)(equals - specification));
    if (project_name == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    library_path = equals + 1;
#ifdef _WIN32
    library = (void *)LoadLibraryA(library_path);
#else
    library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (library == NULL) {
#ifdef _WIN32
        snprintf(error, error_size,
                 "could not load provider library '%s' (Windows error %lu)",
                 library_path, (unsigned long)GetLastError());
#else
        snprintf(error, error_size, "could not load provider library '%s': %s",
                 library_path, dlerror());
#endif
        free(project_name);
        return 0;
    }
#ifdef _WIN32
    entry = (ZSharpProviderEntryV1)(void *)GetProcAddress(
        (HMODULE)library, ZSHARP_PROVIDER_ENTRY_NAME);
#else
    entry = (ZSharpProviderEntryV1)dlsym(library,
                                         ZSHARP_PROVIDER_ENTRY_NAME);
#endif
    if (entry == NULL) {
        snprintf(error, error_size,
                 "provider library '%s' does not export %s", library_path,
                 ZSHARP_PROVIDER_ENTRY_NAME);
        close_library(library);
        free(project_name);
        return 0;
    }
    provider = entry();
    if (provider == NULL ||
        provider->abi_version != ZSHARP_PROVIDER_ABI_VERSION) {
        snprintf(error, error_size,
                 "provider library '%s' does not implement Z# provider ABI %u",
                 library_path, ZSHARP_PROVIDER_ABI_VERSION);
        close_library(library);
        free(project_name);
        return 0;
    }
    loaded->binding.project_name = project_name;
    loaded->binding.provider = provider;
    loaded->library = library;
    return 1;
}

void zsharp_provider_unload(ZSharpLoadedProvider *loaded) {
    if (loaded == NULL) return;
    close_library(loaded->library);
    free((char *)loaded->binding.project_name);
    memset(loaded, 0, sizeof(*loaded));
}
