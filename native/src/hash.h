#ifndef ZSHARP_HASH_H
#define ZSHARP_HASH_H

#include <stddef.h>

#define ZSHARP_SHA256_SIZE 32

void zsharp_project_identity(const char *project_id,
                             unsigned char output[ZSHARP_SHA256_SIZE]);
int zsharp_sha256_file_except(const char *path, long skip_offset,
                              size_t skip_length,
                              unsigned char output[ZSHARP_SHA256_SIZE]);
void zsharp_hash_hex(const unsigned char hash[ZSHARP_SHA256_SIZE],
                     char output[ZSHARP_SHA256_SIZE * 2 + 1]);

#endif
