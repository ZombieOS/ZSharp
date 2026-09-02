#define _CRT_SECURE_NO_WARNINGS

#include "window_style.h"

#include "project.h"
#include "zsharp.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZSHARP_ZSS_FILE_LIMIT (1024u * 1024u)

static void style_error(char *error, size_t error_size, const char *path,
                        unsigned line, const char *message) {
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s:%u: %s", path, line, message);
}

static char *read_style_file(const char *path, char *error,
                             size_t error_size) {
    FILE *file = fopen(path, "rb");
    long length;
    char *text;
    if (file == NULL) {
        snprintf(error, error_size, "could not read ZSS file '%s'", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 ||
        (unsigned long)length > ZSHARP_ZSS_FILE_LIMIT ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        snprintf(error, error_size,
                 "ZSS file '%s' is too large or could not be read", path);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) {
        fclose(file);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (fread(text, 1, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(text);
        snprintf(error, error_size, "could not read ZSS file '%s'", path);
        return NULL;
    }
    text[length] = '\0';
    return text;
}

static int skip_space(const char **cursor, unsigned *line, const char *path,
                      char *error, size_t error_size) {
    for (;;) {
        while (isspace((unsigned char)**cursor)) {
            if (**cursor == '\n') (*line)++;
            (*cursor)++;
        }
        if ((*cursor)[0] != '/' || (*cursor)[1] != '*') return 1;
        *cursor += 2;
        while (**cursor != '\0' &&
               !((*cursor)[0] == '*' && (*cursor)[1] == '/')) {
            if (**cursor == '\n') (*line)++;
            (*cursor)++;
        }
        if (**cursor == '\0') {
            style_error(error, error_size, path, *line,
                        "unterminated ZSS comment");
            return 0;
        }
        *cursor += 2;
    }
}

static char *style_name(const char **cursor) {
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

static int valid_color(const char *value) {
    size_t index;
    if (value == NULL || strlen(value) != 7 || value[0] != '#') return 0;
    for (index = 1; index < 7; index++)
        if (!isxdigit((unsigned char)value[index])) return 0;
    return 1;
}

static ZSharpUIElement *find_element(ZSharpProgram *program,
                                     const char *name) {
    size_t index;
    for (index = 0; index < program->window.element_count; index++) {
        ZSharpUIElement *element = &program->window.elements[index];
        if (element->name != NULL && strcmp(element->name, name) == 0)
            return element;
    }
    return NULL;
}

static ZSharpUIProperty *find_property(ZSharpUIElement *element,
                                       const char *name) {
    size_t index;
    for (index = 0; index < element->property_count; index++)
        if (strcmp(element->properties[index].name, name) == 0)
            return &element->properties[index];
    return NULL;
}

static void clear_property_value(ZSharpUIProperty *property) {
    size_t index;
    free(property->text_value);
    property->text_value = NULL;
    for (index = 0; index < property->item_count; index++)
        free(property->items[index]);
    free(property->items);
    property->items = NULL;
    property->item_count = 0;
    property->status_value = 0;
    property->unit = ZUI_UNIT_NONE;
}

static int set_text_property(ZSharpUIElement *element, const char *name,
                             ZSharpUIPropertyType type, const char *value,
                             ZSharpUIUnit unit, int apply, char *error,
                             size_t error_size) {
    ZSharpUIProperty *property;
    char *copied;
    if (!apply) return 1;
    copied = zsharp_copy_text(value, strlen(value));
    if (copied == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    property = find_property(element, name);
    if (property == NULL) {
        property = zsharp_ui_element_add_property(element);
        if (property == NULL) {
            free(copied);
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        property->name = zsharp_copy_text(name, strlen(name));
        if (property->name == NULL) {
            free(copied);
            snprintf(error, error_size, "out of memory");
            return 0;
        }
    } else {
        clear_property_value(property);
    }
    property->type = type;
    property->text_value = copied;
    property->unit = unit;
    return 1;
}

static void prefixed_name(char *output, size_t capacity,
                          const char *pseudo, const char *name) {
    if (pseudo == NULL || pseudo[0] == '\0') {
        snprintf(output, capacity, "%s", name);
        return;
    }
    snprintf(output, capacity, "%s%c%s", pseudo,
             (char)toupper((unsigned char)name[0]), name + 1);
}

static int parse_measurement(const char *value, char **number,
                             ZSharpUIUnit *unit) {
    char *end = NULL;
    double parsed;
    size_t length;
    *number = NULL;
    *unit = ZUI_UNIT_NONE;
    if (value == NULL || value[0] == '\0') return 0;
    parsed = strtod(value, &end);
    if (end == value || !isfinite(parsed) || parsed < 0.0) return 0;
    if (strcmp(end, "px") == 0) *unit = ZUI_UNIT_PX;
    else if (strcmp(end, "zu") == 0) *unit = ZUI_UNIT_ZU;
    else if (*end != '\0') return 0;
    length = (size_t)(end - value);
    *number = zsharp_copy_text(value, length);
    return *number != NULL;
}

static int set_measurement(ZSharpUIElement *element, const char *name,
                           const char *value, int apply, char *error,
                           size_t error_size) {
    char *number;
    ZSharpUIUnit unit;
    int ok;
    if (!parse_measurement(value, &number, &unit)) return 0;
    ok = set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT,
                           number, unit, apply, error, error_size);
    free(number);
    return ok;
}

static int set_color(ZSharpUIElement *element, const char *name,
                     const char *value, int apply, char *error,
                     size_t error_size) {
    if (!valid_color(value)) return 0;
    return set_text_property(element, name, ZUI_PROPERTY_COLOR, value,
                             ZUI_UNIT_NONE, apply, error, error_size);
}

static int apply_border(ZSharpUIElement *element, const char *pseudo,
                        const char *value, int apply, char *error,
                        size_t error_size) {
    const char *space;
    const char *color;
    char *width;
    char width_name[64];
    char color_name[64];
    int ok;
    prefixed_name(width_name, sizeof(width_name), pseudo, "borderWidth");
    prefixed_name(color_name, sizeof(color_name), pseudo, "borderColor");
    if (strcmp(value, "none") == 0)
        return set_measurement(element, width_name, "0px", apply,
                               error, error_size);
    space = strchr(value, ' ');
    if (space == NULL) return 0;
    width = zsharp_copy_text(value, (size_t)(space - value));
    if (width == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    while (*space == ' ') space++;
    if (strncmp(space, "solid", 5) != 0 ||
        (space[5] != ' ' && space[5] != '\t')) {
        free(width);
        return 0;
    }
    color = space + 5;
    while (*color == ' ' || *color == '\t') color++;
    ok = set_measurement(element, width_name, width, apply, error,
                         error_size) &&
         set_color(element, color_name, color, apply, error, error_size);
    free(width);
    return ok;
}

static int apply_declaration(ZSharpUIElement *element, const char *pseudo,
                             const char *field, const char *value, int apply,
                             char *error, size_t error_size) {
    char name[64];
    const char *base;
    if (strcmp(field, "background") == 0) {
        if (!valid_color(value) &&
            strncmp(value, "linear-gradient(", 16) != 0 &&
            strncmp(value, "radial-gradient(", 16) != 0) return 0;
        base = element->type == ZUI_DESIGN ? "background" :
               element->type == ZUI_BUTTON ? "buttonColor" :
                                              "backgroundColor";
        prefixed_name(name, sizeof(name), pseudo, base);
        return set_text_property(element, name, ZUI_PROPERTY_COLOR, value,
                                 ZUI_UNIT_NONE, apply, error, error_size);
    }
    if (strcmp(field, "color") == 0) {
        base = element->type == ZUI_TEXT ? "color" : "textColor";
        prefixed_name(name, sizeof(name), pseudo, base);
        return set_color(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "border") == 0)
        return apply_border(element, pseudo, value, apply, error, error_size);
    if (strcmp(field, "border-color") == 0) {
        prefixed_name(name, sizeof(name), pseudo, "borderColor");
        return set_color(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "border-radius") == 0) {
        prefixed_name(name, sizeof(name), pseudo, "borderRadius");
        return set_measurement(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "font-family") == 0) {
        prefixed_name(name, sizeof(name), pseudo, "fontFamily");
        return value[0] != '\0' && set_text_property(
            element, name, ZUI_PROPERTY_TEXT, value, ZUI_UNIT_NONE, apply,
            error, error_size);
    }
    if (strcmp(field, "font-size") == 0) {
        prefixed_name(name, sizeof(name), pseudo, "fontSize");
        return set_measurement(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "font-weight") == 0) {
        if (strcmp(value, "normal") != 0 && strcmp(value, "bold") != 0)
            return 0;
        prefixed_name(name, sizeof(name), pseudo, "fontWeight");
        return set_text_property(element, name, ZUI_PROPERTY_IDENTIFIER,
                                 value, ZUI_UNIT_NONE, apply, error,
                                 error_size);
    }
    if (strcmp(field, "padding") == 0 ||
        strcmp(field, "padding-left") == 0 ||
        strcmp(field, "padding-right") == 0 ||
        strcmp(field, "padding-top") == 0 ||
        strcmp(field, "padding-bottom") == 0) {
        const char *suffix = field + 7;
        strcpy(name, "padding");
        if (*suffix == '-') {
            name[7] = (char)toupper((unsigned char)suffix[1]);
            strcpy(name + 8, suffix + 2);
        }
        return set_measurement(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "caret-color") == 0 ||
        strcmp(field, "selection-background") == 0 ||
        strcmp(field, "selection-color") == 0) {
        base = strcmp(field, "caret-color") == 0 ? "caretColor" :
               strcmp(field, "selection-background") == 0
                   ? "selectionBackground" : "selectionColor";
        prefixed_name(name, sizeof(name), pseudo, base);
        return set_color(element, name, value, apply, error, error_size);
    }
    if (strcmp(field, "outline") == 0 && strcmp(value, "none") == 0) {
        prefixed_name(name, sizeof(name), pseudo, "outline");
        return set_text_property(element, name, ZUI_PROPERTY_IDENTIFIER,
                                 value, ZUI_UNIT_NONE, apply, error,
                                 error_size);
    }
    return 0;
}

static int parse_style(const char *path, const char *source,
                       ZSharpProgram *program, int apply, char *error,
                       size_t error_size) {
    const char *cursor = source;
    unsigned line = 1;
    while (1) {
        char *first = NULL;
        char *second = NULL;
        char *pseudo = NULL;
        const char *element_name;
        ZSharpUIElement *element = NULL;
        int window_matches = 1;
        if (!skip_space(&cursor, &line, path, error, error_size)) return 0;
        if (*cursor == '\0') return 1;
        if (*cursor++ != '.') goto selector_error;
        first = style_name(&cursor);
        if (first == NULL) goto selector_error;
        {
            const char *before_space = cursor;
            if (!skip_space(&cursor, &line, path, error, error_size))
                goto failed;
            if (cursor != before_space && *cursor != '{') {
                second = style_name(&cursor);
                if (second == NULL) goto selector_error;
            }
        }
        element_name = second == NULL ? first : second;
        if (second != NULL)
            window_matches = program->window.name != NULL &&
                             strcmp(program->window.name, first) == 0;
        if (*cursor == ':') {
            cursor++;
            pseudo = style_name(&cursor);
            if (pseudo == NULL ||
                (strcmp(pseudo, "hover") != 0 &&
                 strcmp(pseudo, "focus") != 0)) goto selector_error;
        }
        if (!skip_space(&cursor, &line, path, error, error_size)) goto failed;
        if (*cursor++ != '{') goto selector_error;
        if (window_matches) {
            element = find_element(program, element_name);
            if (element == NULL) {
                style_error(error, error_size, path, line,
                            "ZSS selector does not match a window element");
                goto failed;
            }
            if (pseudo != NULL && strcmp(pseudo, "hover") == 0 &&
                element->type != ZUI_BUTTON) {
                style_error(error, error_size, path, line,
                            ":hover is currently supported on buttons");
                goto failed;
            }
            if (pseudo != NULL && strcmp(pseudo, "focus") == 0 &&
                element->type != ZUI_TEXT_INPUT) {
                style_error(error, error_size, path, line,
                            ":focus is currently supported on text inputs");
                goto failed;
            }
        }
        while (1) {
            char *field = NULL;
            char *value = NULL;
            const char *value_start;
            unsigned declaration_line;
            if (!skip_space(&cursor, &line, path, error, error_size))
                goto failed;
            if (*cursor == '}') {
                cursor++;
                break;
            }
            declaration_line = line;
            field = style_name(&cursor);
            if (field == NULL) goto declaration_error;
            if (!skip_space(&cursor, &line, path, error, error_size)) {
                free(field);
                goto failed;
            }
            if (*cursor++ != ':') {
                free(field);
                goto declaration_error;
            }
            value_start = cursor;
            while (*cursor != '\0' && *cursor != ';' && *cursor != '}') {
                if (*cursor == '\n') line++;
                cursor++;
            }
            value = trimmed_text(value_start, cursor);
            if (value == NULL) {
                free(field);
                snprintf(error, error_size, "out of memory");
                goto failed;
            }
            if (value[0] == '\0' ||
                (element != NULL && !apply_declaration(
                    element, pseudo, field, value, apply,
                    error, error_size))) {
                char message[256];
                snprintf(message, sizeof(message),
                         "unsupported or invalid ZSS declaration '%s: %s'",
                         field, value);
                style_error(error, error_size, path, declaration_line,
                            message);
                free(value);
                free(field);
                goto failed;
            }
            free(value);
            free(field);
            if (*cursor == ';') cursor++;
            else if (*cursor != '}') goto declaration_error;
        }
        free(first);
        free(second);
        free(pseudo);
        continue;
selector_error:
        style_error(error, error_size, path, line,
                    "expected '.Element' or '.Window Element' ZSS selector");
        goto failed;
declaration_error:
        style_error(error, error_size, path, line,
                    "invalid ZSS declaration");
failed:
        free(first);
        free(second);
        free(pseudo);
        return 0;
    }
}

static int process_styles(ZSharpProgram *program, const char *project_root,
                          int apply, char *error, size_t error_size) {
    ZSharpSourceList files;
    size_t index;
    if (program == NULL || program->script_type != ZSCRIPT_WINDOW ||
        !program->has_window) return 1;
    if (!zsharp_project_list_files(project_root, ZSHARP_STYLE_EXTENSION,
                                   &files, error, error_size)) return 0;
    for (index = 0; index < files.count; index++) {
        char *source = read_style_file(files.items[index], error, error_size);
        int ok = source != NULL && parse_style(
            files.items[index], source, program, apply, error, error_size);
        free(source);
        if (!ok) {
            zsharp_project_source_list_free(&files);
            return 0;
        }
    }
    zsharp_project_source_list_free(&files);
    return 1;
}

int zsharp_window_styles_validate(const ZSharpProgram *program,
                                  const char *project_root,
                                  char *error, size_t error_size) {
    return process_styles((ZSharpProgram *)program, project_root, 0,
                          error, error_size);
}

int zsharp_window_styles_apply(ZSharpProgram *program,
                               const char *project_root,
                               char *error, size_t error_size) {
    return process_styles(program, project_root, 1, error, error_size);
}
