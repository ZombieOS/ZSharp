#ifndef ZSHARP_PAINT_H
#define ZSHARP_PAINT_H

#include <stddef.h>
#include <stdint.h>

typedef enum ZSharpPaintKind {
    ZSHARP_PAINT_SOLID = 0,
    ZSHARP_PAINT_LINEAR = 1,
    ZSHARP_PAINT_RADIAL = 2
} ZSharpPaintKind;

typedef struct ZSharpPaint {
    ZSharpPaintKind kind;
    double degrees;
    uint32_t *colors;
    size_t color_count;
} ZSharpPaint;

int zsharp_paint_parse(const char *text, ZSharpPaint *paint,
                       char *error, size_t error_size);
void zsharp_paint_free(ZSharpPaint *paint);
uint32_t zsharp_paint_sample(const ZSharpPaint *paint, double position);
int zsharp_paint_is_gradient_text(const char *text);

#endif
