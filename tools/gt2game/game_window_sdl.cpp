#include "game_window.h"
#include "gt2view/vk_context.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gt2game {
namespace {
using Clock = std::chrono::steady_clock;
int Key(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A' + code - SDL_SCANCODE_A;
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return '1' + code - SDL_SCANCODE_1;
    using namespace gt2::keys;
    switch (code) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return kReturn;
    case SDL_SCANCODE_BACKSPACE: return kBack;
    case SDL_SCANCODE_DELETE: return kDelete;
    case SDL_SCANCODE_ESCAPE: return kEscape;
    case SDL_SCANCODE_SPACE: return kSpace;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return kShift;
    case SDL_SCANCODE_PAGEUP: return kPageUp;
    case SDL_SCANCODE_PAGEDOWN: return kPageDown;
    case SDL_SCANCODE_HOME: return kHome;
    case SDL_SCANCODE_LEFT: return kLeft;
    case SDL_SCANCODE_RIGHT: return kRight;
    case SDL_SCANCODE_UP: return kUp;
    case SDL_SCANCODE_DOWN: return kDown;
    case SDL_SCANCODE_F5: return kF5;
    case SDL_SCANCODE_F8: return kF8;
    case SDL_SCANCODE_F10: return kF10;
    default: return 0;
    }
}
class SdlWindow final : public WindowBackend {
public:
    SdlWindow(const std::string& title, int width, int height, bool render) {
#ifdef __APPLE__
        // M1 Pro / MoltenVK 1.4.2 captures lose menu-car materials with argument
        // buffers. Set this before SDL loads Vulkan; explicit diagnostic overrides
        // remain available. Fast math retains the driver's normal default.
        if (SDL_setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0) != 0)
            throw std::runtime_error("Could not configure MoltenVK resource bindings");
        std::printf("graphics: MoltenVK argument buffers %s\n", SDL_getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS"));
#endif
        const bool noFocus = WindowNoFocus() || std::getenv("GT2_NO_FOCUS");
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, noFocus ? "1" : "0");
        // Keep fullscreen on the existing desktop instead of a Cocoa Space.
        // This uses the same presentation path as the working windowed mode.
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        try {
            SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, noFocus ? "1" : "0");
            Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
            if (render) flags |= SDL_WINDOW_VULKAN;
            if (render && !noFocus && !WindowedMode()) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
            if (!window_) throw std::runtime_error(SDL_GetError());
            if (render) {
                context_ = std::make_unique<gt2view::VkContext>(nullptr, window_);
                renderer_ = std::make_unique<gt2view::VkSceneRenderer>(*context_);
            }
        } catch (...) {
            renderer_.reset(); context_.reset();
            if (window_) SDL_DestroyWindow(window_);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            throw;
        }
    }
    ~SdlWindow() override {
        renderer_.reset(); context_.reset();
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    gt2view::VkSceneRenderer& Renderer() override {
        if (!renderer_) throw std::runtime_error("This window has no renderer");
        return *renderer_;
    }
    void* NativeHandle() const override { return window_; }
    void SetTitle(const std::string& title) override { SDL_SetWindowTitle(window_, title.c_str()); }
    void Pump() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) closed_ = true;
            if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED) devicesChanged_ = true;
            if (event.type == SDL_WINDOWEVENT && event.window.windowID == SDL_GetWindowID(window_)) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) closed_ = true;
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESTORED ||
                    event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    timingReset_ = true;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.windowID == SDL_GetWindowID(window_)) {
                if (event.key.keysym.scancode == SDL_SCANCODE_RETURN && (event.key.keysym.mod & (KMOD_ALT | KMOD_GUI))) {
                    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    if (SDL_SetWindowFullscreen(window_, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                        throw std::runtime_error(SDL_GetError());
                    timingReset_ = true;
                } else if (event.key.keysym.scancode == SDL_SCANCODE_Q && (event.key.keysym.mod & KMOD_SHIFT)) {
                    std::printf("input: settings shortcut Shift+Q\n");
                    keyDowns_.push_back(gt2::keys::kF10); // Physical key: works with either keyboard layout.
                } else if (const int key = Key(event.key.keysym.scancode)) {
                    if (key == gt2::keys::kF10) std::printf("input: settings shortcut F10\n");
                    keyDowns_.push_back(key);
                }
            }
        }
        // Sleep while minimized without freezing the event loop.
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) SDL_Delay(20);
    }
    bool TakeTimingReset() override { return std::exchange(timingReset_, false); }
    bool Closed() const override { return closed_; }
    void Close() override { closed_ = true; }
    bool Focused() const override { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0; }
    std::vector<int> TakeKeyDowns() override { return std::exchange(keyDowns_, {}); }
    bool KeyDown(int key) const override {
        int count = 0;
        const Uint8* state = SDL_GetKeyboardState(&count);
        for (int i = 0; i < count; ++i) if (state[i] && Key(SDL_Scancode(i)) == key) return true;
        return false;
    }
    bool TakeDevicesChanged() override { return std::exchange(devicesChanged_, false); }
    void SleepUntil(Clock::time_point t) override {
        if (t - Clock::now() > std::chrono::milliseconds(1)) std::this_thread::sleep_until(t - std::chrono::microseconds(200));
        while (Clock::now() < t) std::this_thread::yield();
    }
    // SDL has no compositor timestamp. Common pacing uses its steady-clock fallback;
    // Vulkan FIFO still synchronizes presentation when vsync is selected.
    bool VBlankTiming(Clock::time_point&, Clock::duration&) const override { return false; }
private:
    SDL_Window* window_ = nullptr;
    std::unique_ptr<gt2view::VkContext> context_;
    std::unique_ptr<gt2view::VkSceneRenderer> renderer_;
    std::vector<int> keyDowns_;
    bool closed_ = false, devicesChanged_ = false, timingReset_ = false;
};
}
std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string& title, int w, int h) {
    return std::make_unique<SdlWindow>(title, w, h, true);
}
std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string& title, int w, int h) {
    return std::make_unique<SdlWindow>(title, w, h, false);
}
std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string&, int, int) {
    throw std::runtime_error("This macOS build supports flat play only. Remove --vr.");
}
} // namespace gt2game
