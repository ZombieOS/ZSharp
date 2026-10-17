#ifndef ZSHARP_TIME_API_H
#define ZSHARP_TIME_API_H

#include <stddef.h>
#include <stdint.h>

enum {
    ZTIME_SINCE = 1, ZTIME_UNTIL, ZTIME_CONVERT,
    ZTIME_YEAR, ZTIME_MONTH, ZTIME_DAY, ZTIME_WEEK,
    ZTIME_HOUR, ZTIME_MINUTE, ZTIME_SECOND, ZTIME_MS,
    ZTIME_HOUR_FORMATTED
};

/* Local fields: year, month (1..12), day, hour, minute, second,
 * weekday (Sunday=0). Clock/conversion timestamps are Unix milliseconds. */
typedef struct ZSharpTimeHost {
    int (*now)(int64_t *milliseconds);
    int (*local)(int64_t milliseconds, int fields[7]);
    int (*convert_local)(const int fields[7], int64_t *milliseconds);
} ZSharpTimeHost;

int zsharp_time_evaluate(int operation, const char *const *arguments,
                        size_t count, const ZSharpTimeHost *host,
                        char *output, size_t output_size, int *is_text,
                        char *error, size_t error_size);

#endif
