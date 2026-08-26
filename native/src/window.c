#define _CRT_SECURE_NO_WARNINGS

#include "window.h"

#include "paint.h"
#include "window_runtime.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <wincodec.h>

typedef struct WindowControl {
    HWND handle;
    ZSharpUIElement *element;
    HBRUSH background;
    HFONT font;
    HBITMAP bitmap;
    int bitmap_width;
    int bitmap_height;
    COLORREF text_color;
    COLORREF button_color;
    ZSharpPaint button_paint;
    int has_text_color;
    int has_button_color;
    int is_image_input;
} WindowControl;

typedef struct WindowState {
    ZSharpProgram *program;
    const char *project_root;
    ZSharpWindowCallback callback;
    void *callback_data;
    HWND window;
    HBRUSH window_background;
    HICON window_icon;
    WindowControl *controls;
    size_t control_count;
    double scale;
    int screen_width;
    int screen_height;
    int layout_width;
    int layout_height;
    int scroll_y;
    int content_height;
    char callback_error[512];
    ZSharpPaint background_paint;
    ZSharpWindowRuntime runtime;
    DWORD ui_thread_id;
    volatile LONG closing;
} WindowState;

typedef struct WindowPropertyRequest {
    WindowState *state;
    const char *path;
    ZSharpWindowValueType value_type;
    const char *text_value;
    ZSharpUIUnit unit;
    int result;
    char error[512];
} WindowPropertyRequest;

static const char *WINDOW_CLASS_NAME = "ZombieOS.ZSharp.Window.1";
#define ZSHARP_WM_SET_PROPERTY (WM_APP + 37)

static void layout_controls(WindowState *state);
static int set_window_property(void *data, const char *path,
                               ZSharpWindowValueType value_type,
                               const char *text_value, ZSharpUIUnit unit,
                               char *error, size_t error_size);
static int wait_with_window_events(void *data, const char *milliseconds,
                                   char *error, size_t error_size);
static int runtime_set_window_property(void *data, const char *path,
                                       ZSharpWindowValueType value_type,
                                       const char *text_value,
                                       ZSharpUIUnit unit, char *error,
                                       size_t error_size);
static int window_is_cancelled(void *data);

static int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static ZSharpUIProperty *find_property(ZSharpUIElement *element,
                                       const char *name) {
    size_t index;
    for (index = 0; index < element->property_count; index++) {
        if (strcmp(element->properties[index].name, name) == 0) {
            return &element->properties[index];
        }
    }
    return NULL;
}

static ZSharpUIElement *find_design(ZSharpWindow *window) {
    size_t index;
    for (index = 0; index < window->element_count; index++) {
        if (window->elements[index].type == ZUI_DESIGN) {
            return &window->elements[index];
        }
    }
    return NULL;
}

static COLORREF parse_color(const char *text, COLORREF fallback) {
    int digits[6];
    unsigned red;
    unsigned green;
    unsigned blue;
    size_t index;
    if (text == NULL || strlen(text) != 7 || text[0] != '#') return fallback;
    for (index = 0; index < 6; index++) {
        digits[index] = hex_value(text[index + 1]);
        if (digits[index] < 0) return fallback;
    }
    red = (unsigned)(digits[0] * 16 + digits[1]);
    green = (unsigned)(digits[2] * 16 + digits[3]);
    blue = (unsigned)(digits[4] * 16 + digits[5]);
    return RGB(red, green, blue);
}

static COLORREF paint_first_color(const ZSharpPaint *paint,
                                  COLORREF fallback) {
    uint32_t color;
    if (paint == NULL || paint->color_count == 0) return fallback;
    color = paint->colors[0];
    return RGB((color >> 16) & 0xffu, (color >> 8) & 0xffu,
               color & 0xffu);
}

static void paint_rectangle(HDC context, const RECT *area,
                            const ZSharpPaint *paint) {
    int width = area->right - area->left;
    int height = area->bottom - area->top;
    size_t pixel_count;
    uint32_t *pixels;
    BITMAPINFO info;
    int x;
    int y;
    double radians;
    double direction_x;
    double direction_y;
    double extent;
    if (paint == NULL || paint->color_count == 0 || width <= 0 || height <= 0)
        return;
    if (paint->kind == ZSHARP_PAINT_SOLID) {
        HBRUSH brush = CreateSolidBrush(paint_first_color(paint, RGB(255,255,255)));
        if (brush != NULL) {
            FillRect(context, area, brush);
            DeleteObject(brush);
        }
        return;
    }
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(*pixels)) return;
    pixels = (uint32_t *)malloc(pixel_count * sizeof(*pixels));
    if (pixels == NULL) return;
    radians = paint->degrees * 3.14159265358979323846 / 180.0;
    direction_x = sin(radians);
    direction_y = -cos(radians);
    extent = fabs(direction_x) + fabs(direction_y);
    if (extent < 0.000001) extent = 1.0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            double nx = ((double)x + 0.5) / (double)width - 0.5;
            double ny = ((double)y + 0.5) / (double)height - 0.5;
            double position;
            uint32_t rgb;
            if (paint->kind == ZSHARP_PAINT_RADIAL) {
                double focus_x = direction_x * 0.2;
                double focus_y = direction_y * 0.2;
                double far_x = 0.5 + fabs(focus_x);
                double far_y = 0.5 + fabs(focus_y);
                double radius = sqrt(far_x * far_x + far_y * far_y);
                double dx = nx - focus_x;
                double dy = ny - focus_y;
                position = sqrt(dx * dx + dy * dy) / radius;
            } else {
                position = 0.5 + (nx * direction_x + ny * direction_y) /
                    extent;
            }
            rgb = zsharp_paint_sample(paint, position);
            pixels[(size_t)y * (size_t)width + (size_t)x] =
                (rgb & 0x00ff00u) |
                ((rgb & 0xff0000u) >> 16) |
                ((rgb & 0x0000ffu) << 16);
        }
    }
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(context, area->left, area->top, width, height,
                  0, 0, width, height, pixels, &info, DIB_RGB_COLORS,
                  SRCCOPY);
    free(pixels);
}

static double display_scale(void) {
    HDC screen = GetDC(NULL);
    double scale = 1.0;
    if (screen != NULL) {
        int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        if (dpi > 0) scale = (double)dpi / 96.0;
        ReleaseDC(NULL, screen);
    }
    return scale;
}

static int measurement_pixels(const ZSharpUIProperty *property,
                              double scale, int fallback) {
    double value;
    if (property == NULL || property->text_value == NULL) return fallback;
    value = strtod(property->text_value, NULL);
    if (property->unit == ZUI_UNIT_ZU) value *= 4.0 * scale;
    return (int)(value < 0.0 ? value - 0.5 : value + 0.5);
}

static char *join_path(const char *root, const char *relative) {
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    int separator = root_length > 0 && root[root_length - 1] != '/' &&
                    root[root_length - 1] != '\\';
    char *path = (char *)malloc(root_length + (size_t)separator +
                               relative_length + 1);
    if (path == NULL) return NULL;
    memcpy(path, root, root_length);
    if (separator) path[root_length++] = '\\';
    memcpy(path + root_length, relative, relative_length + 1);
    return path;
}

static WCHAR *utf8_to_wide(const char *text) {
    int length;
    WCHAR *wide;
    if (text == NULL) return NULL;
    length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length == 0) {
        length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
        if (length == 0) return NULL;
        wide = (WCHAR *)malloc((size_t)length * sizeof(*wide));
        if (wide != NULL)
            MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
        return wide;
    }
    wide = (WCHAR *)malloc((size_t)length * sizeof(*wide));
    if (wide != NULL)
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
    return wide;
}

static HBITMAP load_image_bitmap(const char *path, int requested_width,
                                 int requested_height) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapSource *source = NULL;
    WCHAR *wide_path = utf8_to_wide(path);
    BITMAPINFO bitmap_info;
    HBITMAP bitmap = NULL;
    void *pixels = NULL;
    UINT width = 0;
    UINT height = 0;
    UINT stride;
    UINT buffer_size;
    HRESULT result;
    if (wide_path == NULL) return NULL;
    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                              CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(result))
        result = IWICImagingFactory_CreateDecoderFromFilename(
            factory, wide_path, NULL, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(result)) result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (SUCCEEDED(result) && requested_width > 0 && requested_height > 0 &&
        (width != (UINT)requested_width || height != (UINT)requested_height)) {
        result = IWICImagingFactory_CreateBitmapScaler(factory, &scaler);
        if (SUCCEEDED(result))
            result = IWICBitmapScaler_Initialize(
                scaler, (IWICBitmapSource *)frame, (UINT)requested_width,
                (UINT)requested_height, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(result)) {
            source = (IWICBitmapSource *)scaler;
            width = (UINT)requested_width;
            height = (UINT)requested_height;
        }
    } else if (SUCCEEDED(result)) {
        source = (IWICBitmapSource *)frame;
    }
    if (SUCCEEDED(result))
        result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(result))
        result = IWICFormatConverter_Initialize(
            converter, source, &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(result) && width > 0 && height > 0 &&
        width <= UINT_MAX / 4u) {
        stride = width * 4u;
        if (height <= UINT_MAX / stride) {
            buffer_size = stride * height;
            memset(&bitmap_info, 0, sizeof(bitmap_info));
            bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
            bitmap_info.bmiHeader.biWidth = (LONG)width;
            bitmap_info.bmiHeader.biHeight = -(LONG)height;
            bitmap_info.bmiHeader.biPlanes = 1;
            bitmap_info.bmiHeader.biBitCount = 32;
            bitmap_info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(NULL, &bitmap_info, DIB_RGB_COLORS,
                                      &pixels, NULL, 0);
            if (bitmap != NULL && FAILED(IWICFormatConverter_CopyPixels(
                    converter, NULL, stride, buffer_size,
                    (BYTE *)pixels))) {
                DeleteObject(bitmap);
                bitmap = NULL;
            }
        }
    }
    if (converter != NULL) IWICFormatConverter_Release(converter);
    if (scaler != NULL) IWICBitmapScaler_Release(scaler);
    if (frame != NULL) IWICBitmapFrameDecode_Release(frame);
    if (decoder != NULL) IWICBitmapDecoder_Release(decoder);
    if (factory != NULL) IWICImagingFactory_Release(factory);
    free(wide_path);
    return bitmap;
}

static HICON load_window_icon(const char *path, int size) {
    HBITMAP color = load_image_bitmap(path, size, size);
    HBITMAP mask;
    ICONINFO info;
    HICON icon;
    if (color == NULL) return NULL;
    mask = CreateBitmap(size, size, 1, 1, NULL);
    if (mask == NULL) {
        DeleteObject(color);
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static WindowControl *find_control(WindowState *state, HWND handle) {
    size_t index;
    for (index = 0; index < state->control_count; index++) {
        if (state->controls[index].handle == handle) {
            return &state->controls[index];
        }
    }
    return NULL;
}

static void show_callback_error(WindowState *state, const char *message) {
    MessageBoxA(state->window, message, "Z# callback error",
                MB_OK | MB_ICONERROR);
}

static void run_callback(WindowState *state, ZSharpUIElement *element,
                         const char *button) {
    ZSharpUIProperty *target = find_property(element, button);
    if (target == NULL || target->text_value == NULL ||
        target->text_value[0] == '\0') return;
    state->callback_error[0] = '\0';
    if (!state->callback(state->callback_data, target->text_value,
                         &state->runtime,
                         state->callback_error,
                         sizeof(state->callback_error))) {
        show_callback_error(
            state, state->callback_error[0] == '\0'
                       ? "The Z# callback failed."
                       : state->callback_error);
    }
}

static void update_input_contents(WindowControl *control) {
    ZSharpUIProperty *contents = find_property(control->element, "contents");
    int length;
    char *text;
    if (contents == NULL || control->handle == NULL) return;
    length = GetWindowTextLengthA(control->handle);
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) return;
    GetWindowTextA(control->handle, text, length + 1);
    free(contents->text_value);
    contents->text_value = text;
}

static void choose_image(WindowState *state, WindowControl *control) {
    OPENFILENAMEA dialog;
    char file[MAX_PATH] = {0};
    ZSharpUIProperty *contents;
    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state->window;
    dialog.lpstrFile = file;
    dialog.nMaxFile = sizeof(file);
    dialog.lpstrFilter =
        "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&dialog)) return;
    contents = find_property(control->element, "contents");
    if (contents != NULL) {
        free(contents->text_value);
        contents->text_value =
            zsharp_copy_text(file, strlen(file));
    }
    SetWindowTextA(control->handle, file);
}

static void set_vertical_scroll(WindowState *state, int position) {
    int maximum;
    RECT area;
    GetClientRect(state->window, &area);
    maximum = state->content_height - (area.bottom - area.top);
    if (maximum < 0) maximum = 0;
    if (position < 0) position = 0;
    if (position > maximum) position = maximum;
    if (position == state->scroll_y) return;
    state->scroll_y = position;
    layout_controls(state);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    WindowState *state =
        (WindowState *)GetWindowLongPtrA(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
        state = (WindowState *)create->lpCreateParams;
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)state);
        state->window = window;
    }
    if (state == NULL) return DefWindowProcA(window, message, wparam, lparam);
    switch (message) {
        case ZSHARP_WM_SET_PROPERTY: {
            WindowPropertyRequest *request =
                (WindowPropertyRequest *)lparam;
            if (request == NULL) return 0;
            if (window_is_cancelled(state)) {
                snprintf(request->error, sizeof(request->error),
                         "the window is closing");
                request->result = 0;
            } else {
                request->result = set_window_property(
                    state, request->path, request->value_type,
                    request->text_value, request->unit, request->error,
                    sizeof(request->error));
            }
            return request->result;
        }
        case WM_COMMAND: {
            HWND control_handle = (HWND)lparam;
            WindowControl *control = find_control(state, control_handle);
            if (control == NULL) break;
            if (HIWORD(wparam) == BN_CLICKED) {
                if (control->is_image_input) {
                    choose_image(state, control);
                } else if (control->element->type == ZUI_BUTTON) {
                    run_callback(state, control->element, "left");
                }
            } else if (HIWORD(wparam) == EN_CHANGE &&
                       control->element->type == ZUI_TEXT_INPUT) {
                update_input_contents(control);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            WindowControl *control = find_control(state, (HWND)wparam);
            if (control != NULL && control->element->type == ZUI_BUTTON) {
                run_callback(state, control->element, "right");
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            WindowControl *control = find_control(state, (HWND)lparam);
            HDC context = (HDC)wparam;
            if (control != NULL && control->has_text_color) {
                SetTextColor(context, control->text_color);
            }
            SetBkMode(context, TRANSPARENT);
            if (control != NULL && control->element->type == ZUI_TEXT)
                return (LRESULT)GetStockObject(HOLLOW_BRUSH);
            return (LRESULT)state->window_background;
        }
        case WM_CTLCOLOREDIT: {
            WindowControl *control = find_control(state, (HWND)lparam);
            HDC context = (HDC)wparam;
            if (control != NULL && control->has_text_color) {
                SetTextColor(context, control->text_color);
            }
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lparam;
            WindowControl *control = find_control(state, draw->hwndItem);
            if (control != NULL && control->element->type == ZUI_BUTTON) {
                char text[512];
                HGDIOBJ previous_font;
                COLORREF color = control->has_button_color
                    ? control->button_color : RGB(240, 240, 240);
                if (control->button_paint.color_count != 0) {
                    paint_rectangle(draw->hDC, &draw->rcItem,
                                    &control->button_paint);
                } else {
                    HBRUSH brush = CreateSolidBrush(color);
                    FillRect(draw->hDC, &draw->rcItem, brush);
                    DeleteObject(brush);
                }
                DrawEdge(draw->hDC, &draw->rcItem,
                         (draw->itemState & ODS_SELECTED) ? EDGE_SUNKEN
                                                          : EDGE_RAISED,
                         BF_RECT);
                GetWindowTextA(draw->hwndItem, text, sizeof(text));
                previous_font = SelectObject(draw->hDC, control->font);
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, control->has_text_color
                    ? control->text_color : RGB(0, 0, 0));
                DrawTextA(draw->hDC, text, -1, &draw->rcItem,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(draw->hDC, previous_font);
                return TRUE;
            }
            break;
        }
        case WM_ERASEBKGND: {
            RECT area;
            GetClientRect(window, &area);
            paint_rectangle((HDC)wparam, &area, &state->background_paint);
            return 1;
        }
        case WM_SIZE:
            if (state->control_count != 0) layout_controls(state);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info;
            int position;
            memset(&info, 0, sizeof(info));
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &info);
            position = state->scroll_y;
            switch (LOWORD(wparam)) {
                case SB_TOP: position = 0; break;
                case SB_BOTTOM: position = info.nMax; break;
                case SB_LINEUP: position -= 32; break;
                case SB_LINEDOWN: position += 32; break;
                case SB_PAGEUP: position -= (int)info.nPage; break;
                case SB_PAGEDOWN: position += (int)info.nPage; break;
                case SB_THUMBPOSITION:
                case SB_THUMBTRACK: position = info.nTrackPos; break;
                default: return 0;
            }
            set_vertical_scroll(state, position);
            return 0;
        }
        case WM_MOUSEWHEEL:
            set_vertical_scroll(
                state, state->scroll_y -
                GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 48);
            return 0;
        case WM_CLOSE:
            InterlockedExchange(&state->closing, 1);
            DestroyWindow(window);
            return 0;
        case WM_TIMER:
            if (wparam == 1) {
                KillTimer(window, 1);
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DESTROY:
            InterlockedExchange(&state->closing, 1);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static int text_height(const char *variant) {
    if (variant == NULL || strcmp(variant, "paragraph") == 0) return 20;
    if (strcmp(variant, "subheader") == 0) return 28;
    if (strcmp(variant, "header") == 0) return 36;
    if (strcmp(variant, "subtitle") == 0) return 44;
    return 52;
}

static HFONT create_text_font(const char *variant, double scale) {
    int height = -(int)((double)text_height(variant) * scale + 0.5);
    int weight = variant != NULL &&
                 (strcmp(variant, "title") == 0 ||
                  strcmp(variant, "header") == 0)
        ? FW_BOLD : FW_NORMAL;
    return CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void element_default_size(const ZSharpUIElement *element,
                                 int *width, int *height) {
    *width = 200;
    *height = 32;
    if (element->type == ZUI_TEXT) {
        *width = 400;
        *height = text_height(element->variant) + 12;
    }
}

static int wrapped_text_height(WindowState *state, WindowControl *control,
                               int width, int fallback) {
    HDC context;
    HGDIOBJ previous;
    RECT bounds = {0, 0, width > 16 ? width - 16 : width, 0};
    char *text;
    int length;
    int height = fallback;
    if (width <= 0 || control->handle == NULL) return fallback;
    length = GetWindowTextLengthA(control->handle);
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) return fallback;
    GetWindowTextA(control->handle, text, length + 1);
    context = GetDC(state->window);
    if (context != NULL) {
        previous = SelectObject(context, control->font);
        if (DrawTextA(context, text, -1, &bounds,
                      DT_CALCRECT | DT_WORDBREAK | DT_CENTER |
                      DT_NOPREFIX) != 0) {
            height = bounds.bottom - bounds.top + 6;
            if (height < fallback) height = fallback;
        }
        SelectObject(context, previous);
        ReleaseDC(state->window, context);
    }
    free(text);
    return height;
}

static int longest_word_width(WindowState *state, WindowControl *control) {
    HDC context;
    HGDIOBJ previous;
    char *text;
    char *cursor;
    int length;
    int longest = 0;
    if (control->handle == NULL) return 0;
    length = GetWindowTextLengthA(control->handle);
    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) return 0;
    GetWindowTextA(control->handle, text, length + 1);
    context = GetDC(state->window);
    if (context != NULL) {
        previous = SelectObject(context, control->font);
        cursor = text;
        while (*cursor != '\0') {
            char *start;
            SIZE size;
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                   *cursor == '\n') cursor++;
            start = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
                   *cursor != '\r' && *cursor != '\n') cursor++;
            if (cursor > start && GetTextExtentPoint32A(
                    context, start, (int)(cursor - start), &size) &&
                size.cx > longest) longest = size.cx;
        }
        SelectObject(context, previous);
        ReleaseDC(state->window, context);
    }
    free(text);
    return longest;
}

static void reload_control_image(WindowState *state, WindowControl *control,
                                 int width, int height) {
    ZSharpUIProperty *file;
    char *image_path;
    HBITMAP replacement;
    if (control->element->type != ZUI_IMAGE || width <= 0 || height <= 0 ||
        (control->bitmap != NULL && control->bitmap_width == width &&
         control->bitmap_height == height)) return;
    file = find_property(control->element, "file");
    if (file == NULL || file->text_value == NULL) return;
    image_path = join_path(state->project_root, file->text_value);
    replacement = image_path == NULL ? NULL :
        load_image_bitmap(image_path, width, height);
    free(image_path);
    if (replacement == NULL) return;
    if (control->bitmap != NULL) DeleteObject(control->bitmap);
    control->bitmap = replacement;
    control->bitmap_width = width;
    control->bitmap_height = height;
    SendMessageA(control->handle, STM_SETIMAGE, IMAGE_BITMAP,
                 (LPARAM)replacement);
}

static void layout_controls(WindowState *state) {
    RECT area;
    size_t index;
    int client_width;
    int client_height;
    int content_bottom = 0;
    int initial_scroll = state->scroll_y;
    double responsive_scale;
    SCROLLINFO scroll;
    GetClientRect(state->window, &area);
    client_width = area.right - area.left;
    client_height = area.bottom - area.top;
    responsive_scale = state->layout_width > 0
        ? (double)client_width / (double)state->layout_width : 1.0;
    if (responsive_scale > 1.0) responsive_scale = 1.0;
    if (responsive_scale < 0.05) responsive_scale = 0.05;
    for (index = 0; index < state->control_count; index++) {
        WindowControl *control = &state->controls[index];
        ZSharpUIElement *element = control->element;
        int default_width;
        int default_height;
        int width;
        int height;
        int x;
        int y;
        element_default_size(element, &default_width, &default_height);
        width = (int)((double)measurement_pixels(
            find_property(element, "width"), state->scale, default_width) *
            responsive_scale + 0.5);
        height = (int)((double)measurement_pixels(
            find_property(element, "height"), state->scale, default_height) *
            responsive_scale + 0.5);
        x = (int)(((double)state->layout_width / 2.0 +
            (double)measurement_pixels(find_property(element, "locationX"),
                                       state->scale, 0) -
            (double)measurement_pixels(find_property(element, "width"),
                                       state->scale, default_width) / 2.0) *
            responsive_scale + 0.5);
        if (element->type != ZUI_TEXT) {
            if (width < 32) width = 32;
            if (height < 16) height = 16;
        } else {
            int word_width;
            int available_width = client_width - x - 8;
            if (width < 24) width = 24;
            word_width = longest_word_width(state, control) + 16;
            if (word_width > width) width = word_width;
            if (available_width < 1) available_width = 1;
            if (width > available_width) width = available_width;
            if (width < 1) width = 1;
            if (find_property(element, "height") == NULL)
                height = wrapped_text_height(state, control, width, height);
        }
        y = (int)(((double)state->layout_height / 2.0 -
            (double)measurement_pixels(find_property(element, "locationY"),
                                       state->scale, 0) -
            (double)measurement_pixels(find_property(element, "height"),
                                       state->scale, default_height) / 2.0) *
            responsive_scale + 0.5);
        if (y + height > content_bottom) content_bottom = y + height;
        MoveWindow(control->handle, x, y - state->scroll_y,
                   width, height, TRUE);
        reload_control_image(state, control, width, height);
    }
    state->content_height = content_bottom +
        (int)(16.0 * responsive_scale + 0.5);
    if (state->content_height < client_height)
        state->content_height = client_height;
    if (state->scroll_y > state->content_height - client_height)
        state->scroll_y = state->content_height - client_height;
    if (state->scroll_y < 0) state->scroll_y = 0;
    memset(&scroll, 0, sizeof(scroll));
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = state->content_height > 0 ? state->content_height - 1 : 0;
    scroll.nPage = (UINT)(client_height > 0 ? client_height : 0);
    scroll.nPos = state->scroll_y;
    SetScrollInfo(state->window, SB_VERT, &scroll, TRUE);
    if (initial_scroll != state->scroll_y) layout_controls(state);
}

static int create_controls(WindowState *state, int client_width,
                           int client_height, double scale, char *error,
                           size_t error_size) {
    ZSharpWindow *window = &state->program->window;
    size_t index;
    size_t control_index = 0;
    state->controls = (WindowControl *)calloc(
        window->element_count, sizeof(*state->controls));
    if (state->controls == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    for (index = 0; index < window->element_count; index++) {
        ZSharpUIElement *element = &window->elements[index];
        ZSharpUIProperty *x_property;
        ZSharpUIProperty *y_property;
        ZSharpUIProperty *width_property;
        ZSharpUIProperty *height_property;
        ZSharpUIProperty *text_property = NULL;
        ZSharpUIProperty *color_property = NULL;
        WindowControl *control;
        const char *class_name;
        const char *display_text = "";
        DWORD style = WS_CHILD | WS_VISIBLE;
        int default_width;
        int default_height;
        int x;
        int y;
        int width;
        int height;
        if (element->type == ZUI_DESIGN) continue;
        element_default_size(element, &default_width, &default_height);
        if (element->type == ZUI_TEXT) {
            class_name = "STATIC";
            text_property = find_property(element, "content");
            color_property = find_property(element, "color");
            display_text = text_property == NULL ? "" : text_property->text_value;
            style |= SS_CENTER | SS_EDITCONTROL;
        } else if (element->type == ZUI_BUTTON) {
            class_name = "BUTTON";
            text_property = find_property(element, "text");
            color_property = find_property(element, "textColor");
            display_text = text_property == NULL ? "" : text_property->text_value;
            style |= BS_OWNERDRAW;
        } else if (element->type == ZUI_IMAGE) {
            class_name = "STATIC";
            display_text = "";
            style |= SS_BITMAP | SS_CENTERIMAGE | WS_BORDER;
        } else {
            ZSharpUIProperty *type = find_property(element, "type");
            ZSharpUIProperty *display = find_property(element, "display");
            int is_image = type != NULL &&
                           strcmp(type->text_value, "image") == 0;
            class_name = is_image ? "BUTTON" : "EDIT";
            display_text = is_image && display != NULL
                ? display->text_value : "";
            style |= is_image ? BS_PUSHBUTTON
                              : WS_BORDER | ES_AUTOHSCROLL;
        }
        x_property = find_property(element, "locationX");
        y_property = find_property(element, "locationY");
        width_property = find_property(element, "width");
        height_property = find_property(element, "height");
        width = measurement_pixels(width_property, scale, default_width);
        height = measurement_pixels(height_property, scale, default_height);
        x = client_width / 2 + measurement_pixels(x_property, scale, 0) -
            width / 2;
        y = client_height / 2 - measurement_pixels(y_property, scale, 0) -
            height / 2;
        control = &state->controls[control_index];
        control->element = element;
        control->handle = CreateWindowExA(
            element->type == ZUI_TEXT_INPUT ? WS_EX_CLIENTEDGE :
            element->type == ZUI_TEXT ? WS_EX_TRANSPARENT : 0,
            class_name, display_text, style, x, y, width, height,
            state->window, (HMENU)(INT_PTR)(1000 + control_index),
            GetModuleHandleA(NULL), NULL);
        if (control->handle == NULL) {
            snprintf(error, error_size,
                     "could not create UI element '%s'", element->name);
            return 0;
        }
        if (color_property != NULL) {
            control->text_color =
                parse_color(color_property->text_value, RGB(0, 0, 0));
            control->has_text_color = 1;
        }
        if (element->type == ZUI_BUTTON) {
            ZSharpUIProperty *button_color =
                find_property(element, "buttonColor");
            if (button_color != NULL) {
                control->button_color = parse_color(
                    button_color->text_value, RGB(240, 240, 240));
                control->has_button_color = 1;
                if (!zsharp_paint_parse(button_color->text_value,
                                        &control->button_paint,
                                        error, error_size)) return 0;
            }
        }
        if (element->type == ZUI_TEXT) {
            control->font = create_text_font(element->variant, scale);
        } else {
            control->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
        SendMessageA(control->handle, WM_SETFONT,
                     (WPARAM)control->font, TRUE);
        if (element->type == ZUI_TEXT_INPUT) {
            ZSharpUIProperty *type = find_property(element, "type");
            ZSharpUIProperty *display = find_property(element, "display");
            control->is_image_input = type != NULL &&
                strcmp(type->text_value, "image") == 0;
            if (!control->is_image_input && display != NULL &&
                display->text_value != NULL) {
                WCHAR *cue = utf8_to_wide(display->text_value);
                if (cue != NULL) {
                    SendMessageW(control->handle, 0x1501u, TRUE,
                                 (LPARAM)cue);
                    free(cue);
                }
            }
        } else if (element->type == ZUI_IMAGE) {
            ZSharpUIProperty *file = find_property(element, "file");
            char *image_path = file == NULL ? NULL
                : join_path(state->project_root, file->text_value);
            if (image_path != NULL) {
                control->bitmap = load_image_bitmap(image_path, width, height);
                if (control->bitmap != NULL) {
                    control->bitmap_width = width;
                    control->bitmap_height = height;
                    SendMessageA(control->handle, STM_SETIMAGE, IMAGE_BITMAP,
                                 (LPARAM)control->bitmap);
                }
                free(image_path);
            }
        }
        control_index++;
        state->control_count = control_index;
    }
    state->control_count = control_index;
    return 1;
}

static void cleanup_window_state(WindowState *state) {
    size_t index;
    for (index = 0; index < state->control_count; index++) {
        WindowControl *control = &state->controls[index];
        if (control->font != NULL &&
            control->element->type == ZUI_TEXT) {
            DeleteObject(control->font);
        }
        if (control->background != NULL) DeleteObject(control->background);
        if (control->bitmap != NULL) DeleteObject(control->bitmap);
        zsharp_paint_free(&control->button_paint);
    }
    free(state->controls);
    if (state->window_background != NULL) {
        DeleteObject(state->window_background);
    }
    if (state->window_icon != NULL) DestroyIcon(state->window_icon);
    zsharp_paint_free(&state->background_paint);
}

static WindowControl *control_for_element(WindowState *state,
                                          ZSharpUIElement *element) {
    size_t index;
    for (index = 0; index < state->control_count; index++)
        if (state->controls[index].element == element) return &state->controls[index];
    return NULL;
}

static void replace_background_brush(WindowState *state) {
    HBRUSH replacement = CreateSolidBrush(paint_first_color(
        &state->background_paint, RGB(255, 255, 255)));
    if (replacement != NULL) {
        if (state->window_background != NULL)
            DeleteObject(state->window_background);
        state->window_background = replacement;
    }
}

static void resize_window_client(WindowState *state) {
    ZSharpUIElement *design = find_design(&state->program->window);
    RECT client;
    RECT outer = {0, 0, 0, 0};
    DWORD style;
    int width;
    int height;
    if (design == NULL || state->window == NULL) return;
    GetClientRect(state->window, &client);
    width = measurement_pixels(find_property(design, "width"), state->scale,
                               client.right - client.left);
    height = measurement_pixels(find_property(design, "height"), state->scale,
                                client.bottom - client.top);
    outer.right = width;
    outer.bottom = height;
    style = (DWORD)GetWindowLongPtrA(state->window, GWL_STYLE);
    AdjustWindowRect(&outer, style, FALSE);
    SetWindowPos(state->window, NULL, 0, 0, outer.right - outer.left,
                 outer.bottom - outer.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void position_window_from_design(WindowState *state) {
    ZSharpUIElement *design = find_design(&state->program->window);
    RECT bounds;
    int width;
    int height;
    int x;
    int y;
    if (design == NULL || state->window == NULL ||
        !GetWindowRect(state->window, &bounds)) return;
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2 +
        measurement_pixels(find_property(design, "locationX"),
                           state->scale, 0);
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2 -
        measurement_pixels(find_property(design, "locationY"),
                           state->scale, 0);
    SetWindowPos(state->window, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static int set_window_property(void *data, const char *path,
                               ZSharpWindowValueType value_type,
                               const char *text_value, ZSharpUIUnit unit,
                               char *error, size_t error_size) {
    WindowState *state = (WindowState *)data;
    ZSharpUIElement *element = NULL;
    ZSharpUIProperty *property = NULL;
    WindowControl *control;
    if (!zsharp_window_model_set(state->program, path, value_type, text_value,
                                 unit, &element, &property,
                                 error, error_size)) return 0;
    if (element->type == ZUI_DESIGN) {
        if (strcmp(property->name, "title") == 0) {
            SetWindowTextA(state->window, property->text_value);
        } else if (strcmp(property->name, "background") == 0) {
            ZSharpPaint replacement;
            memset(&replacement, 0, sizeof(replacement));
            if (!zsharp_paint_parse(property->text_value, &replacement,
                                    error, error_size)) return 0;
            zsharp_paint_free(&state->background_paint);
            state->background_paint = replacement;
            replace_background_brush(state);
        } else if (strcmp(property->name, "scalable") == 0) {
            LONG_PTR style = GetWindowLongPtrA(state->window, GWL_STYLE);
            if (property->status_value)
                style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            else
                style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
            SetWindowLongPtrA(state->window, GWL_STYLE, style);
            SetWindowPos(state->window, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (strcmp(property->name, "width") == 0 ||
                   strcmp(property->name, "height") == 0) {
            resize_window_client(state);
            position_window_from_design(state);
        } else if (strcmp(property->name, "locationX") == 0 ||
                   strcmp(property->name, "locationY") == 0) {
            position_window_from_design(state);
        } else if (strcmp(property->name, "icon") == 0) {
            char *icon_path = join_path(state->project_root,
                                        property->text_value);
            HICON replacement = icon_path == NULL ? NULL : load_window_icon(
                icon_path, (int)(32.0 * state->scale + 0.5));
            free(icon_path);
            if (replacement == NULL) {
                snprintf(error, error_size, "could not load window icon '%s'",
                         property->text_value);
                return 0;
            }
            SendMessageA(state->window, WM_SETICON, ICON_BIG,
                         (LPARAM)replacement);
            SendMessageA(state->window, WM_SETICON, ICON_SMALL,
                         (LPARAM)replacement);
            if (state->window_icon != NULL) DestroyIcon(state->window_icon);
            state->window_icon = replacement;
        }
    } else {
        control = control_for_element(state, element);
        if (control == NULL) {
            snprintf(error, error_size, "UI element '%s' is not active",
                     element->name);
            return 0;
        }
        if (strcmp(property->name, "content") == 0 ||
            strcmp(property->name, "text") == 0) {
            SetWindowTextA(control->handle, property->text_value);
            if (element->type == ZUI_TEXT) layout_controls(state);
        } else if (strcmp(property->name, "display") == 0) {
            WCHAR *cue = utf8_to_wide(property->text_value);
            if (cue != NULL) {
                SendMessageW(control->handle, 0x1501u, TRUE, (LPARAM)cue);
                free(cue);
            }
        } else if (strcmp(property->name, "color") == 0 ||
                   strcmp(property->name, "textColor") == 0) {
            control->text_color = parse_color(property->text_value,
                                              RGB(0, 0, 0));
            control->has_text_color = 1;
        } else if (strcmp(property->name, "buttonColor") == 0) {
            ZSharpPaint replacement;
            memset(&replacement, 0, sizeof(replacement));
            if (!zsharp_paint_parse(property->text_value, &replacement,
                                    error, error_size)) return 0;
            zsharp_paint_free(&control->button_paint);
            control->button_paint = replacement;
            control->button_color = paint_first_color(
                &replacement, RGB(240, 240, 240));
            control->has_button_color = 1;
        } else if (strcmp(property->name, "file") == 0) {
            int width;
            int height;
            RECT bounds;
            char *image_path;
            GetClientRect(control->handle, &bounds);
            width = bounds.right - bounds.left;
            height = bounds.bottom - bounds.top;
            image_path = join_path(state->project_root, property->text_value);
            if (control->bitmap != NULL) DeleteObject(control->bitmap);
            control->bitmap = image_path == NULL ? NULL : load_image_bitmap(
                image_path, width, height);
            free(image_path);
            if (control->bitmap == NULL) {
                snprintf(error, error_size, "could not load image '%s'",
                         property->text_value);
                return 0;
            }
            SendMessageA(control->handle, STM_SETIMAGE, IMAGE_BITMAP,
                         (LPARAM)control->bitmap);
        }
        if (property->type == ZUI_PROPERTY_MEASUREMENT)
            layout_controls(state);
    }
    RedrawWindow(state->window, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW |
                 RDW_ALLCHILDREN);
    return 1;
}

static int window_is_cancelled(void *data) {
    WindowState *state = (WindowState *)data;
    return InterlockedCompareExchange(&state->closing, 0, 0) != 0;
}

static int runtime_set_window_property(void *data, const char *path,
                                       ZSharpWindowValueType value_type,
                                       const char *text_value,
                                       ZSharpUIUnit unit, char *error,
                                       size_t error_size) {
    WindowState *state = (WindowState *)data;
    WindowPropertyRequest request;
    if (GetCurrentThreadId() == state->ui_thread_id) {
        return set_window_property(data, path, value_type, text_value, unit,
                                   error, error_size);
    }
    if (window_is_cancelled(state)) {
        snprintf(error, error_size, "the window is closing");
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.state = state;
    request.path = path;
    request.value_type = value_type;
    request.text_value = text_value;
    request.unit = unit;
    SendMessageA(state->window, ZSHARP_WM_SET_PROPERTY, 0,
                 (LPARAM)&request);
    if (!request.result) {
        snprintf(error, error_size, "%s",
                 request.error[0] == '\0' ? "the window is closing"
                                           : request.error);
    }
    return request.result;
}

static int wait_with_window_events(void *data, const char *milliseconds,
                                   char *error, size_t error_size) {
    WindowState *state = (WindowState *)data;
    char *end;
    double parsed = strtod(milliseconds, &end);
    ULONGLONG duration;
    ULONGLONG deadline;
    if (end == milliseconds || *end != '\0' || !isfinite(parsed) ||
        parsed < 0.0 || parsed > 604800000.0) {
        snprintf(error, error_size,
                 "wait/delay must be between 0ms and 604800000ms");
        return 0;
    }
    RedrawWindow(state->window, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW |
                 RDW_ALLCHILDREN);
    duration = (ULONGLONG)(parsed + 0.5);
    deadline = GetTickCount64() + duration;
    if (GetCurrentThreadId() != state->ui_thread_id) {
        while (GetTickCount64() < deadline) {
            ULONGLONG now = GetTickCount64();
            DWORD remaining = (DWORD)((deadline - now) > 10
                ? 10 : deadline - now);
            if (window_is_cancelled(state)) {
                snprintf(error, error_size, "the window is closing");
                return 0;
            }
            Sleep(remaining);
        }
        return !window_is_cancelled(state);
    }
    while (GetTickCount64() < deadline) {
        MSG message;
        ULONGLONG now = GetTickCount64();
        DWORD remaining = (DWORD)((deadline - now) > 50 ? 50 : deadline - now);
        MsgWaitForMultipleObjects(0, NULL, FALSE, remaining, QS_ALLINPUT);
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage((int)message.wParam);
                snprintf(error, error_size,
                         "the window closed during wait/delay");
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    return 1;
}

int zsharp_window_run(ZSharpProgram *program, const char *project_root,
                      ZSharpWindowCallback callback, void *user_data,
                      char *error, size_t error_size) {
    WindowState state;
    ZSharpUIElement *design = find_design(&program->window);
    ZSharpUIProperty *title;
    ZSharpUIProperty *background;
    ZSharpUIProperty *width_property;
    ZSharpUIProperty *height_property;
    ZSharpUIProperty *scalable;
    ZSharpUIProperty *icon;
    WNDCLASSEXA window_class;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
    RECT area;
    MSG message;
    double scale = display_scale();
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int client_width;
    int client_height;
    int outer_width;
    int outer_height;
    int x;
    int y;
    int result = 1;
    HRESULT com_result;
    int uninitialize_com = 0;
    char autoclose[32] = {0};
    if (getenv("ZSHARP_WINDOW_FORCE_FAILURE") != NULL) {
        snprintf(error, error_size, "forced window launch failure");
        return 0;
    }
    if (design == NULL) {
        snprintf(error, error_size, "window bytecode has no design");
        return 0;
    }
    title = find_property(design, "title");
    background = find_property(design, "background");
    width_property = find_property(design, "width");
    height_property = find_property(design, "height");
    scalable = find_property(design, "scalable");
    icon = find_property(design, "icon");
    client_width = measurement_pixels(
        width_property, scale, screen_width / 4);
    client_height = measurement_pixels(
        height_property, scale, screen_height / 4);
    com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com_result)) {
        uninitialize_com = 1;
    } else if (com_result != RPC_E_CHANGED_MODE) {
        snprintf(error, error_size,
                 "could not initialize Windows image support");
        return 0;
    }
    memset(&state, 0, sizeof(state));
    state.program = program;
    state.project_root = project_root;
    state.callback = callback;
    state.callback_data = user_data;
    state.scale = scale;
    state.screen_width = screen_width;
    state.screen_height = screen_height;
    state.layout_width = client_width;
    state.layout_height = client_height;
    state.ui_thread_id = GetCurrentThreadId();
    state.runtime.state = &state;
    state.runtime.set_property = runtime_set_window_property;
    state.runtime.wait = wait_with_window_events;
    state.runtime.is_cancelled = window_is_cancelled;
    if (!zsharp_paint_parse(
            background == NULL ? "#FFFFFF" : background->text_value,
            &state.background_paint, error, error_size)) {
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    state.window_background = CreateSolidBrush(parse_color(
        background == NULL ? NULL : background->text_value,
        paint_first_color(&state.background_paint, RGB(255, 255, 255))));
    if (state.window_background == NULL) {
        snprintf(error, error_size, "could not create the window background");
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = state.window_background;
    window_class.lpszClassName = WINDOW_CLASS_NAME;
    if (!RegisterClassExA(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        cleanup_window_state(&state);
        snprintf(error, error_size, "could not register the Z# window class");
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    if (scalable != NULL && !scalable->status_value) {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    area.left = 0;
    area.top = 0;
    area.right = client_width;
    area.bottom = client_height;
    AdjustWindowRect(&area, style, FALSE);
    outer_width = area.right - area.left;
    outer_height = area.bottom - area.top;
    x = (screen_width - outer_width) / 2;
    y = (screen_height - outer_height) / 2;
    x += measurement_pixels(find_property(design, "locationX"), scale, 0);
    y -= measurement_pixels(find_property(design, "locationY"), scale, 0);
    state.window = CreateWindowExA(
        0, WINDOW_CLASS_NAME,
        title == NULL ? program->window.name : title->text_value,
        style, x, y, outer_width, outer_height, NULL, NULL,
        GetModuleHandleA(NULL), &state);
    if (state.window == NULL) {
        cleanup_window_state(&state);
        snprintf(error, error_size, "could not create the Z# window");
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    if (icon != NULL && icon->text_value != NULL) {
        char *icon_path = join_path(project_root, icon->text_value);
        if (icon_path != NULL) {
            state.window_icon = load_window_icon(
                icon_path, (int)(32.0 * scale + 0.5));
            if (state.window_icon != NULL) {
                SendMessageA(state.window, WM_SETICON, ICON_BIG,
                             (LPARAM)state.window_icon);
                SendMessageA(state.window, WM_SETICON, ICON_SMALL,
                             (LPARAM)state.window_icon);
            }
            free(icon_path);
        }
    }
    if (!create_controls(&state, client_width, client_height, scale,
                         error, error_size)) {
        DestroyWindow(state.window);
        cleanup_window_state(&state);
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    layout_controls(&state);
    ShowWindow(state.window, SW_SHOWDEFAULT);
    UpdateWindow(state.window);
    if (getenv("ZSHARP_DISABLE_PROJECT_STARTS") == NULL &&
        !callback(user_data, ZSHARP_WINDOW_PROJECT_STARTS, &state.runtime,
                  error, error_size)) {
        InterlockedExchange(&state.closing, 1);
        DestroyWindow(state.window);
        callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                 state.callback_error, sizeof(state.callback_error));
        cleanup_window_state(&state);
        if (uninitialize_com) CoUninitialize();
        return 0;
    }
    {
        const char *autorun = getenv("ZSHARP_WINDOW_AUTORUN_CALLBACK");
        if (autorun != NULL && autorun[0] != '\0' &&
            !callback(user_data, autorun, &state.runtime,
                      error, error_size)) {
            InterlockedExchange(&state.closing, 1);
            DestroyWindow(state.window);
            callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                     state.callback_error, sizeof(state.callback_error));
            cleanup_window_state(&state);
            if (uninitialize_com) CoUninitialize();
            return 0;
        }
    }
    if (GetEnvironmentVariableA("ZSHARP_WINDOW_AUTOCLOSE_MS", autoclose,
                                sizeof(autoclose)) > 0) {
        unsigned long milliseconds = strtoul(autoclose, NULL, 10);
        if (milliseconds > 0 && milliseconds <= UINT_MAX) {
            SetTimer(state.window, 1, (UINT)milliseconds, NULL);
        }
    }
    while ((result = GetMessageA(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    if (result < 0) {
        snprintf(error, error_size, "the Windows event loop failed");
        InterlockedExchange(&state.closing, 1);
        if (IsWindow(state.window)) DestroyWindow(state.window);
        result = 0;
    } else {
        result = 1;
    }
    InterlockedExchange(&state.closing, 1);
    if (!callback(user_data, ZSHARP_WINDOW_TASKS_STOP, &state.runtime,
                  error, error_size)) result = 0;
    cleanup_window_state(&state);
    if (uninitialize_com) CoUninitialize();
    return result;
}

int zsharp_window_show_hub(const char *headline, const char *reason,
                           char *error, size_t error_size) {
    size_t headline_length = strlen(headline == NULL ? "Z# Hub" : headline);
    size_t reason_length = strlen(reason == NULL ? "" : reason);
    char *message = (char *)malloc(headline_length + reason_length + 4);
    int result;
    if (message == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    if (reason_length == 0)
        snprintf(message, headline_length + reason_length + 4, "%s",
                 headline == NULL ? "Z# Hub" : headline);
    else
        snprintf(message, headline_length + reason_length + 4, "%s\n\n%s",
                 headline == NULL ? "Z# Hub" : headline, reason);
    result = MessageBoxA(NULL, message, "Z# Hub", MB_OK | MB_ICONINFORMATION);
    free(message);
    if (result == 0) {
        snprintf(error, error_size, "could not open the Z# Hub");
        return 0;
    }
    return 1;
}

#endif
