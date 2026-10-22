#ifndef ZSHARP_DESKTOP_INPUT_H
#define ZSHARP_DESKTOP_INPUT_H

#include <stddef.h>
#include <string.h>

/* Pure input validation, shared by all desktop backends. Parsing never causes
 * focus changes, clicks or keystrokes. Regions are centers of a 3-by-3 grid. */
typedef struct DesktopInputRegion { int row, column; } DesktopInputRegion;
enum {
    DESKTOP_INPUT_CTRL = 1, DESKTOP_INPUT_ALT = 2,
    DESKTOP_INPUT_SHIFT = 4, DESKTOP_INPUT_SUPER = 8
};
enum {
    DESKTOP_KEY_F1 = 256, DESKTOP_KEY_F24 = DESKTOP_KEY_F1 + 23,
    DESKTOP_KEY_ENTER = 300, DESKTOP_KEY_TAB, DESKTOP_KEY_SPACE,
    DESKTOP_KEY_ESCAPE, DESKTOP_KEY_BACKSPACE, DESKTOP_KEY_DELETE,
    DESKTOP_KEY_INSERT, DESKTOP_KEY_HOME, DESKTOP_KEY_END,
    DESKTOP_KEY_PAGEUP, DESKTOP_KEY_PAGEDOWN, DESKTOP_KEY_LEFT,
    DESKTOP_KEY_RIGHT, DESKTOP_KEY_UP, DESKTOP_KEY_DOWN,
    DESKTOP_KEY_MEDIA_PLAY_PAUSE, DESKTOP_KEY_MEDIA_NEXT,
    DESKTOP_KEY_MEDIA_PREVIOUS
};
typedef struct DesktopInputChord { unsigned modifiers; int key; } DesktopInputChord;

static inline int desktop_input_media_key(int key) {
    return key >= DESKTOP_KEY_MEDIA_PLAY_PAUSE && key <= DESKTOP_KEY_MEDIA_PREVIOUS;
}

static inline int desktop_input_upper(int c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static inline int desktop_input_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (desktop_input_upper((unsigned char)*a++) !=
            desktop_input_upper((unsigned char)*b++)) return 0;
    }
    return !*a && !*b;
}

static inline int desktop_input_region(const char *text, DesktopInputRegion *out) {
    static const char *const names[] = {
        "topleft", "topmiddle", "topright", "middleleft", "center",
        "middleright", "bottomleft", "bottommiddle", "bottomright"
    };
    size_t i;
    if (!out) return 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (desktop_input_equal(text, names[i])) {
            out->row = (int)(i / 3); out->column = (int)(i % 3); return 1;
        }
    }
    return 0;
}

static inline int desktop_input_chord(const char *text, DesktopInputChord *out) {
    static const struct { const char *name; int key; } keys[] = {
        {"ENTER", DESKTOP_KEY_ENTER}, {"RETURN", DESKTOP_KEY_ENTER},
        {"TAB", DESKTOP_KEY_TAB}, {"SPACE", DESKTOP_KEY_SPACE},
        {"ESC", DESKTOP_KEY_ESCAPE}, {"ESCAPE", DESKTOP_KEY_ESCAPE},
        {"BACKSPACE", DESKTOP_KEY_BACKSPACE}, {"DELETE", DESKTOP_KEY_DELETE},
        {"DEL", DESKTOP_KEY_DELETE}, {"INSERT", DESKTOP_KEY_INSERT},
        {"INS", DESKTOP_KEY_INSERT}, {"HOME", DESKTOP_KEY_HOME},
        {"END", DESKTOP_KEY_END}, {"PAGEUP", DESKTOP_KEY_PAGEUP},
        {"PGUP", DESKTOP_KEY_PAGEUP}, {"PAGEDOWN", DESKTOP_KEY_PAGEDOWN},
        {"PGDN", DESKTOP_KEY_PAGEDOWN}, {"LEFT", DESKTOP_KEY_LEFT},
        {"RIGHT", DESKTOP_KEY_RIGHT}, {"UP", DESKTOP_KEY_UP},
        {"DOWN", DESKTOP_KEY_DOWN},
        {"MEDIA_PLAY_PAUSE", DESKTOP_KEY_MEDIA_PLAY_PAUSE},
        {"MEDIA_NEXT", DESKTOP_KEY_MEDIA_NEXT},
        {"MEDIA_PREVIOUS", DESKTOP_KEY_MEDIA_PREVIOUS}
    };
    DesktopInputChord result = {0, 0};
    const char *p = text;
    size_t length = 0;
    if (!text || !out) return 0;
    while (text[length]) if (++length > 128) return 0;
    if (!length) return 0;
    for (;;) {
        char token[32];
        const char *start, *end;
        size_t n, i;
        unsigned modifier = 0;
        while (*p == ' ') p++;
        start = p;
        while (*p && *p != '+') p++;
        end = p;
        while (end > start && end[-1] == ' ') end--;
        n = (size_t)(end - start);
        if (!n || n >= sizeof(token)) return 0;
        for (i = 0; i < n; i++) token[i] = (char)desktop_input_upper((unsigned char)start[i]);
        token[n] = '\0';
        if (!strcmp(token, "CTRL") || !strcmp(token, "CONTROL")) modifier = DESKTOP_INPUT_CTRL;
        else if (!strcmp(token, "ALT") || !strcmp(token, "OPTION")) modifier = DESKTOP_INPUT_ALT;
        else if (!strcmp(token, "SHIFT")) modifier = DESKTOP_INPUT_SHIFT;
        else if (!strcmp(token, "WIN") || !strcmp(token, "SUPER") ||
                 !strcmp(token, "META") || !strcmp(token, "CMD") ||
                 !strcmp(token, "COMMAND")) modifier = DESKTOP_INPUT_SUPER;
        if (modifier) {
            if (result.key || (result.modifiers & modifier)) return 0;
            result.modifiers |= modifier;
        } else {
            int key = 0;
            if (result.key) return 0;
            if (n == 1 && ((token[0] >= 'A' && token[0] <= 'Z') ||
                           (token[0] >= '0' && token[0] <= '9'))) key = token[0];
            else if (token[0] == 'F' && n >= 2 && n <= 3) {
                unsigned number = 0;
                if (token[1] == '0') return 0;
                for (i = 1; i < n; i++) {
                    if (token[i] < '0' || token[i] > '9') return 0;
                    number = number * 10 + (unsigned)(token[i] - '0');
                }
                if (number >= 1 && number <= 24) key = DESKTOP_KEY_F1 + (int)number - 1;
            } else for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                if (!strcmp(token, keys[i].name)) { key = keys[i].key; break; }
            }
            if (!key) return 0;
            result.key = key;
        }
        if (!*p) break;
        p++;
    }
    /* Consumer/media keys are individual system actions, not modified
     * character shortcuts. Keep this rule identical on every backend. */
    if (!result.key || (result.modifiers && desktop_input_media_key(result.key))) return 0;
    *out = result; return 1;
}

/* Text is at most 64 KiB of strict UTF-8. Newlines and tabs are text input,
 * not shortcut syntax. Other ASCII control characters are rejected. */
static inline int desktop_input_text_valid(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0, i = 0;
    if (!text) return 0;
    while (p[n]) if (++n > 65536) return 0;
    while (i < n) {
        unsigned c = p[i++], value, extra, j;
        if (c < 128) {
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; extra = 1; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; extra = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; extra = 3; }
        else return 0;
        if (extra > n - i) return 0;
        for (j = 0; j < extra; j++) {
            c = p[i++];
            if ((c & 0xc0) != 0x80) return 0;
            value = (value << 6) | (c & 63);
        }
        if ((extra == 1 && value < 0x80) || (extra == 2 && value < 0x800) ||
            (extra == 3 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}

#endif
