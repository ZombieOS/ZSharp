#include "window_layout.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _MSC_VER
#define ZSS_THREAD_LOCAL __declspec(thread)
#else
#define ZSS_THREAD_LOCAL _Thread_local
#endif
static ZSS_THREAD_LOCAL double viewport_width, viewport_height;

static const char *value(const ZSharpUIElement *element, const char *name) {
    size_t i;
    const char *result = NULL;
    for (i = 0; i < element->property_count; i++) {
        const char *field = element->properties[i].name;
        if (strcmp(field,name) == 0) result = element->properties[i].text_value;
        else if (strncmp(field,"media/",6) == 0) {
            double min_w,max_w,min_h,max_h;
            int consumed = 0;
            if (sscanf(field,"media/%lf/%lf/%lf/%lf/%n",&min_w,&max_w,&min_h,&max_h,&consumed) == 4 &&
                consumed > 0 && strcmp(field+consumed,name) == 0 &&
                viewport_width >= min_w && viewport_width <= max_w &&
                viewport_height >= min_h && viewport_height <= max_h)
                result = element->properties[i].text_value;
        }
    }
    return result;
}
static int equal(const char *a, const char *b) { return a != NULL && strcmp(a, b) == 0; }
double zsharp_css_length(const char *text, double reference, double width,
                         double height, double scale, double fallback) {
    char *end;
    double number;
    if (text == NULL) return fallback;
    number = strtod(text, &end);
    if (end == text || !isfinite(number)) return fallback;
    if (strcmp(end, "%") == 0) return number * reference / 100;
    if (strcmp(end, "vw") == 0) return number * width / 100;
    if (strcmp(end, "vh") == 0) return number * height / 100;
    if (strcmp(end, "zu") == 0) return number * 4 * scale;
    if (*end == '\0' || strcmp(end, "px") == 0) return number * scale;
    return fallback;
}
static double length(const ZSharpUIElement *element, const char *name,
                      double reference, double w, double h, double scale, double fallback) {
    return zsharp_css_length(value(element, name), reference, w, h, scale, fallback);
}
static double clamp(double number, double low, double high) {
    if (high < low) high = low;
    return number < low ? low : number > high ? high : number;
}
static double numeric(const ZSharpUIElement *element, const char *name, double fallback) {
    const char *text = value(element, name);
    return text == NULL ? fallback : strtod(text, NULL);
}
static void margins(const ZSharpUIElement *element, double w, double h,
                     double scale, double output[4]) {
    output[0] = length(element, "marginTop", w, w, h, scale, 0);
    output[1] = length(element, "marginRight", w, w, h, scale, 0);
    output[2] = length(element, "marginBottom", w, w, h, scale, 0);
    output[3] = length(element, "marginLeft", w, w, h, scale, 0);
}

int zsharp_window_layout(const ZSharpWindow *window, double w, double h,
                         double scale, ZSharpLayoutRect *rects) {
    const ZSharpUIElement *design = NULL;
    size_t i, count = 0, *items;
    double padding[4], main_gap, cross_gap, inner_w, inner_h;
    int grid, column, reverse, wrap;
    const char *display, *direction;
    if (window == NULL || rects == NULL) return 0;
    viewport_width = w / scale; viewport_height = h / scale;
    for (i = 0; i < window->element_count; i++) {
        const ZSharpUIElement *element = &window->elements[i];
        ZSharpLayoutRect *r = &rects[i];
        double m[4];
        if (element->type == ZUI_DESIGN) { design = element; continue; }
        r->hidden = equal(value(element, "display"), "none");
        r->width = length(element, "cssWidth", w, w, h, scale, r->width);
        r->height = length(element, "cssHeight", h, w, h, scale, r->height);
        r->width = clamp(r->width, length(element, "minWidth", w,w,h,scale,0),
            length(element, "maxWidth", w,w,h,scale,1e12));
        r->height = clamp(r->height, length(element, "minHeight", h,w,h,scale,0),
            length(element, "maxHeight", h,w,h,scale,1e12));
        margins(element, w, h, scale, m);
        r->x += m[3] - m[1]; r->y += m[0] - m[2];
        if (value(element, "left") != NULL) r->x = length(element,"left",w,w,h,scale,0) + m[3];
        else if (value(element,"right") != NULL) r->x = w - r->width - length(element,"right",w,w,h,scale,0) - m[1];
        if (value(element,"top") != NULL) r->y = length(element,"top",h,w,h,scale,0) + m[0];
        else if (value(element,"bottom") != NULL) r->y = h - r->height - length(element,"bottom",h,w,h,scale,0) - m[2];
    }
    if (design == NULL) return 1;
    display = value(design, "display");
    grid = equal(display,"grid");
    if (!grid && !equal(display,"flex")) return 1;
    items = (size_t *)malloc(window->element_count * sizeof(*items));
    if (items == NULL && window->element_count != 0) return 0;
    for (i = 0; i < window->element_count; i++) {
        const ZSharpUIElement *element = &window->elements[i];
        size_t position = count;
        if (element->type == ZUI_DESIGN || rects[i].hidden || equal(value(element,"position"),"absolute")) continue;
        while (position > 0 && numeric(&window->elements[items[position-1]],"order",0) > numeric(element,"order",0)) {
            items[position] = items[position-1]; position--;
        }
        items[position] = i; count++;
    }
    padding[0] = length(design,"paddingTop",w,w,h,scale,0);
    padding[1] = length(design,"paddingRight",w,w,h,scale,0);
    padding[2] = length(design,"paddingBottom",w,w,h,scale,0);
    padding[3] = length(design,"paddingLeft",w,w,h,scale,0);
    inner_w = fmax(0, w-padding[1]-padding[3]); inner_h = fmax(0,h-padding[0]-padding[2]);
    direction = value(design,"flexDirection");
    column = equal(direction,"column") || equal(direction,"column-reverse");
    reverse = equal(direction,"row-reverse") || equal(direction,"column-reverse");
    wrap = equal(value(design,"flexWrap"),"wrap");
    main_gap = length(design,column ? "rowGap" : "columnGap",w,w,h,scale,
                     length(design,"gap",w,w,h,scale,0));
    cross_gap = length(design,column ? "columnGap" : "rowGap",w,w,h,scale,
                      length(design,"gap",w,w,h,scale,0));
    if (grid) {
        unsigned columns = 1;
        double row_y = padding[0], gap_x = length(design,"columnGap",w,w,h,scale,length(design,"gap",w,w,h,scale,0));
        double gap_y = length(design,"rowGap",w,w,h,scale,length(design,"gap",w,w,h,scale,0));
        const char *template_text = value(design,"gridTemplateColumns");
        if (template_text != NULL) sscanf(template_text,"repeat(%u",&columns);
        if (!columns) columns = 1;
        for (i = 0; i < count; i += columns) {
            size_t j; double row_height = 0;
            double cell = fmax(0,(inner_w-gap_x*(columns-1))/columns);
            for (j = i; j < count && j < i+columns; j++) {
                double m[4]; ZSharpLayoutRect *r = &rects[items[j]];
                margins(&window->elements[items[j]],w,h,scale,m);
                row_height = fmax(row_height,r->height+m[0]+m[2]);
            }
            for (j = i; j < count && j < i+columns; j++) {
                const ZSharpUIElement *element = &window->elements[items[j]];
                ZSharpLayoutRect *r = &rects[items[j]]; double m[4];
                margins(element,w,h,scale,m);
                r->x = padding[3]+(j-i)*(cell+gap_x)+m[3]; r->y = row_y+m[0];
                r->width = clamp(length(element,"cssWidth",cell,w,h,scale,fmax(0,cell-m[1]-m[3])),
                    length(element,"minWidth",cell,w,h,scale,0),length(element,"maxWidth",cell,w,h,scale,1e12));
                if (equal(value(design,"alignItems"),"center")) r->y += (row_height-r->height-m[0]-m[2])/2;
                else if (equal(value(design,"alignItems"),"end")) r->y += row_height-r->height-m[0]-m[2];
                else if (equal(value(design,"alignItems"),"stretch") && value(element,"cssHeight") == NULL)
                    r->height = fmax(0,row_height-m[0]-m[2]);
            }
            row_y += row_height+gap_y;
        }
    } else {
        size_t first = 0; double cross_offset = 0, available = column ? inner_h : inner_w;
        while (first < count) {
            size_t end = first, j;
            double used = 0, grow = 0, shrink = 0, line_cross = 0, free_space, cursor, spacing = main_gap;
            while (end < count) {
                size_t idx = items[end]; const ZSharpUIElement *element = &window->elements[idx];
                ZSharpLayoutRect *r = &rects[idx]; double m[4], main, outer;
                margins(element,w,h,scale,m);
                main = length(element,"flexBasis",available,w,h,scale,column ? r->height : r->width);
                if (column) r->height = main; else r->width = main;
                outer = main + (column ? m[0]+m[2] : m[1]+m[3]);
                if (wrap && end > first && used + main_gap + outer > available) break;
                if (end > first) used += main_gap;
                used += outer;
                grow += numeric(element,"flexGrow",0);
                shrink += numeric(element,"flexShrink",1)*main;
                line_cross = fmax(line_cross,(column ? r->width+m[1]+m[3] : r->height+m[0]+m[2]));
                end++;
            }
            if (!wrap) line_cross = column ? inner_w : inner_h;
            free_space = available-used;
            if (free_space > 0 && grow > 0 || free_space < 0 && shrink > 0) {
                for (j = first; j < end; j++) {
                    const ZSharpUIElement *element = &window->elements[items[j]];
                    ZSharpLayoutRect *r = &rects[items[j]]; double main = column ? r->height : r->width;
                    double delta = free_space > 0 ? free_space*numeric(element,"flexGrow",0)/grow :
                        free_space*numeric(element,"flexShrink",1)*main/shrink;
                    main = fmax(0,main+delta);
                    if (column) r->height = main; else r->width = main;
                }
                free_space = 0;
            }
            free_space = fmax(0,free_space); cursor = 0;
            if (equal(value(design,"justifyContent"),"center")) cursor = free_space/2;
            else if (equal(value(design,"justifyContent"),"end") || equal(value(design,"justifyContent"),"flex-end")) cursor = free_space;
            else if (equal(value(design,"justifyContent"),"space-between") && end-first > 1) spacing += free_space/(end-first-1);
            else if (equal(value(design,"justifyContent"),"space-around")) { spacing += free_space/(end-first); cursor = free_space/(2*(end-first)); }
            else if (equal(value(design,"justifyContent"),"space-evenly")) { spacing += free_space/(end-first+1); cursor = free_space/(end-first+1); }
            for (j = first; j < end; j++) {
                size_t idx = items[j]; const ZSharpUIElement *element = &window->elements[idx];
                ZSharpLayoutRect *r = &rects[idx]; double m[4], main, cross, cross_size;
                const char *align = value(element,"alignSelf");
                if (align == NULL || equal(align,"auto")) align = value(design,"alignItems");
                margins(element,w,h,scale,m); main = column ? r->height : r->width;
                cursor += column ? m[0] : m[3];
                cross = cross_offset+(column ? m[3] : m[0]); cross_size = column ? r->width : r->height;
                if (equal(align,"center")) cross += (line_cross-cross_size-(column ? m[1]+m[3] : m[0]+m[2]))/2;
                else if (equal(align,"end") || equal(align,"flex-end")) cross += line_cross-cross_size-(column ? m[1]+m[3] : m[0]+m[2]);
                else if ((align == NULL || equal(align,"stretch")) && value(element,column ? "cssWidth" : "cssHeight") == NULL)
                    cross_size = fmax(0,line_cross-(column ? m[1]+m[3] : m[0]+m[2]));
                if (column) { r->x = padding[3]+cross; r->y = padding[0]+(reverse ? available-cursor-main : cursor); r->width = cross_size; }
                else { r->x = padding[3]+(reverse ? available-cursor-main : cursor); r->y = padding[0]+cross; r->height = cross_size; }
                cursor += main+(column ? m[2] : m[1])+spacing;
            }
            cross_offset += line_cross+cross_gap; first = end;
        }
    }
    free(items); return 1;
}
