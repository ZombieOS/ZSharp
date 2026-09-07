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
    int definition_mode;
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

static int safe_relative_asset(const char *path);

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
    scene->source_file = zsharp_copy_text(parser->source_name,
                                           strlen(parser->source_name));
    if (scene->source_file == NULL) {
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    scene->background = 0x08080bu;
    scene->gravity_y = -900.0f;
    scene->camera_z = 0.0f;
    scene->camera_fov = 70.0f;
    return scene;
}

static ZSharpGameObject *add_object(ModelParser *parser, char *name) {
    ZSharpGameObject *resized;
    ZSharpGameObject *object;
    ZSharpGameObject **items = parser->definition_mode
        ? &parser->model->definitions : &parser->model->objects;
    size_t *count = parser->definition_mode
        ? &parser->model->definition_count : &parser->model->object_count;
    size_t index;
    for (index = 0; index < *count; index++) {
        if (strcmp((*items)[index].name, name) == 0) {
            free(name);
            parser_fail(parser, &parser->current,
                        "game object names must be unique across the project");
            return NULL;
        }
    }
    resized = (ZSharpGameObject *)realloc(
        *items, (*count + 1) * sizeof(*resized));
    if (resized == NULL) {
        free(name);
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    *items = resized;
    object = &resized[(*count)++];
    memset(object, 0, sizeof(*object));
    object->name = name;
    object->source_file = zsharp_copy_text(parser->source_name,
                                            strlen(parser->source_name));
    if (object->source_file == NULL) {
        parser_fail(parser, &parser->current, "out of memory");
        return NULL;
    }
    object->shape = ZGAME_SHAPE_RECTANGLE;
    object->body = ZGAME_BODY_STATIC;
    object->collider = ZGAME_COLLIDER_NONE;
    object->width = 64.0f;
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
    if (strcmp(field, "title") == 0) {
        if (value->type != MODEL_TEXT) {
            parser_fail(parser, &parser->current,
                        "scene titles require quoted text");
            return 0;
        }
        return replace_text(&scene->title, value->text);
    }
    if (strcmp(field, "icon") == 0) {
        if (value->type != MODEL_TEXT || !safe_relative_asset(value->text)) {
            parser_fail(parser, &parser->current,
                        "scene icons require a safe quoted project-relative path");
            return 0;
        }
        return replace_text(&scene->icon, value->text);
    }
    if (strcmp(field, "background") == 0)
        return value_color(parser, value, &scene->background);
    if (strcmp(field, "gravityX") == 0)
        return value_number(parser, value, &scene->gravity_x);
    if (strcmp(field, "gravityY") == 0)
        return value_number(parser, value, &scene->gravity_y);
    if (strcmp(field, "gravityZ") == 0) {
        parser->model->is_3d = 1;
        return value_number(parser, value, &scene->gravity_z);
    }
    if (strcmp(field, "cameraX") == 0)
        return value_number(parser, value, &scene->camera_x);
    if (strcmp(field, "cameraY") == 0)
        return value_number(parser, value, &scene->camera_y);
    if (strcmp(field, "cameraZ") == 0) {
        parser->model->is_3d = 1;
        return value_number(parser, value, &scene->camera_z);
    }
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
            object->shape = ZGAME_SHAPE_CUBE, parser->model->is_3d = 1;
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
#define NUMBER_FIELD_3D(name, member)                                          \
    if (strcmp(field, name) == 0) {                                            \
        parser->model->is_3d = 1;                                             \
        return value_number(parser, value, &object->member);                   \
    }
    NUMBER_FIELD("positionX", x);
    NUMBER_FIELD("positionY", y);
    NUMBER_FIELD_3D("positionZ", z);
    NUMBER_FIELD("width", width);
    NUMBER_FIELD("height", height);
    NUMBER_FIELD_3D("depth", depth);
    NUMBER_FIELD_3D("length", depth);
    NUMBER_FIELD("rotation", rotation);
    NUMBER_FIELD("scaleX", scale_x);
    NUMBER_FIELD("scaleY", scale_y);
    NUMBER_FIELD_3D("scaleZ", scale_z);
    NUMBER_FIELD("velocityX", velocity_x);
    NUMBER_FIELD("velocityY", velocity_y);
    NUMBER_FIELD_3D("velocityZ", velocity_z);
    NUMBER_FIELD("mass", mass);
    NUMBER_FIELD("gravityScale", gravity_scale);
    NUMBER_FIELD("restitution", restitution);
    NUMBER_FIELD("friction", friction);
    NUMBER_FIELD("audioVolume", audio_volume);
    NUMBER_FIELD("tone", tone_frequency);
    NUMBER_FIELD("toneDuration", tone_duration);
#undef NUMBER_FIELD
#undef NUMBER_FIELD_3D
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
        strcmp(field, "texture") == 0 || strcmp(field, "audio") == 0) {
        char **target = strcmp(field, "text") == 0 ? &object->text
                       : (strcmp(field, "asset") == 0 ||
                          strcmp(field, "texture") == 0) ? &object->asset_path
                                                      : &object->audio_path;
        if (value->type != MODEL_TEXT) {
            parser_fail(parser, &parser->current,
                        "text, texture, asset, and audio fields require quoted text");
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

static ZSharpGameObject *find_definition(const ZSharpGameModel *model,
                                         const char *id) {
    size_t index;
    for (index = 0; index < model->definition_count; index++)
        if (strcmp(model->definitions[index].name, id) == 0)
            return &model->definitions[index];
    return NULL;
}

static int append_attribute(ModelParser *parser, ZSharpGameObject *object,
                            char *id, int active) {
    char **ids = (char **)realloc(object->attribute_ids,
        (object->attribute_count + 1) * sizeof(*ids));
    int *states;
    if (ids == NULL) {
        free(id);
        parser_fail(parser, &parser->current, "out of memory");
        return 0;
    }
    object->attribute_ids = ids;
    states = (int *)realloc(object->attribute_active,
        (object->attribute_count + 1) * sizeof(*states));
    if (states == NULL) {
        free(id);
        parser_fail(parser, &parser->current, "out of memory");
        return 0;
    }
    object->attribute_active = states;
    object->attribute_ids[object->attribute_count] = id;
    object->attribute_active[object->attribute_count++] = active;
    if (active && (strcmp(id, "COLLIDER2D") == 0 ||
                   strcmp(id, "COLLIDER3D") == 0)) {
        object->collider = ZGAME_COLLIDER_BOX;
        if (strcmp(id, "COLLIDER3D") == 0) parser->model->is_3d = 1;
    }
    return 1;
}

static char *json_text(ModelParser *parser, const char *description) {
    ZSharpToken token = parser->current;
    char *result;
    if (token.type != ZTOKEN_STRING) {
        char message[128];
        snprintf(message, sizeof(message), "expected quoted %s", description);
        parser_fail(parser, &token, message);
        return NULL;
    }
    result = zsharp_copy_text(token.start + 1, token.length - 2);
    if (result == NULL) parser_fail(parser, &token, "out of memory");
    parser_advance(parser);
    return result;
}

static int json_key(ModelParser *parser, const char *key) {
    char *actual = json_text(parser, "JSON field name");
    int matches = actual != NULL && strcmp(actual, key) == 0;
    if (actual != NULL && !matches)
        parser_fail(parser, &parser->current, "unexpected JSON field");
    free(actual);
    return matches && parser_expect_type(parser, ZTOKEN_COLON,
                                          "expected ':' after JSON field");
}

static int parse_attributes(ModelParser *parser, ZSharpGameObject *object) {
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after attributes") ||
        !parser_expect_word(parser, "JSON") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after JSON") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before attributes") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' before the JSON array")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        char *id = NULL;
        int active;
        if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                                "expected '{' before an attribute") ||
            !json_key(parser, "id") || (id = json_text(parser, "attribute id")) == NULL ||
            !parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after the attribute id") ||
            !json_key(parser, "active")) {
            free(id);
            return 0;
        }
        if (parser_match_word(parser, "true")) active = 1;
        else if (parser_match_word(parser, "false")) active = 0;
        else {
            free(id);
            parser_fail(parser, &parser->current,
                        "attribute active must be true or false");
            return 0;
        }
        if (!parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                                "expected '}' after an attribute") ||
            !append_attribute(parser, object, id, active)) return 0;
        if (!parser_match_type(parser, ZTOKEN_COMMA)) break;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "expected ']' after the attribute array") &&
           parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after attributes");
}

static int copy_optional(char **target, const char *source) {
    *target = source == NULL ? NULL : zsharp_copy_text(source, strlen(source));
    return source == NULL || *target != NULL;
}

static ZSharpGameObject *place_object(ModelParser *parser,
                                      const ZSharpGameObject *definition,
                                      const char *scene, char *display_name,
                                      float x, float y, float z,
                                      int has_z) {
    ZSharpGameObject *resized = (ZSharpGameObject *)realloc(
        parser->model->objects,
        (parser->model->object_count + 1) * sizeof(*resized));
    ZSharpGameObject *object;
    size_t index;
    if (resized == NULL) goto memory_error;
    parser->model->objects = resized;
    object = &resized[parser->model->object_count++];
    *object = *definition;
    object->name = object->display_name = object->source_file = NULL;
    object->scene = object->text = object->asset_path = object->audio_path = NULL;
    object->attribute_ids = NULL;
    object->attribute_active = NULL;
    object->attribute_count = 0;
    object->audio_stream = object->audio_buffer = NULL;
    object->audio_length = 0;
    if (!copy_optional(&object->name, definition->name) ||
        !copy_optional(&object->source_file, definition->source_file) ||
        !copy_optional(&object->scene, scene) ||
        !copy_optional(&object->text, definition->text) ||
        !copy_optional(&object->asset_path, definition->asset_path) ||
        !copy_optional(&object->audio_path, definition->audio_path))
        goto memory_error;
    object->display_name = display_name;
    display_name = NULL;
    if (definition->attribute_count != 0) {
        object->attribute_ids = (char **)calloc(definition->attribute_count,
                                                sizeof(char *));
        object->attribute_active = (int *)malloc(definition->attribute_count *
                                                 sizeof(int));
        if (object->attribute_ids == NULL || object->attribute_active == NULL)
            goto memory_error;
        object->attribute_count = definition->attribute_count;
        for (index = 0; index < definition->attribute_count; index++) {
            object->attribute_ids[index] = zsharp_copy_text(
                definition->attribute_ids[index],
                strlen(definition->attribute_ids[index]));
            if (object->attribute_ids[index] == NULL) goto memory_error;
            object->attribute_active[index] = definition->attribute_active[index];
        }
    }
    object->x = x;
    object->y = y;
    object->z = z;
    if (has_z) parser->model->is_3d = 1;
    return object;
memory_error:
    free(display_name);
    parser_fail(parser, &parser->current, "out of memory");
    return NULL;
}

static int json_location_number(ModelParser *parser, float *value) {
    ModelValue parsed;
    int ok;
    if (!parse_value(parser, &parsed)) return 0;
    /* JSON locations may be written as numbers or as quoted values, matching
     * the scene format's documented XLOCATION/YLOCATION placeholders. */
    if (parsed.type == MODEL_TEXT) parsed.type = MODEL_NUMBER;
    ok = value_number(parser, &parsed, value);
    free(parsed.text);
    return ok;
}

static int parse_scene_objects(ModelParser *parser, const char *scene_name) {
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after objects") ||
        !parser_expect_word(parser, "JSON") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after JSON") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before objects") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' before the JSON array")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        char *id = NULL;
        char *display_name = NULL;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        int has_z = 0;
        const ZSharpGameObject *definition;
        if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                                "expected '{' before a scene object") ||
            !json_key(parser, "id") || (id = json_text(parser, "object id")) == NULL ||
            !parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after the object id") ||
            !json_key(parser, "name") ||
            (display_name = json_text(parser, "object display name")) == NULL ||
            !parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after the object name") ||
            !json_key(parser, "location") ||
            !parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                                "expected '{' before location") ||
            !json_key(parser, "x") || !json_location_number(parser, &x) ||
            !parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after x") ||
            !json_key(parser, "y") || !json_location_number(parser, &y)) {
            free(id);
            free(display_name);
            return 0;
        }
        if (parser_match_type(parser, ZTOKEN_COMMA)) {
            if (!json_key(parser, "z") || !json_location_number(parser, &z)) {
                free(id);
                free(display_name);
                return 0;
            }
            has_z = 1;
        }
        if (!parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                                "expected '}' after location") ||
            !parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                                "expected '}' after the scene object")) {
            free(id);
            free(display_name);
            return 0;
        }
        definition = find_definition(parser->model, id);
        if (definition == NULL) {
            parser_fail(parser, &parser->current,
                        "scene references an unknown object id");
            free(id);
            free(display_name);
            return 0;
        }
        if (place_object(parser, definition, scene_name, display_name,
                         x, y, z, has_z) == NULL) {
            free(id);
            return 0;
        }
        free(id);
        if (!parser_match_type(parser, ZTOKEN_COMMA)) break;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "expected ']' after the object array") &&
           parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after objects");
}

static int parse_object_declaration(ModelParser *parser, char *name) {
    ZSharpGameObject *object;
    parser->definition_mode = 1;
    object = add_object(parser, name);
    if (object == NULL ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after the object id") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after the object id") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before object fields")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "an object field name");
        ModelValue value;
        if (field == NULL) return 0;
        if (strcmp(field, "attributes") == 0) {
            free(field);
            if (!parse_attributes(parser, object)) return 0;
            continue;
        }
        if (strcmp(field, "scene") == 0 || strcmp(field, "positionX") == 0 ||
            strcmp(field, "positionY") == 0 || strcmp(field, "positionZ") == 0) {
            parser_fail(parser, &parser->current,
                        "object placement belongs in the scene objects[JSON] block");
            free(field);
            return 0;
        }
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the field name") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the field value")) {
            free(field);
            return 0;
        }
        apply_object_field(parser, object, field, &value);
        free(value.text);
        free(field);
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after object fields");
}

static int parse_scene_declaration(ModelParser *parser, char *outer_name) {
    char *scene_name = NULL;
    ZSharpGameScene *scene;
    free(outer_name);
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after the scene declaration") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after the scene declaration") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before scene contents") ||
        !parser_expect_word(parser, "scene") ||
        (scene_name = parser_name(parser, "the scene name")) == NULL ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before scene fields")) {
        free(scene_name);
        return 0;
    }
    scene = add_scene(parser, scene_name);
    if (scene == NULL) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "a scene field name");
        ModelValue value;
        if (field == NULL ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the scene field") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the scene value")) {
            free(field);
            return 0;
        }
        apply_scene_field(parser, scene, field, &value);
        free(value.text);
        free(field);
    }
    if (!parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                            "expected ')' after scene fields") ||
        !parser_expect_word(parser, "objects") ||
        !parse_scene_objects(parser, scene->name) ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                            "expected ')' after scene contents")) return 0;
    return 1;
}

static int parse_model_file(const char *path, const char *source,
                            ZSharpGameModel *model, int is_scene,
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
        !parser_expect_word(&parser, is_scene ? "scene" : "object")) {
        if (!parser.failed)
            parser_fail(&parser, &parser.current,
                        is_scene ? "expected zsharp = type.scene"
                                 : "expected zsharp = type.object");
        free(source_name);
        return 0;
    }
    {
        char *name;
        if (!parser_match_word(&parser, "noticed") &&
            !parser_match_word(&parser, "silent")) {
            parser_fail(&parser, &parser.current,
                        "expected 'noticed' or 'silent'");
        } else if (!parser_match_word(&parser,
                                      is_scene ? "scene" : "object")) {
            parser_fail(&parser, &parser.current,
                        is_scene ? "expected 'scene'" : "expected 'object'");
        } else {
            name = parser_name(&parser,
                               is_scene ? "a scene name" : "an object name");
            if (name != NULL) {
                if (is_scene) parse_scene_declaration(&parser, name);
                else parse_object_declaration(&parser, name);
            }
        }
    }
    if (!parser.failed && parser.current.type != ZTOKEN_EOF)
        parser_fail(&parser, &parser.current,
                    is_scene ? "a .zscene file can define exactly one scene"
                             : "a .zobject file can define exactly one object");
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
        if (strcmp(model->objects[index].name, name) == 0 &&
            model->active_scene != NULL && model->objects[index].scene != NULL &&
            strcmp(model->objects[index].scene, model->active_scene) == 0)
            return &model->objects[index];
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

static int project_asset_exists(const char *root, const char *relative) {
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    int separator = root_length != 0 && root[root_length - 1] != '/' &&
                    root[root_length - 1] != '\\';
    char *path = (char *)malloc(root_length + (size_t)separator +
                               relative_length + 1);
    FILE *file;
    if (path == NULL) return 0;
    memcpy(path, root, root_length);
    if (separator) path[root_length++] = '/';
    memcpy(path + root_length, relative, relative_length + 1);
    file = fopen(path, "rb");
    free(path);
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

int zsharp_game_model_load(const char *project_root,
                           ZSharpGameModel *model, char *error,
                           size_t error_size) {
    ZSharpSourceList files;
    size_t index;
    ZSharpSourceList styles;
    ZSharpSettings settings;
    ZSharpDiagnostic settings_diagnostic;
    const char *start_file = NULL;
    memset(model, 0, sizeof(*model));
    zsharp_settings_init(&settings);
    model->project_root = zsharp_copy_text(project_root, strlen(project_root));
    if (model->project_root == NULL) goto out_of_memory;
    if (!zsharp_settings_load(project_root, &settings, &settings_diagnostic,
                              error, error_size)) goto failed;
    if (settings.game_start_scene != NULL) {
        const char *slash = strrchr(settings.game_start_scene, '/');
        const char *backslash = strrchr(settings.game_start_scene, '\\');
        start_file = settings.game_start_scene;
        if (slash != NULL && slash + 1 > start_file) start_file = slash + 1;
        if (backslash != NULL && backslash + 1 > start_file)
            start_file = backslash + 1;
    }
    if (!zsharp_project_list_files(project_root, ZSHARP_OBJECT_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_model_file(files.items[index], source, model, 0,
                              error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (!zsharp_project_list_files(project_root, ZSHARP_SCENE_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_model_file(files.items[index], source, model, 1,
                              error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (model->scene_count == 0) {
        model_error(error, error_size,
                    "game projects require at least one .zscene file");
        goto failed;
    }
    for (index = 0; index < model->scene_count; index++) {
        const char *icon = model->scenes[index].icon;
        size_t length;
        if (icon == NULL) continue;
        length = strlen(icon);
        if (length < 4 || strcmp(icon + length - 4, ".png") != 0 ||
            !project_asset_exists(project_root, icon)) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game scene '%s' requires an existing project-relative PNG icon",
                         model->scenes[index].name);
            goto failed;
        }
    }
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
    if (model->is_3d) {
        for (index = 0; index < model->scene_count; index++)
            if (model->scenes[index].camera_z == 0.0f)
                model->scenes[index].camera_z = 8.0f;
    }
    {
        ZSharpGameScene *startup = &model->scenes[0];
        if (start_file != NULL) {
            size_t base_length = strlen(start_file);
            size_t extension_length = strlen(ZSHARP_SCENE_EXTENSION);
            if (base_length > extension_length)
                base_length -= extension_length;
            for (index = 0; index < model->scene_count; index++) {
                if (strlen(model->scenes[index].source_file) == base_length &&
                    memcmp(model->scenes[index].source_file, start_file,
                           base_length) == 0) {
                    startup = &model->scenes[index];
                    break;
                }
            }
        }
        model->active_scene = zsharp_copy_text(startup->name,
                                                strlen(startup->name));
    }
    zsharp_settings_free(&settings);
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
        object->spawn_x = object->x;
        object->spawn_y = object->y;
        object->spawn_z = object->z;
        object->spawn_initialized = 1;
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
    zsharp_settings_free(&settings);
    zsharp_game_model_free(model);
    return 0;
}

int zsharp_game_model_validate(const char *project_root,
                               char *error, size_t error_size) {
    ZSharpGameModel model;
    int ok = zsharp_game_model_load(project_root, &model, error, error_size);
    if (ok) zsharp_game_model_free(&model);
    return ok;
}

static void free_game_object(ZSharpGameObject *object) {
    size_t index;
    free(object->name);
    free(object->display_name);
    free(object->source_file);
    free(object->scene);
    free(object->text);
    free(object->asset_path);
    free(object->audio_path);
    for (index = 0; index < object->attribute_count; index++)
        free(object->attribute_ids[index]);
    free(object->attribute_ids);
    free(object->attribute_active);
}

void zsharp_game_model_free(ZSharpGameModel *model) {
    size_t index;
    if (model == NULL) return;
    for (index = 0; index < model->scene_count; index++) {
        free(model->scenes[index].name);
        free(model->scenes[index].title);
        free(model->scenes[index].icon);
        free(model->scenes[index].source_file);
    }
    for (index = 0; index < model->object_count; index++)
        free_game_object(&model->objects[index]);
    for (index = 0; index < model->definition_count; index++)
        free_game_object(&model->definitions[index]);
    free(model->scenes);
    free(model->objects);
    free(model->definitions);
    free(model->active_scene);
    free(model->project_root);
    memset(model, 0, sizeof(*model));
}

static int same_active_scene(const ZSharpGameModel *model,
                             const ZSharpGameObject *object) {
    return model->active_scene != NULL && object->scene != NULL &&
           strcmp(model->active_scene, object->scene) == 0;
}

const char *zsharp_game_model_scene_title(const ZSharpGameModel *model) {
    ZSharpGameScene *scene = model == NULL
        ? NULL : find_scene(model, model->active_scene);
    return scene == NULL ? NULL : scene->title;
}

const char *zsharp_game_model_scene_icon(const ZSharpGameModel *model) {
    ZSharpGameScene *scene = model == NULL
        ? NULL : find_scene(model, model->active_scene);
    return scene == NULL ? NULL : scene->icon;
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

static int game_key_from_name(const char *name) {
    static const char *special[] = {
        "larrow", "rarrow", "uarrow", "darrow",
        "space", "enter", "escape", "tab", "backspace",
        "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt"
    };
    static const int values[] = {
        ZGAME_KEY_LARROW, ZGAME_KEY_RARROW, ZGAME_KEY_UARROW,
        ZGAME_KEY_DARROW, ZGAME_KEY_SPACE, ZGAME_KEY_ENTER,
        ZGAME_KEY_ESCAPE, ZGAME_KEY_TAB, ZGAME_KEY_BACKSPACE,
        ZGAME_KEY_LSHIFT, ZGAME_KEY_RSHIFT, ZGAME_KEY_LCTRL,
        ZGAME_KEY_RCTRL, ZGAME_KEY_LALT, ZGAME_KEY_RALT
    };
    size_t index;
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == '\0')
        return ZGAME_KEY_A + (name[0] - 'a');
    if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0')
        return ZGAME_KEY_0 + (name[0] - '0');
    if (name[0] == 'f' && name[1] == 'n') {
        char *end = NULL;
        long function_number = strtol(name + 2, &end, 10);
        if (end != name + 2 && *end == '\0' && function_number >= 1 &&
            function_number <= 24)
            return ZGAME_KEY_FN1 + (int)function_number - 1;
    }
    for (index = 0; index < sizeof(special) / sizeof(special[0]); index++)
        if (strcmp(name, special[index]) == 0) return values[index];
    return -1;
}

static int valid_object_field(const char *field) {
    static const char *fields[] = {
        "positionX","positionY","positionZ","width","height","depth",
        "rotation","scaleX","scaleY","scaleZ","velocityX","velocityY",
        "velocityZ","mass","gravityScale","restitution","friction",
        "audioVolume","tone",
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
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "key") == 0)
        return game_key_from_name(parts[2]) >= 0;
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "mouse") == 0)
        return strcmp(parts[2], "left") == 0 ||
               strcmp(parts[2], "right") == 0 ||
               strcmp(parts[2], "x") == 0 || strcmp(parts[2], "y") == 0;
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
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "key") == 0) {
        int key = game_key_from_name(field);
        *type = ZWINDOW_READ_STATUS;
        return status_property(model->input.keys[key], text, error,
                               error_size);
    }
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "mouse") == 0) {
        if (strcmp(field, "x") == 0 || strcmp(field, "y") == 0) {
            *type = ZWINDOW_READ_NUMBER;
            return number_property(strcmp(field, "x") == 0
                                       ? model->input.mouse_x
                                       : model->input.mouse_y,
                                   text, error, error_size);
        }
        *type = ZWINDOW_READ_STATUS;
        return status_property(strcmp(field, "left") == 0
                                   ? model->input.mouse_left
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
        size_t index;
        if (find_scene(model, value) == NULL) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size, "unknown game scene '%s'", value);
            return 0;
        }
        if (!replace_text(&model->active_scene, value)) return 0;
        for (index = 0; index < model->object_count; index++) {
            ZSharpGameObject *scene_object = &model->objects[index];
            if (scene_object->scene == NULL ||
                strcmp(scene_object->scene, value) != 0 ||
                !scene_object->spawn_initialized) continue;
            scene_object->x = scene_object->spawn_x;
            scene_object->y = scene_object->spawn_y;
            scene_object->z = scene_object->spawn_z;
            scene_object->velocity_x = 0.0f;
            scene_object->velocity_y = 0.0f;
            scene_object->velocity_z = 0.0f;
            scene_object->grounded = 0;
            scene_object->colliding = 0;
            scene_object->was_colliding = 0;
        }
        return 1;
    }
    if ((count == 3 && strcmp(parts[0], "input") == 0) ||
        (count == 2 && strcmp(parts[0], "Game") == 0)) {
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
