#ifndef ZSHARP_WINDOW_LAYOUT_H
#define ZSHARP_WINDOW_LAYOUT_H
#include "bytecode.h"
typedef struct ZSharpLayoutRect { double x, y, width, height; int hidden; } ZSharpLayoutRect;
/* Rectangles are indexed by window element, initially filled by the backend's
 * legacy layout. CSS overrides leave unstyled projects unchanged. */
int zsharp_window_layout(const ZSharpWindow *window, double width, double height,
                         double scale, ZSharpLayoutRect *rectangles);
double zsharp_css_length(const char *text, double reference, double width,
                         double height, double scale, double fallback);
#endif
