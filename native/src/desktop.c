#define _CRT_SECURE_NO_WARNINGS

#include "desktop.h"
#include "app_icon.h"
#include "settings.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#else
#if defined(__APPLE__)
#include <dirent.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
extern int _NSGetExecutablePath(char *buffer, unsigned int *buffer_size);
#endif
#endif

static void desktop_error(char *error, size_t error_size,
                          const char *format, ...) {
    va_list arguments;
    if (error == NULL || error_size == 0) return;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

#ifndef _WIN32
static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}
#endif

#ifndef _WIN32
static char *join_path(const char *left, const char *right) {
    size_t a = strlen(left), b = strlen(right);
    int separator = a != 0 && left[a - 1] != '/' && left[a - 1] != '\\';
    char *result = (char *)malloc(a + (size_t)separator + b + 1);
    if (result == NULL) return NULL;
    memcpy(result, left, a);
    if (separator) {
#ifdef _WIN32
        result[a++] = '\\';
#else
        result[a++] = '/';
#endif
    }
    memcpy(result + a, right, b + 1);
    return result;
}
#endif

static char *safe_name(const char *display_name) {
    size_t length = strlen(display_name);
    size_t capacity = length < 10 ? 10 : length;
    char *result = (char *)malloc(capacity + 1);
    size_t input;
    size_t output = 0;
    if (result == NULL) return NULL;
    for (input = 0; input < length; input++) {
        unsigned char value = (unsigned char)display_name[input];
        if (value < 32 || value == '/' || value == '\\' || value == ':' ||
            value == '*' || value == '?' || value == '\"' || value == '<' ||
            value == '>' || value == '|') continue;
        result[output++] = (char)value;
    }
    while (output > 0 && (result[output - 1] == ' ' ||
                           result[output - 1] == '.')) output--;
    if (output == 0) {
        memcpy(result, "ZSharp App", 10);
        output = 10;
    }
    result[output] = '\0';
    return result;
}

/* Keep project metadata ahead of the legacy window-design icon for shortcuts. */
static char *project_icon_path(const char *project_root, const char *legacy_icon,
                               char **desktop_id, char *error, size_t error_size) {
    ZSharpSettings settings;
    ZSharpDiagnostic diagnostic;
    char ignored[512] = {0};
    const char *selected = legacy_icon;
    char *result;
    zsharp_settings_init(&settings);
    if (project_root != NULL &&
        zsharp_settings_load(project_root, &settings, &diagnostic,
                              ignored, sizeof(ignored)) &&
        settings.icon != NULL)
        selected = settings.icon;
    if (desktop_id != NULL)
        *desktop_id = zsharp_app_desktop_id(settings.project_id);
    result = zsharp_app_icon_resolve(project_root, selected, error, error_size);
    zsharp_settings_free(&settings);
    return result;
}

#ifdef _WIN32

static WCHAR *wide_text(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *result;
    if (count == 0) count = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (count == 0) return NULL;
    result = (WCHAR *)malloc((size_t)count * sizeof(*result));
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, result, count) == 0)
        MultiByteToWideChar(CP_ACP, 0, text, -1, result, count);
    return result;
}

static int set_registry_default(HKEY root, const WCHAR *path,
                                const WCHAR *value) {
    HKEY key;
    LONG result = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE,
                                  NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return 0;
    result = RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)value,
                            ((DWORD)wcslen(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static int windows_executable(WCHAR *path, DWORD capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    return length != 0 && length < capacity;
}

static int windows_desktop(WCHAR *path, DWORD capacity) {
    const char *override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    WCHAR *wide_override;
    size_t length;
    if (override == NULL || override[0] == '\0')
        return SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL,
                                SHGFP_TYPE_CURRENT, path) == S_OK;
    wide_override = wide_text(override);
    if (wide_override == NULL) return 0;
    length = wcslen(wide_override);
    if (length >= capacity) {
        free(wide_override);
        return 0;
    }
    memcpy(path, wide_override, (length + 1) * sizeof(*path));
    free(wide_override);
    return 1;
}

static void put_u16(unsigned char *bytes, unsigned value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, unsigned long value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

static int png_icon_file(const char *png_path, const char *ico_path) {
    enum { ICON_SIZE = 256, PIXEL_STRIDE = ICON_SIZE * 4,
           MASK_STRIDE = ICON_SIZE / 8 };
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    WCHAR *wide_path = wide_text(png_path);
    unsigned char header[22] = {0}, bitmap[40] = {0};
    unsigned char mask[MASK_STRIDE];
    unsigned char *pixels = NULL, *canvas = NULL;
    UINT width = 0, height = 0, draw_width = 0, draw_height = 0, row;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HRESULT result = E_FAIL;
    FILE *output = NULL;
    int ok = 0;
    if (wide_path == NULL || (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE))
        goto done;
    result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateDecoderFromFilename(
        factory, wide_path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(result)) result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (FAILED(result) || width == 0 || height == 0) goto done;
    draw_width = width >= height ? ICON_SIZE : (UINT)((uint64_t)width * ICON_SIZE / height);
    draw_height = height >= width ? ICON_SIZE : (UINT)((uint64_t)height * ICON_SIZE / width);
    if (draw_width == 0) draw_width = 1;
    if (draw_height == 0) draw_height = 1;
    result = IWICImagingFactory_CreateBitmapScaler(factory, &scaler);
    if (SUCCEEDED(result)) result = IWICBitmapScaler_Initialize(scaler,
        (IWICBitmapSource *)frame, draw_width, draw_height, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(result)) result = IWICFormatConverter_Initialize(converter,
        (IWICBitmapSource *)scaler, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) goto done;
    pixels = (unsigned char *)malloc((size_t)draw_width * draw_height * 4);
    canvas = (unsigned char *)calloc(ICON_SIZE, PIXEL_STRIDE);
    if (pixels == NULL || canvas == NULL) goto done;
    result = IWICFormatConverter_CopyPixels(converter, NULL, draw_width * 4,
        draw_width * draw_height * 4, pixels);
    if (FAILED(result)) goto done;
    for (row = 0; row < draw_height; row++)
        memcpy(canvas + ((ICON_SIZE - draw_height) / 2 + row) * PIXEL_STRIDE +
                       (ICON_SIZE - draw_width) / 2 * 4,
               pixels + row * draw_width * 4, draw_width * 4);
    put_u16(header + 2, 1); put_u16(header + 4, 1);
    put_u16(header + 10, 1); put_u16(header + 12, 32);
    put_u32(header + 14, sizeof(bitmap) + ICON_SIZE * (PIXEL_STRIDE + MASK_STRIDE));
    put_u32(header + 18, sizeof(header));
    put_u32(bitmap, sizeof(bitmap)); put_u32(bitmap + 4, ICON_SIZE);
    put_u32(bitmap + 8, ICON_SIZE * 2); put_u16(bitmap + 12, 1);
    put_u16(bitmap + 14, 32); put_u32(bitmap + 20, ICON_SIZE * PIXEL_STRIDE);
    output = fopen(ico_path, "wb");
    if (output == NULL || fwrite(header, 1, sizeof(header), output) != sizeof(header) ||
        fwrite(bitmap, 1, sizeof(bitmap), output) != sizeof(bitmap)) goto done;
    /* ICO DIBs contain bottom-up BGRA pixels followed by a 1-bit AND mask. */
    for (row = ICON_SIZE; row > 0; row--)
        if (fwrite(canvas + (row - 1) * PIXEL_STRIDE, 1, PIXEL_STRIDE, output) != PIXEL_STRIDE)
            goto done;
    for (row = ICON_SIZE; row > 0; row--) {
        UINT column;
        memset(mask, 0, sizeof(mask));
        for (column = 0; column < ICON_SIZE; column++)
            if (canvas[(row - 1) * PIXEL_STRIDE + column * 4 + 3] < 128)
                mask[column / 8] |= (unsigned char)(0x80u >> (column % 8));
        if (fwrite(mask, 1, sizeof(mask), output) != sizeof(mask)) goto done;
    }
    ok = 1;
done:
    if (output != NULL && fclose(output) != 0) ok = 0;
    if (!ok) remove(ico_path);
    free(canvas); free(pixels); free(wide_path);
    if (converter != NULL) IWICFormatConverter_Release(converter);
    if (scaler != NULL) IWICBitmapScaler_Release(scaler);
    if (frame != NULL) IWICBitmapFrameDecode_Release(frame);
    if (decoder != NULL) IWICBitmapDecoder_Release(decoder);
    if (factory != NULL) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return ok;
}

static char *windows_shortcut_icon(const char *project_root,
                                   const char *legacy_icon,
                                   char *error, size_t error_size) {
    char *identity = NULL;
    char *png = project_root == NULL
        ? zsharp_app_icon_default_path(error, error_size)
        : project_icon_path(project_root, legacy_icon, &identity, error, error_size);
    char *cache = zsharp_app_icon_default_path(error, error_size);
    char *ico = NULL;
    if (identity == NULL) identity = zsharp_app_desktop_id("zsharp-hub");
    if (png != NULL && cache != NULL && identity != NULL) {
        size_t size = strlen(cache) + strlen(identity) + 6;
        ico = (char *)malloc(size);
        if (ico != NULL) {
            snprintf(ico, size, "%s.%s.ico", cache, identity);
            if (!png_icon_file(png, ico)) { free(ico); ico = NULL; }
        }
    }
    if (ico == NULL)
        desktop_error(error, error_size, "could not prepare the Z# shortcut icon");
    free(identity);
    free(cache);
    free(png);
    return ico;
}

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    WCHAR executable[32768];
    WCHAR command[65536];
    if (!windows_executable(executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0])))) {
        desktop_error(error, error_size, "could not locate zsharp.exe");
        return 0;
    }
    swprintf(command, sizeof(command) / sizeof(command[0]),
             L"\"%ls\" open-desktop \"%%1\"", executable);
    if (!set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\.zapp", L"ZSharp.Application") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\.zgame", L"ZSharp.Game") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application", L"Z# Application") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application\\DefaultIcon",
            executable) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Application\\shell\\open\\command",
            command) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game", L"Z# Game") ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game\\DefaultIcon", executable) ||
        !set_registry_default(HKEY_CURRENT_USER,
            L"Software\\Classes\\ZSharp.Game\\shell\\open\\command",
            command)) {
        desktop_error(error, error_size,
                      "could not register Z# package associations for this user");
        return 0;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return 1;
}

int zsharp_desktop_create_hub_shortcut(char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR executable[32768];
    WCHAR shortcut_path[32768];
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT initialized;
    HRESULT result;
    char *icon_path = NULL;
    WCHAR *wide_icon = NULL;
    int ok = 0;
    if (!windows_desktop(desktop,
                         (DWORD)(sizeof(desktop) / sizeof(desktop[0]))) ||
        !windows_executable(executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0])))) {
        desktop_error(error, error_size,
                      "could not locate the Windows desktop or zsharp.exe");
        return 0;
    }
    swprintf(shortcut_path,
             sizeof(shortcut_path) / sizeof(shortcut_path[0]),
             L"%ls\\Z# Hub.lnk", desktop);
    icon_path = windows_shortcut_icon(NULL, NULL, error, error_size);
    wide_icon = icon_path == NULL ? NULL : wide_text(icon_path);
    if (wide_icon == NULL) { free(icon_path); return 0; }
    initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        desktop_error(error, error_size, "could not initialize shortcut support");
        free(wide_icon);
        free(icon_path);
        return 0;
    }
    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(result)) result = IShellLinkW_SetPath(link, executable);
    if (SUCCEEDED(result)) result = IShellLinkW_SetArguments(link, L"hub");
    if (SUCCEEDED(result))
        result = IShellLinkW_SetDescription(
            link, L"Open the Z# Hub for installed apps and games");
    if (SUCCEEDED(result))
        result = IShellLinkW_SetIconLocation(link, wide_icon, 0);
    if (SUCCEEDED(result))
        result = IShellLinkW_QueryInterface(link, &IID_IPersistFile,
                                            (void **)&persist);
    if (SUCCEEDED(result))
        result = IPersistFile_Save(persist, shortcut_path, TRUE);
    if (persist != NULL) IPersistFile_Release(persist);
    if (link != NULL) IShellLinkW_Release(link);
    if (SUCCEEDED(initialized)) CoUninitialize();
    free(wide_icon);
    free(icon_path);
    if (FAILED(result)) {
        desktop_error(error, error_size,
                      "could not create the Z# Hub desktop shortcut");
        return 0;
    }
    ok = 1;
    return ok;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
                                   char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR executable[32768];
    WCHAR package_absolute[32768];
    WCHAR arguments[65536];
    WCHAR shortcut_path[32768];
    WCHAR *wide_package = NULL;
    WCHAR *wide_name = NULL;
    WCHAR *wide_icon = NULL;
    char *name = NULL;
    char *icon_path = NULL;
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT initialized;
    HRESULT result;
    int ok = 0;
    if (!windows_desktop(desktop,
                         (DWORD)(sizeof(desktop) / sizeof(desktop[0]))) ||
        !windows_executable(executable,
                            (DWORD)(sizeof(executable) / sizeof(executable[0])))) {
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_package = wide_text(package_path);
    name = safe_name(display_name);
    wide_name = name == NULL ? NULL : wide_text(name);
    if (wide_package == NULL || wide_name == NULL ||
        GetFullPathNameW(wide_package,
                         (DWORD)(sizeof(package_absolute) /
                                 sizeof(package_absolute[0])),
                         package_absolute, NULL) == 0) {
        desktop_error(error, error_size, "could not resolve the shortcut paths");
        goto done;
    }
    swprintf(shortcut_path, sizeof(shortcut_path) / sizeof(shortcut_path[0]),
             L"%ls\\%ls.lnk", desktop, wide_name);
    swprintf(arguments, sizeof(arguments) / sizeof(arguments[0]),
             L"open-desktop \"%ls\"", package_absolute);
    initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        desktop_error(error, error_size, "could not initialize shortcut support");
        goto done;
    }
    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(result)) result = IShellLinkW_SetPath(link, executable);
    if (SUCCEEDED(result)) result = IShellLinkW_SetArguments(link, arguments);
    if (SUCCEEDED(result)) result = IShellLinkW_SetDescription(link, wide_name);
    icon_path = windows_shortcut_icon(project_root, icon_relative, error, error_size);
    wide_icon = icon_path == NULL ? NULL : wide_text(icon_path);
    if (wide_icon == NULL && SUCCEEDED(result)) result = E_FAIL;
    if (SUCCEEDED(result))
        result = IShellLinkW_SetIconLocation(
            link, wide_icon, 0);
    if (SUCCEEDED(result))
        result = IShellLinkW_QueryInterface(link, &IID_IPersistFile,
                                            (void **)&persist);
    if (SUCCEEDED(result)) result = IPersistFile_Save(persist, shortcut_path, TRUE);
    if (persist != NULL) IPersistFile_Release(persist);
    if (link != NULL) IShellLinkW_Release(link);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(result)) {
        desktop_error(error, error_size, "could not create the desktop shortcut");
        goto done;
    }
    ok = 1;
done:
    free(icon_path);
    free(wide_icon);
    free(wide_name);
    free(name);
    free(wide_package);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR shortcut_path[32768];
    WCHAR *wide_name;
    char *name = safe_name(display_name);
    DWORD attributes;
    if (name == NULL || !windows_desktop(
            desktop, (DWORD)(sizeof(desktop) / sizeof(desktop[0])))) {
        free(name);
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_name = wide_text(name);
    free(name);
    if (wide_name == NULL) return 0;
    swprintf(shortcut_path,
             sizeof(shortcut_path) / sizeof(shortcut_path[0]),
             L"%ls\\%ls.lnk", desktop, wide_name);
    free(wide_name);
    attributes = GetFileAttributesW(shortcut_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    WCHAR desktop[MAX_PATH];
    WCHAR path[32768];
    WCHAR *wide_name;
    char *name = safe_name(display_name);
    DWORD failure;
    if (name == NULL ||
        !windows_desktop(desktop,
                         (DWORD)(sizeof(desktop) / sizeof(desktop[0])))) {
        free(name);
        desktop_error(error, error_size, "could not locate the Windows desktop");
        return 0;
    }
    wide_name = wide_text(name);
    free(name);
    if (wide_name == NULL) {
        desktop_error(error, error_size, "could not resolve the shortcut name");
        return 0;
    }
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\%ls.lnk",
             desktop, wide_name);
    free(wide_name);
    if (DeleteFileW(path)) return 1;
    failure = GetLastError();
    if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND)
        return 1;
    desktop_error(error, error_size, "could not remove the desktop shortcut");
    return 0;
}

#else

static int make_directory(const char *path) {
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST;
}

static int make_directories(const char *path) {
    char *copy = copy_text(path);
    char *cursor;
    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (!make_directory(copy)) { free(copy); return 0; }
            *cursor = '/';
        }
    }
    if (!make_directory(copy)) { free(copy); return 0; }
    free(copy);
    return 1;
}

static char *executable_path(char *error, size_t error_size) {
#if defined(__linux__)
    size_t capacity = 1024;
    for (;;) {
        char *path = (char *)malloc(capacity);
        ssize_t length;
        if (path == NULL) return NULL;
        length = readlink("/proc/self/exe", path, capacity - 1);
        if (length >= 0 && (size_t)length < capacity - 1) {
            path[length] = '\0';
            return path;
        }
        free(path);
        if (capacity >= 65536) break;
        capacity *= 2;
    }
#else
    unsigned int capacity = 1024;
    for (;;) {
        char *path = (char *)malloc(capacity);
        unsigned int needed = capacity;
        if (path == NULL) return NULL;
        if (_NSGetExecutablePath(path, &needed) == 0) {
            char *resolved = realpath(path, NULL);
            free(path);
            return resolved;
        }
        free(path);
        capacity = needed;
        if (capacity > 65536u) break;
    }
#endif
    desktop_error(error, error_size, "could not locate the Z# executable");
    return NULL;
}

#if defined(__linux__)
static char *desktop_string_escape(const char *text);

static char *desktop_exec_quote(const char *text) {
    size_t length = 3;
    const char *cursor;
    char *result;
    char *output;
    for (cursor = text; *cursor != '\0'; cursor++)
        length += (*cursor == '\\' || *cursor == '\"' ||
                   *cursor == '`' || *cursor == '$' || *cursor == '%') ? 2 : 1;
    result = (char *)malloc(length);
    if (result == NULL) return NULL;
    output = result;
    *output++ = '\"';
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '%') *output++ = '%';
        else if (*cursor == '\\' || *cursor == '\"' ||
            *cursor == '`' || *cursor == '$') *output++ = '\\';
        *output++ = *cursor;
    }
    *output++ = '\"';
    *output = '\0';
    /* Desktop string escapes are decoded before Exec argument quoting. */
    {
        char *escaped = desktop_string_escape(result);
        free(result);
        return escaped;
    }
}

static char *desktop_string_escape(const char *text) {
    const unsigned char *cursor;
    char *result, *output;
    size_t size = 1;
    for (cursor = (const unsigned char *)text; *cursor != 0; cursor++)
        size += *cursor == '\\' || *cursor == '\n' ||
                *cursor == '\r' || *cursor == '\t' ? 2 : 1;
    result = (char *)malloc(size);
    if (result == NULL) return NULL;
    output = result;
    for (cursor = (const unsigned char *)text; *cursor != 0; cursor++) {
        if (*cursor == '\\' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
            *output++ = '\\';
            *output++ = *cursor == '\n' ? 'n' : *cursor == '\r' ? 'r' :
                        *cursor == '\t' ? 't' : '\\';
        } else *output++ = (char)*cursor;
    }
    *output = '\0';
    return result;
}
#endif

static int write_file(const char *path, const char *contents, int executable,
                      char *error, size_t error_size) {
    FILE *file;
    file = fopen(path, "wb");
    if (file == NULL) {
        desktop_error(error, error_size, "could not write '%s'", path);
        return 0;
    }
    if (fwrite(contents, 1, strlen(contents), file) != strlen(contents) ||
        fclose(file) != 0) {
        desktop_error(error, error_size, "could not write '%s'", path);
        return 0;
    }
    if (executable && chmod(path, 0755) != 0) {
        desktop_error(error, error_size, "could not make '%s' executable", path);
        return 0;
    }
    return 1;
}

static int run_process(const char *executable, char *const arguments[]) {
    pid_t child = fork();
    int status;
    if (child < 0) return 0;
    if (child == 0) {
        execv(executable, arguments);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#if defined(__linux__)

static char *linux_applications_directory(void) {
    const char *base = getenv("XDG_DATA_HOME");
    char *data, *applications;
    if (base != NULL && base[0] == '/')
        return join_path(base, "applications");
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0') return NULL;
    data = join_path(base, ".local/share");
    applications = data == NULL ? NULL : join_path(data, "applications");
    free(data);
    return applications;
}

static void trust_desktop_shortcut(const char *path) {
    pid_t child = fork();
    int status;
    if (child == 0) {
        /* GNOME otherwise displays a generated launcher as an untrusted text
         * document. Other desktops may not support this optional metadata. */
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execl("/usr/bin/gio", "gio", "set", path, "metadata::trusted", "true",
              (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

int zsharp_desktop_register_application(const char *display_name,
                                        const char *project_id,
                                        const char *entry_path, int entry_kind,
                                        const char *icon_path,
                                        char *error, size_t error_size) {
    static const char *const actions[] = {"run", "run-bytecode", "open-desktop", "hub"};
    char *applications = NULL, *identity = NULL, *filename = NULL, *path = NULL;
    char *executable = NULL, *quoted_executable = NULL, *entry = NULL, *quoted_entry = NULL;
    char *icon = NULL, *escaped_icon = NULL, *escaped_name = NULL, *contents = NULL;
    size_t size;
    int ok = 0;
    if (entry_kind < 0 || entry_kind > 3 ||
        (entry_kind != 3 && (entry_path == NULL || entry_path[0] == '\0'))) {
        desktop_error(error, error_size, "invalid Z# application launcher entry");
        return 0;
    }
    applications = linux_applications_directory();
    identity = zsharp_app_desktop_id(entry_kind == 3 ? "zsharp-hub" : project_id);
    executable = executable_path(error, error_size);
    quoted_executable = executable == NULL ? NULL : desktop_exec_quote(executable);
    if (entry_kind != 3) {
        entry = realpath(entry_path, NULL);
        quoted_entry = entry == NULL ? NULL : desktop_exec_quote(entry);
    }
    icon = zsharp_app_icon_resolve(NULL, icon_path, error, error_size);
    escaped_icon = icon == NULL ? NULL : desktop_string_escape(icon);
    escaped_name = desktop_string_escape(display_name == NULL ? "Z# Application" : display_name);
    filename = identity == NULL ? NULL : (char *)malloc(strlen(identity) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", identity);
    path = applications == NULL || filename == NULL ? NULL : join_path(applications, filename);
    if (applications == NULL || identity == NULL || executable == NULL ||
        quoted_executable == NULL || escaped_icon == NULL || escaped_name == NULL ||
        path == NULL || (entry_kind != 3 && quoted_entry == NULL) ||
        !make_directories(applications)) {
        desktop_error(error, error_size, "could not prepare the Z# application launcher");
        goto done;
    }
    size = strlen(escaped_name) + strlen(quoted_executable) + strlen(escaped_icon) +
           strlen(identity) + (quoted_entry == NULL ? 0 : strlen(quoted_entry)) + 320;
    contents = (char *)malloc(size);
    if (contents == NULL) {
        desktop_error(error, error_size, "out of memory");
        goto done;
    }
    snprintf(contents, size,
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s %s%s%s\n"
        "Terminal=false\nIcon=%s\nStartupWMClass=%s\nStartupNotify=true\nNoDisplay=false\n"
        "Categories=%s\n",
        escaped_name, quoted_executable, actions[entry_kind],
        quoted_entry == NULL ? "" : " ", quoted_entry == NULL ? "" : quoted_entry,
        escaped_icon, identity,
        entry_kind == 3 ? "Development;Utility;" : "Utility;");
    ok = write_file(path, contents, 1, error, error_size);
done:
    free(contents); free(path); free(filename); free(identity); free(applications);
    free(escaped_name); free(escaped_icon); free(icon);
    free(quoted_entry); free(entry); free(quoted_executable); free(executable);
    return ok;
}

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    char *data_root = NULL;
    char *executable = NULL;
    char *applications = NULL;
    char *mime_packages = NULL;
    char *desktop_file = NULL;
    char *mime_file = NULL;
    char *quoted_executable = NULL;
    char *launcher = NULL;
    char *icon = NULL;
    char *escaped_icon = NULL;
    const char *mime =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
        " <mime-type type=\"application/x-zsharp-app\"><comment>Z# Application</comment><glob pattern=\"*.zapp\"/></mime-type>\n"
        " <mime-type type=\"application/x-zsharp-game\"><comment>Z# Game</comment><glob pattern=\"*.zgame\"/></mime-type>\n"
        "</mime-info>\n";
    int ok = 0;
    if ((data_home == NULL || data_home[0] != '/') &&
        (home == NULL || home[0] == '\0')) {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    executable = executable_path(error, error_size);
    data_root = data_home != NULL && data_home[0] == '/'
        ? copy_text(data_home) : join_path(home, ".local/share");
    applications = data_root == NULL ? NULL : join_path(data_root, "applications");
    mime_packages = data_root == NULL ? NULL : join_path(data_root, "mime/packages");
    desktop_file = applications == NULL ? NULL
        : join_path(applications, "zsharp-package.desktop");
    mime_file = mime_packages == NULL ? NULL
        : join_path(mime_packages, "zsharp-package.xml");
    quoted_executable = executable == NULL ? NULL : desktop_exec_quote(executable);
    icon = zsharp_app_icon_default_path(error, error_size);
    escaped_icon = icon == NULL ? NULL : desktop_string_escape(icon);
    if (executable == NULL || applications == NULL || mime_packages == NULL ||
        desktop_file == NULL || mime_file == NULL || quoted_executable == NULL ||
        escaped_icon == NULL ||
        !make_directories(applications) || !make_directories(mime_packages)) {
        desktop_error(error, error_size, "could not prepare the user application directories");
        goto done;
    }
    launcher = (char *)malloc(strlen(quoted_executable) + strlen(escaped_icon) + 288);
    if (launcher == NULL) goto done;
    sprintf(launcher,
        "[Desktop Entry]\nType=Application\nName=Z# Package Launcher\n"
        "Exec=%s open %%f\nTerminal=false\nNoDisplay=true\nIcon=%s\n"
        "MimeType=application/x-zsharp-app;application/x-zsharp-game;\n",
        quoted_executable, escaped_icon);
    if (!write_file(desktop_file, launcher, 1, error, error_size) ||
        !write_file(mime_file, mime, 0, error, error_size)) goto done;
    {
        char *mime_args[] = {"xdg-mime", "install", "--mode", "user",
                             mime_file, NULL};
        char *app_args[] = {"xdg-mime", "default", "zsharp-package.desktop",
                            "application/x-zsharp-app", NULL};
        char *game_args[] = {"xdg-mime", "default", "zsharp-package.desktop",
                             "application/x-zsharp-game", NULL};
        /* These helpers are optional; desktop databases also refresh on login. */
        run_process("/usr/bin/xdg-mime", mime_args);
        run_process("/usr/bin/xdg-mime", app_args);
        run_process("/usr/bin/xdg-mime", game_args);
    }
    ok = 1;
done:
    free(launcher);
    free(escaped_icon);
    free(icon);
    free(data_root);
    free(quoted_executable);
    free(mime_file);
    free(desktop_file);
    free(mime_packages);
    free(applications);
    free(executable);
    return ok;
}

int zsharp_desktop_create_hub_shortcut(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *path = NULL;
    char *executable = NULL;
    char *quoted_executable = NULL;
    char *contents = NULL;
    char *icon = NULL;
    char *escaped_icon = NULL;
    char *identity = NULL;
    int ok = 0;
    if ((desktop_override == NULL || desktop_override[0] == '\0') &&
        (home == NULL || home[0] == '\0')) {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    path = desktop == NULL ? NULL : join_path(desktop, "Z# Hub.desktop");
    executable = executable_path(error, error_size);
    quoted_executable = executable == NULL
        ? NULL : desktop_exec_quote(executable);
    icon = zsharp_app_icon_default_path(error, error_size);
    escaped_icon = icon == NULL ? NULL : desktop_string_escape(icon);
    identity = zsharp_app_desktop_id("zsharp-hub");
    if (desktop == NULL || path == NULL || executable == NULL ||
        quoted_executable == NULL || escaped_icon == NULL || identity == NULL ||
        !make_directories(desktop)) {
        desktop_error(error, error_size,
                      "could not prepare the Z# Hub desktop shortcut");
        goto done;
    }
    if (!zsharp_desktop_register_application("Z# Hub", "zsharp-hub", NULL, 3,
                                             icon, error, error_size)) goto done;
    contents = (char *)malloc(strlen(quoted_executable) + strlen(escaped_icon) +
                               strlen(identity) + 288);
    if (contents == NULL) {
        desktop_error(error, error_size, "out of memory");
        goto done;
    }
    sprintf(contents,
        "[Desktop Entry]\nType=Application\nName=Z# Hub\n"
        "Comment=Open installed Z# apps and games\nExec=%s hub\n"
        "Terminal=false\nIcon=%s\nCategories=Development;Utility;\n"
        "StartupWMClass=%s\nStartupNotify=true\n",
        quoted_executable, escaped_icon, identity);
    ok = write_file(path, contents, 1, error, error_size);
    if (ok) trust_desktop_shortcut(path);
done:
    free(identity);
    free(escaped_icon);
    free(icon);
    free(contents);
    free(quoted_executable);
    free(executable);
    free(path);
    free(desktop);
    return ok;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *name = NULL;
    char *filename = NULL;
    char *path = NULL;
    char *executable = NULL;
    char *package_absolute = NULL;
    char *quoted_executable = NULL;
    char *quoted_package = NULL;
    char *icon = NULL;
    char *escaped_icon = NULL;
    char *identity = NULL;
    char *contents = NULL;
    int ok = 0;
    if ((desktop_override == NULL || desktop_override[0] == '\0') &&
        (home == NULL || home[0] == '\0')) {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL : join_path(desktop, filename);
    executable = executable_path(error, error_size);
    package_absolute = realpath(package_path, NULL);
    quoted_executable = executable == NULL ? NULL : desktop_exec_quote(executable);
    quoted_package = package_absolute == NULL ? NULL : desktop_exec_quote(package_absolute);
    icon = project_icon_path(project_root, icon_relative, &identity, error, error_size);
    escaped_icon = icon == NULL ? NULL : desktop_string_escape(icon);
    if (desktop == NULL || name == NULL || path == NULL || executable == NULL ||
        package_absolute == NULL || quoted_executable == NULL ||
        quoted_package == NULL || escaped_icon == NULL || identity == NULL ||
        !make_directories(desktop)) {
        desktop_error(error, error_size, "could not prepare the desktop shortcut");
        goto done;
    }
    contents = (char *)malloc(strlen(name) + strlen(quoted_executable) +
                              strlen(quoted_package) +
                              strlen(escaped_icon) + strlen(identity) + 288);
    if (contents == NULL) goto done;
    sprintf(contents,
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s open %s\n"
        "Terminal=false\nIcon=%s\nStartupWMClass=%s\nStartupNotify=true\nCategories=Utility;\n",
        name, quoted_executable, quoted_package, escaped_icon, identity);
    ok = write_file(path, contents, 1, error, error_size);
    if (ok) trust_desktop_shortcut(path);
done:
    free(contents);
    free(identity);
    free(escaped_icon);
    free(icon);
    free(quoted_package);
    free(quoted_executable);
    free(package_absolute);
    free(executable);
    free(path);
    free(filename);
    free(name);
    free(desktop);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *filename;
    char *path;
    int exists;
    if (home == NULL || home[0] == '\0') return 1;
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL
        : join_path(desktop, filename);
    exists = path != NULL && access(path, F_OK) == 0;
    free(path);
    free(filename);
    free(name);
    free(desktop);
    if (!exists) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *filename;
    char *path;
    int ok;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    filename = name == NULL ? NULL : (char *)malloc(strlen(name) + 9);
    if (filename != NULL) sprintf(filename, "%s.desktop", name);
    path = desktop == NULL || filename == NULL ? NULL
        : join_path(desktop, filename);
    if (path == NULL) {
        desktop_error(error, error_size, "could not resolve the desktop shortcut");
        free(filename);
        free(name);
        free(desktop);
        return 0;
    }
    ok = unlink(path) == 0 || errno == ENOENT;
    if (!ok)
        desktop_error(error, error_size,
                      "could not remove the desktop shortcut");
    free(path);
    free(filename);
    free(name);
    free(desktop);
    return ok;
}

#else

static char *apple_quote(const char *text) {
    size_t length = 3;
    const char *cursor;
    char *result;
    char *output;
    for (cursor = text; *cursor != '\0'; cursor++)
        length += (*cursor == '\\' || *cursor == '\"') ? 2 : 1;
    result = (char *)malloc(length);
    if (result == NULL) return NULL;
    output = result;
    *output++ = '\"';
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\\' || *cursor == '\"') *output++ = '\\';
        *output++ = *cursor;
    }
    *output++ = '\"';
    *output = '\0';
    return result;
}

static char *apple_script_text(const char *executable, const char *package) {
    char *quoted_executable = apple_quote(executable);
    char *quoted_package = package == NULL ? NULL : apple_quote(package);
    char *script;
    size_t size;
    if (quoted_executable == NULL || (package != NULL && quoted_package == NULL)) {
        free(quoted_executable); free(quoted_package); return NULL;
    }
    if (package == NULL) {
        size = strlen(quoted_executable) + 512;
        script = (char *)malloc(size);
        if (script != NULL) snprintf(script, size,
            "on open droppedItems\n repeat with itemRef in droppedItems\n"
            "  set commandText to quoted form of %s & \" open \" & quoted form of POSIX path of itemRef\n"
            "  do shell script commandText & \" >/dev/null 2>&1 &\"\n"
            " end repeat\nend open\n", quoted_executable);
    } else {
        size = strlen(quoted_executable) + strlen(quoted_package) + 384;
        script = (char *)malloc(size);
        if (script != NULL) snprintf(script, size,
            "on run\n set commandText to quoted form of %s & \" open \" & quoted form of %s\n"
            " do shell script commandText & \" >/dev/null 2>&1 &\"\n"
            "end run\n", quoted_executable, quoted_package);
    }
    free(quoted_executable);
    free(quoted_package);
    return script;
}

static int compile_applet(const char *app_path, const char *script,
                          char *error, size_t error_size) {
    const char *home = getenv("HOME");
    char *cache = home == NULL ? NULL : join_path(home, ".cache/zsharp");
    char *source = cache == NULL ? NULL : join_path(cache, "launcher.applescript");
    int ok = 0;
    if (cache == NULL || source == NULL || !make_directories(cache) ||
        !write_file(source, script, 0, error, error_size)) goto done;
    {
        char *arguments[] = {"osacompile", "-o", (char *)app_path, source, NULL};
        if (!run_process("/usr/bin/osacompile", arguments)) {
            desktop_error(error, error_size, "could not create the macOS app shortcut");
            goto done;
        }
    }
    ok = 1;
done:
    free(source);
    free(cache);
    return ok;
}

static int remove_applet_tree(const char *path);

static int apply_applet_icon(const char *app, const char *project_root,
                             const char *legacy_icon,
                             char *error, size_t error_size) {
    char *source = project_root == NULL
        ? zsharp_app_icon_default_path(error, error_size)
        : project_icon_path(project_root, legacy_icon, NULL, error, error_size);
    char *target = join_path(app, "Contents/Resources/applet.icns");
    char *normalized = join_path(app, "Contents/Resources/.zsharp-icon-512.png");
    int ok = 0;
    if (source != NULL && target != NULL && normalized != NULL) {
        char *resize[] = {"sips", "-s", "format", "png",
                          "--resampleHeightWidth", "512", "512",
                          source, "--out", normalized, NULL};
        char *convert[] = {"sips", "-s", "format", "icns",
                           normalized, "--out", target, NULL};
        /* ICNS requires supported square dimensions; project PNGs and the
         * original official image need not already have those dimensions. */
        ok = run_process("/usr/bin/sips", resize) &&
             run_process("/usr/bin/sips", convert);
    }
    if (normalized != NULL) remove(normalized);
    if (!ok) desktop_error(error, error_size, "could not prepare the macOS shortcut icon");
    free(normalized);
    free(target);
    free(source);
    return ok;
}

int zsharp_desktop_create_hub_shortcut(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *app = NULL;
    char *executable = NULL;
    char *quoted_executable = NULL;
    char *script = NULL;
    size_t size;
    int ok = 0;
    if ((desktop_override == NULL || desktop_override[0] == '\0') &&
        (home == NULL || home[0] == '\0')) {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    app = desktop == NULL ? NULL : join_path(desktop, "Z# Hub.app");
    executable = executable_path(error, error_size);
    quoted_executable = executable == NULL ? NULL : apple_quote(executable);
    if (desktop == NULL || app == NULL || executable == NULL ||
        quoted_executable == NULL || !make_directories(desktop)) {
        desktop_error(error, error_size,
                      "could not prepare the Z# Hub desktop shortcut");
        goto done;
    }
    size = strlen(quoted_executable) + 256;
    script = (char *)malloc(size);
    if (script == NULL) {
        desktop_error(error, error_size, "out of memory");
        goto done;
    }
    snprintf(script, size,
        "on run\n set commandText to quoted form of %s & \" hub\"\n"
        " do shell script commandText & \" >/dev/null 2>&1 &\"\nend run\n",
        quoted_executable);
    if (!remove_applet_tree(app)) {
        desktop_error(error, error_size,
                      "could not replace the Z# Hub desktop shortcut");
        goto done;
    }
    ok = compile_applet(app, script, error, error_size) &&
         apply_applet_icon(app, NULL, NULL, error, error_size);
done:
    free(script);
    free(quoted_executable);
    free(executable);
    free(app);
    free(desktop);
    return ok;
}

int zsharp_desktop_install_associations(char *error, size_t error_size) {
    const char *home = getenv("HOME");
    char *applications = NULL;
    char *app = NULL;
    char *info = NULL;
    char *executable = NULL;
    char *script = NULL;
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    applications = join_path(home, "Applications");
    app = applications == NULL ? NULL
        : join_path(applications, "ZSharp Package Launcher.app");
    executable = executable_path(error, error_size);
    script = executable == NULL ? NULL : apple_script_text(executable, NULL);
    if (applications == NULL || app == NULL || executable == NULL ||
        script == NULL || !make_directories(applications) ||
        !compile_applet(app, script, error, error_size) ||
        !apply_applet_icon(app, NULL, NULL, error, error_size)) goto done;
    info = join_path(app, "Contents/Info.plist");
    if (info == NULL) goto done;
    {
        const char *buddy = "/usr/libexec/PlistBuddy";
        char *commands[] = {
            "Add :CFBundleDocumentTypes array",
            "Add :CFBundleDocumentTypes:0 dict",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeName string ZSharp Package",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Viewer",
            "Add :CFBundleDocumentTypes:0:LSHandlerRank string Owner",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions array",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:0 string zapp",
            "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:1 string zgame"
        };
        size_t index;
        for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
            char *arguments[] = {"PlistBuddy", "-c", commands[index], info, NULL};
            run_process(buddy, arguments);
        }
    }
    {
        const char *lsregister =
            "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
        char *arguments[] = {"lsregister", "-f", app, NULL};
        run_process(lsregister, arguments);
    }
    ok = 1;
done:
    free(script);
    free(executable);
    free(info);
    free(app);
    free(applications);
    return ok;
}

int zsharp_desktop_create_shortcut(const char *display_name,
                                   const char *package_path,
                                   const char *project_root,
                                   const char *icon_relative,
    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop = NULL;
    char *name = NULL;
    char *bundle_name = NULL;
    char *app = NULL;
    char *executable = NULL;
    char *package_absolute = NULL;
    char *script = NULL;
    int ok = 0;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle_name = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle_name != NULL) sprintf(bundle_name, "%s.app", name);
    app = desktop == NULL || bundle_name == NULL ? NULL
        : join_path(desktop, bundle_name);
    executable = executable_path(error, error_size);
    package_absolute = realpath(package_path, NULL);
    script = executable == NULL || package_absolute == NULL ? NULL
        : apple_script_text(executable, package_absolute);
    if (desktop == NULL || app == NULL || executable == NULL ||
        package_absolute == NULL || script == NULL || !make_directories(desktop)) {
        desktop_error(error, error_size, "could not prepare the macOS desktop shortcut");
        goto done;
    }
    ok = compile_applet(app, script, error, error_size) &&
         apply_applet_icon(app, project_root, icon_relative, error, error_size);
done:
    free(script);
    free(package_absolute);
    free(executable);
    free(app);
    free(bundle_name);
    free(name);
    free(desktop);
    return ok;
}

int zsharp_desktop_refresh_shortcut(const char *display_name,
                                    const char *package_path,
                                    const char *project_root,
                                    const char *icon_relative,
                                    char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *bundle;
    char *path;
    int exists;
    if (home == NULL || home[0] == '\0') return 1;
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle != NULL) sprintf(bundle, "%s.app", name);
    path = desktop == NULL || bundle == NULL ? NULL : join_path(desktop, bundle);
    exists = path != NULL && access(path, F_OK) == 0;
    free(path);
    free(bundle);
    free(name);
    free(desktop);
    if (!exists) return 1;
    return zsharp_desktop_create_shortcut(
        display_name, package_path, project_root, icon_relative,
        error, error_size);
}

static int remove_applet_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode)) return unlink(path) == 0;
    {
        DIR *directory = opendir(path);
        struct dirent *entry;
        int ok = directory != NULL;
        if (directory == NULL) return 0;
        while (ok && (entry = readdir(directory)) != NULL) {
            char *child;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) continue;
            child = join_path(path, entry->d_name);
            if (child == NULL || !remove_applet_tree(child)) ok = 0;
            free(child);
        }
        if (closedir(directory) != 0) ok = 0;
        if (ok && rmdir(path) != 0) ok = 0;
        return ok;
    }
}

int zsharp_desktop_remove_shortcut(const char *display_name,
                                   char *error, size_t error_size) {
    const char *home = getenv("HOME");
    const char *desktop_override = getenv("ZSHARP_DESKTOP_DIRECTORY");
    char *desktop;
    char *name;
    char *bundle;
    char *path;
    int ok;
    if (home == NULL || home[0] == '\0') {
        desktop_error(error, error_size, "HOME is not available");
        return 0;
    }
    desktop = desktop_override != NULL && desktop_override[0] != '\0'
        ? copy_text(desktop_override) : join_path(home, "Desktop");
    name = safe_name(display_name);
    bundle = name == NULL ? NULL : (char *)malloc(strlen(name) + 5);
    if (bundle != NULL) sprintf(bundle, "%s.app", name);
    path = desktop == NULL || bundle == NULL ? NULL : join_path(desktop, bundle);
    if (path == NULL) {
        desktop_error(error, error_size, "could not resolve the desktop shortcut");
        free(bundle);
        free(name);
        free(desktop);
        return 0;
    }
    ok = remove_applet_tree(path);
    if (!ok)
        desktop_error(error, error_size,
                      "could not remove the desktop shortcut");
    free(path);
    free(bundle);
    free(name);
    free(desktop);
    return ok;
}

#endif
#endif

#if !defined(__linux__)
int zsharp_desktop_register_application(const char *display_name,
                                        const char *project_id,
                                        const char *entry_path, int entry_kind,
                                        const char *icon_path,
                                        char *error, size_t error_size) {
    (void)display_name; (void)project_id; (void)entry_path; (void)entry_kind;
    (void)icon_path; (void)error; (void)error_size;
    return 1;
}
#endif
