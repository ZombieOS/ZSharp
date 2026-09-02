#ifndef ZSHARP_UPDATER_H
#define ZSHARP_UPDATER_H

#include <stddef.h>

void zsharp_update_check_start(void);
/* Starts the bundled installer in visible, manual update mode. The caller
 * should exit after this succeeds so the installer can replace the ZVM. */
int zsharp_update_now(char *error, size_t error_size);
int zsharp_update_agent_register(char *error, size_t error_size);
int zsharp_update_agent_start(void);
int zsharp_update_agent_run(void);

#endif
