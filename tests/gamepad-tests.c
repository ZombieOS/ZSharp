#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gamepad_input.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static double read_number(const ZSharpGamepadState *state, const char *path) {
    char output[128];
    int kind = -1;
    assert(zsharp_gamepad_read(state, NULL, path, output, sizeof(output), &kind));
    assert(kind == ZSHARP_GAMEPAD_NUMBER);
    {
        double value = 0;
        assert(sscanf(output, "%lf", &value) == 1);
        assert(isfinite(value));
        return value;
    }
}

static int read_status(const ZSharpGamepadState *state, ZSharpGamepadCursor *cursor,
                       const char *path) {
    char output[128];
    int kind = -1;
    assert(zsharp_gamepad_read(state, cursor, path, output, sizeof(output), &kind));
    assert(kind == ZSHARP_GAMEPAD_STATUS);
    assert(!strcmp(output, "alive") || !strcmp(output, "dead"));
    return !strcmp(output, "alive");
}

static void read_text(const ZSharpGamepadState *state, const char *path,
                      const char *expected) {
    char output[128];
    int kind = -1;
    assert(zsharp_gamepad_read(state, NULL, path, output, sizeof(output), &kind));
    assert(kind == ZSHARP_GAMEPAD_TEXT);
    assert(!strcmp(output, expected));
}

static void test_paths(void) {
    static const char *controls[] = {
        "south", "east", "west", "north",
        "dpad.up", "dpad.down", "dpad.left", "dpad.right",
        "left.stick", "right.stick", "left.trigger", "right.trigger",
        "left.bumper", "right.bumper", "start", "back", "guide",
        "misc1", "misc2", "misc3", "misc4", "misc5", "misc6",
        "left.paddle1", "right.paddle1", "left.paddle2", "right.paddle2", "touchpad"
    };
    static const char *invalid[] = {
        "", "Gamepad", "Gamepad.", "Gamepad.a.pressed", "Gamepad.y.held",
        "Gamepad.LEFT.stick.x", "Gamepad.left.stick.X", "Gamepad.left.stick.value",
        "Gamepad.left.trigger.x", "Gamepad.south.x", "Gamepad.left.bumper.value",
        "Gamepad.dpad", "Gamepad.left.stick.x.more", "Gamepad.vibrate",
        "input.controller.connected", "Gamepad.left.paddle3.held"
    };
    size_t i;
    for (i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), "Gamepad.%s.held", controls[i]);
        assert(zsharp_gamepad_property(path, 0));
        assert(!zsharp_gamepad_property(path, 1));
        snprintf(path, sizeof(path), "Gamepad.%s.pressed", controls[i]);
        assert(zsharp_gamepad_property(path, 0));
        snprintf(path, sizeof(path), "Gamepad.%s.released", controls[i]);
        assert(zsharp_gamepad_property(path, 0));
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!zsharp_gamepad_property(invalid[i], 0));
    assert(!zsharp_gamepad_property(NULL, 0));
    assert(!zsharp_gamepad_property(NULL, 1));
    assert(zsharp_gamepad_property("Gamepad.vibrate", 1));
    assert(zsharp_gamepad_property("Gamepad.left.stick.angle", 0));
    assert(zsharp_gamepad_property("Gamepad.right.stick.magnitude", 0));
}

static void test_edges(void) {
    ZSharpGamepadState state;
    ZSharpGamepadCursor first, second, late;
    int button;
    zsharp_gamepad_init(&state);
    assert(!read_status(&state, NULL, "Gamepad.connected"));
    read_text(&state, "Gamepad.type", "none");
    read_text(&state, "Gamepad.name", "");
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    assert(!state.held[ZGP_SOUTH]);
    zsharp_gamepad_connect(&state, "Test pad", "xbox", 1);
    zsharp_gamepad_cursor_begin(&first, &state);
    zsharp_gamepad_cursor_begin(&second, &state);
    /* Both edges of short taps survive even if no task ran while held. */
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    zsharp_gamepad_button(&state, ZGP_SOUTH, 0);
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    zsharp_gamepad_button(&state, ZGP_SOUTH, 0);
    assert(state.pressed[ZGP_SOUTH] == 2);
    assert(state.released[ZGP_SOUTH] == 2);
    assert(!read_status(&state, NULL, "Gamepad.south.held"));
    assert(read_status(&state, &first, "Gamepad.south.pressed"));
    assert(read_status(&state, &first, "Gamepad.south.pressed"));
    assert(!read_status(&state, &first, "Gamepad.south.pressed"));
    assert(read_status(&state, &first, "Gamepad.south.released"));
    assert(read_status(&state, &first, "Gamepad.south.released"));
    assert(!read_status(&state, &first, "Gamepad.south.released"));
    assert(read_status(&state, &second, "Gamepad.south.pressed"));
    assert(read_status(&state, &second, "Gamepad.south.pressed"));
    zsharp_gamepad_cursor_begin(&late, &state);
    assert(!read_status(&state, &late, "Gamepad.south.pressed"));
    assert(!read_status(&state, &late, "Gamepad.south.released"));
    for (button = 0; button < ZGP_BUTTONS; button++) {
        uint64_t before = state.pressed[button];
        zsharp_gamepad_button(&state, button, 1);
        assert(state.held[button]);
        assert(state.pressed[button] == before + 1);
        zsharp_gamepad_button(&state, button, 0);
        assert(!state.held[button]);
    }
    zsharp_gamepad_button(&state, -1, 1);
    zsharp_gamepad_button(&state, ZGP_BUTTONS, 1);
}

static void test_lifecycle(void) {
    ZSharpGamepadState state;
    ZSharpGamepadCursor cursor, new_task;
    zsharp_gamepad_init(&state);
    zsharp_gamepad_connect(&state, "Pad", "xbox", 1);
    zsharp_gamepad_cursor_begin(&cursor, &state);
    zsharp_gamepad_button(&state, ZGP_NORTH, 1);
    zsharp_gamepad_stick(&state, 0, 20000, -20000);
    zsharp_gamepad_trigger(&state, 1, 32767);
    /* Focus loss must clear all held state, axes and stale queued presses. */
    zsharp_gamepad_reset(&state, 0);
    assert(read_status(&state, NULL, "Gamepad.connected"));
    assert(read_number(&state, "Gamepad.left.stick.magnitude") == 0);
    assert(read_number(&state, "Gamepad.right.trigger.value") == 0);
    assert(!read_status(&state, NULL, "Gamepad.north.held"));
    assert(!read_status(&state, &cursor, "Gamepad.north.pressed"));
    assert(read_status(&state, &cursor, "Gamepad.north.released"));
    assert(!read_status(&state, &cursor, "Gamepad.north.released"));
    assert(read_status(&state, &cursor, "Gamepad.right.trigger.released"));
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    zsharp_gamepad_reset(&state, 1);
    assert(!read_status(&state, NULL, "Gamepad.connected"));
    assert(!read_status(&state, NULL, "Gamepad.rumbleAvailable"));
    assert(read_status(&state, &cursor, "Gamepad.south.released"));
    assert(!read_status(&state, &cursor, "Gamepad.south.pressed"));
    zsharp_gamepad_connect(&state, "Other pad", "nintendo", 0);
    assert(!read_status(&state, &cursor, "Gamepad.south.pressed"));
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    assert(read_status(&state, &cursor, "Gamepad.south.pressed"));
    zsharp_gamepad_cursor_begin(&new_task, &state);
    assert(!read_status(&state, &new_task, "Gamepad.south.pressed"));
    assert(read_status(&state, NULL, "Gamepad.south.held"));
    /* A reconnect before the old task reads cannot replay the old press,
       but a genuinely new post-reconnect press is still delivered. */
    zsharp_gamepad_reset(&state, 1);
    zsharp_gamepad_connect(&state, "Third pad", "generic", 0);
    zsharp_gamepad_button(&state, ZGP_SOUTH, 1);
    assert(read_status(&state, &cursor, "Gamepad.south.pressed"));
    assert(!read_status(&state, &cursor, "Gamepad.south.pressed"));
    assert(read_status(&state, &cursor, "Gamepad.south.released"));
}

static void test_axes(void) {
    ZSharpGamepadState state;
    ZSharpGamepadCursor cursor;
    const int16_t raw[] = {0, 1, -1, 4900, -4900, 16384, -16384, INT16_MIN, INT16_MAX};
    size_t x, y;
    zsharp_gamepad_init(&state);
    zsharp_gamepad_connect(&state, "Pad", "generic", 0);
    zsharp_gamepad_cursor_begin(&cursor, &state);
    zsharp_gamepad_stick(&state, 0, 4900, 0);
    assert(read_number(&state, "Gamepad.left.stick.magnitude") == 0);
    zsharp_gamepad_stick(&state, 0, 0, INT16_MIN);
    assert(read_number(&state, "Gamepad.left.stick.x") == 0);
    assert(read_number(&state, "Gamepad.left.stick.y") == 1);
    assert(read_number(&state, "Gamepad.left.stick.angle") == 0);
    zsharp_gamepad_stick(&state, 0, INT16_MAX, 0);
    assert(read_number(&state, "Gamepad.left.stick.angle") == 90);
    zsharp_gamepad_stick(&state, 0, 0, INT16_MAX);
    assert(read_number(&state, "Gamepad.left.stick.angle") == 180);
    zsharp_gamepad_stick(&state, 0, INT16_MIN, 0);
    assert(read_number(&state, "Gamepad.left.stick.angle") == 270);
    zsharp_gamepad_stick(&state, 1, INT16_MAX, INT16_MIN);
    assert(fabs(read_number(&state, "Gamepad.right.stick.angle") - 45) < 0.00001);
    assert(read_number(&state, "Gamepad.right.stick.magnitude") <= 1);
    for (x = 0; x < sizeof(raw) / sizeof(raw[0]); x++)
        for (y = 0; y < sizeof(raw) / sizeof(raw[0]); y++) {
            double a, b, magnitude;
            zsharp_gamepad_stick(&state, 0, raw[x], raw[y]);
            a = read_number(&state, "Gamepad.left.stick.x");
            b = read_number(&state, "Gamepad.left.stick.y");
            magnitude = read_number(&state, "Gamepad.left.stick.magnitude");
            assert(fabs(a) <= 1 && fabs(b) <= 1 && magnitude <= 1);
            assert(hypot(a, b) <= 1.0000001);
        }
    zsharp_gamepad_trigger(&state, 0, INT16_MIN);
    assert(read_number(&state, "Gamepad.left.trigger.value") == 0);
    zsharp_gamepad_trigger(&state, 0, 16383);
    assert(!read_status(&state, NULL, "Gamepad.left.trigger.held"));
    zsharp_gamepad_trigger(&state, 0, 16384);
    assert(read_status(&state, NULL, "Gamepad.left.trigger.held"));
    assert(read_status(&state, &cursor, "Gamepad.left.trigger.pressed"));
    zsharp_gamepad_trigger(&state, 0, INT16_MAX);
    assert(read_number(&state, "Gamepad.left.trigger.value") == 1);
    assert(!read_status(&state, &cursor, "Gamepad.left.trigger.pressed"));
    zsharp_gamepad_trigger(&state, 0, 0);
    assert(read_status(&state, &cursor, "Gamepad.left.trigger.released"));
    zsharp_gamepad_reset(&state, 1);
    zsharp_gamepad_stick(&state, 0, INT16_MAX, INT16_MIN);
    zsharp_gamepad_trigger(&state, 0, INT16_MAX);
    assert(read_number(&state, "Gamepad.left.stick.magnitude") == 0);
    assert(read_number(&state, "Gamepad.left.trigger.value") == 0);
}

static void test_labels_and_vibration(void) {
    ZSharpGamepadState state;
    char error[256], output[8];
    int kind;
    static const char *invalid[] = {"", "50", "|1", "50|", "-1|10", "101|10", "50|-1",
        "50|10001", "nan|10", "50|nan", "inf|10", "50|inf", "50|10junk", "50|1|2"};
    size_t i;
    unsigned before;
    zsharp_gamepad_init(&state);
    zsharp_gamepad_connect(&state, "Xbox", "xbox", 1);
    read_text(&state, "Gamepad.south.label", "A");
    read_text(&state, "Gamepad.north.label", "Y");
    assert(read_status(&state, NULL, "Gamepad.rumbleAvailable"));
    before = state.vibration_serial;
    assert(zsharp_gamepad_write(&state, "Gamepad.vibrate", "75|200", error, sizeof(error)));
    assert(state.vibration == 0.75f && state.vibration_ms == 200);
    assert(state.vibration_serial == before + 1);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        before = state.vibration_serial;
        assert(!zsharp_gamepad_write(&state, "Gamepad.vibrate", invalid[i], error, sizeof(error)));
        assert(state.vibration_serial == before);
        assert(error[0]);
    }
    assert(!zsharp_gamepad_write(&state, "Gamepad.left.stick.x", "1", error, sizeof(error)));
    assert(zsharp_gamepad_write(&state, "Gamepad.vibrate", "100|10000", error, sizeof(error)));
    assert(state.vibration == 1 && state.vibration_ms == 10000);
    assert(zsharp_gamepad_write(&state, "Gamepad.vibrate", "0|0", error, sizeof(error)));
    assert(state.vibration == 0 && state.vibration_ms == 0);
    zsharp_gamepad_connect(&state, "Switch", "nintendo", 0);
    read_text(&state, "Gamepad.south.label", "B");
    read_text(&state, "Gamepad.east.label", "A");
    read_text(&state, "Gamepad.west.label", "Y");
    read_text(&state, "Gamepad.north.label", "X");
    assert(zsharp_gamepad_write(&state, "Gamepad.vibrate", "50|100", error, sizeof(error)));
    assert(state.vibration == 0 && state.vibration_ms == 0);
    zsharp_gamepad_connect(&state, "DualSense", "playstation", 1);
    read_text(&state, "Gamepad.south.label", "Cross");
    read_text(&state, "Gamepad.north.label", "Triangle");
    snprintf(state.labels[ZGP_SOUTH], sizeof(state.labels[0]), "B");
    read_text(&state, "Gamepad.south.label", "B");
    zsharp_gamepad_connect(&state, "Unknown", "unrecognized", 0);
    read_text(&state, "Gamepad.type", "generic");
    read_text(&state, "Gamepad.south.label", "South");
    zsharp_gamepad_reset(&state, 1);
    assert(zsharp_gamepad_write(&state, "Gamepad.vibrate", "100|1000", error, sizeof(error)));
    assert(state.vibration == 0 && state.vibration_ms == 0);
    assert(!zsharp_gamepad_read(&state, NULL, "Gamepad.south.pressed", output, sizeof(output), &kind));
    assert(!zsharp_gamepad_read(&state, NULL, "Gamepad.connected", output, 0, &kind));
    assert(!zsharp_gamepad_read(&state, NULL, "Gamepad.connected", output, sizeof(output), NULL));
}

int main(void) {
    test_paths();
    test_edges();
    test_lifecycle();
    test_axes();
    test_labels_and_vibration();
    puts("Gamepad input tests passed.");
    return 0;
}
