#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "package_executable.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    char error[512] = {0};
#ifdef _WIN32
    assert(zsharp_package_enable_go_companion("missing.zgo", error, sizeof(error)));
    puts("Go package execute bits: Windows no-op passed");
#else
    char directory[] = "/tmp/zsharp-package-go-XXXXXX", path[512], other[512], link[512];
    struct stat status;
    FILE *file;
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/Module.zgo", directory);
    snprintf(other, sizeof(other), "%s/Other.txt", directory);
    snprintf(link, sizeof(link), "%s/Linked.zgo", directory);
    file = fopen(path, "wb"); assert(file); assert(fputs("owned test fixture\n", file) >= 0); assert(fclose(file) == 0);
    assert(chmod(path, 0600) == 0);
    assert(zsharp_package_enable_go_companion(path, error, sizeof(error)));
    assert(stat(path, &status) == 0 && (status.st_mode & 0777) == 0700);
    file = fopen(other, "wb"); assert(file); assert(fclose(file) == 0);
    assert(chmod(other, 0600) == 0);
    assert(zsharp_package_enable_go_companion(other, error, sizeof(error)));
    assert(stat(other, &status) == 0 && (status.st_mode & 0777) == 0600);
    assert(symlink(other, link) == 0);
    assert(!zsharp_package_enable_go_companion(link, error, sizeof(error)));
    assert(strstr(error, "regular") != NULL);
    assert(unlink(link) == 0 && unlink(other) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
    puts("Go package execute bits: owner-only, ordinary-file and symlink checks passed");
#endif
    return 0;
}
