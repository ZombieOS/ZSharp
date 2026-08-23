#include "decimal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DecimalParts {
    int negative;
    char *digits;
    size_t scale;
} DecimalParts;

static char *copy_range(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *big_trim_owned(char *digits) {
    size_t skip = 0;
    size_t length;
    while (digits[skip] == '0' && digits[skip + 1] != '\0') skip++;
    if (skip == 0) return digits;
    length = strlen(digits + skip);
    memmove(digits, digits + skip, length + 1);
    return digits;
}

static int big_compare(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    int compared;
    if (left_length != right_length) return left_length < right_length ? -1 : 1;
    compared = strcmp(left, right);
    return compared < 0 ? -1 : compared > 0 ? 1 : 0;
}

static char *big_add(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t length = (left_length > right_length ? left_length : right_length) + 1;
    char *result = (char *)malloc(length + 1);
    int carry = 0;
    if (result == NULL) return NULL;
    result[length] = '\0';
    while (length > 0) {
        int digit = carry;
        if (left_length > 0) digit += left[--left_length] - '0';
        if (right_length > 0) digit += right[--right_length] - '0';
        result[--length] = (char)('0' + digit % 10);
        carry = digit / 10;
    }
    return big_trim_owned(result);
}

static char *big_subtract(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t output_length = left_length;
    char *result = (char *)malloc(output_length + 1);
    int borrow = 0;
    if (result == NULL) return NULL;
    result[output_length] = '\0';
    while (left_length > 0) {
        int digit = left[--left_length] - '0' - borrow;
        int other = right_length > 0 ? right[--right_length] - '0' : 0;
        if (digit < other) {
            digit += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result[left_length] = (char)('0' + digit - other);
    }
    return big_trim_owned(result);
}

static char *big_multiply(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t length = left_length + right_length;
    unsigned int *work;
    char *result;
    size_t left_index;
    size_t right_index;
    size_t index;
    if (strcmp(left, "0") == 0 || strcmp(right, "0") == 0) {
        return copy_range("0", 1);
    }
    work = (unsigned int *)calloc(length, sizeof(*work));
    result = (char *)malloc(length + 1);
    if (work == NULL || result == NULL) {
        free(work);
        free(result);
        return NULL;
    }
    for (left_index = left_length; left_index > 0; left_index--) {
        for (right_index = right_length; right_index > 0; right_index--) {
            size_t position = left_index + right_index - 1;
            work[position] += (unsigned int)(left[left_index - 1] - '0') *
                              (unsigned int)(right[right_index - 1] - '0');
        }
    }
    for (index = length; index > 1; index--) {
        work[index - 2] += work[index - 1] / 10;
        work[index - 1] %= 10;
    }
    for (index = 0; index < length; index++) {
        result[index] = (char)('0' + work[index]);
    }
    result[length] = '\0';
    free(work);
    return big_trim_owned(result);
}

static char *big_append_zeros(const char *digits, size_t count) {
    size_t length = strlen(digits);
    char *result;
    if (strcmp(digits, "0") == 0) return copy_range("0", 1);
    if (count > SIZE_MAX - length - 1) return NULL;
    result = (char *)malloc(length + count + 1);
    if (result == NULL) return NULL;
    memcpy(result, digits, length);
    memset(result + length, '0', count);
    result[length + count] = '\0';
    return result;
}

static int big_divmod(const char *numerator, const char *denominator,
                      char **quotient_output, char **remainder_output) {
    size_t numerator_length = strlen(numerator);
    char *quotient = (char *)malloc(numerator_length + 1);
    char *remainder = copy_range("0", 1);
    size_t index;
    if (quotient == NULL || remainder == NULL ||
        strcmp(denominator, "0") == 0) {
        free(quotient);
        free(remainder);
        return 0;
    }
    for (index = 0; index < numerator_length; index++) {
        size_t remainder_length = strlen(remainder);
        char *extended = (char *)malloc(remainder_length + 2);
        unsigned int digit = 0;
        char *next;
        if (extended == NULL) {
            free(quotient);
            free(remainder);
            return 0;
        }
        memcpy(extended, remainder, remainder_length);
        extended[remainder_length] = numerator[index];
        extended[remainder_length + 1] = '\0';
        free(remainder);
        remainder = big_trim_owned(extended);
        while (big_compare(remainder, denominator) >= 0) {
            next = big_subtract(remainder, denominator);
            free(remainder);
            if (next == NULL) {
                free(quotient);
                return 0;
            }
            remainder = next;
            digit++;
        }
        quotient[index] = (char)('0' + digit);
    }
    quotient[numerator_length] = '\0';
    *quotient_output = big_trim_owned(quotient);
    *remainder_output = remainder;
    return 1;
}

static char *big_gcd(const char *left, const char *right) {
    char *a = copy_range(left, strlen(left));
    char *b = copy_range(right, strlen(right));
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return NULL;
    }
    while (strcmp(b, "0") != 0) {
        char *quotient;
        char *remainder;
        if (!big_divmod(a, b, &quotient, &remainder)) {
            free(a);
            free(b);
            return NULL;
        }
        free(quotient);
        free(a);
        a = b;
        b = remainder;
    }
    free(b);
    return a;
}

static int big_divide_small(const char *digits, unsigned int divisor,
                            char **quotient_output,
                            unsigned int *remainder_output) {
    size_t length = strlen(digits);
    char *quotient = (char *)malloc(length + 1);
    unsigned int remainder = 0;
    size_t index;
    if (quotient == NULL) return 0;
    for (index = 0; index < length; index++) {
        unsigned int value = remainder * 10u +
                             (unsigned int)(digits[index] - '0');
        quotient[index] = (char)('0' + value / divisor);
        remainder = value % divisor;
    }
    quotient[length] = '\0';
    *quotient_output = big_trim_owned(quotient);
    *remainder_output = remainder;
    return 1;
}

static void parts_free(DecimalParts *parts) {
    free(parts->digits);
    memset(parts, 0, sizeof(*parts));
}

static int parts_parse(const char *value, DecimalParts *parts) {
    const char *cursor = value;
    const char *dot;
    size_t length;
    size_t output = 0;
    memset(parts, 0, sizeof(*parts));
    if (*cursor == '-') {
        parts->negative = 1;
        cursor++;
    }
    dot = strchr(cursor, '.');
    length = strlen(cursor);
    parts->scale = dot == NULL ? 0 : strlen(dot + 1);
    parts->digits = (char *)malloc(length + 1);
    if (parts->digits == NULL) return 0;
    while (*cursor != '\0') {
        if (*cursor != '.') parts->digits[output++] = *cursor;
        cursor++;
    }
    parts->digits[output] = '\0';
    big_trim_owned(parts->digits);
    if (strcmp(parts->digits, "0") == 0) parts->negative = 0;
    return 1;
}

int zsharp_decimal_normalize(const char *input, char **output,
                             char *error, size_t error_size) {
    const char *cursor = input;
    const char *integer_start;
    const char *fraction_start = NULL;
    const char *integer_end;
    const char *end;
    size_t integer_length;
    size_t fraction_length = 0;
    int negative = 0;
    char *result;
    char *write;
    if (*cursor == '-') {
        negative = 1;
        cursor++;
    }
    integer_start = cursor;
    while (isdigit((unsigned char)*cursor)) cursor++;
    integer_end = cursor;
    if (cursor == integer_start) goto invalid;
    if (*cursor == '.') {
        cursor++;
        fraction_start = cursor;
        while (isdigit((unsigned char)*cursor)) cursor++;
        if (cursor == fraction_start) goto invalid;
    }
    if (*cursor != '\0') goto invalid;
    end = cursor;
    while (integer_start + 1 < integer_end && *integer_start == '0') {
        integer_start++;
    }
    if (fraction_start != NULL) {
        while (end > fraction_start && end[-1] == '0') end--;
        fraction_length = (size_t)(end - fraction_start);
    }
    integer_length = (size_t)(integer_end - integer_start);
    if (integer_length == 1 && integer_start[0] == '0' &&
        fraction_length == 0) negative = 0;
    result = (char *)malloc((size_t)negative + integer_length +
                            (fraction_length > 0 ? fraction_length + 1 : 0) +
                            1);
    if (result == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    write = result;
    if (negative) *write++ = '-';
    memcpy(write, integer_start, integer_length);
    write += integer_length;
    if (fraction_length > 0) {
        *write++ = '.';
        memcpy(write, fraction_start, fraction_length);
        write += fraction_length;
    }
    *write = '\0';
    *output = result;
    return 1;

invalid:
    snprintf(error, error_size,
             "invalid number '%s'; use ordinary decimal notation", input);
    return 0;
}

static char *decimal_format(const char *digits, size_t scale, int negative,
                            char *error, size_t error_size) {
    size_t length = strlen(digits);
    size_t zero_count = scale >= length ? scale - length + 1 : 0;
    size_t raw_length = (size_t)negative + length + zero_count +
                        (scale > 0 ? 1 : 0);
    char *raw = (char *)malloc(raw_length + 1);
    char *write;
    char *normalized = NULL;
    if (raw == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    write = raw;
    if (negative && strcmp(digits, "0") != 0) *write++ = '-';
    if (scale == 0) {
        memcpy(write, digits, length + 1);
    } else if (zero_count > 0) {
        *write++ = '0';
        *write++ = '.';
        if (zero_count > 1) {
            memset(write, '0', zero_count - 1);
            write += zero_count - 1;
        }
        memcpy(write, digits, length + 1);
    } else {
        size_t integer_length = length - scale;
        memcpy(write, digits, integer_length);
        write += integer_length;
        *write++ = '.';
        memcpy(write, digits + integer_length, scale + 1);
    }
    if (!zsharp_decimal_normalize(raw, &normalized, error, error_size)) {
        free(raw);
        return NULL;
    }
    free(raw);
    return normalized;
}

static char *decimal_add_parts(const DecimalParts *left,
                               const DecimalParts *right,
                               char *error, size_t error_size) {
    size_t scale = left->scale > right->scale ? left->scale : right->scale;
    char *left_digits = big_append_zeros(left->digits, scale - left->scale);
    char *right_digits = big_append_zeros(right->digits, scale - right->scale);
    char *digits = NULL;
    int negative = 0;
    if (left_digits == NULL || right_digits == NULL) goto memory_error;
    if (left->negative == right->negative) {
        digits = big_add(left_digits, right_digits);
        negative = left->negative;
    } else {
        int comparison = big_compare(left_digits, right_digits);
        if (comparison >= 0) {
            digits = big_subtract(left_digits, right_digits);
            negative = left->negative;
        } else {
            digits = big_subtract(right_digits, left_digits);
            negative = right->negative;
        }
    }
    free(left_digits);
    free(right_digits);
    if (digits == NULL) goto memory_error_without_inputs;
    {
        char *result = decimal_format(digits, scale, negative, error,
                                      error_size);
        free(digits);
        return result;
    }

memory_error:
    free(left_digits);
    free(right_digits);
memory_error_without_inputs:
    snprintf(error, error_size, "out of memory");
    return NULL;
}

char *zsharp_decimal_add(const char *left, const char *right,
                         char *error, size_t error_size) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    char *result;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        parts_free(&left_parts);
        parts_free(&right_parts);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    result = decimal_add_parts(&left_parts, &right_parts, error, error_size);
    parts_free(&left_parts);
    parts_free(&right_parts);
    return result;
}

char *zsharp_decimal_subtract(const char *left, const char *right,
                              char *error, size_t error_size) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    char *result;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        parts_free(&left_parts);
        parts_free(&right_parts);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    right_parts.negative = !right_parts.negative;
    if (strcmp(right_parts.digits, "0") == 0) right_parts.negative = 0;
    result = decimal_add_parts(&left_parts, &right_parts, error, error_size);
    parts_free(&left_parts);
    parts_free(&right_parts);
    return result;
}

char *zsharp_decimal_multiply(const char *left, const char *right,
                              char *error, size_t error_size) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    char *digits;
    char *result;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        parts_free(&left_parts);
        parts_free(&right_parts);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    digits = big_multiply(left_parts.digits, right_parts.digits);
    if (digits == NULL) {
        snprintf(error, error_size, "out of memory");
        result = NULL;
    } else {
        result = decimal_format(digits, left_parts.scale + right_parts.scale,
                                left_parts.negative != right_parts.negative,
                                error, error_size);
    }
    free(digits);
    parts_free(&left_parts);
    parts_free(&right_parts);
    return result;
}

static int reduced_fraction(const DecimalParts *left,
                            const DecimalParts *right,
                            char **numerator_output,
                            char **denominator_output,
                            char *error, size_t error_size) {
    char *numerator = big_append_zeros(left->digits, right->scale);
    char *denominator = big_append_zeros(right->digits, left->scale);
    char *gcd;
    char *quotient;
    char *remainder;
    if (numerator == NULL || denominator == NULL) goto memory_error;
    gcd = big_gcd(numerator, denominator);
    if (gcd == NULL) goto memory_error;
    if (!big_divmod(numerator, gcd, &quotient, &remainder)) {
        free(gcd);
        goto memory_error;
    }
    free(numerator);
    numerator = quotient;
    free(remainder);
    if (!big_divmod(denominator, gcd, &quotient, &remainder)) {
        free(gcd);
        goto memory_error;
    }
    free(denominator);
    denominator = quotient;
    free(remainder);
    free(gcd);
    *numerator_output = numerator;
    *denominator_output = denominator;
    return 1;

memory_error:
    free(numerator);
    free(denominator);
    snprintf(error, error_size, "out of memory");
    return 0;
}

char *zsharp_decimal_divide(const char *left, const char *right,
                            char *error, size_t error_size) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    char *numerator = NULL;
    char *denominator = NULL;
    char *factor_test = NULL;
    char *next;
    char *scaled = NULL;
    char *quotient = NULL;
    char *remainder = NULL;
    char *result = NULL;
    size_t twos = 0;
    size_t fives = 0;
    size_t scale;
    unsigned int small_remainder;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    if (strcmp(right_parts.digits, "0") == 0) {
        snprintf(error, error_size, "cannot divide by zero");
        goto done;
    }
    if (!reduced_fraction(&left_parts, &right_parts, &numerator,
                          &denominator, error, error_size)) goto done;
    factor_test = copy_range(denominator, strlen(denominator));
    if (factor_test == NULL) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    for (;;) {
        if (!big_divide_small(factor_test, 2, &next, &small_remainder)) {
            snprintf(error, error_size, "out of memory");
            goto done;
        }
        if (small_remainder != 0) {
            free(next);
            break;
        }
        free(factor_test);
        factor_test = next;
        twos++;
    }
    for (;;) {
        if (!big_divide_small(factor_test, 5, &next, &small_remainder)) {
            snprintf(error, error_size, "out of memory");
            goto done;
        }
        if (small_remainder != 0) {
            free(next);
            break;
        }
        free(factor_test);
        factor_test = next;
        fives++;
    }
    scale = strcmp(factor_test, "1") == 0
        ? (twos > fives ? twos : fives)
        : 1;
    scaled = big_append_zeros(numerator, scale);
    if (scaled == NULL ||
        !big_divmod(scaled, denominator, &quotient, &remainder)) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    result = decimal_format(quotient, scale,
                            left_parts.negative != right_parts.negative,
                            error, error_size);

done:
    parts_free(&left_parts);
    parts_free(&right_parts);
    free(numerator);
    free(denominator);
    free(factor_test);
    free(scaled);
    free(quotient);
    free(remainder);
    return result;
}

char *zsharp_decimal_remainder(const char *left, const char *right,
                               char *error, size_t error_size) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    size_t scale;
    char *left_digits = NULL;
    char *right_digits = NULL;
    char *quotient = NULL;
    char *remainder = NULL;
    char *result = NULL;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    if (strcmp(right_parts.digits, "0") == 0) {
        snprintf(error, error_size, "cannot divide by zero");
        goto done;
    }
    scale = left_parts.scale > right_parts.scale
        ? left_parts.scale : right_parts.scale;
    left_digits = big_append_zeros(left_parts.digits,
                                   scale - left_parts.scale);
    right_digits = big_append_zeros(right_parts.digits,
                                    scale - right_parts.scale);
    if (left_digits == NULL || right_digits == NULL ||
        !big_divmod(left_digits, right_digits, &quotient, &remainder)) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    result = decimal_format(remainder, scale, left_parts.negative,
                            error, error_size);

done:
    parts_free(&left_parts);
    parts_free(&right_parts);
    free(left_digits);
    free(right_digits);
    free(quotient);
    free(remainder);
    return result;
}

char *zsharp_decimal_negate(const char *value,
                            char *error, size_t error_size) {
    char *result;
    size_t length = strlen(value);
    (void)error;
    (void)error_size;
    if (zsharp_decimal_is_zero(value)) return copy_range("0", 1);
    if (value[0] == '-') return copy_range(value + 1, length - 1);
    result = (char *)malloc(length + 2);
    if (result == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    result[0] = '-';
    memcpy(result + 1, value, length + 1);
    return result;
}

int zsharp_decimal_compare(const char *left, const char *right) {
    DecimalParts left_parts = {0};
    DecimalParts right_parts = {0};
    size_t scale;
    char *left_digits;
    char *right_digits;
    int compared;
    if (!parts_parse(left, &left_parts) || !parts_parse(right, &right_parts)) {
        parts_free(&left_parts);
        parts_free(&right_parts);
        return 0;
    }
    if (left_parts.negative != right_parts.negative) {
        compared = left_parts.negative ? -1 : 1;
        parts_free(&left_parts);
        parts_free(&right_parts);
        return compared;
    }
    scale = left_parts.scale > right_parts.scale
        ? left_parts.scale : right_parts.scale;
    left_digits = big_append_zeros(left_parts.digits,
                                   scale - left_parts.scale);
    right_digits = big_append_zeros(right_parts.digits,
                                    scale - right_parts.scale);
    compared = left_digits == NULL || right_digits == NULL
        ? 0 : big_compare(left_digits, right_digits);
    if (left_parts.negative) compared = -compared;
    free(left_digits);
    free(right_digits);
    parts_free(&left_parts);
    parts_free(&right_parts);
    return compared;
}

int zsharp_decimal_is_zero(const char *value) {
    return strcmp(value, "0") == 0;
}

int zsharp_decimal_to_size(const char *value, size_t *output) {
    size_t result = 0;
    const char *cursor = value;
    if (*cursor == '-' || strchr(cursor, '.') != NULL || *cursor == '\0') {
        return 0;
    }
    while (*cursor != '\0') {
        unsigned int digit = (unsigned int)(*cursor++ - '0');
        if (result > (SIZE_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *output = result;
    return 1;
}

int zsharp_decimal_to_int32(const char *value, int32_t *output) {
    char *end;
    long long parsed;
    if (strchr(value, '.') != NULL) return 0;
    parsed = strtoll(value, &end, 10);
    if (*end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) return 0;
    *output = (int32_t)parsed;
    return 1;
}
