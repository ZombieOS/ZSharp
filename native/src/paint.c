#include "paint.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

int zsharp_color_parse(const char *text, uint32_t *color, unsigned char *alpha) {
    int digits[8];
    size_t index;
    size_t length;
    if (text == NULL) return 0;
    if (strcmp(text, "transparent") == 0) {
        *color = 0; *alpha = 0; return 1;
    }
    length = strlen(text);
    if (strncmp(text, "rgb(", 4) == 0 || strncmp(text, "rgba(", 5) == 0) {
        const char *cursor = text + (text[3] == '(' ? 4 : 5);
        double values[4] = {0, 0, 0, 1};
        int count = text[3] == '(' ? 3 : 4, i;
        for (i = 0; i < count; i++) {
            char *end;
            while (isspace((unsigned char)*cursor)) cursor++;
            values[i] = strtod(cursor, &end);
            if (cursor == end || !isfinite(values[i])) return 0;
            if (*end == '%') {
                values[i] *= i == 3 ? 0.01 : 2.55; end++;
            }
            if (values[i] < 0 || values[i] > (i == 3 ? 1 : 255)) return 0;
            while (isspace((unsigned char)*end)) end++;
            if (*end != (i + 1 == count ? ')' : ',')) return 0;
            cursor = end + 1;
        }
        if (*cursor != '\0') return 0;
        *color = ((uint32_t)(values[0] + 0.5) << 16) |
                 ((uint32_t)(values[1] + 0.5) << 8) | (uint32_t)(values[2] + 0.5);
        *alpha = (unsigned char)(values[3] * 255 + 0.5);
        return 1;
    }
    if ((length != 4 && length != 5 && length != 7 && length != 9) || text[0] != '#') return 0;
    if (length == 4 || length == 5) {
        int r = hex_digit(text[1]), g = hex_digit(text[2]), b = hex_digit(text[3]);
        int a = length == 5 ? hex_digit(text[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) return 0;
        *color = ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
        *alpha = (unsigned char)(a * 17); return 1;
    }
    for (index = 0; index < length - 1; index++) {
        digits[index] = hex_digit(text[index + 1]);
        if (digits[index] < 0) return 0;
    }
    *color = ((uint32_t)(digits[0] * 16 + digits[1]) << 16) |
             ((uint32_t)(digits[2] * 16 + digits[3]) << 8) |
             (uint32_t)(digits[4] * 16 + digits[5]);
    *alpha = length == 9 ? (unsigned char)(digits[6] * 16 + digits[7]) : 255;
    return 1;
}

uint32_t zsharp_color_over(uint32_t rgb, unsigned char alpha, uint32_t background) {
    uint32_t result = 0;
    unsigned shift;
    for (shift = 0; shift <= 16; shift += 8) {
        unsigned value = (((rgb >> shift) & 255) * alpha +
            ((background >> shift) & 255) * (255 - alpha) + 127) / 255;
        result |= value << shift;
    }
    return result;
}

int zsharp_paint_is_gradient_text(const char *text) {
    return text != NULL &&
        (strncmp(text, "linear-gradient(", 16) == 0 ||
         strncmp(text, "radial-gradient(", 16) == 0);
}

void zsharp_paint_free(ZSharpPaint *paint) {
    if (paint == NULL) return;
    free(paint->colors);
    free(paint->alphas);
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
        unsigned char alpha;
        if (!zsharp_color_parse(text, colors, &alpha)) {
            free(colors);
            snprintf(error, error_size,
                     "invalid color: use hex, rgb(), rgba(), transparent, or a Z# gradient");
            return 0;
        }
        parsed.kind = ZSHARP_PAINT_SOLID;
        parsed.colors = colors;
        parsed.alphas = (unsigned char *)malloc(1);
        if (parsed.alphas == NULL) { free(colors); return 0; }
        parsed.alphas[0] = alpha;
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
        unsigned char alpha;
        if (!zsharp_color_parse(color_text, &color, &alpha)) {
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

unsigned char zsharp_paint_alpha_sample(const ZSharpPaint *paint, double position) {
    double scaled;
    size_t first;
    if (paint == NULL || paint->color_count == 0) return 0;
    if (paint->alphas == NULL) return 255;
    if (position <= 0 || paint->color_count == 1) return paint->alphas[0];
    if (position >= 1) return paint->alphas[paint->color_count - 1];
    scaled = position * (double)(paint->color_count - 1);
    first = (size_t)scaled;
    return (unsigned char)(paint->alphas[first] * (1 - (scaled - first)) +
        paint->alphas[first + 1] * (scaled - first) + 0.5);
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
