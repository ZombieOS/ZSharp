#ifdef NDEBUG
#undef NDEBUG
#endif
#include "xr_hands_openxr.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
template<class Handle> Handle handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}
const XrInstance instance = handle<XrInstance>(0x10);
const XrSession session = handle<XrSession>(0x20);
const XrSpace space = handle<XrSpace>(0x30);
constexpr XrSystemId system_id = 0x40;
constexpr XrTime display_time = 123456789;
const XrSpaceLocationFlags tracked_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

struct Mock {
    bool session_alive = true;
    bool supports_hands = true;
    XrResult system_result = XR_SUCCESS;
    const char *missing_proc = nullptr;
    bool null_proc_success = false;
    int fail_create = -1;
    int system_calls = 0, proc_calls = 0, create_calls = 0, destroy_calls = 0;
    int locate_calls[2]{};
    bool tracker_alive[2]{};
    XrResult locate_result[2]{XR_SUCCESS, XR_SUCCESS};
    bool active[2]{true, true};
    XrSpace last_space[2]{};
    XrTime last_time[2]{};
    XrHandJointLocationEXT joints[2][XR_HAND_JOINT_COUNT_EXT]{};
} mock;

XrHandTrackerEXT tracker(int hand) { return handle<XrHandTrackerEXT>(0x100 + hand); }
int hand_id(XrHandTrackerEXT value) {
    if (value == tracker(0)) return 0;
    assert(value == tracker(1));
    return 1;
}

void reset_mock() {
    mock = Mock{};
    for (int hand = 0; hand < 2; hand++) {
        for (int joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; joint++) {
            auto &out = mock.joints[hand][joint];
            out.locationFlags = tracked_flags;
            out.pose.orientation.w = 1;
            out.pose.position = {0, 1, -1};
            out.radius = 0.005f;
        }
        mock.joints[hand][XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.x = 0.06f;
    }
}

XrResult XRAPI_CALL create_tracker(XrSession value, const XrHandTrackerCreateInfoEXT *info,
                                  XrHandTrackerEXT *out) {
    assert(mock.session_alive && value == session && info && out);
    assert(info->type == XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT);
    assert(!info->next && info->handJointSet == XR_HAND_JOINT_SET_DEFAULT_EXT);
    const int hand = info->hand == XR_HAND_LEFT_EXT ? 0 : 1;
    assert(info->hand == (hand ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT));
    mock.create_calls++;
    if (hand == mock.fail_create) return XR_ERROR_RUNTIME_FAILURE;
    assert(!mock.tracker_alive[hand]);
    mock.tracker_alive[hand] = true;
    *out = tracker(hand);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_tracker(XrHandTrackerEXT value) {
    assert(mock.session_alive); // Trackers must die before their session.
    const int hand = hand_id(value);
    assert(mock.tracker_alive[hand]);
    mock.tracker_alive[hand] = false;
    mock.destroy_calls++;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL locate_joints(XrHandTrackerEXT value,
                                 const XrHandJointsLocateInfoEXT *info,
                                 XrHandJointLocationsEXT *out) {
    const int hand = hand_id(value);
    assert(mock.session_alive && mock.tracker_alive[hand] && info && out);
    assert(info->type == XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT && !info->next);
    assert(out->type == XR_TYPE_HAND_JOINT_LOCATIONS_EXT && !out->next);
    assert(out->jointCount == XR_HAND_JOINT_COUNT_EXT && out->jointLocations);
    mock.locate_calls[hand]++;
    mock.last_space[hand] = info->baseSpace;
    mock.last_time[hand] = info->time;
    if (XR_FAILED(mock.locate_result[hand])) return mock.locate_result[hand];
    out->isActive = mock.active[hand] ? XR_TRUE : XR_FALSE;
    std::memcpy(out->jointLocations, mock.joints[hand], sizeof(mock.joints[hand]));
    return XR_SUCCESS;
}

template<class Function> PFN_xrVoidFunction erase_function(Function function) {
    PFN_xrVoidFunction erased;
    static_assert(sizeof(erased) == sizeof(function), "OpenXR function pointer size");
    std::memcpy(&erased, &function, sizeof(erased));
    return erased;
}

bool read_status(ZSharpXRState &state, ZSharpXRCursor &cursor, const char *path) {
    char output[64];
    int is_status = 0;
    assert(zsharp_xr_read(&state, &cursor, path, output, sizeof(output), &is_status));
    assert(is_status);
    return !std::strcmp(output, "alive");
}

void set_pinch(int hand, float distance) {
    mock.joints[hand][XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.x = distance;
}

void test_optional_unavailable() {
    reset_mock();
    ZSharpXRState state{};
    XRHandsBackend backend;
    assert(!backend.init(instance, session, system_id, false));
    assert(mock.system_calls == 0 && mock.proc_calls == 0 && mock.create_calls == 0);
    assert(!backend.init(XR_NULL_HANDLE, session, system_id, true));
    assert(!backend.init(instance, XR_NULL_HANDLE, system_id, true));
    assert(!backend.init(instance, session, XR_NULL_SYSTEM_ID, true));
    assert(mock.system_calls == 0);
    mock.supports_hands = false;
    assert(!backend.init(instance, session, system_id, true));
    assert(mock.system_calls == 1 && mock.proc_calls == 0);
    mock.supports_hands = true;
    mock.system_result = XR_ERROR_RUNTIME_FAILURE;
    assert(!backend.init(instance, session, system_id, true));
    assert(mock.system_calls == 2 && mock.create_calls == 0);
    state.hand_tracking_available = 1;
    state.hands[0].hand.tracked = 1;
    backend.update(space, display_time, true, state);
    assert(!state.hand_tracking_available && !state.hands[0].hand.tracked);
    assert(mock.locate_calls[0] == 0 && mock.locate_calls[1] == 0);
}

void test_missing_procs_and_partial_cleanup() {
    static const char *const names[] = {
        "xrCreateHandTrackerEXT", "xrDestroyHandTrackerEXT", "xrLocateHandJointsEXT"
    };
    for (const char *name : names) {
        for (int null_success = 0; null_success < 2; null_success++) {
            reset_mock();
            mock.missing_proc = name;
            mock.null_proc_success = null_success != 0;
            XRHandsBackend backend;
            assert(!backend.init(instance, session, system_id, true));
            assert(!backend.available() && mock.create_calls == 0 && mock.destroy_calls == 0);
        }
    }
    for (int fail_hand = 0; fail_hand < 2; fail_hand++) {
        reset_mock();
        mock.fail_create = fail_hand;
        XRHandsBackend backend;
        assert(!backend.init(instance, session, system_id, true));
        assert(!backend.available());
        assert(mock.create_calls == fail_hand + 1);
        assert(mock.destroy_calls == fail_hand);
        assert(!mock.tracker_alive[0] && !mock.tracker_alive[1]);
    }
}

void test_all_joint_conversion_and_shutdown() {
    reset_mock();
    ZSharpXRState state{};
    state.hands[0].pose.position[0] = 999; // Grip pose is a separate API.
    for (int hand = 0; hand < 2; hand++) for (int joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; joint++) {
        auto &out = mock.joints[hand][joint];
        out.pose.position = {static_cast<float>(hand * 10 + joint),
                             static_cast<float>(joint) * 0.01f,
                             -static_cast<float>(joint) * 0.02f};
        out.pose.orientation = {0, 0.5f, 0, 0.8660254038f};
        out.radius = 0.002f + static_cast<float>(joint) * 0.0001f;
    }
    XRHandsBackend backend;
    assert(backend.init(instance, session, system_id, true));
    assert(backend.available() && mock.create_calls == 2);
    backend.update(space, display_time, true, state);
    assert(state.hand_tracking_available);
    for (int hand = 0; hand < 2; hand++) {
        assert(mock.last_space[hand] == space && mock.last_time[hand] == display_time);
        assert(mock.locate_calls[hand] == 1 && state.hands[hand].hand.tracked);
        for (int joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; joint++) {
            const auto &actual = state.hands[hand].hand.joints[joint];
            const auto &expected = mock.joints[hand][joint];
            assert(actual.tracked && actual.position[0] == expected.pose.position.x);
            assert(actual.position[1] == expected.pose.position.y);
            assert(actual.position[2] == expected.pose.position.z);
            assert(actual.radius == expected.radius);
            assert(std::fabs(actual.rotation[0]) < 0.001f);
            assert(std::fabs(actual.rotation[1] - 60) < 0.001f);
            assert(std::fabs(actual.rotation[2]) < 0.001f);
        }
    }
    assert(state.hands[0].pose.position[0] == 999);
    // Reinitialization first destroys both trackers; it does not leak them.
    assert(backend.init(instance, session, system_id, true));
    assert(mock.create_calls == 4 && mock.destroy_calls == 2);
    backend.shutdown();
    assert(mock.destroy_calls == 4 && !mock.tracker_alive[0] && !mock.tracker_alive[1]);
    mock.session_alive = false;
    backend.shutdown(); // Idempotent, and destructor must not use the dead session.
    backend.update(space, display_time, true, state);
    assert(!state.hand_tracking_available && !state.hands[0].hand.tracked);
}

void test_tracking_edges_and_focus() {
    reset_mock();
    XRHandsBackend backend;
    ZSharpXRState state{};
    ZSharpXRCursor cursor{};
    assert(backend.init(instance, session, system_id, true));
    zsharp_xr_cursor_begin(&cursor, &state);
    backend.update(space, display_time, true, state); // Open hands arm pinch.
    set_pinch(0, 0.01f);
    backend.update(space, display_time + 1, true, state);
    assert(read_status(state, cursor, "XR.left.hand.pinch.pressed"));
    assert(state.hands[0].hand.pinch_held);
    const float last_x = state.hands[0].hand.joints[ZXR_HAND_INDEX_TIP].position[0];
    // A valid-but-not-tracked joint must not be used for pinch.
    mock.joints[0][XR_HAND_JOINT_INDEX_TIP_EXT].locationFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    backend.update(space, display_time + 2, true, state);
    assert(state.hands[0].hand.tracked && !state.hands[0].hand.joints[ZXR_HAND_INDEX_TIP].tracked);
    assert(state.hands[0].hand.joints[ZXR_HAND_INDEX_TIP].position[0] == last_x);
    assert(read_status(state, cursor, "XR.left.hand.pinch.released"));
    mock.joints[0][XR_HAND_JOINT_INDEX_TIP_EXT].locationFlags = tracked_flags;
    backend.update(space, display_time + 3, true, state);
    assert(!state.hands[0].hand.pinch_held); // Reacquired pinched: disarmed.
    set_pinch(0, 0.06f);
    backend.update(space, display_time + 4, true, state);
    set_pinch(0, 0.01f);
    backend.update(space, display_time + 5, true, state);
    assert(read_status(state, cursor, "XR.left.hand.pinch.pressed"));
    mock.locate_result[0] = XR_ERROR_RUNTIME_FAILURE;
    backend.update(space, display_time + 6, true, state);
    assert(!state.hands[0].hand.tracked && state.hands[1].hand.tracked);
    assert(state.hand_tracking_available); // Sample failure is not capability loss.
    assert(read_status(state, cursor, "XR.left.hand.pinch.released"));
    mock.locate_result[0] = XR_SUCCESS;
    mock.active[0] = false;
    backend.update(space, display_time + 7, true, state);
    assert(!state.hands[0].hand.tracked);
    mock.active[0] = true;
    set_pinch(0, 0.06f);
    backend.update(space, display_time + 8, true, state);
    set_pinch(0, 0.01f);
    backend.update(space, display_time + 9, true, state);
    const int left_locates = mock.locate_calls[0], right_locates = mock.locate_calls[1];
    backend.update(space, display_time + 10, false, state);
    assert(mock.locate_calls[0] == left_locates && mock.locate_calls[1] == right_locates);
    assert(!state.hands[0].hand.tracked && !state.hands[1].hand.tracked);
    assert(!read_status(state, cursor, "XR.left.hand.pinch.pressed")); // No stale press after focus loss.
    assert(read_status(state, cursor, "XR.left.hand.pinch.released"));
    backend.update(XR_NULL_HANDLE, display_time, true, state);
    backend.update(space, 0, true, state);
    assert(mock.locate_calls[0] == left_locates && mock.locate_calls[1] == right_locates);
    backend.shutdown();
    mock.session_alive = false;
}
} // namespace

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(
    XrInstance value, XrSystemId id, XrSystemProperties *out) {
    assert(value == instance && id == system_id && out && out->type == XR_TYPE_SYSTEM_PROPERTIES);
    mock.system_calls++;
    if (XR_FAILED(mock.system_result)) return mock.system_result;
    auto *tracking = static_cast<XrSystemHandTrackingPropertiesEXT *>(out->next);
    assert(tracking && tracking->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT);
    tracking->supportsHandTracking = mock.supports_hands ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance value, const char *name, PFN_xrVoidFunction *out) {
    assert(value == instance && name && out);
    mock.proc_calls++;
    *out = nullptr;
    if (mock.missing_proc && !std::strcmp(name, mock.missing_proc))
        return mock.null_proc_success ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!std::strcmp(name, "xrCreateHandTrackerEXT")) *out = erase_function(create_tracker);
    else if (!std::strcmp(name, "xrDestroyHandTrackerEXT")) *out = erase_function(destroy_tracker);
    else if (!std::strcmp(name, "xrLocateHandJointsEXT")) *out = erase_function(locate_joints);
    else return XR_ERROR_FUNCTION_UNSUPPORTED;
    return XR_SUCCESS;
}

int main() {
    test_optional_unavailable();
    test_missing_procs_and_partial_cleanup();
    test_all_joint_conversion_and_shutdown();
    test_tracking_edges_and_focus();
    std::puts("XR hand backend mock tests passed (no headset or live session).");
    return 0;
}
