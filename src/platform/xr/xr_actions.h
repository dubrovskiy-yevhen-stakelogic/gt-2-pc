#pragma once
#include <openxr/openxr.h>
#include "platform/input/ps1_pad.h"
#include "platform/xr/vr_driving.h"
namespace gt2::xr {
class ControllerActions {
public:
    ControllerActions(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr get, bool alternateMenuChord = false);
    ~ControllerActions();
    bool Poll(input::Ps1PadFrame& pad, float vibration, bool focused);
    vr::TrackedControllers Locate(XrSpace base, XrTime time);
private:
    XrSession session_;
    bool alternateMenuChord_ = false;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction stick_, trigger_, accept_, back_, brake_, view_, menu_, grip_, click_, haptic_;
    XrPath hands_[2]{};
    XrAction gripPose_, aimPose_;
    XrSpace gripSpaces_[2]{}, aimSpaces_[2]{};
    vr::TrackedControllers tracking_;
#define GT2_ACTION_API(X) X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) \
    X(xrCreateActionSpace) X(xrDestroySpace) X(xrLocateSpace) X(xrGetActionStatePose) \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) X(xrStopHapticFeedback)
#define DECLARE(n) PFN_##n n = nullptr;
    GT2_ACTION_API(DECLARE)
#undef DECLARE
};
}
