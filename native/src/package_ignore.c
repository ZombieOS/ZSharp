#define _CRT_SECURE_NO_WARNINGS
#include "package_ignore.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zsharp_package_ignore_free(ZSharpPackageIgnore *ignore) {
    size_t i;
    for (i = 0; i < ignore->count; i++) free(ignore->rules[i].pattern);
    free(ignore->rules);
    memset(ignore, 0, sizeof(*ignore));
}

int zsharp_package_ignore_load(ZSharpPackageIgnore *ignore, const char *root,
                              char *error, size_t size) {
    char *path = malloc(strlen(root) + 12), *buffer = NULL, *line;
    FILE *file;
    long length;
    int ok = 0;
    if (!path) goto memory;
    sprintf(path, "%s/.zignore", root);
    file = fopen(path, "rb");
    free(path);
    if (!file) {
        if (errno == ENOENT) return 1;
        snprintf(error, size, "could not read project .zignore"); return 0;
    }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file); snprintf(error, size, ".zignore must be readable and at most 1 MiB"); return 0;
    }
    buffer = malloc((size_t)length + 1);
    if (!buffer) { fclose(file); goto memory; }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length || memchr(buffer, 0, (size_t)length)) {
        fclose(file); snprintf(error, size, ".zignore must be text without NUL bytes"); goto done;
    }
    fclose(file);
    buffer[length] = 0;
    line = buffer;
    if (length >= 3 && !memcmp(line, "\xef\xbb\xbf", 3)) line += 3;
    while (line) {
        char *next = strchr(line, '\n'), *end;
        ZSharpIgnoreRule rule = {0}, *resized;
        if (next) *next++ = 0;
        end = line + strlen(line);
        if (end > line && end[-1] == '\r') *--end = 0;
        while (end > line && end[-1] == ' ') {
            size_t slashes = 0;
            char *p = end - 1;
            while (p > line && p[-1] == '\\') { slashes++; p--; }
            if (slashes % 2) break;
            *--end = 0;
        }
        if (!*line || *line == '#') goto next_line;
        if (*line == '!') { rule.negate = 1; line++; }
        if (*line == '/') { rule.rooted = 1; line++; }
        end = line + strlen(line);
        if (end > line && end[-1] == '/') { rule.directory = 1; *--end = 0; }
        if (!*line) goto next_line;
        if (strchr(line, '/')) rule.rooted = 1;
        if (ignore->count >= 10000 || strlen(line) > 4096) {
            snprintf(error, size, ".zignore exceeds 10000 rules or 4096 bytes per pattern"); goto done;
        }
        rule.pattern = malloc(strlen(line) + 1);
        if (!rule.pattern) goto memory;
        strcpy(rule.pattern, line);
        resized = realloc(ignore->rules, (ignore->count + 1) * sizeof(*resized));
        if (!resized) { free(rule.pattern); goto memory; }
        ignore->rules = resized;
        ignore->rules[ignore->count++] = rule;
next_line:
        if (!next) break;
        line = next;
    }
    ok = 1;
    goto done;
memory:
    snprintf(error, size, "out of memory reading .zignore");
done:
    free(buffer);
    if (!ok) zsharp_package_ignore_free(ignore);
    return ok;
}

/* Iterative dynamic programming: no recursive glob backtracking. */
static int glob_match(const char *pattern, const char *text) {
    size_t n = strlen(text), j;
    unsigned char *a = calloc(n + 1, 1), *b = calloc(n + 1, 1), *swap;
    if (!a || !b) { free(a); free(b); return -1; }
    a[0] = 1;
    while (*pattern) {
        unsigned char literal = (unsigned char)*pattern++;
        memset(b, 0, n + 1);
        if (literal == '*') {
            int cross = *pattern == '*', directories;
            while (*pattern == '*') pattern++;
            directories = cross && *pattern == '/';
            b[0] = a[0];
            for (j = 1; j <= n; j++) b[j] = a[j] || (b[j - 1] && (cross || text[j - 1] != '/'));
            if (directories) {
                for (j = 1; j <= n; j++) if (text[j - 1] != '/') b[j] = a[j];
                pattern++;
            }
        } else if (literal == '?') {
            for (j = 1; j <= n; j++) b[j] = a[j - 1] && text[j - 1] != '/';
        } else if (literal == '[' && strchr(pattern, ']')) {
            const char *end = strchr(pattern, ']'), *start = pattern;
            int negate = *start == '!' || *start == '^';
            if (negate) start++;
            for (j = 1; j <= n; j++) {
                const char *p = start;
                unsigned char c = (unsigned char)text[j - 1];
                int found = 0;
                while (p < end) {
                    unsigned char low = (unsigned char)*p++;
                    if (low == '\\' && p < end) low = (unsigned char)*p++;
                    if (p + 1 < end && *p == '-') {
                        unsigned char high = (unsigned char)p[1]; p += 2;
                        if (c >= low && c <= high) found = 1;
                    } else if (c == low) found = 1;
                }
                b[j] = a[j - 1] && c != '/' && (negate ? !found : found);
            }
            pattern = end + 1;
        } else {
            if (literal == '\\' && *pattern) literal = (unsigned char)*pattern++;
            for (j = 1; j <= n; j++) b[j] = a[j - 1] && (unsigned char)text[j - 1] == literal;
        }
        swap = a; a = b; b = swap;
    }
    j = a[n]; free(a); free(b); return (int)j;
}

int zsharp_package_ignore_match(const ZSharpPackageIgnore *ignore, const char *relative, int directory) {
    size_t i;
    int excluded = 0;
    const char *base = strrchr(relative, '/');
    base = base ? base + 1 : relative;
    for (i = 0; i < ignore->count; i++) {
        const ZSharpIgnoreRule *rule = &ignore->rules[i];
        int match;
        if (rule->directory && !directory) continue;
        match = glob_match(rule->pattern, rule->rooted ? relative : base);
        if (match < 0) return -1;
        if (match) excluded = !rule->negate;
    }
    return excluded;
}
