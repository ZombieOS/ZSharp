#include "window_runtime.h"

#include "paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *source_stem(const char *source, size_t *length) {
    const char *name = source == NULL ? "" : source;
    const char *cursor;
    const char *dot;
    for (cursor = name; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    dot = strrchr(name, '.');
    *length = dot == NULL ? strlen(name) : (size_t)(dot - name);
    return name;
}

static ZSharpUIPropertyType expected_property_type(
    ZSharpUIElementType element, const char *name) {
    int measurement = strcmp(name, "width") == 0 ||
        strcmp(name, "height") == 0 || strcmp(name, "locationX") == 0 ||
        strcmp(name, "locationY") == 0;
    if (measurement) return ZUI_PROPERTY_MEASUREMENT;
    if (element == ZUI_DESIGN) {
        if (strcmp(name, "title") == 0 || strcmp(name, "icon") == 0)
            return ZUI_PROPERTY_TEXT;
        if (strcmp(name, "scalable") == 0) return ZUI_PROPERTY_STATUS;
        if (strcmp(name, "background") == 0) return ZUI_PROPERTY_COLOR;
    } else if (element == ZUI_TEXT) {
        if (strcmp(name, "content") == 0) return ZUI_PROPERTY_TEXT;
        if (strcmp(name, "color") == 0) return ZUI_PROPERTY_COLOR;
    } else if (element == ZUI_BUTTON) {
        if (strcmp(name, "text") == 0) return ZUI_PROPERTY_TEXT;
        if (strcmp(name, "textColor") == 0 ||
            strcmp(name, "buttonColor") == 0) return ZUI_PROPERTY_COLOR;
    } else if (element == ZUI_IMAGE) {
        if (strcmp(name, "file") == 0) return ZUI_PROPERTY_TEXT;
    } else if (element == ZUI_TEXT_INPUT) {
        if (strcmp(name, "display") == 0) return ZUI_PROPERTY_TEXT;
    }
    return 0;
}

static const char *type_name(ZSharpUIPropertyType type) {
    switch (type) {
        case ZUI_PROPERTY_TEXT: return "quoted text";
        case ZUI_PROPERTY_STATUS: return "alive or dead";
        case ZUI_PROPERTY_COLOR: return "a color or gradient";
        case ZUI_PROPERTY_MEASUREMENT: return "a measurement";
        case ZUI_PROPERTY_IDENTIFIER: return "an option name";
        default: return "a compatible value";
    }
}

int zsharp_window_model_set(ZSharpProgram *program, const char *path,
                            ZSharpWindowValueType value_type,
                            const char *text_value, ZSharpUIUnit unit,
                            ZSharpUIElement **element_output,
                            ZSharpUIProperty **property_output,
                            char *error, size_t error_size) {
    char *copy;
    char *parts[3] = {0};
    size_t count = 1;
    char *cursor;
    const char *element_name;
    const char *property_name;
    ZSharpUIElement *element = NULL;
    ZSharpUIProperty *property = NULL;
    ZSharpUIPropertyType expected;
    size_t index;
    char *replacement;
    if (program == NULL || path == NULL) {
        snprintf(error, error_size, "window property target is missing");
        return 0;
    }
    copy = zsharp_copy_text(path, strlen(path));
    if (copy == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    parts[0] = copy;
    for (cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor == '.') {
            *cursor = '\0';
            if (count == 3 || cursor[1] == '\0') {
                free(copy);
                snprintf(error, error_size,
                         "window property paths use Element.property or "
                         "File.Element.property");
                return 0;
            }
            parts[count++] = cursor + 1;
        }
    }
    if (count != 2 && count != 3) {
        free(copy);
        snprintf(error, error_size,
                 "window property paths use Element.property or "
                 "File.Element.property");
        return 0;
    }
    if (count == 3) {
        size_t stem_length;
        const char *stem = source_stem(program->source_name, &stem_length);
        if (strlen(parts[0]) != stem_length ||
            memcmp(parts[0], stem, stem_length) != 0) {
            snprintf(error, error_size,
                     "active window file is '%.*s', not '%s'",
                     (int)stem_length, stem, parts[0]);
            free(copy);
            return 0;
        }
    }
    element_name = parts[count - 2];
    property_name = parts[count - 1];
    for (index = 0; index < program->window.element_count; index++) {
        if (strcmp(program->window.elements[index].name, element_name) == 0) {
            element = &program->window.elements[index];
            break;
        }
    }
    if (element == NULL) {
        snprintf(error, error_size,
                 "active window has no element named '%s'", element_name);
        free(copy);
        return 0;
    }
    expected = expected_property_type(element->type, property_name);
    if (expected == 0) {
        snprintf(error, error_size, "UI element '%s' has no mutable '%s' field",
                 element_name, property_name);
        free(copy);
        return 0;
    }
    if ((int)expected != (int)value_type) {
        snprintf(error, error_size, "%s.%s requires %s", element_name,
                 property_name, type_name(expected));
        free(copy);
        return 0;
    }
    if (expected == ZUI_PROPERTY_COLOR) {
        ZSharpPaint paint;
        memset(&paint, 0, sizeof(paint));
        if (!zsharp_paint_parse(text_value, &paint, error, error_size)) {
            free(copy);
            return 0;
        }
        if (paint.kind != ZSHARP_PAINT_SOLID &&
            !(element->type == ZUI_DESIGN &&
              strcmp(property_name, "background") == 0)) {
            zsharp_paint_free(&paint);
            snprintf(error, error_size,
                     "gradients are supported by design backgrounds");
            free(copy);
            return 0;
        }
        zsharp_paint_free(&paint);
    }
    if (expected == ZUI_PROPERTY_MEASUREMENT &&
        (strcmp(property_name, "width") == 0 ||
         strcmp(property_name, "height") == 0) &&
        strtod(text_value, NULL) <= 0.0) {
        snprintf(error, error_size,
                 "UI width and height must be greater than zero");
        free(copy);
        return 0;
    }
    for (index = 0; index < element->property_count; index++) {
        if (strcmp(element->properties[index].name, property_name) == 0) {
            property = &element->properties[index];
            break;
        }
    }
    if (property == NULL) {
        property = zsharp_ui_element_add_property(element);
        if (property == NULL) {
            snprintf(error, error_size, "out of memory");
            free(copy);
            return 0;
        }
        property->name = zsharp_copy_text(property_name, strlen(property_name));
        if (property->name == NULL) {
            snprintf(error, error_size, "out of memory");
            free(copy);
            return 0;
        }
        property->type = expected;
    }
    replacement = zsharp_copy_text(text_value == NULL ? "" : text_value,
                                   strlen(text_value == NULL ? "" : text_value));
    if (replacement == NULL) {
        snprintf(error, error_size, "out of memory");
        free(copy);
        return 0;
    }
    free(property->text_value);
    property->text_value = replacement;
    property->unit = unit;
    if (expected == ZUI_PROPERTY_STATUS)
        property->status_value = strcmp(replacement, "alive") == 0;
    if (element_output != NULL) *element_output = element;
    if (property_output != NULL) *property_output = property;
    free(copy);
    return 1;
}
