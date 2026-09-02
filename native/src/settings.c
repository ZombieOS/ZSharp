#include "settings.h"

#include "bytecode.h"
#include "lexer.h"
#include "zsharp.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SettingsParser {
    ZSharpLexer lexer;
    ZSharpToken current;
    int failed;
    ZSharpDiagnostic *diagnostic;
} SettingsParser;

static void settings_fail(SettingsParser *parser, const ZSharpToken *token,
                          const char *format, ...) {
    va_list arguments;
    if (parser->failed) return;
    parser->failed = 1;
    parser->diagnostic->line = token->line;
    parser->diagnostic->column = token->column;
    va_start(arguments, format);
    vsnprintf(parser->diagnostic->message,
              sizeof(parser->diagnostic->message), format, arguments);
    va_end(arguments);
}

static void settings_advance(SettingsParser *parser) {
    if (parser->failed) return;
    parser->current = zsharp_lexer_next(&parser->lexer);
    if (parser->current.type == ZTOKEN_ERROR) {
        settings_fail(parser, &parser->current, "unexpected character '%.*s'",
                      (int)parser->current.length, parser->current.start);
    }
}

static int settings_match_type(SettingsParser *parser,
                               ZSharpTokenType type) {
    if (parser->current.type != type) return 0;
    settings_advance(parser);
    return 1;
}

static int settings_match_word(SettingsParser *parser, const char *word) {
    if (!zsharp_token_equals(&parser->current, word)) return 0;
    settings_advance(parser);
    return 1;
}

static int settings_consume_type(SettingsParser *parser,
                                 ZSharpTokenType type,
                                 const char *description) {
    if (settings_match_type(parser, type)) return 1;
    settings_fail(parser, &parser->current, "expected %s", description);
    return 0;
}

static int settings_consume_word(SettingsParser *parser, const char *word) {
    if (settings_match_word(parser, word)) return 1;
    settings_fail(parser, &parser->current, "expected '%s'", word);
    return 0;
}

static char *settings_copy_token(const ZSharpToken *token) {
    return zsharp_copy_text(token->start, token->length);
}

static char *settings_decode_text(SettingsParser *parser,
                                  const ZSharpToken *token) {
    const char *input = token->start + 1;
    const char *end = token->start + token->length - 1;
    char *result = (char *)malloc(token->length);
    char *output = result;
    if (result == NULL) {
        settings_fail(parser, token, "out of memory");
        return NULL;
    }
    while (input < end) {
        if (*input == '\\' && input + 1 < end) {
            input++;
            if (*input == 'n') {
                *output++ = '\n';
            } else if (*input == 'r') {
                *output++ = '\r';
            } else if (*input == 't') {
                *output++ = '\t';
            } else if (*input == '"' || *input == '\\') {
                *output++ = *input;
            } else {
                free(result);
                settings_fail(parser, token,
                              "unsupported text escape '\\%c'", *input);
                return NULL;
            }
            input++;
        } else {
            *output++ = *input++;
        }
    }
    *output = '\0';
    return result;
}

static char *settings_consume_text(SettingsParser *parser,
                                   const char *description) {
    ZSharpToken token = parser->current;
    if (!settings_consume_type(parser, ZTOKEN_STRING, description)) {
        return NULL;
    }
    return settings_decode_text(parser, &token);
}

static int settings_number(SettingsParser *parser, uint32_t *value) {
    ZSharpToken token = parser->current;
    char *text;
    char *end;
    unsigned long parsed;
    if (!settings_consume_type(parser, ZTOKEN_NUMBER,
                               "a version number")) {
        return 0;
    }
    text = settings_copy_token(&token);
    if (text == NULL) {
        settings_fail(parser, &token, "out of memory");
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || parsed > UINT32_MAX) {
        free(text);
        settings_fail(parser, &token, "version number is too large");
        return 0;
    }
    free(text);
    *value = (uint32_t)parsed;
    return 1;
}

static int settings_version(SettingsParser *parser, uint32_t version[4],
                            int bracketed) {
    size_t index;
    if (bracketed &&
        !settings_consume_type(parser, ZTOKEN_LEFT_BRACKET,
                               "'[' before the version")) {
        return 0;
    }
    for (index = 0; index < ZSHARP_VERSION_PART_COUNT; index++) {
        if (!settings_number(parser, &version[index])) return 0;
        if (index + 1 < ZSHARP_VERSION_PART_COUNT &&
            !settings_consume_type(parser, ZTOKEN_DOT,
                                   "'.' in the four-part version")) {
            return 0;
        }
    }
    return !bracketed ||
           settings_consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                                 "']' after the version");
}

static int append_author(SettingsParser *parser, ZSharpSettings *settings,
                         char *author) {
    char **resized = (char **)realloc(
        settings->authors,
        (settings->author_count + 1) * sizeof(*settings->authors));
    if (resized == NULL) {
        free(author);
        settings_fail(parser, &parser->current, "out of memory");
        return 0;
    }
    settings->authors = resized;
    settings->authors[settings->author_count++] = author;
    return 1;
}

static int append_dependency(SettingsParser *parser,
                             ZSharpSettings *settings, char *project_id,
                             const uint32_t version[4]) {
    ZSharpDependency *resized;
    size_t index;
    if (settings->project_id != NULL &&
        strcmp(settings->project_id, project_id) == 0) {
        settings_fail(parser, &parser->current,
                      "project '%s' cannot depend on itself", project_id);
        free(project_id);
        return 0;
    }
    for (index = 0; index < settings->dependency_count; index++) {
        if (strcmp(settings->dependencies[index].project_id,
                   project_id) == 0) {
            settings_fail(parser, &parser->current,
                          "duplicate dependency '%s'", project_id);
            free(project_id);
            return 0;
        }
    }
    resized = (ZSharpDependency *)realloc(
        settings->dependencies,
        (settings->dependency_count + 1) * sizeof(*settings->dependencies));
    if (resized == NULL) {
        free(project_id);
        settings_fail(parser, &parser->current, "out of memory");
        return 0;
    }
    settings->dependencies = resized;
    settings->dependencies[settings->dependency_count].project_id = project_id;
    for (index = 0; index < 4; index++) {
        settings->dependencies[settings->dependency_count].version[index] =
            version[index];
    }
    settings->dependency_count++;
    return 1;
}

static int normalize_project_id(SettingsParser *parser, char *project_id) {
    size_t index;
    if (project_id[0] == '\0') {
        settings_fail(parser, &parser->current, "PID cannot be empty");
        return 0;
    }
    for (index = 0; project_id[index] != '\0'; index++) {
        unsigned char character = (unsigned char)project_id[index];
        if (character == ' ') {
            project_id[index] = '_';
        } else if (isupper(character)) {
            settings_fail(parser, &parser->current,
                          "PID must use lowercase letters");
            return 0;
        } else if (!islower(character) && !isdigit(character) &&
                   character != '_') {
            settings_fail(parser, &parser->current,
                          "PID may contain lowercase letters, numbers, and "
                          "underscores only");
            return 0;
        }
    }
    return 1;
}

static int validate_window_path(SettingsParser *parser, const char *name,
                                const char *path) {
    const char *segment = path;
    const char *cursor = path;
    size_t length = strlen(path);
    if (length == 0) {
        settings_fail(parser, &parser->current,
                      "Window %s path cannot be empty", name);
        return 0;
    }
    if (path[0] == '/' || strchr(path, '\\') != NULL ||
        strchr(path, ':') != NULL) {
        settings_fail(parser, &parser->current,
                      "Window %s must be a project-relative path using '/'",
                      name);
        return 0;
    }
    while (1) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t segment_length = (size_t)(cursor - segment);
            if (segment_length == 0 ||
                (segment_length == 1 && segment[0] == '.') ||
                (segment_length == 2 && segment[0] == '.' &&
                 segment[1] == '.')) {
                settings_fail(parser, &parser->current,
                              "Window %s path cannot contain empty, '.', or "
                              "'..' segments", name);
                return 0;
            }
            if (*cursor == '\0') break;
            segment = cursor + 1;
        }
        cursor++;
    }
    if (length < strlen(ZSHARP_SOURCE_EXTENSION) ||
        strcmp(path + length - strlen(ZSHARP_SOURCE_EXTENSION),
               ZSHARP_SOURCE_EXTENSION) != 0) {
        settings_fail(parser, &parser->current,
                      "Window %s must identify a .zsharp file", name);
        return 0;
    }
    return 1;
}

static int icon_extension(const char *path) {
    static const char *extensions[] = {".png"};
    size_t path_length = strlen(path);
    size_t index;
    for (index = 0; index < sizeof(extensions) / sizeof(extensions[0]); index++) {
        const char *extension = extensions[index];
        size_t extension_length = strlen(extension);
        size_t character;
        if (path_length < extension_length) continue;
        for (character = 0; character < extension_length; character++) {
            if (tolower((unsigned char)path[path_length - extension_length +
                                                character]) !=
                tolower((unsigned char)extension[character])) break;
        }
        if (character == extension_length) return 1;
    }
    return 0;
}

static int validate_icon_path(SettingsParser *parser, const char *path) {
    const char *segment = path;
    const char *cursor = path;
    if (path[0] == '\0') {
        settings_fail(parser, &parser->current, "Icon path cannot be empty");
        return 0;
    }
    if (path[0] == '/' || strchr(path, '\\') != NULL ||
        strchr(path, ':') != NULL) {
        settings_fail(parser, &parser->current,
                      "Icon must be a project-relative path using '/'");
        return 0;
    }
    while (1) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - segment);
            if (length == 0 ||
                (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.')) {
                settings_fail(parser, &parser->current,
                              "Icon path cannot contain empty, '.', or '..' segments");
                return 0;
            }
            if (*cursor == '\0') break;
            segment = cursor + 1;
        }
        cursor++;
    }
    if (!icon_extension(path)) {
        settings_fail(parser, &parser->current,
                      "Icon must identify a PNG image");
        return 0;
    }
    return 1;
}

static char *read_settings_source(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *source;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    source = (char *)malloc((size_t)size + 1);
    if (source == NULL) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source);
        fclose(file);
        return NULL;
    }
    source[size] = '\0';
    fclose(file);
    return source;
}

void zsharp_settings_init(ZSharpSettings *settings) {
    memset(settings, 0, sizeof(*settings));
}

void zsharp_settings_free(ZSharpSettings *settings) {
    size_t index;
    free(settings->project_name);
    free(settings->project_id);
    for (index = 0; index < settings->author_count; index++) {
        free(settings->authors[index]);
    }
    free(settings->authors);
    free(settings->description);
    free(settings->icon);
    for (index = 0; index < settings->dependency_count; index++) {
        free(settings->dependencies[index].project_id);
    }
    free(settings->dependencies);
    free(settings->window_startup);
    free(settings->window_uninstall);
    zsharp_settings_init(settings);
}

int zsharp_settings_parse_source(const char *source, ZSharpSettings *settings,
                                 ZSharpDiagnostic *diagnostic, char *error,
                                 size_t error_size) {
    SettingsParser parser;
    unsigned seen = 0;
    zsharp_settings_init(settings);
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (source == NULL) {
        snprintf(error, error_size, "settings source is missing");
        return 0;
    }
    memset(&parser, 0, sizeof(parser));
    parser.diagnostic = diagnostic;
    zsharp_lexer_init(&parser.lexer, source);
    settings_advance(&parser);
    settings_consume_word(&parser, "zsharp");
    settings_consume_type(&parser, ZTOKEN_EQUAL, "'=' after 'zsharp'");
    settings_consume_word(&parser, "type");
    settings_consume_type(&parser, ZTOKEN_DOT, "'.' after 'type'");
    settings_consume_word(&parser, "settings");
    while (!parser.failed && parser.current.type != ZTOKEN_EOF) {
        if (settings_match_word(&parser, "Project")) {
            if ((seen & 1u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Project setting");
                break;
            }
            seen |= 1u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'Project'");
            settings->project_name =
                settings_consume_text(&parser, "a project display name");
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the project display name");
        } else if (settings_match_word(&parser, "PID")) {
            if ((seen & 2u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate PID setting");
                break;
            }
            seen |= 2u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'PID'");
            settings->project_id =
                settings_consume_text(&parser, "a project ID");
            if (settings->project_id != NULL) {
                normalize_project_id(&parser, settings->project_id);
            }
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the project ID");
        } else if (settings_match_word(&parser, "Version")) {
            if ((seen & 4u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Version setting");
                break;
            }
            seen |= 4u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'Version'");
            settings_version(&parser, settings->version, 1);
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the project version");
        } else if (settings_match_word(&parser, "Authors")) {
            if ((seen & 8u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Authors setting");
                break;
            }
            seen |= 8u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'Authors'");
            settings_consume_type(&parser, ZTOKEN_LEFT_BRACKET,
                                  "'[' before the authors");
            if (parser.current.type != ZTOKEN_RIGHT_BRACKET) {
                do {
                    char *author = settings_consume_text(
                        &parser, "a quoted author name");
                    if (author == NULL ||
                        !append_author(&parser, settings, author)) {
                        break;
                    }
                } while (settings_match_type(&parser, ZTOKEN_COMMA));
            }
            settings_consume_type(&parser, ZTOKEN_RIGHT_BRACKET,
                                  "']' after the authors");
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the authors");
        } else if (settings_match_word(&parser, "Description")) {
            if ((seen & 16u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Description setting");
                break;
            }
            seen |= 16u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'Description'");
            settings->description =
                settings_consume_text(&parser, "a project description");
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the project description");
        } else if (settings_match_word(&parser, "Icon")) {
            if ((seen & 256u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Icon setting");
                break;
            }
            seen |= 256u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'Icon'");
            settings->icon = settings_consume_text(
                &parser, "a quoted project icon path");
            if (settings->icon != NULL)
                validate_icon_path(&parser, settings->icon);
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the project icon path");
        } else if (settings_match_word(&parser, "ZSharp")) {
            if ((seen & 32u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate ZSharp setting");
                break;
            }
            seen |= 32u;
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after 'ZSharp'");
            settings_version(&parser, settings->zsharp_version, 1);
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after the Z# version");
        } else if (settings_match_word(&parser, "Dependencies")) {
            if ((seen & 64u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Dependencies setting");
                break;
            }
            seen |= 64u;
            settings_consume_type(&parser, ZTOKEN_LEFT_PAREN,
                                  "'(' before dependencies");
            while (!parser.failed &&
                   parser.current.type != ZTOKEN_RIGHT_PAREN) {
                ZSharpToken id_token = parser.current;
                char *project_id;
                uint32_t version[4] = {0};
                if (!settings_consume_type(&parser, ZTOKEN_IDENTIFIER,
                                           "a dependency project ID")) {
                    break;
                }
                project_id = settings_copy_token(&id_token);
                if (project_id == NULL) {
                    settings_fail(&parser, &id_token, "out of memory");
                    break;
                }
                settings_consume_type(&parser, ZTOKEN_COLON,
                                      "':' after the dependency project ID");
                if (!normalize_project_id(&parser, project_id) ||
                    !settings_version(&parser, version, 0)) {
                    free(project_id);
                    break;
                }
                if (!append_dependency(&parser, settings, project_id,
                                       version)) break;
                settings_match_type(&parser, ZTOKEN_COMMA);
            }
            settings_consume_type(&parser, ZTOKEN_RIGHT_PAREN,
                                  "')' after dependencies");
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after Dependencies");
        } else if (settings_match_word(&parser, "Window")) {
            unsigned window_seen = 0;
            if ((seen & 128u) != 0) {
                settings_fail(&parser, &parser.current,
                              "duplicate Window setting");
                break;
            }
            seen |= 128u;
            settings->has_window = 1;
            settings_consume_type(&parser, ZTOKEN_LEFT_PAREN,
                                  "'(' before Window settings");
            while (!parser.failed &&
                   parser.current.type != ZTOKEN_RIGHT_PAREN) {
                if (settings_match_word(&parser, "Startup")) {
                    if ((window_seen & 1u) != 0) {
                        settings_fail(&parser, &parser.current,
                                      "duplicate Window Startup setting");
                        break;
                    }
                    window_seen |= 1u;
                    settings_consume_type(&parser, ZTOKEN_COLON,
                                          "':' after 'Startup'");
                    settings->window_startup = settings_consume_text(
                        &parser, "a quoted startup window path");
                    if (settings->window_startup != NULL) {
                        validate_window_path(&parser, "Startup",
                                             settings->window_startup);
                    }
                    settings_consume_type(&parser, ZTOKEN_COLON,
                                          "':' after the Startup path");
                } else if (settings_match_word(&parser, "Uninstall")) {
                    if ((window_seen & 2u) != 0) {
                        settings_fail(&parser, &parser.current,
                                      "duplicate Window Uninstall setting");
                        break;
                    }
                    window_seen |= 2u;
                    settings_consume_type(&parser, ZTOKEN_COLON,
                                          "':' after 'Uninstall'");
                    settings->window_uninstall = settings_consume_text(
                        &parser, "a quoted uninstall window path");
                    if (settings->window_uninstall != NULL) {
                        validate_window_path(&parser, "Uninstall",
                                             settings->window_uninstall);
                    }
                    settings_consume_type(&parser, ZTOKEN_COLON,
                                          "':' after the Uninstall path");
                } else {
                    settings_fail(&parser, &parser.current,
                                  "unknown Window setting '%.*s'",
                                  (int)parser.current.length,
                                  parser.current.start);
                }
            }
            settings_consume_type(&parser, ZTOKEN_RIGHT_PAREN,
                                  "')' after Window settings");
            settings_consume_type(&parser, ZTOKEN_COLON,
                                  "':' after Window settings");
            if (!parser.failed && window_seen != 3u) {
                settings_fail(&parser, &parser.current,
                              "Window must define Startup and Uninstall");
            }
        } else {
            settings_fail(&parser, &parser.current,
                          "unknown project setting '%.*s'",
                          (int)parser.current.length, parser.current.start);
        }
    }
    if (!parser.failed && (seen & 127u) != 127u) {
        settings_fail(&parser, &parser.current,
                      "project.zsettings must define Project, PID, Version, "
                      "Authors, Description, ZSharp, and Dependencies");
    }
    if (!parser.failed) {
        size_t dependency_index;
        for (dependency_index = 0;
             dependency_index < settings->dependency_count;
             dependency_index++) {
            if (strcmp(settings->project_id,
                       settings->dependencies[dependency_index].project_id) ==
                0) {
                settings_fail(&parser, &parser.current,
                              "project '%s' cannot depend on itself",
                              settings->project_id);
                break;
            }
        }
    }
    if (!parser.failed &&
        settings->zsharp_version[0] == 0) {
        settings_fail(&parser, &parser.current,
                      "the Z# generation must be Z1 or newer");
    }
    if (!parser.failed &&
        settings->zsharp_version[0] > ZSHARP_CURRENT_GENERATION) {
        settings_fail(&parser, &parser.current,
                      "this compiler supports through Z%u, but the project "
                      "requires Z%u",
                      ZSHARP_CURRENT_GENERATION,
                      settings->zsharp_version[0]);
    }
    if (!parser.failed && settings->has_window) {
        size_t dependency_index;
        int found = 0;
        for (dependency_index = 0;
             dependency_index < settings->dependency_count;
             dependency_index++) {
            if (strcmp(settings->dependencies[dependency_index].project_id,
                       "zsharpwindow") == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            settings_fail(&parser, &parser.current,
                          "Window settings require the zsharpwindow "
                          "dependency");
        }
    }
    if (parser.failed) {
        zsharp_settings_free(settings);
        return 0;
    }
    return 1;
}

int zsharp_settings_parse_file(const char *path, ZSharpSettings *settings,
                               ZSharpDiagnostic *diagnostic, char *error,
                               size_t error_size) {
    char *source = read_settings_source(path);
    int ok;
    if (source == NULL) {
        zsharp_settings_init(settings);
        memset(diagnostic, 0, sizeof(*diagnostic));
        snprintf(error, error_size, "could not read '%s'", path);
        return 0;
    }
    ok = zsharp_settings_parse_source(source, settings, diagnostic, error,
                                      error_size);
    free(source);
    return ok;
}

static char *settings_path_join(const char *root, const char *name) {
    size_t root_length = strlen(root);
    size_t name_length = strlen(name);
    int separator = root_length > 0 && root[root_length - 1] != '/' &&
                    root[root_length - 1] != '\\';
    char *path = (char *)malloc(root_length + (size_t)separator +
                               name_length + 1);
    if (path == NULL) return NULL;
    memcpy(path, root, root_length);
    if (separator) {
#ifdef _WIN32
        path[root_length++] = '\\';
#else
        path[root_length++] = '/';
#endif
    }
    memcpy(path + root_length, name, name_length + 1);
    return path;
}

int zsharp_settings_load(const char *project_root, ZSharpSettings *settings,
                         ZSharpDiagnostic *diagnostic, char *error,
                         size_t error_size) {
    char *path = settings_path_join(project_root, ZSHARP_SETTINGS_FILE);
    int ok;
    if (path == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    ok = zsharp_settings_parse_file(path, settings, diagnostic, error,
                                    error_size);
    free(path);
    return ok;
}

const ZSharpDependency *zsharp_settings_find_dependency(
    const ZSharpSettings *settings, const char *project_id) {
    size_t index;
    for (index = 0; index < settings->dependency_count; index++) {
        if (strcmp(settings->dependencies[index].project_id, project_id) == 0) {
            return &settings->dependencies[index];
        }
    }
    return NULL;
}
