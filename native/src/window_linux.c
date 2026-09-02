#include "window.h"

#include "paint.h"
#include "window_runtime.h"

#if defined(__linux__)

#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void GtkWidget;
typedef void GdkPixbuf;
typedef struct GtkAllocation { int x, y, width, height; } GtkAllocation;
typedef struct GtkTextIterStorage { void *words[16]; } GtkTextIterStorage;
typedef struct GdkRGBA { double red, green, blue, alpha; } GdkRGBA;
typedef struct GdkEventButton {
    int type;
    void *window;
    signed char send_event;
    unsigned int time;
    double x, y;
    double *axes;
    unsigned int state;
    unsigned int button;
} GdkEventButton;

typedef struct GtkApi {
    void *gtk;
    void *gobject;
    void *glib;
    void *pixbuf;
    int (*init_check)(int *, char ***);
    GtkWidget *(*window_new)(int);
    void (*window_set_title)(void *, const char *);
    void (*window_set_default_size)(void *, int, int);
    void (*window_resize)(void *, int, int);
    void (*window_move)(void *, int, int);
    void (*window_set_resizable)(void *, int);
    void (*window_set_position)(void *, int);
    int (*window_set_icon_from_file)(void *, const char *, void **);
    GtkWidget *(*fixed_new)(void);
    GtkWidget *(*event_box_new)(void);
    GtkWidget *(*scrolled_window_new)(void *, void *);
    void (*scrolled_window_set_policy)(void *, int, int);
    void (*container_add)(void *, void *);
    void (*fixed_put)(void *, void *, int, int);
    void (*fixed_move)(void *, void *, int, int);
    GtkWidget *(*label_new)(const char *);
    void (*label_set_markup)(void *, const char *);
    void (*label_set_line_wrap)(void *, int);
    void (*label_set_justify)(void *, int);
    GtkWidget *(*button_new_with_label)(const char *);
    void (*button_set_label)(void *, const char *);
    GtkWidget *(*entry_new)(void);
    void (*entry_set_placeholder_text)(void *, const char *);
    const char *(*entry_get_text)(void *);
    int (*editable_get_position)(void *);
    GtkWidget *(*text_view_new)(void);
    void *(*text_view_get_buffer)(void *);
    void (*text_view_set_wrap_mode)(void *, int);
    void (*text_buffer_set_text)(void *, const char *, int);
    void (*text_buffer_get_start_iter)(void *, void *);
    void (*text_buffer_get_end_iter)(void *, void *);
    char *(*text_buffer_get_text)(void *, const void *, const void *, int);
    void *(*text_buffer_get_insert)(void *);
    void (*text_buffer_get_iter_at_mark)(void *, void *, void *);
    int (*text_iter_get_offset)(const void *);
    GtkWidget *(*image_new_from_pixbuf)(void *);
    void (*image_set_from_pixbuf)(void *, void *);
    GtkWidget *(*file_chooser_button_new)(const char *, int);
    char *(*file_chooser_get_filename)(void *);
    void (*widget_set_size_request)(void *, int, int);
    void (*widget_show_all)(void *);
    void (*widget_destroy)(void *);
    int (*widget_get_scale_factor)(void *);
    int (*widget_get_allocated_width)(void *);
    int (*widget_get_allocated_height)(void *);
    void (*widget_queue_draw)(void *);
    void (*widget_set_name)(void *, const char *);
    void *(*widget_get_style_context)(void *);
    void (*widget_override_background_color)(void *, unsigned int,
                                              const GdkRGBA *);
    void (*widget_override_color)(void *, unsigned int, const GdkRGBA *);
    void (*main_loop)(void);
    void (*main_quit)(void);
    int (*events_pending)(void);
    int (*main_iteration_do)(int);
    void *(*css_provider_new)(void);
    int (*css_provider_load_from_data)(void *, const char *, long, void **);
    void (*style_context_add_provider)(void *, void *, unsigned int);
    GtkWidget *(*message_dialog_new)(void *, unsigned int, int, int,
                                     const char *, ...);
    int (*dialog_run)(void *);
    unsigned long (*signal_connect_data)(void *, const char *, void *, void *,
                                         void *, int);
    unsigned int (*timeout_add)(unsigned int, int (*)(void *), void *);
    unsigned int (*idle_add)(int (*)(void *), void *);
    GdkPixbuf *(*pixbuf_new_from_file_at_scale)(const char *, int, int, int,
                                                void **);
    void (*object_unref)(void *);
    void (*g_free)(void *);
    void *(*screen_get_default)(void);
    int (*screen_get_width)(void *);
    int (*screen_get_height)(void *);
} GtkApi;

struct LinuxWindowState;

typedef struct LinuxControl {
    GtkWidget *widget;
    GtkWidget *input_widget;
    void *text_buffer;
    ZSharpUIElement *element;
    int is_image_input;
    int is_multiline;
    int placeholder_active;
    int image_width;
    int image_height;
    void *css_provider;
    void *zss_provider;
    struct LinuxWindowState *state;
} LinuxControl;

typedef struct LinuxWindowState {
    GtkApi api;
    ZSharpProgram *program;
    const char *project_root;
    ZSharpWindowCallback callback;
    void *callback_data;
    GtkWidget *window;
    GtkWidget *scrolled;
    GtkWidget *fixed;
    LinuxControl *controls;
    size_t control_count;
    double scale;
    int screen_width;
    int screen_height;
    int layout_width;
    int layout_height;
    char callback_error[512];
    void *background_css;
    ZSharpWindowRuntime runtime;
    volatile int closing;
    volatile int pending_requests;
} LinuxWindowState;

typedef struct LinuxPropertyRequest {
    LinuxWindowState *state;
    const char *path;
    ZSharpWindowValueType value_type;
    const char *text_value;
    ZSharpUIUnit unit;
    volatile int done;
    int result;
    char error[512];
} LinuxPropertyRequest;

typedef struct LinuxReadRequest {
    LinuxWindowState *state;
    const char *path;
    ZSharpWindowReadType value_type;
    char *text_value;
    volatile int done;
    int result;
    char error[512];
} LinuxReadRequest;

static void *load_symbol(void *library, const char *name) {
    return library == NULL ? NULL : dlsym(library, name);
}

#define GTK_REQUIRED(api, field, library, name) do { \
    *(void **)(&(api)->field) = load_symbol((library), (name)); \
    if ((api)->field == NULL) return 0; \
} while (0)

static int gtk_api_load(GtkApi *api, char *error, size_t error_size) {
    memset(api, 0, sizeof(*api));
    api->gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_LOCAL);
    api->gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    api->glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    api->pixbuf = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (api->gtk == NULL || api->gobject == NULL || api->glib == NULL) {
        snprintf(error, error_size,
                 "Linux window apps require the GTK 3 desktop runtime "
                 "(libgtk-3.so.0)");
        return 0;
    }
    GTK_REQUIRED(api, init_check, api->gtk, "gtk_init_check");
    GTK_REQUIRED(api, window_new, api->gtk, "gtk_window_new");
    GTK_REQUIRED(api, window_set_title, api->gtk, "gtk_window_set_title");
    GTK_REQUIRED(api, window_set_default_size, api->gtk,
                 "gtk_window_set_default_size");
    GTK_REQUIRED(api, window_resize, api->gtk, "gtk_window_resize");
    GTK_REQUIRED(api, window_move, api->gtk, "gtk_window_move");
    GTK_REQUIRED(api, window_set_resizable, api->gtk,
                 "gtk_window_set_resizable");
    GTK_REQUIRED(api, window_set_position, api->gtk,
                 "gtk_window_set_position");
    GTK_REQUIRED(api, fixed_new, api->gtk, "gtk_fixed_new");
    GTK_REQUIRED(api, event_box_new, api->gtk, "gtk_event_box_new");
    GTK_REQUIRED(api, scrolled_window_new, api->gtk,
                 "gtk_scrolled_window_new");
    GTK_REQUIRED(api, scrolled_window_set_policy, api->gtk,
                 "gtk_scrolled_window_set_policy");
    GTK_REQUIRED(api, container_add, api->gtk, "gtk_container_add");
    GTK_REQUIRED(api, fixed_put, api->gtk, "gtk_fixed_put");
    GTK_REQUIRED(api, fixed_move, api->gtk, "gtk_fixed_move");
    GTK_REQUIRED(api, label_new, api->gtk, "gtk_label_new");
    GTK_REQUIRED(api, label_set_markup, api->gtk, "gtk_label_set_markup");
    GTK_REQUIRED(api, label_set_line_wrap, api->gtk,
                 "gtk_label_set_line_wrap");
    GTK_REQUIRED(api, label_set_justify, api->gtk,
                 "gtk_label_set_justify");
    GTK_REQUIRED(api, button_new_with_label, api->gtk,
                 "gtk_button_new_with_label");
    GTK_REQUIRED(api, button_set_label, api->gtk, "gtk_button_set_label");
    GTK_REQUIRED(api, entry_new, api->gtk, "gtk_entry_new");
    GTK_REQUIRED(api, entry_set_placeholder_text, api->gtk,
                 "gtk_entry_set_placeholder_text");
    GTK_REQUIRED(api, entry_get_text, api->gtk, "gtk_entry_get_text");
    GTK_REQUIRED(api, editable_get_position, api->gtk,
                 "gtk_editable_get_position");
    GTK_REQUIRED(api, text_view_new, api->gtk, "gtk_text_view_new");
    GTK_REQUIRED(api, text_view_get_buffer, api->gtk,
                 "gtk_text_view_get_buffer");
    GTK_REQUIRED(api, text_view_set_wrap_mode, api->gtk,
                 "gtk_text_view_set_wrap_mode");
    GTK_REQUIRED(api, text_buffer_set_text, api->gtk,
                 "gtk_text_buffer_set_text");
    GTK_REQUIRED(api, text_buffer_get_start_iter, api->gtk,
                 "gtk_text_buffer_get_start_iter");
    GTK_REQUIRED(api, text_buffer_get_end_iter, api->gtk,
                 "gtk_text_buffer_get_end_iter");
    GTK_REQUIRED(api, text_buffer_get_text, api->gtk,
                 "gtk_text_buffer_get_text");
    GTK_REQUIRED(api, text_buffer_get_insert, api->gtk,
                 "gtk_text_buffer_get_insert");
    GTK_REQUIRED(api, text_buffer_get_iter_at_mark, api->gtk,
                 "gtk_text_buffer_get_iter_at_mark");
    GTK_REQUIRED(api, text_iter_get_offset, api->gtk,
                 "gtk_text_iter_get_offset");
    GTK_REQUIRED(api, image_new_from_pixbuf, api->gtk,
                 "gtk_image_new_from_pixbuf");
    GTK_REQUIRED(api, image_set_from_pixbuf, api->gtk,
                 "gtk_image_set_from_pixbuf");
    GTK_REQUIRED(api, file_chooser_button_new, api->gtk,
                 "gtk_file_chooser_button_new");
    GTK_REQUIRED(api, file_chooser_get_filename, api->gtk,
                 "gtk_file_chooser_get_filename");
    GTK_REQUIRED(api, widget_set_size_request, api->gtk,
                 "gtk_widget_set_size_request");
    GTK_REQUIRED(api, widget_show_all, api->gtk, "gtk_widget_show_all");
    GTK_REQUIRED(api, widget_destroy, api->gtk, "gtk_widget_destroy");
    GTK_REQUIRED(api, widget_get_scale_factor, api->gtk,
                 "gtk_widget_get_scale_factor");
    GTK_REQUIRED(api, widget_get_allocated_width, api->gtk,
                 "gtk_widget_get_allocated_width");
    GTK_REQUIRED(api, widget_get_allocated_height, api->gtk,
                 "gtk_widget_get_allocated_height");
    GTK_REQUIRED(api, widget_queue_draw, api->gtk, "gtk_widget_queue_draw");
    GTK_REQUIRED(api, widget_set_name, api->gtk, "gtk_widget_set_name");
    GTK_REQUIRED(api, widget_get_style_context, api->gtk,
                 "gtk_widget_get_style_context");
    GTK_REQUIRED(api, main_loop, api->gtk, "gtk_main");
    GTK_REQUIRED(api, main_quit, api->gtk, "gtk_main_quit");
    GTK_REQUIRED(api, events_pending, api->gtk, "gtk_events_pending");
    GTK_REQUIRED(api, main_iteration_do, api->gtk, "gtk_main_iteration_do");
    GTK_REQUIRED(api, css_provider_new, api->gtk, "gtk_css_provider_new");
    GTK_REQUIRED(api, css_provider_load_from_data, api->gtk,
                 "gtk_css_provider_load_from_data");
    GTK_REQUIRED(api, style_context_add_provider, api->gtk,
                 "gtk_style_context_add_provider");
    GTK_REQUIRED(api, message_dialog_new, api->gtk,
                 "gtk_message_dialog_new");
    GTK_REQUIRED(api, dialog_run, api->gtk, "gtk_dialog_run");
    GTK_REQUIRED(api, signal_connect_data, api->gobject,
                 "g_signal_connect_data");
    GTK_REQUIRED(api, timeout_add, api->glib, "g_timeout_add");
    GTK_REQUIRED(api, idle_add, api->glib, "g_idle_add");
    GTK_REQUIRED(api, g_free, api->glib, "g_free");
    *(void **)(&api->window_set_icon_from_file) =
        load_symbol(api->gtk, "gtk_window_set_icon_from_file");
    *(void **)(&api->widget_override_background_color) =
        load_symbol(api->gtk, "gtk_widget_override_background_color");
    *(void **)(&api->widget_override_color) =
        load_symbol(api->gtk, "gtk_widget_override_color");
    *(void **)(&api->object_unref) =
        load_symbol(api->gobject, "g_object_unref");
    if (api->pixbuf != NULL) {
        *(void **)(&api->pixbuf_new_from_file_at_scale) =
            load_symbol(api->pixbuf, "gdk_pixbuf_new_from_file_at_scale");
    }
    *(void **)(&api->screen_get_default) =
        load_symbol(api->gtk, "gdk_screen_get_default");
    *(void **)(&api->screen_get_width) =
        load_symbol(api->gtk, "gdk_screen_get_width");
    *(void **)(&api->screen_get_height) =
        load_symbol(api->gtk, "gdk_screen_get_height");
    return 1;
}

static void gtk_api_close(GtkApi *api) {
    if (api->pixbuf != NULL) dlclose(api->pixbuf);
    if (api->glib != NULL) dlclose(api->glib);
    if (api->gobject != NULL) dlclose(api->gobject);
    if (api->gtk != NULL) dlclose(api->gtk);
    memset(api, 0, sizeof(*api));
}

static ZSharpUIProperty *property(ZSharpUIElement *element, const char *name) {
    size_t index;
    for (index = 0; index < element->property_count; index++)
        if (strcmp(element->properties[index].name, name) == 0)
            return &element->properties[index];
    return NULL;
}

static ZSharpUIElement *design_element(ZSharpWindow *window) {
    size_t index;
    for (index = 0; index < window->element_count; index++)
        if (window->elements[index].type == ZUI_DESIGN)
            return &window->elements[index];
    return NULL;
}

static int status_property(ZSharpUIElement *element, const char *name,
                           int fallback) {
    ZSharpUIProperty *value = property(element, name);
    return value == NULL ? fallback : value->status_value;
}

static char *path_join(const char *root, const char *relative) {
    size_t a = strlen(root), b = strlen(relative);
    if (relative[0] == '/') return zsharp_copy_text(relative, b);
    int slash = a != 0 && root[a - 1] != '/';
    char *result = (char *)malloc(a + (size_t)slash + b + 1);
    if (result == NULL) return NULL;
    memcpy(result, root, a);
    if (slash) result[a++] = '/';
    memcpy(result + a, relative, b + 1);
    return result;
}

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_rgba(const char *text, GdkRGBA *color) {
    int digits[6];
    size_t index;
    if (text == NULL || strlen(text) != 7 || text[0] != '#') return 0;
    for (index = 0; index < 6; index++) {
        digits[index] = hex_digit(text[index + 1]);
        if (digits[index] < 0) return 0;
    }
    color->red = (double)(digits[0] * 16 + digits[1]) / 255.0;
    color->green = (double)(digits[2] * 16 + digits[3]) / 255.0;
    color->blue = (double)(digits[4] * 16 + digits[5]) / 255.0;
    color->alpha = 1.0;
    return 1;
}

static char *paint_css(const char *selector_text, const char *paint_text,
                       char *error, size_t error_size) {
    ZSharpPaint paint;
    const char *property_name;
    size_t length;
    size_t index;
    char *css;
    char *cursor;
    memset(&paint, 0, sizeof(paint));
    if (!zsharp_paint_parse(paint_text, &paint, error, error_size)) return NULL;
    property_name = paint.kind == ZSHARP_PAINT_SOLID
        ? "background-color:" : "background-image:";
    length = strlen(selector_text) + strlen(property_name) + 160 +
             paint.color_count * 40;
    css = (char *)malloc(length);
    if (css == NULL) {
        zsharp_paint_free(&paint);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    cursor = css;
    cursor += sprintf(cursor, "%s{%s", selector_text, property_name);
    if (paint.kind == ZSHARP_PAINT_SOLID) {
        cursor += sprintf(cursor, "#%06X", (unsigned)paint.colors[0]);
    } else if (paint.kind == ZSHARP_PAINT_LINEAR) {
        cursor += sprintf(cursor, "linear-gradient(%.12gdeg", paint.degrees);
        for (index = 0; index < paint.color_count; index++)
            cursor += sprintf(cursor, ",#%06X", (unsigned)paint.colors[index]);
        *cursor++ = ')';
    } else {
        double radians = paint.degrees * 3.14159265358979323846 / 180.0;
        double x = 0.5 + sin(radians) * 0.2;
        double y = 0.5 - cos(radians) * 0.2;
        cursor += sprintf(cursor,
            "-gtk-gradient(radial,%.6g %.6g,0,center center,1",
            x, y);
        for (index = 0; index < paint.color_count; index++) {
            if (index == 0)
                cursor += sprintf(cursor, ",from(#%06X)",
                                  (unsigned)paint.colors[index]);
            else if (index + 1 == paint.color_count)
                cursor += sprintf(cursor, ",to(#%06X)",
                                  (unsigned)paint.colors[index]);
            else
                cursor += sprintf(cursor, ",color-stop(%.12g,#%06X)",
                    (double)index / (double)(paint.color_count - 1),
                    (unsigned)paint.colors[index]);
        }
        *cursor++ = ')';
    }
    strcpy(cursor, ";}");
    zsharp_paint_free(&paint);
    return css;
}

static int apply_css(LinuxWindowState *state, GtkWidget *widget,
                     void **provider_slot, const char *selector_text,
                     const char *paint_text, char *error, size_t error_size) {
    char *css = paint_css(selector_text, paint_text, error, error_size);
    void *provider;
    if (css == NULL) return 0;
    provider = *provider_slot;
    if (provider == NULL) {
        provider = state->api.css_provider_new();
        if (provider == NULL) {
            free(css);
            snprintf(error, error_size, "could not create GTK gradient style");
            return 0;
        }
        state->api.style_context_add_provider(
            state->api.widget_get_style_context(widget), provider, 800u);
        *provider_slot = provider;
    }
    if (!state->api.css_provider_load_from_data(provider, css, -1, NULL)) {
        free(css);
        snprintf(error, error_size, "GTK could not load the window gradient");
        return 0;
    }
    free(css);
    state->api.widget_queue_draw(widget);
    return 1;
}

static int pixels(const ZSharpUIProperty *value, double scale, int fallback) {
    double parsed;
    if (value == NULL || value->text_value == NULL) return fallback;
    parsed = strtod(value->text_value, NULL);
    if (value->unit == ZUI_UNIT_ZU) parsed *= 4.0 * scale;
    return (int)(parsed < 0.0 ? parsed - 0.5 : parsed + 0.5);
}

static int append_css(char **cursor, size_t *remaining,
                      const char *format, ...) {
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(*cursor, *remaining, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= *remaining) return 0;
    *cursor += written;
    *remaining -= (size_t)written;
    return 1;
}

static int append_measurement_css(char **cursor, size_t *remaining,
                                  const char *css_name,
                                  const ZSharpUIProperty *value,
                                  double scale) {
    if (value == NULL) return 1;
    return append_css(cursor, remaining, "%s:%dpx;", css_name,
                      pixels(value, scale, 0));
}

static int apply_element_zss(LinuxWindowState *state, LinuxControl *control,
                             char *error, size_t error_size) {
    ZSharpUIElement *element = control->element;
    GtkWidget *target = control->is_multiline && control->input_widget != NULL
        ? control->input_widget : control->widget;
    const char *selector = element->type == ZUI_TEXT ? "label" :
        element->type == ZUI_BUTTON ? "button" :
        element->type == ZUI_TEXT_INPUT
            ? (control->is_multiline ? "textview" : "entry") : "*";
    ZSharpUIProperty *background = property(
        element, element->type == ZUI_BUTTON ? "buttonColor" :
                                               "backgroundColor");
    ZSharpUIProperty *color = property(
        element, element->type == ZUI_TEXT ? "color" : "textColor");
    ZSharpUIProperty *border_width = property(element, "borderWidth");
    ZSharpUIProperty *border_color = property(element, "borderColor");
    ZSharpUIProperty *border_radius = property(element, "borderRadius");
    ZSharpUIProperty *family = property(element, "fontFamily");
    ZSharpUIProperty *font_size = property(element, "fontSize");
    ZSharpUIProperty *weight = property(element, "fontWeight");
    ZSharpUIProperty *padding = property(element, "padding");
    ZSharpUIProperty *padding_left = property(element, "paddingLeft");
    ZSharpUIProperty *padding_right = property(element, "paddingRight");
    ZSharpUIProperty *padding_top = property(element, "paddingTop");
    ZSharpUIProperty *padding_bottom = property(element, "paddingBottom");
    ZSharpUIProperty *caret = property(element, "caretColor");
    ZSharpUIProperty *selection_background = property(
        element, "selectionBackground");
    ZSharpUIProperty *selection_color = property(element, "selectionColor");
    ZSharpUIProperty *hover_background = property(
        element, element->type == ZUI_BUTTON ? "hoverButtonColor" :
                                               "hoverBackgroundColor");
    ZSharpUIProperty *hover_color = property(
        element, element->type == ZUI_TEXT ? "hoverColor" :
                                             "hoverTextColor");
    ZSharpUIProperty *hover_border = property(element, "hoverBorderColor");
    ZSharpUIProperty *focus_border = property(element, "focusBorderColor");
    size_t capacity = 8192;
    size_t index;
    char *css;
    char *write;
    size_t remaining;
    int has_style = 0;
    for (index = 0; index < element->property_count; index++)
        if (element->properties[index].text_value != NULL)
            capacity += strlen(element->properties[index].text_value) * 4;
    css = (char *)malloc(capacity);
    if (css == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    write = css;
    remaining = capacity;
    if (!append_css(&write, &remaining, "%s{", selector)) goto too_large;
    if (background != NULL && background->text_value[0] == '#') {
        if (!append_css(&write, &remaining, "background-color:%s;",
                        background->text_value)) goto too_large;
        has_style = 1;
    }
    if (color != NULL) {
        if (!append_css(&write, &remaining, "color:%s;", color->text_value))
            goto too_large;
        has_style = 1;
    }
    if (border_width != NULL) {
        if (!append_measurement_css(&write, &remaining, "border-width",
                                    border_width, state->scale) ||
            !append_css(&write, &remaining, "border-style:solid;"))
            goto too_large;
        has_style = 1;
    }
    if (border_color != NULL) {
        if (!append_css(&write, &remaining, "border-color:%s;",
                        border_color->text_value)) goto too_large;
        has_style = 1;
    }
    if (border_radius != NULL) {
        if (!append_measurement_css(&write, &remaining, "border-radius",
                                    border_radius, state->scale))
            goto too_large;
        has_style = 1;
    }
    if (family != NULL) {
        if (!append_css(&write, &remaining, "font-family:%s;",
                        family->text_value)) goto too_large;
        has_style = 1;
    }
    if (font_size != NULL) {
        if (!append_measurement_css(&write, &remaining, "font-size",
                                    font_size, state->scale)) goto too_large;
        has_style = 1;
    }
    if (weight != NULL) {
        if (!append_css(&write, &remaining, "font-weight:%s;",
                        weight->text_value)) goto too_large;
        has_style = 1;
    }
    if (padding != NULL) {
        if (!append_measurement_css(&write, &remaining, "padding", padding,
                                    state->scale)) goto too_large;
        has_style = 1;
    }
    if (padding_left != NULL || padding_right != NULL ||
        padding_top != NULL || padding_bottom != NULL) {
        if (!append_measurement_css(&write, &remaining, "padding-left",
                                    padding_left, state->scale) ||
            !append_measurement_css(&write, &remaining, "padding-right",
                                    padding_right, state->scale) ||
            !append_measurement_css(&write, &remaining, "padding-top",
                                    padding_top, state->scale) ||
            !append_measurement_css(&write, &remaining, "padding-bottom",
                                    padding_bottom, state->scale))
            goto too_large;
        has_style = 1;
    }
    if (caret != NULL) {
        if (!append_css(&write, &remaining, "caret-color:%s;",
                        caret->text_value)) goto too_large;
        has_style = 1;
    }
    if (property(element, "outline") != NULL) {
        if (!append_css(&write, &remaining, "outline-style:none;"))
            goto too_large;
        has_style = 1;
    }
    if (!append_css(&write, &remaining, "}")) goto too_large;
    if (hover_background != NULL || hover_color != NULL ||
        hover_border != NULL) {
        if (!append_css(&write, &remaining, "%s:hover{", selector))
            goto too_large;
        if (hover_background != NULL && hover_background->text_value[0] == '#' &&
            !append_css(&write, &remaining, "background-color:%s;",
                        hover_background->text_value)) goto too_large;
        if (hover_color != NULL &&
            !append_css(&write, &remaining, "color:%s;",
                        hover_color->text_value)) goto too_large;
        if (hover_border != NULL &&
            !append_css(&write, &remaining, "border-color:%s;",
                        hover_border->text_value)) goto too_large;
        if (!append_css(&write, &remaining, "}")) goto too_large;
        has_style = 1;
    }
    if (focus_border != NULL) {
        if (!append_css(&write, &remaining,
                        "%s:focus{border-color:%s;}", selector,
                        focus_border->text_value)) goto too_large;
        has_style = 1;
    }
    if (selection_background != NULL || selection_color != NULL) {
        if (!append_css(&write, &remaining, "%s selection{", selector))
            goto too_large;
        if (selection_background != NULL &&
            !append_css(&write, &remaining, "background-color:%s;",
                        selection_background->text_value)) goto too_large;
        if (selection_color != NULL &&
            !append_css(&write, &remaining, "color:%s;",
                        selection_color->text_value)) goto too_large;
        if (!append_css(&write, &remaining, "}")) goto too_large;
        has_style = 1;
    }
    if (has_style) {
        control->zss_provider = state->api.css_provider_new();
        if (control->zss_provider == NULL ||
            !state->api.css_provider_load_from_data(control->zss_provider,
                                                     css, -1, NULL)) {
            free(css);
            snprintf(error, error_size,
                     "GTK could not apply ZSS to '%s'", element->name);
            return 0;
        }
        state->api.style_context_add_provider(
            state->api.widget_get_style_context(target),
            control->zss_provider, 850u);
    }
    free(css);
    return 1;
too_large:
    free(css);
    snprintf(error, error_size, "ZSS for '%s' is too large", element->name);
    return 0;
}

static void default_size(const ZSharpUIElement *element, int *width,
                         int *height) {
    *width = 200;
    *height = 32;
    if (element->type == ZUI_TEXT) {
        *width = 400;
        *height = element->variant != NULL &&
                  strcmp(element->variant, "title") == 0 ? 64 : 44;
    }
}

static void update_contents(LinuxWindowState *state, LinuxControl *control,
                            const char *text) {
    ZSharpUIProperty *contents = property(control->element, "contents");
    char *copy;
    (void)state;
    if (contents == NULL || text == NULL) return;
    copy = zsharp_copy_text(text, strlen(text));
    if (copy == NULL) return;
    free(contents->text_value);
    contents->text_value = copy;
}

static void show_callback_error(LinuxWindowState *state, const char *message) {
    GtkWidget *dialog = state->api.message_dialog_new(
        state->window, 1u, 3, 2, "%s", message);
    if (dialog == NULL) return;
    state->api.dialog_run(dialog);
    state->api.widget_destroy(dialog);
}

static int finish_close(void *data) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    if (__atomic_load_n(&state->pending_requests, __ATOMIC_ACQUIRE) != 0)
        return 1;
    state->api.main_quit();
    return 0;
}

static void run_target(LinuxWindowState *state, LinuxControl *control,
                       const char *name) {
    ZSharpUIProperty *target = property(control->element, name);
    if (target == NULL || target->text_value == NULL ||
        target->text_value[0] == '\0') return;
    state->callback_error[0] = '\0';
    if (!state->callback(state->callback_data, target->text_value,
                         &state->runtime,
                         state->callback_error,
                         sizeof(state->callback_error)))
        show_callback_error(state, state->callback_error[0] == '\0'
                                   ? "The Z# callback failed."
                                   : state->callback_error);
}

static void window_destroyed(GtkWidget *widget, void *data) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    (void)widget;
    __atomic_store_n(&state->closing, 1, __ATOMIC_RELEASE);
    state->api.idle_add(finish_close, state);
}

static void button_clicked(GtkWidget *widget, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    (void)widget;
    run_target(control->state, control, "left");
}

static LinuxWindowState *control_state(LinuxControl *control) {
    return control->state;
}

static int button_pressed(GtkWidget *widget, GdkEventButton *event,
                          void *data) {
    LinuxControl *control = (LinuxControl *)data;
    (void)widget;
    if (event != NULL && event->button == 3u) {
        run_target(control_state(control), control, "right");
        return 1;
    }
    return 0;
}

static int image_pressed(GtkWidget *widget, GdkEventButton *event,
                         void *data) {
    LinuxControl *control = (LinuxControl *)data;
    (void)widget;
    if (event == NULL) return 0;
    if (event->button == 1u) {
        run_target(control_state(control), control, "left");
        return 1;
    }
    if (event->button == 3u) {
        run_target(control_state(control), control, "right");
        return 1;
    }
    return 0;
}

static void entry_changed(GtkWidget *widget, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    LinuxWindowState *state = control_state(control);
    update_contents(state, control, state->api.entry_get_text(widget));
}

static char *text_buffer_text(LinuxWindowState *state,
                              LinuxControl *control) {
    GtkTextIterStorage start;
    GtkTextIterStorage end;
    if (control->text_buffer == NULL) return NULL;
    memset(&start, 0, sizeof(start));
    memset(&end, 0, sizeof(end));
    state->api.text_buffer_get_start_iter(control->text_buffer, &start);
    state->api.text_buffer_get_end_iter(control->text_buffer, &end);
    return state->api.text_buffer_get_text(control->text_buffer, &start, &end,
                                           1);
}

static void text_buffer_changed(void *buffer, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    LinuxWindowState *state = control_state(control);
    char *text;
    (void)buffer;
    if (control->placeholder_active) {
        update_contents(state, control, "");
        return;
    }
    text = text_buffer_text(state, control);
    if (text != NULL) {
        update_contents(state, control, text);
        state->api.g_free(text);
    }
}

static int multiline_focus_in(GtkWidget *widget, void *event, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    (void)widget;
    (void)event;
    if (control->placeholder_active) {
        control->placeholder_active = 0;
        control->state->api.text_buffer_set_text(control->text_buffer, "", -1);
    }
    return 0;
}

static int multiline_focus_out(GtkWidget *widget, void *event, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    LinuxWindowState *state = control_state(control);
    ZSharpUIProperty *display = property(control->element, "display");
    char *text = text_buffer_text(state, control);
    (void)widget;
    (void)event;
    if (text != NULL && text[0] == '\0' && display != NULL &&
        display->text_value != NULL && display->text_value[0] != '\0') {
        control->placeholder_active = 1;
        state->api.text_buffer_set_text(control->text_buffer,
                                        display->text_value, -1);
        update_contents(state, control, "");
    }
    if (text != NULL) state->api.g_free(text);
    return 0;
}

static void file_selected(GtkWidget *widget, void *data) {
    LinuxControl *control = (LinuxControl *)data;
    LinuxWindowState *state = control_state(control);
    char *filename = state->api.file_chooser_get_filename(widget);
    if (filename != NULL) {
        update_contents(state, control, filename);
        state->api.g_free(filename);
    }
}

static void layout_controls(LinuxWindowState *state, int width, int height) {
    size_t index;
    int content_bottom = 0;
    double responsive_scale = state->layout_width > 0
        ? (double)width / (double)state->layout_width : 1.0;
    if (responsive_scale > 1.0) responsive_scale = 1.0;
    if (responsive_scale < 0.05) responsive_scale = 0.05;
    for (index = 0; index < state->control_count; index++) {
        LinuxControl *control = &state->controls[index];
        ZSharpUIElement *element = control->element;
        int fallback_width, fallback_height, w, h, x, y;
        default_size(element, &fallback_width, &fallback_height);
        w = (int)((double)pixels(property(element, "width"), state->scale,
                                fallback_width) * responsive_scale + 0.5);
        h = (int)((double)pixels(property(element, "height"), state->scale,
                                fallback_height) * responsive_scale + 0.5);
        if (element->type != ZUI_TEXT) {
            if (w < 32) w = 32;
            if (h < 16) h = 16;
        } else if (w < 24) {
            w = 24;
        }
        x = (int)(((double)state->layout_width / 2.0 +
            (double)pixels(property(element, "locationX"), state->scale, 0) -
            (double)pixels(property(element, "width"), state->scale,
                           fallback_width) / 2.0) * responsive_scale + 0.5);
        y = (int)(((double)state->layout_height / 2.0 -
            (double)pixels(property(element, "locationY"), state->scale, 0) -
            (double)pixels(property(element, "height"), state->scale,
                           fallback_height) / 2.0) * responsive_scale + 0.5);
        state->api.widget_set_size_request(
            control->widget, w,
            element->type == ZUI_TEXT && property(element, "height") == NULL
                ? -1 : h);
        state->api.fixed_move(state->fixed, control->widget, x, y);
        if (element->type == ZUI_IMAGE &&
            (control->image_width != w || control->image_height != h) &&
            state->api.pixbuf_new_from_file_at_scale != NULL) {
            ZSharpUIProperty *file = property(element, "file");
            char *path = file == NULL ? NULL :
                path_join(state->project_root, file->text_value);
            GdkPixbuf *pixbuf = path == NULL ? NULL :
                state->api.pixbuf_new_from_file_at_scale(path, w, h, 1, NULL);
            if (pixbuf != NULL) {
                state->api.image_set_from_pixbuf(
                    control->input_widget != NULL ? control->input_widget
                                                   : control->widget,
                    pixbuf);
                if (state->api.object_unref != NULL)
                    state->api.object_unref(pixbuf);
                control->image_width = w;
                control->image_height = h;
            }
            free(path);
        }
        if (element->type == ZUI_TEXT && property(element, "height") == NULL) {
            int allocated = state->api.widget_get_allocated_height(
                control->widget);
            if (allocated > h) h = allocated;
        }
        if (y + h > content_bottom) content_bottom = y + h;
    }
    content_bottom = content_bottom > height ? content_bottom + 16 : height;
    state->api.widget_set_size_request(state->fixed, width, content_bottom);
}

static void window_allocated(GtkWidget *widget, GtkAllocation *allocation,
                             void *data) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    (void)widget;
    if (allocation != NULL)
        layout_controls(state, allocation->width, allocation->height);
}

static int autoclose_window(void *data) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    state->api.widget_destroy(state->window);
    return 0;
}

static char *markup_escape(const char *text) {
    size_t length = 1;
    const char *cursor;
    char *result;
    char *out;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '&') length += 5;
        else if (*cursor == '<' || *cursor == '>') length += 4;
        else if (*cursor == '\"' || *cursor == '\'') length += 6;
        else length++;
    }
    result = (char *)malloc(length);
    if (result == NULL) return NULL;
    out = result;
    for (cursor = text; *cursor != '\0'; cursor++) {
        const char *replacement = NULL;
        if (*cursor == '&') replacement = "&amp;";
        else if (*cursor == '<') replacement = "&lt;";
        else if (*cursor == '>') replacement = "&gt;";
        else if (*cursor == '\"') replacement = "&quot;";
        else if (*cursor == '\'') replacement = "&apos;";
        if (replacement != NULL) {
            size_t count = strlen(replacement);
            memcpy(out, replacement, count);
            out += count;
        } else *out++ = *cursor;
    }
    *out = '\0';
    return result;
}

static GtkWidget *make_label(LinuxWindowState *state,
                             ZSharpUIElement *element) {
    ZSharpUIProperty *content = property(element, "content");
    ZSharpUIProperty *color = property(element, "color");
    const char *size = "medium";
    const char *weight = "normal";
    char *escaped;
    char *markup;
    GtkWidget *widget;
    if (element->variant != NULL) {
        if (strcmp(element->variant, "title") == 0) {
            size = "xx-large"; weight = "bold";
        } else if (strcmp(element->variant, "subtitle") == 0) size = "x-large";
        else if (strcmp(element->variant, "header") == 0) {
            size = "large"; weight = "bold";
        } else if (strcmp(element->variant, "subheader") == 0) size = "large";
    }
    escaped = markup_escape(content == NULL ? "" : content->text_value);
    if (escaped == NULL) return NULL;
    markup = (char *)malloc(strlen(escaped) + strlen(size) + strlen(weight) +
                            (color == NULL ? 7 : strlen(color->text_value)) + 64);
    if (markup == NULL) { free(escaped); return NULL; }
    sprintf(markup, "<span foreground=\"%s\" size=\"%s\" weight=\"%s\">%s</span>",
            color == NULL ? "#000000" : color->text_value, size, weight, escaped);
    widget = state->api.label_new(NULL);
    if (widget != NULL) {
        state->api.label_set_markup(widget, markup);
        state->api.label_set_line_wrap(widget, 1);
        state->api.label_set_justify(widget, 2);
    }
    free(markup);
    free(escaped);
    return widget;
}

static LinuxControl *control_for_element(LinuxWindowState *state,
                                         ZSharpUIElement *element) {
    size_t index;
    for (index = 0; index < state->control_count; index++)
        if (state->controls[index].element == element)
            return &state->controls[index];
    return NULL;
}

static int update_label(LinuxWindowState *state, LinuxControl *control,
                        char *error, size_t error_size) {
    ZSharpUIElement *element = control->element;
    ZSharpUIProperty *content = property(element, "content");
    ZSharpUIProperty *color = property(element, "color");
    const char *size = "medium";
    const char *weight = "normal";
    char *escaped;
    char *markup;
    if (element->variant != NULL) {
        if (strcmp(element->variant, "title") == 0) {
            size = "xx-large";
            weight = "bold";
        } else if (strcmp(element->variant, "subtitle") == 0) {
            size = "x-large";
        } else if (strcmp(element->variant, "header") == 0) {
            size = "large";
            weight = "bold";
        } else if (strcmp(element->variant, "subheader") == 0) {
            size = "large";
        }
    }
    escaped = markup_escape(content == NULL ? "" : content->text_value);
    if (escaped == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    markup = (char *)malloc(strlen(escaped) + strlen(size) + strlen(weight) +
        (color == NULL ? 7 : strlen(color->text_value)) + 64);
    if (markup == NULL) {
        free(escaped);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    sprintf(markup,
            "<span foreground=\"%s\" size=\"%s\" weight=\"%s\">%s</span>",
            color == NULL ? "#000000" : color->text_value,
            size, weight, escaped);
    state->api.label_set_markup(control->widget, markup);
    free(markup);
    free(escaped);
    return 1;
}

static void position_window_from_design(LinuxWindowState *state) {
    ZSharpUIElement *design = design_element(&state->program->window);
    int width;
    int height;
    int x;
    int y;
    if (design == NULL || state->screen_width <= 0 ||
        state->screen_height <= 0) return;
    width = state->api.widget_get_allocated_width(state->window);
    height = state->api.widget_get_allocated_height(state->window);
    if (width <= 1) width = state->screen_width / 4;
    if (height <= 1) height = state->screen_height / 4;
    width = pixels(property(design, "width"), state->scale, width);
    height = pixels(property(design, "height"), state->scale, height);
    x = (state->screen_width - width) / 2 +
        pixels(property(design, "locationX"), state->scale, 0);
    y = (state->screen_height - height) / 2 -
        pixels(property(design, "locationY"), state->scale, 0);
    state->api.window_move(state->window, x, y);
}

static int set_window_property_ui(void *data, const char *path,
                                  ZSharpWindowValueType value_type,
                                  const char *text_value, ZSharpUIUnit unit,
                                  char *error, size_t error_size) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    ZSharpUIElement *element = NULL;
    ZSharpUIProperty *changed = NULL;
    LinuxControl *control;
    if (!zsharp_window_model_set(state->program, path, value_type, text_value,
                                 unit, &element, &changed,
                                 error, error_size)) return 0;
    if (element->type == ZUI_DESIGN) {
        if (strcmp(changed->name, "title") == 0) {
            state->api.window_set_title(state->window, changed->text_value);
        } else if (strcmp(changed->name, "background") == 0) {
            if (!apply_css(state, state->fixed, &state->background_css,
                           "#zsharp-window-background", changed->text_value,
                           error, error_size)) return 0;
        } else if (strcmp(changed->name, "scalable") == 0) {
            state->api.window_set_resizable(state->window,
                                             changed->status_value);
        } else if (strcmp(changed->name, "width") == 0 ||
                   strcmp(changed->name, "height") == 0) {
            int current_width =
                state->api.widget_get_allocated_width(state->window);
            int current_height =
                state->api.widget_get_allocated_height(state->window);
            ZSharpUIElement *design = design_element(&state->program->window);
            state->api.window_resize(state->window,
                pixels(property(design, "width"), state->scale, current_width),
                pixels(property(design, "height"), state->scale,
                       current_height));
            position_window_from_design(state);
        } else if (strcmp(changed->name, "locationX") == 0 ||
                   strcmp(changed->name, "locationY") == 0) {
            position_window_from_design(state);
        } else if (strcmp(changed->name, "icon") == 0 &&
                   state->api.window_set_icon_from_file != NULL) {
            char *icon = path_join(state->project_root, changed->text_value);
            int loaded = icon != NULL && state->api.window_set_icon_from_file(
                state->window, icon, NULL);
            free(icon);
            if (!loaded) {
                snprintf(error, error_size, "could not load window icon '%s'",
                         changed->text_value);
                return 0;
            }
        }
    } else {
        control = control_for_element(state, element);
        if (control == NULL) {
            snprintf(error, error_size, "UI element '%s' is not active",
                     element->name);
            return 0;
        }
        if (element->type == ZUI_TEXT &&
            (strcmp(changed->name, "content") == 0 ||
             strcmp(changed->name, "color") == 0)) {
            if (!update_label(state, control, error, error_size)) return 0;
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "text") == 0) {
            state->api.button_set_label(control->widget, changed->text_value);
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "buttonColor") == 0) {
            if (!apply_css(state, control->widget, &control->css_provider,
                           "button", changed->text_value,
                           error, error_size)) return 0;
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "textColor") == 0) {
            GdkRGBA color;
            if (state->api.widget_override_color != NULL &&
                parse_rgba(changed->text_value, &color))
                state->api.widget_override_color(control->widget, 0, &color);
        } else if (element->type == ZUI_TEXT_INPUT &&
                   strcmp(changed->name, "display") == 0 &&
                   !control->is_image_input) {
            if (control->is_multiline) {
                if (control->placeholder_active)
                    state->api.text_buffer_set_text(control->text_buffer,
                                                    changed->text_value, -1);
            } else {
                state->api.entry_set_placeholder_text(control->widget,
                                                       changed->text_value);
            }
        } else if (element->type == ZUI_IMAGE &&
                   strcmp(changed->name, "file") == 0) {
            int width = state->api.widget_get_allocated_width(control->widget);
            int height = state->api.widget_get_allocated_height(control->widget);
            char *image_path = path_join(state->project_root,
                                         changed->text_value);
            GdkPixbuf *pixbuf = image_path == NULL ? NULL :
                state->api.pixbuf_new_from_file_at_scale(
                    image_path, width, height, 1, NULL);
            free(image_path);
            if (pixbuf == NULL) {
                snprintf(error, error_size, "could not load image '%s'",
                         changed->text_value);
                return 0;
            }
            state->api.image_set_from_pixbuf(
                control->input_widget != NULL ? control->input_widget
                                               : control->widget,
                pixbuf);
            control->image_width = width;
            control->image_height = height;
            if (state->api.object_unref != NULL)
                state->api.object_unref(pixbuf);
        }
        if (changed->type == ZUI_PROPERTY_MEASUREMENT ||
            (element->type == ZUI_TEXT &&
             strcmp(changed->name, "content") == 0))
            layout_controls(state,
                state->api.widget_get_allocated_width(state->window),
                state->api.widget_get_allocated_height(state->window));
    }
    state->api.widget_queue_draw(state->window);
    while (state->api.events_pending()) state->api.main_iteration_do(0);
    return 1;
}

static int linux_window_cancelled(void *data) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    return __atomic_load_n(&state->closing, __ATOMIC_ACQUIRE) != 0;
}

static int apply_property_request(void *data) {
    LinuxPropertyRequest *request = (LinuxPropertyRequest *)data;
    LinuxWindowState *state = request->state;
    if (linux_window_cancelled(state)) {
        snprintf(request->error, sizeof(request->error),
                 "the window is closing");
        request->result = 0;
    } else {
        request->result = set_window_property_ui(
            state, request->path, request->value_type, request->text_value,
            request->unit, request->error, sizeof(request->error));
    }
    __atomic_store_n(&request->done, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
    return 0;
}

static int get_window_property_ui(LinuxWindowState *state, const char *path,
                                  ZSharpWindowReadType *value_type,
                                  char **text_value, char *error,
                                  size_t error_size) {
    ZSharpUIElement *element = NULL;
    ZSharpWindowInputField field;
    LinuxControl *control;
    ZSharpUIProperty *contents;
    size_t cursor_characters = 0;
    size_t cursor_bytes;
    size_t total_characters;
    size_t total_lines;
    size_t current_line;
    size_t current_column;
    if (!zsharp_window_model_resolve_input(
            state->program, path, &element, &field, error, error_size))
        return 0;
    control = control_for_element(state, element);
    if (control == NULL) {
        snprintf(error, error_size, "textInput '%s' is not active",
                 element->name);
        return 0;
    }
    if (!control->is_image_input) {
        if (control->is_multiline) {
            if (control->placeholder_active) {
                update_contents(state, control, "");
            } else {
                GtkTextIterStorage caret;
                void *mark;
                char *text = text_buffer_text(state, control);
                if (text != NULL) {
                    update_contents(state, control, text);
                    state->api.g_free(text);
                }
                memset(&caret, 0, sizeof(caret));
                mark = state->api.text_buffer_get_insert(control->text_buffer);
                if (mark != NULL) {
                    int offset;
                    state->api.text_buffer_get_iter_at_mark(
                        control->text_buffer, &caret, mark);
                    offset = state->api.text_iter_get_offset(&caret);
                    if (offset > 0) cursor_characters = (size_t)offset;
                }
            }
        } else {
            int offset;
            update_contents(state, control,
                            state->api.entry_get_text(control->widget));
            offset = state->api.editable_get_position(control->widget);
            if (offset > 0) cursor_characters = (size_t)offset;
        }
    }
    contents = property(element, "contents");
    if (contents == NULL || contents->text_value == NULL) {
        snprintf(error, error_size, "textInput '%s' has no contents",
                 element->name);
        return 0;
    }
    if (field == ZWINDOW_INPUT_CONTENTS) {
        *value_type = ZWINDOW_READ_TEXT;
        *text_value = zsharp_copy_text(contents->text_value,
                                       strlen(contents->text_value));
        if (*text_value == NULL) {
            snprintf(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    cursor_bytes = zsharp_window_utf8_byte_offset(contents->text_value,
                                                   cursor_characters);
    zsharp_window_text_metrics(contents->text_value, cursor_bytes,
                               &total_characters, &total_lines,
                               &current_line, &current_column);
    *value_type = ZWINDOW_READ_NUMBER;
    *text_value = zsharp_window_copy_size(
        field == ZWINDOW_INPUT_TOTAL_CHARACTERS ? total_characters :
        field == ZWINDOW_INPUT_CURRENT_COLUMN ? current_column :
        field == ZWINDOW_INPUT_TOTAL_LINES ? total_lines : current_line);
    if (*text_value == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    return 1;
}

static int apply_read_request(void *data) {
    LinuxReadRequest *request = (LinuxReadRequest *)data;
    LinuxWindowState *state = request->state;
    if (linux_window_cancelled(state)) {
        snprintf(request->error, sizeof(request->error),
                 "the window is closing");
        request->result = 0;
    } else {
        request->result = get_window_property_ui(
            state, request->path, &request->value_type, &request->text_value,
            request->error, sizeof(request->error));
    }
    __atomic_store_n(&request->done, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
    return 0;
}

static int runtime_set_window_property(void *data, const char *path,
                                       ZSharpWindowValueType value_type,
                                       const char *text_value,
                                       ZSharpUIUnit unit, char *error,
                                       size_t error_size) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    LinuxPropertyRequest *request;
    struct timespec pause = {0, 1000000L};
    if (linux_window_cancelled(state)) {
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    request = (LinuxPropertyRequest *)calloc(1, sizeof(*request));
    if (request == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    request->state = state;
    request->path = path;
    request->value_type = value_type;
    request->text_value = text_value;
    request->unit = unit;
    __atomic_add_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
    if (linux_window_cancelled(state)) {
        __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
        free(request);
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    if (state->api.idle_add(apply_property_request, request) == 0) {
        __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
        free(request);
        snprintf(error, error_size, "could not schedule a window update");
        return 0;
    }
    while (!__atomic_load_n(&request->done, __ATOMIC_ACQUIRE))
        nanosleep(&pause, NULL);
    if (!request->result) {
        snprintf(error, error_size, "%s",
                 request->error[0] == '\0' ? "the window update failed"
                                           : request->error);
    }
    {
        int result = request->result;
        free(request);
        return result;
    }
}

static int runtime_get_window_property(void *data, const char *path,
                                       ZSharpWindowReadType *value_type,
                                       char **text_value, char *error,
                                       size_t error_size) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    LinuxReadRequest *request;
    struct timespec pause = {0, 1000000L};
    if (linux_window_cancelled(state)) {
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    request = (LinuxReadRequest *)calloc(1, sizeof(*request));
    if (request == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    request->state = state;
    request->path = path;
    __atomic_add_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
    if (linux_window_cancelled(state)) {
        __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
        free(request);
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    if (state->api.idle_add(apply_read_request, request) == 0) {
        __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
        free(request);
        snprintf(error, error_size, "could not schedule a window read");
        return 0;
    }
    while (!__atomic_load_n(&request->done, __ATOMIC_ACQUIRE))
        nanosleep(&pause, NULL);
    if (!request->result) {
        snprintf(error, error_size, "%s",
                 request->error[0] == '\0' ? "the window read failed"
                                           : request->error);
    } else {
        *value_type = request->value_type;
        *text_value = request->text_value;
    }
    {
        int result = request->result;
        free(request);
        return result;
    }
}

static double monotonic_milliseconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec * 1000.0 +
           (double)value.tv_nsec / 1000000.0;
}

static int wait_with_window_events(void *data, const char *milliseconds,
                                   char *error, size_t error_size) {
    LinuxWindowState *state = (LinuxWindowState *)data;
    char *end;
    double duration = strtod(milliseconds, &end);
    double deadline;
    if (end == milliseconds || *end != '\0' || !isfinite(duration) ||
        duration < 0.0 || duration > 604800000.0) {
        snprintf(error, error_size,
                 "wait/delay must be between 0ms and 604800000ms");
        return 0;
    }
    deadline = monotonic_milliseconds() + duration;
    while (monotonic_milliseconds() < deadline) {
        struct timespec pause = {0, 1000000L};
        if (linux_window_cancelled(state)) {
            snprintf(error, error_size, "the window is closing");
            return 0;
        }
        nanosleep(&pause, NULL);
    }
    return !linux_window_cancelled(state);
}

static int create_controls(LinuxWindowState *state, int width, int height,
                           char *error, size_t error_size) {
    ZSharpWindow *window = &state->program->window;
    size_t index;
    state->controls = (LinuxControl *)calloc(window->element_count,
                                             sizeof(*state->controls));
    if (state->controls == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    for (index = 0; index < window->element_count; index++) {
        ZSharpUIElement *element = &window->elements[index];
        LinuxControl *control;
        GtkWidget *widget = NULL;
        int fallback_width, fallback_height, w, h;
        if (element->type == ZUI_DESIGN) continue;
        control = &state->controls[state->control_count];
        control->element = element;
        control->state = state;
        default_size(element, &fallback_width, &fallback_height);
        w = pixels(property(element, "width"), state->scale, fallback_width);
        h = pixels(property(element, "height"), state->scale, fallback_height);
        if (element->type == ZUI_TEXT) {
            widget = make_label(state, element);
        } else if (element->type == ZUI_BUTTON) {
            ZSharpUIProperty *text = property(element, "text");
            GdkRGBA color;
            widget = state->api.button_new_with_label(
                text == NULL ? "" : text->text_value);
            if (widget != NULL && state->api.widget_override_background_color != NULL &&
                property(element, "buttonColor") != NULL &&
                parse_rgba(property(element, "buttonColor")->text_value, &color))
                state->api.widget_override_background_color(widget, 0, &color);
            if (widget != NULL && state->api.widget_override_color != NULL &&
                property(element, "textColor") != NULL &&
                parse_rgba(property(element, "textColor")->text_value, &color))
                state->api.widget_override_color(widget, 0, &color);
        } else if (element->type == ZUI_IMAGE) {
            ZSharpUIProperty *file = property(element, "file");
            char *path = file == NULL ? NULL : path_join(state->project_root,
                                                         file->text_value);
            GdkPixbuf *pixbuf = NULL;
            if (path != NULL && state->api.pixbuf_new_from_file_at_scale != NULL)
                pixbuf = state->api.pixbuf_new_from_file_at_scale(path, w, h, 1,
                                                                  NULL);
            control->input_widget = state->api.image_new_from_pixbuf(pixbuf);
            if (property(element, "left") != NULL ||
                property(element, "right") != NULL) {
                widget = state->api.event_box_new();
                if (widget != NULL && control->input_widget != NULL)
                    state->api.container_add(widget, control->input_widget);
            } else {
                widget = control->input_widget;
                control->input_widget = NULL;
            }
            if (pixbuf != NULL) {
                control->image_width = w;
                control->image_height = h;
            }
            if (pixbuf != NULL && state->api.object_unref != NULL)
                state->api.object_unref(pixbuf);
            free(path);
        } else {
            ZSharpUIProperty *type = property(element, "type");
            ZSharpUIProperty *display = property(element, "display");
            control->is_image_input = type != NULL &&
                strcmp(type->text_value, "image") == 0;
            control->is_multiline = !control->is_image_input &&
                status_property(element, "multiline", 0);
            if (control->is_image_input)
                widget = state->api.file_chooser_button_new(
                    display == NULL ? "Choose an image" : display->text_value,
                    0);
            else if (control->is_multiline) {
                int wraps = status_property(element, "wrap", 1);
                control->input_widget = state->api.text_view_new();
                widget = state->api.scrolled_window_new(NULL, NULL);
                if (widget != NULL && control->input_widget != NULL) {
                    control->text_buffer = state->api.text_view_get_buffer(
                        control->input_widget);
                    state->api.text_view_set_wrap_mode(control->input_widget,
                                                       wraps ? 3 : 0);
                    state->api.scrolled_window_set_policy(widget,
                                                          wraps ? 2 : 1, 1);
                    state->api.container_add(widget, control->input_widget);
                    if (display != NULL && display->text_value != NULL &&
                        display->text_value[0] != '\0') {
                        control->placeholder_active = 1;
                        state->api.text_buffer_set_text(control->text_buffer,
                                                        display->text_value,
                                                        -1);
                    }
                }
            }
            else {
                widget = state->api.entry_new();
                if (widget != NULL && display != NULL)
                    state->api.entry_set_placeholder_text(widget,
                                                          display->text_value);
            }
        }
        if (widget == NULL) {
            snprintf(error, error_size, "could not create UI element '%s'",
                     element->name);
            return 0;
        }
        control->widget = widget;
        state->api.widget_set_size_request(widget, w, h);
        state->api.fixed_put(state->fixed, widget, 0, 0);
        if (!apply_element_zss(state, control, error, error_size)) return 0;
        if (element->type == ZUI_BUTTON) {
            state->api.signal_connect_data(widget, "clicked",
                (void *)button_clicked, control, NULL, 0);
            state->api.signal_connect_data(widget, "button-press-event",
                (void *)button_pressed, control, NULL, 0);
        } else if (element->type == ZUI_IMAGE &&
                   (property(element, "left") != NULL ||
                    property(element, "right") != NULL)) {
            state->api.signal_connect_data(widget, "button-press-event",
                (void *)image_pressed, control, NULL, 0);
        } else if (element->type == ZUI_TEXT_INPUT) {
            if (control->is_image_input) {
                state->api.signal_connect_data(widget, "file-set",
                    (void *)file_selected, control, NULL, 0);
            } else if (control->is_multiline) {
                state->api.signal_connect_data(control->text_buffer, "changed",
                    (void *)text_buffer_changed, control, NULL, 0);
                state->api.signal_connect_data(control->input_widget,
                    "focus-in-event", (void *)multiline_focus_in,
                    control, NULL, 0);
                state->api.signal_connect_data(control->input_widget,
                    "focus-out-event", (void *)multiline_focus_out,
                    control, NULL, 0);
            } else {
                state->api.signal_connect_data(widget, "changed",
                    (void *)entry_changed, control, NULL, 0);
            }
        }
        state->control_count++;
        if (element->type == ZUI_BUTTON &&
            property(element, "buttonColor") != NULL &&
            zsharp_paint_is_gradient_text(
                property(element, "buttonColor")->text_value)) {
            if (!apply_css(state, widget, &control->css_provider, "button",
                           property(element, "buttonColor")->text_value,
                           error, error_size)) return 0;
        }
    }
    layout_controls(state, width, height);
    return 1;
}

int zsharp_window_run(ZSharpProgram *program, const char *project_root,
                      ZSharpWindowCallback callback, void *user_data,
                      char *error, size_t error_size) {
    LinuxWindowState state;
    ZSharpUIElement *design;
    ZSharpUIProperty *value;
    int width = 480;
    int height = 270;
    int result = 1;
    const char *autoclose;
    memset(&state, 0, sizeof(state));
    if (getenv("ZSHARP_WINDOW_FORCE_FAILURE") != NULL) {
        snprintf(error, error_size, "forced window launch failure");
        return 0;
    }
    if (!gtk_api_load(&state.api, error, error_size)) return 0;
    if (!state.api.init_check(NULL, NULL)) {
        gtk_api_close(&state.api);
        snprintf(error, error_size,
                 "GTK 3 is installed, but no Linux desktop display is available");
        return 0;
    }
    state.program = program;
    state.project_root = project_root;
    state.callback = callback;
    state.callback_data = user_data;
    state.runtime.state = &state;
    state.runtime.set_property = runtime_set_window_property;
    state.runtime.get_property = runtime_get_window_property;
    state.runtime.wait = wait_with_window_events;
    state.runtime.is_cancelled = linux_window_cancelled;
    state.screen_width = width * 4;
    state.screen_height = height * 4;
    design = design_element(&program->window);
    if (design == NULL) {
        gtk_api_close(&state.api);
        snprintf(error, error_size, "window bytecode has no design");
        return 0;
    }
    state.window = state.api.window_new(0);
    if (state.window == NULL) {
        gtk_api_close(&state.api);
        snprintf(error, error_size, "could not create the GTK window");
        return 0;
    }
    state.scale = (double)state.api.widget_get_scale_factor(state.window);
    if (state.scale <= 0.0) state.scale = 1.0;
    if (state.api.screen_get_default != NULL &&
        state.api.screen_get_width != NULL &&
        state.api.screen_get_height != NULL) {
        void *screen = state.api.screen_get_default();
        if (screen != NULL) {
            int screen_width = state.api.screen_get_width(screen);
            int screen_height = state.api.screen_get_height(screen);
            state.screen_width = screen_width;
            state.screen_height = screen_height;
            if (screen_width > 0) width = screen_width / 4;
            if (screen_height > 0) height = screen_height / 4;
        }
    }
    value = property(design, "width");
    width = pixels(value, state.scale, width);
    value = property(design, "height");
    height = pixels(value, state.scale, height);
    state.layout_width = width;
    state.layout_height = height;
    value = property(design, "title");
    state.api.window_set_title(state.window,
        value == NULL ? program->window.name : value->text_value);
    state.api.window_set_default_size(state.window, width, height);
    value = property(design, "scalable");
    state.api.window_set_resizable(state.window,
                                   value == NULL || value->status_value);
    state.api.window_set_position(state.window, 1);
    position_window_from_design(&state);
    value = property(design, "icon");
    if (value != NULL && state.api.window_set_icon_from_file != NULL) {
        char *icon = path_join(project_root, value->text_value);
        if (icon != NULL) {
            state.api.window_set_icon_from_file(state.window, icon, NULL);
            free(icon);
        }
    }
    state.scrolled = state.api.scrolled_window_new(NULL, NULL);
    state.fixed = state.api.fixed_new();
    if (state.scrolled == NULL || state.fixed == NULL) {
        state.api.widget_destroy(state.window);
        gtk_api_close(&state.api);
        snprintf(error, error_size,
                 "could not create the responsive window layout");
        return 0;
    }
    state.api.scrolled_window_set_policy(state.scrolled, 2, 1);
    state.api.widget_set_name(state.fixed, "zsharp-window-background");
    state.api.container_add(state.scrolled, state.fixed);
    state.api.container_add(state.window, state.scrolled);
    value = property(design, "background");
    if (value != NULL && !apply_css(&state, state.fixed,
                                    &state.background_css,
                                    "#zsharp-window-background",
                                    value->text_value,
                                    error, error_size)) {
        state.api.widget_destroy(state.window);
        gtk_api_close(&state.api);
        return 0;
    }
    state.api.signal_connect_data(state.window, "destroy",
                                  (void *)window_destroyed, &state, NULL, 0);
    state.api.signal_connect_data(state.window, "size-allocate",
                                  (void *)window_allocated, &state, NULL, 0);
    if (!create_controls(&state, width, height, error, error_size)) {
        state.api.widget_destroy(state.window);
        while (state.api.events_pending()) state.api.main_iteration_do(0);
        free(state.controls);
        gtk_api_close(&state.api);
        return 0;
    }
    state.api.widget_show_all(state.window);
    if (getenv("ZSHARP_DISABLE_PROJECT_STARTS") == NULL &&
        !callback(user_data, ZSHARP_WINDOW_PROJECT_STARTS, &state.runtime,
                  error, error_size)) {
        __atomic_store_n(&state.closing, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&state.pending_requests, __ATOMIC_ACQUIRE) != 0)
            state.api.main_iteration_do(0);
        callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                 state.callback_error, sizeof(state.callback_error));
        state.api.widget_destroy(state.window);
        while (state.api.events_pending()) state.api.main_iteration_do(0);
        free(state.controls);
        gtk_api_close(&state.api);
        return 0;
    }
    autoclose = getenv("ZSHARP_WINDOW_AUTOCLOSE_MS");
    if (autoclose != NULL) {
        unsigned long milliseconds = strtoul(autoclose, NULL, 10);
        if (milliseconds > 0 && milliseconds <= 0xfffffffful)
            state.api.timeout_add((unsigned int)milliseconds,
                                  autoclose_window, &state);
    }
    state.api.main_loop();
    __atomic_store_n(&state.closing, 1, __ATOMIC_RELEASE);
    if (!callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                  error, error_size)) result = 0;
    if (state.background_css != NULL && state.api.object_unref != NULL)
        state.api.object_unref(state.background_css);
    {
        size_t index;
        for (index = 0; index < state.control_count; index++) {
            if (state.controls[index].css_provider != NULL &&
                state.api.object_unref != NULL)
                state.api.object_unref(state.controls[index].css_provider);
            if (state.controls[index].zss_provider != NULL &&
                state.api.object_unref != NULL)
                state.api.object_unref(state.controls[index].zss_provider);
        }
    }
    free(state.controls);
    gtk_api_close(&state.api);
    return result;
}

int zsharp_window_show_hub(const char *headline, const char *reason,
                           char *error, size_t error_size) {
    GtkApi api;
    GtkWidget *dialog;
    char *message;
    size_t a = strlen(headline == NULL ? "Z# Hub" : headline);
    size_t b = strlen(reason == NULL ? "" : reason);
    if (!gtk_api_load(&api, error, error_size)) return 0;
    if (!api.init_check(NULL, NULL)) {
        gtk_api_close(&api);
        snprintf(error, error_size, "no Linux desktop display is available");
        return 0;
    }
    message = (char *)malloc(a + b + 4);
    if (message == NULL) {
        gtk_api_close(&api);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    snprintf(message, a + b + 4, b == 0 ? "%s" : "%s\n\n%s",
             headline == NULL ? "Z# Hub" : headline,
             reason == NULL ? "" : reason);
    dialog = api.message_dialog_new(NULL, 0, 0, 2, "%s", message);
    free(message);
    if (dialog == NULL) {
        gtk_api_close(&api);
        snprintf(error, error_size, "could not create the Z# Hub window");
        return 0;
    }
    api.window_set_title(dialog, "Z# Hub");
    api.dialog_run(dialog);
    api.widget_destroy(dialog);
    gtk_api_close(&api);
    return 1;
}

#endif
