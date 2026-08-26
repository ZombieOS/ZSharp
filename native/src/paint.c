#include "paint.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_color(const char *text, uint32_t *color) {
    int digits[6];
    size_t index;
    if (text == NULL || strlen(text) != 7 || text[0] != '#') return 0;
    for (index = 0; index < 6; index++) {
        digits[index] = hex_digit(text[index + 1]);
        if (digits[index] < 0) return 0;
    }
    *color = ((uint32_t)(digits[0] * 16 + digits[1]) << 16) |
             ((uint32_t)(digits[2] * 16 + digits[3]) << 8) |
             (uint32_t)(digits[4] * 16 + digits[5]);
    return 1;
}

int zsharp_paint_is_gradient_text(const char *text) {
    return text != NULL &&
        (strncmp(text, "linear-gradient(", 16) == 0 ||
         strncmp(text, "radial-gradient(", 16) == 0);
}

void zsharp_paint_free(ZSharpPaint *paint) {
    if (paint == NULL) return;
    free(paint->colors);
    memset(paint, 0, sizeof(*paint));
}

int zsharp_paint_parse(const char *text, ZSharpPaint *paint,
                       char *error, size_t error_size) {
    const char *cursor;
    char *end;
    size_t prefix_length;
    ZSharpPaint parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (text == NULL) {
        snprintf(error, error_size, "paint value is empty");
        return 0;
    }
    if (!zsharp_paint_is_gradient_text(text)) {
        uint32_t *colors = (uint32_t *)malloc(sizeof(*colors));
        if (colors == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        if (!parse_color(text, colors)) {
            free(colors);
            snprintf(error, error_size,
                     "colors must use #RRGGBB or a Z# gradient");
            return 0;
        }
        parsed.kind = ZSHARP_PAINT_SOLID;
        parsed.colors = colors;
        parsed.color_count = 1;
        *paint = parsed;
        return 1;
    }
    parsed.kind = strncmp(text, "linear", 6) == 0
        ? ZSHARP_PAINT_LINEAR : ZSHARP_PAINT_RADIAL;
    prefix_length = 16;
    cursor = text + prefix_length;
    parsed.degrees = strtod(cursor, &end);
    if (end == cursor || *end != ':') {
        snprintf(error, error_size,
                 "a gradient requires a numeric degree followed by colors");
        return 0;
    }
    cursor = end + 1;
    for (;;) {
        uint32_t color;
        uint32_t *resized;
        char color_text[8];
        if (strlen(cursor) < 7) {
            snprintf(error, error_size,
                     "gradient colors must use #RRGGBB");
            zsharp_paint_free(&parsed);
            return 0;
        }
        memcpy(color_text, cursor, 7);
        color_text[7] = '\0';
        if (!parse_color(color_text, &color)) {
            snprintf(error, error_size,
                     "gradient colors must use #RRGGBB");
            zsharp_paint_free(&parsed);
            return 0;
        }
        resized = (uint32_t *)realloc(
            parsed.colors,
            (parsed.color_count + 1) * sizeof(*parsed.colors));
        if (resized == NULL) {
            snprintf(error, error_size, "out of memory");
            zsharp_paint_free(&parsed);
            return 0;
        }
        parsed.colors = resized;
        parsed.colors[parsed.color_count++] = color;
        cursor += 7;
        if (*cursor == ':') {
            cursor++;
            continue;
        }
        if (*cursor != ')' || cursor[1] != '\0') {
            snprintf(error, error_size,
                     "gradient colors must be separated with ':'");
            zsharp_paint_free(&parsed);
            return 0;
        }
        break;
    }
    if (parsed.color_count < 2) {
        snprintf(error, error_size,
                 "a gradient requires at least two colors");
        zsharp_paint_free(&parsed);
        return 0;
    }
    *paint = parsed;
    return 1;
}

uint32_t zsharp_paint_sample(const ZSharpPaint *paint, double position) {
    size_t left;
    size_t right;
    double scaled;
    double amount;
    uint32_t a;
    uint32_t b;
    unsigned red;
    unsigned green;
    unsigned blue;
    if (paint == NULL || paint->color_count == 0) return 0;
    if (position <= 0.0 || paint->color_count == 1) return paint->colors[0];
    if (position >= 1.0) return paint->colors[paint->color_count - 1];
    scaled = position * (double)(paint->color_count - 1);
    left = (size_t)scaled;
    right = left + 1;
    amount = scaled - (double)left;
    a = paint->colors[left];
    b = paint->colors[right];
    red = (unsigned)((double)((a >> 16) & 0xffu) * (1.0 - amount) +
                     (double)((b >> 16) & 0xffu) * amount + 0.5);
    green = (unsigned)((double)((a >> 8) & 0xffu) * (1.0 - amount) +
                       (double)((b >> 8) & 0xffu) * amount + 0.5);
    blue = (unsigned)((double)(a & 0xffu) * (1.0 - amount) +
                      (double)(b & 0xffu) * amount + 0.5);
    return (red << 16) | (green << 8) | blue;
}
