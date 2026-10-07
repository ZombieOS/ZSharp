#ifndef ZSHARP_PACKAGE_IGNORE_H
#define ZSHARP_PACKAGE_IGNORE_H
#include <stddef.h>
typedef struct ZSharpIgnoreRule {
    char *pattern;
    int negate, directory, rooted;
} ZSharpIgnoreRule;
typedef struct ZSharpPackageIgnore {
    ZSharpIgnoreRule *rules;
    size_t count;
} ZSharpPackageIgnore;
int zsharp_package_ignore_load(ZSharpPackageIgnore *, const char *root, char *, size_t);
void zsharp_package_ignore_free(ZSharpPackageIgnore *);
/* -1 means allocation failure; otherwise 1 excluded, 0 included. */
int zsharp_package_ignore_match(const ZSharpPackageIgnore *, const char *relative, int directory);
#endif
