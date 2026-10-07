#ifndef ZSHARP_PROJECT_H
#define ZSHARP_PROJECT_H

#include "bytecode.h"
#include "parser.h"
#include "settings.h"

#include <stddef.h>

char *zsharp_project_current_directory(char *error, size_t error_size);

/* Finds the nearest parent containing project.zsettings. start_path may be a
 * source/bytecode file, the settings file itself, or a project directory. */
char *zsharp_project_find_root(const char *start_path, char *error,
                               size_t error_size);

/* Installed source dependency roots are scoped to the declaring project. */
char *zsharp_project_dependency_root(const char *project_root, const char *project_id,
                                     char *error, size_t error_size);
int zsharp_project_foreign_root(const char *project_root, const char *module,
    char **resolved_root, const char **local_module, char *error, size_t error_size);

int zsharp_project_find_source(const char *project_root, const char *file_name,
                               char **source_path, char *error,
                               size_t error_size);

typedef struct ZSharpSourceList {
    char **items;
    size_t count;
} ZSharpSourceList;

int zsharp_project_list_sources(const char *project_root,
                                ZSharpSourceList *sources,
                                char *error, size_t error_size);
int zsharp_project_search_files(const char *project_root, const char *extension,
                               ZSharpSourceList *sources, char *error, size_t error_size);
int zsharp_project_list_files(const char *project_root, const char *extension,
                              ZSharpSourceList *files,
                              char *error, size_t error_size);
void zsharp_project_source_list_free(ZSharpSourceList *sources);

int zsharp_project_parse_file(const char *path, ZSharpProgram *program,
                              ZSharpDiagnostic *diagnostic, char *error,
                              size_t error_size);

int zsharp_project_validate(const ZSharpProgram *program,
                            const ZSharpSettings *settings,
                            const char *project_root, char *error,
                            size_t error_size);

/* Initialize to zero. A context is scoped to one immutable validation/build
 * operation, never reused across builds or shared between threads. */
typedef struct ZSharpValidationContext {
    struct ZSharpGameModel *game;
    size_t game_model_loads;
} ZSharpValidationContext;

int zsharp_project_validate_with_context(const ZSharpProgram *program,
                            const ZSharpSettings *settings,
                            const char *project_root,
                            ZSharpValidationContext *context,
                            char *error, size_t error_size);
int zsharp_project_validation_game(ZSharpValidationContext *context,
                            const char *project_root,
                            char *error, size_t error_size);
void zsharp_project_validation_free(ZSharpValidationContext *context);

int zsharp_project_authorize_call(const ZSharpProgram *program,
                                 const ZSharpSettings *settings,
                                 const ZSharpRoom *room,
                                 const ZSharpInstruction *target,
                                 char *error, size_t error_size);

int zsharp_project_validate_settings(const ZSharpSettings *settings,
                                     const char *project_root, char *error,
                                     size_t error_size);

#endif
