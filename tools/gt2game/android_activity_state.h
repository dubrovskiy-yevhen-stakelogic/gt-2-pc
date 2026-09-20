#pragma once
#include <array>
#include <vector>

namespace gt2game::android {
// Android can create another NativeActivity without unloading the native library.
// Access is protected by the host mutex; reset before starting its event/game threads.
struct ActivityState {
    bool resumed = false;
    bool quit = false;
    std::vector<int> keyDowns;
    std::array<bool, 256> down{};
    void Reset() { resumed = false; quit = false; keyDowns.clear(); down.fill(false); }
};
}
