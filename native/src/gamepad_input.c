#include "gamepad_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum GamepadField {
    FIELD_NONE, FIELD_CONNECTED, FIELD_RUMBLE, FIELD_NAME, FIELD_TYPE,
    FIELD_HELD, FIELD_PRESSED, FIELD_RELEASED, FIELD_LABEL,
    FIELD_X, FIELD_Y, FIELD_ANGLE, FIELD_MAGNITUDE, FIELD_VALUE
} GamepadField;

typedef struct GamepadPath {
    GamepadField field;
    int button;
    int hand;
} GamepadPath;

static int gamepad_path(const char *path, GamepadPath *result) {
    static const struct { const char *name; int id; } controls[] = {
        {"south", ZGP_SOUTH}, {"east", ZGP_EAST},
        {"west", ZGP_WEST}, {"north", ZGP_NORTH},
        {"dpad.up", ZGP_DPAD_UP}, {"dpad.down", ZGP_DPAD_DOWN},
        {"dpad.left", ZGP_DPAD_LEFT}, {"dpad.right", ZGP_DPAD_RIGHT},
        {"left.stick", ZGP_LEFT_STICK}, {"right.stick", ZGP_RIGHT_STICK},
        {"left.trigger", ZGP_LEFT_TRIGGER}, {"right.trigger", ZGP_RIGHT_TRIGGER},
        {"left.bumper", ZGP_LEFT_BUMPER}, {"right.bumper", ZGP_RIGHT_BUMPER},
        {"start", ZGP_START}, {"back", ZGP_BACK}, {"guide", ZGP_GUIDE},
        {"misc1", ZGP_MISC1}, {"misc2", ZGP_MISC2}, {"misc3", ZGP_MISC3},
        {"misc4", ZGP_MISC4}, {"misc5", ZGP_MISC5}, {"misc6", ZGP_MISC6},
        {"left.paddle1", ZGP_LEFT_PADDLE1}, {"right.paddle1", ZGP_RIGHT_PADDLE1},
        {"left.paddle2", ZGP_LEFT_PADDLE2}, {"right.paddle2", ZGP_RIGHT_PADDLE2},
        {"touchpad", ZGP_TOUCHPAD}
    };
    size_t i;
    if (!path || !result || strncmp(path, "Gamepad.", 8)) return 0;
    memset(result, 0, sizeof(*result));
    result->button = result->hand = -1;
    path += 8;
    if (!strcmp(path, "connected")) result->field = FIELD_CONNECTED;
    else if (!strcmp(path, "rumbleAvailable")) result->field = FIELD_RUMBLE;
    else if (!strcmp(path, "name")) result->field = FIELD_NAME;
    else if (!strcmp(path, "type")) result->field = FIELD_TYPE;
    else for (i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        size_t n = strlen(controls[i].name);
        const char *field;
        if (strncmp(path, controls[i].name, n) || path[n] != '.') continue;
        field = path + n + 1;
        result->button = controls[i].id;
        if (!strcmp(field, "held")) result->field = FIELD_HELD;
        else if (!strcmp(field, "pressed")) result->field = FIELD_PRESSED;
        else if (!strcmp(field, "released")) result->field = FIELD_RELEASED;
        else if (!strcmp(field, "label") && controls[i].id <= ZGP_NORTH)
            result->field = FIELD_LABEL;
        else if (controls[i].id == ZGP_LEFT_STICK || controls[i].id == ZGP_RIGHT_STICK) {
            result->hand = controls[i].id == ZGP_LEFT_STICK ? 0 : 1;
            if (!strcmp(field, "x")) result->field = FIELD_X;
            else if (!strcmp(field, "y")) result->field = FIELD_Y;
            else if (!strcmp(field, "angle")) result->field = FIELD_ANGLE;
            else if (!strcmp(field, "magnitude")) result->field = FIELD_MAGNITUDE;
        } else if (controls[i].id == ZGP_LEFT_TRIGGER || controls[i].id == ZGP_RIGHT_TRIGGER) {
            result->hand = controls[i].id == ZGP_LEFT_TRIGGER ? 0 : 1;
            if (!strcmp(field, "value")) result->field = FIELD_VALUE;
        }
        return result->field != FIELD_NONE;
    }
    return result->field != FIELD_NONE;
}

void zsharp_gamepad_init(ZSharpGamepadState *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->generation = 1;
    snprintf(state->type, sizeof(state->type), "none");
}

void zsharp_gamepad_button(ZSharpGamepadState *state, int button, int held) {
    if (!state || button < 0 || button >= ZGP_BUTTONS) return;
    held = !!held && state->connected;
    if (state->held[button] == held) return;
    state->held[button] = held;
    if (held) state->pressed[button]++;
    else state->released[button]++;
}

void zsharp_gamepad_reset(ZSharpGamepadState *state, int disconnect) {
    int i;
    if (!state) return;
    memcpy(state->press_floor, state->pressed, sizeof(state->press_floor));
    memcpy(state->release_floor, state->released, sizeof(state->release_floor));
    state->generation++;
    for (i = 0; i < ZGP_BUTTONS; i++) zsharp_gamepad_button(state, i, 0);
    memset(state->stick, 0, sizeof(state->stick));
    memset(state->trigger, 0, sizeof(state->trigger));
    state->vibration = state->vibration_ms = 0;
    state->vibration_serial++;
    if (disconnect) {
        state->connected = state->rumble_available = 0;
        state->name[0] = 0;
        memset(state->labels, 0, sizeof(state->labels));
        snprintf(state->type, sizeof(state->type), "none");
    }
}

void zsharp_gamepad_connect(ZSharpGamepadState *state, const char *name,
                           const char *type, int rumble_available) {
    if (!state) return;
    if (state->connected) zsharp_gamepad_reset(state, 1);
    state->connected = 1;
    state->rumble_available = !!rumble_available;
    memset(state->labels, 0, sizeof(state->labels));
    snprintf(state->name, sizeof(state->name), "%s", name ? name : "");
    if (!type || (strcmp(type, "xbox") && strcmp(type, "playstation") &&
                  strcmp(type, "nintendo") && strcmp(type, "generic"))) type = "generic";
    snprintf(state->type, sizeof(state->type), "%s", type);
}

void zsharp_gamepad_stick(ZSharpGamepadState *state, int hand,
                         int16_t raw_x, int16_t raw_y) {
    double x, y, length, scaled;
    if (!state || hand < 0 || hand > 1) return;
    x = raw_x / (raw_x < 0 ? 32768.0 : 32767.0);
    y = -raw_y / (raw_y < 0 ? 32768.0 : 32767.0);
    length = hypot(x, y);
    if (!state->connected || length <= 0.15) {
        state->stick[hand][0] = state->stick[hand][1] = 0;
        return;
    }
    scaled = (fmin(1.0, length) - 0.15) / 0.85;
    state->stick[hand][0] = (float)(x / length * scaled);
    state->stick[hand][1] = (float)(y / length * scaled);
}

void zsharp_gamepad_trigger(ZSharpGamepadState *state, int hand, int16_t raw) {
    float value;
    if (!state || hand < 0 || hand > 1) return;
    value = !state->connected || raw < 0 ? 0 : raw / 32767.0f;
    state->trigger[hand] = value;
    zsharp_gamepad_button(state, hand == 0 ? ZGP_LEFT_TRIGGER : ZGP_RIGHT_TRIGGER,
                          value >= 0.5f);
}

void zsharp_gamepad_cursor_begin(ZSharpGamepadCursor *cursor,
                                const ZSharpGamepadState *state) {
    if (!cursor || !state) return;
    cursor->generation = state->generation;
    memcpy(cursor->pressed, state->pressed, sizeof(cursor->pressed));
    memcpy(cursor->released, state->released, sizeof(cursor->released));
}

int zsharp_gamepad_property(const char *path, int writing) {
    GamepadPath parsed;
    if (writing) return path && !strcmp(path, "Gamepad.vibrate");
    return gamepad_path(path, &parsed);
}

static const char *button_label(const ZSharpGamepadState *state, int button) {
    static const char *generic[] = {"South", "East", "West", "North"};
    static const char *xbox[] = {"A", "B", "X", "Y"};
    static const char *playstation[] = {"Cross", "Circle", "Square", "Triangle"};
    static const char *nintendo[] = {"B", "A", "Y", "X"};
    if (state->labels[button][0]) return state->labels[button];
    if (!strcmp(state->type, "xbox")) return xbox[button];
    if (!strcmp(state->type, "playstation")) return playstation[button];
    if (!strcmp(state->type, "nintendo")) return nintendo[button];
    return generic[button];
}

int zsharp_gamepad_read(const ZSharpGamepadState *state,
                       ZSharpGamepadCursor *cursor, const char *path,
                       char *output, size_t capacity, int *kind) {
    GamepadPath parsed;
    double value = 0;
    const char *text = NULL;
    int result_kind = ZSHARP_GAMEPAD_NUMBER;
    if (!state || !output || !capacity || !kind || !gamepad_path(path, &parsed)) return 0;
    switch (parsed.field) {
    case FIELD_CONNECTED: value = !!state->connected; result_kind = ZSHARP_GAMEPAD_STATUS; break;
    case FIELD_RUMBLE: value = state->connected && state->rumble_available; result_kind = ZSHARP_GAMEPAD_STATUS; break;
    case FIELD_NAME: text = state->name; break;
    case FIELD_TYPE: text = state->type; break;
    case FIELD_LABEL: text = button_label(state, parsed.button); break;
    case FIELD_HELD: value = state->held[parsed.button]; result_kind = ZSHARP_GAMEPAD_STATUS; break;
    case FIELD_PRESSED:
    case FIELD_RELEASED: {
        uint64_t *seen;
        uint64_t count;
        if (!cursor) return 0;
        if (cursor->generation != state->generation) {
            memcpy(cursor->pressed, state->press_floor, sizeof(cursor->pressed));
            memcpy(cursor->released, state->release_floor, sizeof(cursor->released));
            cursor->generation = state->generation;
        }
        seen = parsed.field == FIELD_PRESSED ? &cursor->pressed[parsed.button] : &cursor->released[parsed.button];
        count = parsed.field == FIELD_PRESSED ? state->pressed[parsed.button] : state->released[parsed.button];
        value = *seen < count;
        if (value) (*seen)++;
        result_kind = ZSHARP_GAMEPAD_STATUS;
        break;
    }
    case FIELD_X: value = state->stick[parsed.hand][0]; break;
    case FIELD_Y: value = state->stick[parsed.hand][1]; break;
    case FIELD_MAGNITUDE:
        value = fmin(1.0, hypot(state->stick[parsed.hand][0], state->stick[parsed.hand][1])); break;
    case FIELD_ANGLE:
        if (state->stick[parsed.hand][0] || state->stick[parsed.hand][1]) {
            value = atan2(state->stick[parsed.hand][0], state->stick[parsed.hand][1]) * (180.0 / 3.14159265358979323846);
            if (value < 0) value += 360.0;
        }
        break;
    case FIELD_VALUE: value = state->trigger[parsed.hand]; break;
    default: return 0;
    }
    if (text) {
        *kind = ZSHARP_GAMEPAD_TEXT;
        snprintf(output, capacity, "%s", text);
    } else if (result_kind == ZSHARP_GAMEPAD_STATUS) {
        *kind = result_kind;
        snprintf(output, capacity, "%s", value ? "alive" : "dead");
    } else {
        *kind = result_kind;
        snprintf(output, capacity, "%.9f", isfinite(value) ? value : 0.0);
    }
    return 1;
}

int zsharp_gamepad_write(ZSharpGamepadState *state, const char *path,
                        const char *value, char *error, size_t capacity) {
    char *end;
    double strength, duration;
    if (!state || !zsharp_gamepad_property(path, 1)) {
        if (error && capacity) snprintf(error, capacity, "Gamepad property is read-only or unknown: %s", path ? path : "(null)");
        return 0;
    }
    if (!value) value = "";
    strength = strtod(value, &end);
    if (end == value || *end != '|') {
        if (error && capacity) snprintf(error, capacity, "Gamepad vibration requires strength and duration");
        return 0;
    }
    value = end + 1;
    duration = strtod(value, &end);
    if (end == value || *end || !isfinite(strength) || !isfinite(duration) ||
        strength < 0 || strength > 100 || duration < 0 || duration > 10000) {
        if (error && capacity) snprintf(error, capacity, "Gamepad vibration strength must be 0..100 and duration 0..10000ms");
        return 0;
    }
    state->vibration = state->connected && state->rumble_available ? (float)(strength / 100.0) : 0;
    state->vibration_ms = state->connected && state->rumble_available ? (float)duration : 0;
    state->vibration_serial++;
    return 1;
}
