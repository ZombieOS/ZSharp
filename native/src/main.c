#define _CRT_SECURE_NO_WARNINGS

#include "zsharp.h"

#include "bytecode.h"
#include "desktop.h"
#include "game_runtime.h"
#include "package.h"
#include "project.h"
#include "provider_loader.h"
#include "registry.h"
#include "updater.h"
#include "vm.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static char command_failure[512];

static void remember_failure(const char *message) {
    snprintf(command_failure, sizeof(command_failure), "%s",
             message == NULL || message[0] == '\0'
                 ? "The Z# runtime did not provide a reason."
                 : message);
}

static void print_help(void) {
    puts("Z# toolchain");
    puts("");
    puts("Usage:");
    puts("  zsharp --version");
    puts("  zsharp game-info");
    puts("  zsharp check <file.zsharp|project.zsettings>");
    puts("  zsharp check-bytecode <file.zbc>");
    puts("  zsharp compile <file.zsharp> -o <output.zbc>");
    puts("  zsharp package <app|game> <project> <filename> [--unbytecode]");
    puts("  zsharp open <file.zapp|file.zgame>");
    puts("  zsharp uninstall <file.zapp|file.zgame>");
    puts("  zsharp associate");
    puts("  zsharp hub");
    puts("  zsharp project <project-directory|project.zsettings>");
    puts("  zsharp run <file.zsharp|file.zapp|file.zgame> [options]...");
    puts("  zsharp run-bytecode <file.zbc> [--provider Project=library]...");
}

static int has_source_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension != NULL && strcmp(extension, ZSHARP_SOURCE_EXTENSION) == 0;
}

static int is_package_file(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension != NULL &&
           (strcmp(extension, ".zapp") == 0 ||
            strcmp(extension, ".zgame") == 0);
}

static int is_settings_file(const char *path) {
    const char *name = path;
    const char *cursor;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return strcmp(name, ZSHARP_SETTINGS_FILE) == 0;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int parse_file(const char *path, ZSharpProgram *program) {
    ZSharpDiagnostic diagnostic;
    char error[256] = {0};
    int ok;
    if (!has_source_extension(path)) {
        snprintf(command_failure, sizeof(command_failure),
                 "Z# source files must use the %s extension",
                 ZSHARP_SOURCE_EXTENSION);
        fprintf(stderr, "error: %s\n", command_failure);
        return 0;
    }
    ok = zsharp_project_parse_file(path, program, &diagnostic, error,
                                   sizeof(error));
    if (!ok) {
        if (diagnostic.message[0] != '\0') {
            snprintf(command_failure, sizeof(command_failure),
                     "%s:%u:%u: %s", path, diagnostic.line,
                     diagnostic.column, diagnostic.message);
            fprintf(stderr, "%s:%u:%u: error: %s\n", path, diagnostic.line,
                    diagnostic.column, diagnostic.message);
        } else {
            remember_failure(error);
            fprintf(stderr, "error: %s\n", error);
        }
    }
    return ok;
}

static int load_settings_or_report(const char *project_root,
                                   ZSharpSettings *settings) {
    ZSharpDiagnostic diagnostic;
    char error[512] = {0};
    if (zsharp_settings_load(project_root, settings, &diagnostic, error,
                             sizeof(error))) {
        if (zsharp_project_validate_settings(settings, project_root, error,
                                             sizeof(error))) {
            return 1;
        }
        zsharp_settings_free(settings);
        remember_failure(error);
        fprintf(stderr, "compile error: %s\n", error);
        return 0;
    }
    if (diagnostic.message[0] != '\0') {
        snprintf(command_failure, sizeof(command_failure),
                 "%s:%u:%u: %s", ZSHARP_SETTINGS_FILE,
                 diagnostic.line, diagnostic.column, diagnostic.message);
        fprintf(stderr, "%s:%u:%u: compile error: %s\n",
                ZSHARP_SETTINGS_FILE, diagnostic.line, diagnostic.column,
                diagnostic.message);
    } else {
        remember_failure(error);
        fprintf(stderr, "compile error: %s\n", error);
    }
    return 0;
}

static char *settings_parent_directory(const char *path) {
    const char *last_separator = NULL;
    const char *cursor;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') last_separator = cursor;
    }
    if (last_separator == NULL) return zsharp_copy_text(".", 1);
    if (last_separator == path) return zsharp_copy_text(path, 1);
    return zsharp_copy_text(path, (size_t)(last_separator - path));
}

static char *join_project_path(const char *root, const char *relative) {
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    int separator = root_length > 0 && root[root_length - 1] != '/' &&
                    root[root_length - 1] != '\\';
    char *result = (char *)malloc(root_length + (size_t)separator +
                                  relative_length + 1);
    if (result == NULL) return NULL;
    memcpy(result, root, root_length);
    if (separator) {
#ifdef _WIN32
        result[root_length++] = '\\';
#else
        result[root_length++] = '/';
#endif
    }
    memcpy(result + root_length, relative, relative_length + 1);
    return result;
}

static int check_command(const char *source_path) {
    ZSharpProgram program;
    char error[512] = {0};
    char *project_root;
    ZSharpSettings settings;
    if (!parse_file(source_path, &program)) {
        return 1;
    }
    project_root = zsharp_project_find_root(source_path, error, sizeof(error));
    if (project_root == NULL ||
        !load_settings_or_report(project_root, &settings)) {
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (!zsharp_project_validate(&program, &settings, project_root, error,
                                 sizeof(error))) {
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        fprintf(stderr, "compile error: %s\n", error);
        return 1;
    }
    zsharp_settings_free(&settings);
    free(project_root);
    zsharp_program_free(&program);
    printf("%s: syntax accepted\n", source_path);
    return 0;
}

static int check_settings_command(const char *settings_path) {
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    char error[512] = {0};
    char *project_root;
    if (!zsharp_settings_parse_file(settings_path, &settings, &diagnostic,
                                    error, sizeof(error))) {
        if (diagnostic.message[0] != '\0') {
            fprintf(stderr, "%s:%u:%u: compile error: %s\n", settings_path,
                    diagnostic.line, diagnostic.column, diagnostic.message);
        } else {
            fprintf(stderr, "compile error: %s\n", error);
        }
        return 1;
    }
    project_root = settings_parent_directory(settings_path);
    if (project_root == NULL ||
        !zsharp_project_validate_settings(&settings, project_root, error,
                                          sizeof(error))) {
        fprintf(stderr, "compile error: %s\n",
                project_root == NULL ? "out of memory" : error);
        free(project_root);
        zsharp_settings_free(&settings);
        return 1;
    }
    free(project_root);
    printf("%s: settings accepted (PID %s, Z%u)\n", settings_path,
           settings.project_id, settings.zsharp_version[0]);
    zsharp_settings_free(&settings);
    return 0;
}

static int compile_command(const char *source_path, const char *output_path) {
    ZSharpProgram program;
    char error[256];
    int ok;
    char *project_root;
    ZSharpSettings settings;
    unsigned char project_identity[ZSHARP_SHA256_SIZE];
    unsigned char build_hash[ZSHARP_SHA256_SIZE];
    char project_identity_hex[ZSHARP_SHA256_SIZE * 2 + 1];
    char build_hash_hex[ZSHARP_SHA256_SIZE * 2 + 1];
    if (!parse_file(source_path, &program)) {
        return 1;
    }
    project_root = zsharp_project_find_root(source_path, error, sizeof(error));
    if (project_root == NULL ||
        !load_settings_or_report(project_root, &settings)) {
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (!zsharp_project_validate(&program, &settings, project_root, error,
                                 sizeof(error))) {
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        fprintf(stderr, "compile error: %s\n", error);
        return 1;
    }
    free(project_root);
    ok = zsharp_bytecode_write(output_path, &program, settings.project_id,
                               project_identity, build_hash, error,
                               sizeof(error));
    zsharp_settings_free(&settings);
    zsharp_program_free(&program);
    if (!ok) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    zsharp_hash_hex(project_identity, project_identity_hex);
    zsharp_hash_hex(build_hash, build_hash_hex);
    printf("project identity: %s\n", project_identity_hex);
    printf("build SHA-256: %s\n", build_hash_hex);
    return 0;
}

static int check_bytecode_command(const char *bytecode_path) {
    ZSharpProgram program;
    ZSharpSettings settings;
    char error[512] = {0};
    char *project_root;
    if (!zsharp_bytecode_read(bytecode_path, &program, error,
                              sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    project_root = zsharp_project_find_root(bytecode_path, error, sizeof(error));
    if (project_root == NULL ||
        !load_settings_or_report(project_root, &settings)) {
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (strcmp(program.project_id, settings.project_id) != 0) {
        snprintf(error, sizeof(error),
                 "bytecode belongs to PID '%s', but this project is '%s'",
                 program.project_id, settings.project_id);
    } else if (zsharp_project_validate(&program, &settings, project_root,
                                       error, sizeof(error))) {
        printf("%s: bytecode accepted\n", bytecode_path);
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        return 0;
    }
    fprintf(stderr, "compile error: %s\n", error);
    zsharp_settings_free(&settings);
    free(project_root);
    zsharp_program_free(&program);
    return 1;
}

static void unload_providers(ZSharpLoadedProvider *loaded, size_t count) {
    while (count > 0) zsharp_provider_unload(&loaded[--count]);
    free(loaded);
}

static int load_providers(int argc, char **argv, int first,
                          ZSharpLoadedProvider **loaded_output,
                          ZSharpProviderBinding **bindings_output,
                          size_t *count_output, char *error,
                          size_t error_size) {
    size_t capacity = (size_t)(argc - first) / 2;
    ZSharpLoadedProvider *loaded = NULL;
    ZSharpProviderBinding *bindings = NULL;
    size_t count = 0;
    int index;
    if (capacity > 0) {
        loaded = (ZSharpLoadedProvider *)calloc(capacity, sizeof(*loaded));
        bindings = (ZSharpProviderBinding *)calloc(capacity,
                                                    sizeof(*bindings));
        if (loaded == NULL || bindings == NULL) {
            free(loaded);
            free(bindings);
            snprintf(error, error_size, "out of memory");
            return 0;
        }
    }
    for (index = first; index < argc;) {
        size_t previous;
        if (strcmp(argv[index], "--") == 0) break;
        if (strcmp(argv[index], "--provider") != 0) {
            snprintf(error, error_size, "unknown run option '%s'", argv[index]);
            unload_providers(loaded, count);
            free(bindings);
            return 0;
        }
        if (index + 1 >= argc) {
            snprintf(error, error_size,
                     "--provider requires Project=path-to-library");
            unload_providers(loaded, count);
            free(bindings);
            return 0;
        }
        if (!zsharp_provider_load(argv[index + 1], &loaded[count], error,
                                  error_size)) {
            unload_providers(loaded, count);
            free(bindings);
            return 0;
        }
        for (previous = 0; previous < count; previous++) {
            if (strcmp(loaded[previous].binding.project_name,
                       loaded[count].binding.project_name) == 0) {
                snprintf(error, error_size,
                         "provider project '%s' was registered more than once",
                         loaded[count].binding.project_name);
                zsharp_provider_unload(&loaded[count]);
                unload_providers(loaded, count);
                free(bindings);
                return 0;
            }
        }
        bindings[count] = loaded[count].binding;
        count++;
        index += 2;
    }
    *loaded_output = loaded;
    *bindings_output = bindings;
    *count_output = count;
    return 1;
}

static int run_source_command(const char *source_path, int argc, char **argv,
                              int first_option) {
    ZSharpProgram program;
    ZSharpLoadedProvider *loaded = NULL;
    ZSharpProviderBinding *bindings = NULL;
    size_t provider_count = 0;
    char error[512] = {0};
    char *project_root;
    ZSharpSettings settings;
    int ok;
    command_failure[0] = '\0';
    if (!parse_file(source_path, &program)) {
        return 1;
    }
    project_root = zsharp_project_find_root(source_path, error, sizeof(error));
    if (project_root == NULL) {
        zsharp_program_free(&program);
        remember_failure(error);
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    if (!load_settings_or_report(project_root, &settings)) {
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (!zsharp_project_validate(&program, &settings, project_root, error,
                                 sizeof(error))) {
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        remember_failure(error);
        fprintf(stderr, "compile error: %s\n", error);
        return 1;
    }
    program.project_id = zsharp_copy_text(settings.project_id,
                                          strlen(settings.project_id));
    if (program.project_id == NULL) {
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        remember_failure("out of memory");
        fprintf(stderr, "runtime error: out of memory\n");
        return 1;
    }
    zsharp_settings_free(&settings);
    if (!load_providers(argc, argv, first_option, &loaded, &bindings,
                        &provider_count, error, sizeof(error))) {
        free(project_root);
        zsharp_program_free(&program);
        remember_failure(error);
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    ok = zsharp_vm_run_with_providers(&program, project_root, bindings,
                                      provider_count, error, sizeof(error));
    unload_providers(loaded, provider_count);
    free(bindings);
    free(project_root);
    zsharp_program_free(&program);
    if (!ok) {
        remember_failure(error);
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    return 0;
}

static int run_bytecode_command(const char *bytecode_path, int argc,
                                char **argv, int first_option) {
    ZSharpProgram program;
    ZSharpLoadedProvider *loaded = NULL;
    ZSharpProviderBinding *bindings = NULL;
    size_t provider_count = 0;
    char error[512] = {0};
    char *project_root;
    ZSharpSettings settings;
    int ok = zsharp_bytecode_read(bytecode_path, &program, error, sizeof(error));
    if (!ok) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    project_root = zsharp_project_find_root(bytecode_path, error, sizeof(error));
    if (project_root == NULL) {
        zsharp_program_free(&program);
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    if (!load_settings_or_report(project_root, &settings)) {
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (strcmp(program.project_id, settings.project_id) != 0) {
        fprintf(stderr,
                "runtime error: bytecode belongs to project PID '%s', but "
                "the current project PID is '%s'\n",
                program.project_id, settings.project_id);
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        return 1;
    }
    if (!zsharp_project_validate(&program, &settings, project_root, error,
                                 sizeof(error))) {
        zsharp_settings_free(&settings);
        free(project_root);
        zsharp_program_free(&program);
        fprintf(stderr, "compile error: %s\n", error);
        return 1;
    }
    zsharp_settings_free(&settings);
    if (!load_providers(argc, argv, first_option, &loaded, &bindings,
                        &provider_count, error, sizeof(error))) {
        free(project_root);
        zsharp_program_free(&program);
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    ok = zsharp_vm_run_with_providers(&program, project_root, bindings,
                                      provider_count, error, sizeof(error));
    unload_providers(loaded, provider_count);
    free(bindings);
    free(project_root);
    zsharp_program_free(&program);
    if (!ok) {
        fprintf(stderr, "runtime error: %s\n", error);
        return 1;
    }
    return 0;
}

static int project_command(const char *project_path) {
    char error[512] = {0};
    char *project_root = zsharp_project_find_root(project_path, error,
                                                  sizeof(error));
    ZSharpSettings settings;
    if (project_root == NULL) {
        fprintf(stderr, "project error: %s\n", error);
        return 1;
    }
    if (!load_settings_or_report(project_root, &settings)) {
        free(project_root);
        return 1;
    }
    if (!zsharp_registry_register_project(project_root, &settings, error,
                                          sizeof(error))) {
        fprintf(stderr, "project error: %s\n", error);
        zsharp_settings_free(&settings);
        free(project_root);
        return 1;
    }
    printf("registered Z# project '%s' at %s (PID %s)\n",
           settings.project_name, project_root, settings.project_id);
    zsharp_settings_free(&settings);
    free(project_root);
    return 0;
}

static int package_command(const char *kind_text, const char *project_path,
                           const char *package_name, int include_unbytecoded) {
    char error[512] = {0};
    unsigned char hash[ZSHARP_SHA256_SIZE];
    char hash_hex[ZSHARP_SHA256_SIZE * 2 + 1];
    char *output_path = NULL;
    char *source_output_path = NULL;
    ZSharpPackageKind kind;
    if (strcmp(kind_text, "app") == 0)
        kind = ZSHARP_PACKAGE_APP;
    else if (strcmp(kind_text, "game") == 0)
        kind = ZSHARP_PACKAGE_GAME;
    else {
        fputs("package error: package type must be 'app' or 'game'\n", stderr);
        return 2;
    }
    if (!zsharp_package_create_named(project_path, kind, package_name,
                                     &output_path, error, sizeof(error))) {
        fprintf(stderr, "package error: %s\n", error);
        return 1;
    }
    if (!zsharp_sha256_file(output_path, hash)) {
        fprintf(stderr, "package error: could not verify '%s'\n", output_path);
        free(output_path);
        return 1;
    }
    zsharp_hash_hex(hash, hash_hex);
    printf("created bytecoded package %s\nbytecoded package SHA-256: %s\n",
           output_path, hash_hex);
    if (include_unbytecoded) {
        if (!zsharp_package_create_unbytecoded_named(
                project_path, kind, package_name, &source_output_path,
                error, sizeof(error))) {
            fprintf(stderr, "package error: bytecoded package was created, "
                            "but its unbytecoded companion failed: %s\n",
                    error);
            free(output_path);
            return 1;
        }
        if (!zsharp_sha256_file(source_output_path, hash)) {
            fprintf(stderr, "package error: could not verify '%s'\n",
                    source_output_path);
            free(source_output_path);
            free(output_path);
            return 1;
        }
        zsharp_hash_hex(hash, hash_hex);
        printf("created unbytecoded package %s\n"
               "unbytecoded package SHA-256: %s\n",
               source_output_path, hash_hex);
    }
    free(source_output_path);
    free(output_path);
    return 0;
}

static int show_hub_message(const char *headline, const char *reason) {
    char hub_error[512] = {0};
    if (getenv("ZSHARP_HUB_CONSOLE_ONLY") != NULL) {
        printf("Z# Hub\n%s\n", headline == NULL ? "" : headline);
        if (reason != NULL && reason[0] != '\0') printf("%s\n", reason);
        return 1;
    }
    if (zsharp_window_show_hub(headline, reason, hub_error,
                               sizeof(hub_error))) return 1;
    fprintf(stderr, "Z# Hub\n%s\n", headline == NULL ? "" : headline);
    if (reason != NULL && reason[0] != '\0') fprintf(stderr, "%s\n", reason);
    if (hub_error[0] != '\0')
        fprintf(stderr, "Hub window unavailable: %s\n", hub_error);
    return 0;
}

static void show_app_failure(const char *app_name, const char *reason) {
    size_t length = strlen(app_name == NULL ? "Z# application" : app_name);
    char *headline = (char *)malloc(length + 19);
    if (headline == NULL) {
        show_hub_message("Z# application failed to launch!", reason);
        return;
    }
    snprintf(headline, length + 19, "%s failed to launch!",
             app_name == NULL ? "Z# application" : app_name);
    show_hub_message(headline, reason);
    free(headline);
}

static char *startup_window_icon(const char *startup_path) {
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic;
    char error[256] = {0};
    char *result = NULL;
    size_t element_index;
    if (!zsharp_project_parse_file(startup_path, &program, &diagnostic,
                                   error, sizeof(error))) return NULL;
    if (program.has_window) {
        for (element_index = 0;
             element_index < program.window.element_count; element_index++) {
            ZSharpUIElement *element = &program.window.elements[element_index];
            size_t property_index;
            if (element->type != ZUI_DESIGN) continue;
            for (property_index = 0;
                 property_index < element->property_count; property_index++) {
                ZSharpUIProperty *property = &element->properties[property_index];
                if (strcmp(property->name, "icon") == 0 &&
                    property->text_value != NULL)
                    result = zsharp_copy_text(property->text_value,
                                               strlen(property->text_value));
            }
        }
    }
    zsharp_program_free(&program);
    return result;
}

static void offer_desktop_shortcut(const char *app_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *startup_path) {
    char answer[32];
    char error[512] = {0};
    char *icon;
    printf("Add %s to your desktop? [y/N]: ", app_name);
    fflush(stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL ||
        (strcmp(answer, "y\n") != 0 && strcmp(answer, "y\r\n") != 0 &&
         strcmp(answer, "yes\n") != 0 && strcmp(answer, "yes\r\n") != 0)) {
        puts("Desktop shortcut skipped.");
        return;
    }
    icon = startup_window_icon(startup_path);
    if (!zsharp_desktop_create_shortcut(app_name, package_path, project_root,
                                        icon, error, sizeof(error)))
        fprintf(stderr, "shortcut warning: %s\n", error);
    else
        puts("Desktop shortcut created.");
    free(icon);
}

static void refresh_desktop_shortcut(const char *app_name,
                                     const char *package_path,
                                     const char *project_root,
                                     const char *startup_path) {
    char error[512] = {0};
    char *icon = startup_window_icon(startup_path);
    if (!zsharp_desktop_refresh_shortcut(
            app_name, package_path, project_root, icon,
            error, sizeof(error)))
        fprintf(stderr, "shortcut warning: %s\n", error);
    free(icon);
}

static char *find_game_startup_path(const char *project_root, char *error,
                                    size_t error_size) {
    ZSharpSourceList sources;
    char *first_2d = NULL;
    char *first_3d = NULL;
    size_t index;
    if (!zsharp_project_list_sources(project_root, &sources, error,
                                     error_size))
        return NULL;
    for (index = 0; index < sources.count; index++) {
        ZSharpProgram program;
        ZSharpDiagnostic diagnostic;
        char parse_error[512] = {0};
        char **candidate;
        if (!zsharp_project_parse_file(sources.items[index], &program,
                                       &diagnostic, parse_error,
                                       sizeof(parse_error))) {
            snprintf(error, error_size, "%s",
                     diagnostic.message[0] != '\0'
                         ? diagnostic.message
                         : parse_error);
            free(first_2d);
            free(first_3d);
            zsharp_project_source_list_free(&sources);
            return NULL;
        }
        candidate = program.script_type == ZSCRIPT_3D
                        ? &first_3d
                        : program.script_type == ZSCRIPT_2D ? &first_2d
                                                            : NULL;
        if (candidate != NULL &&
            (*candidate == NULL || strcmp(sources.items[index], *candidate) < 0)) {
            char *replacement = zsharp_copy_text(
                sources.items[index], strlen(sources.items[index]));
            if (replacement == NULL) {
                snprintf(error, error_size, "out of memory");
                zsharp_program_free(&program);
                free(first_2d);
                free(first_3d);
                zsharp_project_source_list_free(&sources);
                return NULL;
            }
            free(*candidate);
            *candidate = replacement;
        }
        zsharp_program_free(&program);
    }
    zsharp_project_source_list_free(&sources);
    if (first_3d != NULL) {
        free(first_2d);
        return first_3d;
    }
    if (first_2d != NULL) return first_2d;
    snprintf(error, error_size,
             "the game package has no 2D or 3D Z# startup script");
    return NULL;
}

static int open_package_command(const char *package_path, int argc,
                                char **argv, int first_option) {
    ZSharpPackageInfo info;
    ZSharpPackageInfo preview;
    ZSharpSettings settings;
    char error[512] = {0};
    char *root = NULL;
    char *startup = NULL;
    char *startup_bytecode;
    char *app_name;
    ZSharpPackageKind package_kind;
    int new_install = 0;
    int result;
    if (!zsharp_package_read_info(package_path, &preview, error,
                                  sizeof(error))) {
        fprintf(stderr, "package error: %s\n", error);
        show_app_failure("Z# application", error);
        return 1;
    }
    package_kind = preview.kind;
    app_name = zsharp_copy_text(preview.project_name,
                                strlen(preview.project_name));
    zsharp_package_info_free(&preview);
    if (app_name == NULL) {
        show_app_failure("Z# application", "Out of memory.");
        return 1;
    }
    if (!zsharp_package_extract(package_path, &root, &info, &new_install,
                                error,
                                sizeof(error))) {
        fprintf(stderr, "package error: %s\n", error);
        show_app_failure(app_name, error);
        free(app_name);
        return 1;
    }
    if (!load_settings_or_report(root, &settings)) {
        show_app_failure(app_name, command_failure);
        zsharp_package_info_free(&info);
        free(root);
        free(app_name);
        return 1;
    }
    if (package_kind == ZSHARP_PACKAGE_APP &&
        (!settings.has_window || settings.window_startup == NULL)) {
        fprintf(stderr, "package error: package has no Window Startup entry\n");
        show_app_failure(app_name, "The package has no Window Startup entry.");
        zsharp_settings_free(&settings);
        zsharp_package_info_free(&info);
        free(root);
        free(app_name);
        return 1;
    }
    if (package_kind == ZSHARP_PACKAGE_GAME)
        startup = find_game_startup_path(root, error, sizeof(error));
    else
        startup = join_project_path(root, settings.window_startup);
    startup_bytecode = join_project_path(
        root, ZSHARP_PACKAGE_STARTUP_BYTECODE);
    printf("opening %s '%s' (%s %u.%u.%u.%u)\n",
           info.kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
           info.project_name, info.project_id, info.version[0], info.version[1],
           info.version[2], info.version[3]);
    if (startup == NULL || startup_bytecode == NULL) {
        fprintf(stderr, "package error: %s\n",
                error[0] == '\0' ? "out of memory" : error);
        show_app_failure(app_name,
                         error[0] == '\0' ? "Out of memory." : error);
        zsharp_settings_free(&settings);
        zsharp_package_info_free(&info);
        free(root);
        free(app_name);
        free(startup);
        free(startup_bytecode);
        return 1;
    }
    if (getenv("ZSHARP_SKIP_DESKTOP_INTEGRATION") == NULL) {
        if (new_install) {
            char association_error[512] = {0};
            if (getenv("ZSHARP_SKIP_ASSOCIATION_INSTALL") == NULL &&
                !zsharp_desktop_install_associations(
                    association_error, sizeof(association_error)))
                fprintf(stderr, "association warning: %s\n",
                        association_error);
            offer_desktop_shortcut(app_name, package_path, root, startup);
        } else {
            refresh_desktop_shortcut(app_name, package_path, root, startup);
        }
    }
    zsharp_settings_free(&settings);
    zsharp_package_info_free(&info);
    free(root);
    if (file_exists(startup_bytecode)) {
        puts("running bytecoded startup");
        result = run_bytecode_command(startup_bytecode, argc, argv,
                                      first_option);
    } else {
        puts("running unbytecoded source startup");
        result = run_source_command(startup, argc, argv, first_option);
    }
    free(startup);
    free(startup_bytecode);
    if (result != 0)
        show_app_failure(app_name,
                         command_failure[0] == '\0'
                             ? "The application returned a launch error."
                             : command_failure);
    free(app_name);
    return result;
}

static int uninstall_package_command(const char *package_path) {
    ZSharpPackageInfo info;
    char error[512] = {0};
    char answer[32];
    if (!zsharp_package_read_info(package_path, &info, error, sizeof(error))) {
        fprintf(stderr, "uninstall error: %s\n", error);
        return 1;
    }
    printf("Permanently uninstall %s '%s' (%s %u.%u.%u.%u)?\n",
           info.kind == ZSHARP_PACKAGE_GAME ? "game" : "app",
           info.project_name, info.project_id, info.version[0], info.version[1],
           info.version[2], info.version[3]);
    printf("This removes its verified Z# package cache and deletes '%s'.\n",
           package_path);
    fputs("Type yes to continue: ", stdout);
    fflush(stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL ||
        (strcmp(answer, "yes\n") != 0 && strcmp(answer, "yes\r\n") != 0)) {
        puts("Uninstall cancelled; nothing was deleted.");
        zsharp_package_info_free(&info);
        return 0;
    }
    if (!zsharp_package_uninstall(package_path, 1, error, sizeof(error))) {
        fprintf(stderr, "uninstall error: %s\n", error);
        zsharp_package_info_free(&info);
        return 1;
    }
    error[0] = '\0';
    if (!zsharp_desktop_remove_shortcut(info.project_name, error,
                                        sizeof(error)))
        fprintf(stderr, "shortcut warning: %s\n", error);
    zsharp_package_info_free(&info);
    puts("Uninstall completed.");
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc > 1 && strcmp(argv[1], "update-agent") == 0) {
        HWND console = GetConsoleWindow();
        if (console != NULL) ShowWindow(console, SW_HIDE);
        FreeConsole();
    } else if (argc > 1 && (strcmp(argv[1], "open-desktop") == 0 ||
        strcmp(argv[1], "open") == 0)) {
        DWORD console_processes[2];
        DWORD count = GetConsoleProcessList(console_processes, 2);
        if (strcmp(argv[1], "open-desktop") == 0 || count <= 1) {
            HWND console = GetConsoleWindow();
            if (console != NULL) ShowWindow(console, SW_HIDE);
            FreeConsole();
        }
    }
#endif
    if (!(argc > 1 && (strcmp(argv[1], "associate") == 0 ||
                       strcmp(argv[1], "update-agent") == 0)))
        zsharp_update_check_start();
    if (argc > 1 && strcmp(argv[1], "update-agent") == 0)
        return zsharp_update_agent_run();
    if (argc == 1) {
        char association_error[512] = {0};
        if (!zsharp_desktop_install_associations(association_error,
                                                  sizeof(association_error)))
            fprintf(stderr, "association warning: %s\n", association_error);
        show_hub_message("Welcome to the Z# Hub",
                         "Installed applications and games will appear here "
                         "in a future Hub library update.");
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("Z# %d.%d.%d.%d\n", ZSHARP_VERSION_MAJOR,
               ZSHARP_VERSION_MINOR, ZSHARP_VERSION_PATCH,
               ZSHARP_VERSION_REVISION);
        return 0;
    }
    if (strcmp(argv[1], "game-info") == 0) {
        printf("Z# game runtime: %s\n",
               zsharp_game_runtime_available() ? "available" : "unavailable");
        printf("Renderer: %s\n", zsharp_game_runtime_backend());
        printf("Dependency: zsharpgame:1.0.0.0\n");
        return zsharp_game_runtime_available() ? 0 : 1;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help();
        return 0;
    }
    if (strcmp(argv[1], "hub") == 0) {
        show_hub_message("Welcome to the Z# Hub",
                         "Installed applications and games will appear here "
                         "in a future Hub library update.");
        return 0;
    }
    if (strcmp(argv[1], "associate") == 0) {
        char error[512] = {0};
        if (!zsharp_desktop_install_associations(error, sizeof(error))) {
            fprintf(stderr, "association error: %s\n", error);
            return 1;
        }
#ifdef _WIN32
        if (!zsharp_update_agent_register(error, sizeof(error))) {
            fprintf(stderr, "update agent error: %s\n", error);
            return 1;
        }
        if (!zsharp_update_agent_start())
            fputs("update agent warning: it will start at your next sign-in\n",
                  stderr);
        else
            puts("Z# will check for updates from the system tray at sign-in.");
#endif
        puts("Z# now opens .zapp and .zgame files for this user.");
        return 0;
    }
    if (is_package_file(argv[1])) {
        return open_package_command(argv[1], argc, argv, 2);
    }
    if (strcmp(argv[1], "check") == 0) {
        if (argc != 3) {
            fputs("error: use 'zsharp check <file.zsharp>'\n", stderr);
            return 2;
        }
        return is_settings_file(argv[2])
            ? check_settings_command(argv[2])
            : check_command(argv[2]);
    }
    if (strcmp(argv[1], "compile") == 0) {
        if (argc != 5 || strcmp(argv[3], "-o") != 0) {
            fputs("error: use 'zsharp compile <file.zsharp> -o <output.zbc>'\n",
                  stderr);
            return 2;
        }
        return compile_command(argv[2], argv[4]);
    }
    if (strcmp(argv[1], "package") == 0) {
        int include_unbytecoded = argc == 6 &&
                                 strcmp(argv[5], "--unbytecode") == 0;
        if ((argc != 5 && argc != 6) || (argc == 6 && !include_unbytecoded)) {
            fputs("error: use 'zsharp package <app|game> <project> <filename> [--unbytecode]'\n",
                  stderr);
            return 2;
        }
        return package_command(argv[2], argv[3], argv[4],
                               include_unbytecoded);
    }
    if (strcmp(argv[1], "open") == 0 ||
        strcmp(argv[1], "open-desktop") == 0) {
        if (argc < 3 || !is_package_file(argv[2])) {
            fputs("error: use 'zsharp open <file.zapp|file.zgame>'\n", stderr);
            return 2;
        }
        return open_package_command(argv[2], argc, argv, 3);
    }
    if (strcmp(argv[1], "uninstall") == 0) {
        if (argc != 3 || !is_package_file(argv[2])) {
            fputs("error: use 'zsharp uninstall <file.zapp|file.zgame>'\n",
                  stderr);
            return 2;
        }
        return uninstall_package_command(argv[2]);
    }
    if (strcmp(argv[1], "project") == 0) {
        if (argc != 3) {
            fputs("error: use 'zsharp project <project-directory|project.zsettings>'\n",
                  stderr);
            return 2;
        }
        return project_command(argv[2]);
    }
    if (strcmp(argv[1], "check-bytecode") == 0) {
        if (argc != 3) {
            fputs("error: use 'zsharp check-bytecode <file.zbc>'\n", stderr);
            return 2;
        }
        return check_bytecode_command(argv[2]);
    }
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fputs("error: use 'zsharp run <file.zsharp|file.zapp|file.zgame>'\n",
                  stderr);
            return 2;
        }
        if (is_package_file(argv[2]))
            return open_package_command(argv[2], argc, argv, 3);
        if (is_settings_file(argv[2])) {
            fputs("error: project.zsettings cannot be run directly; use "
                  "'zsharp package app <project> <filename>' and then "
                  "'zsharp run <file.zapp>'\n",
                  stderr);
            return 2;
        }
        return run_source_command(argv[2], argc, argv, 3);
    }
    if (strcmp(argv[1], "run-bytecode") == 0) {
        if (argc < 3) {
            fputs("error: use 'zsharp run-bytecode <file.zbc>'\n", stderr);
            return 2;
        }
        return run_bytecode_command(argv[2], argc, argv, 3);
    }
    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    print_help();
    return 2;
}
