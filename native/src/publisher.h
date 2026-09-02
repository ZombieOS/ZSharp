#ifndef ZSHARP_PUBLISHER_H
#define ZSHARP_PUBLISHER_H

#include <stddef.h>

/* Runs the repository's local release builder. This never uploads, pushes,
 * tags, or publishes anything to a remote service. */
int zsharp_publisher_run(const char *repository, char *error,
                         size_t error_size);

#endif
