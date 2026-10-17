#ifndef ZSHARP_GAMEPAD_INPUT_H
#define ZSHARP_GAMEPAD_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard physical positions, independent of a controller's printed labels. */
enum {
    ZGP_SOUTH, ZGP_EAST, ZGP_WEST, ZGP_NORTH,
    ZGP_DPAD_UP, ZGP_DPAD_DOWN, ZGP_DPAD_LEFT, ZGP_DPAD_RIGHT,
    ZGP_LEFT_STICK, ZGP_RIGHT_STICK, ZGP_LEFT_TRIGGER, ZGP_RIGHT_TRIGGER,
    ZGP_LEFT_BUMPER, ZGP_RIGHT_BUMPER, ZGP_START, ZGP_BACK, ZGP_GUIDE,
    ZGP_MISC1, ZGP_MISC2, ZGP_MISC3, ZGP_MISC4, ZGP_MISC5, ZGP_MISC6,
    ZGP_LEFT_PADDLE1, ZGP_RIGHT_PADDLE1, ZGP_LEFT_PADDLE2, ZGP_RIGHT_PADDLE2,
    ZGP_TOUCHPAD, ZGP_BUTTONS
};

enum {
    ZSHARP_GAMEPAD_NUMBER = 0,
    ZSHARP_GAMEPAD_STATUS = 1,
    ZSHARP_GAMEPAD_TEXT = 2
};

typedef struct ZSharpGamepadState {
    int connected;
    int rumble_available;
    char name[128];
    char type[32];
    /* Optional physical face labels supplied by the backend; otherwise the
       family defaults are used. Order: south, east, west, north. */
    char labels[4][16];
    float stick[2][2]; /* Left/right, x positive right, y positive forward. */
    float trigger[2];
    int held[ZGP_BUTTONS];
    uint64_t pressed[ZGP_BUTTONS], released[ZGP_BUTTONS];
    /* Lifecycle floors discard stale presses without losing new transitions. */
    uint64_t generation;
    uint64_t press_floor[ZGP_BUTTONS], release_floor[ZGP_BUTTONS];
    float vibration, vibration_ms;
    unsigned vibration_serial;
} ZSharpGamepadState;

typedef struct ZSharpGamepadCursor {
    uint64_t generation;
    uint64_t pressed[ZGP_BUTTONS], released[ZGP_BUTTONS];
} ZSharpGamepadCursor;

void zsharp_gamepad_init(ZSharpGamepadState *state);
void zsharp_gamepad_connect(ZSharpGamepadState *state, const char *name,
                           const char *type, int rumble_available);
/* Call once per focus/disconnection transition, not once per idle frame.
   disconnect==0 keeps the selected device but clears all live input. */
void zsharp_gamepad_reset(ZSharpGamepadState *state, int disconnect);
void zsharp_gamepad_button(ZSharpGamepadState *state, int button, int held);
void zsharp_gamepad_stick(ZSharpGamepadState *state, int hand,
                         int16_t raw_x, int16_t raw_y);
void zsharp_gamepad_trigger(ZSharpGamepadState *state, int hand, int16_t raw);
void zsharp_gamepad_cursor_begin(ZSharpGamepadCursor *cursor,
                                const ZSharpGamepadState *state);
int zsharp_gamepad_property(const char *path, int writing);
int zsharp_gamepad_read(const ZSharpGamepadState *state,
                       ZSharpGamepadCursor *cursor, const char *path,
                       char *output, size_t capacity, int *kind);
/* Internal write path used by Gamepad.vibrate(strength, duration). The value
   is the finite numeric strength and milliseconds separated by '|'. */
int zsharp_gamepad_write(ZSharpGamepadState *state, const char *path,
                        const char *value, char *error, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
