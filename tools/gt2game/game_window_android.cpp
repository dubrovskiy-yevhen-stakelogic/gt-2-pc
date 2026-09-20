// The window backend of the Quest build (game_window.h WindowBackend; docs/research/vr_port_plan.md, M6).
//
// It is the Android twin of game_window_xr.cpp: the same OpenXR session (src/platform/xr/xr_session.h), the same
// offscreen renderer and the same cinema quad / stereo projection layer. What the Windows file gets from a desktop
// window - the keyboard, the focus, the controllers, the high-resolution sleep - comes here from the NativeActivity:
// android_main services the looper and hands this file the lifecycle and the input events (android_host.h).
//
// There is no desktop mirror and no plain window: on the headset the picture exists only in the compositor. Both
// CreateWindowBackend and CreateInputWindowBackend therefore refuse instead of opening something that cannot work -
// the Android build always runs in VR (android_main passes --vr).
#include <vulkan/vulkan.h>

#include "game_window.h"
#include "pc_overlay.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "android_host.h"
#include "android_activity_state.h"
#include "game/audio/pause.h"
#include "gt2view/vk_context.h"
#include "gt2view/vr_driving_visuals.h"
#include "platform/input/android_input.h"
#include "platform/os/keys.h"
#include "platform/xr/vr_rig.h"
#include "platform/xr/xr_session.h"

namespace gt2game {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kTag = "GT2.Quest";

// ---------------------------------------------------------------- the host's side (android_main -> here)

struct HostState : android::ActivityState {
    std::mutex mutex;
    void* vm = nullptr;
    void* activity = nullptr;
};

HostState& Host() {
    static HostState state;
    return state;
}

// Android key code (android/keycodes.h) -> a key code of platform/os/keys.h, 0 when the screens do not use it.
// A Bluetooth keyboard gives letters and the arrow block; a gamepad's buttons reach the game through the pad path
// (platform/input/android_input.h) and the pad-to-key merge of game_window.cpp, not here.
int GameKey(int32_t androidKeyCode) {
    namespace keys = gt2::keys;
    switch (androidKeyCode) {
    case 19: return keys::kUp;     // AKEYCODE_DPAD_UP
    case 20: return keys::kDown;   // AKEYCODE_DPAD_DOWN
    case 21: return keys::kLeft;   // AKEYCODE_DPAD_LEFT
    case 22: return keys::kRight;  // AKEYCODE_DPAD_RIGHT
    case 23: return keys::kReturn; // AKEYCODE_DPAD_CENTER
    case 62: return keys::kSpace;  // AKEYCODE_SPACE
    case 66: return keys::kReturn; // AKEYCODE_ENTER
    case 67: return keys::kBack;   // AKEYCODE_DEL (backspace)
    case 111: return keys::kEscape;// AKEYCODE_ESCAPE
    case 112: return keys::kDelete;// AKEYCODE_FORWARD_DEL
    case 92: return keys::kPageUp;  // AKEYCODE_PAGE_UP
    case 93: return keys::kPageDown;// AKEYCODE_PAGE_DOWN
    case 122: return keys::kHome;   // AKEYCODE_MOVE_HOME
    case 59: case 60: return keys::kShift; // AKEYCODE_SHIFT_LEFT / _RIGHT
    case 136: return keys::kF5;    // AKEYCODE_F5
    case 141: return keys::kF10;   // AKEYCODE_F10
    default: break;
    }
    if (androidKeyCode >= 29 && androidKeyCode <= 54) return 'A' + (androidKeyCode - 29); // AKEYCODE_A .. AKEYCODE_Z
    if (androidKeyCode >= 7 && androidKeyCode <= 16) return '0' + (androidKeyCode - 7);   // AKEYCODE_0 .. AKEYCODE_9
    return 0;
}

} // namespace

namespace android {

void SetHost(void* javaVm, void* activity) {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.vm = javaVm;
    h.activity = activity;
    h.Reset();
    gt2::input::android::Reset();
}

void* JavaVm() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.vm;
}

void* Activity() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.activity;
}

void SetResumed(bool resumed) {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.resumed = resumed;
}

bool Resumed() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.resumed;
}

void RequestQuit() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.quit = true;
}

bool QuitRequested() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.quit;
}

void KeyEvent(int32_t androidKeyCode, bool down) {
    gt2::input::android::KeyEvent(androidKeyCode, down); // the gamepad half of the same event
    const int key = GameKey(androidKeyCode);
    if (key <= 0 || key >= 256) return;
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    if (down && !h.down[size_t(key)]) h.keyDowns.push_back(key);
    h.down[size_t(key)] = down;
}

void MotionEvent(float leftX, float leftY, float rightX, float rightY, float leftTrigger, float rightTrigger, float hatX, float hatY) {
    gt2::input::android::Axes axes;
    axes.leftX = leftX;
    axes.leftY = leftY;
    axes.rightX = rightX;
    axes.rightY = rightY;
    axes.leftTrigger = leftTrigger;
    axes.rightTrigger = rightTrigger;
    axes.hatX = hatX;
    axes.hatY = hatY;
    gt2::input::android::MotionEvent(axes);
}

} // namespace android

namespace {

// ---------------------------------------------------------------- the backend

class AndroidWindow final : public WindowBackend {
public:
    AndroidWindow(const std::string& title, bool deterministic);
    ~AndroidWindow() override;

    gt2view::VkSceneRenderer& Renderer() override { return *renderer_; }
    void* NativeHandle() const override { return nullptr; } // there is no window handle on the headset
    void SetTitle(const std::string& title) override { __android_log_print(ANDROID_LOG_INFO, kTag, "screen: %s", title.c_str()); }

    void Pump() override;
    void ConfigureVr() override;
    void SetDrivingActive(bool active) override { drivingActive_ = active; if (!active) driving_.Reset(); }
    void SetVrMenuActive(bool active) override { vrMenuActive_ = active; driving_.Reset(); steering_ = 0; }
    bool PhysicalSteering(float& value) const override {
        value = steering_; return drivingActive_ && !vrMenuActive_ && OverlayDrivingSettings().mode != 0;
    }
    void AppendDrivingVisuals(std::vector<gt2view::DrawItem>& items) override {
        if (stereoFrame_ && !vrMenuActive_ && drivingActive_) hands_->Append(items, tracking_, driving_, OverlayDrivingSettings(), drivingMatrix_);
    }
    bool TakeTimingReset() override { const bool reset = timingReset_; timingReset_ = false; return reset; }
    void BeginRenderFrame() override;
    void EndRenderFrame() override;
    bool Closed() const override { return closed_ || android::QuitRequested(); }
    void Close() override { closed_ = true; }
    bool Focused() const override { return session_->State() == 5 /* XR_SESSION_STATE_FOCUSED */; }
    std::vector<int> TakeKeyDowns() override;
    bool KeyDown(int key) const override;
    bool TakeDevicesChanged() override { return false; } // the Android pad appears through its first event

    bool XrPaced() const override { return !deterministic_; }
    std::vector<int> RefreshRates() const override {
        std::vector<int> rates;
        for (double rate : session_->Info().refreshRates) rates.push_back(int(rate + 0.5));
        return rates;
    }
    bool SetRefreshRate(int hz) override { return session_->SetRefreshRate(float(hz)); }
    Clock::time_point DisplayTime() const override { return session_->LastDisplayTime(); }
    void SleepUntil(Clock::time_point t) override;
    bool VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const override;

    bool StereoAvailable() const override { return stereo_ && session_->StereoReady() && renderer_->StereoReady(); }
    float StereoAspect() const override {
        const VkExtent2D e = renderer_->StereoExtent();
        return e.height ? float(e.width) / float(e.height) : 4.0f / 3.0f;
    }
    bool BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) override;
    void DrawStereoScene(const std::vector<gt2view::DrawItem>& items, size_t sceneItems, const std::string& shotPath) override {
        const VkImage target = renderer_->EffectiveMsaa() > 1 ? session_->AcquireStereoRenderImage() : VK_NULL_HANDLE;
        renderer_->SetExternalStereoImage(target);
        directFrame_ = target != VK_NULL_HANDLE;
        renderer_->DrawStereo(items, shotPath, sceneItems);
    }
    void CancelStereoScene() override { stereoFrame_ = false; }

private:
    bool WaitForRunning();
    void IdleFrame();
    void UpdateDriving() {
        tracking_ = session_->ControllerTracking();
        if (recenter_.Pending()) tracking_ = {};
        else for (int h = 0; h < 2; ++h) {
            tracking_.gripPose[h] = recenter_.Apply(tracking_.gripPose[h]);
            tracking_.aimPose[h] = recenter_.Apply(tracking_.aimPose[h]);
        }
        steering_ = driving_.Update(tracking_,OverlayDrivingSettings(),drivingActive_ && !vrMenuActive_);
    }
    gt2::vr::DrivingController driving_;
    gt2::vr::TrackedControllers tracking_;
    std::unique_ptr<gt2view::VrDrivingVisuals> hands_;
    bool drivingActive_ = false, vrMenuActive_ = false;
    float steering_ = 0, drivingMatrix_[16]{};

    std::unique_ptr<gt2::xr::Session> session_;
    std::unique_ptr<gt2view::VkContext> vulkan_;
    std::unique_ptr<gt2view::VkSceneRenderer> renderer_;
    bool deterministic_ = false;
    bool closed_ = false;
    bool timingReset_ = false;
    bool frameOpen_ = false;
    bool directFrame_ = false;
    bool everDrawn_ = false;

    bool stereo_ = false;
    bool stereoFrame_ = false;
    bool lastStereo_ = false;
    gt2::vr::Recenter recenter_;
    gt2::vr::Settings rig_;
    gt2::vr::EyeView eyes_[2];
};

AndroidWindow::AndroidWindow(const std::string& title, bool deterministic) : deterministic_(deterministic) {
    gt2::xr::SessionOptions options;
    options.appName = "gt2game";
    options.refreshHz = float(OverlayRefreshRate());
    options.quadWidth = uint32_t(kCinemaWidth);
    options.quadHeight = uint32_t(kCinemaHeight);
    options.androidVm = android::JavaVm();
    options.androidActivity = android::Activity();
    session_ = std::make_unique<gt2::xr::Session>(options);
    const gt2::xr::SessionInfo& info = session_->Info();
    std::printf("xr: runtime \"%s\", system \"%s\", views %ux%u, %.1f Hz%s%s%s\n", info.runtimeName.c_str(), info.systemName.c_str(), info.viewWidth,
                info.viewHeight, info.refreshHz, info.refreshRateExtension ? ", FB display refresh rate" : "",
                info.performanceExtension ? ", EXT performance settings" : "", info.stageSpace ? ", STAGE space" : "");
    std::string rates;
    for (double r : info.refreshRates) rates += (rates.empty() ? "" : ", ") + std::to_string(int(r + 0.5)) + " Hz";
    if (!rates.empty()) std::printf("xr: refresh rates offered: %s\n", rates.c_str());
    std::printf("xr: cinema quad %dx%d (%s)\n", kCinemaWidth, kCinemaHeight, title.c_str());

    const gt2view::VkContext::Existing existing{session_->VulkanInstance(), session_->PhysicalDevice(), session_->Device(), session_->QueueFamily(),
                                                session_->Queue(), session_->Info().shadingRate};
    vulkan_ = std::make_unique<gt2view::VkContext>(existing);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(session_->PhysicalDevice(), &props);
    std::printf("vulkan: %s, API %u.%u.%u, driver %u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
    renderer_ = std::make_unique<gt2view::VkSceneRenderer>(*vulkan_, VkExtent2D{uint32_t(kCinemaWidth), uint32_t(kCinemaHeight)}, session_->RenderFormat());

    hands_ = std::make_unique<gt2view::VrDrivingVisuals>(*renderer_);
    ConfigureVr();
}

void AndroidWindow::ConfigureVr() {
    renderer_->WaitFrame(); // Finish pending direct rendering before swapchain replacement.
    const auto& info = session_->Info();
    session_->SetRefreshRate(float(OverlayRefreshRate()));
    const VrOptions& vr = VrOptionsInUse();
    renderer_->SetFoveation(OverlayFoveation());
    rig_.horizonLock = vr.horizonLock;
    rig_.worldScale = vr.worldScale;
    rig_.nearZ = vr.nearZ;
    rig_.ipd = vr.ipd;
    rig_.originalFov = vr.originalFov;
    for (int i = 0; i < 3; i++) rig_.seat[i] = vr.seat[i];
    if (vr.stereo) {
        uint32_t w = vr.eyeWidth > 0 ? uint32_t(vr.eyeWidth) : uint32_t(std::lround(double(info.viewWidth) * double(vr.renderScale)));
        uint32_t h = vr.eyeHeight > 0 ? uint32_t(vr.eyeHeight) : uint32_t(std::lround(double(info.viewHeight) * double(vr.renderScale)));
        w = std::max(w, 16u);
        h = std::max(h, 16u);
        std::printf("xr: allocating stereo %ux%u per eye, scale %.0f%%\n", w, h, double(vr.renderScale) * 100);
        session_->CreateStereoSwapchains(w, h, true);
        renderer_->CreateStereoTarget(VkExtent2D{w, h}, session_->StereoRenderFormat(), vr.multiview);
        stereo_ = true;
        std::printf("xr: stereo race %ux%u per eye, %s, near %.2f m, horizon lock %.0f %%, world scale %.2f%s\n", w, h,
                    vr.multiview ? "multiview (one pass)" : "two passes", double(vr.nearZ), double(vr.horizonLock) * 100.0, double(vr.worldScale),
                    session_->StereoDepth() ? ", depth layer" : "");
    } else {
        std::printf("xr: stereo off (vr_stereo=0 / --vr-mono): the race stays on the cinema quad\n");
    }
}

AndroidWindow::~AndroidWindow() {
    if (renderer_) renderer_->WaitFrame();
    if (session_) {
        session_->RequestExit();
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!session_->Quit() && Clock::now() < deadline) {
            if (!session_->PollEvents()) break;
            if (session_->Running()) {
                if (session_->BeginFrame()) session_->SubmitFrame(VK_NULL_HANDLE);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        std::printf("xr: %lld compositor frames, session %s\n", session_->FrameIndex(), session_->StateName());
    }
    renderer_.reset();
    vulkan_.reset();
    session_.reset();
}

void AndroidWindow::Pump() {
    if (!session_->PollEvents()) closed_ = true;
    if (!Focused() && !Closed()) {
        gt2::audio::ScopedMixPause pause;
        gt2::input::android::SetXrPad({});
        driving_.Reset(); steering_ = 0;
        while (!Focused() && !Closed()) {
            if (!session_->PollEvents()) { closed_ = true; break; }
            IdleFrame();
            if (!session_->Running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        timingReset_ = true;
    }
    gt2::input::Ps1PadFrame pad;
    session_->ReadController(pad, gt2::input::android::XrVibration());
    UpdateDriving();
    if (!vrMenuActive_ && drivingActive_ && OverlayControlBindings().steeringStick == 1) {
        std::swap(pad.analog[0],pad.analog[2]); std::swap(pad.analog[1],pad.analog[3]);
        pad.buttons &= ~(gt2::input::ps1::kLeft|gt2::input::ps1::kRight|gt2::input::ps1::kUp|gt2::input::ps1::kDown);
    }
    if (!vrMenuActive_ && drivingActive_ && OverlayDrivingSettings().mode != 0) {
        pad.analog = {128,128,128,128};
        pad.buttons &= ~(gt2::input::ps1::kLeft | gt2::input::ps1::kRight | gt2::input::ps1::kUp | gt2::input::ps1::kDown);
    }
    gt2::input::android::SetXrPad(pad);
}

std::vector<int> AndroidWindow::TakeKeyDowns() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    std::vector<int> out;
    out.swap(h.keyDowns);
    return out;
}

bool AndroidWindow::KeyDown(int key) const {
    if (key <= 0 || key >= 256) return false;
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.down[size_t(key)];
}

bool AndroidWindow::WaitForRunning() {
    // The runtime brings the session to READY when the activity is in the foreground; while the player is in the
    // Horizon shell that can take as long as they like, so this waits instead of giving up.
    bool logged = false;
    while (!session_->Running()) {
        if (!session_->PollEvents() || android::QuitRequested()) {
            closed_ = true;
            return false;
        }
        if (session_->Running()) break;
        if (!logged) {
            std::printf("xr: waiting for the runtime to start the session (state %s)\n", session_->StateName());
            logged = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void AndroidWindow::BeginRenderFrame() {
    if (frameOpen_) return;
    if (Closed()) return;
    if (!session_->Running() && !WaitForRunning()) return;
    directFrame_ = false;
    frameOpen_ = session_->BeginFrame();
}

bool AndroidWindow::BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) {
    stereoFrame_ = false;
    if (!StereoAvailable() || !frameOpen_ || !session_->ShouldRender()) return false;
    gt2::vr::Pose head;
    gt2::vr::EyeView eyes[2];
    if (!session_->LocateViews(head, eyes)) return false;
    if (session_->TakeRecenter() || recenter_.Pending()) {
        recenter_.Latch(head);
        driving_.Reset();
        std::printf("xr: seated recenter (reference-space height %.3f m removed, yaw %.1f deg)\n", double(head.position[1]), double(recenter_.Yaw()) * 180.0 / 3.14159265358979323846);
    }
    gt2::vr::EyeView recentred[2] = {eyes[0], eyes[1]};
    for (int v = 0; v < 2; v++) recentred[v].pose = recenter_.Apply(eyes[v].pose);
    view = gt2::vr::Build(camera, recentred, rig_);
    if (!view.valid) return false;
    UpdateDriving();
    std::fill(std::begin(drivingMatrix_),std::end(drivingMatrix_),0.f); drivingMatrix_[15] = 1;
    for (int axis = 0; axis < 3; ++axis) for (int k = 0; k < 3; ++k)
        drivingMatrix_[axis*4+k] = view.levelled[axis*3+k]*rig_.worldScale;
    for (int k = 0; k < 3; ++k) {
        drivingMatrix_[12+k] = camera.eye[k]-view.refEye[k];
        for (int axis = 0; axis < 3; ++axis) drivingMatrix_[12+k] += view.levelled[axis*3+k]*rig_.seat[axis];
    }
    gt2view::StereoViews matrices;
    gt2::vr::HudProjection(head, eyes, matrices.hudVP);
    std::memcpy(matrices.worldVP, view.worldVP, sizeof(matrices.worldVP));
    std::memcpy(matrices.skyVP, view.skyVP, sizeof(matrices.skyVP));
    renderer_->SetStereoViews(matrices);
    renderer_->BeginFrameUploads();
    for (int v = 0; v < 2; v++) {
        eyes_[v].pose = eyes[v].pose;
        eyes_[v].fov = view.fov[v];
    }
    stereoFrame_ = true;
    return true;
}

void AndroidWindow::EndRenderFrame() {
    everDrawn_ = !stereoFrame_ || everDrawn_;
    // Vulkan enable2 permits release with rendering still queued on the bound VkQueue.
    // All queue calls are on this thread; staged buffer writes wait in DrawStereo.
    // The copy paths keep their existing completion requirement.
    if (!stereoFrame_ || !directFrame_) renderer_->WaitFrame();
    if (frameOpen_) {
        if (stereoFrame_) {
            const VkImage depth = renderer_->EffectiveMsaa() == 1 ? renderer_->StereoDepthImage() : VK_NULL_HANDLE;
            session_->SubmitStereoFrame(renderer_->StereoImage(), depth, eyes_, rig_.nearZ);
        } else {
            session_->SubmitFrame(renderer_->OffscreenImage());
        }
    }
    lastStereo_ = stereoFrame_;
    stereoFrame_ = false;
    frameOpen_ = false;
}

void AndroidWindow::IdleFrame() {
    if (!session_->Running() || !session_->BeginFrame()) return;
    if (lastStereo_ && renderer_->StereoReady()) {
        const VkImage depth = renderer_->EffectiveMsaa() == 1 ? renderer_->StereoDepthImage() : VK_NULL_HANDLE;
        session_->SubmitStereoFrame(renderer_->StereoImage(), depth, eyes_, rig_.nearZ);
        return;
    }
    session_->SubmitFrame(everDrawn_ ? renderer_->OffscreenImage() : VK_NULL_HANDLE);
}

void AndroidWindow::SleepUntil(Clock::time_point t) {
    if (deterministic_) return;
    const Clock::duration period = session_->Period();
    while (session_->Running() && !Closed() && period > Clock::duration::zero() && session_->LastDisplayTime() < t) {
        IdleFrame();
        if (!session_->PollEvents()) {
            closed_ = true;
            return;
        }
    }
    // The compositor's frames are the clock; whatever is left of the field is a plain sleep.
    const Clock::duration left = t - Clock::now();
    if (left > std::chrono::microseconds(200)) std::this_thread::sleep_for(left - std::chrono::microseconds(100));
    while (Clock::now() < t) std::this_thread::yield();
}

bool AndroidWindow::VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const {
    if (!session_->Running() || session_->Period() <= Clock::duration::zero()) return false;
    vblank = session_->LastDisplayTime();
    period = session_->Period();
    return period > std::chrono::milliseconds(2) && period < std::chrono::milliseconds(50);
}

} // namespace

std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string& title, int, int) {
    return std::make_unique<AndroidWindow>(title, VrDeterministic());
}

// There is no desktop window on the headset. The Android build runs in VR only (android_main passes --vr), so
// anything that asks for a plain window is a mistake to report, not something to fake.
std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string&, int, int) {
    throw std::runtime_error("the Android build has no desktop window: every screen runs in the OpenXR session (--vr)");
}

std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string&, int, int) {
    throw std::runtime_error("the Android build has no desktop window for the keyboard: input comes from the activity");
}

} // namespace gt2game
