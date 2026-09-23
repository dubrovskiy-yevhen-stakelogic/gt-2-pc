#pragma once
#include "frame_profiler.h"
#include "frame_profiler_log.h"
#include "platform/os/keys.h"
// The one window of gt2game: the platform's window, the Vulkan renderer on it, the keyboard, the scripted input of
// automated runs and the frame pacing. The menus (menu_mode.h), the race (race_view.h) and the native panels between
// them (panel.h) all draw into it, so that starting an event from the menus switches to the race in the same window.
//
// Input: every screen reads keys through Held / Pressed. A key is held when it is down on the keyboard (window
// focused) or when the input script holds it in the current field; Pressed = went down since the previous field
// (keyboard key-down messages or a scripted press starting). The script ("--script") uses gt2play's syntax
// "field:key[:hold],..." with fields = presentation frames counted from the first frame of the program (the menus, the
// race and the panels all count the same frames). Key names: up down left right, enter / cross, space / circle,
// backspace / triangle, delete / square, esc, s / start, r, q, a, c, f5, shift, and single letters / digits.
//
// Controllers (src/platform/input): the window polls the PC pads once per field into the virtual PS1 controller of
// port 1 (Pad()). For the screens that read keys, the pad's buttons also act as the keys of the keyboard convention of
// every menu (D-pad = arrows, Cross = Enter, Circle = Space, Triangle = Backspace, Square = Delete, Start = S, L1 = Q,
// R1 = W): Held / Pressed / PressedKeys include them. KeyHeld / KeyPressed are the keyboard (and key script) alone -
// the race drives from the pad through the original's logical pad (race_view.cpp) and must not see them twice.
//
// Platforms (docs/research/vr_port_plan.md, M0): everything above the operating system - the input latching, the key
// script, the field clock, the screenshots and the pacing - is in game_window.cpp; the window itself, its renderer,
// the keyboard and the frame timing are a WindowBackend, today game_window_win32.cpp.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gt2view/vk_scene_renderer.h"
#include "platform/input/input_system.h"
#include "platform/xr/vr_rig.h"

namespace gt2game {

struct ScriptKey {
    int field = 0, hold = 1;
    int key = 0; // a key code of platform/os/keys.h
};
// "field:key[:hold],..." -> presses; throws on a malformed item.
std::vector<ScriptKey> ParseKeyScript(const std::string& script);
// The key name of the script syntax -> a key code of platform/os/keys.h (0 = unknown).
int ScriptKeyCode(const std::string& name);

// Process-wide controller options (main.cpp), applied to every window created afterwards: --fake-pad (a scripted
// controller, platform/input/input_system.h) and the motor strength of our settings (0..100 %).
void SetFakePadScript(const std::string& script);
bool FakePadGiven();
// --fake-pad2: a scripted controller in port 2 (the 2 player Battle's second player).
void SetFakePad2Script(const std::string& script);

// Player 2's keyboard (ours; docs/research/arcade_disc.md section 19): a PS1 digital pad on the right-hand letter block,
// merged with port 2's controller - I / K / J / L the D-pad (up / down / left / right), U Cross (choose / accelerate), O Square
// (brake), P Circle (handbrake), Y Triangle (back / reverse), 9 / 8 R2 / L2 (shift up / down), 0 / 7 R1 / L1 (view / look back),
// T Start (pause).
constexpr std::pair<uint16_t, int> kPlayer2Keys[] = {
    {gt2::input::ps1::kUp, 'I'}, {gt2::input::ps1::kDown, 'K'}, {gt2::input::ps1::kLeft, 'J'}, {gt2::input::ps1::kRight, 'L'},
    {gt2::input::ps1::kCross, 'U'}, {gt2::input::ps1::kSquare, 'O'}, {gt2::input::ps1::kCircle, 'P'}, {gt2::input::ps1::kTriangle, 'Y'},
    {gt2::input::ps1::kR2, '9'}, {gt2::input::ps1::kL2, '8'}, {gt2::input::ps1::kR1, '0'}, {gt2::input::ps1::kL1, '7'}, {gt2::input::ps1::kStart, 'T'},
};
void SetRumbleScale(int percent);

// Automated runs (a key script, a screenshot, --fast, --headless, a comparison or a check, a frame limit) must not
// steal the keyboard: their window is shown without being activated, so the user can keep working. Interactive
// launches activate their window as before. The environment variable GT2_NO_FOCUS forces it as well. Call before the
// first GameWindow.
void SetWindowNoFocus(bool on);
bool WindowNoFocus();
void SetWindowedMode(bool on);
bool WindowedMode();

// The operating system's half of the window: the window itself, the Vulkan renderer on it, the keyboard and the
// frame timing. One implementation per platform (game_window_win32.cpp; the XR session of M1 adds its own).
class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    virtual gt2view::VkSceneRenderer& Renderer() = 0;
    // The platform's window handle (HWND on Windows) for the input devices; null when there is none.
    virtual void* NativeHandle() const = 0;
    virtual void SetTitle(const std::string& title) = 0;

    // Services the event queue; called once at the start of every presentation frame.
    virtual void Pump() = 0;
    virtual bool TakeTimingReset() { return false; }
    // Around the renderer's frame (GameWindow::EndFrame / Present). The window backend does nothing: its Draw
    // acquires and presents a swapchain image by itself. The XR backend runs xrWaitFrame / xrBeginFrame before the
    // frame is recorded and xrEndFrame with the cinema quad after it.
    virtual void BeginRenderFrame() {}
    virtual void ConfigureVr() {}
    virtual void SetDrivingActive(bool) {}
    virtual void SetVrMenuActive(bool) {}
    virtual bool PhysicalSteering(float&) const { return false; }
    virtual void AppendDrivingVisuals(std::vector<gt2view::DrawItem>&, const float* = nullptr) {}
    virtual void EndRenderFrame() {}
    virtual bool Closed() const = 0;
    virtual void Close() = 0;
    virtual bool Focused() const = 0;
    // The keys (platform/os/keys.h) that went down since the previous call, in order.
    virtual std::vector<int> TakeKeyDowns() = 0;
    // Whether the key is down on the keyboard right now.
    virtual bool KeyDown(int key) const = 0;
    // True once after the system reported a controller being plugged in or out.
    virtual bool TakeDevicesChanged() = 0;
    virtual std::unique_ptr<gt2::input::Device> CreateInputDevice() { return {}; }

    // True when the frames are paced by a compositor whose display times the caller should render for (the XR
    // backend outside --xr-deterministic): the race's display-rate loop then runs one frame per display period.
    virtual bool XrPaced() const { return false; }
    virtual std::vector<int> RefreshRates() const { return {}; }
    virtual bool SetRefreshRate(int) { return false; }
    // The time the frame being recorded will be shown at (XR: predictedDisplayTime); now, without a compositor.
    virtual std::chrono::steady_clock::time_point DisplayTime() const { return std::chrono::steady_clock::now(); }

    // Sleeps until `t` to within ~0.1 ms (GameWindow::SleepUntil).
    virtual void SleepUntil(std::chrono::steady_clock::time_point t) = 0;
    // The display's refresh timing (GameWindow::VBlankTiming); false when the system does not report it.
    virtual bool VBlankTiming(std::chrono::steady_clock::time_point& vblank, std::chrono::steady_clock::duration& period) const = 0;

    // ---- stereo (docs/research/vr_port_plan.md, M2; only the XR backend) ----
    // True when the race can be drawn as an XrCompositionLayerProjection instead of the mono cinema quad.
    virtual bool StereoAvailable() const { return false; }
    // Width / height of one eye image, used by the stereo scene builders.
    virtual float StereoAspect() const { return 4.0f / 3.0f; }
    // Inside an open compositor frame (BeginRenderFrame): locates the head, runs the VR rig on the original camera
    // `camera` of the frame being built and hands the caller the mid eye and the per-eye matrices. False when the
    // runtime gave no views - the caller then builds a mono frame for the cinema quad as before.
    virtual bool BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) {
        (void)camera;
        (void)view;
        return false;
    }
    // Records the draw list into the stereo target (once for both eyes) instead of the mono Draw. EndRenderFrame
    // then submits the projection layer.
    virtual void DrawStereoScene(const std::vector<gt2view::DrawItem>& items, size_t sceneItems, const std::string& shotPath) {
        (void)items;
        (void)sceneItems;
        (void)shotPath;
    }
    // Drops a stereo frame that was begun but is not going to be drawn (the frame falls back to the quad).
    virtual void CancelStereoScene() {}
};

// The platform's backend with its window of `clientWidth` x `clientHeight` (game_window_win32.cpp).
std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string& title, int clientWidth, int clientHeight);
// The same window, its keyboard and its timing, but without a renderer (Renderer() throws): the XR backend keeps it
// as the desktop window that the keyboard and the controllers are read from.
std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string& title, int clientWidth, int clientHeight);
// --vr (docs/research/vr_port_plan.md, M1): every window of the program is an OpenXR session showing the game on the
// cinema quad (game_window_xr.cpp). `deterministic` = --xr-deterministic: one field per compositor frame and no
// pacing of its own, so that a run is reproducible frame by frame. Call before the first GameWindow.
void SetVrMode(bool on, bool deterministic);
bool VrMode();
void ReleaseRetainedWindow();
bool VrDeterministic();
// The XR backend (game_window_xr.cpp); throws with the reason when there is no OpenXR loader or runtime.
std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string& title, int clientWidth, int clientHeight);

// The VR options in effect (docs/research/vr_port_plan.md, M2): settings.txt's vr_* keys with the command line on
// top. game_main.cpp sets them before the first GameWindow; the XR backend and the VR rig read them.
struct VrOptions {
    bool stereo = true;        // the race as a stereo projection layer (--vr-mono / vr_stereo=0: M1's cinema quad)
    bool multiview = false;    // one pass with a view mask instead of one pass per eye
    float horizonLock = 0.6f;  // 0..1
    float worldScale = 1.0f;   // game metres per real metre
    float renderScale = 1.0f;  // of the runtime's recommended per-eye image
    float nearZ = 0.05f;
    float seat[3] = {0, 0, 0}; // metres along the levelled camera's right / up / back axes
    // Checks only (tests/xr): the eye separation forced to a value (0 = both eyes in one place), the original
    // camera's frustum instead of the runtime's, a fixed per-eye image size, and a CSV of the rig's output.
    float ipd = -1.0f;
    bool originalFov = false;
    int eyeWidth = 0, eyeHeight = 0;
    std::string poseLog;
};
void SetVrOptions(const VrOptions& options);
const VrOptions& VrOptionsInUse();
// The size every screen is rendered at in VR: the cinema quad's image (4:3, as the game's 2D builders expect).
constexpr int kCinemaWidth = 1280, kCinemaHeight = 960;

class GameWindow {
public:
    // `title`: the window caption; `clientWidth` x `clientHeight` = the client area.
    GameWindow(const std::string& title, int clientWidth, int clientHeight);
    ~GameWindow();
    void RetainBackendForNextWindow();
    void EnableNativeMenu(bool on) { nativeMenuEnabled_ = on; }
    void RequestGamePause() { gamePauseRequested_ = true; }
    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    // The window's caption (the screens name themselves).
    void SetTitle(const std::string& title) { backend_->SetTitle(title); }
    gt2view::VkSceneRenderer& Renderer() { return backend_->Renderer(); }

    // Adds scripted presses (may be called several times; all use the global field numbering).
    void AddScript(const std::string& script);
    bool Scripted() const { return !script_.empty(); }
    // The last field the script refers to (its last press + hold); 0 without a script.
    int ScriptEnd() const;

    // One presentation frame: pumps the window messages and latches the input of this field. False once the window
    // is closed. Call once per frame before reading the input.
    bool BeginFrame();
    // Presents `items`; `shotPath` non-empty = also save the frame as PNG. Then waits for the frame's time slot
    // (`frameTime`, unless pacing is off) and advances the field counter.
    // `sceneItems`: items[0, sceneItems) are the 3D scene (VkSceneRenderer::Draw: the graphics options' render scale /
    // MSAA / texture options apply to them).
    void EndFrame(const std::vector<gt2view::DrawItem>& items, const std::string& shotPath = {},
                  std::chrono::nanoseconds frameTime = std::chrono::nanoseconds(16'666'667), size_t sceneItems = 0);
    // High frame rates (race_view.cpp): the field advances without presenting or waiting; the caller presents with
    // Present() at its own cadence until NextField(). A field that starts late keeps its scheduled time while it is at most
    // `maxLag` behind (the following fields catch up: no game time is lost to a blocking present); beyond that the clock
    // restarts at now.
    void SkipFrame(std::chrono::nanoseconds frameTime = std::chrono::nanoseconds(16'666'667),
                   std::chrono::nanoseconds maxLag = std::chrono::nanoseconds(0));
    void Present(const std::vector<gt2view::DrawItem>& items, size_t sceneItems) {
        BeginPresent();
        FinishPresent(items, sceneItems);
    }
    // The compositor's frames as the clock (XrPaced): BeginPresent blocks until the runtime wants the next frame,
    // DisplayTime is when that frame will be shown (build it for that moment), FinishPresent records and submits it.
    bool XrPaced() const { return backend_->XrPaced(); }
    std::vector<int> RefreshRates() const { return backend_->RefreshRates(); }
    bool SetRefreshRate(int hz) { return backend_->SetRefreshRate(hz); }
    void BeginPresent();
    bool ProfilerLogFailed() const { return profilerLog_.Failed(); }
    std::chrono::steady_clock::time_point DisplayTime() const { return backend_->DisplayTime(); }
    void FinishPresent(const std::vector<gt2view::DrawItem>& items, size_t sceneItems);

    // Stereo (M2). The race view asks for it once per compositor frame, between BeginPresent and the frame's build;
    // the next FinishPresent / EndFrame then records the list into the stereo target instead of the mono image.
    bool StereoAvailable() const { return backend_->StereoAvailable(); }
    float StereoAspect() const { return backend_->StereoAspect(); }
    bool BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) {
        stereo_ = backend_->BeginStereoScene(camera, view);
        return stereo_;
    }
    bool StereoPending() const { return stereo_; }
    void CancelStereoScene() {
        if (stereo_) backend_->CancelStereoScene();
        stereo_ = false;
    }
    // Whether the next EndFrame will really present (--fast presents only saved frames and one per second).
    bool WillPresent(const std::string& shotPath = {}) const;
    // The time of the next field (with pacing).
    std::chrono::steady_clock::time_point NextField() const { return next_; }
    // Sleeps until `t` to within ~0.1 ms: a high-resolution waitable timer, then a short spin. (std::this_thread::sleep_until
    // rounds to the system timer; Windows 11 ignores timeBeginPeriod for occluded windows, so that can be 15.6 ms.)
    void SleepUntil(std::chrono::steady_clock::time_point t);
    // The compositor's refresh timing (DwmGetCompositionTimingInfo): the time of a recent vertical blank and the refresh
    // period, on the steady clock. False when the compositor does not report it.
    bool VBlankTiming(std::chrono::steady_clock::time_point& vblank, std::chrono::steady_clock::duration& period) const;
    // Restarts the pacing clock (after a long load, so that the next frames do not rush to catch up).
    void ResetPacing() { next_ = Clock::now(); }
    void SetDrivingActive(bool on) { backend_->SetDrivingActive(on); }
    bool PhysicalSteering(float& value) const { return backend_->PhysicalSteering(value); }
    void AppendDrivingVisuals(std::vector<gt2view::DrawItem>& items, const float* vehicleFrame = nullptr) {
        backend_->AppendDrivingVisuals(items, vehicleFrame);
    }
    // --fast: no frame pacing (automated runs render as fast as the GPU allows).
    void SetPacing(bool on) { pacing_ = on; }
    bool Pacing() const { return pacing_; }

    int Field() const { return field_; }
    uint64_t ClockRevision() const { return clockRevision_; }
    bool Closed() const { return backend_->Closed(); }
    void Close() { backend_->Close(); }

    bool Held(int key) const;
    bool Pressed(int key) const;
    // Every key that went down in this field (keyboard, script and the pad's menu keys), in order.
    const std::vector<int>& PressedKeys() const { return pressedNow_; }
    // The keyboard and the key script only (no pad).
    bool KeyHeld(int key) const;
    bool KeyPressed(int key) const;

    // The controllers (src/platform/input): port 1 of this field and its buttons (ps1::k*) held / newly pressed.
    gt2::input::InputSystem& Input() { return input_; }
    const gt2::input::Ps1PadFrame& Pad() const { return input_.Port1(); }
    bool PadHeld(uint16_t ps1Buttons) const { return (input_.Port1().buttons & ps1Buttons) != 0; }
    bool PadPressed(uint16_t ps1Buttons) const { return (padPressed_ & ps1Buttons) != 0; }
    // Port 2 of this field: the second controller (InputSystem::Port2) merged with player 2's keyboard (kPlayer2Keys; a
    // digital pad when no controller is in port 2), and its newly pressed buttons.
    const gt2::input::Ps1PadFrame& Pad2() const { return pad2_; }
    bool Pad2Pressed(uint16_t ps1Buttons) const { return (pad2Pressed_ & ps1Buttons) != 0; }

    // Global screenshots: --shot-at <field> <png> (taken by EndFrame in whatever screen is showing).
    void AddShot(int field, const std::string& path) { shots_.emplace_back(field, path); }
    int LastShotField() const;

private:
    using Clock = std::chrono::steady_clock;
    void DrawFrame(const std::vector<gt2view::DrawItem>& items, size_t sceneItems, const std::string& path);
    FrameProfiler frameProfiler_;
    FrameProfilerLog profilerLog_;
    Clock::time_point frameBuildStart_{};
    double frameBeginWaitMs_ = -1;
    bool frameBuildTimed_ = false;

    std::unique_ptr<WindowBackend> backend_;
    std::vector<ScriptKey> script_;
    std::vector<int> pressedNow_; // the field's presses (with the pad's menu keys)
    std::vector<int> keyPressedNow_; // ... of the keyboard and the script alone
    std::vector<int> scriptHeld_; // keys the script holds in this field
    std::vector<int> scriptHeldBefore_; // ... and in the previous field (a scripted press = held now, not before)
    std::vector<std::pair<int, std::string>> shots_;
    bool overlayActive_ = false;
    bool nativeMenuEnabled_ = true;
    bool gamePauseRequested_ = false;
    std::vector<gt2view::DrawItem> lastItems_;
    size_t lastScene_ = 0;
    bool focused_ = false;
    bool pacing_ = true;
    bool stereo_ = false; // a stereo frame was begun and is waiting for its draw list (M2)
    int field_ = 0;
    uint64_t clockRevision_ = 0;
    Clock::time_point next_ = Clock::now();
    gt2::input::InputSystem input_;
    uint16_t padPrevious_ = 0, padPressed_ = 0;
    gt2::input::Ps1PadFrame pad2_;
    uint16_t pad2Previous_ = 0, pad2Pressed_ = 0;
};

} // namespace gt2game
