#include "xr_hands.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const joint_names[ZXR_HAND_JOINTS] = {
    "palm", "wrist", "thumbMetacarpal", "thumbProximal", "thumbDistal", "thumbTip",
    "indexMetacarpal", "indexProximal", "indexIntermediate", "indexDistal", "indexTip",
    "middleMetacarpal", "middleProximal", "middleIntermediate", "middleDistal", "middleTip",
    "ringMetacarpal", "ringProximal", "ringIntermediate", "ringDistal", "ringTip",
    "littleMetacarpal", "littleProximal", "littleIntermediate", "littleDistal", "littleTip"
};

enum { HAND_INVALID, HAND_TRACKED, HAND_PINCH_VALUE, HAND_PINCH_HELD,
       HAND_PINCH_PRESSED, HAND_PINCH_RELEASED, HAND_JOINT_TRACKED,
       HAND_JOINT_POSITION, HAND_JOINT_ROTATION };

typedef struct HandPath { int field, joint, axis; } HandPath;

static int parse_path(const char *path, HandPath *out) {
    int joint;
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (!strcmp(path, "tracked")) out->field = HAND_TRACKED;
    else if (!strcmp(path, "pinch.value")) out->field = HAND_PINCH_VALUE;
    else if (!strcmp(path, "pinch.held")) out->field = HAND_PINCH_HELD;
    else if (!strcmp(path, "pinch.pressed")) out->field = HAND_PINCH_PRESSED;
    else if (!strcmp(path, "pinch.released")) out->field = HAND_PINCH_RELEASED;
    else for (joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
        size_t size = strlen(joint_names[joint]);
        const char *field;
        if (strncmp(path, joint_names[joint], size) || path[size] != '.') continue;
        field = path + size + 1;
        out->joint = joint;
        if (!strcmp(field, "tracked")) out->field = HAND_JOINT_TRACKED;
        else if (!strncmp(field, "position", 8) && strlen(field) == 9 &&
                 field[8] >= 'X' && field[8] <= 'Z') {
            out->field = HAND_JOINT_POSITION;
            out->axis = field[8] - 'X';
        } else if (!strncmp(field, "rotation", 8) && strlen(field) == 9 &&
                   field[8] >= 'X' && field[8] <= 'Z') {
            out->field = HAND_JOINT_ROTATION;
            out->axis = field[8] - 'X';
        }
        break;
    }
    return out->field != HAND_INVALID;
}

static int finite_joint(const ZSharpXRHandJoint *joint) {
    int axis;
    if (!joint->tracked || !isfinite(joint->radius) || joint->radius < 0) return 0;
    for (axis = 0; axis < 3; axis++)
        if (!isfinite(joint->position[axis]) || !isfinite(joint->rotation[axis])) return 0;
    return 1;
}

static void set_pinch(ZSharpXRTrackedHand *hand, int held) {
    if (hand->pinch_held == held) return;
    hand->pinch_held = held;
    if (held) hand->pinch_pressed++;
    else hand->pinch_released++;
}

void zsharp_xr_hand_update(ZSharpXRTrackedHand *hand,
                           const ZSharpXRHandJoint joints[ZXR_HAND_JOINTS],
                           int tracked, int focused) {
    int joint, eligible, tracked_joints = 0;
    double distance;
    if (!hand) return;
    hand->tracked = !!tracked && !!focused && joints != NULL;
    for (joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
        if (hand->tracked && finite_joint(&joints[joint])) {
            hand->joints[joint] = joints[joint];
            tracked_joints++;
        }
        else hand->joints[joint].tracked = 0;
    }
    hand->tracked = hand->tracked && tracked_joints > 0;
    eligible = hand->tracked && hand->joints[ZXR_HAND_THUMB_TIP].tracked &&
               hand->joints[ZXR_HAND_INDEX_TIP].tracked;
    if (!eligible) {
        if (hand->pinch_tracking) {
            hand->press_floor = hand->pinch_pressed;
            hand->release_floor = hand->pinch_released;
            hand->generation++;
        }
        hand->pinch_tracking = hand->pinch_armed = 0;
        hand->pinch_value = 0;
        set_pinch(hand, 0);
        return;
    }
    hand->pinch_tracking = 1;
    {
        const float *thumb = hand->joints[ZXR_HAND_THUMB_TIP].position;
        const float *index = hand->joints[ZXR_HAND_INDEX_TIP].position;
        double x = (double)thumb[0] - index[0];
        double y = (double)thumb[1] - index[1];
        double z = (double)thumb[2] - index[2];
        distance = sqrt(x*x + y*y + z*z);
    }
    hand->pinch_value = (float)fmax(0, fmin(1, 1 - distance / (double)0.06f));
    if (distance >= (double)0.04f) {
        set_pinch(hand, 0);
        hand->pinch_armed = 1;
    } else if (distance <= (double)0.02f && hand->pinch_armed) set_pinch(hand, 1);
}

void zsharp_xr_hand_cursor_begin(ZSharpXRHandCursor *cursor,
                                 const ZSharpXRTrackedHand *hand) {
    if (!cursor || !hand) return;
    cursor->pressed = hand->pinch_pressed;
    cursor->released = hand->pinch_released;
    cursor->generation = hand->generation;
}

int zsharp_xr_hand_property(const char *path) {
    HandPath parsed;
    return parse_path(path, &parsed);
}

int zsharp_xr_hand_read(const ZSharpXRTrackedHand *hand,
                        ZSharpXRHandCursor *cursor, const char *path,
                        char *output, size_t capacity, int *status) {
    HandPath parsed;
    double value = 0;
    if (!hand || !output || !capacity || !status || !parse_path(path, &parsed)) return 0;
    *status = 1;
    switch (parsed.field) {
    case HAND_TRACKED: value = hand->tracked; break;
    case HAND_PINCH_HELD: value = hand->pinch_held; break;
    case HAND_PINCH_VALUE: value = hand->pinch_value; *status = 0; break;
    case HAND_PINCH_PRESSED:
    case HAND_PINCH_RELEASED: {
        uint64_t *seen, count;
        if (!cursor) return 0;
        if (cursor->generation != hand->generation) {
            cursor->generation = hand->generation;
            cursor->pressed = hand->press_floor;
            cursor->released = hand->release_floor;
        }
        seen = parsed.field == HAND_PINCH_PRESSED ? &cursor->pressed : &cursor->released;
        count = parsed.field == HAND_PINCH_PRESSED ? hand->pinch_pressed : hand->pinch_released;
        value = *seen < count;
        if (value) (*seen)++;
        break;
    }
    case HAND_JOINT_TRACKED: value = hand->joints[parsed.joint].tracked; break;
    case HAND_JOINT_POSITION: value = hand->joints[parsed.joint].position[parsed.axis]; *status = 0; break;
    case HAND_JOINT_ROTATION: value = hand->joints[parsed.joint].rotation[parsed.axis]; *status = 0; break;
    default: return 0;
    }
    if (*status) snprintf(output, capacity, "%s", value ? "alive" : "dead");
    else snprintf(output, capacity, "%.9f", isfinite(value) ? value : 0);
    return 1;
}
