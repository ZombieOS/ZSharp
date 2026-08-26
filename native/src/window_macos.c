#include "window.h"

#include "paint.h"
#include "window_runtime.h"

#if defined(__APPLE__)

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *MacId;
typedef void *MacClass;
typedef void *MacSelector;
typedef void (*MacImp)(void);

typedef struct MacPoint { double x, y; } MacPoint;
typedef struct MacSize { double width, height; } MacSize;
typedef struct MacRect { MacPoint origin; MacSize size; } MacRect;

typedef struct MacApi {
    void *appkit;
    void *objc;
    void *dispatch;
    MacClass (*get_class)(const char *);
    MacSelector (*selector)(const char *);
    void *message;
    void *message_stret;
    MacClass (*allocate_class_pair)(MacClass, const char *, size_t);
    int (*add_method)(MacClass, MacSelector, MacImp, const char *);
    void (*register_class_pair)(MacClass);
    void (*rect_fill)(MacRect);
    void *(*dispatch_get_main_queue)(void);
    void (*dispatch_async_f)(void *, void *, void (*)(void *));
} MacApi;

struct MacWindowState;

typedef struct MacControl {
    MacId widget;
    MacId right_gesture;
    ZSharpUIElement *element;
    int is_image_input;
    struct MacWindowState *state;
} MacControl;

typedef struct MacWindowState {
    MacApi api;
    ZSharpProgram *program;
    const char *project_root;
    ZSharpWindowCallback callback;
    void *callback_data;
    MacId application;
    MacId window;
    MacId target;
    MacId scroll_view;
    MacId content;
    MacControl *controls;
    size_t control_count;
    double scale;
    double screen_width;
    double screen_height;
    double layout_width;
    double layout_height;
    char callback_error[512];
    ZSharpWindowRuntime runtime;
    volatile int closing;
    volatile int pending_requests;
} MacWindowState;

typedef struct MacPropertyRequest {
    MacWindowState *state;
    const char *path;
    ZSharpWindowValueType value_type;
    const char *text_value;
    ZSharpUIUnit unit;
    volatile int done;
    int result;
    char error[512];
} MacPropertyRequest;

static MacWindowState *active_state;
static void layout_controls(MacWindowState *state);

static MacSelector selector(MacApi *api, const char *name) {
    return api->selector(name);
}

static MacId send_id(MacApi *api, MacId object, const char *name) {
    return ((MacId (*)(MacId, MacSelector))api->message)(
        object, selector(api, name));
}

static MacId send_id_id(MacApi *api, MacId object, const char *name,
                        MacId value) {
    return ((MacId (*)(MacId, MacSelector, MacId))api->message)(
        object, selector(api, name), value);
}

static MacId send_id_text(MacApi *api, MacId object, const char *name,
                          const char *value) {
    return ((MacId (*)(MacId, MacSelector, const char *))api->message)(
        object, selector(api, name), value);
}

static void send_void(MacApi *api, MacId object, const char *name) {
    ((void (*)(MacId, MacSelector))api->message)(object, selector(api, name));
}

static void send_void_id(MacApi *api, MacId object, const char *name,
                         MacId value) {
    ((void (*)(MacId, MacSelector, MacId))api->message)(
        object, selector(api, name), value);
}

static void send_void_bool(MacApi *api, MacId object, const char *name,
                           int value) {
    ((void (*)(MacId, MacSelector, signed char))api->message)(
        object, selector(api, name), (signed char)(value != 0));
}

static void send_void_integer(MacApi *api, MacId object, const char *name,
                              unsigned long value) {
    ((void (*)(MacId, MacSelector, unsigned long))api->message)(
        object, selector(api, name), value);
}

static void send_void_rect(MacApi *api, MacId object, const char *name,
                           MacRect value) {
    ((void (*)(MacId, MacSelector, MacRect))api->message)(
        object, selector(api, name), value);
}

static void send_void_size(MacApi *api, MacId object, const char *name,
                           MacSize value) {
    ((void (*)(MacId, MacSelector, MacSize))api->message)(
        object, selector(api, name), value);
}

static void send_void_point(MacApi *api, MacId object, const char *name,
                            MacPoint value) {
    ((void (*)(MacId, MacSelector, MacPoint))api->message)(
        object, selector(api, name), value);
}

static double send_double(MacApi *api, MacId object, const char *name) {
    return ((double (*)(MacId, MacSelector))api->message)(
        object, selector(api, name));
}

static long send_long(MacApi *api, MacId object, const char *name) {
    return ((long (*)(MacId, MacSelector))api->message)(
        object, selector(api, name));
}

static const char *send_utf8(MacApi *api, MacId object) {
    return ((const char *(*)(MacId, MacSelector))api->message)(
        object, selector(api, "UTF8String"));
}

static MacRect send_rect(MacApi *api, MacId object, const char *name) {
    MacRect result;
#if defined(__x86_64__)
    ((void (*)(MacRect *, MacId, MacSelector))api->message_stret)(
        &result, object, selector(api, name));
#else
    result = ((MacRect (*)(MacId, MacSelector))api->message)(
        object, selector(api, name));
#endif
    return result;
}

static MacId ns_string(MacApi *api, const char *text) {
    return send_id_text(api, (MacId)api->get_class("NSString"),
                        "stringWithUTF8String:", text == NULL ? "" : text);
}

static int mac_api_load(MacApi *api, char *error, size_t error_size) {
    memset(api, 0, sizeof(*api));
    api->appkit = dlopen(
        "/System/Library/Frameworks/AppKit.framework/AppKit",
        RTLD_NOW | RTLD_GLOBAL);
    api->objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    api->dispatch = dlopen("/usr/lib/system/libdispatch.dylib",
                           RTLD_NOW | RTLD_GLOBAL);
    if (api->dispatch == NULL)
        api->dispatch = dlopen("/usr/lib/libSystem.B.dylib",
                               RTLD_NOW | RTLD_GLOBAL);
    if (api->appkit == NULL || api->objc == NULL || api->dispatch == NULL) {
        snprintf(error, error_size, "could not load the macOS AppKit runtime");
        return 0;
    }
    *(void **)(&api->get_class) = dlsym(api->objc, "objc_getClass");
    *(void **)(&api->selector) = dlsym(api->objc, "sel_registerName");
    api->message = dlsym(api->objc, "objc_msgSend");
    api->message_stret = dlsym(api->objc, "objc_msgSend_stret");
    *(void **)(&api->allocate_class_pair) =
        dlsym(api->objc, "objc_allocateClassPair");
    *(void **)(&api->add_method) = dlsym(api->objc, "class_addMethod");
    *(void **)(&api->register_class_pair) =
        dlsym(api->objc, "objc_registerClassPair");
    *(void **)(&api->rect_fill) = dlsym(api->appkit, "NSRectFill");
    *(void **)(&api->dispatch_get_main_queue) =
        dlsym(api->dispatch, "dispatch_get_main_queue");
    *(void **)(&api->dispatch_async_f) =
        dlsym(api->dispatch, "dispatch_async_f");
    if (api->get_class == NULL || api->selector == NULL ||
        api->message == NULL || api->allocate_class_pair == NULL ||
        api->add_method == NULL || api->register_class_pair == NULL ||
        api->rect_fill == NULL || api->dispatch_get_main_queue == NULL ||
        api->dispatch_async_f == NULL
#if defined(__x86_64__)
        || api->message_stret == NULL
#endif
        ) {
        snprintf(error, error_size,
                 "the macOS Objective-C runtime is missing required symbols");
        return 0;
    }
    return 1;
}

static void mac_api_close(MacApi *api) {
    if (api->dispatch != NULL) dlclose(api->dispatch);
    if (api->objc != NULL) dlclose(api->objc);
    if (api->appkit != NULL) dlclose(api->appkit);
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

static char *path_join(const char *root, const char *relative) {
    size_t a = strlen(root), b = strlen(relative);
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

static MacId ns_color(MacApi *api, const char *text, const char *fallback) {
    int digits[6];
    size_t index;
    double red, green, blue;
    if (text == NULL || strlen(text) != 7 || text[0] != '#') text = fallback;
    for (index = 0; index < 6; index++) {
        digits[index] = hex_digit(text[index + 1]);
        if (digits[index] < 0) return NULL;
    }
    red = (double)(digits[0] * 16 + digits[1]) / 255.0;
    green = (double)(digits[2] * 16 + digits[3]) / 255.0;
    blue = (double)(digits[4] * 16 + digits[5]) / 255.0;
    return ((MacId (*)(MacId, MacSelector, double, double, double, double))
        api->message)((MacId)api->get_class("NSColor"),
                      selector(api, "colorWithRed:green:blue:alpha:"),
                      red, green, blue, 1.0);
}

static double points(const ZSharpUIProperty *value, double scale,
                     double fallback) {
    double parsed;
    if (value == NULL || value->text_value == NULL) return fallback;
    parsed = strtod(value->text_value, NULL);
    return value->unit == ZUI_UNIT_ZU ? parsed * 4.0 : parsed / scale;
}

static double text_size(const char *variant) {
    if (variant == NULL || strcmp(variant, "paragraph") == 0) return 14.0;
    if (strcmp(variant, "subheader") == 0) return 18.0;
    if (strcmp(variant, "header") == 0) return 24.0;
    if (strcmp(variant, "subtitle") == 0) return 30.0;
    return 38.0;
}

static MacId system_font(MacApi *api, double size, int bold) {
    return ((MacId (*)(MacId, MacSelector, double))api->message)(
        (MacId)api->get_class("NSFont"),
        selector(api, bold ? "boldSystemFontOfSize:" : "systemFontOfSize:"),
        size);
}

static void update_input(MacWindowState *state, MacControl *control) {
    ZSharpUIProperty *contents = property(control->element, "contents");
    MacId value;
    const char *text;
    char *copy;
    if (contents == NULL || control->is_image_input) return;
    value = send_id(&state->api, control->widget, "stringValue");
    text = value == NULL ? "" : send_utf8(&state->api, value);
    if (text == NULL) text = "";
    copy = zsharp_copy_text(text, strlen(text));
    if (copy == NULL) return;
    free(contents->text_value);
    contents->text_value = copy;
}

static void sync_inputs(MacWindowState *state) {
    size_t index;
    for (index = 0; index < state->control_count; index++)
        if (state->controls[index].element->type == ZUI_TEXT_INPUT)
            update_input(state, &state->controls[index]);
}

static void show_alert(MacWindowState *state, const char *headline,
                       const char *reason) {
    MacApi *api = &state->api;
    MacId alert = send_id(api, (MacId)api->get_class("NSAlert"), "alloc");
    alert = send_id(api, alert, "init");
    send_void_id(api, alert, "setMessageText:", ns_string(api, headline));
    if (reason != NULL && reason[0] != '\0')
        send_void_id(api, alert, "setInformativeText:", ns_string(api, reason));
    send_long(api, alert, "runModal");
}

static MacControl *find_control(MacWindowState *state, MacId sender,
                                int gesture) {
    size_t index;
    for (index = 0; index < state->control_count; index++) {
        if ((!gesture && state->controls[index].widget == sender) ||
            (gesture && state->controls[index].right_gesture == sender))
            return &state->controls[index];
    }
    return NULL;
}

static void run_target(MacWindowState *state, MacControl *control,
                       const char *name) {
    ZSharpUIProperty *target;
    sync_inputs(state);
    target = property(control->element, name);
    if (target == NULL || target->text_value == NULL ||
        target->text_value[0] == '\0') return;
    state->callback_error[0] = '\0';
    if (!state->callback(state->callback_data, target->text_value,
                         &state->runtime,
                         state->callback_error,
                         sizeof(state->callback_error)))
        show_alert(state, "Z# callback error",
                   state->callback_error[0] == '\0'
                       ? "The Z# callback failed."
                       : state->callback_error);
}

static void choose_image(MacWindowState *state, MacControl *control) {
    MacApi *api = &state->api;
    MacId panel = send_id(api, (MacId)api->get_class("NSOpenPanel"),
                          "openPanel");
    if (panel != NULL && send_long(api, panel, "runModal") == 1) {
        MacId url = send_id(api, panel, "URL");
        MacId path = url == NULL ? NULL : send_id(api, url, "path");
        const char *text = path == NULL ? NULL : send_utf8(api, path);
        ZSharpUIProperty *contents = property(control->element, "contents");
        if (text != NULL && contents != NULL) {
            char *copy = zsharp_copy_text(text, strlen(text));
            if (copy != NULL) {
                free(contents->text_value);
                contents->text_value = copy;
                send_void_id(api, control->widget, "setTitle:",
                             ns_string(api, text));
            }
        }
    }
}

static void mac_action(MacId target, MacSelector command, MacId sender) {
    MacControl *control;
    (void)target;
    (void)command;
    if (active_state == NULL) return;
    control = find_control(active_state, sender, 0);
    if (control == NULL) return;
    if (control->is_image_input) choose_image(active_state, control);
    else run_target(active_state, control, "left");
}

static void mac_right_action(MacId target, MacSelector command, MacId sender) {
    MacControl *control;
    (void)target;
    (void)command;
    if (active_state == NULL) return;
    control = find_control(active_state, sender, 1);
    if (control != NULL) run_target(active_state, control, "right");
}

static int mac_window_cancelled(void *data) {
    MacWindowState *state = (MacWindowState *)data;
    return __atomic_load_n(&state->closing, __ATOMIC_ACQUIRE) != 0;
}

static void mac_finish_close(void *data) {
    MacWindowState *state = (MacWindowState *)data;
    if (__atomic_load_n(&state->pending_requests, __ATOMIC_ACQUIRE) != 0) {
        state->api.dispatch_async_f(state->api.dispatch_get_main_queue(),
                                    state, mac_finish_close);
        return;
    }
    send_void_id(&state->api, state->application, "stop:", NULL);
}

static void mac_window_closed(MacId target, MacSelector command,
                              MacId notification) {
    (void)target;
    (void)command;
    (void)notification;
    if (active_state != NULL) {
        __atomic_store_n(&active_state->closing, 1, __ATOMIC_RELEASE);
        active_state->api.dispatch_async_f(
            active_state->api.dispatch_get_main_queue(), active_state,
            mac_finish_close);
    }
}

static void mac_window_resized(MacId target, MacSelector command,
                               MacId notification) {
    (void)target;
    (void)command;
    (void)notification;
    if (active_state != NULL) layout_controls(active_state);
}

static void mac_timer_fired(MacId target, MacSelector command, MacId timer) {
    (void)target;
    (void)command;
    (void)timer;
    if (active_state != NULL) {
        send_void(&active_state->api, active_state->window, "close");
    }
}

static void mac_draw_background(MacId view, MacSelector command,
                                MacRect dirty) {
    MacWindowState *state = active_state;
    ZSharpUIElement *design;
    ZSharpUIProperty *background;
    ZSharpPaint paint;
    MacId colors;
    MacId gradient;
    MacRect bounds;
    size_t index;
    char ignored_error[128] = {0};
    (void)command;
    (void)dirty;
    if (state == NULL) return;
    design = design_element(&state->program->window);
    background = design == NULL ? NULL : property(design, "background");
    memset(&paint, 0, sizeof(paint));
    if (!zsharp_paint_parse(background == NULL ? "#FFFFFF" :
                            background->text_value, &paint,
                            ignored_error, sizeof(ignored_error))) return;
    bounds = send_rect(&state->api, view, "bounds");
    if (paint.kind == ZSHARP_PAINT_SOLID) {
        char color_text[8];
        sprintf(color_text, "#%06X", (unsigned)paint.colors[0]);
        send_void(&state->api,
                  ns_color(&state->api, color_text, "#FFFFFF"), "setFill");
        state->api.rect_fill(bounds);
        zsharp_paint_free(&paint);
        return;
    }
    colors = send_id(&state->api,
        (MacId)state->api.get_class("NSMutableArray"), "array");
    for (index = 0; index < paint.color_count; index++) {
        char color_text[8];
        sprintf(color_text, "#%06X", (unsigned)paint.colors[index]);
        send_void_id(&state->api, colors, "addObject:",
                     ns_color(&state->api, color_text, "#FFFFFF"));
    }
    gradient = send_id(&state->api,
        (MacId)state->api.get_class("NSGradient"), "alloc");
    gradient = send_id_id(&state->api, gradient, "initWithColors:", colors);
    if (gradient != NULL) {
        if (paint.kind == ZSHARP_PAINT_LINEAR) {
            ((void (*)(MacId, MacSelector, MacRect, double))state->api.message)(
                gradient, selector(&state->api, "drawInRect:angle:"),
                bounds, 90.0 - paint.degrees);
        } else {
            MacPoint center;
            double radians = paint.degrees * 3.14159265358979323846 / 180.0;
            center.x = sin(radians) * 0.2;
            center.y = cos(radians) * 0.2;
            ((void (*)(MacId, MacSelector, MacRect, MacPoint))
                state->api.message)(
                    gradient,
                    selector(&state->api,
                             "drawInRect:relativeCenterPosition:"),
                    bounds, center);
        }
    }
    zsharp_paint_free(&paint);
}

static signed char mac_view_is_flipped(MacId view, MacSelector command) {
    (void)view;
    (void)command;
    return 1;
}

static MacId make_background_view(MacWindowState *state, MacRect frame) {
    MacApi *api = &state->api;
    MacClass view_class = api->get_class("ZombieOSZSharpGradientView");
    MacId view;
    if (view_class == NULL) {
        view_class = api->allocate_class_pair(api->get_class("NSView"),
                                              "ZombieOSZSharpGradientView", 0);
        if (view_class == NULL) return NULL;
        api->add_method(view_class, selector(api, "drawRect:"),
                        (MacImp)mac_draw_background,
                        "v@:{_NSRect={_NSPoint=dd}{_NSSize=dd}}");
        api->add_method(view_class, selector(api, "isFlipped"),
                        (MacImp)mac_view_is_flipped, "c@:");
        api->register_class_pair(view_class);
    }
    view = send_id(api, (MacId)view_class, "alloc");
    view = ((MacId (*)(MacId, MacSelector, MacRect))api->message)(
        view, selector(api, "initWithFrame:"), frame);
    if (view != NULL) send_void_integer(api, view, "setAutoresizingMask:", 2);
    return view;
}

static MacId make_target(MacWindowState *state) {
    MacApi *api = &state->api;
    MacClass target_class = api->get_class("ZombieOSZSharpWindowTarget");
    if (target_class == NULL) {
        target_class = api->allocate_class_pair(api->get_class("NSObject"),
                                                "ZombieOSZSharpWindowTarget", 0);
        if (target_class == NULL) return NULL;
        api->add_method(target_class, selector(api, "zsharpAction:"),
                        (MacImp)mac_action, "v@:@");
        api->add_method(target_class, selector(api, "zsharpRightAction:"),
                        (MacImp)mac_right_action, "v@:@");
        api->add_method(target_class, selector(api, "windowWillClose:"),
                        (MacImp)mac_window_closed, "v@:@");
        api->add_method(target_class, selector(api, "windowDidResize:"),
                        (MacImp)mac_window_resized, "v@:@");
        api->add_method(target_class, selector(api, "zsharpTimer:"),
                        (MacImp)mac_timer_fired, "v@:@");
        api->register_class_pair(target_class);
    }
    return send_id(api, send_id(api, (MacId)target_class, "alloc"), "init");
}

static MacControl *control_for_element(MacWindowState *state,
                                       ZSharpUIElement *element) {
    size_t index;
    for (index = 0; index < state->control_count; index++)
        if (state->controls[index].element == element)
            return &state->controls[index];
    return NULL;
}

static size_t wrapped_line_count(const char *text, size_t capacity) {
    size_t lines = 1;
    size_t column = 0;
    const unsigned char *cursor = (const unsigned char *)(text == NULL ? "" : text);
    if (capacity == 0) capacity = 1;
    while (*cursor != '\0') {
        if (*cursor == '\n') {
            lines++;
            column = 0;
            cursor++;
            continue;
        }
        if (column == capacity) {
            lines++;
            column = 0;
        }
        if ((*cursor & 0xc0u) != 0x80u) column++;
        cursor++;
    }
    return lines;
}

static void layout_controls(MacWindowState *state) {
    MacId clip = send_id(&state->api, state->scroll_view, "contentView");
    MacRect viewport = send_rect(&state->api, clip, "bounds");
    size_t index;
    double content_bottom = viewport.size.height;
    double responsive_scale = state->layout_width > 0.0
        ? viewport.size.width / state->layout_width : 1.0;
    if (responsive_scale > 1.0) responsive_scale = 1.0;
    if (responsive_scale < 0.05) responsive_scale = 0.05;
    for (index = 0; index < state->control_count; index++) {
        MacControl *control = &state->controls[index];
        ZSharpUIElement *element = control->element;
        MacRect frame;
        double fallback_width = element->type == ZUI_TEXT ? 400.0 : 200.0;
        double fallback_height = element->type == ZUI_TEXT ? 48.0 : 32.0;
        double base_width = points(property(element, "width"), state->scale,
                                   fallback_width);
        double base_height = points(property(element, "height"), state->scale,
                                    fallback_height);
        frame.size.width = base_width * responsive_scale;
        frame.size.height = base_height * responsive_scale;
        if (element->type != ZUI_TEXT) {
            if (frame.size.width < 32.0) frame.size.width = 32.0;
            if (frame.size.height < 16.0) frame.size.height = 16.0;
        } else {
            if (frame.size.width < 24.0) frame.size.width = 24.0;
            if (property(element, "height") == NULL) {
                ZSharpUIProperty *content = property(element, "content");
                double font_size = text_size(element->variant);
                size_t capacity = (size_t)(frame.size.width /
                                                (font_size * 0.55));
                size_t lines = wrapped_line_count(
                    content == NULL ? "" : content->text_value, capacity);
                frame.size.height = (font_size * 1.3) * (double)lines + 6.0;
                if (frame.size.height < base_height * responsive_scale)
                    frame.size.height = base_height * responsive_scale;
            }
        }
        frame.origin.x = (state->layout_width / 2.0 +
            points(property(element, "locationX"), state->scale, 0.0) -
            base_width / 2.0) * responsive_scale;
        frame.origin.y = (state->layout_height / 2.0 -
            points(property(element, "locationY"), state->scale, 0.0) -
            base_height / 2.0) * responsive_scale;
        send_void_rect(&state->api, control->widget, "setFrame:", frame);
        if (frame.origin.y + frame.size.height > content_bottom)
            content_bottom = frame.origin.y + frame.size.height;
    }
    {
        MacSize document_size;
        document_size.width = viewport.size.width;
        document_size.height = content_bottom + 16.0;
        send_void_size(&state->api, state->content, "setFrameSize:",
                       document_size);
    }
}

static void position_window_from_design(MacWindowState *state) {
    ZSharpUIElement *design = design_element(&state->program->window);
    MacId screen;
    MacRect visible;
    MacRect frame;
    MacPoint origin;
    if (design == NULL) return;
    screen = send_id(&state->api, (MacId)state->api.get_class("NSScreen"),
                     "mainScreen");
    visible = send_rect(&state->api, screen, "visibleFrame");
    frame = send_rect(&state->api, state->window, "frame");
    origin.x = visible.origin.x + (visible.size.width - frame.size.width) / 2.0 +
        points(property(design, "locationX"), state->scale, 0.0);
    origin.y = visible.origin.y +
        (visible.size.height - frame.size.height) / 2.0 +
        points(property(design, "locationY"), state->scale, 0.0);
    send_void_point(&state->api, state->window, "setFrameOrigin:", origin);
}

static int set_window_property_ui(void *data, const char *path,
                                  ZSharpWindowValueType value_type,
                                  const char *text_value, ZSharpUIUnit unit,
                                  char *error, size_t error_size) {
    MacWindowState *state = (MacWindowState *)data;
    MacApi *api = &state->api;
    ZSharpUIElement *element = NULL;
    ZSharpUIProperty *changed = NULL;
    MacControl *control;
    if (!zsharp_window_model_set(state->program, path, value_type, text_value,
                                 unit, &element, &changed,
                                 error, error_size)) return 0;
    if (element->type == ZUI_DESIGN) {
        if (strcmp(changed->name, "title") == 0) {
            send_void_id(api, state->window, "setTitle:",
                         ns_string(api, changed->text_value));
        } else if (strcmp(changed->name, "background") == 0) {
            send_void_bool(api, state->content, "setNeedsDisplay:", 1);
        } else if (strcmp(changed->name, "scalable") == 0) {
            unsigned long style = (unsigned long)send_long(
                api, state->window, "styleMask");
            if (changed->status_value) style |= 8u;
            else style &= ~8u;
            send_void_integer(api, state->window, "setStyleMask:", style);
        } else if (strcmp(changed->name, "width") == 0 ||
                   strcmp(changed->name, "height") == 0) {
            ZSharpUIElement *design = design_element(&state->program->window);
            MacRect current = send_rect(
                api, send_id(api, state->scroll_view, "contentView"),
                "bounds");
            MacSize size;
            size.width = points(property(design, "width"), state->scale,
                                current.size.width);
            size.height = points(property(design, "height"), state->scale,
                                 current.size.height);
            send_void_size(api, state->window, "setContentSize:", size);
            layout_controls(state);
            position_window_from_design(state);
        } else if (strcmp(changed->name, "locationX") == 0 ||
                   strcmp(changed->name, "locationY") == 0) {
            position_window_from_design(state);
        } else if (strcmp(changed->name, "icon") == 0) {
            char *icon_path = path_join(state->project_root,
                                        changed->text_value);
            MacId image = icon_path == NULL ? NULL : send_id_id(
                api, send_id(api, (MacId)api->get_class("NSImage"), "alloc"),
                "initWithContentsOfFile:", ns_string(api, icon_path));
            free(icon_path);
            if (image == NULL) {
                snprintf(error, error_size, "could not load window icon '%s'",
                         changed->text_value);
                return 0;
            }
            send_void_id(api, state->application, "setApplicationIconImage:",
                         image);
        }
    } else {
        control = control_for_element(state, element);
        if (control == NULL) {
            snprintf(error, error_size, "UI element '%s' is not active",
                     element->name);
            return 0;
        }
        if (element->type == ZUI_TEXT &&
            strcmp(changed->name, "content") == 0) {
            send_void_id(api, control->widget, "setStringValue:",
                         ns_string(api, changed->text_value));
            layout_controls(state);
        } else if (element->type == ZUI_TEXT &&
                   strcmp(changed->name, "color") == 0) {
            send_void_id(api, control->widget, "setTextColor:",
                         ns_color(api, changed->text_value, "#000000"));
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "text") == 0) {
            send_void_id(api, control->widget, "setTitle:",
                         ns_string(api, changed->text_value));
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "buttonColor") == 0) {
            send_void_id(api, control->widget, "setBezelColor:",
                         ns_color(api, changed->text_value, "#F0F0F0"));
        } else if (element->type == ZUI_BUTTON &&
                   strcmp(changed->name, "textColor") == 0) {
            send_void_id(api, control->widget, "setContentTintColor:",
                         ns_color(api, changed->text_value, "#000000"));
        } else if (element->type == ZUI_TEXT_INPUT &&
                   strcmp(changed->name, "display") == 0) {
            send_void_id(api, control->widget,
                         control->is_image_input ? "setTitle:"
                                                 : "setPlaceholderString:",
                         ns_string(api, changed->text_value));
        } else if (element->type == ZUI_IMAGE &&
                   strcmp(changed->name, "file") == 0) {
            char *image_path = path_join(state->project_root,
                                         changed->text_value);
            MacId image = image_path == NULL ? NULL : send_id_id(
                api, send_id(api, (MacId)api->get_class("NSImage"), "alloc"),
                "initWithContentsOfFile:", ns_string(api, image_path));
            free(image_path);
            if (image == NULL) {
                snprintf(error, error_size, "could not load image '%s'",
                         changed->text_value);
                return 0;
            }
            send_void_id(api, control->widget, "setImage:", image);
        }
        if (changed->type == ZUI_PROPERTY_MEASUREMENT)
            layout_controls(state);
    }
    send_void(api, state->window, "displayIfNeeded");
    return 1;
}

static void apply_property_request(void *data) {
    MacPropertyRequest *request = (MacPropertyRequest *)data;
    MacWindowState *state = request->state;
    if (mac_window_cancelled(state)) {
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
}

static int runtime_set_window_property(void *data, const char *path,
                                       ZSharpWindowValueType value_type,
                                       const char *text_value,
                                       ZSharpUIUnit unit, char *error,
                                       size_t error_size) {
    MacWindowState *state = (MacWindowState *)data;
    MacPropertyRequest *request;
    struct timespec pause = {0, 1000000L};
    if (mac_window_cancelled(state)) {
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    request = (MacPropertyRequest *)calloc(1, sizeof(*request));
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
    if (mac_window_cancelled(state)) {
        __atomic_sub_fetch(&state->pending_requests, 1, __ATOMIC_ACQ_REL);
        free(request);
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    state->api.dispatch_async_f(state->api.dispatch_get_main_queue(), request,
                                apply_property_request);
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

static double monotonic_milliseconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec * 1000.0 +
           (double)value.tv_nsec / 1000000.0;
}

static void drain_pending_requests(MacWindowState *state) {
    MacApi *api = &state->api;
    MacId run_loop = send_id(api, (MacId)api->get_class("NSRunLoop"),
                             "currentRunLoop");
    while (__atomic_load_n(&state->pending_requests, __ATOMIC_ACQUIRE) != 0) {
        MacId date = ((MacId (*)(MacId, MacSelector, double))api->message)(
            (MacId)api->get_class("NSDate"),
            selector(api, "dateWithTimeIntervalSinceNow:"), 0.001);
        send_void_id(api, run_loop, "runUntilDate:", date);
    }
}

static int wait_with_window_events(void *data, const char *milliseconds,
                                   char *error, size_t error_size) {
    MacWindowState *state = (MacWindowState *)data;
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
        if (mac_window_cancelled(state)) {
            snprintf(error, error_size, "the window is closing");
            return 0;
        }
        nanosleep(&pause, NULL);
    }
    return !mac_window_cancelled(state);
}

static int create_controls(MacWindowState *state, MacId content,
                           double width, double height, char *error,
                           size_t error_size) {
    ZSharpWindow *window = &state->program->window;
    MacApi *api = &state->api;
    size_t index;
    state->controls = (MacControl *)calloc(window->element_count,
                                           sizeof(*state->controls));
    if (state->controls == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    for (index = 0; index < window->element_count; index++) {
        ZSharpUIElement *element = &window->elements[index];
        MacControl *control;
        MacRect frame;
        double fallback_width = element->type == ZUI_TEXT ? 400.0 : 200.0;
        double fallback_height = element->type == ZUI_TEXT ? 48.0 : 32.0;
        double w, h, x, y;
        MacId widget = NULL;
        if (element->type == ZUI_DESIGN) continue;
        w = points(property(element, "width"), state->scale, fallback_width);
        h = points(property(element, "height"), state->scale, fallback_height);
        x = width / 2.0 + points(property(element, "locationX"),
                                 state->scale, 0.0) - w / 2.0;
        y = height / 2.0 + points(property(element, "locationY"),
                                  state->scale, 0.0) - h / 2.0;
        frame.origin.x = x;
        frame.origin.y = y;
        frame.size.width = w;
        frame.size.height = h;
        control = &state->controls[state->control_count];
        control->element = element;
        control->state = state;
        if (element->type == ZUI_TEXT) {
            ZSharpUIProperty *value = property(element, "content");
            widget = send_id_id(api, (MacId)api->get_class("NSTextField"),
                                "labelWithString:",
                                ns_string(api, value == NULL ? "" : value->text_value));
            send_void_id(api, widget, "setTextColor:", ns_color(api,
                property(element, "color") == NULL ? NULL
                    : property(element, "color")->text_value, "#000000"));
            send_void_id(api, widget, "setFont:", system_font(api,
                text_size(element->variant), element->variant != NULL &&
                (strcmp(element->variant, "title") == 0 ||
                 strcmp(element->variant, "header") == 0)));
            send_void_integer(api, widget, "setAlignment:", 1);
            {
                MacId cell = send_id(api, widget, "cell");
                if (cell != NULL) {
                    send_void_bool(api, cell, "setWraps:", 1);
                    send_void_bool(api, cell, "setScrollable:", 0);
                    send_void_integer(api, cell, "setLineBreakMode:", 0);
                }
            }
        } else if (element->type == ZUI_BUTTON) {
            ZSharpUIProperty *value = property(element, "text");
            widget = ((MacId (*)(MacId, MacSelector, MacId, MacId, MacSelector))
                api->message)((MacId)api->get_class("NSButton"),
                    selector(api, "buttonWithTitle:target:action:"),
                    ns_string(api, value == NULL ? "" : value->text_value),
                    state->target, selector(api, "zsharpAction:"));
            send_void_id(api, widget, "setBezelColor:", ns_color(api,
                property(element, "buttonColor") == NULL ? NULL
                    : property(element, "buttonColor")->text_value, "#F0F0F0"));
            if (property(element, "textColor") != NULL)
                send_void_id(api, widget, "setContentTintColor:", ns_color(api,
                    property(element, "textColor")->text_value, "#000000"));
        } else if (element->type == ZUI_IMAGE) {
            ZSharpUIProperty *value = property(element, "file");
            char *path = value == NULL ? NULL : path_join(state->project_root,
                                                          value->text_value);
            MacId image = path == NULL ? NULL
                : send_id_id(api, send_id(api,
                      (MacId)api->get_class("NSImage"), "alloc"),
                      "initWithContentsOfFile:", ns_string(api, path));
            widget = ((MacId (*)(MacId, MacSelector, MacRect))api->message)(
                send_id(api, (MacId)api->get_class("NSImageView"), "alloc"),
                selector(api, "initWithFrame:"), frame);
            if (image != NULL) send_void_id(api, widget, "setImage:", image);
            send_void_integer(api, widget, "setImageScaling:", 3);
            free(path);
        } else {
            ZSharpUIProperty *type = property(element, "type");
            ZSharpUIProperty *display = property(element, "display");
            control->is_image_input = type != NULL &&
                strcmp(type->text_value, "image") == 0;
            if (control->is_image_input) {
                widget = ((MacId (*)(MacId, MacSelector, MacId, MacId,
                                     MacSelector))api->message)(
                    (MacId)api->get_class("NSButton"),
                    selector(api, "buttonWithTitle:target:action:"),
                    ns_string(api, display == NULL ? "Choose an image"
                                                   : display->text_value),
                    state->target, selector(api, "zsharpAction:"));
            } else {
                widget = send_id_id(api, (MacId)api->get_class("NSTextField"),
                                    "textFieldWithString:", ns_string(api, ""));
                if (display != NULL)
                    send_void_id(api, widget, "setPlaceholderString:",
                                 ns_string(api, display->text_value));
            }
        }
        if (widget == NULL) {
            snprintf(error, error_size, "could not create UI element '%s'",
                     element->name);
            return 0;
        }
        control->widget = widget;
        send_void_rect(api, widget, "setFrame:", frame);
        send_void_integer(api, widget, "setAutoresizingMask:", 45);
        send_void_id(api, content, "addSubview:", widget);
        if (element->type == ZUI_BUTTON) {
            MacId gesture = send_id(api,
                (MacId)api->get_class("NSClickGestureRecognizer"), "alloc");
            gesture = ((MacId (*)(MacId, MacSelector, MacId, MacSelector))
                api->message)(gesture, selector(api, "initWithTarget:action:"),
                              state->target,
                              selector(api, "zsharpRightAction:"));
            if (gesture != NULL) {
                send_void_integer(api, gesture, "setButtonMask:", 2);
                send_void_id(api, widget, "addGestureRecognizer:", gesture);
                control->right_gesture = gesture;
            }
        }
        state->control_count++;
    }
    return 1;
}

int zsharp_window_run(ZSharpProgram *program, const char *project_root,
                      ZSharpWindowCallback callback, void *user_data,
                      char *error, size_t error_size) {
    MacWindowState state;
    MacApi *api = &state.api;
    ZSharpUIElement *design;
    ZSharpUIProperty *value;
    MacId pool;
    MacId screen;
    MacId content;
    MacRect screen_frame;
    MacRect window_frame;
    unsigned long style = 1u | 2u | 4u | 8u;
    const char *autoclose;
    int result = 1;
    memset(&state, 0, sizeof(state));
    if (getenv("ZSHARP_WINDOW_FORCE_FAILURE") != NULL) {
        snprintf(error, error_size, "forced window launch failure");
        return 0;
    }
    if (!mac_api_load(api, error, error_size)) return 0;
    pool = send_id(api, send_id(api,
        (MacId)api->get_class("NSAutoreleasePool"), "alloc"), "init");
    state.application = send_id(api, (MacId)api->get_class("NSApplication"),
                                "sharedApplication");
    ((void (*)(MacId, MacSelector, long))api->message)(
        state.application, selector(api, "setActivationPolicy:"), 0L);
    state.program = program;
    state.project_root = project_root;
    state.callback = callback;
    state.callback_data = user_data;
    state.runtime.state = &state;
    state.runtime.set_property = runtime_set_window_property;
    state.runtime.wait = wait_with_window_events;
    state.runtime.is_cancelled = mac_window_cancelled;
    design = design_element(&program->window);
    if (design == NULL) {
        snprintf(error, error_size, "window bytecode has no design");
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    screen = send_id(api, (MacId)api->get_class("NSScreen"), "mainScreen");
    screen_frame = send_rect(api, screen, "visibleFrame");
    state.scale = send_double(api, screen, "backingScaleFactor");
    if (state.scale <= 0.0) state.scale = 1.0;
    state.screen_width = screen_frame.size.width;
    state.screen_height = screen_frame.size.height;
    window_frame.origin.x = 0.0;
    window_frame.origin.y = 0.0;
    window_frame.size.width = points(property(design, "width"), state.scale,
                                     screen_frame.size.width / 4.0);
    window_frame.size.height = points(property(design, "height"), state.scale,
                                      screen_frame.size.height / 4.0);
    state.layout_width = window_frame.size.width;
    state.layout_height = window_frame.size.height;
    value = property(design, "scalable");
    if (value != NULL && !value->status_value) style &= ~8u;
    state.window = ((MacId (*)(MacId, MacSelector, MacRect, unsigned long,
                               unsigned long, signed char))api->message)(
        send_id(api, (MacId)api->get_class("NSWindow"), "alloc"),
        selector(api, "initWithContentRect:styleMask:backing:defer:"),
        window_frame, style, 2u, 0);
    if (state.window == NULL) {
        snprintf(error, error_size, "could not create the AppKit window");
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    state.target = make_target(&state);
    if (state.target == NULL) {
        snprintf(error, error_size, "could not create the AppKit event target");
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    value = property(design, "title");
    send_void_id(api, state.window, "setTitle:", ns_string(api,
        value == NULL ? program->window.name : value->text_value));
    send_void_id(api, state.window, "setDelegate:", state.target);
    send_void_bool(api, state.window, "setReleasedWhenClosed:", 0);
    content = send_id(api, state.window, "contentView");
    active_state = &state;
    state.scroll_view = ((MacId (*)(MacId, MacSelector, MacRect))api->message)(
        send_id(api, (MacId)api->get_class("NSScrollView"), "alloc"),
        selector(api, "initWithFrame:"), send_rect(api, content, "bounds"));
    if (state.scroll_view != NULL) {
        send_void_bool(api, state.scroll_view, "setHasVerticalScroller:", 1);
        send_void_bool(api, state.scroll_view, "setHasHorizontalScroller:", 0);
        send_void_bool(api, state.scroll_view, "setAutohidesScrollers:", 1);
        send_void_integer(api, state.scroll_view, "setAutoresizingMask:", 18);
    }
    content = make_background_view(&state, send_rect(api, content, "bounds"));
    if (state.scroll_view == NULL || content == NULL) {
        active_state = NULL;
        snprintf(error, error_size,
                 "could not create the AppKit gradient view");
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    state.content = content;
    send_void_id(api, state.scroll_view, "setDocumentView:", content);
    send_void_id(api, state.window, "setContentView:", state.scroll_view);
    if (!create_controls(&state, content, window_frame.size.width,
                         window_frame.size.height, error, error_size)) {
        active_state = NULL;
        free(state.controls);
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    layout_controls(&state);
    value = property(design, "icon");
    if (value != NULL) {
        char *path = path_join(project_root, value->text_value);
        if (path != NULL) {
            MacId image = send_id_id(api, send_id(api,
                (MacId)api->get_class("NSImage"), "alloc"),
                "initWithContentsOfFile:", ns_string(api, path));
            if (image != NULL)
                send_void_id(api, state.application, "setApplicationIconImage:",
                             image);
            free(path);
        }
    }
    send_void(api, state.window, "center");
    position_window_from_design(&state);
    send_void_id(api, state.window, "makeKeyAndOrderFront:", NULL);
    send_void_bool(api, state.application, "activateIgnoringOtherApps:", 1);
    if (getenv("ZSHARP_DISABLE_PROJECT_STARTS") == NULL &&
        !callback(user_data, ZSHARP_WINDOW_PROJECT_STARTS, &state.runtime,
                  error, error_size)) {
        __atomic_store_n(&state.closing, 1, __ATOMIC_RELEASE);
        drain_pending_requests(&state);
        callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                 state.callback_error, sizeof(state.callback_error));
        active_state = NULL;
        free(state.controls);
        send_void(api, pool, "drain");
        mac_api_close(api);
        return 0;
    }
    autoclose = getenv("ZSHARP_WINDOW_AUTOCLOSE_MS");
    if (autoclose != NULL) {
        double seconds = strtod(autoclose, NULL) / 1000.0;
        if (seconds > 0.0)
            ((MacId (*)(MacId, MacSelector, double, MacId, MacSelector, MacId,
                        signed char))api->message)(
                (MacId)api->get_class("NSTimer"),
                selector(api, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
                seconds, state.target, selector(api, "zsharpTimer:"), NULL, 0);
    }
    send_void(api, state.application, "run");
    __atomic_store_n(&state.closing, 1, __ATOMIC_RELEASE);
    drain_pending_requests(&state);
    if (!callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                  error, error_size)) result = 0;
    active_state = NULL;
    free(state.controls);
    send_void(api, pool, "drain");
    mac_api_close(api);
    return result;
}

int zsharp_window_show_hub(const char *headline, const char *reason,
                           char *error, size_t error_size) {
    MacApi api;
    MacId pool;
    MacId application;
    MacId alert;
    if (!mac_api_load(&api, error, error_size)) return 0;
    pool = send_id(&api, send_id(&api,
        (MacId)api.get_class("NSAutoreleasePool"), "alloc"), "init");
    application = send_id(&api, (MacId)api.get_class("NSApplication"),
                          "sharedApplication");
    send_void_bool(&api, application, "activateIgnoringOtherApps:", 1);
    alert = send_id(&api, (MacId)api.get_class("NSAlert"), "alloc");
    alert = send_id(&api, alert, "init");
    send_void_id(&api, alert, "setMessageText:", ns_string(&api,
        headline == NULL ? "Z# Hub" : headline));
    if (reason != NULL && reason[0] != '\0')
        send_void_id(&api, alert, "setInformativeText:", ns_string(&api,
                                                                    reason));
    send_long(&api, alert, "runModal");
    send_void(&api, pool, "drain");
    mac_api_close(&api);
    return 1;
}

#endif
