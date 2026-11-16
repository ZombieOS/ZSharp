#include <stdio.h>

#ifndef _WIN32
#include "../installer/path.h"
#endif

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: installer-path-driver TEST_HOME INSTALL_DIRECTORY\n", stderr);
        return 2;
    }
#ifdef _WIN32
    return 77;
#else
    {
        char error[512] = {0};
        if (!installer_configure_user_path(argv[1], argv[2], error, sizeof(error))) {
            fprintf(stderr, "PATH setup failed: %s\n", error[0] ? error : "no diagnostic supplied");
            return 1;
        }
    }
    puts("Configured PATH for future terminal sessions.");
    return 0;
#endif
}
