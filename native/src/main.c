#include "zsharp.h"

#include "bytecode.h"
#include "project.h"
#include "provider_loader.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void) {
    puts("Z# toolchain");
    puts("");
    puts("Usage:");
    puts("  zsharp --version");
    puts("  zsharp check <file.zsharp|project.zsettings>");
    puts("  zsharp compile <file.zsharp> -o <output.zbc>");
    puts("  zsharp run <file.zsharp> [--provider Project=library]...");
    puts("  zsharp run-bytecode <file.zbc> [--provider Project=library]...");
}

static int has_source_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension != NULL && strcmp(extension, ZSHARP_SOURCE_EXTENSION) == 0;
}

static int is_settings_file(const char *path) {
    const char *name = path;
    const char *cursor;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return strcmp(name, ZSHARP_SETTINGS_FILE) == 0;
}

static int parse_file(const char *path, ZSharpProgram *program) {
    ZSharpDiagnostic diagnostic;
    char error[256] = {0};
    int ok;
    if (!has_source_extension(path)) {
        fprintf(stderr, "error: Z# source files must use the %s extension\n",
                ZSHARP_SOURCE_EXTENSION);
        return 0;
    }
    ok = zsharp_project_parse_file(path, program, &diagnostic, error,
                                   sizeof(error));
    if (!ok) {
        if (diagnostic.message[0] != '\0') {
            fprintf(stderr, "%s:%u:%u: error: %s\n", path, diagnostic.line,
                    diagnostic.column, diagnostic.message);
        } else {
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
        return 1;
    }
    if (diagnostic.message[0] != '\0') {
        fprintf(stderr, "%s:%u:%u: compile error: %s\n",
                ZSHARP_SETTINGS_FILE, diagnostic.line, diagnostic.column,
                diagnostic.message);
    } else {
        fprintf(stderr, "compile error: %s\n", error);
    }
    return 0;
}

static int check_command(const char *source_path) {
    ZSharpProgram program;
    char error[512] = {0};
    char *project_root;
    ZSharpSettings settings;
    if (!parse_file(source_path, &program)) {
        return 1;
    }
    project_root = zsharp_project_current_directory(error, sizeof(error));
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
    project_root = zsharp_project_current_directory(error, sizeof(error));
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
    if (!parse_file(source_path, &program)) {
        return 1;
    }
    project_root = zsharp_project_current_directory(error, sizeof(error));
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
    project_root = zsharp_project_current_directory(error, sizeof(error));
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

int main(int argc, char **argv) {
    if (argc == 1) {
        print_help();
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("Z# %d.%d.%d.%d\n", ZSHARP_VERSION_MAJOR,
               ZSHARP_VERSION_MINOR, ZSHARP_VERSION_PATCH,
               ZSHARP_VERSION_REVISION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help();
        return 0;
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
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fputs("error: use 'zsharp run <file.zsharp>'\n", stderr);
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
