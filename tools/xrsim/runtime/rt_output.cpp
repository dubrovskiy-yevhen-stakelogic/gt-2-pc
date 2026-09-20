// xrsim - outputs: per-frame CSV, background PNG writer (captures), desktop mirror window.
#include "rt.h"

#include "gt2export/png_deflate.h"

#include <filesystem>
#include <stdexcept>

namespace xs {

// ================================================================================================ Output
Output::~Output() { Close(); }

bool Output::Open(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (!std::filesystem::is_directory(std::filesystem::path(dir), ec)) return false;
    dir_ = dir;
    csv_ = fopen((dir + "\\xrsim_frames.csv").c_str(), "w");
    stop_ = false;
    worker_ = std::thread([this] { Run(); });
    return csv_ != nullptr;
}

void Output::Csv(const std::string& row) {
    if (!csv_) return;
    fputs(row.c_str(), csv_);
    fputc('\n', csv_);
    fflush(csv_);
}

void Output::QueuePng(std::string path, int w, int h, std::vector<uint8_t> raw, const FmtInfo* fmt, bool opaque) {
    if (!worker_.joinable()) return;
    std::lock_guard<std::mutex> lk(m_);
    jobs_.push_back({std::move(path), w, h, std::move(raw), fmt, opaque});
    cv_.notify_all();
}

void Output::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return; // stop_ and nothing left
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        try {
            const std::vector<uint8_t> rgba = ToDisplayRgba(job.fmt, job.raw.data(), (uint32_t)job.w, (uint32_t)job.h, job.opaque);
            gt2::WritePngRgbaCompressed(job.path, job.w, job.h, rgba);
            Log("wrote %s (%dx%d)", job.path.c_str(), job.w, job.h);
        } catch (const std::exception& e) {
            Log("PNG write failed: %s: %s", job.path.c_str(), e.what());
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            busy_ = false;
            ++written_;
        }
        idle_.notify_all();
    }
}

void Output::Flush() {
    if (!worker_.joinable()) return;
    std::unique_lock<std::mutex> lk(m_);
    idle_.wait(lk, [&] { return jobs_.empty() && !busy_; });
}

void Output::Close() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    if (csv_) fclose(csv_);
    csv_ = nullptr;
    dir_.clear();
}

// ================================================================================================ Mirror
Mirror::~Mirror() { Stop(); }

void Mirror::Start(int ew, int eh) {
    if (thread_.joinable()) return;
    eyeW = ew;
    eyeH = eh;
    w_ = ew * 2;
    h_ = eh;
    bgra_.assign((size_t)w_ * h_ * 4, 0);
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void Mirror::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    if (hwnd_) PostMessageW(hwnd_, WM_APP + 1, 0, 0);
    thread_.join();
}

void Mirror::Present(const uint8_t* rgba, int w, int h) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (w != w_ || h != h_) return;
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i) {
            bgra_[i * 4 + 0] = rgba[i * 4 + 2];
            bgra_[i * 4 + 1] = rgba[i * 4 + 1];
            bgra_[i * 4 + 2] = rgba[i * 4 + 0];
            bgra_[i * 4 + 3] = 255;
        }
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK Mirror::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Mirror*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            std::lock_guard<std::mutex> lk(self->m_);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = self->w_;
            bi.bmiHeader.biHeight = -self->h_; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, self->w_, self->h_, self->bgra_.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
        if (self) self->closeRequested_ = true;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void Mirror::Run() {
    HMODULE hi = nullptr; // this DLL (the class is unregistered below, so a reloaded runtime never sees a stale WndProc)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Mirror::WndProc), &hi);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &Mirror::WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.lpszClassName = L"xrsim_mirror";
    RegisterClassW(&wc); // fails harmlessly when already registered
    RECT r{0, 0, w_, h_};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"xrsim_mirror", L"xrsim mirror (left eye | right eye) - close to request session exit",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi,
                                nullptr);
    if (!hwnd) {
        Log("mirror: CreateWindow failed (%lu)", GetLastError());
        return;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hwnd_ = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Log("mirror window %dx%d opened", w_, h_);
    while (!stop_) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    hwnd_ = nullptr;
    DestroyWindow(hwnd);
    UnregisterClassW(L"xrsim_mirror", hi);
}

} // namespace xs
