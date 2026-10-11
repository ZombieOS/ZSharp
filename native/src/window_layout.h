#ifndef ZSHARP_WINDOW_LAYOUT_H
#define ZSHARP_WINDOW_LAYOUT_H
#include "bytecode.h"
typedef struct ZSharpLayoutRect {
    double x, y, width, height;
    int hidden;
    double content_width, content_height, scroll_x, scroll_y;
} ZSharpLayoutRect;
/* Serialized parent indexes are one-based; zero means the window root.
 * Parents precede children, so layout can walk arbitrarily deep trees without
 * recursion. Element names remain globally unique for existing property paths. */
size_t zsharp_window_parent(const ZSharpUIElement *element);
/* Rectangles are indexed by window element, initially filled by the backend's
 * legacy layout. CSS overrides leave unstyled projects unchanged. */
int zsharp_window_layout(const ZSharpWindow *window, double width, double height,
                         double scale, ZSharpLayoutRect *rectangles);
double zsharp_css_length(const char *text, double reference, double width,
                         double height, double scale, double fallback);
#endif
