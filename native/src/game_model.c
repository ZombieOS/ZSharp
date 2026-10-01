#define _CRT_SECURE_NO_WARNINGS

#include "game_model.h"

#include "lexer.h"
#include "project.h"
#include "zsharp.h"
#include <ufbx.h>

#include <ctype.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZSHARP_COLLISION_TEST
typedef struct ZSharpCollisionTriangle {
    float point[3][3];
} ZSharpCollisionTriangle;

typedef struct ZSharpMeshCollision {
    char *path;
    ZSharpCollisionTriangle *triangles;
    size_t triangle_count;
    float source_extent[3];
    struct ZSharpMeshCollision *next;
} ZSharpMeshCollision;

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

typedef enum ModelFileKind {
    MODEL_FILE_OBJECT,
    MODEL_FILE_SCENE,
    MODEL_FILE_AUDIO,
    MODEL_FILE_MODEL,
    MODEL_FILE_AI
} ModelFileKind;

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
         strcmp(value->text, "dead") != 0 &&
         strcmp(value->text, "true") != 0 &&
         strcmp(value->text, "false") != 0)) {
        parser_fail(parser, &parser->current,
                    "field requires alive/dead or true/false");
        return 0;
    }
    *status = strcmp(value->text, "alive") == 0 ||
              strcmp(value->text, "true") == 0;
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
    object->opacity = 1.0f;
    object->roughness = 1.0f;
    object->visible = 1;
    object->audio_volume = 1.0f;
    object->audio_pitch = 1.0f;
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
    if (strcmp(field, "cameraRotationX") == 0) {
        parser->model->is_3d = 1;
        return value_number(parser, value, &scene->camera_rotation_x);
    }
    if (strcmp(field, "cameraRotationY") == 0) {
        parser->model->is_3d = 1;
        return value_number(parser, value, &scene->camera_rotation_y);
    }
    if (strcmp(field, "cameraRotationZ") == 0) {
        parser->model->is_3d = 1;
        return value_number(parser, value, &scene->camera_rotation_z);
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
        else if (strcmp(value->text, "cube") == 0) {
            object->shape = ZGAME_SHAPE_CUBE;
            parser->model->is_3d = 1;
            /* The 2D default is 64 pixels, while documented 3D coordinates
               use world units with the default camera at Z=8. A cube that
               keeps the 2D default surrounds the camera and cannot be
               meaningfully projected. Only replace dimensions the author
               did not explicitly provide. */
            if (!object->width_explicit) object->width = 1.0f;
            if (!object->height_explicit) object->height = 1.0f;
            if (!object->depth_explicit) object->depth = 1.0f;
        }
        else if (strcmp(value->text, "text") == 0)
            object->shape = ZGAME_SHAPE_TEXT;
        else if (strcmp(value->text, "button") == 0)
            object->shape = ZGAME_SHAPE_BUTTON;
        else {
            parser_fail(parser, &parser->current,
                        "shape must be rectangle, circle, triangle, sprite, cube, text, or button");
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
        else if (strcmp(value->text, "sphere") == 0) {
            object->collider = ZGAME_COLLIDER_SPHERE;
            parser->model->is_3d = 1;
        } else if (strcmp(value->text, "capsule") == 0) {
            object->collider = ZGAME_COLLIDER_CAPSULE;
            parser->model->is_3d = 1;
        } else if (strcmp(value->text, "mesh") == 0) {
            object->collider = ZGAME_COLLIDER_MESH;
            parser->model->is_3d = 1;
        }
        else {
            parser_fail(parser, &parser->current,
                        "collider must be none, box, circle, sphere, capsule, or mesh");
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
    if (strcmp(field, "width") == 0) {
        object->width_explicit = 1;
        return value_number(parser, value, &object->width);
    }
    if (strcmp(field, "height") == 0) {
        object->height_explicit = 1;
        return value_number(parser, value, &object->height);
    }
    if (strcmp(field, "depth") == 0 || strcmp(field, "length") == 0) {
        object->depth_explicit = 1;
        parser->model->is_3d = 1;
        return value_number(parser, value, &object->depth);
    }
    NUMBER_FIELD("rotation", rotation);
    NUMBER_FIELD_3D("rotationX", rotation_x);
    NUMBER_FIELD_3D("rotationY", rotation_y);
    NUMBER_FIELD_3D("rotationZ", rotation_z);
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
    if (strcmp(field, "hoverColor") == 0) {
        object->hover_color_explicit = 1;
        return value_color(parser, value, &object->hover_color);
    }
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
    object->click_left = object->click_right = NULL;
    object->mesh_path = NULL;
    object->material_names = NULL;
    object->material_textures = NULL;
    object->material_count = 0;
    object->part_poses = NULL;
    object->part_pose_count = 0;
    object->nav_aliases = object->nav_ids = NULL;
    object->nav_count = 0;
    object->nav_target = NULL;
    object->nav_path = NULL;
    object->nav_path_count = object->nav_path_step = 0;
    object->attribute_ids = NULL;
    object->attribute_active = NULL;
    object->attribute_count = 0;
    object->audio_stream = object->audio_buffer = NULL;
    object->audio_length = 0;
    if (!copy_optional(&object->name, definition->name) ||
        !copy_optional(&object->source_file, definition->source_file) ||
        !copy_optional(&object->scene, scene) ||
        !copy_optional(&object->text, definition->text) ||
        !copy_optional(&object->click_left, definition->click_left) ||
        !copy_optional(&object->click_right, definition->click_right) ||
        !copy_optional(&object->asset_path, definition->asset_path) ||
        !copy_optional(&object->mesh_path, definition->mesh_path) ||
        !copy_optional(&object->audio_path, definition->audio_path))
        goto memory_error;
    if (definition->material_count != 0) {
        object->material_names = (char **)calloc(definition->material_count,
                                                 sizeof(char *));
        object->material_textures = (char **)calloc(definition->material_count,
                                                    sizeof(char *));
        if (object->material_names == NULL ||
            object->material_textures == NULL) goto memory_error;
        object->material_count = definition->material_count;
        for (index = 0; index < definition->material_count; index++) {
            if (!copy_optional(&object->material_names[index],
                               definition->material_names[index]) ||
                !copy_optional(&object->material_textures[index],
                               definition->material_textures[index]))
                goto memory_error;
        }
    }
    if (definition->nav_count != 0) {
        object->nav_aliases = (char **)calloc(definition->nav_count,
                                              sizeof(char *));
        object->nav_ids = (char **)calloc(definition->nav_count,
                                          sizeof(char *));
        if (object->nav_aliases == NULL || object->nav_ids == NULL)
            goto memory_error;
        object->nav_count = definition->nav_count;
        for (index = 0; index < definition->nav_count; index++) {
            if (!copy_optional(&object->nav_aliases[index],
                               definition->nav_aliases[index]) ||
                !copy_optional(&object->nav_ids[index],
                               definition->nav_ids[index]))
                goto memory_error;
        }
    }
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

static int parse_surface_override(ModelParser *parser,
                                  ZSharpGameObject *object) {
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                            "expected '{' before surface")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACE) {
        char *field = json_text(parser, "surface field");
        float value = 0.0f;
        float *target = NULL;
        if (field == NULL) return 0;
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after surface field")) {
            free(field);
            return 0;
        }
        if (strcmp(field, "opacity") == 0) target = &object->opacity;
        else if (strcmp(field, "roughness") == 0) target = &object->roughness;
        else if (strcmp(field, "emissive") == 0) target = &object->emissive;
        else if (strcmp(field, "metallic") == 0) target = &object->metallic;
        if (target == NULL) {
            free(field);
            parser_fail(parser, &parser->current, "unknown surface field");
            return 0;
        }
        free(field);
        if (!json_location_number(parser, &value)) return 0;
        if (!isfinite(value) || value < 0.0f || value > 100.0f) {
            parser_fail(parser, &parser->current,
                        "surface values must be between 0 and 100");
            return 0;
        }
        *target = value / 100.0f;
        if (!parser_match_type(parser, ZTOKEN_COMMA)) break;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                              "expected '}' after surface");
}

static int parse_light_attributes(ModelParser *parser,
                                  ZSharpGameObject *object) {
    if (object->shape != ZGAME_SHAPE_LIGHT) {
        parser_fail(parser, &parser->current,
                    "light attributes require id 'light'");
        return 0;
    }
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                            "expected '{' before light attributes")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACE) {
        char *field = json_text(parser, "light attribute");
        int ok = 1;
        if (field == NULL) return 0;
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after light attribute")) {
            free(field);
            return 0;
        }
        if (strcmp(field, "light_type") == 0) {
            char *kind = json_text(parser, "light type");
            if (kind == NULL) ok = 0;
            else {
                object->light_type = strcmp(kind, "point") == 0 ? 1 :
                                     strcmp(kind, "spot") == 0 ? 2 :
                                     strcmp(kind, "directional") == 0 ? 3 : 0;
                if (object->light_type == 0) {
                    parser_fail(parser, &parser->current,
                                "light_type must be point, spot, or directional");
                    ok = 0;
                }
            }
            free(kind);
        } else if (strcmp(field, "intensity") == 0) {
            ok = json_location_number(parser, &object->light_intensity);
        } else if (strcmp(field, "range") == 0) {
            ok = json_location_number(parser, &object->light_range);
        } else if (strcmp(field, "angle") == 0) {
            ok = json_location_number(parser, &object->light_angle);
        } else if (strcmp(field, "color") == 0) {
            char *color = json_text(parser, "light color");
            if (color == NULL) ok = 0;
            else {
                ModelValue value;
                value.type = MODEL_COLOR;
                value.text = color;
                ok = value_color(parser, &value, &object->color);
            }
            free(color);
        } else if (strcmp(field, "castShadows") == 0) {
            if (parser_match_word(parser, "true")) object->cast_shadows = 1;
            else if (parser_match_word(parser, "false")) object->cast_shadows = 0;
            else {
                parser_fail(parser, &parser->current,
                            "castShadows must be true or false");
                ok = 0;
            }
        } else {
            parser_fail(parser, &parser->current, "unknown light attribute");
            ok = 0;
        }
        free(field);
        if (!ok) return 0;
        if (!parser_match_type(parser, ZTOKEN_COMMA)) break;
    }
    if (!parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                            "expected '}' after light attributes")) return 0;
    if (!isfinite(object->light_intensity) || object->light_intensity < 0.0f ||
        !isfinite(object->light_range) || object->light_range < 0.0f ||
        !isfinite(object->light_angle) || object->light_angle <= 0.0f ||
        object->light_angle > 180.0f) {
        parser_fail(parser, &parser->current,
                    "light intensity/range must be nonnegative and angle must be 0-180");
        return 0;
    }
    return 1;
}

static int parse_scene_object_override(ModelParser *parser,
                                       ZSharpGameObject *object) {
    char *field = json_text(parser, "scene object field name");
    int ok = 0;
    if (field == NULL) return 0;
    if (!parser_expect_type(parser, ZTOKEN_COLON,
                            "expected ':' after scene object field")) {
        free(field);
        return 0;
    }
    if (strcmp(field, "width") == 0) {
        ok = json_location_number(parser, &object->width);
    } else if (strcmp(field, "height") == 0) {
        ok = json_location_number(parser, &object->height);
    } else if (strcmp(field, "length") == 0 ||
               strcmp(field, "depth") == 0) {
        ok = json_location_number(parser, &object->depth);
    } else if (strcmp(field, "color") == 0) {
        char *color = json_text(parser, "object color");
        if (color != NULL) {
            ModelValue value;
            value.type = MODEL_COLOR;
            value.text = color;
            ok = value_color(parser, &value, &object->color);
            free(color);
        }
    } else if (strcmp(field, "texture") == 0) {
        char *texture = json_text(parser, "object texture path");
        if (texture != NULL) {
            ok = replace_text(&object->asset_path, texture);
            if (!ok) parser_fail(parser, &parser->current, "out of memory");
            free(texture);
        }
    } else if (strcmp(field, "surface") == 0) {
        ok = parse_surface_override(parser, object);
    } else if (strcmp(field, "attributes") == 0) {
        ok = parse_light_attributes(parser, object);
    } else if (strcmp(field, "rotations") == 0) {
        ok = parser_expect_type(parser, ZTOKEN_LEFT_BRACE,
                                "expected '{' before rotations") &&
             json_key(parser, "x") &&
             json_location_number(parser, &object->rotation_x) &&
             parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after rotation x") &&
             json_key(parser, "y") &&
             json_location_number(parser, &object->rotation_y) &&
             parser_expect_type(parser, ZTOKEN_COMMA,
                                "expected ',' after rotation y") &&
             json_key(parser, "z") &&
             json_location_number(parser, &object->rotation_z) &&
             parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                                "expected '}' after rotations");
    } else {
        parser_fail(parser, &parser->current,
                    "unknown scene object override (expected width, height, length, color, texture, surface, rotations, or light attributes)");
    }
    free(field);
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
        ZSharpGameObject *placed;
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
                                "expected '}' after location")) {
            free(id);
            free(display_name);
            return 0;
        }
        ZSharpGameObject builtin_light;
        ZSharpGameObject builtin_nav;
        memset(&builtin_light, 0, sizeof(builtin_light));
        builtin_light.name = "light";
        builtin_light.shape = ZGAME_SHAPE_LIGHT;
        builtin_light.body = ZGAME_BODY_STATIC;
        builtin_light.visible = 1;
        builtin_light.width = builtin_light.height = builtin_light.depth = 1.0f;
        builtin_light.mass = 1.0f;
        builtin_light.audio_pitch = builtin_light.audio_volume = 1.0f;
        builtin_light.scale_x = builtin_light.scale_y =
            builtin_light.scale_z = 1.0f;
        builtin_light.color = 0xffffffu;
        builtin_light.opacity = builtin_light.roughness = 1.0f;
        builtin_light.light_type = 1;
        builtin_light.light_intensity = 100.0f;
        builtin_light.light_range = 100.0f;
        builtin_light.light_angle = 45.0f;
        builtin_nav = builtin_light;
        builtin_nav.name = id;
        builtin_nav.shape = ZGAME_SHAPE_NAV;
        builtin_nav.visible = 0;
        builtin_nav.light_intensity = 0.0f;
        definition = strcmp(id, "light") == 0 ? &builtin_light :
                     strncmp(id, "nav:", 4) == 0 && id[4] != '\0'
                         ? &builtin_nav :
                     find_definition(parser->model, id);
        if (definition == NULL) {
            parser_fail(parser, &parser->current,
                        "scene references an unknown object or audio id");
            free(id);
            free(display_name);
            return 0;
        }
        placed = place_object(parser, definition, scene_name, display_name,
                              x, y, z, has_z);
        if (placed == NULL) {
            free(id);
            return 0;
        }
        free(id);
        while (parser_match_type(parser, ZTOKEN_COMMA)) {
            if (!parse_scene_object_override(parser, placed)) return 0;
        }
        if (!parser_expect_type(parser, ZTOKEN_RIGHT_BRACE,
                                "expected '}' after the scene object")) return 0;
        if (!parser_match_type(parser, ZTOKEN_COMMA)) break;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "expected ']' after the object array") &&
           parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after objects");
}

static int parse_button_events(ModelParser *parser, ZSharpGameObject *object) {
    if (object->event_defined) {
        parser_fail(parser, &parser->current, "duplicate event block");
        return 0;
    }
    object->event_defined = 1;
    if (!parser_expect_type(parser, ZTOKEN_LEFT_PAREN, "expected '(' after event")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN) {
        char *field = parser_name(parser, "left or right click action");
        char **target;
        ModelValue value;
        if (field == NULL) return 0;
        target = strcmp(field, "left") == 0 ? &object->click_left :
                 strcmp(field, "right") == 0 ? &object->click_right : NULL;
        free(field);
        if (target == NULL || *target != NULL) {
            parser_fail(parser, &parser->current, "event accepts one left and/or right action");
            return 0;
        }
        if (!parser_expect_type(parser, ZTOKEN_COLON, "expected ':' after click action") ||
            !parse_value(parser, &value)) return 0;
        if (value.type != MODEL_TEXT || value.text == NULL || value.text[0] == '\0') {
            free(value.text);
            parser_fail(parser, &parser->current, "click action requires a nonempty quoted File:Room:Function target");
            return 0;
        }
        *target = value.text;
        {
            const char *part = *target;
            int parts = 1;
            while (*part != '\0') {
                if (*part == ':') {
                    if (part == *target || part[-1] == ':' || part[1] == '\0') break;
                    ++parts;
                } else if (isspace((unsigned char)*part)) break;
                ++part;
            }
            if (*part != '\0' || (parts != 3 && parts != 4)) {
                parser_fail(parser, &parser->current, "click target must be File:Room:Function or Project:File:Room:Function");
                return 0;
            }
        }
        if (!parser_expect_type(parser, ZTOKEN_COLON, "expected ':' after click target")) return 0;
    }
    if (object->click_left == NULL && object->click_right == NULL) {
        parser_fail(parser, &parser->current, "event requires at least one click action");
        return 0;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN, "expected ')' after event");
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
        if (strcmp(field, "event") == 0) {
            free(field);
            if (!parse_button_events(parser, object)) return 0;
            continue;
        }
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
    if (object->shape == ZGAME_SHAPE_BUTTON && !object->event_defined) {
        parser_fail(parser, &parser->current, "shape: button requires an event block");
        return 0;
    }
    if (object->shape != ZGAME_SHAPE_BUTTON && object->event_defined) {
        parser_fail(parser, &parser->current, "event is only supported for shape: button");
        return 0;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after object fields");
}

static int parse_model_textures(ModelParser *parser, ZSharpGameObject *model) {
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after textures") ||
        !parser_expect_word(parser, "JSON") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after textures[JSON]") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before model textures")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN) {
        char *slot = json_text(parser, "material slot name");
        char *path = NULL;
        char **names, **textures;
        if (slot == NULL ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after material slot")) {
            free(slot);
            return 0;
        }
        path = json_text(parser, "texture path");
        if (path == NULL) { free(slot); return 0; }
        if (slot[0] == '\0' || !safe_relative_asset(path)) {
            free(slot); free(path);
            parser_fail(parser, &parser->current,
                        "model texture requires a named slot and safe relative path");
            return 0;
        }
        names = (char **)realloc(model->material_names,
             (model->material_count + 1) * sizeof(char *));
        if (names == NULL) { free(slot); free(path); return 0; }
        model->material_names = names;
        textures = (char **)realloc(model->material_textures,
             (model->material_count + 1) * sizeof(char *));
        if (textures == NULL) { free(slot); free(path); return 0; }
        model->material_textures = textures;
        model->material_names[model->material_count] = slot;
        model->material_textures[model->material_count++] = path;
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after model texture path")) return 0;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after model textures");
}

static int parse_model_declaration(ModelParser *parser, char *name) {
    ZSharpGameObject *model;
    parser->definition_mode = 1;
    model = add_object(parser, name);
    if (model == NULL ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after the model id") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after the model id") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before model fields")) return 0;
    model->shape = ZGAME_SHAPE_MESH;
    model->width = model->height = model->depth = 1.0f;
    parser->model->is_3d = 1;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "a model field name");
        ModelValue value;
        if (field == NULL) return 0;
        if (strcmp(field, "textures") == 0) {
            free(field);
            if (!parse_model_textures(parser, model)) return 0;
            continue;
        }
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after model field") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after model value")) {
            free(field);
            return 0;
        }
        if (strcmp(field, "parent") == 0) {
            if (value.type != MODEL_TEXT || !safe_relative_asset(value.text) ||
                !replace_text(&model->mesh_path, value.text))
                parser_fail(parser, &parser->current,
                            "model parent requires a quoted project-relative FBX path");
        } else if (strcmp(field, "shape") == 0) {
            parser_fail(parser, &parser->current,
                        "a .zmodel's shape is its imported mesh");
        } else {
            apply_object_field(parser, model, field, &value);
        }
        free(value.text);
        free(field);
    }
    if (model->mesh_path == NULL && !parser->failed)
        parser_fail(parser, &parser->current,
                    "model definitions require parent: \"Model.fbx\":");
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after model fields");
}

static int parse_ai_navigation(ModelParser *parser, ZSharpGameObject *ai) {
    if (!parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after Navigation") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after Navigation") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before Navigation points")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN) {
        char *alias = parser_name(parser, "a Navigation alias");
        char *id;
        char **aliases, **ids;
        if (alias == NULL ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after Navigation alias")) {
            free(alias);
            return 0;
        }
        id = json_text(parser, "navigation point id");
        if (id == NULL) { free(alias); return 0; }
        if (id[0] == '\0' ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after navigation point id")) {
            free(alias); free(id);
            return 0;
        }
        aliases = (char **)realloc(ai->nav_aliases,
            (ai->nav_count + 1) * sizeof(char *));
        if (aliases == NULL) { free(alias); free(id); return 0; }
        ai->nav_aliases = aliases;
        ids = (char **)realloc(ai->nav_ids,
            (ai->nav_count + 1) * sizeof(char *));
        if (ids == NULL) { free(alias); free(id); return 0; }
        ai->nav_ids = ids;
        ai->nav_aliases[ai->nav_count] = alias;
        ai->nav_ids[ai->nav_count++] = id;
    }
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after Navigation points");
}

static int parse_ai_declaration(ModelParser *parser, char *name) {
    ZSharpGameObject *ai;
    parser->definition_mode = 1;
    ai = add_object(parser, name);
    if (ai == NULL ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after AI id") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after AI id") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before AI fields")) return 0;
    ai->is_ai = 1;
    ai->shape = ZGAME_SHAPE_MESH;
    ai->body = ZGAME_BODY_DYNAMIC;
    ai->collider = ZGAME_COLLIDER_BOX;
    ai->width = ai->height = ai->depth = 1.0f;
    parser->model->is_3d = 1;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "an AI field name");
        ModelValue value;
        if (field == NULL) return 0;
        if (strcmp(field, "Navigation") == 0) {
            free(field);
            if (!parse_ai_navigation(parser, ai)) return 0;
            continue;
        }
        if (!parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after AI field") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after AI value")) {
            free(field);
            return 0;
        }
        if (strcmp(field, "model") == 0) {
            const char *stem, *dot;
            ZSharpGameObject *source;
            char name_buffer[256];
            size_t length;
            if (value.type != MODEL_TEXT || !safe_relative_asset(value.text)) {
                parser_fail(parser, &parser->current,
                            "AI model requires a quoted .zmodel path");
            } else {
                stem = strrchr(value.text, '/');
                stem = stem == NULL ? value.text : stem + 1;
                dot = strrchr(stem, '.');
                length = dot == NULL ? strlen(stem) : (size_t)(dot - stem);
                if (length == 0 || length >= sizeof(name_buffer)) {
                    parser_fail(parser, &parser->current, "invalid AI model name");
                } else {
                    memcpy(name_buffer, stem, length);
                    name_buffer[length] = '\0';
                    source = find_definition(parser->model, name_buffer);
                    if (source == NULL || source->shape != ZGAME_SHAPE_MESH ||
                        !replace_text(&ai->mesh_path, source->mesh_path)) {
                        parser_fail(parser, &parser->current,
                                    "AI model must reference a loaded .zmodel");
                    } else {
                        size_t slot;
                        ai->material_names = (char **)calloc(source->material_count,
                                                               sizeof(char *));
                        ai->material_textures = (char **)calloc(source->material_count,
                                                                  sizeof(char *));
                        if (source->material_count != 0 &&
                            (ai->material_names == NULL ||
                             ai->material_textures == NULL))
                            parser_fail(parser, &parser->current, "out of memory");
                        else {
                            ai->material_count = source->material_count;
                            for (slot = 0; slot < source->material_count; slot++) {
                                if (!copy_optional(&ai->material_names[slot],
                                        source->material_names[slot]) ||
                                    !copy_optional(&ai->material_textures[slot],
                                        source->material_textures[slot])) {
                                    parser_fail(parser, &parser->current,
                                                "out of memory");
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            apply_object_field(parser, ai, field, &value);
        }
        free(value.text);
        free(field);
    }
    if (ai->mesh_path == NULL && !parser->failed)
        parser_fail(parser, &parser->current,
                    "AI definitions require model: \"Models/AI.zmodel\":");
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after AI fields");
}

static int parse_audio_declaration(ModelParser *parser, char *name) {
    ZSharpGameObject *audio;
    parser->definition_mode = 1;
    audio = add_object(parser, name);
    if (audio == NULL ||
        !parser_expect_type(parser, ZTOKEN_LEFT_BRACKET,
                            "expected '[' after the audio id") ||
        !parser_expect_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "expected ']' after the audio id") ||
        !parser_expect_type(parser, ZTOKEN_LEFT_PAREN,
                            "expected '(' before audio fields")) return 0;
    audio->visible = 0;
    audio->is_audio_source = 1;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *field = parser_name(parser, "an audio field name");
        ModelValue value;
        float number;
        if (field == NULL ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the audio field") ||
            !parse_value(parser, &value) ||
            !parser_expect_type(parser, ZTOKEN_COLON,
                                "expected ':' after the audio value")) {
            free(field);
            return 0;
        }
        if (strcmp(field, "source") == 0) {
            if (value.type != MODEL_TEXT ||
                !replace_text(&audio->audio_path, value.text))
                parser_fail(parser, &parser->current,
                            "audio source requires a quoted path");
        } else if (strcmp(field, "volume") == 0) {
            if (value_number(parser, &value, &number))
                audio->audio_volume = number / 100.0f;
        } else if (strcmp(field, "pitch") == 0) {
            if (value_number(parser, &value, &number))
                audio->audio_pitch = number / 100.0f;
        } else if (strcmp(field, "loop") == 0) {
            value_status(parser, &value, &audio->audio_loop);
        } else {
            parser_fail(parser, &parser->current, "unknown audio field");
        }
        free(value.text);
        free(field);
    }
    if (audio->audio_path == NULL && !parser->failed)
        parser_fail(parser, &parser->current,
                    "audio definitions require a source field");
    return parser_expect_type(parser, ZTOKEN_RIGHT_PAREN,
                              "expected ')' after audio fields");
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
                            ZSharpGameModel *model, ModelFileKind kind,
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
        !parser_expect_word(&parser, kind == MODEL_FILE_AI
                                      ? "ai"
                                      : kind == MODEL_FILE_AUDIO ||
                                      kind == MODEL_FILE_MODEL
                                      ? "script"
                                      : kind == MODEL_FILE_SCENE
                                            ? "scene" : "object") ||
        ((kind == MODEL_FILE_AUDIO || kind == MODEL_FILE_MODEL) &&
         (!parser_expect_type(&parser, ZTOKEN_COLON,
                              "expected ':' after type.script") ||
          !parser_expect_word(&parser,
              kind == MODEL_FILE_MODEL ? "model" : "audio")))) {
        if (!parser.failed)
            parser_fail(&parser, &parser.current,
                        kind == MODEL_FILE_AI
                            ? "expected zsharp = type.ai"
                            : kind == MODEL_FILE_MODEL
                            ? "expected zsharp = type.script:model"
                            : kind == MODEL_FILE_AUDIO
                            ? "expected zsharp = type.script:audio"
                            : kind == MODEL_FILE_SCENE
                                  ? "expected zsharp = type.scene"
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
        } else if (!parser_match_word(
                       &parser, kind == MODEL_FILE_AI
                                      ? "ai"
                                      : kind == MODEL_FILE_MODEL
                                      ? "model"
                                      : kind == MODEL_FILE_AUDIO
                                      ? "audio"
                                      : kind == MODEL_FILE_SCENE
                                            ? "scene" : "object")) {
            parser_fail(&parser, &parser.current,
                        kind == MODEL_FILE_AI
                            ? "expected 'ai'"
                            : kind == MODEL_FILE_MODEL
                            ? "expected 'model'"
                            : kind == MODEL_FILE_AUDIO
                            ? "expected 'audio'"
                            : kind == MODEL_FILE_SCENE
                                  ? "expected 'scene'" : "expected 'object'");
        } else {
            name = parser_name(
                &parser, kind == MODEL_FILE_AI
                             ? "an AI name"
                             : kind == MODEL_FILE_MODEL
                             ? "a model name"
                             : kind == MODEL_FILE_AUDIO
                             ? "an audio name"
                             : kind == MODEL_FILE_SCENE
                                   ? "a scene name" : "an object name");
            if (name != NULL) {
                if (kind == MODEL_FILE_SCENE)
                    parse_scene_declaration(&parser, name);
                else if (kind == MODEL_FILE_AI)
                    parse_ai_declaration(&parser, name);
                else if (kind == MODEL_FILE_MODEL)
                    parse_model_declaration(&parser, name);
                else if (kind == MODEL_FILE_AUDIO)
                    parse_audio_declaration(&parser, name);
                else
                    parse_object_declaration(&parser, name);
            }
        }
    }
    if (!parser.failed && parser.current.type != ZTOKEN_EOF)
        parser_fail(&parser, &parser.current,
                    kind == MODEL_FILE_AI
                        ? "a .zai file can define exactly one AI"
                        : kind == MODEL_FILE_MODEL
                        ? "a .zmodel file can define exactly one model"
                        : kind == MODEL_FILE_AUDIO
                        ? "a .zaudio file can define exactly one audio source"
                        : kind == MODEL_FILE_SCENE
                              ? "a .zscene file can define exactly one scene"
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
    /* Built-in lights all have id "light". A unique identifier-style scene
       name gives scripts a way to move or toggle a particular light. */
    for (index = 0; index < model->object_count; index++)
        if (model->objects[index].display_name != NULL &&
            strcmp(model->objects[index].display_name, name) == 0 &&
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

static int wav_asset(const char *path) {
    size_t length = path == NULL ? 0 : strlen(path);
    const char *extension;
    if (length < 4) return 0;
    extension = path + length - 4;
    return extension[0] == '.' &&
           tolower((unsigned char)extension[1]) == 'w' &&
           tolower((unsigned char)extension[2]) == 'a' &&
           tolower((unsigned char)extension[3]) == 'v';
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
            !parse_model_file(files.items[index], source, model,
                              MODEL_FILE_OBJECT,
                              error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (!zsharp_project_list_files(project_root, ZSHARP_MODEL_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_model_file(files.items[index], source, model,
                              MODEL_FILE_MODEL, error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (!zsharp_project_list_files(project_root, ZSHARP_AI_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_model_file(files.items[index], source, model,
                              MODEL_FILE_AI, error, error_size)) {
            free(source);
            zsharp_project_source_list_free(&files);
            goto failed;
        }
        free(source);
    }
    zsharp_project_source_list_free(&files);
    if (!zsharp_project_list_files(project_root, ZSHARP_AUDIO_EXTENSION,
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        char *source = NULL;
        if (!read_file(files.items[index], &source, error, error_size) ||
            !parse_model_file(files.items[index], source, model,
                              MODEL_FILE_AUDIO,
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
            !parse_model_file(files.items[index], source, model,
                              MODEL_FILE_SCENE,
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
    if (!zsharp_project_list_files(project_root, ".zanimation",
                                   &files, error, error_size)) goto failed;
    for (index = 0; index < files.count; index++) {
        if (!zsharp_game_animation_load(model, files.items[index], error,
                                        error_size)) {
            zsharp_project_source_list_free(&files);
            goto failed;
        }
    }
    zsharp_project_source_list_free(&files);
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
            object->audio_volume > 1.0f || object->audio_pitch <= 0.0f ||
            object->audio_pitch > 4.0f) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' has invalid size, mass, scale, volume, or pitch",
                         object->name);
            goto failed;
        }
        if (object->collider == ZGAME_COLLIDER_MESH &&
            (object->shape != ZGAME_SHAPE_MESH ||
             object->body != ZGAME_BODY_STATIC)) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "mesh collider '%s' requires a static .zmodel object",
                         object->name);
            goto failed;
        }
        if ((object->shape == ZGAME_SHAPE_SPRITE &&
             !safe_relative_asset(object->asset_path)) ||
            (object->asset_path != NULL &&
             !safe_relative_asset(object->asset_path)) ||
            (object->mesh_path != NULL &&
             !safe_relative_asset(object->mesh_path)) ||
            (object->audio_path != NULL &&
             !safe_relative_asset(object->audio_path))) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "game object '%s' requires a safe project-relative asset path",
                         object->name);
            goto failed;
        }
        if (object->shape == ZGAME_SHAPE_MESH) {
            size_t root_length, mesh_length, slot;
            char *full_path;
            ufbx_scene *imported;
            ufbx_error import_error;
            if (object->mesh_path == NULL ||
                !project_asset_exists(project_root, object->mesh_path)) {
                if (error != NULL && error_size != 0)
                    snprintf(error, error_size,
                             "model '%s' requires an existing parent FBX asset",
                             object->name);
                goto failed;
            }
            for (slot = 0; slot < object->material_count; slot++) {
                if (!project_asset_exists(project_root,
                                          object->material_textures[slot])) {
                    if (error != NULL && error_size != 0)
                        snprintf(error, error_size,
                                 "model '%s' texture slot '%s' has no file",
                                 object->name, object->material_names[slot]);
                    goto failed;
                }
            }
            root_length = strlen(project_root);
            mesh_length = strlen(object->mesh_path);
            full_path = (char *)malloc(root_length + mesh_length + 2);
            if (full_path == NULL) goto out_of_memory;
            snprintf(full_path, root_length + mesh_length + 2, "%s/%s",
                     project_root, object->mesh_path);
            imported = ufbx_load_file(full_path, NULL, &import_error);
            free(full_path);
            if (imported == NULL) {
                if (error != NULL && error_size != 0)
                    snprintf(error, error_size,
                             "model '%s' has an invalid FBX parent: %.180s",
                             object->name, import_error.description.data);
                goto failed;
            }
            if (imported->meshes.count == 0) {
                ufbx_free_scene(imported);
                if (error != NULL && error_size != 0)
                    snprintf(error, error_size,
                             "model '%s' has no renderable FBX mesh",
                             object->name);
                goto failed;
            }
            ufbx_free_scene(imported);
        }
        if (object->is_audio_source && !wav_asset(object->audio_path)) {
            if (error != NULL && error_size != 0)
                snprintf(error, error_size,
                         "audio source '%s' requires a project-relative WAV source",
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
    free(object->click_left);
    free(object->click_right);
    free(object->asset_path);
    free(object->mesh_path);
    for (index = 0; index < object->material_count; index++) {
        free(object->material_names[index]);
        free(object->material_textures[index]);
    }
    free(object->material_names);
    free(object->material_textures);
    free(object->part_poses);
    for (index = 0; index < object->nav_count; index++) {
        free(object->nav_aliases[index]);
        free(object->nav_ids[index]);
    }
    free(object->nav_aliases);
    free(object->nav_ids);
    free(object->nav_target);
    free(object->nav_path);
    free(object->audio_path);
    for (index = 0; index < object->attribute_count; index++)
        free(object->attribute_ids[index]);
    free(object->attribute_ids);
    free(object->attribute_active);
}

void zsharp_game_model_free(ZSharpGameModel *model) {
    size_t index;
    ZSharpMeshCollision *collision;
    if (model == NULL) return;
    collision = model->mesh_collisions;
    while (collision != NULL) {
        ZSharpMeshCollision *next = collision->next;
        free(collision->path);
        free(collision->triangles);
        free(collision);
        collision = next;
    }
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
    zsharp_game_animation_free(model);
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

static ZSharpMeshCollision *collision_mesh(ZSharpGameModel *model,
                                            const char *relative) {
    ZSharpMeshCollision *cache;
    ufbx_scene *source;
    char *full_path;
    size_t node_index;
    if (relative == NULL || model->project_root == NULL) return NULL;
    for (cache = model->mesh_collisions; cache != NULL; cache = cache->next)
        if (strcmp(cache->path, relative) == 0) return cache;
    full_path = (char *)malloc(strlen(model->project_root) +
                               strlen(relative) + 2);
    if (full_path == NULL) return NULL;
    sprintf(full_path, "%s/%s", model->project_root, relative);
    source = ufbx_load_file(full_path, NULL, NULL);
    free(full_path);
    if (source == NULL) return NULL;
    cache = (ZSharpMeshCollision *)calloc(1, sizeof(*cache));
    if (cache == NULL) { ufbx_free_scene(source); return NULL; }
    cache->path = zsharp_copy_text(relative, strlen(relative));
    if (cache->path == NULL) goto failed;
    for (node_index = 0; node_index < source->nodes.count; node_index++) {
        const ufbx_node *node = source->nodes.data[node_index];
        const ufbx_mesh *mesh = node->mesh;
        size_t face_index;
        if (mesh == NULL) continue;
        for (face_index = 0; face_index < mesh->faces.count; face_index++) {
            ufbx_face face = mesh->faces.data[face_index];
            uint32_t *indices;
            uint32_t count, triangle;
            ZSharpCollisionTriangle *resized;
            if (face.num_indices < 3 || face.num_indices > 100000) continue;
            indices = (uint32_t *)malloc((face.num_indices - 2) * 3 *
                                          sizeof(uint32_t));
            if (indices == NULL) goto failed;
            count = ufbx_triangulate_face(indices,
                (face.num_indices - 2) * 3, mesh, face);
            if (cache->triangle_count + count > 200000) {
                free(indices); goto failed;
            }
            resized = (ZSharpCollisionTriangle *)realloc(cache->triangles,
                (cache->triangle_count + count) * sizeof(*resized));
            if (resized == NULL && count != 0) { free(indices); goto failed; }
            if (count != 0) cache->triangles = resized;
            for (triangle = 0; triangle < count; triangle++) {
                ZSharpCollisionTriangle *destination =
                    &cache->triangles[cache->triangle_count + triangle];
                int corner;
                for (corner = 0; corner < 3; corner++) {
                    uint32_t vertex_index = indices[triangle * 3 + corner];
                    ufbx_vec3 vertex = ufbx_get_vertex_vec3(
                        &mesh->vertex_position, vertex_index);
                    ufbx_vec3 position = ufbx_transform_position(
                        &node->geometry_to_world, vertex);
                    destination->point[corner][0] = (float)position.x;
                    destination->point[corner][1] = (float)position.y;
                    destination->point[corner][2] = (float)position.z;
                }
            }
            cache->triangle_count += count;
            free(indices);
        }
    }
    if (cache->triangle_count > 0) {
        float low[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float high[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        size_t triangle;
        int axis, corner;
        for (triangle = 0; triangle < cache->triangle_count; triangle++)
            for (corner = 0; corner < 3; corner++)
                for (axis = 0; axis < 3; axis++) {
                    float value = cache->triangles[triangle].point[corner][axis];
                    if (value < low[axis]) low[axis] = value;
                    if (value > high[axis]) high[axis] = value;
                }
        for (axis = 0; axis < 3; axis++) {
            float extent = high[axis] - low[axis];
            cache->source_extent[axis] = extent > 0.000001f ? extent : 1.0f;
        }
    }
    ufbx_free_scene(source);
    cache->next = model->mesh_collisions;
    model->mesh_collisions = cache;
    return cache;
failed:
    ufbx_free_scene(source);
    free(cache->path);
    free(cache->triangles);
    free(cache);
    return NULL;
}

#endif

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

typedef struct CollisionBox {
    float center[3];
    float axis[3][3];
    float half[3];
} CollisionBox;

/* Match the renderer's X, Y, Z Euler order (and legacy cube yaw). */
static void collision_box(const ZSharpGameObject *object, CollisionBox *box) {
    float rx = object->rotation_x * 0.017453292519943295f;
    float ry = (object->rotation_y + object->rotation) * 0.017453292519943295f;
    float rz = object->rotation_z * 0.017453292519943295f;
    float sx = sinf(rx), cx = cosf(rx);
    float sy = sinf(ry), cy = cosf(ry);
    float sz = sinf(rz), cz = cosf(rz);
    box->center[0] = object->x;
    box->center[1] = object->y;
    box->center[2] = object->z;
    box->half[0] = fabsf(object->width * object->scale_x) * 0.5f;
    box->half[1] = fabsf(object->height * object->scale_y) * 0.5f;
    box->half[2] = fabsf(object->depth * object->scale_z) * 0.5f;
    box->axis[0][0] = cz*cy;
    box->axis[0][1] = sz*cy;
    box->axis[0][2] = -sy;
    box->axis[1][0] = cz*sy*sx-sz*cx;
    box->axis[1][1] = sz*sy*sx+cz*cx;
    box->axis[1][2] = cy*sx;
    box->axis[2][0] = cz*sy*cx+sz*sx;
    box->axis[2][1] = sz*sy*cx-cz*sx;
    box->axis[2][2] = cy*cx;
}

/* Separating-axis test for two oriented boxes. Normal points from b to a.
 * Cross-product axes are necessary when both boxes rotate. */
static int box_contact(const ZSharpGameObject *a, const ZSharpGameObject *b,
                       float normal[3], float *penetration) {
    CollisionBox first, second;
    float delta[3], best = INFINITY;
    int axis_index;
    collision_box(a, &first);
    collision_box(b, &second);
    for (axis_index = 0; axis_index < 3; axis_index++)
        delta[axis_index] = first.center[axis_index] - second.center[axis_index];
    for (axis_index = 0; axis_index < 15; axis_index++) {
        float axis[3], length, distance, reach_a = 0.0f, reach_b = 0.0f;
        float overlap;
        int index;
        if (axis_index < 3) memcpy(axis, first.axis[axis_index], sizeof(axis));
        else if (axis_index < 6)
            memcpy(axis, second.axis[axis_index-3], sizeof(axis));
        else {
            const float *u = first.axis[(axis_index-6)/3];
            const float *v = second.axis[(axis_index-6)%3];
            axis[0] = u[1]*v[2]-u[2]*v[1];
            axis[1] = u[2]*v[0]-u[0]*v[2];
            axis[2] = u[0]*v[1]-u[1]*v[0];
        }
        length = sqrtf(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
        if (length < 0.00001f) continue;
        for (index = 0; index < 3; index++) axis[index] /= length;
        distance = delta[0]*axis[0]+delta[1]*axis[1]+delta[2]*axis[2];
        for (index = 0; index < 3; index++) {
            const float *u = first.axis[index], *v = second.axis[index];
            reach_a += first.half[index]*fabsf(u[0]*axis[0]+u[1]*axis[1]+u[2]*axis[2]);
            reach_b += second.half[index]*fabsf(v[0]*axis[0]+v[1]*axis[1]+v[2]*axis[2]);
        }
        overlap = reach_a + reach_b - fabsf(distance);
        if (overlap <= 0.0f) return 0;
        if (overlap < best) {
            best = overlap;
            for (index = 0; index < 3; index++)
                normal[index] = distance >= 0.0f ? axis[index] : -axis[index];
        }
    }
    *penetration = best;
    return isfinite(best);
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
        if (above) {
            dynamic->x += other->motion_x;
            dynamic->z += other->motion_z;
        }
    } else {
        dynamic->z += dynamic->z < other->z ? -overlap_z : overlap_z;
        dynamic->velocity_z = -dynamic->velocity_z * bounce;
    }
}

static float collision_clamp(float value, float low, float high) {
    return fminf(fmaxf(value, low), high);
}

static int round_collider(const ZSharpGameObject *object) {
    return object->collider == ZGAME_COLLIDER_SPHERE ||
           object->collider == ZGAME_COLLIDER_CAPSULE;
}

static float round_radius(const ZSharpGameObject *object) {
    float x = fabsf(object->width * object->scale_x) * 0.5f;
    float z = fabsf(object->depth * object->scale_z) * 0.5f;
    float y = fabsf(object->height * object->scale_y) * 0.5f;
    return object->collider == ZGAME_COLLIDER_SPHERE ?
        fminf(x, fminf(y, z)) : fminf(x, z);
}

static float capsule_segment(const ZSharpGameObject *object, float radius) {
    if (object->collider != ZGAME_COLLIDER_CAPSULE) return 0.0f;
    return fmaxf(0.0f,
        fabsf(object->height * object->scale_y) * 0.5f - radius);
}

static float round_box_distance(const float origin[3], const float direction[3],
                                const CollisionBox *box, float height,
                                float closest[3], float point[3]) {
    float distance = 0.0f;
    int axis;
    for (axis = 0; axis < 3; axis++) {
        point[axis] = origin[axis] + direction[axis] * height;
        closest[axis] = collision_clamp(point[axis], -box->half[axis],
                                        box->half[axis]);
        distance += (point[axis]-closest[axis]) *
                    (point[axis]-closest[axis]);
    }
    return distance;
}

static int round_box_contact(const ZSharpGameObject *a,
                             const ZSharpGameObject *b,
                             float normal[3], float *penetration) {
    const ZSharpGameObject *round = round_collider(a) ? a : b;
    const ZSharpGameObject *other = round == a ? b : a;
    CollisionBox box;
    float radius = round_radius(round);
    float segment = capsule_segment(round, radius);
    float origin[3], direction[3], closest[3], point[3], local_normal[3];
    float low = -segment, high = segment, distance, length;
    int axis, iteration;
    if (radius <= 0.0f) return 0;
    collision_box(other, &box);
    for (axis = 0; axis < 3; axis++) {
        origin[axis] = (round->x-box.center[0])*box.axis[axis][0] +
                       (round->y-box.center[1])*box.axis[axis][1] +
                       (round->z-box.center[2])*box.axis[axis][2];
        direction[axis] = box.axis[axis][1];
    }
    /* Squared distance from a line segment to a convex box is convex. */
    for (iteration = 0; iteration < 28; iteration++) {
        float first = low + (high-low)/3.0f;
        float second = high - (high-low)/3.0f;
        float unused_a[3], unused_b[3];
        float d1 = round_box_distance(origin, direction, &box, first,
                                      unused_a, unused_b);
        float d2 = round_box_distance(origin, direction, &box, second,
                                      unused_a, unused_b);
        if (d1 < d2) high = second;
        else low = first;
    }
    distance = sqrtf(round_box_distance(origin, direction, &box,
                                        (low+high)*0.5f, closest, point));
    if (distance >= radius) return 0;
    if (distance > 0.00001f) {
        for (axis = 0; axis < 3; axis++)
            local_normal[axis] = (point[axis]-closest[axis])/distance;
        *penetration = radius-distance;
    } else {
        int nearest = 0;
        float exit = INFINITY;
        for (axis = 0; axis < 3; axis++) {
            float candidate = box.half[axis]-fabsf(point[axis]);
            if (candidate < exit) { exit = candidate; nearest = axis; }
        }
        local_normal[0] = local_normal[1] = local_normal[2] = 0.0f;
        local_normal[nearest] = point[nearest] >= 0.0f ? 1.0f : -1.0f;
        *penetration = radius+exit;
    }
    for (axis = 0; axis < 3; axis++)
        normal[axis] = local_normal[0]*box.axis[0][axis] +
                       local_normal[1]*box.axis[1][axis] +
                       local_normal[2]*box.axis[2][axis];
    length = sqrtf(normal[0]*normal[0] + normal[1]*normal[1] +
                   normal[2]*normal[2]);
    if (length < 0.00001f) return 0;
    for (axis = 0; axis < 3; axis++)
        normal[axis] = (round == a ? normal[axis] : -normal[axis])/length;
    return 1;
}

/* Contact normal points from b into a. Boxes retain the existing AABB
 * resolution path; round shapes use true radial distance to avoid false
 * contacts at box corners. */
static int round_contact(const ZSharpGameObject *a,
                         const ZSharpGameObject *b,
                         float normal[3], float *penetration) {
    const ZSharpGameObject *round = round_collider(a) ? a : b;
    const ZSharpGameObject *other = round == a ? b : a;
    float radius = round_radius(round);
    float segment = capsule_segment(round, radius);
    float dx, dy, dz, distance;
    if (radius <= 0.0f) return 0;
    if (round_collider(other)) {
        float other_radius = round_radius(other);
        float other_segment = capsule_segment(other, other_radius);
        float first_y = collision_clamp(other->y, round->y - segment,
                                         round->y + segment);
        float second_y = collision_clamp(first_y,
                                          other->y - other_segment,
                                          other->y + other_segment);
        first_y = collision_clamp(second_y, round->y - segment,
                                   round->y + segment);
        dx = round->x - other->x;
        dy = first_y - second_y;
        dz = round->z - other->z;
        radius += other_radius;
    } else {
        float half_x = fabsf(other->width * other->scale_x) * 0.5f;
        float half_y = fabsf(other->height * other->scale_y) * 0.5f;
        float half_z = fabsf(other->depth * other->scale_z) * 0.5f;
        float sample_y = collision_clamp(other->y,
                                          round->y - segment,
                                          round->y + segment);
        dx = round->x - collision_clamp(round->x,
                                         other->x - half_x,
                                         other->x + half_x);
        dy = sample_y - collision_clamp(sample_y,
                                         other->y - half_y,
                                         other->y + half_y);
        dz = round->z - collision_clamp(round->z,
                                         other->z - half_z,
                                         other->z + half_z);
        if (dx == 0.0f && dy == 0.0f && dz == 0.0f) {
            float exits[3] = {
                half_x - fabsf(round->x - other->x),
                half_y + segment - fabsf(round->y - other->y),
                half_z - fabsf(round->z - other->z)
            };
            int axis = exits[0] < exits[1] ? 0 : 1;
            if (exits[2] < exits[axis]) axis = 2;
            normal[0] = normal[1] = normal[2] = 0.0f;
            normal[axis] = (axis == 0 ? round->x >= other->x :
                            axis == 1 ? round->y >= other->y :
                                        round->z >= other->z) ? 1.0f : -1.0f;
            if (round != a) normal[axis] = -normal[axis];
            *penetration = fmaxf(0.0f, exits[axis]) + radius;
            return *penetration > 0.0f;
        }
    }
    distance = sqrtf(dx*dx + dy*dy + dz*dz);
    if (distance >= radius) return 0;
    if (distance < 0.00001f) {
        normal[0] = normal[2] = 0.0f;
        normal[1] = round->y >= other->y ? 1.0f : -1.0f;
    } else {
        normal[0] = dx / distance;
        normal[1] = dy / distance;
        normal[2] = dz / distance;
    }
    if (round != a) {
        normal[0] = -normal[0];
        normal[1] = -normal[1];
        normal[2] = -normal[2];
    }
    *penetration = radius - distance;
    return 1;
}

static void resolve_round_collision(ZSharpGameObject *dynamic,
                                    const ZSharpGameObject *other,
                                    const float normal[3], float penetration) {
    float motion = dynamic->velocity_x * normal[0] +
                   dynamic->velocity_y * normal[1] +
                   dynamic->velocity_z * normal[2];
    dynamic->x += normal[0] * penetration;
    dynamic->y += normal[1] * penetration;
    dynamic->z += normal[2] * penetration;
    if (normal[1] > 0.5f && dynamic->velocity_y <= 0.0f) {
        dynamic->grounded = 1;
        dynamic->velocity_x *= 1.0f - collision_clamp(dynamic->friction, 0, 1);
        dynamic->velocity_z *= 1.0f - collision_clamp(dynamic->friction, 0, 1);
        dynamic->x += other->motion_x;
        dynamic->z += other->motion_z;
    }
    if (motion < 0.0f) {
        float impulse = (1.0f + dynamic->restitution) * motion;
        dynamic->velocity_x -= impulse * normal[0];
        dynamic->velocity_y -= impulse * normal[1];
        dynamic->velocity_z -= impulse * normal[2];
    }
}

#ifndef ZSHARP_COLLISION_TEST
static void transform_collision_point(const ZSharpGameObject *object,
                                      const ZSharpMeshCollision *mesh,
                                      const float input[3], float output[3]) {
    float angle, sine, cosine, first, second;
    output[0] = input[0] * object->width * object->scale_x /
                mesh->source_extent[0];
    output[1] = input[1] * object->height * object->scale_y /
                mesh->source_extent[1];
    output[2] = input[2] * object->depth * object->scale_z /
                mesh->source_extent[2];
    angle = object->rotation_x * 0.017453292519943295f;
    sine = sinf(angle); cosine = cosf(angle);
    first = output[1]*cosine-output[2]*sine;
    second = output[1]*sine+output[2]*cosine;
    output[1] = first; output[2] = second;
    angle = (object->rotation_y + object->rotation) * 0.017453292519943295f;
    sine = sinf(angle); cosine = cosf(angle);
    first = output[0]*cosine+output[2]*sine;
    second = -output[0]*sine+output[2]*cosine;
    output[0] = first; output[2] = second;
    angle = object->rotation_z * 0.017453292519943295f;
    sine = sinf(angle); cosine = cosf(angle);
    first = output[0]*cosine-output[1]*sine;
    second = output[0]*sine+output[1]*cosine;
    output[0] = first + object->x;
    output[1] = second + object->y;
    output[2] += object->z;
}

static float collision_dot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void closest_triangle_point(const float p[3],
                                    const float a[3], const float b[3],
                                    const float c[3], float result[3]) {
    float ab[3], ac[3], ap[3], bp[3], cp[3], bc[3];
    float d1, d2, d3, d4, d5, d6, va, vb, vc, amount;
    int axis;
    for (axis = 0; axis < 3; axis++) {
        ab[axis] = b[axis]-a[axis];
        ac[axis] = c[axis]-a[axis];
        ap[axis] = p[axis]-a[axis];
    }
    d1 = collision_dot(ab, ap); d2 = collision_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { memcpy(result,a,3*sizeof(float)); return; }
    for (axis = 0; axis < 3; axis++) bp[axis] = p[axis]-b[axis];
    d3 = collision_dot(ab,bp); d4 = collision_dot(ac,bp);
    if (d3 >= 0.0f && d4 <= d3) { memcpy(result,b,3*sizeof(float)); return; }
    vc = d1*d4-d3*d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        amount = d1/(d1-d3);
        for (axis=0;axis<3;axis++) result[axis]=a[axis]+amount*ab[axis];
        return;
    }
    for (axis = 0; axis < 3; axis++) cp[axis] = p[axis]-c[axis];
    d5 = collision_dot(ab,cp); d6 = collision_dot(ac,cp);
    if (d6 >= 0.0f && d5 <= d6) { memcpy(result,c,3*sizeof(float)); return; }
    vb = d5*d2-d1*d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        amount = d2/(d2-d6);
        for (axis=0;axis<3;axis++) result[axis]=a[axis]+amount*ac[axis];
        return;
    }
    va = d3*d6-d5*d4;
    if (va <= 0.0f && d4-d3 >= 0.0f && d5-d6 >= 0.0f) {
        amount = (d4-d3)/((d4-d3)+(d5-d6));
        for (axis=0;axis<3;axis++) {
            bc[axis] = c[axis]-b[axis];
            result[axis]=b[axis]+amount*bc[axis];
        }
        return;
    }
    {
        float denominator = va+vb+vc;
        if (fabsf(denominator) < 0.000001f) {
            memcpy(result,a,3*sizeof(float)); return;
        }
        amount = 1.0f/denominator;
        for (axis=0;axis<3;axis++)
            result[axis]=a[axis]+ab[axis]*vb*amount+ac[axis]*vc*amount;
    }
}

static int mesh_contact(ZSharpGameModel *model,
                        const ZSharpGameObject *dynamic,
                        const ZSharpGameObject *static_mesh,
                        float normal[3], float *penetration) {
    ZSharpMeshCollision *cache = collision_mesh(model, static_mesh->mesh_path);
    float radius = round_radius(dynamic);
    float segment = capsule_segment(dynamic, radius);
    size_t index;
    *penetration = 0.0f;
    if (cache == NULL || radius <= 0.0f) return 0;
    for (index = 0; index < cache->triangle_count; index++) {
        const ZSharpCollisionTriangle *triangle = &cache->triangles[index];
        float world[3][3];
        int corner, sample;
        for (corner = 0; corner < 3; corner++)
            transform_collision_point(static_mesh, cache,
                                       triangle->point[corner],
                                       world[corner]);
        for (sample = 0; sample < (segment > 0.0f ? 5 : 1); sample++) {
            float center[3] = {dynamic->x,
                dynamic->y + (segment > 0.0f ?
                    segment * ((float)sample - 2.0f) * 0.5f : 0.0f),
                dynamic->z};
            float closest[3], delta[3], distance_squared = 0.0f;
            float depth;
            int axis;
            closest_triangle_point(center, world[0], world[1], world[2],
                                   closest);
            for (axis=0;axis<3;axis++) {
                delta[axis]=center[axis]-closest[axis];
                distance_squared += delta[axis]*delta[axis];
            }
            if (distance_squared >= radius*radius) continue;
            depth = radius-sqrtf(distance_squared);
            if (depth <= *penetration) continue;
            *penetration = depth;
            if (distance_squared > 0.000001f) {
                float inverse = 1.0f/sqrtf(distance_squared);
                for (axis=0;axis<3;axis++) normal[axis]=delta[axis]*inverse;
            } else {
                float ab[3], ac[3], length;
                for (axis=0;axis<3;axis++) {
                    ab[axis]=world[1][axis]-world[0][axis];
                    ac[axis]=world[2][axis]-world[0][axis];
                }
                normal[0]=ab[1]*ac[2]-ab[2]*ac[1];
                normal[1]=ab[2]*ac[0]-ab[0]*ac[2];
                normal[2]=ab[0]*ac[1]-ab[1]*ac[0];
                length=sqrtf(collision_dot(normal,normal));
                if (length < 0.00001f) {
                    normal[0]=normal[2]=0.0f; normal[1]=1.0f;
                } else for(axis=0;axis<3;axis++) normal[axis]/=length;
                if (normal[1] < 0.0f) for(axis=0;axis<3;axis++)
                    normal[axis]=-normal[axis];
            }
        }
    }
    return *penetration > 0.0f;
}

void zsharp_game_model_update(ZSharpGameModel *model, double delta_seconds) {
    ZSharpGameScene *scene = find_scene(model, model->active_scene);
    float delta = (float)fmin(delta_seconds, 0.05);
    size_t index;
    size_t other_index;
    if (scene == NULL) return;
    model->delta = delta;
    model->elapsed += delta;
    zsharp_game_animation_update(model, delta);
    zsharp_game_navigation_update(model, delta);
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        if (!same_active_scene(model, object)) continue;
        object->grounded = 0;
        object->colliding = 0;
        object->motion_x = 0.0f;
        object->motion_y = 0.0f;
        object->motion_z = 0.0f;
        if (object->body == ZGAME_BODY_DYNAMIC) {
            object->velocity_x += scene->gravity_x * object->gravity_scale * delta;
            object->velocity_y += scene->gravity_y * object->gravity_scale * delta;
            object->velocity_z += scene->gravity_z * object->gravity_scale * delta;
        }
        if (object->body != ZGAME_BODY_STATIC) {
            object->motion_x = object->velocity_x * delta;
            object->motion_y = object->velocity_y * delta;
            object->motion_z = object->velocity_z * delta;
            object->x += object->motion_x;
            object->y += object->motion_y;
            object->z += object->motion_z;
        }
    }
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *a = &model->objects[index];
        if (!same_active_scene(model, a) ||
            a->collider == ZGAME_COLLIDER_NONE) continue;
        for (other_index = index + 1; other_index < model->object_count;
             other_index++) {
            ZSharpGameObject *b = &model->objects[other_index];
            float overlap_x = 0.0f, overlap_y = 0.0f, overlap_z = 0.0f;
            float normal[3] = {0.0f, 0.0f, 0.0f}, penetration = 0.0f;
            int rounded;
            ZSharpGameObject *mesh_body, *round_body;
            if (!same_active_scene(model, b) ||
                b->collider == ZGAME_COLLIDER_NONE ||
                (a->body == ZGAME_BODY_STATIC &&
                 b->body == ZGAME_BODY_STATIC)) continue;
            mesh_body = a->collider == ZGAME_COLLIDER_MESH ? a :
                        b->collider == ZGAME_COLLIDER_MESH ? b : NULL;
            round_body = mesh_body == a ? b : a;
            if (mesh_body != NULL && round_collider(round_body)) {
                if (!mesh_contact(model, round_body, mesh_body,
                                  normal, &penetration)) continue;
                a->colliding = b->colliding = 1;
                if (!a->trigger && !b->trigger &&
                    round_body->body == ZGAME_BODY_DYNAMIC)
                    resolve_round_collision(round_body, mesh_body,
                                            normal, penetration);
                continue;
            }
            rounded = model->is_3d &&
                (round_collider(a) || round_collider(b));
            if (model->is_3d && rounded &&
                (a->collider == ZGAME_COLLIDER_BOX ||
                 b->collider == ZGAME_COLLIDER_BOX)) {
                if (!round_box_contact(a, b, normal, &penetration)) continue;
            } else if (model->is_3d && !rounded &&
                a->collider == ZGAME_COLLIDER_BOX &&
                b->collider == ZGAME_COLLIDER_BOX) {
                if (!box_contact(a, b, normal, &penetration)) continue;
            } else {
                if (!overlaps(a, b, &overlap_x, &overlap_y, &overlap_z))
                    continue;
                if (rounded && !round_contact(a, b, normal, &penetration))
                    continue;
            }
            a->colliding = b->colliding = 1;
            if (a->trigger || b->trigger) continue;
            if (rounded || (model->is_3d &&
                            a->collider == ZGAME_COLLIDER_BOX &&
                            b->collider == ZGAME_COLLIDER_BOX)) {
                float share = a->body == ZGAME_BODY_DYNAMIC &&
                              b->body == ZGAME_BODY_DYNAMIC ? 0.5f : 1.0f;
                if (a->body == ZGAME_BODY_DYNAMIC)
                    resolve_round_collision(a, b, normal,
                                            penetration * share);
                if (b->body == ZGAME_BODY_DYNAMIC) {
                    float opposite[3] = {-normal[0], -normal[1], -normal[2]};
                    resolve_round_collision(b, a, opposite,
                                            penetration * share);
                }
            } else if (a->body == ZGAME_BODY_DYNAMIC)
                resolve_collision(a, b, model->is_3d,
                                  overlap_x, overlap_y, overlap_z);
            if (!rounded && b->body == ZGAME_BODY_DYNAMIC)
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
        "rotation","rotationX","rotationY","rotationZ",
        "scaleX","scaleY","scaleZ","velocityX","velocityY",
        "velocityZ","mass","gravityScale","restitution","friction",
        "opacity","roughness","metallic","emissive",
        "lightType","lightIntensity","lightRange","lightAngle",
        "castShadows",
        "audioVolume","tone","texture",
        "toneDuration","layer","color",
        "visible","trigger","grounded","colliding","text","scene",
        "audioLoop","audioAutoplay","audioOnCollision","audioPlay",
        "navStatus"
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
           strcmp(field, "cameraRotationX") == 0 ||
           strcmp(field, "cameraRotationY") == 0 ||
           strcmp(field, "cameraRotationZ") == 0 ||
           strcmp(field, "cameraFov") == 0;
}

int zsharp_game_model_owns_property(const ZSharpGameModel *model,
                                    const char *path) {
    char storage[512];
    char *parts[3];
    size_t count;
    ZSharpGameObject *object;
    if (zsharp_game_animation_has_command(model, path)) return 1;
    if (zsharp_game_navigation_has_command(model, path)) return 1;
    if (zsharp_game_navigation_has_sight_query(model, path)) return 1;
    if (!split_path(path, storage, sizeof(storage), parts, &count)) return 0;
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "key") == 0)
        return game_key_from_name(parts[2]) >= 0;
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "mouse") == 0)
        return strcmp(parts[2], "left") == 0 ||
               strcmp(parts[2], "right") == 0 ||
               strcmp(parts[2], "leftPressed") == 0 ||
               strcmp(parts[2], "leftReleased") == 0 ||
               strcmp(parts[2], "rightPressed") == 0 ||
               strcmp(parts[2], "rightReleased") == 0 ||
               strcmp(parts[2], "x") == 0 || strcmp(parts[2], "y") == 0 ||
               strcmp(parts[2], "deltaX") == 0 ||
               strcmp(parts[2], "deltaY") == 0 ||
               strcmp(parts[2], "captured") == 0;
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
    char *end;
    if (!isfinite(value)) {
        model_error(error, error_size, "game property has a non-finite number");
        return 0;
    }
    /* Z# number expressions deliberately reject exponent notation.  Physics
     * values near zero naturally make %g emit it, so return ordinary decimal
     * notation and discard precision beyond the float's useful digits. */
    snprintf(buffer, sizeof(buffer), "%.9f", (double)value);
    end = buffer + strlen(buffer);
    while (end > buffer && end[-1] == '0') *--end = '\0';
    if (end > buffer && end[-1] == '.') *--end = '\0';
    if (strcmp(buffer, "-0") == 0) strcpy(buffer, "0");
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
    if (zsharp_game_navigation_has_sight_query(model, path)) {
        *type = ZWINDOW_READ_STATUS;
        return status_property(zsharp_game_navigation_can_see(model, path),
                               text, error, error_size);
    }
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "key") == 0) {
        int key = game_key_from_name(field);
        *type = ZWINDOW_READ_STATUS;
        return status_property(model->input.keys[key], text, error,
                               error_size);
    }
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "mouse") == 0) {
        if (strcmp(field, "x") == 0 || strcmp(field, "y") == 0 ||
            strcmp(field, "deltaX") == 0 || strcmp(field, "deltaY") == 0) {
            *type = ZWINDOW_READ_NUMBER;
            return number_property(
                strcmp(field, "x") == 0 ? model->input.mouse_x :
                strcmp(field, "y") == 0 ? model->input.mouse_y :
                strcmp(field, "deltaX") == 0 ? model->input.mouse_delta_x :
                                                model->input.mouse_delta_y,
                                   text, error, error_size);
        }
        *type = ZWINDOW_READ_STATUS;
        return status_property(
            strcmp(field, "left") == 0 ? model->input.mouse_left :
            strcmp(field, "right") == 0 ? model->input.mouse_right :
                                           model->input.mouse_captured,
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
            strcmp(field, "cameraZ") == 0 ? scene->camera_z :
            strcmp(field, "cameraRotationX") == 0 ? scene->camera_rotation_x :
            strcmp(field, "cameraRotationY") == 0 ? scene->camera_rotation_y :
            strcmp(field, "cameraRotationZ") == 0 ? scene->camera_rotation_z
                                                    : scene->camera_fov,
            text, error, error_size);
    }
    object = find_object(model, count == 2 ? parts[0] : parts[1]);
    if (strcmp(field, "navStatus") == 0) {
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(object->nav_reachable ?
                                  (object->nav_moving ? "moving" : "reachable") :
                                  "notReachable", text, error, error_size);
    }
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
    GET_NUMBER("rotationX", rotation_x)
    GET_NUMBER("rotationY", rotation_y)
    GET_NUMBER("rotationZ", rotation_z)
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
    GET_NUMBER("lightIntensity", light_intensity)
    GET_NUMBER("lightRange", light_range)
    GET_NUMBER("lightAngle", light_angle)
    GET_NUMBER("audioVolume", audio_volume)
    GET_NUMBER("tone", tone_frequency)
    GET_NUMBER("toneDuration", tone_duration)
#undef GET_NUMBER
    if (strcmp(field, "layer") == 0) {
        *type = ZWINDOW_READ_NUMBER;
        return number_property((float)object->layer, text, error, error_size);
    }
    if (strcmp(field, "opacity") == 0 ||
        strcmp(field, "roughness") == 0 ||
        strcmp(field, "metallic") == 0 ||
        strcmp(field, "emissive") == 0) {
        float surface = strcmp(field, "opacity") == 0 ? object->opacity :
                        strcmp(field, "roughness") == 0 ? object->roughness :
                        strcmp(field, "metallic") == 0 ? object->metallic :
                                                       object->emissive;
        *type = ZWINDOW_READ_NUMBER;
        return number_property(surface * 100.0f, text, error, error_size);
    }
    if (strcmp(field, "lightType") == 0) {
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(object->light_type == 2 ? "spot" :
                                  object->light_type == 3 ? "directional" :
                                                             "point",
                                  text, error, error_size);
    }
    if (strcmp(field, "color") == 0) {
        char color[8];
        snprintf(color, sizeof(color), "#%06X", object->color);
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(color, text, error, error_size);
    }
    if (strcmp(field, "text") == 0 || strcmp(field, "scene") == 0 ||
        strcmp(field, "texture") == 0) {
        *type = ZWINDOW_READ_TEXT;
        return copy_property_text(
                                  strcmp(field, "text") == 0
                                      ? (object->text == NULL ? "" : object->text)
                                  : strcmp(field, "texture") == 0
                                      ? (object->asset_path == NULL ? "" : object->asset_path)
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
                                              :
        strcmp(field, "castShadows") == 0 ? object->cast_shadows
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
    if (zsharp_game_animation_has_command(model, path))
        return zsharp_game_animation_command(model, path, value, error,
                                             error_size);
    if (zsharp_game_navigation_has_command(model, path))
        return zsharp_game_navigation_command(model, path, value, error,
                                              error_size);
    if (zsharp_game_navigation_has_sight_query(model, path)) {
        model_error(error, error_size, "AI sight queries are read-only");
        return 0;
    }
    if (!zsharp_game_model_owns_property(model, path) ||
        !split_path(path, storage, sizeof(storage), parts, &count)) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "unknown game property '%s'", path);
        return 0;
    }
    field = parts[count - 1];
    if (strcmp(field, "navStatus") == 0) {
        model_error(error, error_size, "AI navStatus is read-only");
        return 0;
    }
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
            if (scene_object->audio_autoplay)
                scene_object->audio_started = 1;
        }
        return 1;
    }
    if (count == 3 && strcmp(parts[0], "input") == 0 &&
        strcmp(parts[1], "mouse") == 0 &&
        strcmp(field, "captured") == 0) {
        if (strcmp(value, "alive") == 0) model->input.mouse_captured = 1;
        else if (strcmp(value, "dead") == 0) model->input.mouse_captured = 0;
        else {
            model_error(error, error_size,
                        "input.mouse.captured requires alive or dead");
            return 0;
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
        else if (strcmp(field, "cameraRotationX") == 0)
            scene->camera_rotation_x = number;
        else if (strcmp(field, "cameraRotationY") == 0)
            scene->camera_rotation_y = number;
        else if (strcmp(field, "cameraRotationZ") == 0)
            scene->camera_rotation_z = number;
        else scene->camera_fov = number;
        return 1;
    }
    object = find_object(model, count == 2 ? parts[0] : parts[1]);
    if (strcmp(field, "text") == 0)
        return replace_text(&object->text, value);
    if (strcmp(field, "texture") == 0) {
        if (!safe_relative_asset(value)) {
            model_error(error, error_size,
                        "textures require a safe project-relative path");
            return 0;
        }
        return replace_text(&object->asset_path, value);
    }
    if (strcmp(field, "lightType") == 0) {
        if (object->shape != ZGAME_SHAPE_LIGHT ||
            (strcmp(value, "point") != 0 &&
             strcmp(value, "spot") != 0 &&
             strcmp(value, "directional") != 0)) {
            model_error(error, error_size,
                        "lightType requires point, spot, or directional on a light");
            return 0;
        }
        object->light_type = strcmp(value, "point") == 0 ? 1 :
                             strcmp(value, "spot") == 0 ? 2 : 3;
        return 1;
    }
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
        strcmp(field, "castShadows") == 0 ||
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
        else if (strcmp(field, "castShadows") == 0) {
            if (object->shape != ZGAME_SHAPE_LIGHT) {
                model_error(error, error_size, "castShadows requires a light");
                return 0;
            }
            object->cast_shadows = status;
        }
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
    if (strcmp(field, "lightIntensity") == 0 ||
        strcmp(field, "lightRange") == 0 ||
        strcmp(field, "lightAngle") == 0) {
        if (object->shape != ZGAME_SHAPE_LIGHT || number < 0.0f ||
            (strcmp(field, "lightAngle") == 0 && number > 180.0f)) {
            model_error(error, error_size,
                        "light property requires a valid number on a light");
            return 0;
        }
        if (strcmp(field, "lightIntensity") == 0)
            object->light_intensity = number;
        else if (strcmp(field, "lightRange") == 0)
            object->light_range = number;
        else object->light_angle = number;
        return 1;
    }
    if (strcmp(field, "opacity") == 0 ||
        strcmp(field, "roughness") == 0 ||
        strcmp(field, "metallic") == 0 ||
        strcmp(field, "emissive") == 0) {
        if (number < 0.0f || number > 100.0f) {
            model_error(error, error_size,
                        "surface property must be between 0 and 100");
            return 0;
        }
        if (strcmp(field, "opacity") == 0) object->opacity = number / 100.0f;
        else if (strcmp(field, "roughness") == 0)
            object->roughness = number / 100.0f;
        else if (strcmp(field, "metallic") == 0)
            object->metallic = number / 100.0f;
        else object->emissive = number / 100.0f;
        return 1;
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
    SET_NUMBER("rotationX", rotation_x)
    SET_NUMBER("rotationY", rotation_y)
    SET_NUMBER("rotationZ", rotation_z)
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

const ZSharpGameObject *zsharp_game_model_button_at(const ZSharpGameModel *model,
                                                   float mouse_x, float mouse_y) {
    const ZSharpGameObject *selected = NULL;
    ZSharpGameScene *scene = find_scene(model, model->active_scene);
    size_t i;
    if (!model->is_3d && scene != NULL) {
        mouse_x += scene->camera_x;
        mouse_y += scene->camera_y;
    }
    for (i = 0; i < model->object_count; ++i) {
        const ZSharpGameObject *object = &model->objects[i];
        if (object->shape != ZGAME_SHAPE_BUTTON || !object->visible ||
            object->opacity <= 0 || object->scene == NULL || model->active_scene == NULL ||
            strcmp(object->scene, model->active_scene) != 0) continue;
        if (fabsf(mouse_x - object->x) <= fabsf(object->width * object->scale_x) * 0.5f &&
            fabsf(mouse_y - object->y) <= fabsf(object->height * object->scale_y) * 0.5f &&
            (selected == NULL || object->layer >= selected->layer)) selected = object;
    }
    return selected;
}

void zsharp_game_model_frame(const ZSharpGameModel *model,
                             ZSharpGameRenderFrame *frame,
                             ZSharpGameRenderObject **objects) {
    ZSharpGameScene *scene = find_scene(model, model->active_scene);
    size_t index;
    size_t count = 0;
    const ZSharpGameObject *hovered_button = model->input.mouse_captured ? NULL :
        zsharp_game_model_button_at(model, model->input.mouse_x, model->input.mouse_y);
    memset(frame, 0, sizeof(*frame));
    *objects = model->object_count == 0 ? NULL :
        (ZSharpGameRenderObject *)calloc(model->object_count,
                                         sizeof(**objects));
    for (index = 0; index < model->object_count; index++) {
        const ZSharpGameObject *source = &model->objects[index];
        ZSharpGameRenderObject *target;
        if (!same_active_scene(model, source)) continue;
        target = &(*objects)[count++];
        target->render_id = count;
        target->shape = source->shape;
        target->x = source->x;
        target->y = source->y;
        target->z = source->z;
        target->width = source->width;
        target->height = source->height;
        target->depth = source->depth;
        target->rotation = source->rotation;
        target->rotation_x = source->rotation_x;
        target->rotation_y = source->rotation_y;
        target->rotation_z = source->rotation_z;
        target->scale_x = source->scale_x;
        target->scale_y = source->scale_y;
        target->scale_z = source->scale_z;
        target->color = source->color;
        if (source->shape == ZGAME_SHAPE_BUTTON && source->hover_color_explicit &&
            hovered_button == source)
            target->color = source->hover_color;
        target->opacity = source->opacity;
        target->roughness = source->roughness;
        target->emissive = source->emissive;
        target->metallic = source->metallic;
        target->light_type = source->light_type;
        target->light_intensity = source->light_intensity;
        target->light_range = source->light_range;
        target->light_angle = source->light_angle;
        target->cast_shadows = source->cast_shadows;
        target->visible = source->visible;
        target->layer = source->layer;
        target->text = source->text;
        target->asset_path = source->asset_path;
        target->mesh_path = source->mesh_path;
        target->material_names = (const char *const *)source->material_names;
        target->material_textures = (const char *const *)source->material_textures;
        target->material_count = source->material_count;
        target->part_poses = source->part_poses;
        target->part_pose_count = source->part_pose_count;
    }
    frame->is_3d = model->is_3d;
    frame->background = scene == NULL ? 0x08080bu : scene->background;
    frame->camera_x = scene == NULL ? 0.0f : scene->camera_x;
    frame->camera_y = scene == NULL ? 0.0f : scene->camera_y;
    frame->camera_z = scene == NULL ? 8.0f : scene->camera_z;
    frame->camera_rotation_x = scene == NULL ? 0.0f : scene->camera_rotation_x;
    frame->camera_rotation_y = scene == NULL ? 0.0f : scene->camera_rotation_y;
    frame->camera_rotation_z = scene == NULL ? 0.0f : scene->camera_rotation_z;
    frame->camera_fov = scene == NULL ? 70.0f : scene->camera_fov;
    frame->project_root = model->project_root;
    frame->objects = *objects;
    frame->object_count = count;
}
#endif
