#ifndef ZSHARP_DECIMAL_H
#define ZSHARP_DECIMAL_H

#include <stddef.h>
#include <stdint.h>

int zsharp_decimal_normalize(const char *input, char **output,
                             char *error, size_t error_size);
char *zsharp_decimal_add(const char *left, const char *right,
                         char *error, size_t error_size);
char *zsharp_decimal_subtract(const char *left, const char *right,
                              char *error, size_t error_size);
char *zsharp_decimal_multiply(const char *left, const char *right,
                              char *error, size_t error_size);
char *zsharp_decimal_divide(const char *left, const char *right,
                            char *error, size_t error_size);
char *zsharp_decimal_remainder(const char *left, const char *right,
                               char *error, size_t error_size);
char *zsharp_decimal_negate(const char *value,
                            char *error, size_t error_size);
int zsharp_decimal_compare(const char *left, const char *right);
int zsharp_decimal_is_zero(const char *value);
int zsharp_decimal_to_size(const char *value, size_t *output);
int zsharp_decimal_to_int32(const char *value, int32_t *output);

#endif
