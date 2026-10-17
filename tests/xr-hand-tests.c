#ifdef NDEBUG
#undef NDEBUG
#endif
#include "xr_input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int status_read(ZSharpXRState *state, ZSharpXRCursor *cursor, const char *path) {
    char output[128];
    int status = 0;
    assert(zsharp_xr_read(state, cursor, path, output, sizeof(output), &status));
    assert(status);
    assert(!strcmp(output, "alive") || !strcmp(output, "dead"));
    return !strcmp(output, "alive");
}

static double number_read(ZSharpXRState *state, ZSharpXRCursor *cursor, const char *path) {
    char output[128];
    int status = 1;
    assert(zsharp_xr_read(state, cursor, path, output, sizeof(output), &status));
    assert(!status);
    assert(isfinite(atof(output)));
    return atof(output);
}

static void tips(ZSharpXRHandJoint joints[ZXR_HAND_JOINTS], float distance) {
    memset(joints, 0, ZXR_HAND_JOINTS * sizeof(*joints));
    for (int joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
        joints[joint].tracked = 1;
        joints[joint].radius = 0.005f;
    }
    joints[ZXR_HAND_INDEX_TIP].position[0] = distance;
}

static void test_properties(void) {
    static const char *names[ZXR_HAND_JOINTS] = {
        "palm", "wrist", "thumbMetacarpal", "thumbProximal", "thumbDistal", "thumbTip",
        "indexMetacarpal", "indexProximal", "indexIntermediate", "indexDistal", "indexTip",
        "middleMetacarpal", "middleProximal", "middleIntermediate", "middleDistal", "middleTip",
        "ringMetacarpal", "ringProximal", "ringIntermediate", "ringDistal", "ringTip",
        "littleMetacarpal", "littleProximal", "littleIntermediate", "littleDistal", "littleTip"
    };
    static const char *invalid[] = {
        "XR.hand.available", "XR.hands.tracked", "XR.left.hand", "XR.left.hand.pinch",
        "XR.left.hand.pinch.angle", "XR.left.hand.indexTip.x", "XR.left.hand.indexTip.positionW",
        "XR.left.hand.indexTip.rotationx", "XR.left.hand.indexTip.radius", "XR.left.hand.IndexTip.tracked",
        "XR.left.hand.thumbIntermediate.positionX", "XR.head.hand.tracked",
        "XR.left.hand.indexTip.tracked.extra", "XR.passthrough", "XR.passthrough.unsupported"
    };
    ZSharpXRState state = {0};
    ZSharpXRCursor cursor = {0};
    ZSharpXRHandJoint joints[ZXR_HAND_JOINTS];
    char path[128], output[128];
    int status;
    assert(zsharp_xr_property("XR.hands.available", 0));
    assert(!zsharp_xr_property("XR.hands.available", 1));
    assert(!status_read(&state, &cursor, "XR.hands.available"));
    state.hand_tracking_available = 1;
    assert(status_read(&state, &cursor, "XR.hands.available"));
    tips(joints, 0.06f);
    for (int joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
        for (int axis = 0; axis < 3; axis++) {
            joints[joint].position[axis] = joint * 10.0f + axis;
            joints[joint].rotation[axis] = joint * 3.0f + axis;
        }
    }
    for (int hand = 0; hand < 2; hand++) {
        zsharp_xr_hand_update(&state.hands[hand].hand, joints, 1, 1);
        for (int joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
            snprintf(path, sizeof(path), "XR.%s.hand.%s.tracked", hand ? "right" : "left", names[joint]);
            assert(zsharp_xr_property(path, 0) && !zsharp_xr_property(path, 1));
            assert(status_read(&state, &cursor, path));
            for (int axis = 0; axis < 3; axis++) {
                snprintf(path, sizeof(path), "XR.%s.hand.%s.position%c", hand ? "right" : "left", names[joint], 'X' + axis);
                assert(zsharp_xr_property(path, 0) && !zsharp_xr_property(path, 1));
                assert(number_read(&state, &cursor, path) == joint * 10.0 + axis);
                snprintf(path, sizeof(path), "XR.%s.hand.%s.rotation%c", hand ? "right" : "left", names[joint], 'X' + axis);
                assert(zsharp_xr_property(path, 0) && !zsharp_xr_property(path, 1));
                assert(number_read(&state, &cursor, path) == joint * 3.0 + axis);
            }
        }
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!zsharp_xr_property(invalid[i], 0));
    assert(!zsharp_xr_read(&state, NULL, "XR.left.hand.pinch.pressed", output, sizeof(output), &status));
    /* Existing controller grip fields must stay independent of hand joints. */
    state.hands[0].pose.position[0] = 123;
    state.hands[0].pose.tracked = 1;
    assert(number_read(&state, &cursor, "XR.left.positionX") == 123);
    assert(number_read(&state, &cursor, "XR.left.hand.palm.positionX") == 0);
}

static void test_pinch(void) {
    ZSharpXRState state = {0};
    ZSharpXRCursor first = {0}, second = {0}, late = {0};
    ZSharpXRHandJoint joints[ZXR_HAND_JOINTS];
    ZSharpXRTrackedHand *hand = &state.hands[0].hand;
    zsharp_xr_cursor_begin(&first, &state);
    zsharp_xr_cursor_begin(&second, &state);
    /* Initial tracking while already pinched must not fabricate a click. */
    tips(joints, 0);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(status_read(&state, &first, "XR.left.hand.tracked"));
    assert(number_read(&state, &first, "XR.left.hand.pinch.value") == 1);
    assert(!status_read(&state, &first, "XR.left.hand.pinch.held"));
    assert(!status_read(&state, &first, "XR.left.hand.pinch.pressed"));
    tips(joints, 0.04f);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(hand->pinch_armed);
    for (int tap = 0; tap < 2; tap++) {
        tips(joints, 0.02f);
        zsharp_xr_hand_update(hand, joints, 1, 1);
        assert(hand->pinch_held);
        tips(joints, 0.03f);
        zsharp_xr_hand_update(hand, joints, 1, 1);
        assert(hand->pinch_held);
        tips(joints, 0.04f);
        zsharp_xr_hand_update(hand, joints, 1, 1);
        assert(!hand->pinch_held);
        tips(joints, 0.03f);
        zsharp_xr_hand_update(hand, joints, 1, 1);
        assert(!hand->pinch_held);
    }
    assert(fabs(number_read(&state, &first, "XR.left.hand.pinch.value") - 0.5) < 0.00001);
    for (int tap = 0; tap < 2; tap++) {
        assert(status_read(&state, &first, "XR.left.hand.pinch.pressed"));
        assert(status_read(&state, &first, "XR.left.hand.pinch.released"));
        assert(status_read(&state, &second, "XR.left.hand.pinch.pressed"));
        assert(status_read(&state, &second, "XR.left.hand.pinch.released"));
    }
    assert(!status_read(&state, &first, "XR.left.hand.pinch.pressed"));
    assert(!status_read(&state, &first, "XR.left.hand.pinch.released"));
    zsharp_xr_cursor_begin(&late, &state);
    assert(!status_read(&state, &late, "XR.left.hand.pinch.pressed"));
    /* Right hand counters/cursors are separate. */
    tips(joints, 0.06f);
    zsharp_xr_hand_update(&state.hands[1].hand, joints, 1, 1);
    tips(joints, 0.01f);
    zsharp_xr_hand_update(&state.hands[1].hand, joints, 1, 1);
    assert(status_read(&state, &first, "XR.right.hand.pinch.pressed"));
    assert(!status_read(&state, &first, "XR.left.hand.pinch.pressed"));
}

static void test_tracking_loss(void) {
    ZSharpXRState state = {0};
    ZSharpXRCursor cursor = {0};
    ZSharpXRHandJoint joints[ZXR_HAND_JOINTS];
    ZSharpXRTrackedHand *hand = &state.hands[0].hand;
    zsharp_xr_cursor_begin(&cursor, &state);
    tips(joints, 0.06f);
    joints[ZXR_HAND_WRIST].position[1] = 2.0f;
    zsharp_xr_hand_update(hand, joints, 1, 1);
    tips(joints, 0.01f);
    joints[ZXR_HAND_WRIST].position[1] = 2.0f;
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(hand->pinch_held);
    zsharp_xr_hand_update(hand, NULL, 0, 1);
    assert(!hand->tracked && !hand->pinch_held);
    assert(!status_read(&state, &cursor, "XR.left.hand.pinch.pressed"));
    assert(status_read(&state, &cursor, "XR.left.hand.pinch.released"));
    assert(!status_read(&state, &cursor, "XR.left.hand.wrist.tracked"));
    assert(number_read(&state, &cursor, "XR.left.hand.wrist.positionY") == 2);
    /* Reacquisition still pinched is disarmed until a visible open hand. */
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(!hand->pinch_held && !hand->pinch_armed);
    assert(!status_read(&state, &cursor, "XR.left.hand.pinch.pressed"));
    tips(joints, 0.06f);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    tips(joints, 0.01f);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(status_read(&state, &cursor, "XR.left.hand.pinch.pressed"));
    /* Missing tip joints and focus loss act like tracking loss. */
    joints[ZXR_HAND_INDEX_TIP].tracked = 0;
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(!hand->pinch_held && !hand->pinch_armed);
    assert(status_read(&state, &cursor, "XR.left.hand.pinch.released"));
    tips(joints, 0.06f);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    tips(joints, 0);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    zsharp_xr_hand_update(hand, joints, 1, 0);
    assert(!status_read(&state, &cursor, "XR.left.hand.tracked"));
    assert(!status_read(&state, &cursor, "XR.left.hand.pinch.pressed"));
    assert(status_read(&state, &cursor, "XR.left.hand.pinch.released"));
    assert(number_read(&state, &cursor, "XR.left.hand.pinch.value") == 0);
    /* Malformed backend samples never leak NaNs or bogus gestures. */
    tips(joints, 0.06f);
    zsharp_xr_hand_update(hand, joints, 1, 1);
    joints[ZXR_HAND_INDEX_TIP].position[0] = NAN;
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(!hand->joints[ZXR_HAND_INDEX_TIP].tracked);
    assert(isfinite(hand->joints[ZXR_HAND_INDEX_TIP].position[0]));
    assert(!hand->pinch_armed);
    memset(joints, 0, sizeof(joints));
    zsharp_xr_hand_update(hand, joints, 1, 1);
    assert(!status_read(&state, &cursor, "XR.left.hand.tracked"));
}

static void test_removed_passthrough(void) {
    ZSharpXRState state = {0};
    ZSharpXRCursor cursor = {0};
    char error[256], output[128];
    int status;
    static const char *removed[] = {
        "XR.passthrough.available", "XR.passthrough.enabled", "XR.passthrough.active"
    };
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        assert(!zsharp_xr_property(removed[i], 0));
        assert(!zsharp_xr_property(removed[i], 1));
        assert(!zsharp_xr_read(&state, &cursor, removed[i], output, sizeof(output), &status));
        assert(!zsharp_xr_write(&state, removed[i], "alive", error, sizeof(error)));
        assert(strstr(error, "read-only or unknown") && strstr(error, removed[i]));
        assert(!zsharp_xr_write(&state, removed[i], "dead", error, sizeof(error)));
    }
    assert(!zsharp_xr_write(&state, "XR.left.hand.pinch.held", "alive", error, sizeof(error)));
    assert(!zsharp_xr_write(&state, "XR.left.hand.indexTip.positionX", "1", error, sizeof(error)));
}

int main(void) {
    test_properties();
    test_pinch();
    test_tracking_loss();
    test_removed_passthrough();
    puts("XR hand tracking and unsupported-property tests passed.");
    return 0;
}
