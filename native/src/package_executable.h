#ifndef ZSHARP_PACKAGE_EXECUTABLE_H
#define ZSHARP_PACKAGE_EXECUTABLE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Called only after an extracted entry passed SHA-256/CRC validation. The
 * package format intentionally stores no arbitrary permissions. Go companions
 * need owner-execute on POSIX; ordinary assets and Windows .exe stay unchanged. */
static int zsharp_package_enable_go_companion(const char *path,
                                              char *error, size_t size) {
#ifdef _WIN32
    (void)path; (void)error; (void)size;
    return 1;
#else
    struct stat status, opened;
    size_t length = strlen(path);
    int descriptor, flags = O_RDONLY, ok;
    if (length < 4 || strcmp(path + length - 4, ".zgo")) return 1;
    if (lstat(path, &status) || !S_ISREG(status.st_mode)) {
        snprintf(error, size, "Go companion is not a regular extracted file: '%s'", path);
        return 0;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0 || fstat(descriptor, &opened) || !S_ISREG(opened.st_mode) ||
        opened.st_dev != status.st_dev || opened.st_ino != status.st_ino) {
        if (descriptor >= 0) close(descriptor);
        snprintf(error, size, "could not open the validated Go companion: '%s'", path);
        return 0;
    }
    ok = fchmod(descriptor, (opened.st_mode & 0777) | S_IXUSR) == 0;
    close(descriptor);
    if (!ok) snprintf(error, size, "could not enable owner-execute for Go companion: '%s'", path);
    return ok;
#endif
}

#endif
