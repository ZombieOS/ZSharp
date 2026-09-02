#define _CRT_SECURE_NO_WARNINGS

#include "game_model.h"

#include "lexer.h"
#include "project.h"
#include "zsharp.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum ModelValueType {
    MODEL_IDENTIFIER,
    MODEL_TEXT,
    MODEL_NUMBER,
    MODEL_COLOR
} ModelValueType;

typedef struct ModelValue {
    ModelValueType type;
    char *text;
    unsigned line;
    unsigned column;
} ModelValue;

typedef struct ModelParser {
    ZSharpLexer lexer;
    ZSharpToken current;
    const char *path;
    const char *source_name;
    ZSharpGameModel *model;
    int file_is_3d;
    char *error;
    size_t error_size;
    int failed;
} ModelParser;

static void model_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s", message == NULL ? "game error"
                                                            : message);
}

static void parser_fail(ModelParser *parser, const ZSharpToken *token,
                        const char *message) {
    if (parser->failed) return;
    parser->failed = 1;
    if (parser->error != NULL && parser->error_size != 0)
        snprintf(parser->error, parser->error_size, "%s:%u:%u: %s",
                 parser->path, token->line, token->column, message);
}

static void parser_advance(ModelParser *parser) {
    if (parser->failed) return;
    parser->current = zsharp_lexer_next(&parser->lexer);
    if (parser->current.type == ZTOKEN_ERROR)
        parser_fail(parser, &parser->current,
                    parser->current.start[0] == '"'
                        ? "unterminated text value"
                        : "unexpected character in game object");
}

static int parser_match_type(ModelParser *parser, ZSharpTokenType type) {
    if (parser->current.type != type) return 0;
    parser_advance(parser);
    return 1;
}

static int parser_match_word(ModelParser *parser, const char *word) {
    if (!zsharp_token_equals(&parser->current, word)) return 0;
    parser_advance(parser);
    return 1;
}

static int parser_expect_type(ModelParser *parser, ZSharpTokenType type,
                              const char *message) {
    if (parser_match_type(parser, type)) return 1;
    parser_fail(parser, &parser->current, message);
    return 0;
}

static int parser_expect_word(ModelParser *parser, const char *word) {
    char message[128];
    if (parser_match_word(parser, word)) return 1;
    snprintf(message, sizeof(message), "expected '%s'", word);
    parser_fail(parser, &parser->current, message);
    return 0;
}

static char *token_copy(const ZSharpToken *token) {
    return zsharp_copy_text(token->start, token->length);
}

static char *parser_name(ModelParser *parser, const char *description) {
    ZSharpToken token = parser->current;
    char message[160];
    char *copy;
    if (token.type != ZTOKEN_IDENTIFIER) {
        snprintf(message, sizeof(message), "expected %s", description);
        parser_fail(parser, &token, message);
        return NULL;
    }
    copy = token_copy(&token);
    if (copy == NULL) {
        parser_fail(parser, &token, "out of memory");
        return NULL;
    }
    parser_advance(parser);
    return copy;
}

static int read_file(const char *path, char **text, char *error,
                     size_t error_size) {
    FILE *file = fopen(path, "rb");
    long length;
    char *buffer;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not read '%s'", path);
        return 0;
    }
    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        model_error(error, error_size, "out of memory");
        return 0;
    }
    if ((length != 0 && fread(buffer, 1, (size_t)length, file) !=
                            (size_t)length) || fclose(file) != 0) {
        free(buffer);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "could not read '%s'", path);
        return 0;
    }
    buffer[length] = '\0';
    *text = buffer;
    return 1;
}

static int parse_value(ModelParser *parser, ModelValue *value) {
    ZSharpToken token = parser->current;
    char *first;
    memset(value, 0, sizeof(*value));
    value->line = token.line;
    value->column = token.column;
    if (token.type == ZTOKEN_STRING) {
        value->type = MODEL_TEXT;
        value->text = zsharp_copy_text(token.start + 1, token.length - 2);
        parser_advance(parser);
    } else if (token.type == ZTOKEN_COLOR) {
        value->type = MODEL_COLOR;
        value->text = token_copy(&token);
        parser_advance(parser);
    } else if (token.type == ZTOKEN_IDENTIFIER) {
        value->type = MODEL_IDENTIFIER;
        value->text = token_copy(&token);
        parser_advance(parser);
    } else if (token.type == ZTOKEN_NUMBER || token.type == ZTOKEN_MINUS) {
        ZSharpToken whole;
        size_t length;
        int negative = parser_match_type(parser, ZTOKEN_MINUS);
        if (parser->current.type != ZTOKEN_NUMBER) {
            parser_fail(parser, &parser->current,
                        "expected a number after '-'");
            return 0;
        }
        whole = parser->current;
        parser_advance(parser);
        first = token_copy(&whole);
        if (first == NULL) goto out_of_memory;
        length = strlen(first) + (size_t)negative;
        if (parser_match_type(parser, ZTOKEN_DOT)) {
            ZSharpToken fraction = parser->current;
            char *combined;
            if (fraction.type != ZTOKEN_NUMBER) {
                free(first);
                parser_fail(parser, &fraction,
                            "expected digits after the decimal point");
                return 0;
            }
            combined = (char *)malloc(length + fraction.length + 2);
            if (combined == NULL) {
                free(first);
                goto out_of_memory;
            }
            snprintf(combined, length + fraction.length + 2, "%s%s.%.*s",
                     negative ? "-" : "", first, (int)fraction.length,
                     fraction.start);
            free(first);
            value->text = combined;
            parser_advance(parser);
        } else {
            value->text = (char *)malloc(length + 1);
            if (value->text == NULL) {
                free(first);
                goto out_of_memory;
            }
            snprintf(value->text, length + 1, "%s%s",
                     negative ? "-" : "", first);
            free(first);
        }
        value->type = MODEL_NUMBER;
    } else {
        parser_fail(parser, &token,
                    "expected text, number, status, color, or identifier");
        return 0;
    }
    if (value->text == NULL) goto out_of_memory;
    return 1;
out_of_memory:
    parser_fail(parser, &token, "out of memory");
    return 0;
}

static int value_number(ModelParser *parser, const ModelValue *value,
                        float *number) {
    char *end = NULL;
    double parsed;
    if (value->type != MODEL_NUMBER) {
        parser_fail(parser, &parser->current, "field requires a number");
        return 0;
    }
    parsed = strtod(value->text, &end);
    if (end == value->text || end == NULL || *end != '\0' ||
        !isfinite(parsed) || fabs(parsed) > 1000000000.0) {
        parser_fail(parser, &parser->current, "invalid game number");
        return 0;
    }
    *number = (float)parsed;
    return 1;
}

static int value_status(ModelParser *parser, const ModelValue *value,
                        int *status) {
    if (value->type != MODEL_IDENTIFIER ||
        (strcmp(value->text, "alive") != 0 &&
         strcmp(value->text, "dead") != 0)) {
        parser_fail(parser, &parser->current,
                    "field requires alive or dead");
        return 0;
    }
    *status = strcmp(value->text, "alive") == 0;
    return 1;
}

static int value_color(ModelParser *parser, const ModelValue *value,
                       unsigned *color) {
    char *end = NULL;
    unsigned long parsed;
    if (value->type != MODEL_COLOR || strlen(value->text) != 7) {
        parser_fail(parser, &parser->current,
                    "color fields require #RRGGBB");
        return 0;
    }
    parsed = strtoul(value->text + 1, &end, 16);
    if (end == NULL || *end != '\0') {
        parser_fail(parser, &parser->current,
                    "color fields require #RRGGBB");
        return 0;
    }
    *color = (unsigned)parsed;
    return 1;
}

static int replace_text(char **target, const char *value) {
    char *copy = zsharp_copy_text(value, strlen(value));
    if (copy == NULL) return 0;
    free(*target);
    *target = copy;
    return 1;
}

static ZSharpGameScene *add_scene(ModelParser *parser, char *name) {
    ZSharpGameScene *resized;
    ZSharpGameScene *scene;
    size_t index;
    for (index = 0; index < parser->model->scene_count; index++) {
        if (strcmp(parser->model->scenes[index].name, name) == 0) {
            free(name);
            parser_fail(parser, &parser->current, "duplicate scene name");
            return NULL;
        }
    }
    resized = (ZSharpGameScene *)realloc(
        parser->model->scenes,
        (parser->model->scene_count + 1) * sizeof(*resized));
    if (resized == NULL) {
        free(name);
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    parser->model->scenes = resized;
    scene = &resized[parser->model->scene_count++];
    memset(scene, 0, sizeof(*scene));
    scene->name = name;
    scene->background = 0x08080bu;
    scene->gravity_y = -900.0f;
    scene->camera_z = parser->file_is_3d ? 8.0f : 0.0f;
    scene->camera_fov = 70.0f;
    return scene;
}

static ZSharpGameObject *add_object(ModelParser *parser, char *name) {
    ZSharpGameObject *resized;
    ZSharpGameObject *object;
    size_t index;
    for (index = 0; index < parser->model->object_count; index++) {
        if (strcmp(parser->model->objects[index].name, name) == 0) {
            free(name);
            parser_fail(parser, &parser->current,
                        "game object names must be unique across the project");
            return NULL;
        }
    }
    resized = (ZSharpGameObject *)realloc(
        parser->model->objects,
        (parser->model->object_count + 1) * sizeof(*resized));
    if (resized == NULL) {
        free(name);
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    parser->model->objects = resized;
    object = &resized[parser->model->object_count++];
    memset(object, 0, sizeof(*object));
    object->name = name;
    object->source_file = zsharp_copy_text(parser->source_name,
                                            strlen(parser->source_name));
    if (object->source_file == NULL) {
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    object->shape = parser->file_is_3d ? ZGAME_SHAPE_CUBE
                                      : ZGAME_SHAPE_RECTANGLE;
    object->body = ZGAME_BODY_STATIC;
    object->collider = ZGAME_COLLIDER_NONE;
    object->width = parser->file_is_3d ? 1.0f : 64.0f;
    object->height = object->width;
    object->depth = object->width;
    object->scale_x = object->scale_y = object->scale_z = 1.0f;
    object->mass = 1.0f;
    object->gravity_scale = 1.0f;
    object->friction = 0.2f;
    object->color = 0xffffffu;
    object->visible = 1;
    object->audio_volume = 1.0f;
    object->tone_duration = 0.12f;
    return object;
}

static int apply_scene_field(ModelParser *parser, ZSharpGameScene *scene,
                             const char *field, const ModelValue *value) {
    if (strcmp(field, "background") == 0)
        return value_color(parser, value, &scene->background);
    if (strcmp(field, "gravityX") == 0)
        return value_number(parser, value, &scene->gravity_x);
    if (strcmp(field, "gravityY") == 0)
        return value_number(parser, value, &scene->gravity_y);
    if (strcmp(field, "gravityZ") == 0)
        return value_number(parser, value, &scene->gravity_z);
    if (strcmp(field, "cameraX") == 0)
        return value_number(parser, value, &scene->camera_x);
    if (strcmp(field, "cameraY") == 0)
        return value_number(parser, value, &scene->camera_y);
    if (strcmp(field, "cameraZ") == 0)
        return value_number(parser, value, &scene->camera_z);
    if (strcmp(field, "cameraFov") == 0)
        return value_number(parser, value, &scene->camera_fov);
    parser_fail(parser, &parser->current, "unknown scene field");
    return 0;
}

static int apply_object_field(ModelParser *parser, ZSharpGameObject *object,
                              const char *field, const ModelValue *value) {
    float number;
    if (strcmp(field, "scene") == 0) {
        if (value->type != MODEL_IDENTIFIER && value->type != MODEL_TEXT)
            goto needs_identifier;
        return replace_text(&object->scene, value->text);
    }
    if (strcmp(field, "shape") == 0) {
        if (value->type != MODEL_IDENTIFIER) goto needs_identifier;
        if (strcmp(value->text, "rectangle") == 0)
            object->shape = ZGAME_SHAPE_RECTANGLE;
        else if (strcmp(value->text, "circle") == 0)
            object->shape = ZGAME_SHAPE_CIRCLE;
        else if (strcmp(value->text, "triangle") == 0)
            object->shape = ZGAME_SHAPE_TRIANGLE;
        else if (strcmp(value->text, "sprite") == 0)
            object->shape = ZGAME_SHAPE_SPRITE;
        else if (strcmp(value->text, "cube") == 0)
            object->shape = ZGAME_SHAPE_CUBE;
        else if (strcmp(value->text, "text") == 0)
            object->shape = ZGAME_SHAPE_TEXT;
        else {
            parser_fail(parser, &parser->current,
                        "shape must be rectangle, circle, triangle, sprite, cube, or text");
            return 0;
        }
        return 1;
    }
    if (strcmp(field, "body") == 0) {
        if (value->type != MODEL_IDENTIFIER) goto needs_identifier;
        if (strcmp(value->text, "static") == 0)
            object->body = ZGAME_BODY_STATIC;
        else if (strcmp(value->text, "dynamic") == 0)
            object->body = ZGAME_BODY_DYNAMIC;
        else if (strcmp(value->text, "kinematic") == 0)
            object->body = ZGAME_BODY_KINEMATIC;
        else {
            parser_fail(parser, &parser->current,
                        "body must be static, dynamic, or kinematic");
            return 0;
        }
        return 1;
    }
    if (strcmp(field, "collider") == 0) {
        if (value->type != MODEL_IDENTIFIER) goto needs_identifier;
        if (strcmp(value->text, "none") == 0)
            object->collider = ZGAME_COLLIDER_NONE;
        else if (strcmp(value->text, "box") == 0)
            object->collider = ZGAME_COLLIDER_BOX;
        else if (strcmp(value->text, "circle") == 0)
            object->collider = ZGAME_COLLIDER_CIRCLE;
        else {
            parser_fail(parser, &parser->current,
                        "collider must be none, box, or circle");
            return 0;
        }
        return 1;
    }
#define NUMBER_FIELD(name, member)                                             \
    if (strcmp(field, name) == 0)                                              \
        return value_number(parser, value, &object->member)
    NUMBER_FIELD("positionX", x);
    NUMBER_FIELD("positionY", y);
    NUMBER_FIELD("positionZ", z);
    NUMBER_FIELD("width", width);
    NUMBER_FIELD("height", height);
    NUMBER_FIELD("depth", depth);
    NUMBER_FIELD("rotation", rotation);
    NUMBER_FIELD("scaleX", scale_x);
    NUMBER_FIELD("scaleY", scale_y);
    NUMBER_FIELD("scaleZ", scale_z);
    NUMBER_FIELD("velocityX", velocity_x);
    NUMBER_FIELD("velocityY", velocity_y);
    NUMBER_FIELD("velocityZ", velocity_z);
    NUMBER_FIELD("mass", mass);
    NUMBER_FIELD("gravityScale", gravity_scale);
    NUMBER_FIELD("restitution", restitution);
    NUMBER_FIELD("friction", friction);
    NUMBER_FIELD("controlX", control_x);
    NUMBER_FIELD("controlY", control_y);
    NUMBER_FIELD("jumpSpeed", jump_speed);
    NUMBER_FIELD("audioVolume", audio_volume);
    NUMBER_FIELD("tone", tone_frequency);
    NUMBER_FIELD("toneDuration", tone_duration);
#undef NUMBER_FIELD
    if (strcmp(field, "layer") == 0) {
        if (!value_number(parser, value, &number)) return 0;
        object->layer = (int)number;
        return 1;
    }
    if (strcmp(field, "color") == 0)
        return value_color(parser, value, &object->color);
#define STATUS_FIELD(name, member)                                             \
    if (strcmp(field, name) == 0)                                              \
        return value_status(parser, value, &object->member)
    STATUS_FIELD("visible", visible);
    STATUS_FIELD("trigger", trigger);
    STATUS_FIELD("audioLoop", audio_loop);
    STATUS_FIELD("audioAutoplay", audio_autoplay);
    STATUS_FIELD("audioOnCollision", audio_on_collision);
#undef STATUS_FIELD
    if (strcmp(field, "text") == 0 || strcmp(field, "asset") == 0 ||
        strcmp(field, "audio") == 0) {
        char **target = strcmp(field, "text") == 0 ? &object->text
                       : strcmp(field, "asset") == 0 ? &object->asset_path
                                                      : &object->audio_path;
        if (value->type != MODEL_TEXT) {
            parser_fail(parser, &parser->current,
                        "text, asset, and audio fields require quoted text");
            return 0;
        }
        if (!replace_text(target, value->text)) {
            parser_fail(parser, &parser->current, "out of memory");
            return 0;
        }
        return 1;
    }
    parser_fail(parser, &parser->current, "unknown game object field");
    return 0;
needs_identifier:
    parser_fail(parser, &parser->current, "field requires an identifier");
    return 0;
}

static int parse_block(ModelParser *parser, int is_scene, char *name) {
    ZSharpGameScene *scene = NULL;
    ZSharpGameObject *object = NULL;
    if (is_scene) scene = add_scene(parser, name);
    else object = add_object(parser, name);
    if (parser->failed || !parser_expect_type(
            parser, ZTOKEN_LEFT_BRACKET, "expected '[' after the name") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after the name") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before game fields")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "a game field name");
        ModelValue value;
        if (field == NULL || !parser_expect_type(
                parser, ZTOKEN_COLON, "expected ':' after the field name") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the field value")) {
            free(field);
            return 0;
        }
        if (is_scene) apply_scene_field(parser, scene, field, &value);
        else apply_object_field(parser, object, field, &value);
        free(value.text);
        free(field);
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after game fields");
}

static int parse_object_file(const char *path, const char *source,
                             ZSharpGameModel *model, int expected_is_3d,
                             char *error, size_t error_size) {
    ModelParser parser;
    const char *file_name = path;
    const char *cursor;
    const char *extension;
    char *source_name;
    memset(&parser, 0, sizeof(parser));
    parser.path = path;
    parser.model = model;
    parser.error = error;
    parser.error_size = error_size;
    for (cursor = path; *cursor != '\0'; cursor++)
        if (*cursor == '/' || *cursor == '\\') file_name = cursor + 1;
    extension = strrchr(file_name, '.');
    if (extension == NULL) extension = file_name + strlen(file_name);
    source_name = zsharp_copy_text(file_name,
                                   (size_t)(extension - file_name));
    if (source_name == NULL) {
        model_error(error, error_size, "out of memory");
        return 0;
    }
    parser.source_name = source_name;
    zsharp_lexer_init(&parser.lexer, source);
    parser_advance(&parser);
    if (!parser_expect_word(&parser, "zsharp") ||
        !parser_expect_type(&parser, ZTOKEN_EQUAL, "expected '='") ||
        !parser_expect_word(&parser, "type") ||
        !parser_expect_type(&parser, ZTOKEN_DOT, "expected '.'") ||
        !parser_expect_word(&parser, "object") ||
        !parser_expect_type(&parser, ZTOKEN_COLON,
                            "expected ':' before 2D or 3D") ||
        parser.current.type != ZTOKEN_NUMBER || parser.current.length != 1 ||
        (parser.current.start[0] != '2' && parser.current.start[0] != '3')) {
        if (!parser.failed)
            parser_fail(&parser, &parser.current,
                        "expected zsharp = type.object:2D or type.object:3D");
        free(source_name);
        return 0;
    }
    parser.file_is_3d = parser.current.start[0] == '3';
    parser_advance(&parser);
    if (!parser_expect_word(&parser, "D")) {
        free(source_name);
        return 0;
    }
    if (parser.file_is_3d != expected_is_3d) {
        parser_fail(&parser, &parser.current,
                    "object dimension does not match the game startup script");
        free(source_name);
        return 0;
    }
    while (!parser.failed && parser.current.type != ZTOKEN_EOF) {
        char *name;
        int is_scene;
        if (!parser_match_word(&parser, "noticed") &&
            !parser_match_word(&parser, "silent")) {
            parser_fail(&parser, &parser.current,
                        "expected 'noticed' or 'silent'");
            break;
        }
        if (parser_match_word(&parser, "scene")) is_scene = 1;
        else if (parser_match_word(&parser, "object")) is_scene = 0;
        else {
            parser_fail(&parser, &parser.current,
                        "expected 'scene' or 'object'");
            break;
        }
        name = parser_name(&parser, "a scene or object name");
        if (name == NULL || !parse_block(&parser, is_scene, name)) break;
    }
    free(source_name);
    return !parser.failed;
}

static void skip_zss_space(const char **cursor, unsigned *line) {
    for (;;) {
        while (isspace((unsigned char)**cursor)) {
            if (**cursor == '\n') (*line)++;
            (*cursor)++;
        }
        if ((*cursor)[0] == '/' && (*cursor)[1] == '*') {
            *cursor += 2;
            while (**cursor != '\0' &&
                   !((*cursor)[0] == '*' && (*cursor)[1] == '/')) {
                if (**cursor == '\n') (*line)++;
                (*cursor)++;
            }
            if (**cursor != '\0') *cursor += 2;
            continue;
        }
        break;
    }
}

static char *zss_name(const char **cursor) {
    const char *start = *cursor;
    while (isalnum((unsigned char)**cursor) || **cursor == '_' ||
           **cursor == '-') (*cursor)++;
    if (*cursor == start) return NULL;
    return zsharp_copy_text(start, (size_t)(*cursor - start));
}

static char *trimmed_text(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    return zsharp_copy_text(start, (size_t)(end - start));
}

static void zss_field_name(char *field) {
    char *input = field;
    char *output = field;
    int uppercase = 0;
    while (*input != '\0') {
        if (*input == '-') {
            uppercase = 1;
        } else if (uppercase) {
            *output++ = (char)toupper((unsigned char)*input);
            uppercase = 0;
        } else {
            *output++ = *input;
        }
        input++;
    }
    *output = '\0';
}

static int parse_zss_value(const char *text, ModelValue *value) {
    size_t length = strlen(text);
    memset(value, 0, sizeof(*value));
    if (length >= 2 && text[0] == '"' && text[length - 1] == '"') {
        value->type = MODEL_TEXT;
        value->text = zsharp_copy_text(text + 1, length - 2);
    } else {
        char *end = NULL;
        strtod(text, &end);
        if (text[0] == '#') value->type = MODEL_COLOR;
        else if (end != text && end != NULL && *end == '\0')
            value->type = MODEL_NUMBER;
        else value->type = MODEL_IDENTIFIER;
        value->text = zsharp_copy_text(text, length);
    }
    return value->text != NULL;
}

static int parse_style_file(const char *path, const char *source,
                            ZSharpGameModel *model, char *error,
                            size_t error_size) {
    const char *cursor = source;
    unsigned line = 1;
    while (1) {
        char *first = NULL;
        char *second = NULL;
        const char *object_name;
        const char *file_name = NULL;
        size_t match_count = 0;
        size_t object_index;
        skip_zss_space(&cursor, &line);
        if (*cursor == '\0') return 1;
        if (*cursor++ != '.') goto selector_error;
        first = zss_name(&cursor);
        if (first == NULL) goto selector_error;
        skip_zss_space(&cursor, &line);
        if (*cursor != '{') {
            second = zss_name(&cursor);
            if (second == NULL) goto selector_error;
            file_name = first;
            object_name = second;
            skip_zss_space(&cursor, &line);
        } else {
            object_name = first;
        }
        if (*cursor++ != '{') goto selector_error;
        for (object_index = 0; object_index < model->object_count;
             object_index++) {
            ZSharpGameObject *object = &model->objects[object_index];
            if (strcmp(object->name, object_name) == 0 &&
                (file_name == NULL ||
                 strcmp(object->source_file, file_name) == 0)) match_count++;
        }
        if (match_count == 0) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "%s:%u: ZSS selector does not match a game object",
                         path, line);
            free(first);
            free(second);
            return 0;
        }
        while (1) {
            char *field;
            char *raw_value;
            const char *value_start;
            ModelValue value;
            ModelParser parser;
            skip_zss_space(&cursor, &line);
            if (*cursor == '}') {
                cursor++;
                break;
            }
            field = zss_name(&cursor);
            if (field == NULL) goto declaration_error;
            zss_field_name(field);
            skip_zss_space(&cursor, &line);
            if (*cursor++ != ':') {
                free(field);
                goto declaration_error;
            }
            value_start = cursor;
            while (*cursor != '\0' && *cursor != ';' && *cursor != '}') {
                if (*cursor == '\n') line++;
                cursor++;
            }
            raw_value = trimmed_text(value_start, cursor);
            if (raw_value == NULL || raw_value[0] == '\0' ||
                !parse_zss_value(raw_value, &value)) {
                free(raw_value);
                free(field);
                goto declaration_error;
            }
            free(raw_value);
            memset(&parser, 0, sizeof(parser));
            parser.path = path;
            parser.error = error;
            parser.error_size = error_size;
            parser.current.line = line;
            parser.current.column = 1;
            for (object_index = 0; object_index < model->object_count;
                 object_index++) {
                ZSharpGameObject *object = &model->objects[object_index];
                if (strcmp(object->name, object_name) != 0 ||
                    (file_name != NULL &&
                     strcmp(object->source_file, file_name) != 0)) continue;
                if (!apply_object_field(&parser, object, field, &value)) {
                    free(value.text);
                    free(field);
                    free(first);
                    free(second);
                    return 0;
                }
            }
            free(value.text);
            free(field);
            if (*cursor == ';') cursor++;
            else if (*cursor != '}') goto declaration_error;
        }
        free(first);
        free(second);
        continue;
selector_error:
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                     "%s:%u: expected ZSS selector '.Object' or '.File Object'",
                     path, line);
        free(first);
        free(second);
        return 0;
declaration_error:
        if (error != NULL && error_size != 0)
            snprintf(error, error_size,
                     "%s:%u: invalid ZSS declaration", path, line);
        free(first);
        free(second);
        return 0;
    }
}

static ZSharpGameScene *find_scene(const ZSharpGameModel *model,
                                   const char *name) {
    size_t index;
    for (index = 0; index < model->scene_count; index++)
        if (strcmp(model->scenes[index].name, name) == 0)
            return &model->scenes[index];
    return NULL;
}

static ZSharpGameObject *find_object(const ZSharpGameModel *model,
                                     const char *name) {
    size_t index;
    for (index = 0; index < model->object_count; index++)
        if (strcmp(model->objects[index].name, name) == 0)
            return &model->objects[index];
    return NULL;
}

static int safe_relative_asset(const char *path) {
    return path != NULL && path[0] != '\0' && path[0] != '/' &&
           path[0] != '\\' &&
           !(isalpha((unsigned char)path[0]) && path[1] == ':') &&
           strstr(path, "..") == NULL;
}

int zsharp_game_model_load(const char *project_root, int is_3d,
                           ZSharpGameModel *model, char *error,
                           size_t error_size) {
    ZSharpSourceList files;
    size_t index;
    ZSharpSourceList styles;
    memset(model, 0, sizeof(*model));
    model->is_3d = is_3d;
    model->project_root = zsharp_copy_text(project_root, strlen(project_root));
    if (model->project_root == NULL) goto out_of_memory;
    if (!zsharp_project_list_files(project_root, ZSHARP_OBJECT_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_object_file(files.items[index], source, model, is_3d,
                               error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (!zsharp_project_list_files(project_root, ZSHARP_STYLE_EXTENSION,
                                   &styles, error, error_size)) goto failed;
    for (index = 0; index < styles.count; index++) {
        char *source = NULL;
        if (!read_file(styles.items[index], &source, error, error_size) ||
            !parse_style_file(styles.items[index], source, model,
                              error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&styles);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&styles);
    if (model->scene_count == 0) {
        ModelParser parser;
        memset(&parser, 0, sizeof(parser));
        parser.model = model;
        parser.file_is_3d = is_3d;
        parser.error = error;
        parser.error_size = error_size;
        if (add_scene(&parser, zsharp_copy_text("Main", 4)) == NULL)
            goto failed;
    }
    model->active_scene = zsharp_copy_text(model->scenes[0].name,
                                            strlen(model->scenes[0].name));
    if (model->active_scene == NULL) goto out_of_memory;
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        if (object->scene == NULL) {
            object->scene = zsharp_copy_text(model->active_scene,
                                              strlen(model->active_scene));
            if (object->scene == NULL) goto out_of_memory;
        }
        if (find_scene(model, object->scene) == NULL) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' uses unknown scene '%s'",
                         object->name, object->scene);
            goto failed;
        }
        if (object->width <= 0.0f || object->height <= 0.0f ||
            object->depth <= 0.0f || object->mass <= 0.0f ||
            object->scale_x <= 0.0f || object->scale_y <= 0.0f ||
            object->scale_z <= 0.0f || object->audio_volume < 0.0f ||
            object->audio_volume > 1.0f) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' has invalid size, mass, scale, or audioVolume",
                         object->name);
            goto failed;
        }
        if ((object->shape == ZGAME_SHAPE_SPRITE &&
             !safe_relative_asset(object->asset_path)) ||
            (object->asset_path != NULL &&
             !safe_relative_asset(object->asset_path)) ||
            (object->audio_path != NULL &&
             !safe_relative_asset(object->audio_path))) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' requires a safe project-relative asset path",
                         object->name);
            goto failed;
        }
    }
    return 1;
out_of_memory:
    model_error(error, error_size, "out of memory");
failed:
    zsharp_game_model_free(model);
    return 0;
}

int zsharp_game_model_validate(const char *project_root, int is_3d,
                               char *error, size_t error_size) {
    ZSharpGameModel model;
    int ok = zsharp_game_model_load(project_root, is_3d, &model, error,
                                    error_size);
    if (ok) zsharp_game_model_free(&model);
    return ok;
}

void zsharp_game_model_free(ZSharpGameModel *model) {
    size_t index;
    if (model == NULL) return;
    for (index = 0; index < model->scene_count; index++)
        free(model->scenes[index].name);
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        free(object->name);
        free(object->source_file);
        free(object->scene);
        free(object->text);
        free(object->asset_path);
        free(object->audio_path);
    }
    free(model->scenes);
    free(model->objects);
    free(model->active_scene);
    free(model->project_root);
    memset(model, 0, sizeof(*model));
}

static int same_active_scene(const ZSharpGameModel *model,
                             const ZSharpGameObject *object) {
    return model->active_scene != NULL && object->scene != NULL &&
           strcmp(model->active_scene, object->scene) == 0;
}

static int overlaps(const ZSharpGameObject *a, const ZSharpGameObject *b,
                    float *overlap_x, float *overlap_y, float *overlap_z) {
    float ax = a->width * a->scale_x * 0.5f;
    float ay = a->height * a->scale_y * 0.5f;
    float az = a->depth * a->scale_z * 0.5f;
    float bx = b->width * b->scale_x * 0.5f;
    float by = b->height * b->scale_y * 0.5f;
    float bz = b->depth * b->scale_z * 0.5f;
    *overlap_x = ax + bx - fabsf(a->x - b->x);
    *overlap_y = ay + by - fabsf(a->y - b->y);
    *overlap_z = az + bz - fabsf(a->z - b->z);
    return *overlap_x > 0.0f && *overlap_y > 0.0f && *overlap_z > 0.0f;
}

static void resolve_collision(ZSharpGameObject *dynamic,
                              const ZSharpGameObject *other, int is_3d,
                              float overlap_x, float overlap_y,
                              float overlap_z) {
    float bounce = dynamic->restitution;
    if (overlap_x <= overlap_y && (!is_3d || overlap_x <= overlap_z)) {
        dynamic->x += dynamic->x < other->x ? -overlap_x : overlap_x;
        dynamic->velocity_x = -dynamic->velocity_x * bounce;
    } else if (overlap_y <= overlap_z || !is_3d) {
        int above = dynamic->y > other->y;
        dynamic->y += above ? overlap_y : -overlap_y;
        if (above && dynamic->velocity_y <= 0.0f) dynamic->grounded = 1;
        dynamic->velocity_y = -dynamic->velocity_y * bounce;
        dynamic->velocity_x *= 1.0f - fminf(fmaxf(dynamic->friction, 0.0f),
                                             1.0f);
    } else {
        dynamic->z += dynamic->z < other->z ? -overlap_z : overlap_z;
        dynamic->velocity_z = -dynamic->velocity_z * bounce;
    }
}

void zsharp_game_model_update(ZSharpGameModel *model, double delta_seconds) {
    ZSharpGameScene *scene = find_scene(model, model->active_scene);
    float delta = (float)fmin(delta_seconds, 0.05);
    size_t index;
    size_t other_index;
    if (scene == NULL) return;
    model->delta = delta;
    model->elapsed += delta;
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        if (!same_active_scene(model, object)) continue;
        object->grounded = 0;
        object->colliding = 0;
        if (object->control_x != 0.0f)
            object->velocity_x =
                (float)(model->input.right - model->input.left) *
                object->control_x;
        if (object->control_y != 0.0f)
            object->velocity_y =
                (float)(model->input.up - model->input.down) *
                object->control_y;
        if (object->body == ZGAME_BODY_DYNAMIC) {
            object->velocity_x += scene->gravity_x * object->gravity_scale * delta;
            object->velocity_y += scene->gravity_y * object->gravity_scale * delta;
            object->velocity_z += scene->gravity_z * object->gravity_scale * delta;
        }
        if (object->body != ZGAME_BODY_STATIC) {
            object->x += object->velocity_x * delta;
            object->y += object->velocity_y * delta;
            object->z += object->velocity_z * delta;
        }
    }
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *a = &model->objects[index];
        if (!same_active_scene(model, a) ||
            a->collider == ZGAME_COLLIDER_NONE) continue;
        for (other_index = index + 1; other_index < model->object_count;
             other_index++) {
            ZSharpGameObject *b = &model->objects[other_index];
            float overlap_x, overlap_y, overlap_z;
            if (!same_active_scene(model, b) ||
                b->collider == ZGAME_COLLIDER_NONE ||
                (a->body == ZGAME_BODY_STATIC &&
                 b->body == ZGAME_BODY_STATIC) ||
                !overlaps(a, b, &overlap_x, &overlap_y, &overlap_z)) continue;
            a->colliding = b->colliding = 1;
            if (a->trigger || b->trigger) continue;
            if (a->body == ZGAME_BODY_DYNAMIC)
                resolve_collision(a, b, model->is_3d,
                                  overlap_x, overlap_y, overlap_z);
            if (b->body == ZGAME_BODY_DYNAMIC)
                resolve_collision(b, a, model->is_3d,
                                  overlap_x, overlap_y, overlap_z);
        }
    }
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        if (object->jump_speed != 0.0f && object->grounded &&
            (model->input.up || model->input.space)) {
            object->velocity_y = object->jump_speed;
            object->grounded = 0;
        }
    }
}

static int split_path(const char *path, char *storage, size_t storage_size,
                      char **parts, size_t *count) {
    char *cursor;
    size_t result = 1;
    if (path == NULL || strlen(path) + 1 > storage_size) return 0;
    memcpy(storage, path, strlen(path) + 1);
    parts[0] = storage;
    for (cursor = storage; *cursor != '\0'; cursor++) {
        if (*cursor != '.') continue;
        *cursor = '\0';
        if (result == 3 || cursor[1] == '\0') return 0;
        parts[result++] = cursor + 1;
    }
    *count = result;
    return result >= 2;
}

static int valid_input_field(const char *field) {
    return strcmp(field, "left") == 0 || strcmp(field, "right") == 0 ||
           strcmp(field, "up") == 0 || strcmp(field, "down") == 0 ||
           strcmp(field, "space") == 0 || strcmp(field, "action") == 0 ||
           strcmp(field, "mouseLeft") == 0 ||
           strcmp(field, "mouseRight") == 0 ||
           strcmp(field, "mouseX") == 0 || strcmp(field, "mouseY") == 0;
}

static int valid_object_field(const char *field) {
    static const char *fields[] = {
        "positionX","positionY","positionZ","width","height","depth",
        "rotation","scaleX","scaleY","scaleZ","velocityX","velocityY",
        "velocityZ","mass","gravityScale","restitution","friction",
        "controlX","controlY","jumpSpeed","audioVolume","tone",
        "toneDuration","layer","color",
        "visible","trigger","grounded","colliding","text","scene",
        "audioLoop","audioAutoplay","audioOnCollision","audioPlay"
    };
    size_t index;
    for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++)
        if (strcmp(field, fields[index]) == 0) return 1;
    return 0;
}

static int valid_scene_field(const char *field) {
    return strcmp(field, "background") == 0 ||
           strcmp(field, "gravityX") == 0 ||
           strcmp(field, "gravityY") == 0 ||
           strcmp(field, "gravityZ") == 0 ||
           strcmp(field, "cameraX") == 0 ||
           strcmp(field, "cameraY") == 0 ||
           strcmp(field, "cameraZ") == 0 ||
           strcmp(field, "cameraFov") == 0;
}

int zsharp_game_model_owns_property(const ZSharpGameModel *model,
                                    const char *path) {
    char storage[512];
    char *parts[3];
    size_t count;
    ZSharpGameObject *object;
    if (!split_path(path, storage, sizeof(storage), parts, &count)) return 0;
    if (count == 2 && strcmp(parts[0], "Input") == 0)
        return valid_input_field(parts[1]);
    if (count == 2 && strcmp(parts[0], "Game") == 0)
        return strcmp(parts[1], "scene") == 0 ||
               strcmp(parts[1], "delta") == 0 ||
               strcmp(parts[1], "elapsed") == 0 ||
               strcmp(parts[1], "fps") == 0;
    if (count == 2 && find_scene(model, parts[0]) != NULL)
        return valid_scene_field(parts[1]);
    object = find_object(model, count == 2 ? parts[0] : parts[1]);
    if (object == NULL || !valid_object_field(parts[count - 1])) return 0;
    return count == 2 || strcmp(parts[0], object->scene) == 0;
}

static int copy_property_text(const char *source, char **output, char *error,
                              size_t error_size) {
    *output = zsharp_copy_text(source, strlen(source));
    if (*output == NULL) {
        model_error(error, error_size, "out of memory");
        return 0;
    }
    return 1;
}

static int number_property(float value, char **output, char *error,
                           size_t error_size) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9g", (double)value);
    return copy_property_text(buffer, output, error, error_size);
}

static int status_property(int value, char **output, char *error,
                           size_t error_size) {
    return copy_property_text(value ? "alive" : "dead", output, error,
                              error_size);
}

int zsharp_game_model_get_property(const ZSharpGameModel *model,
                                   const char *path,
                                   ZSharpWindowReadType *type, char **text,
                                   char *error, size_t error_size) {
    char storage[512];
    char *parts[3];
    size_t count;
    const char *field;
    ZSharpGameScene *scene;
    ZSharpGameObject *object;
    if (!zsharp_game_model_owns_property(model, path) ||
        !split_path(path, storage, sizeof(storage), parts, &count)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "unknown game property '%s'", path);
        return 0;
    }
    field = parts[count - 1];
    if (count == 2 && strcmp(parts[0], "Input") == 0) {
        if (strcmp(field, "mouseX") == 0 || strcmp(field, "mouseY") == 0) {
            *type = ZWINDOW_READ_NUMBER;
            return number_property(strcmp(field, "mouseX") == 0
                                       ? model->input.mouse_x
                                       : model->input.mouse_y,
                                   text, error, error_size);
        }
        *type = ZWINDOW_READ_STATUS;
        return status_property(
            strcmp(field, "left") == 0 ? model->input.left :
            strcmp(field, "right") == 0 ? model->input.right :
            strcmp(field, "up") == 0 ? model->input.up :
            strcmp(field, "down") == 0 ? model->input.down :
            strcmp(field, "space") == 0 ? model->input.space :
            strcmp(field, "action") == 0 ? model->input.action :
            strcmp(field, "mouseLeft") == 0 ? model->input.mouse_left
                                              : model->input.mouse_right,
            text, error, error_size);
    }
    if (count == 2 && strcmp(parts[0], "Game") == 0) {
        if (strcmp(field, "scene") == 0) {
            *type = ZWINDOW_READ_TEXT;
            return copy_property_text(model->active_scene, text, error,
                                      error_size);
        }
        *type = ZWINDOW_READ_NUMBER;
        return number_property(
            strcmp(field, "delta") == 0 ? (float)model->delta :
            strcmp(field, "elapsed") == 0 ? (float)model->elapsed :
            model->delta > 0.0 ? (float)(1.0 / model->delta) : 0.0f,
            text, error, error_size);
    }
    scene = count == 2 ? find_scene(model, parts[0]) : NULL;
    if (scene != NULL) {
        if (strcmp(field, "background") == 0) {
            char color[8];
            snprintf(color, sizeof(color), "#%06X", scene->background);
            *type = ZWINDOW_READ_TEXT;
            return copy_property_text(color, text, error, error_size);
        }
        *type = ZWINDOW_READ_NUMBER;
        return number_property(
            strcmp(field, "gravityX") == 0 ? scene->gravity_x :
            strcmp(field, "gravityY") == 0 ? scene->gravity_y :
            strcmp(field, "gravityZ") == 0 ? scene->gravity_z :
            strcmp(field, "cameraX") == 0 ? scene->camera_x :
            strcmp(field, "cameraY") == 0 ? scene->camera_y :
            strcmp(field, "cameraZ") == 0 ? scene->camera_z
                                            : scene->camera_fov,
            text, error, error_size);
    }
    object = find_object(model, count == 2 ? parts[0] : parts[1]);
#define GET_NUMBER(name, member)                                               \
    if (strcmp(field, name) == 0) {                                            \
        *type = ZWINDOW_READ_NUMBER;                                           \
        return number_property(object->member, text, error, error_size);       \
    }
    GET_NUMBER("positionX", x)
    GET_NUMBER("positionY", y)
    GET_NUMBER("positionZ", z)
    GET_NUMBER("width", width)
    GET_NUMBER("height", height)
    GET_NUMBER("depth", depth)
    GET_NUMBER("rotation", rotation)
    GET_NUMBER("scaleX", scale_x)
    GET_NUMBER("scaleY", scale_y)
    GET_NUMBER("scaleZ", scale_z)
    GET_NUMBER("velocityX", velocity_x)
    GET_NUMBER("velocityY", velocity_y)
    GET_NUMBER("velocityZ", velocity_z)
    GET_NUMBER("mass", mass)
    GET_NUMBER("gravityScale", gravity_scale)
    GET_NUMBER("restitution", restitution)
    GET_NUMBER("friction", friction)
    GET_NUMBER("controlX", control_x)
    GET_NUMBER("controlY", control_y)
    GET_NUMBER("jumpSpeed", jump_speed)
    GET_NUMBER("audioVolume", audio_volume)
    GET_NUMBER("tone", tone_frequency)
    GET_NUMBER("toneDuration", tone_duration)
#undef GET_NUMBER
    if (strcmp(field, "layer") == 0) {
        *type = ZWINDOW_READ_NUMBER;
        return number_property((float)object->layer, text, error, error_size);
    }
    if (strcmp(field, "color") == 0) {
        char color[8];
        snprintf(color, sizeof(color), "#%06X", object->color);
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(color, text, error, error_size);
    }
    if (strcmp(field, "text") == 0 || strcmp(field, "scene") == 0) {
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(strcmp(field, "text") == 0
                                      ? (object->text == NULL ? "" : object->text)
                                      : object->scene,
                                  text, error, error_size);
    }
    *type = ZWINDOW_READ_STATUS;
    return status_property(
        strcmp(field, "visible") == 0 ? object->visible :
        strcmp(field, "trigger") == 0 ? object->trigger :
        strcmp(field, "grounded") == 0 ? object->grounded :
        strcmp(field, "colliding") == 0 ? object->colliding :
        strcmp(field, "audioLoop") == 0 ? object->audio_loop :
        strcmp(field, "audioAutoplay") == 0 ? object->audio_autoplay :
        strcmp(field, "audioOnCollision") == 0 ? object->audio_on_collision
                                                : object->audio_started,
        text, error, error_size);
}

static int parse_runtime_number(const char *value, float *output) {
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || end == NULL || *end != '\0' || !isfinite(parsed) ||
        fabs(parsed) > 1000000000.0) return 0;
    *output = (float)parsed;
    return 1;
}

int zsharp_game_model_set_property(ZSharpGameModel *model, const char *path,
                                   ZSharpWindowValueType value_type,
                                   const char *value, char *error,
                                   size_t error_size) {
    char storage[512];
    char *parts[3];
    size_t count;
    const char *field;
    ZSharpGameScene *scene;
    ZSharpGameObject *object;
    float number;
    int status;
    (void)value_type;
    if (!zsharp_game_model_owns_property(model, path) ||
        !split_path(path, storage, sizeof(storage), parts, &count)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "unknown game property '%s'", path);
        return 0;
    }
    field = parts[count - 1];
    if (count == 2 && strcmp(parts[0], "Game") == 0 &&
        strcmp(field, "scene") == 0) {
        if (find_scene(model, value) == NULL) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size, "unknown game scene '%s'", value);
            return 0;
        }
        return replace_text(&model->active_scene, value);
    }
    if (count == 2 && (strcmp(parts[0], "Input") == 0 ||
                       strcmp(parts[0], "Game") == 0)) {
        model_error(error, error_size, "that engine property is read-only");
        return 0;
    }
    scene = count == 2 ? find_scene(model, parts[0]) : NULL;
    if (scene != NULL) {
        if (strcmp(field, "background") == 0) {
            char *end = NULL;
            unsigned long color;
            if (strlen(value) != 7 || value[0] != '#') {
                model_error(error, error_size,
                            "scene backgrounds require #RRGGBB");
                return 0;
            }
            color = strtoul(value + 1, &end, 16);
            if (end == NULL || *end != '\0') return 0;
            scene->background = (unsigned)color;
            return 1;
        }
        if (!parse_runtime_number(value, &number)) {
            model_error(error, error_size, "scene property requires a number");
            return 0;
        }
        if (strcmp(field, "gravityX") == 0) scene->gravity_x = number;
        else if (strcmp(field, "gravityY") == 0) scene->gravity_y = number;
        else if (strcmp(field, "gravityZ") == 0) scene->gravity_z = number;
        else if (strcmp(field, "cameraX") == 0) scene->camera_x = number;
        else if (strcmp(field, "cameraY") == 0) scene->camera_y = number;
        else if (strcmp(field, "cameraZ") == 0) scene->camera_z = number;
        else scene->camera_fov = number;
        return 1;
    }
    object = find_object(model, count == 2 ? parts[0] : parts[1]);
    if (strcmp(field, "text") == 0)
        return replace_text(&object->text, value);
    if (strcmp(field, "scene") == 0) {
        if (find_scene(model, value) == NULL) {
            model_error(error, error_size, "unknown game scene");
            return 0;
        }
        return replace_text(&object->scene, value);
    }
    if (strcmp(field, "color") == 0) {
        char *end = NULL;
        unsigned long color;
        if (strlen(value) != 7 || value[0] != '#') {
            model_error(error, error_size, "game colors require #RRGGBB");
            return 0;
        }
        color = strtoul(value + 1, &end, 16);
        if (end == NULL || *end != '\0') return 0;
        object->color = (unsigned)color;
        return 1;
    }
    if (strcmp(field, "visible") == 0 || strcmp(field, "trigger") == 0 ||
        strcmp(field, "audioLoop") == 0 ||
        strcmp(field, "audioAutoplay") == 0 ||
        strcmp(field, "audioOnCollision") == 0 ||
        strcmp(field, "audioPlay") == 0) {
        if (strcmp(value, "alive") == 0) status = 1;
        else if (strcmp(value, "dead") == 0) status = 0;
        else {
            model_error(error, error_size, "status property requires alive or dead");
            return 0;
        }
        if (strcmp(field, "visible") == 0) object->visible = status;
        else if (strcmp(field, "trigger") == 0) object->trigger = status;
        else if (strcmp(field, "audioLoop") == 0) object->audio_loop = status;
        else if (strcmp(field, "audioAutoplay") == 0)
            object->audio_autoplay = status;
        else if (strcmp(field, "audioOnCollision") == 0)
            object->audio_on_collision = status;
        else object->audio_started = status;
        return 1;
    }
    if (strcmp(field, "grounded") == 0 || strcmp(field, "colliding") == 0) {
        model_error(error, error_size, "collision state is read-only");
        return 0;
    }
    if (!parse_runtime_number(value, &number)) {
        model_error(error, error_size, "game property requires a number");
        return 0;
    }
#define SET_NUMBER(name, member)                                               \
    if (strcmp(field, name) == 0) { object->member = number; return 1; }
    SET_NUMBER("positionX", x)
    SET_NUMBER("positionY", y)
    SET_NUMBER("positionZ", z)
    SET_NUMBER("width", width)
    SET_NUMBER("height", height)
    SET_NUMBER("depth", depth)
    SET_NUMBER("rotation", rotation)
    SET_NUMBER("scaleX", scale_x)
    SET_NUMBER("scaleY", scale_y)
    SET_NUMBER("scaleZ", scale_z)
    SET_NUMBER("velocityX", velocity_x)
    SET_NUMBER("velocityY", velocity_y)
    SET_NUMBER("velocityZ", velocity_z)
    SET_NUMBER("mass", mass)
    SET_NUMBER("gravityScale", gravity_scale)
    SET_NUMBER("restitution", restitution)
    SET_NUMBER("friction", friction)
    SET_NUMBER("controlX", control_x)
    SET_NUMBER("controlY", control_y)
    SET_NUMBER("jumpSpeed", jump_speed)
    SET_NUMBER("audioVolume", audio_volume)
    SET_NUMBER("tone", tone_frequency)
    SET_NUMBER("toneDuration", tone_duration)
#undef SET_NUMBER
    if (strcmp(field, "layer") == 0) {
        object->layer = (int)number;
        return 1;
    }
    model_error(error, error_size, "property cannot be changed");
    return 0;
}

void zsharp_game_model_frame(const ZSharpGameModel *model,
                             ZSharpGameRenderFrame *frame,
                             ZSharpGameRenderObject **objects) {
    ZSharpGameScene *scene = find_scene(model, model->active_scene);
    size_t index;
    size_t count = 0;
    memset(frame, 0, sizeof(*frame));
    *objects = model->object_count == 0 ? NULL :
        (ZSharpGameRenderObject *)calloc(model->object_count,
                                         sizeof(**objects));
    for (index = 0; index < model->object_count; index++) {
        const ZSharpGameObject *source = &model->objects[index];
        ZSharpGameRenderObject *target;
        if (!same_active_scene(model, source)) continue;
        target = &(*objects)[count++];
        target->shape = source->shape;
        target->x = source->x;
        target->y = source->y;
        target->z = source->z;
        target->width = source->width;
        target->height = source->height;
        target->depth = source->depth;
        target->rotation = source->rotation;
        target->scale_x = source->scale_x;
        target->scale_y = source->scale_y;
        target->scale_z = source->scale_z;
        target->color = source->color;
        target->visible = source->visible;
        target->layer = source->layer;
        target->text = source->text;
        target->asset_path = source->asset_path;
    }
    frame->is_3d = model->is_3d;
    frame->background = scene == NULL ? 0x08080bu : scene->background;
    frame->camera_x = scene == NULL ? 0.0f : scene->camera_x;
    frame->camera_y = scene == NULL ? 0.0f : scene->camera_y;
    frame->camera_z = scene == NULL ? 8.0f : scene->camera_z;
    frame->camera_fov = scene == NULL ? 70.0f : scene->camera_fov;
    frame->project_root = model->project_root;
    frame->objects = *objects;
    frame->object_count = count;
}
