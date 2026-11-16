#ifndef ZSHARP_INSTALLER_PATH_H
#define ZSHARP_INSTALLER_PATH_H

/* Per-user shell integration, kept header-only for the bootstrap builders. */
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define INSTALLER_PATH_START "# >>> Z# installer PATH >>>"
#define INSTALLER_PATH_END "# <<< Z# installer PATH <<<"
#define INSTALLER_PATH_FILE_LIMIT (1024u * 1024u)

static void installer_path_error(char *error, size_t size,
                                 const char *format, ...) {
    va_list args;
    if (!error || !size) return;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static char *installer_path_join(const char *left, const char *right) {
    size_t a = strlen(left), b = strlen(right);
    int separator = a && left[a - 1] != '/';
    char *result;
    if (a > SIZE_MAX - b - 2) return NULL;
    result = (char *)malloc(a + b + (size_t)separator + 1);
    if (!result) return NULL;
    memcpy(result, left, a);
    if (separator) result[a++] = '/';
    memcpy(result + a, right, b + 1);
    return result;
}

static int installer_path_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int installer_path_mkdirs(const char *path) {
    char *copy = installer_path_join(path, "");
    char *cursor;
    int ok = 1;
    if (!copy) return 0;
    for (cursor = copy + 1; ; cursor++) {
        if (*cursor == '/' || !*cursor) {
            char saved = *cursor;
            *cursor = '\0';
            if (mkdir(copy, 0700) != 0 &&
                (errno != EEXIST || !installer_path_directory(copy))) ok = 0;
            *cursor = saved;
            if (!ok || !saved) break;
        }
    }
    free(copy);
    return ok;
}

/* POSIX and fish have different rules inside single-quoted strings. */
static char *installer_path_quote(const char *text, int fish) {
    size_t length = strlen(text), i, used = 0;
    char *result;
    if (length > (SIZE_MAX - 3) / 4) return NULL;
    result = (char *)malloc(length * 4 + 3);
    if (!result) return NULL;
    result[used++] = '\'';
    for (i = 0; i < length; i++) {
        if (text[i] == '\'') {
            if (fish) {
                result[used++] = '\\';
                result[used++] = '\'';
            } else {
                memcpy(result + used, "'\\''", 4);
                used += 4;
            }
        } else {
            if (fish && text[i] == '\\') result[used++] = '\\';
            result[used++] = text[i];
        }
    }
    result[used++] = '\'';
    result[used] = '\0';
    return result;
}

static char *installer_path_block(const char *directory, int fish) {
    static const char posix_format[] =
        INSTALLER_PATH_START "\n"
        "case \":${PATH-}:\" in\n"
        "    *:%s:*) ;;\n"
        "    *) PATH=%s${PATH:+\":$PATH\"} ;;\n"
        "esac\n"
        "export PATH\n"
        INSTALLER_PATH_END "\n";
    static const char fish_format[] =
        INSTALLER_PATH_START "\n"
        "if not contains -- %s $PATH\n"
        "    set -gx PATH %s $PATH\n"
        "end\n"
        INSTALLER_PATH_END "\n";
    const char *format = fish ? fish_format : posix_format;
    char *quoted = installer_path_quote(directory, fish);
    char *result;
    int length;
    if (!quoted) return NULL;
    length = snprintf(NULL, 0, format, quoted, quoted);
    result = length < 0 ? NULL : (char *)malloc((size_t)length + 1);
    if (result) snprintf(result, (size_t)length + 1, format, quoted, quoted);
    free(quoted);
    return result;
}

static int installer_path_marker(const char *line, size_t size,
                                  const char *marker) {
    if (size && line[size - 1] == '\r') size--;
    return size == strlen(marker) && !memcmp(line, marker, size);
}

/* Replace complete owned blocks; never interpret or source the user's shell code. */
static char *installer_path_content(const char *old, size_t old_size,
                                    const char *block, size_t *new_size,
                                    char *error, size_t error_size) {
    size_t block_size = strlen(block), pos = 0, copied = 0, used = 0, start = 0;
    int inside = 0, inserted = 0;
    char *result;
    if (old_size > SIZE_MAX - block_size - 2) return NULL;
    result = (char *)malloc(old_size + block_size + 2);
    if (!result) return NULL;
    while (pos < old_size) {
        size_t line = pos, length;
        while (pos < old_size && old[pos] != '\n') pos++;
        length = pos - line;
        if (pos < old_size) pos++;
        if (installer_path_marker(old + line, length, INSTALLER_PATH_START)) {
            if (inside) goto malformed;
            inside = 1;
            start = line;
        } else if (installer_path_marker(old + line, length, INSTALLER_PATH_END)) {
            if (!inside) goto malformed;
            memcpy(result + used, old + copied, start - copied);
            used += start - copied;
            if (!inserted) {
                memcpy(result + used, block, block_size);
                used += block_size;
                inserted = 1;
            }
            copied = pos;
            inside = 0;
        }
    }
    if (inside) goto malformed;
    memcpy(result + used, old + copied, old_size - copied);
    used += old_size - copied;
    if (!inserted) {
        if (used && result[used - 1] != '\n') result[used++] = '\n';
        memcpy(result + used, block, block_size);
        used += block_size;
    }
    result[used] = '\0';
    *new_size = used;
    return result;
malformed:
    installer_path_error(error, error_size,
                         "incomplete or nested Z# PATH markers; file left unchanged");
    free(result);
    return NULL;
}

static int installer_path_same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime &&
#ifdef __APPLE__
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static int installer_path_write(const char *path, const char *block,
                                char *error, size_t error_size) {
    struct stat before, current, opened;
    int existing = 0, input = -1, output = -1, ok = 0;
    char *target = NULL, *old = NULL, *updated = NULL, *temporary = NULL;
    size_t old_size = 0, new_size = 0, done = 0;
    const char *reason = NULL;
    if (lstat(path, &before) == 0) {
        existing = 1;
        if (!S_ISREG(before.st_mode) && !S_ISLNK(before.st_mode)) {
            reason = "profile is not a regular file or symlink"; goto finish;
        }
        target = realpath(path, NULL);
        if (!target) { reason = "could not resolve existing profile or symlink"; goto finish; }
        if (stat(target, &before) != 0 || !S_ISREG(before.st_mode)) {
            reason = "profile target is not a regular file"; goto finish;
        }
        input = open(target, O_RDONLY | O_NONBLOCK
#ifdef O_NOFOLLOW
                     | O_NOFOLLOW
#endif
        );
        if (input < 0 || fstat(input, &before) != 0 || !S_ISREG(before.st_mode)) {
            reason = "profile target is not a readable regular file"; goto finish;
        }
        if (before.st_size < 0 || (uintmax_t)before.st_size > INSTALLER_PATH_FILE_LIMIT) {
            reason = "profile exceeds the 1 MiB edit limit"; goto finish;
        }
        old_size = (size_t)before.st_size;
    } else {
        if (errno != ENOENT) { reason = "could not inspect profile"; goto finish; }
        target = installer_path_join(path, "");
        if (target && strlen(target) && target[strlen(target) - 1] == '/')
            target[strlen(target) - 1] = '\0';
    }
    if (!target) { reason = "out of memory"; goto finish; }
    old = (char *)malloc(old_size + 1);
    if (!old) { reason = "out of memory"; goto finish; }
    while (done < old_size) {
        ssize_t count = read(input, old + done, old_size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { reason = "could not read complete profile"; goto finish; }
        done += (size_t)count;
    }
    old[old_size] = '\0';
    if (memchr(old, '\0', old_size)) { reason = "profile contains NUL bytes"; goto finish; }
    if (existing && (fstat(input, &opened) != 0 ||
                     !installer_path_same_file(&before, &opened))) {
        reason = "profile changed while being read; retry installation"; goto finish;
    }
    updated = installer_path_content(old, old_size, block, &new_size, error, error_size);
    if (!updated) {
        if (!error || !error_size || !error[0]) reason = "out of memory";
        goto finish;
    }
    if (new_size > INSTALLER_PATH_FILE_LIMIT) {
        reason = "updated profile would exceed the 1 MiB edit limit"; goto finish;
    }
    if (new_size == old_size && !memcmp(updated, old, old_size)) { ok = 1; goto finish; }
    temporary = (char *)malloc(strlen(target) + sizeof(".zsharp-path-XXXXXX"));
    if (!temporary) { reason = "out of memory"; goto finish; }
    sprintf(temporary, "%s.zsharp-path-XXXXXX", target);
    output = mkstemp(temporary);
    if (output < 0) { reason = "could not create a temporary profile beside the target"; goto finish; }
    if (existing) {
        struct stat temporary_info;
        if (fstat(output, &temporary_info) != 0 ||
            ((temporary_info.st_uid != before.st_uid || temporary_info.st_gid != before.st_gid) &&
             fchown(output, before.st_uid, before.st_gid) != 0)) {
            reason = "could not preserve profile ownership and permissions"; goto finish;
        }
    }
    done = 0;
    while (done < new_size) {
        ssize_t count = write(output, updated + done, new_size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { reason = "could not write complete profile"; goto finish; }
        done += (size_t)count;
    }
    if (existing && fchmod(output, before.st_mode & 07777) != 0) {
        reason = "could not preserve profile permissions"; goto finish;
    }
    if (fsync(output) != 0) { reason = "could not save profile"; goto finish; }
    if (close(output) != 0) { output = -1; reason = "could not close profile"; goto finish; }
    output = -1;
    if (existing) {
        if (lstat(target, &current) != 0 || !S_ISREG(current.st_mode) ||
            !installer_path_same_file(&before, &current) ||
            stat(path, &opened) != 0 || !installer_path_same_file(&before, &opened)) {
            reason = "profile changed before replacement; retry installation"; goto finish;
        }
    } else if (lstat(target, &current) == 0 || errno != ENOENT) {
        reason = "profile appeared before replacement; retry installation"; goto finish;
    }
    if (rename(temporary, target) != 0) { reason = "could not replace profile atomically"; goto finish; }
    ok = 1;
finish:
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    if (temporary) unlink(temporary);
    if (reason) installer_path_error(error, error_size, "%s: %s", path, reason);
    else if (!ok && error && error_size && error[0]) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%.255s", error);
        installer_path_error(error, error_size, "%s: %s", path, detail);
    }
    free(temporary); free(updated); free(old); free(target);
    return ok;
}

static int installer_path_apply(const char *directory, const char *name,
                                const char *block, char *error, size_t error_size) {
    char detail[512] = {0};
    char *path = installer_path_join(directory, name);
    int ok = path && installer_path_write(path, block, detail, sizeof(detail));
    if (!ok && error && error_size && !error[0])
        installer_path_error(error, error_size, "%s", path ? detail : "out of memory");
    free(path);
    return ok;
}

/* Returns 1 on success. A 0 is a nonfatal integration warning for the caller.
   HOME is deliberately supplied, never reassigned or inferred from PATH. */
static int installer_configure_user_path(const char *home,
                                         const char *install_directory,
                                         char *error, size_t error_size) {
    static const char *bash_names[] = { ".bashrc", ".profile" };
    static const char *login_names[] = { ".bash_profile", ".bash_login" };
    const char *zsh_home = getenv("ZDOTDIR"), *shell = getenv("SHELL");
    const char *xdg = getenv("XDG_CONFIG_HOME"), *shell_name;
    char *posix_block = NULL, *fish_block = NULL, *config = NULL, *fish_dir = NULL;
    size_t i;
    int ok = 1, selected_fish, selected_zsh;
    if (error && error_size) error[0] = '\0';
    if (!home || home[0] != '/' || !installer_path_directory(home) ||
        !install_directory || install_directory[0] != '/') {
        installer_path_error(error, error_size, "PATH setup needs an existing absolute home and an absolute install directory");
        return 0;
    }
    if (strpbrk(install_directory, ":\r\n")) {
        installer_path_error(error, error_size, "PATH setup cannot represent an install directory containing ':', CR or LF");
        return 0;
    }
    posix_block = installer_path_block(install_directory, 0);
    if (!posix_block) { installer_path_error(error, error_size, "out of memory"); return 0; }
    for (i = 0; i < sizeof(bash_names) / sizeof(bash_names[0]); i++)
        if (!installer_path_apply(home, bash_names[i], posix_block, error, error_size)) ok = 0;
    for (i = 0; i < sizeof(login_names) / sizeof(login_names[0]); i++) {
        char *path = installer_path_join(home, login_names[i]);
        struct stat info;
        int exists = path && stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, R_OK) == 0;
        if (!path) {
            if (error && error_size && !error[0]) installer_path_error(error, error_size, "out of memory");
            ok = 0;
        }
        free(path);
        if (exists) {
            if (!installer_path_apply(home, login_names[i], posix_block, error, error_size)) ok = 0;
            break;
        }
    }
    shell_name = shell ? strrchr(shell, '/') : NULL;
    shell_name = shell_name ? shell_name + 1 : shell;
    if (!zsh_home) zsh_home = home;
    selected_zsh = shell_name && !strcmp(shell_name, "zsh");
    if (!selected_zsh && zsh_home[0] == '/') {
        const char *names[] = { ".zshrc", ".zprofile" };
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            struct stat info;
            char *path = installer_path_join(zsh_home, names[i]);
            if (path && lstat(path, &info) == 0) selected_zsh = 1;
            free(path);
        }
    }
    if (selected_zsh && (zsh_home[0] != '/' || !installer_path_directory(zsh_home))) {
        if (error && error_size && !error[0])
            installer_path_error(error, error_size, "ZDOTDIR must name an existing absolute directory; zsh profiles left unchanged");
        ok = 0;
    } else if (selected_zsh) {
        if (!installer_path_apply(zsh_home, ".zshrc", posix_block, error, error_size)) ok = 0;
        if (!installer_path_apply(zsh_home, ".zprofile", posix_block, error, error_size)) ok = 0;
    }
    config = installer_path_join(xdg && xdg[0] == '/' ? xdg : home,
                                 xdg && xdg[0] == '/' ? "fish/conf.d" : ".config/fish/conf.d");
    fish_dir = config;
    selected_fish = shell_name && !strcmp(shell_name, "fish");
    if (!fish_dir) {
        if (error && error_size && !error[0]) installer_path_error(error, error_size, "out of memory");
        ok = 0;
    } else if (selected_fish || installer_path_directory(fish_dir)) {
        fish_block = installer_path_block(install_directory, 1);
        if (!fish_block || !installer_path_mkdirs(fish_dir)) {
            if (error && error_size && !error[0])
                installer_path_error(error, error_size, "could not prepare fish PATH configuration in %s", fish_dir);
            ok = 0;
        } else if (!installer_path_apply(fish_dir, "zsharp-path.fish", fish_block, error, error_size)) ok = 0;
    }
    free(config); free(fish_block); free(posix_block);
    return ok;
}
#endif /* !_WIN32 */
#endif /* ZSHARP_INSTALLER_PATH_H */
