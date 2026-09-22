#pragma once
#include <chrono>
#include <string>

namespace gt2::input {
// Sparse diagnostics only: transitions and calls exceeding the frame budget.
void InputDiagnostic(const std::string& message);
class InputCallTimer {
public:
    explicit InputCallTimer(const char* operation, int thresholdMs = 100) : operation_(operation), thresholdMs_(thresholdMs) {}
    ~InputCallTimer() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
        if (ms >= thresholdMs_) InputDiagnostic(std::string(operation_) + " took " + std::to_string(ms) + " ms");
    }
private:
    const char* operation_;
    int thresholdMs_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
}
