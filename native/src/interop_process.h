#ifndef ZSHARP_INTEROP_PROCESS_H
#define ZSHARP_INTEROP_PROCESS_H
#include <stddef.h>
typedef struct ZSharpInteropProcess ZSharpInteropProcess;
typedef int (*ZSharpInteropCancelled)(void *context);
/* Private owned workers only; argv is passed without a shell. Frames are UTF-8
 * JSON lines, limited to 1 MiB. Failed requests are never replayed. */
ZSharpInteropProcess *zsharp_interop_process_start(const char *const *argv,
    const char *directory, char *error, size_t error_size);
int zsharp_interop_process_write(ZSharpInteropProcess *process, const char *line,
    unsigned timeout_ms, ZSharpInteropCancelled cancelled, void *context,
    char *error, size_t error_size);
char *zsharp_interop_process_read(ZSharpInteropProcess *process,
    unsigned timeout_ms, ZSharpInteropCancelled cancelled, void *context,
    char *error, size_t error_size);
void zsharp_interop_process_destroy(ZSharpInteropProcess *process);
#endif
