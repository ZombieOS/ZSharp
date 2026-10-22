#define _CRT_SECURE_NO_WARNINGS
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "desktop_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static unsigned assertions;
static void require(int ok, const char *name, const char *error) {
    if (!ok) {
        fprintf(stderr, "desktop API fixture failed: %s: %s\n", name, error ? error : "");
        exit(1);
    }
    assertions++;
}
static int set_environment(const char *key, const char *value) {
#ifdef _WIN32
    return _putenv_s(key, value) == 0;
#else
    return value[0] ? setenv(key, value, 1) == 0 : unsetenv(key) == 0;
#endif
}
static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file); return 1;
}
static void write_fixture(const char *path) {
    FILE *file = fopen(path, "wb");
    require(file != NULL, "create source fixture", path);
    require(fputs("zsharp = type.script\nnoticed room Fixture[] ()\n", file) >= 0,
            "write source fixture", path);
    require(fclose(file) == 0, "close source fixture", path);
}

int main(int argc, char **argv) {
    char error[1024] = {0}, root[4096], first[4200], second[4200];
    char path[4200], other[4200], body[32768];
    char *document;
    const char *extension;
    const char *disabled_args[] = {"fixture-must-not-open-an-application"};
    ZSharpSettings settings;
    FILE *file;
    int created;
    if (argc != 2) { fputs("usage: desktop-api-tests <absolute temporary parent directory>\n", stderr); return 2; }
    /* There is no path to real integration in this executable: set the action
     * guard first, then require an absolute, caller-provided fixture location. */
    require(set_environment("ZSHARP_DESKTOP_DISABLE_ACTIONS", "1"), "disable real app actions", "");
#ifdef _WIN32
    require((strlen(argv[1]) > 2 && argv[1][1] == ':') ||
            (argv[1][0] == '\\' && argv[1][1] == '\\'), "absolute fixture parent", argv[1]);
    snprintf(root, sizeof(root), "%s/desktop-api-%lu", argv[1], (unsigned long)_getpid());
    created = _mkdir(root);
    extension = "windows-command.txt";
#else
    require(argv[1][0] == '/', "absolute fixture parent", argv[1]);
    snprintf(root, sizeof(root), "%s/desktop-api-%lu", argv[1], (unsigned long)getpid());
    created = mkdir(root, 0700);
#ifdef __APPLE__
    extension = "plist";
#else
    extension = "desktop";
#endif
#endif
    require(created == 0, "create unique owned fixture directory", root);
    require(set_environment("ZSHARP_STARTUP_DIRECTORY", root), "redirect startup to fixture directory", "");
    require(set_environment("ZSHARP_SKIP_DESKTOP_INTEGRATION", ""), "test isolated integration path", "");
    snprintf(first, sizeof(first), "%s/EntryOne.zsharp", root);
    snprintf(second, sizeof(second), "%s/EntryTwo.zsharp", root);
    write_fixture(first); write_fixture(second);

    document = zsharp_desktop_startup_document("windows", "fixture", "Fixture",
        "C:\\Z VM\\zsharp.exe", "D:\\Game Files\\Main.zsharp", 0, error, sizeof(error));
    require(document && !strcmp(document, "\"C:\\Z VM\\zsharp.exe\" run \"D:\\Game Files\\Main.zsharp\""),
            "Windows quoted argv", error); free(document);
    document = zsharp_desktop_startup_document("windows", "fixture", "Fixture",
        "C:\\Z VM\\zsharp.exe", "D:\\Game Files\\Main.zbc",
        ZDESKTOP_ENTRY_BYTECODE, error, sizeof(error));
    require(document && strstr(document, " run-bytecode ") && strstr(document, "Main.zbc"),
            "bytecode startup uses run-bytecode", error); free(document);
    document = zsharp_desktop_startup_document("linux", "fixture", "Fixture",
        "/opt/Z VM/zsharp", "/tmp/100% quoted\" $literal/Main.zsharp", 1, error, sizeof(error));
    require(document && strstr(document, "100%%") && strstr(document, " open ") && strstr(document, "\\\\$literal"),
            "Desktop Entry percent and argv escaping", error); free(document);
    document = zsharp_desktop_startup_document("macos", "fixture", "Fixture",
        "/opt/Z&VM/zsharp", "/tmp/a<b\"/Main.zsharp", 0, error, sizeof(error));
    require(document && strstr(document, "Z&amp;VM") && strstr(document, "a&lt;b&quot;") && strstr(document, "<string>run</string>"),
            "LaunchAgent XML argument escaping", error); free(document);
    document = zsharp_desktop_startup_document("linux", "../escape", "Fixture",
        "/opt/zsharp", "/tmp/Main.zsharp", 0, error, sizeof(error));
    require(document == NULL, "reject path traversal in project identity", error);
    document = zsharp_desktop_startup_document("linux", "fixture", "Fixture",
        "/opt/zsharp", "/tmp/line\nExec=bad", 0, error, sizeof(error));
    require(document == NULL, "reject injected desktop fields", error);
    require(!zsharp_desktop_action("openApp", disabled_args, 1, error, sizeof(error)) && strstr(error, "disabled"),
            "real desktop action guard", error);

    memset(&settings, 0, sizeof(settings));
    settings.project_id = "startup_fixture"; settings.project_name = "Startup Fixture";
    settings.has_launch_at_start = 1; settings.launch_at_start = 1;
    snprintf(path, sizeof(path), "%s/com.zombieos.zsharp.startup_fixture.%s", root, extension);
    snprintf(other, sizeof(other), "%s/com.zombieos.zsharp.other_fixture.%s", root, extension);
    require(zsharp_desktop_apply_startup(&settings, first, 0, error, sizeof(error)) && exists(path), "isolated registration created", error);
    require(zsharp_desktop_apply_startup(&settings, second, 0, error, sizeof(error)), "same PID registration replaced", error);
    file = fopen(path, "rb"); require(file != NULL, "registration readable", error);
    { size_t length = fread(body, 1, sizeof(body) - 1, file); body[length] = '\0'; fclose(file); }
    require(strstr(body, "EntryTwo.zsharp") != NULL, "moved entry refreshed", body);
    settings.has_launch_at_start = 0; settings.launch_at_start = 0;
    require(zsharp_desktop_apply_startup(&settings, NULL, 0, error, sizeof(error)) && exists(path), "omitted setting makes no change", error);
    settings.has_launch_at_start = 1; settings.launch_at_start = 1; settings.project_id = "other_fixture";
    require(zsharp_desktop_apply_startup(&settings, first, 0, error, sizeof(error)) && exists(other), "independent project registration", error);
    settings.project_id = "startup_fixture"; settings.launch_at_start = 0;
    require(zsharp_desktop_apply_startup(&settings, NULL, 0, error, sizeof(error)) && !exists(path) && exists(other), "dead removes only owned registration", error);
    require(set_environment("ZSHARP_SKIP_DESKTOP_INTEGRATION", "1"), "enable skip guard", "");
    settings.launch_at_start = 1;
    require(zsharp_desktop_apply_startup(&settings, first, 0, error, sizeof(error)) && !exists(path), "skip guard suppresses writes", error);
    require(set_environment("ZSHARP_SKIP_DESKTOP_INTEGRATION", ""), "disable skip for isolated cleanup", "");
    require(zsharp_desktop_remove_startup("other_fixture", error, sizeof(error)) && !exists(other), "owned uninstall cleanup", error);
    require(set_environment("ZSHARP_STARTUP_DIRECTORY", "relative-test-root"), "set invalid override", "");
    require(!zsharp_desktop_apply_startup(&settings, first, 0, error, sizeof(error)), "relative override rejected", error);
    require(set_environment("ZSHARP_STARTUP_DIRECTORY", root), "restore safe override", "");
    /* Remove only exact files created inside this test's unique directory. */
    require(remove(first) == 0 && remove(second) == 0, "remove owned source fixtures", "");
#ifdef _WIN32
    require(_rmdir(root) == 0, "remove empty owned fixture directory", root);
#else
    require(rmdir(root) == 0, "remove empty owned fixture directory", root);
#endif
    printf("desktop API fixtures: %u assertions passed; no real startup or app actions\n", assertions);
    return 0;
}
