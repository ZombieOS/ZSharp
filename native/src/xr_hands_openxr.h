#ifndef ZSHARP_XR_HANDS_OPENXR_H
#define ZSHARP_XR_HANDS_OPENXR_H

#include "xr_input.h"
#include <openxr/openxr.h>

/* Optional extension backend: absence, unsupported runtimes or per-frame
   tracking failures never make ordinary controller-based PCVR fail. */
class XRHandsBackend {
public:
    XRHandsBackend() = default;
    XRHandsBackend(const XRHandsBackend &) = delete;
    XRHandsBackend &operator=(const XRHandsBackend &) = delete;
    ~XRHandsBackend() { shutdown(); }

    bool init(XrInstance instance, XrSession session, XrSystemId system,
              bool extension_enabled) {
        shutdown();
        if (!extension_enabled || instance == XR_NULL_HANDLE ||
            session == XR_NULL_HANDLE || system == XR_NULL_SYSTEM_ID) return false;
        XrSystemHandTrackingPropertiesEXT tracking{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        properties.next = &tracking;
        if (XR_FAILED(xrGetSystemProperties(instance, system, &properties)) ||
            !tracking.supportsHandTracking) return false;
        if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
                         reinterpret_cast<PFN_xrVoidFunction *>(&create_))) || !create_ ||
            XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
                         reinterpret_cast<PFN_xrVoidFunction *>(&destroy_))) || !destroy_ ||
            XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
                         reinterpret_cast<PFN_xrVoidFunction *>(&locate_))) || !locate_) {
            shutdown();
            return false;
        }
        for (int hand = 0; hand < 2; hand++) {
            XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(create_(session, &info, &trackers_[hand]))) {
                shutdown();
                return false;
            }
        }
        available_ = true;
        return true;
    }

    void shutdown() {
        for (auto &tracker : trackers_) {
            if (tracker != XR_NULL_HANDLE && destroy_) destroy_(tracker);
            tracker = XR_NULL_HANDLE;
        }
        available_ = false;
        create_ = nullptr;
        destroy_ = nullptr;
        locate_ = nullptr;
    }

    bool available() const { return available_; }

    void update(XrSpace base_space, XrTime predicted_time, bool focused,
                ZSharpXRState &state) {
        state.hand_tracking_available = available_;
        const bool can_locate = available_ && focused &&
                                base_space != XR_NULL_HANDLE && predicted_time > 0;
        for (int hand = 0; hand < 2; hand++) {
            ZSharpXRHandJoint converted[ZXR_HAND_JOINTS]{};
            if (!can_locate) {
                zsharp_xr_hand_update(&state.hands[hand].hand, nullptr, 0, focused);
                continue;
            }
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
            XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locations.jointLocations = joints;
            XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            info.baseSpace = base_space;
            info.time = predicted_time;
            if (XR_FAILED(locate_(trackers_[hand], &info, &locations)) || !locations.isActive) {
                zsharp_xr_hand_update(&state.hands[hand].hand, nullptr, 0, focused);
                continue;
            }
            const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                               XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                                                 XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            for (int joint = 0; joint < ZXR_HAND_JOINTS; joint++) {
                const auto &source = joints[joint];
                auto &out = converted[joint];
                out.tracked = (source.locationFlags & (valid | tracked)) == (valid | tracked);
                if (!out.tracked) continue;
                out.position[0] = source.pose.position.x;
                out.position[1] = source.pose.position.y;
                out.position[2] = source.pose.position.z;
                const float quaternion[] = {source.pose.orientation.x, source.pose.orientation.y,
                                            source.pose.orientation.z, source.pose.orientation.w};
                zsharp_xr_orientation(quaternion, out.rotation);
                out.radius = source.radius;
            }
            zsharp_xr_hand_update(&state.hands[hand].hand, converted, 1, focused);
        }
    }

private:
    bool available_ = false;
    XrHandTrackerEXT trackers_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT create_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy_ = nullptr;
    PFN_xrLocateHandJointsEXT locate_ = nullptr;
};

static_assert(ZXR_HAND_JOINTS == XR_HAND_JOINT_COUNT_EXT,
              "OpenXR hand joint ordering must match the runtime snapshot");

#endif
