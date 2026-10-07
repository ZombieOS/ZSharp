#define _CRT_SECURE_NO_WARNINGS

#include "window_style.h"

#include "project.h"
#include "zsharp.h"
#include "paint.h"
#include "style_transition.h"

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
    uint32_t rgb;
    unsigned char alpha;
    return zsharp_color_parse(value, &rgb, &alpha);
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

static int css_length(const char *value, int negative) {
    char *end;
    double number = strtod(value, &end);
    if (end == value || !isfinite(number) || (!negative && number < 0)) return 0;
    return *end == '\0' || strcmp(end, "px") == 0 || strcmp(end, "zu") == 0 ||
           strcmp(end, "%") == 0 || strcmp(end, "vw") == 0 || strcmp(end, "vh") == 0;
}

static int box_shorthand(ZSharpUIElement *element, const char *base,
                          const char *value, int apply, char *error, size_t error_size) {
    char values[4][64];
    int count = 0, i;
    const char *cursor = value;
    const char *sides[4] = {"Top", "Right", "Bottom", "Left"};
    while (*cursor) {
        const char *start;
        size_t length;
        while (isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
        length = (size_t)(cursor - start);
        if (count == 4 || length >= 64) return 0;
        memcpy(values[count], start, length); values[count][length] = '\0';
        if (!css_length(values[count], strcmp(base, "margin") == 0)) return 0;
        count++;
    }
    if (count == 0) return 0;
    for (i = 0; i < 4; i++) {
        int index = i == 0 ? 0 : i == 1 ? (count > 1 ? 1 : 0) :
            i == 2 ? (count > 2 ? 2 : 0) : (count > 3 ? 3 : count > 1 ? 1 : 0);
        char name[48];
        snprintf(name, sizeof(name), "%s%s", base, sides[i]);
        if (!set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT,
                values[index], ZUI_UNIT_NONE, apply, error, error_size)) return 0;
    }
    return 1;
}

static int apply_declaration(ZSharpUIElement *element, const char *pseudo,
                             const char *field, const char *value, int apply,
                             char *error, size_t error_size) {
    char name[64];
    const char *base;
    if (strcmp(field,"transition")==0) {
        double seconds; int easing;
        if (pseudo != NULL && pseudo[0] || element->type != ZUI_BUTTON ||
            !zsharp_style_transition_parse(value,&seconds,&easing)) return 0;
        return set_text_property(element,"transition",ZUI_PROPERTY_TEXT,value,
                                 ZUI_UNIT_NONE,apply,error,error_size);
    }
    if (strcmp(field, "margin") == 0 || strcmp(field, "padding") == 0) {
        if (pseudo != NULL && pseudo[0]) return 0;
        return box_shorthand(element, field, value, apply, error, error_size);
    }
    if (strncmp(field, "margin-", 7) == 0 || strcmp(field, "gap") == 0 ||
        strcmp(field, "row-gap") == 0 || strcmp(field, "column-gap") == 0 ||
        strcmp(field, "min-width") == 0 || strcmp(field, "max-width") == 0 ||
        strcmp(field, "min-height") == 0 || strcmp(field, "max-height") == 0 ||
        strcmp(field, "flex-basis") == 0 || strcmp(field, "left") == 0 ||
        strcmp(field, "right") == 0 || strcmp(field, "top") == 0 || strcmp(field, "bottom") == 0) {
        const char *p = field;
        size_t n = 0;
        int upper = 0;
        if (pseudo != NULL && pseudo[0]) return 0;
        if (strncmp(field, "margin-", 7) == 0 && strcmp(field, "margin-left") != 0 &&
            strcmp(field, "margin-right") != 0 && strcmp(field, "margin-top") != 0 &&
            strcmp(field, "margin-bottom") != 0) return 0;
        if (!css_length(value, strncmp(field, "margin", 6) == 0 ||
            strcmp(field, "left") == 0 || strcmp(field, "right") == 0 ||
            strcmp(field, "top") == 0 || strcmp(field, "bottom") == 0)) return 0;
        while (*p && n + 1 < sizeof(name)) {
            if (*p == '-') upper = 1;
            else { name[n++] = upper ? (char)toupper((unsigned char)*p) : *p; upper = 0; }
            p++;
        }
        name[n] = '\0';
        return set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT, value,
                                  ZUI_UNIT_NONE, apply, error, error_size);
    }
    if (strcmp(field, "display") == 0 || strcmp(field, "position") == 0 ||
        strcmp(field, "flex-direction") == 0 || strcmp(field, "flex-wrap") == 0 ||
        strcmp(field, "justify-content") == 0 || strcmp(field, "align-items") == 0 ||
        strcmp(field, "align-self") == 0 || strcmp(field, "grid-template-columns") == 0) {
        const char *choices = NULL;
        if (pseudo != NULL && pseudo[0]) return 0;
        if (strcmp(field, "display") == 0) { strcpy(name, "display"); choices = "|block|none|flex|grid|"; }
        else if (strcmp(field, "position") == 0) { strcpy(name, "position"); choices = "|absolute|relative|static|"; }
        else if (strcmp(field, "flex-direction") == 0) { strcpy(name, "flexDirection"); choices = "|row|column|row-reverse|column-reverse|"; }
        else if (strcmp(field, "flex-wrap") == 0) { strcpy(name, "flexWrap"); choices = "|nowrap|wrap|"; }
        else if (strcmp(field, "justify-content") == 0) { strcpy(name, "justifyContent"); choices = "|flex-start|start|flex-end|end|center|space-between|space-around|space-evenly|"; }
        else if (strcmp(field, "align-items") == 0) { strcpy(name, "alignItems"); choices = "|start|flex-start|end|flex-end|center|stretch|"; }
        else if (strcmp(field, "align-self") == 0) { strcpy(name, "alignSelf"); choices = "|auto|start|flex-start|end|flex-end|center|stretch|"; }
        else {
            unsigned columns; int consumed = 0;
            strcpy(name, "gridTemplateColumns");
            if (sscanf(value, "repeat(%u, 1fr)%n", &columns, &consumed) != 1 ||
                consumed != (int)strlen(value) || columns == 0 || columns > 256) return 0;
        }
        if (choices != NULL) {
            char option[128];
            if (strlen(value) > 120) return 0;
            snprintf(option, sizeof(option), "|%s|", value);
            if (strstr(choices, option) == NULL) return 0;
        }
        if ((strcmp(value, "flex") == 0 || strcmp(value, "grid") == 0 ||
            strcmp(field, "grid-template-columns") == 0 || strcmp(field, "flex-direction") == 0 ||
            strcmp(field, "flex-wrap") == 0 || strcmp(field, "justify-content") == 0 ||
            strcmp(field, "align-items") == 0) && element->type != ZUI_DESIGN) return 0;
        return set_text_property(element, name, ZUI_PROPERTY_IDENTIFIER, value,
                                  ZUI_UNIT_NONE, apply, error, error_size);
    }
    if (strcmp(field, "flex-grow") == 0 || strcmp(field, "flex-shrink") == 0 || strcmp(field, "order") == 0) {
        char *end; double number = strtod(value, &end);
        if (pseudo != NULL && pseudo[0] || end == value || *end || !isfinite(number) ||
            (strcmp(field, "order") != 0 && number < 0) ||
            (strcmp(field, "order") == 0 && floor(number) != number)) return 0;
        strcpy(name, strcmp(field, "order") == 0 ? "order" :
                     strcmp(field, "flex-grow") == 0 ? "flexGrow" : "flexShrink");
        return set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT, value,
                                 ZUI_UNIT_NONE, apply, error, error_size);
    }
    if (strcmp(field, "opacity") == 0) {
        char *end;
        double opacity = strtod(value, &end);
        if (end == value || *end != '\0' || !isfinite(opacity) ||
            opacity < 0 || opacity > 1) return 0;
        prefixed_name(name, sizeof(name), pseudo, "opacity");
        return set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT,
                                 value, ZUI_UNIT_NONE, apply, error, error_size);
    }
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
    if (strcmp(field, "background-color") == 0) {
        if (!valid_color(value)) return 0;
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
    if (strcmp(field, "width") == 0 || strcmp(field, "height") == 0) {
        if (pseudo != NULL && pseudo[0] != '\0') return 0;
        if (!css_length(value, 0)) return 0;
        snprintf(name, sizeof(name), "css%c%s", (char)toupper(field[0]), field + 1);
        if (!set_text_property(element, name, ZUI_PROPERTY_MEASUREMENT, value,
                ZUI_UNIT_NONE, apply, error, error_size)) return 0;
        if (strchr(value, '%') != NULL || strstr(value, "vw") != NULL || strstr(value, "vh") != NULL) return 1;
        return set_measurement(element, field, value, apply, error,
                               error_size);
    }
    if (strcmp(field, "text-align") == 0) {
        if (strcmp(value, "left") != 0 && strcmp(value, "center") != 0 &&
            strcmp(value, "right") != 0) return 0;
        prefixed_name(name, sizeof(name), pseudo, "textAlign");
        return set_text_property(element, name, ZUI_PROPERTY_IDENTIFIER,
                                 value, ZUI_UNIT_NONE, apply, error,
                                 error_size);
    }
    if (strcmp(field, "text-transform") == 0) {
        if (strcmp(value, "none") != 0 &&
            strcmp(value, "uppercase") != 0 &&
            strcmp(value, "lowercase") != 0) return 0;
        prefixed_name(name, sizeof(name), pseudo, "textTransform");
        return set_text_property(element, name, ZUI_PROPERTY_IDENTIFIER,
                                 value, ZUI_UNIT_NONE, apply, error,
                                 error_size);
    }
    if (strcmp(field, "max-length") == 0 ||
        strcmp(field, "maxlength") == 0) {
        if (pseudo != NULL && pseudo[0] != '\0' ||
            element->type != ZUI_TEXT_INPUT) return 0;
        return set_measurement(element, "maxLength", value, apply, error,
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

static const char *element_type_name(ZSharpUIElementType type) {
    switch (type) {
        case ZUI_DESIGN: return "design";
        case ZUI_BUTTON: return "button";
        case ZUI_TEXT: return "text";
        case ZUI_TEXT_INPUT: return "textInput";
        default: return "";
    }
}

/* Native windows have one flat root, not an HTML DOM. Both legacy name
 * selectors and CSS-like id/type/universal selectors address that tree. */
static int selector_atom(const char **cursor, const char *name, const char *type) {
    char *word;
    int by_name = 0, matches;
    if (**cursor == '*') { (*cursor)++; return 1; }
    if (**cursor == '.' || **cursor == '#') { by_name = 1; (*cursor)++; }
    word = style_name(cursor);
    if (word == NULL) return -1;
    matches = (by_name ? name != NULL && strcmp(word, name) == 0 :
        (type != NULL && strcmp(word, type) == 0) ||
        (name != NULL && strcmp(word, name) == 0));
    free(word);
    return matches;
}

static int selector_match(const char *start, const char *end,
                          const ZSharpWindow *window, const ZSharpUIElement *element,
                          char pseudo[16]) {
    char *text = trimmed_text(start, end);
    const char *cursor;
    int result;
    pseudo[0] = '\0';
    if (text == NULL) return -1;
    cursor = text;
    result = selector_atom(&cursor, element->name, element_type_name(element->type));
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != '\0' && *cursor != ':') {
        const char *root = text;
        result = selector_atom(&root, window->name, "window");
        if (*cursor == '>') cursor++;
        while (isspace((unsigned char)*cursor)) cursor++;
        {
            int child = selector_atom(&cursor, element->name, element_type_name(element->type));
            if (child < 0 || result < 0) result = -1;
            else result = result && child;
        }
    }
    if (*cursor == ':') {
        char *state;
        cursor++;
        state = style_name(&cursor);
        if (state == NULL || (strcmp(state, "hover") != 0 && strcmp(state, "focus") != 0)) result = -1;
        else snprintf(pseudo, 16, "%s", state);
        free(state);
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != '\0') result = -1;
    free(text);
    return result;
}

static int apply_selector_rule(ZSharpProgram *program, const char *selectors,
                               const char *field, const char *value, int apply,
                               const char *media, char *error, size_t error_size) {
    const char *start = selectors;
    int any = 0;
    while (*start != '\0') {
        const char *end = strchr(start, ',');
        size_t i;
        if (end == NULL) end = start + strlen(start);
        for (i = 0; i < program->window.element_count; i++) {
            ZSharpUIElement *element = &program->window.elements[i];
            char pseudo[16];
            int match = selector_match(start, end, &program->window, element, pseudo);
            if (match < 0) return 0;
            if (!match) continue;
            any = 1;
            if (strcmp(pseudo, "hover") == 0 && element->type != ZUI_BUTTON) return 0;
            if (strcmp(pseudo, "focus") == 0 && element->type != ZUI_TEXT_INPUT && element->type != ZUI_BUTTON) return 0;
            if (media == NULL) {
                if (!apply_declaration(element, pseudo, field, value, apply, error, error_size)) return 0;
            } else {
                ZSharpUIElement temporary;
                size_t j;
                int valid;
                memset(&temporary,0,sizeof(temporary)); temporary.type = element->type;
                if (pseudo[0] || (strcmp(field,"width") != 0 && strcmp(field,"height") != 0 &&
                    strcmp(field,"display") != 0 && strcmp(field,"position") != 0 &&
                    strcmp(field,"left") != 0 && strcmp(field,"right") != 0 &&
                    strcmp(field,"top") != 0 && strcmp(field,"bottom") != 0 &&
                    strcmp(field,"gap") != 0 && strcmp(field,"row-gap") != 0 && strcmp(field,"column-gap") != 0 &&
                    strcmp(field,"order") != 0 && strcmp(field,"justify-content") != 0 &&
                    strcmp(field,"align-items") != 0 && strcmp(field,"align-self") != 0 &&
                    strcmp(field,"grid-template-columns") != 0 && strncmp(field,"margin",6) != 0 &&
                    strncmp(field,"padding",7) != 0 && strncmp(field,"min-",4) != 0 &&
                    strncmp(field,"max-",4) != 0 && strncmp(field,"flex-",5) != 0)) return 0;
                valid = apply_declaration(&temporary,NULL,field,value,1,error,error_size);
                for (j = 0; j < temporary.property_count; j++) {
                    ZSharpUIProperty *property = &temporary.properties[j];
                    char encoded[256];
                    snprintf(encoded,sizeof(encoded),"%s%s",media,property->name);
                    if (valid && !set_text_property(element,encoded,property->type,property->text_value,
                        property->unit,apply,error,error_size)) valid = 0;
                    clear_property_value(property); free(property->name);
                }
                free(temporary.properties);
                if (!valid) return 0;
            }
        }
        if (*end == '\0') break;
        start = end + 1;
        if (*start == '\0') return 0;
    }
    /* Preserve strict errors for unmatched short selectors, but a qualified
     * rule for another window in a shared stylesheet is intentionally skipped. */
    return any || strchr(selectors, ' ') != NULL;
}

static int parse_media(const char *start, const char *end, char encoded[192]) {
    double limits[4] = {0,1e12,0,1e12};
    const char *cursor = start;
    while (cursor < end) {
        const char *name_start, *name_end;
        char name[32]; double number; char *number_end; size_t length; int index;
        while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
        if (cursor == end) break;
        if (*cursor++ != '(') return 0;
        while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
        name_start=cursor; while (cursor < end && *cursor != ':') cursor++;
        if (cursor == end) return 0;
        name_end=cursor; while (name_end>name_start && isspace((unsigned char)name_end[-1])) name_end--;
        length=(size_t)(name_end-name_start); if (length >= sizeof(name)) return 0;
        memcpy(name,name_start,length);name[length]='\0';cursor++;
        number=strtod(cursor,&number_end);
        if (number_end==cursor || !isfinite(number) || number<0) return 0;
        cursor=number_end;
        if (cursor+2<=end && strncmp(cursor,"px",2)==0) cursor+=2;
        while (cursor<end && isspace((unsigned char)*cursor)) cursor++;
        if (cursor==end || *cursor++!=')') return 0;
        index=strcmp(name,"min-width")==0 ? 0 : strcmp(name,"max-width")==0 ? 1 :
            strcmp(name,"min-height")==0 ? 2 : strcmp(name,"max-height")==0 ? 3 : -1;
        if (index<0) return 0;
        if (index==0 || index==2) { if(number>limits[index])limits[index]=number; }
        else if(number<limits[index]) limits[index]=number;
        while (cursor<end && isspace((unsigned char)*cursor)) cursor++;
        if(cursor<end) {
            if (end-cursor<3 || strncmp(cursor,"and",3)!=0) return 0;
            cursor+=3;
            if(cursor==end) return 0;
        }
    }
    if (start==end) return 0;
    snprintf(encoded,192,"media/%.12g/%.12g/%.12g/%.12g/",limits[0],limits[1],limits[2],limits[3]);
    return 1;
}

static int parse_style(const char *path, const char *source,
                       ZSharpProgram *program, int apply, char *error,
                       size_t error_size, const char *media) {
    const char *cursor = source;
    unsigned line = 1;
    while (1) {
        char *first = NULL;
        char *second = NULL;
        char *pseudo = NULL;
        if (!skip_space(&cursor, &line, path, error, error_size)) return 0;
        if (*cursor == '\0') return 1;
        if (strncmp(cursor,"@media",6)==0) {
            const char *condition, *body;
            char encoded[192], *body_text;
            int depth=1, parsed;
            if (media != NULL) goto selector_error;
            cursor+=6; condition=cursor;
            while (*cursor && *cursor!='{') cursor++;
            if (*cursor!='{' || !parse_media(condition,cursor,encoded)) goto selector_error;
            body=++cursor;
            while (*cursor && depth) {
                if (*cursor=='{')depth++;
                else if (*cursor=='}')depth--;
                if (*cursor=='\n')line++;
                if(depth)cursor++;
            }
            if(depth)goto declaration_error;
            body_text=zsharp_copy_text(body,(size_t)(cursor-body));
            if(body_text==NULL)goto failed;
            parsed=parse_style(path,body_text,program,apply,error,error_size,encoded);
            free(body_text);if(!parsed)goto failed;cursor++;continue;
        }
        {
            const char *start = cursor;
            while (*cursor != '\0' && *cursor != '{') {
                if (*cursor == '\n') line++;
                cursor++;
            }
            first = trimmed_text(start, cursor);
            if (first == NULL || first[0] == '\0') goto selector_error;
        }
        if (*cursor++ != '{') goto selector_error;
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
                !apply_selector_rule(program, first, field, value, apply,
                                     media, error, error_size)) {
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
            files.items[index], source, program, apply, error, error_size,NULL);
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
