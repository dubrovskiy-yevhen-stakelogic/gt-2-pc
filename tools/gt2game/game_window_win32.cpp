// The Windows backend of gt2game's window (game_window.h WindowBackend): the window class and its message pump, the
// Vulkan context and renderer on it, the keyboard, the high-resolution sleep and the compositor's frame timing.
#include "game_window.h"

#include <windows.h>
#include <mmsystem.h>
#include <dwmapi.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#include "gt2view/vk_context.h"

namespace gt2game {
namespace {

using Clock = std::chrono::steady_clock;

class Win32Window final : public WindowBackend {
public:
    // `withRenderer` false: the window, its keyboard and its timing only - the XR backend (game_window_xr.cpp) keeps
    // such a window as the desktop window and renders through the OpenXR session's own Vulkan device.
    Win32Window(const std::string& title, int clientWidth, int clientHeight, bool withRenderer);
    ~Win32Window() override;

    gt2view::VkSceneRenderer& Renderer() override {
        if (!renderer_) throw std::runtime_error("this window has no renderer of its own");
        return *renderer_;
    }
    void* NativeHandle() const override { return hwnd_; }
    void SetTitle(const std::string& title) override { SetWindowTextA(hwnd_, title.c_str()); }

    void Pump() override;
    bool Closed() const override { return closed_; }
    void Close() override { closed_ = true; }
    bool Focused() const override { return GetForegroundWindow() == hwnd_; }
    std::vector<int> TakeKeyDowns() override {
        std::vector<int> out;
        out.swap(keyDowns_);
        return out;
    }
    bool KeyDown(int key) const override { return (GetAsyncKeyState(key) & 0x8000) != 0; }
    bool TakeDevicesChanged() override {
        const bool changed = devicesChanged_;
        devicesChanged_ = false;
        return changed;
    }

    void SleepUntil(Clock::time_point t) override;
    bool VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    std::unique_ptr<gt2view::VkContext> vulkan_;
    std::unique_ptr<gt2view::VkSceneRenderer> renderer_;
    std::vector<int> keyDowns_; // key-down messages since the last TakeKeyDowns
    bool closed_ = false;
    bool devicesChanged_ = false;
    HANDLE timer_ = nullptr; // SleepUntil's high-resolution waitable timer
};

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Window* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (msg == WM_DESTROY) {
        if (self) self->closed_ = true;
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (self) self->closed_ = true;
        return 0; // the owner destroys the window
    }
    if (msg == WM_DEVICECHANGE && self) self->devicesChanged_ = true; // a controller plugged in / out
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && self) {
        self->keyDowns_.push_back(int(wp));
        if (msg == WM_SYSKEYDOWN && wp == VK_F10) return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Per-monitor DPI awareness, once, before the first window: without it Windows shrinks a window that does not fit
// the SCALED desktop (a 1280 x 960 client is impossible on a 150 % 1080p or 250 % 4K screen) and stretches what is
// presented. The rendered frame is unaffected either way - the swapchain is the client area's size, which is what
// --window asks for - so the frames of the checks stay what they were.
void EnableDpiAwareness() {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        if (auto set = reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))) {
            if (set(reinterpret_cast<HANDLE>(-4))) return; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }
    SetProcessDPIAware();
}

Win32Window::Win32Window(const std::string& title, int clientWidth, int clientHeight, bool withRenderer) {
    static const bool dpi = (EnableDpiAwareness(), true);
    (void)dpi;
    HINSTANCE hinst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "gt2game_window";
    RegisterClassA(&wc); // (fails harmlessly when already registered)
    RECT rc{0, 0, clientWidth, clientHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    // Created hidden and then shown: an automated run (WindowNoFocus / GT2_NO_FOCUS) must not take the keyboard away
    // from whatever the user is doing, so its window appears without being activated.
    hwnd_ = CreateWindowA("gt2game_window", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                          nullptr, nullptr, hinst, nullptr);
    if (!hwnd_) throw std::runtime_error("CreateWindow failed");
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    const bool noFocus = WindowNoFocus() || std::getenv("GT2_NO_FOCUS") != nullptr;
    if (noFocus) {
        // Not ShowWindow: the FIRST ShowWindow of a process takes its command from the launcher's STARTUPINFO, so
        // SW_SHOWNOACTIVATE can be turned back into "show and activate" by whoever started the run. SetWindowPos
        // shows the window without ever activating it, whatever the launcher asked for.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, SW_SHOW);
    }
    if (withRenderer) {
        vulkan_ = std::make_unique<gt2view::VkContext>(hinst, hwnd_);
        renderer_ = std::make_unique<gt2view::VkSceneRenderer>(*vulkan_);
    }
    timeBeginPeriod(1);
}

Win32Window::~Win32Window() {
    if (timer_) CloseHandle(timer_);
    timeEndPeriod(1);
    renderer_.reset();
    vulkan_.reset();
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowLongPtrA(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void Win32Window::Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void Win32Window::SleepUntil(Clock::time_point t) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto spin = std::chrono::microseconds(200);
    auto now = Clock::now();
    if (timer_ && t - now > spin) {
        LARGE_INTEGER due;
        due.QuadPart = -std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(t - now - spin).count(); // relative, 100 ns
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, INFINITE);
    } else if (!timer_ && t - now > std::chrono::milliseconds(2)) {
        std::this_thread::sleep_until(t - std::chrono::milliseconds(1));
    }
    while (Clock::now() < t) std::this_thread::yield();
}

bool Win32Window::VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const {
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || info.qpcRefreshPeriod == 0 || info.qpcVBlank == 0) return false;
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const auto now = Clock::now();
    auto ticks = [&](long long qpc) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(qpc) / double(frequency.QuadPart)));
    };
    period = ticks(static_cast<long long>(info.qpcRefreshPeriod));
    vblank = now - ticks(counter.QuadPart - static_cast<long long>(info.qpcVBlank));
    return period > std::chrono::milliseconds(2) && period < std::chrono::milliseconds(50);
}

} // namespace

std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string& title, int clientWidth, int clientHeight) {
    return std::make_unique<Win32Window>(title, clientWidth, clientHeight, true);
}

std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string& title, int clientWidth, int clientHeight) {
    return std::make_unique<Win32Window>(title, clientWidth, clientHeight, false);
}

} // namespace gt2game
