#include "package_ignore.h"
#include <stdio.h>

static int failures;
static void check(const char *pattern, const char *path, int rooted, int directory, int expected) {
    ZSharpIgnoreRule rule = {(char *)pattern, 0, directory, rooted};
    ZSharpPackageIgnore ignore = {&rule, 1};
    int actual = zsharp_package_ignore_match(&ignore, path, directory);
    if (actual != expected) { fprintf(stderr, "%s / %s: expected %d got %d\n", pattern, path, expected, actual); failures++; }
}
int main(void) {
    ZSharpIgnoreRule rules[] = {{"*.txt", 0, 0, 0}, {"keep.txt", 1, 0, 0}};
    ZSharpPackageIgnore ignore = {rules, 2};
    check("*.obj", "C/Main.obj", 0, 0, 1);
    check("*.obj", "C/Main.dll", 0, 0, 0);
    check("secret", "deep/secret", 0, 0, 1);
    check("secret", "deep/secret", 1, 0, 0);
    check("assets/*", "assets/a.png", 1, 0, 1);
    check("assets/*", "assets/deep/a.png", 1, 0, 0);
    check("**/*.tmp", "a.tmp", 1, 0, 1);
    check("**/*.tmp", "a/b/c.tmp", 1, 0, 1);
    check("a/**/b", "a/b", 1, 0, 1);
    check("a/**/b", "a/x/y/b", 1, 0, 1);
    check("a/**/b", "a/xb", 1, 0, 0);
    check("a/**", "a/x/y/z", 1, 0, 1);
    check("file?.txt", "file1.txt", 0, 0, 1);
    check("file?.txt", "file12.txt", 0, 0, 0);
    check("[a-c].txt", "b.txt", 0, 0, 1);
    check("[!a-c].txt", "z.txt", 0, 0, 1);
    check("[!a-c].txt", "b.txt", 0, 0, 0);
    check("\\#secret", "#secret", 0, 0, 1);
    check("\\!secret", "!secret", 0, 0, 1);
    check("literal\\*", "literal*", 0, 0, 1);
    check("Mixed", "mixed", 0, 0, 0);
    if (zsharp_package_ignore_match(&ignore, "keep.txt", 0) ||
        !zsharp_package_ignore_match(&ignore, "drop.txt", 0)) failures++;
    return failures ? 1 : 0;
}
