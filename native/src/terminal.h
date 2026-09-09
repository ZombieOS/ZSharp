#ifndef ZSHARP_TERMINAL_H
#define ZSHARP_TERMINAL_H

#include <stddef.h>

int zsharp_terminal_start(const char *project_id, char *error,
                          size_t error_size);
void zsharp_terminal_write(const char *text);
void zsharp_terminal_stop(void);
int zsharp_terminal_attach(const char *project_id, char *error,
                           size_t error_size);

#endif
