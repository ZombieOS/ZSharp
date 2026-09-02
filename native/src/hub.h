#ifndef ZSHARP_HUB_H
#define ZSHARP_HUB_H

#include <stddef.h>

int zsharp_hub_show(char *error, size_t error_size);
int zsharp_hub_add(const char *package_path, char *error, size_t error_size);
int zsharp_hub_remove(const char *project_id, char *error, size_t error_size);
int zsharp_hub_list(char *error, size_t error_size);

#endif
