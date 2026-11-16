#include "updater.h"
#include "zsharp.h"

#include <stdio.h>
#include <string.h>

/* Exercise the public updater API from an isolated executable directory. */
int main(int argc, char **argv) {
    ZSharpUpdatePreferences preferences;
    char error[512] = {0};
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%d.%d.%d.%d\n", ZSHARP_VERSION_MAJOR, ZSHARP_VERSION_MINOR,
               ZSHARP_VERSION_PATCH, ZSHARP_VERSION_REVISION);
        return 0;
    }
    if (argc != 3 ||
        (strcmp(argv[1], "manual") && strcmp(argv[1], "background")) ||
        (strcmp(argv[2], "stable") && strcmp(argv[2], "beta"))) {
        fputs("usage: updater-manifest-driver manual|background stable|beta\n",
              stderr);
        return 2;
    }
#ifdef _WIN32
    /* The Python harness is POSIX-only and never touches registry preferences. */
    return 77;
#else
    zsharp_update_preferences_default(&preferences);
    preferences.beta_updates = strcmp(argv[2], "beta") == 0;
    preferences.automatic_beta_updates = preferences.beta_updates;
    if (!zsharp_update_preferences_save(&preferences, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (strcmp(argv[1], "background") == 0) {
        zsharp_update_check_start();
        return 0;
    }
    if (!zsharp_update_now(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    return 0;
#endif
}
