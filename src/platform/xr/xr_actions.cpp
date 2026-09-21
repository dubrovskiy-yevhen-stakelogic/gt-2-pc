#include "platform/xr/xr_actions.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace gt2::xr {
namespace {
void Check(XrResult r, const char* where) {
    if (XR_FAILED(r)) throw std::runtime_error(std::string(where) + ": " + std::to_string(r));
}
}
ControllerActions::ControllerActions(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr get, bool alternateMenuChord)
    : session_(session), alternateMenuChord_(alternateMenuChord) {
#define LOAD(n) Check(get(instance, #n, reinterpret_cast<PFN_xrVoidFunction*>(&n)), #n);
    GT2_ACTION_API(LOAD)
#undef LOAD
    auto path = [&](const char* text) { XrPath p{}; Check(xrStringToPath(instance, text, &p), text); return p; };
    hands_[0] = path("/user/hand/left"); hands_[1] = path("/user/hand/right");
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "driving"); std::strcpy(si.localizedActionSetName, "Driving and menus");
    Check(xrCreateActionSet(instance, &si, &set_), "xrCreateActionSet");
    auto action = [&](const char* name, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name); std::strcpy(ci.localizedActionName, name);
        ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = hands_;
        XrAction a{}; Check(xrCreateAction(set_, &ci, &a), name); return a;
    };
    stick_ = action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    trigger_ = action("pedal", XR_ACTION_TYPE_FLOAT_INPUT);
    grip_ = action("grip", XR_ACTION_TYPE_FLOAT_INPUT);
    accept_ = action("accept", XR_ACTION_TYPE_BOOLEAN_INPUT);
    back_ = action("back", XR_ACTION_TYPE_BOOLEAN_INPUT);
    brake_ = action("brake", XR_ACTION_TYPE_BOOLEAN_INPUT);
    view_ = action("view", XR_ACTION_TYPE_BOOLEAN_INPUT);
    menu_ = action("pause", XR_ACTION_TYPE_BOOLEAN_INPUT);
    click_ = action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    haptic_ = action("rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    gripPose_ = action("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    aimPose_ = action("aim_pose", XR_ACTION_TYPE_POSE_INPUT);
    std::vector<XrActionSuggestedBinding> bindings;
    auto bind = [&](XrAction a, const char* p) { bindings.push_back({a, path(p)}); };
    for (const char* hand : {"left", "right"}) {
        const std::string prefix = std::string("/user/hand/") + hand;
        bind(gripPose_, (prefix + "/input/grip/pose").c_str());
        bind(aimPose_, (prefix + "/input/aim/pose").c_str());
        bind(stick_, (prefix + "/input/thumbstick").c_str());
        bind(trigger_, (prefix + "/input/trigger/value").c_str());
        bind(grip_, (prefix + "/input/squeeze/value").c_str());
        bind(click_, (prefix + "/input/thumbstick/click").c_str());
        bind(haptic_, (prefix + "/output/haptic").c_str());
    }
    bind(accept_, "/user/hand/right/input/a/click");
    bind(back_, "/user/hand/right/input/b/click");
    bind(brake_, "/user/hand/left/input/x/click");
    bind(view_, "/user/hand/left/input/y/click");
    bind(menu_, "/user/hand/left/input/menu/click");
    XrInteractionProfileSuggestedBinding bi{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    bi.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    bi.countSuggestedBindings = uint32_t(bindings.size()); bi.suggestedBindings = bindings.data();
    Check(xrSuggestInteractionProfileBindings(instance, &bi), "Touch bindings");
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1; ai.actionSets = &set_;
    Check(xrAttachSessionActionSets(session_, &ai), "Attach Touch actions");
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.subactionPath = hands_[h]; ci.poseInActionSpace.orientation.w = 1;
        ci.action = gripPose_; Check(xrCreateActionSpace(session_, &ci, &gripSpaces_[h]), "grip space");
        ci.action = aimPose_; Check(xrCreateActionSpace(session_, &ci, &aimSpaces_[h]), "aim space");
    }
}
ControllerActions::~ControllerActions() {
    for (int h = 0; h < 2; ++h) { if (gripSpaces_[h]) xrDestroySpace(gripSpaces_[h]); if (aimSpaces_[h]) xrDestroySpace(aimSpaces_[h]); }
    if (set_) xrDestroyActionSet(set_);
}
vr::TrackedControllers ControllerActions::Locate(XrSpace base, XrTime time) {
    auto result = tracking_;
    auto locate = [&](XrAction action, XrSpace space, int h, vr::Pose& pose) {
        if (time <= 0) return false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = action; gi.subactionPath = hands_[h];
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session_, &gi, &state)) || !state.isActive) return false;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(space, base, time, &loc)) || (loc.locationFlags & valid) != valid) return false;
        pose.position[0] = loc.pose.position.x; pose.position[1] = loc.pose.position.y; pose.position[2] = loc.pose.position.z;
        pose.orientation[0] = loc.pose.orientation.x; pose.orientation[1] = loc.pose.orientation.y;
        pose.orientation[2] = loc.pose.orientation.z; pose.orientation[3] = loc.pose.orientation.w;
        return true;
    };
    for (int h = 0; h < 2; ++h) {
        result.gripValid[h] = locate(gripPose_,gripSpaces_[h],h,result.gripPose[h]);
        result.aimValid[h] = locate(aimPose_,aimSpaces_[h],h,result.aimPose[h]);
    }
    return result;
}
bool ControllerActions::Poll(input::Ps1PadFrame& pad, float vibration, bool focused) {
    pad = {};
    tracking_ = {};
    if (!focused) return false;
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets = 1; sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &sync))) return false;
    bool available = false;
    auto boolean = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = hands_[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &state)) && state.isActive && state.currentState;
    };
    auto scalar = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = hands_[hand];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session_, &gi, &state)) || !state.isActive) return 0.0f;
        available = true; return std::clamp(state.currentState, 0.0f, 1.0f);
    };
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = stick_; gi.subactionPath = hands_[h];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &state)) && state.isActive) {
            available = true;
            const size_t offset = h == 0 ? 2 : 0;
            pad.analog[offset] = uint8_t(std::lround(128 + std::clamp(state.currentState.x, -1.0f, 1.0f) * 127));
            pad.analog[offset + 1] = uint8_t(std::lround(128 - std::clamp(state.currentState.y, -1.0f, 1.0f) * 127));
            if (h == 0) {
                if (state.currentState.x < -0.65f) pad.buttons |= input::ps1::kLeft;
                if (state.currentState.x > 0.65f) pad.buttons |= input::ps1::kRight;
                if (state.currentState.y < -0.65f) pad.buttons |= input::ps1::kDown;
                if (state.currentState.y > 0.65f) pad.buttons |= input::ps1::kUp;
            }
        }
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO}; hi.action = haptic_; hi.subactionPath = hands_[h];
        if (vibration > 0) {
            XrHapticVibration value{XR_TYPE_HAPTIC_VIBRATION};
            value.amplitude = std::clamp(vibration, 0.0f, 1.0f); value.duration = 35'000'000; value.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(session_, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&value));
        } else xrStopHapticFeedback(session_, &hi);
    }
    for (int h = 0; h < 2; ++h) { tracking_.grip[h] = scalar(grip_,h); tracking_.trigger[h] = scalar(trigger_,h); }
    pad.pressure = true;
    pad.pressureL2 = uint8_t(std::lround(scalar(trigger_, 0) * 255));
    pad.pressureR2 = uint8_t(std::lround(scalar(trigger_, 1) * 255));
    if (pad.pressureL2 > 30) pad.buttons |= input::ps1::kL2;
    if (pad.pressureR2 > 30) pad.buttons |= input::ps1::kR2;
    if (boolean(accept_, 1)) pad.buttons |= input::ps1::kCross;
    if (boolean(back_, 1)) pad.buttons |= input::ps1::kTriangle;
    if (boolean(brake_, 0)) pad.buttons |= input::ps1::kSquare;
    const bool viewPressed = boolean(view_, 0);
    const bool bothGrips = tracking_.grip[0] > 0.7f && tracking_.grip[1] > 0.7f;
    const bool leftClick = boolean(click_, 0), rightClick = boolean(click_, 1);
    const bool alternateMenu = alternateMenuChord_ && leftClick && rightClick;
    if (viewPressed) pad.buttons |= input::ps1::kR1;
    if (boolean(menu_, 0) || alternateMenu) {
        pad.buttons |= input::ps1::kStart;
        if (bothGrips || alternateMenu) pad.buttons |= input::ps1::kSelect;
    }
    if (rightClick && !alternateMenu) pad.buttons |= input::ps1::kCircle; // Handbrake; grips only interact with the wheel.
    if (leftClick && !alternateMenu) pad.buttons |= input::ps1::kSelect;
    pad.type = available ? input::kTypeAnalog : input::kTypeNone;
    return available;
}
}
