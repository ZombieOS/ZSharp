#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "app_icon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(path) (_mkdir(path) == 0 || errno == EEXIST)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(path) (mkdir(path, 0700) == 0 || errno == EEXIST)
#endif

#define CHECK(test, message) do { if (!(test)) { \
    fprintf(stderr, "app icon regression: %s (line %d)\n", message, __LINE__); return 1; \
} } while (0)

/* Original one-pixel RGBA fixture, including valid PNG chunks and checksums. */
static const unsigned char custom_png[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,
    8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,16,73,
    185,243,31,0,4,56,2,84,194,249,99,149,0,0,0,0,73,69,78,68,174,66,96,130
};

static void environment(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value == NULL ? "" : value);
#else
    if (value == NULL) unsetenv(name); else setenv(name, value, 1);
#endif
}

static int write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    int written, closed;
    if (file == NULL) return 0;
    written = fwrite(data, 1, size, file) == size;
    closed = fclose(file) == 0;
    return written && closed;
}

static int same_files(const char *first, const char *second) {
    unsigned char a[4096], b[4096];
    FILE *left = fopen(first, "rb"), *right = fopen(second, "rb");
    size_t na = 0, nb = 0;
    int equal = left != NULL && right != NULL;
    while (equal) {
        na = fread(a, 1, sizeof(a), left); nb = fread(b, 1, sizeof(b), right);
        if (na != nb || memcmp(a, b, na)) { equal = 0; break; }
        if (!na) break;
    }
    if (left != NULL) { if (ferror(left)) equal = 0; fclose(left); }
    if (right != NULL) { if (ferror(right)) equal = 0; fclose(right); }
    return equal;
}

static int settings_file(const char *path, const char *icon) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    fputs("zsharp = type.settings\nProject: \"Icon Fixture\":\nPID: \"icon_fixture\":\n"
          "Version: [1.4.1.0]:\nAuthors: [\"Z# Tests\"]:\nDescription: \"Icon selection\":\n"
          "ZSharp: [1.4.1.0]:\nDependencies (\n):\n", file);
    if (icon != NULL) fprintf(file, "Icon: \"%s\":\n", icon);
    return fclose(file) == 0;
}

int main(int argc, char **argv) {
    char root[4096], data[4096], project[4096], assets[4096], custom[4096];
    char legacy[4096], invalid[4096], settings[4096], blocked[4096], error[512] = {0};
    const char *data_variable;
    char *official, *again, *path, *first_id, *second_id;
    unsigned char bad_header[sizeof(custom_png)];
#ifndef _WIN32
    struct stat before, after;
#endif
    if (argc != 3) { fputs("usage: app-icon-tests SCRATCH_DIRECTORY OFFICIAL_PNG\n", stderr); return 2; }
    CHECK(make_dir(argv[1]), "prepare scratch parent");
#ifdef _WIN32
    CHECK(GetTempFileNameA(argv[1], "zsi", 0, root) != 0, "unique test directory name");
    CHECK(DeleteFileA(root) && make_dir(root), "create isolated test directory");
    data_variable = "LOCALAPPDATA";
#else
    snprintf(root, sizeof(root), "%s/app-icons-XXXXXX", argv[1]);
    CHECK(mkdtemp(root) != NULL, "create isolated test directory");
    data_variable = "XDG_DATA_HOME";
#endif
    snprintf(data, sizeof(data), "%s/data", root);
    snprintf(project, sizeof(project), "%s/project with spaces", root);
    snprintf(assets, sizeof(assets), "%s/assets", project);
    snprintf(custom, sizeof(custom), "%s/project.png", assets);
    snprintf(legacy, sizeof(legacy), "%s/legacy.png", assets);
    snprintf(invalid, sizeof(invalid), "%s/invalid.png", assets);
    snprintf(settings, sizeof(settings), "%s/project.zsettings", project);
    snprintf(blocked, sizeof(blocked), "%s/blocked", root);
    CHECK(make_dir(project) && make_dir(assets), "create fixture project");
    CHECK(write_bytes(custom, custom_png, sizeof(custom_png)) &&
          write_bytes(legacy, custom_png, sizeof(custom_png)), "write real custom PNG fixtures");
    CHECK(write_bytes(invalid, "not a PNG", 9), "write invalid icon fixture");
    environment(data_variable, data);
    environment("ZSHARP_HUB_DEFAULT_ICON", NULL);
#ifdef __APPLE__
    /* macOS uses HOME/Library for its cache. Keep that untouched in this test;
       validate selection/override there, and exercise cache repair on Linux/Windows. */
    environment("ZSHARP_HUB_DEFAULT_ICON", argv[2]);
#endif
    CHECK(zsharp_app_icon_is_png(argv[2]) && zsharp_app_icon_is_png(custom), "real PNG files accepted");
    CHECK(!zsharp_app_icon_is_png(invalid) && !zsharp_app_icon_is_png(assets) &&
          !zsharp_app_icon_is_png(NULL), "nonimages and directories rejected");
    memcpy(bad_header, custom_png, sizeof(bad_header));
    memset(bad_header+16, 0, 4);
    CHECK(write_bytes(invalid, bad_header, sizeof(bad_header)), "write invalid IHDR fixture");
    CHECK(!zsharp_app_icon_is_png(invalid), "zero PNG width rejected");
#ifndef _WIN32
    {
        char fifo[4096]; snprintf(fifo, sizeof(fifo), "%s/pipe.png", assets);
        CHECK(mkfifo(fifo, 0600) == 0, "create nonregular PNG path");
        CHECK(!zsharp_app_icon_is_png(fifo), "named pipe rejected without blocking");
        unlink(fifo);
    }
#endif
    official = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(official != NULL && zsharp_app_icon_is_png(official), "offline default icon exists");
    CHECK(same_files(official, argv[2]), "embedded default preserves official PNG bytes exactly");
    CHECK(error[0] == '\0', "successful fallback clears errors");
#ifndef _WIN32
    CHECK(official[0] == '/', "default resolver returns an absolute path");
    CHECK(stat(official, &before) == 0, "inspect cached default");
#endif
    again = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(again != NULL && !strcmp(again, official), "default path stable across calls");
#ifndef _WIN32
    CHECK(stat(official, &after) == 0 && before.st_ino == after.st_ino, "matching cache is reused");
#endif
    free(again);
#ifndef __APPLE__
    CHECK(write_bytes(official, custom_png, sizeof(custom_png)), "replace cache with another valid PNG");
    again = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(again != NULL && same_files(again, argv[2]), "wrong but valid cached PNG repaired from embedded logo");
    free(again);
    CHECK(write_bytes(official, "broken", 6), "corrupt default cache");
    again = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(again != NULL && same_files(again, argv[2]), "truncated cache repaired offline");
    free(again);
#endif
    path = zsharp_app_icon_resolve(project, "assets/project.png", error, sizeof(error));
    CHECK(path != NULL && same_files(path, custom) && strcmp(path, official), "project-relative PNG selected before default");
    free(path);
    path = zsharp_app_icon_resolve(project, custom, error, sizeof(error));
    CHECK(path != NULL && same_files(path, custom), "absolute internal PNG supported");
    free(path);
    path = zsharp_app_icon_resolve(project, "assets/missing.png", error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "missing image resolves official fallback");
    free(path);
    path = zsharp_app_icon_resolve(project, "assets/invalid.png", error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "invalid image resolves official fallback");
    free(path);
    CHECK(settings_file(settings, "assets/project.png"), "write settings with project Icon");
    path = zsharp_app_icon_for_window(project, "icon_fixture", "assets/legacy.png", error, sizeof(error));
    CHECK(path != NULL && strstr(path, "project.png") && same_files(path, custom), "settings Icon wins over legacy design icon");
    free(path);
    path = zsharp_app_icon_for_window(project, NULL, NULL, error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "anonymous window ignores unrelated settings");
    free(path);
    path = zsharp_app_icon_for_window(project, "zsharp-hub", NULL, error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "Hub never adopts current project's settings icon");
    free(path);
    CHECK(settings_file(settings, NULL), "write settings without optional Icon");
    path = zsharp_app_icon_for_window(project, "icon_fixture", "assets/legacy.png", error, sizeof(error));
    CHECK(path != NULL && strstr(path, "legacy.png"), "missing Icon setting preserves legacy design icon");
    free(path);
    path = zsharp_app_icon_for_window(project, "icon_fixture", NULL, error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "missing setting and legacy icon uses offline official logo");
    free(path);
    CHECK(settings_file(settings, "assets/missing.png"), "write unavailable explicit icon");
    path = zsharp_app_icon_for_window(project, "icon_fixture", "assets/legacy.png", error, sizeof(error));
    CHECK(path != NULL && strstr(path, "legacy.png"), "unavailable image can fall back to valid legacy icon");
    free(path);
    environment("ZSHARP_HUB_DEFAULT_ICON", custom);
    path = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(path != NULL && same_files(path, custom) && strcmp(path, official), "valid Hub test override preserved");
    free(path);
    environment("ZSHARP_HUB_DEFAULT_ICON", invalid);
#ifndef __APPLE__
    path = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(path != NULL && same_files(path, argv[2]), "invalid override ignored in favor of embedded official logo");
    free(path);
    environment("ZSHARP_HUB_DEFAULT_ICON", NULL);
    CHECK(write_bytes(blocked, "file", 4), "create unavailable data-directory fixture");
    environment(data_variable, blocked);
    path = zsharp_app_icon_resolve(project, custom, error, sizeof(error));
    CHECK(path != NULL && same_files(path, custom), "valid project image does not need writable fallback cache");
    free(path);
    path = zsharp_app_icon_default_path(error, sizeof(error));
    CHECK(path == NULL && error[0] != '\0', "unwritable cache reports failure without using user HOME");
    environment(data_variable, data);
#else
    environment("ZSHARP_HUB_DEFAULT_ICON", argv[2]);
#endif
    first_id = zsharp_app_desktop_id("hello");
    second_id = zsharp_app_desktop_id("hello");
    CHECK(first_id != NULL && second_id != NULL && !strcmp(first_id, second_id) &&
          !strcmp(first_id, "zsharp-app-a430d84680aabd0b"), "stable FNV-1a desktop identity");
    free(second_id);
    second_id = zsharp_app_desktop_id("world");
    CHECK(second_id != NULL && strcmp(first_id, second_id), "different projects have distinct grouping IDs");
    free(first_id); free(second_id);
    first_id = zsharp_app_desktop_id(NULL); second_id = zsharp_app_desktop_id("zsharp-hub");
    CHECK(first_id != NULL && second_id != NULL && !strcmp(first_id, "zsharp-hub") &&
          !strcmp(first_id, second_id), "Hub has one stable desktop identity");
    free(first_id); free(second_id);
    free(official);
    printf("App icon API: PNG validation, offline cache, precedence, overrides and desktop IDs passed (%s)\n", root);
    return 0;
}
