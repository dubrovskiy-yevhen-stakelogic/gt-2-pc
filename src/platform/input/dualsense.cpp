#include "dualsense.h"
#include <windows.h>
#include <hidsdi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace gt2::input {
struct DualSenseEffects::Impl {
    HANDLE device = INVALID_HANDLE_VALUE;
    std::atomic<bool> ok{false};
    bool bt = false, dirty = false;
    std::atomic<bool> stop{false}, inputFailed{false};
    DWORD inputLength = 0;
    bool haveInput = false;
    Ps1PadFrame input;
    std::chrono::steady_clock::time_point inputTime;
    DWORD outputLength = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker, reader;
    DualSenseState state;
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
    void Read() {
        OVERLAPPED io{};
        io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io.hEvent) { inputFailed = true; return; }
        std::vector<uint8_t> bytes(inputLength);
        while (!stop) {
            ResetEvent(io.hEvent);
            DWORD count = 0;
            BOOL received = ReadFile(device, bytes.data(), inputLength, &count, &io);
            if (!received && GetLastError() == ERROR_IO_PENDING) {
                DWORD wait;
                do { wait = WaitForSingleObject(io.hEvent, 50); } while (wait == WAIT_TIMEOUT && !stop);
                if (wait != WAIT_OBJECT_0) CancelIoEx(device, &io);
                received = GetOverlappedResult(device, &io, &count, TRUE);
            }
            if (stop) break;
            if (!received) { inputFailed = true; break; }
            Ps1PadFrame pad;
            if (DecodeDualSenseInput(std::span<const uint8_t>(bytes.data(), count), bt, pad)) {
                std::lock_guard lock(mutex);
                if (!haveInput) std::printf("DualSense: native %s input active (report 0x%02X)\n", bt ? "Bluetooth" : "USB", unsigned(bytes[0]));
                input = pad;
                haveInput = true;
                inputTime = std::chrono::steady_clock::now();
            }
        }
        CloseHandle(io.hEvent);
    }
    void Run() {
        uint8_t sequence = 0;
        OVERLAPPED io{};
        io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io.hEvent) { ok = false; return; }
        DualSenseState previous;
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop || dirty; });
            const bool ending = stop;
            DualSenseState next = state;
            if (ending || std::chrono::steady_clock::now() - touched > std::chrono::milliseconds(500)) next = {};
            dirty = false;
            if (next == previous && !ending) continue;
            previous = next;
            lock.unlock();
            auto report = DualSenseReport(next, bt, sequence++);
            std::vector<uint8_t> packet(outputLength, 0);
            std::copy_n(report.begin(), bt ? 78 : 48, packet.begin());
            ResetEvent(io.hEvent);
            DWORD written = 0;
            // Win32 HID writes require padding to the maximum output report length.
            BOOL sent = WriteFile(device, packet.data(), outputLength, &written, &io);
            if (!sent && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(io.hEvent, 100) != WAIT_OBJECT_0) CancelIoEx(device, &io);
                sent = GetOverlappedResult(device, &io, &written, TRUE);
            }
            if (!sent || written != outputLength) {
                ok = false;
                std::fprintf(stderr, "DualSense: HID output failed (%lu); reconnect controller to retry\n", GetLastError());
            }
            lock.lock();
            if (ending || !ok) break;
            // Bound USB/BT traffic even when the race updates faster than the device.
            wake.wait_for(lock, std::chrono::milliseconds(8), [&] { return stop.load(); });
        }
        CloseHandle(io.hEvent);
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        if (reader.joinable()) reader.join();
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    }
};

DualSenseEffects::DualSenseEffects(const wchar_t* path) : impl_(std::make_unique<Impl>()) {
    if (!path || !*path) return;
    auto& p = *impl_;
    p.device = CreateFileW(path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (p.device == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES a{}; a.Size = sizeof(a);
    if (!HidD_GetAttributes(p.device, &a) || a.VendorID != 0x054C || (a.ProductID != 0x0CE6 && a.ProductID != 0x0DF2)) return;
    PHIDP_PREPARSED_DATA prep = nullptr;
    if (!HidD_GetPreparsedData(p.device, &prep)) return;
    HIDP_CAPS caps{};
    const auto status = HidP_GetCaps(prep, &caps);
    HidD_FreePreparsedData(prep);
    if (status != HIDP_STATUS_SUCCESS || caps.UsagePage != 1 || caps.Usage != 5) return;
    const auto transport = DualSenseReportTransport(caps.InputReportByteLength, caps.OutputReportByteLength);
    if (transport == DualSenseTransport::Unsupported) return;
    p.bt = transport == DualSenseTransport::Bluetooth;
    p.outputLength = caps.OutputReportByteLength;
    p.inputLength = caps.InputReportByteLength;
    p.ok = true;
    p.reader = std::thread([&p] { p.Read(); });
    p.worker = std::thread([&p] { p.Run(); });
    std::printf("DualSense: native %s rumble and adaptive pedals available\n", p.bt ? "Bluetooth" : "USB");
}
DualSenseEffects::~DualSenseEffects() = default;
bool DualSenseEffects::Available() const { return impl_->ok; }
int DualSenseEffects::ReadPad(Ps1PadFrame& pad) const {
    auto& p = *impl_;
    if (p.inputFailed) return -1;
    std::lock_guard lock(p.mutex);
    if (!p.haveInput) return 0;
    pad = std::chrono::steady_clock::now() - p.inputTime < std::chrono::milliseconds(250) ? p.input : Ps1PadFrame{};
    return 1;
}
void DualSenseEffects::Motors(uint8_t small, uint8_t large, int strength) {
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex); p.state.small = small; p.state.large = large; p.state.rumblePercent = uint8_t(std::clamp(strength, 0, 100)); p.dirty = true; p.touched = std::chrono::steady_clock::now(); }
    p.wake.notify_one();
}
void DualSenseEffects::Triggers(uint8_t accelerator, uint8_t brake) {
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex); p.state.accelerator = accelerator; p.state.brake = brake; p.dirty = true; p.touched = std::chrono::steady_clock::now(); }
    p.wake.notify_one();
}
}
