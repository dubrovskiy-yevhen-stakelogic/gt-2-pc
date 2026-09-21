// The one window of gt2game, above the operating system (see game_window.h): the key script, the input latching of a
// field, the controllers, the screenshots and the frame pacing. The window itself is a WindowBackend
// (game_window_win32.cpp).
#include "game_window.h"
#include "pc_overlay.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <thread>

#include "platform/os/keys.h"

namespace gt2game {

namespace {
namespace ps1 = gt2::input::ps1;
namespace keys = gt2::keys;
// The pad's buttons as the keys of the menus' keyboard convention (game_window.h).
constexpr std::pair<uint16_t, int> kPadKeys[] = {
    {ps1::kUp, keys::kUp},       {ps1::kDown, keys::kDown},        {ps1::kLeft, keys::kLeft},    {ps1::kRight, keys::kRight}, {ps1::kCross, keys::kReturn}, {ps1::kCircle, keys::kSpace},
    {ps1::kTriangle, keys::kBack}, {ps1::kSquare, keys::kDelete}, {ps1::kStart, int('S')}, {ps1::kL1, int('Q')},     {ps1::kR1, int('W')},
};
std::string g_fakePad, g_fakePad2;
int g_rumbleScale = 100;
bool g_vr = false, g_vrDeterministic = false;
bool g_noFocus = false;
VrOptions g_vrOptions;
std::unique_ptr<WindowBackend> retainedBackend;
} // namespace

void SetVrOptions(const VrOptions& options) { g_vrOptions = options; }
const VrOptions& VrOptionsInUse() { return g_vrOptions; }

void SetWindowNoFocus(bool on) { g_noFocus = on; }
bool WindowNoFocus() { return g_noFocus; }

void SetVrMode(bool on, bool deterministic) {
    g_vr = on;
    g_vrDeterministic = deterministic;
}
bool VrMode() { return g_vr; }
bool VrDeterministic() { return g_vrDeterministic; }

void SetFakePadScript(const std::string& script) { g_fakePad = script; }
void SetFakePad2Script(const std::string& script) { g_fakePad2 = script; }
bool FakePadGiven() { return !g_fakePad.empty(); }
void SetRumbleScale(int percent) { g_rumbleScale = percent; }

int ScriptKeyCode(const std::string& raw) {
    std::string name = raw;
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, int> kNames = {
        {"up", keys::kUp}, {"down", keys::kDown}, {"left", keys::kLeft}, {"right", keys::kRight},
        {"enter", keys::kReturn}, {"cross", keys::kReturn}, {"space", keys::kSpace}, {"circle", keys::kSpace},
        {"backspace", keys::kBack}, {"back", keys::kBack}, {"triangle", keys::kBack}, {"delete", keys::kDelete}, {"square", keys::kDelete},
        {"esc", keys::kEscape}, {"escape", keys::kEscape}, {"start", 'S'}, {"shift", keys::kShift}, {"f5", keys::kF5}, {"f10", keys::kF10},
    };
    const auto it = kNames.find(name);
    if (it != kNames.end()) return it->second;
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return std::toupper(static_cast<unsigned char>(name[0]));
    return 0;
}

std::vector<ScriptKey> ParseKeyScript(const std::string& s) {
    std::vector<ScriptKey> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) throw std::runtime_error("bad script item " + item);
        const size_t c2 = item.find(':', c1 + 1);
        ScriptKey k;
        k.field = std::atoi(item.substr(0, c1).c_str());
        const std::string key = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        k.key = ScriptKeyCode(key);
        if (k.key == 0) throw std::runtime_error("bad script key " + key);
        if (c2 != std::string::npos) k.hold = std::max(1, std::atoi(item.substr(c2 + 1).c_str()));
        out.push_back(k);
    }
    return out;
}

GameWindow::GameWindow(const std::string& title, int clientWidth, int clientHeight) {
    if (retainedBackend) {
        backend_ = std::move(retainedBackend); backend_->ConfigureVr(); backend_->SetTitle(title);
    } else backend_ = g_vr ? CreateXrWindowBackend(title, clientWidth, clientHeight) : CreateWindowBackend(title, clientWidth, clientHeight);
    PrepareNativeUi(backend_->Renderer());
    input_.AttachWindow(backend_->NativeHandle());
    input_.AddDevice(backend_->CreateInputDevice());
    input_.SetRumbleScale(g_rumbleScale);
    if (OverlayRumbleStrength() >= 0) input_.SetRumbleScale(OverlayRumbleStrength());
    if (!g_fakePad.empty()) input_.AddFakePad(g_fakePad);
    if (!g_fakePad2.empty()) input_.AddFakePad(g_fakePad2, 2);
    next_ = Clock::now();
}

GameWindow::~GameWindow() = default;
void GameWindow::RetainBackendForNextWindow() { input_.StopFeedback(); retainedBackend = std::move(backend_); }
void ReleaseRetainedWindow() { retainedBackend.reset(); }

void GameWindow::AddScript(const std::string& script) {
    const std::vector<ScriptKey> keys = ParseKeyScript(script);
    script_.insert(script_.end(), keys.begin(), keys.end());
}

int GameWindow::ScriptEnd() const {
    int end = 0;
    for (const ScriptKey& k : script_) end = std::max(end, k.field + k.hold);
    return end;
}

int GameWindow::LastShotField() const {
    int last = -1;
    for (const auto& s : shots_) last = std::max(last, s.first);
    return last;
}

bool GameWindow::BeginFrame() {
    backend_->Pump();
    if (backend_->TakeTimingReset()) { ResetPacing(); frameProfiler_.Reset(); ++clockRevision_; }
    if (backend_->TakeDevicesChanged()) input_.DevicesChanged(); // a controller plugged in / out
    focused_ = backend_->Focused();
    pressedNow_.clear();
    const std::vector<int> keyDowns = backend_->TakeKeyDowns();
    if (focused_) pressedNow_ = keyDowns;
    scriptHeldBefore_.swap(scriptHeld_);
    scriptHeld_.clear();
    for (const ScriptKey& k : script_)
        if (field_ >= k.field && field_ < k.field + k.hold) scriptHeld_.push_back(k.key);
    for (int key : scriptHeld_)
        if (std::find(scriptHeldBefore_.begin(), scriptHeldBefore_.end(), key) == scriptHeldBefore_.end() &&
            std::find(pressedNow_.begin(), pressedNow_.end(), key) == pressedNow_.end())
            pressedNow_.push_back(key);
    keyPressedNow_ = pressedNow_;
    // The controllers: port 1 of this field; its newly pressed buttons also press the menus' keys.
    input_.Poll(field_, focused_);
    for (const std::string& line : input_.TakeLog()) std::printf("input: %s\n", line.c_str());
    const uint16_t buttons = input_.Port1().buttons;
    padPressed_ = uint16_t(buttons & ~padPrevious_);
    padPrevious_ = buttons;
    for (const auto& [bit, key] : kPadKeys)
        if ((padPressed_ & bit) && std::find(pressedNow_.begin(), pressedNow_.end(), key) == pressedNow_.end()) pressedNow_.push_back(key);
    // Port 2: the second controller and player 2's keys (a digital pad when no controller is there).
    pad2_ = input_.Port2();
    uint16_t keys2 = 0;
    for (const auto& [bit, key] : kPlayer2Keys)
        if (KeyHeld(key)) keys2 = uint16_t(keys2 | bit);
    if (keys2 && pad2_.type == gt2::input::kTypeNone) pad2_.type = gt2::input::kTypeDigital;
    pad2_.buttons = uint16_t(pad2_.buttons | keys2);
    if (pad2_.type == gt2::input::kTypeNone) pad2_.type = gt2::input::kTypeDigital; // the keyboard is always there
    pad2Pressed_ = uint16_t(pad2_.buttons & ~pad2Previous_);
    pad2Previous_ = pad2_.buttons;
    if (nativeMenuEnabled_ && !overlayActive_ && (Pressed(gt2::keys::kF10) || (PadHeld(ps1::kSelect) && PadPressed(ps1::kStart)))) {
        overlayActive_ = true;
        backend_->SetVrMenuActive(true);
        ShowPcOverlay(*this, VrMode() ? std::vector<gt2view::DrawItem>{} : lastItems_, VrMode() ? 0 : lastScene_);
        overlayActive_ = false;
        backend_->SetVrMenuActive(false);
        pressedNow_.clear(); keyPressedNow_.clear(); scriptHeld_.clear();
        padPressed_ = pad2Pressed_ = 0;
        if (gamePauseRequested_) {
            gamePauseRequested_ = false;
            padPressed_ = ps1::kStart;
            pressedNow_.push_back('S');
        }
    }
    return !backend_->Closed();
}

bool GameWindow::KeyHeld(int key) const {
    if (std::find(scriptHeld_.begin(), scriptHeld_.end(), key) != scriptHeld_.end()) return true;
    return focused_ && backend_->KeyDown(key);
}

bool GameWindow::KeyPressed(int key) const { return std::find(keyPressedNow_.begin(), keyPressedNow_.end(), key) != keyPressedNow_.end(); }

bool GameWindow::Held(int key) const {
    if (KeyHeld(key)) return true;
    const uint16_t buttons = input_.Port1().buttons;
    for (const auto& [bit, k] : kPadKeys)
        if (k == key && (buttons & bit)) return true;
    return false;
}

bool GameWindow::Pressed(int key) const { return std::find(pressedNow_.begin(), pressedNow_.end(), key) != pressedNow_.end(); }

void GameWindow::SleepUntil(Clock::time_point t) { backend_->SleepUntil(t); }

bool GameWindow::VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const { return backend_->VBlankTiming(vblank, period); }

void GameWindow::SkipFrame(std::chrono::nanoseconds frameTime, std::chrono::nanoseconds maxLag) {
    field_++;
    if (!pacing_) return;
    next_ += frameTime;
    const auto now = Clock::now();
    if (next_ + maxLag < now) next_ = now;
}

bool GameWindow::WillPresent(const std::string& shotPath) const {
    std::string path = shotPath;
    for (const auto& s : shots_)
        if (s.first == field_ && path.empty()) path = s.second;
    return pacing_ || !path.empty() || field_ % 60 == 0;
}

void GameWindow::FinishPresent(const std::vector<gt2view::DrawItem>& items, size_t sceneItems) {
    if (!overlayActive_) { lastItems_ = items; lastScene_ = sceneItems; }
    DrawFrame(items, sceneItems, {});
}

void GameWindow::DrawFrame(const std::vector<gt2view::DrawItem>& items, size_t sceneItems, const std::string& path) {
    const auto* draw = &items;
    std::vector<gt2view::DrawItem> withProfiler;
    if (FrameProfilerEnabled()) {
        frameProfiler_.Record(std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
        withProfiler = items;
        AppendFrameProfiler(backend_->Renderer(), withProfiler, frameProfiler_);
        draw = &withProfiler;
    } else frameProfiler_.Reset();
    if (stereo_) {
        backend_->DrawStereoScene(*draw, sceneItems, path);
        stereo_ = false;
    } else backend_->Renderer().Draw(*draw, path, sceneItems);
    backend_->EndRenderFrame();
}

void GameWindow::EndFrame(const std::vector<gt2view::DrawItem>& items, const std::string& shotPath, std::chrono::nanoseconds frameTime, size_t sceneItems) {
    if (!overlayActive_) { lastItems_ = items; lastScene_ = sceneItems; }
    std::string path = shotPath;
    for (const auto& s : shots_)
        if (s.first == field_ && path.empty()) path = s.second;
    // --fast: only the frames that are saved (and one per second, to keep the window alive) are presented - with the
    // display off or the window hidden, the compositor throttles presentation to a few frames per second.
    if (pacing_ || !path.empty() || field_ % 60 == 0) {
        backend_->BeginRenderFrame();
        DrawFrame(items, sceneItems, path);
    } else {
        CancelStereoScene();
    }
    if (!path.empty()) std::printf("frame %d -> %s\n", field_, path.c_str());
    field_++;
    if (!pacing_) return;
    next_ += frameTime;
    const auto now = Clock::now();
    if (next_ > now) backend_->SleepUntil(next_);
    else next_ = now;
}

} // namespace gt2game
