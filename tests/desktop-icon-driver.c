#define _CRT_SECURE_NO_WARNINGS
#include "app_icon.h"
#include "desktop.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

int main(int argc, char **argv) {
    const char *capture = getenv("ZSHARP_ICON_ARGUMENT_CAPTURE");
    char error[1024] = {0};
    int ok = 0;
    if (argc > 1 && capture != NULL &&
        (!strcmp(argv[1], "run") || !strcmp(argv[1], "run-bytecode") ||
         !strcmp(argv[1], "open-desktop") || !strcmp(argv[1], "open") ||
         !strcmp(argv[1], "hub"))) {
        int index, written;
        char *temporary = (char *)malloc(strlen(capture) + 5);
        FILE *file;
        if (temporary == NULL) return 1;
        sprintf(temporary, "%s.tmp", capture);
        file = fopen(temporary, "wb");
        if (file == NULL) { free(temporary); return 1; }
        if (!put_u32(file, (uint32_t)(argc - 1))) {
            fclose(file); free(temporary); return 1;
        }
        for (index = 1; index < argc; index++) {
            size_t length = strlen(argv[index]);
            if (!put_u32(file, (uint32_t)length) ||
                fwrite(argv[index], 1, length, file) != length) {
                fclose(file); free(temporary); return 1;
            }
        }
        written = fclose(file) == 0 && rename(temporary, capture) == 0;
        free(temporary);
        return written ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "id")) {
        char *identity = zsharp_app_desktop_id(argv[2]);
        if (identity == NULL) return 1;
        puts(identity); free(identity); return 0;
    }
    if (argc == 7 && !strcmp(argv[1], "register"))
        ok = zsharp_desktop_register_application(argv[2], argv[3], argv[5],
              atoi(argv[4]), argv[6], error, sizeof(error));
    else if (argc == 2 && !strcmp(argv[1], "hub-shortcut"))
        ok = zsharp_desktop_create_hub_shortcut(error, sizeof(error));
    else if (argc == 6 && !strcmp(argv[1], "shortcut"))
        ok = zsharp_desktop_create_shortcut(argv[2], argv[3], argv[4], argv[5],
                                            error, sizeof(error));
    else {
        fputs("usage: desktop-icon-driver id|register|hub-shortcut|shortcut ...\n", stderr);
        return 2;
    }
    if (!ok) fprintf(stderr, "%s\n", error);
    return ok ? 0 : 1;
}
