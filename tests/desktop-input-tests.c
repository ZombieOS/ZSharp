#include "desktop_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;

static void require(int ok, const char *description) {
    if (!ok) {
        fprintf(stderr, "desktop input parser fixture failed: %s\n", description);
        exit(1);
    }
    assertions++;
}

int main(void) {
    static const char *const regions[] = {
        "topleft", "topmiddle", "topright", "middleleft", "center",
        "middleright", "bottomleft", "bottommiddle", "bottomright"
    };
    static const char *const invalid_regions[] = {
        "", "top", "middle", "top-left", "top left", "center ", " center", "all", "0"
    };
    static const struct { const char *text; unsigned modifiers; int key; } chords[] = {
        {"A", 0, 'A'}, {"z", 0, 'Z'}, {"0", 0, '0'},
        {"Ctrl+A", DESKTOP_INPUT_CTRL, 'A'},
        {" control + shift + z ", DESKTOP_INPUT_CTRL | DESKTOP_INPUT_SHIFT, 'Z'},
        {"Alt+F4", DESKTOP_INPUT_ALT, DESKTOP_KEY_F1 + 3},
        {"OPTION+tab", DESKTOP_INPUT_ALT, DESKTOP_KEY_TAB},
        {"super+Return", DESKTOP_INPUT_SUPER, DESKTOP_KEY_ENTER},
        {"WIN+E", DESKTOP_INPUT_SUPER, 'E'},
        {"META+Home", DESKTOP_INPUT_SUPER, DESKTOP_KEY_HOME},
        {"CMD+END", DESKTOP_INPUT_SUPER, DESKTOP_KEY_END},
        {"Command+Left", DESKTOP_INPUT_SUPER, DESKTOP_KEY_LEFT},
        {"Ctrl+Alt+Shift+Win+F24", 15, DESKTOP_KEY_F24},
        {"F1", 0, DESKTOP_KEY_F1}, {"f24", 0, DESKTOP_KEY_F24},
        {"space", 0, DESKTOP_KEY_SPACE}, {"escape", 0, DESKTOP_KEY_ESCAPE},
        {"esc", 0, DESKTOP_KEY_ESCAPE}, {"Backspace", 0, DESKTOP_KEY_BACKSPACE},
        {"Del", 0, DESKTOP_KEY_DELETE}, {"Delete", 0, DESKTOP_KEY_DELETE},
        {"Ins", 0, DESKTOP_KEY_INSERT}, {"Insert", 0, DESKTOP_KEY_INSERT},
        {"PgUp", 0, DESKTOP_KEY_PAGEUP}, {"PageUp", 0, DESKTOP_KEY_PAGEUP},
        {"PgDn", 0, DESKTOP_KEY_PAGEDOWN}, {"PageDown", 0, DESKTOP_KEY_PAGEDOWN},
        {"Right", 0, DESKTOP_KEY_RIGHT}, {"Up", 0, DESKTOP_KEY_UP},
        {"Down", 0, DESKTOP_KEY_DOWN},
        {"MEDIA_PLAY_PAUSE", 0, DESKTOP_KEY_MEDIA_PLAY_PAUSE},
        {"media_next", 0, DESKTOP_KEY_MEDIA_NEXT},
        {" Media_Previous ", 0, DESKTOP_KEY_MEDIA_PREVIOUS}
    };
    static const char *const invalid_chords[] = {
        "", " ", "+A", "A+", "Ctrl++A", "Ctrl", "Shift+Alt", "Ctrl+Control+A",
        "Alt+Option+A", "Win+Command+A", "A+B", "A+Ctrl", "Ctrl+A+Shift",
        "F0", "F00", "F01", "F25", "F99", "F100", "Fx", "Ctrl+Unknown",
        "Ctrl+!", "Ctrl+\\", "Ctrl+\tA", "Ctrl+\nA", "ControlPanel", "A;B",
        "CTRL+MEDIA_PLAY_PAUSE", "ALT+MEDIA_NEXT", "SHIFT+MEDIA_PREVIOUS",
        "WIN+MEDIA_NEXT", "MEDIA_PLAY_PAUSE+MEDIA_NEXT", "MEDIA_PLAY", "MEDIA_PREV"
    };
    static const char *const invalid_text[] = {
        "\x01", "\x08", "\x1b", "\x7f", "\x80", "\xc0\xaf", "\xc1\xbf",
        "\xc2", "\xc2\x20", "\xe0\x80\xaf", "\xe2\x82", "\xed\xa0\x80",
        "\xed\xbf\xbf", "\xf0\x80\x80\xaf", "\xf0\x9f\x98", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xff"
    };
    DesktopInputRegion region;
    DesktopInputChord chord;
    char *long_text;
    size_t i;

    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        require(desktop_input_region(regions[i], &region) &&
                region.row == (int)(i / 3) && region.column == (int)(i % 3),
                "3-by-3 region maps to its expected cell");
    require(desktop_input_region("BoTToMRiGhT", &region) && region.row == 2 && region.column == 2,
            "region names are case insensitive");
    for (i = 0; i < sizeof(invalid_regions) / sizeof(invalid_regions[0]); i++) {
        region.row = 9; region.column = 8;
        require(!desktop_input_region(invalid_regions[i], &region) &&
                region.row == 9 && region.column == 8, "invalid region never changes output");
    }
    require(!desktop_input_region(NULL, &region) && !desktop_input_region("center", NULL),
            "region parser rejects null arguments");
    for (i = 0; i < sizeof(chords) / sizeof(chords[0]); i++)
        require(desktop_input_chord(chords[i].text, &chord) &&
                chord.modifiers == chords[i].modifiers && chord.key == chords[i].key,
                "shortcut aliases, case, modifiers and base key are normalized");
    for (i = 0; i < sizeof(invalid_chords) / sizeof(invalid_chords[0]); i++) {
        chord.modifiers = 99; chord.key = -1;
        require(!desktop_input_chord(invalid_chords[i], &chord) &&
                chord.modifiers == 99 && chord.key == -1,
                "malformed shortcut never changes output");
    }
    require(!desktop_input_chord(NULL, &chord) && !desktop_input_chord("A", NULL),
            "shortcut parser rejects null arguments");
    require(desktop_input_media_key(DESKTOP_KEY_MEDIA_PLAY_PAUSE) &&
            desktop_input_media_key(DESKTOP_KEY_MEDIA_NEXT) &&
            desktop_input_media_key(DESKTOP_KEY_MEDIA_PREVIOUS) &&
            !desktop_input_media_key(DESKTOP_KEY_DOWN) && !desktop_input_media_key('S'),
            "consumer keys have a distinct, bounded classification");
    require(desktop_input_text_valid("") && desktop_input_text_valid("plain text + literal ^!#{}"),
            "empty/plain text is not shortcut syntax");
    require(desktop_input_text_valid("first\r\nsecond\n\tthird\r"),
            "text allows CR, LF and tab");
    require(desktop_input_text_valid("Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"),
            "strict UTF-8 includes two-, three- and four-byte code points");
    require(desktop_input_text_valid("\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
                                     "\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"),
            "UTF-8 boundary code points remain valid");
    for (i = 0; i < sizeof(invalid_text) / sizeof(invalid_text[0]); i++)
        require(!desktop_input_text_valid(invalid_text[i]), "malformed UTF-8 or forbidden controls rejected");
    require(!desktop_input_text_valid(NULL), "text validator rejects null");
    long_text = (char *)malloc(65538);
    require(long_text != NULL, "allocate bounded validation fixture");
    memset(long_text, 'A', 65537); long_text[65537] = '\0';
    require(!desktop_input_text_valid(long_text), "text exceeding 64 KiB rejected");
    long_text[65536] = '\0';
    require(desktop_input_text_valid(long_text), "exactly 64 KiB text accepted");
    memset(long_text, 'A', 129); long_text[129] = '\0';
    require(!desktop_input_chord(long_text, &chord), "shortcut length is bounded");
    free(long_text);
    printf("desktop input parser fixtures: %u assertions passed; no desktop actions\n", assertions);
    return 0;
}
