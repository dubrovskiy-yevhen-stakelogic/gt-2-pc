#pragma once
#include <algorithm>

namespace gt2game {
// Requires release on the dangerous row, then a fresh uninterrupted hold.
// Long frame gaps (focus loss, suspend, disconnect) never count towards consent.
class CampaignConfirmation {
public:
    bool Update(bool selected, bool held, bool focused, double seconds) {
        if (!selected || !focused || seconds < 0 || seconds > .25) {
            armed_ = false; elapsed_ = 0; return false;
        }
        if (!held) { armed_ = true; elapsed_ = 0; return false; }
        if (!armed_) return false;
        elapsed_ += seconds;
        if (elapsed_ < 3.) return false;
        armed_ = false; elapsed_ = 0; return true;
    }
    int Percent() const { return std::clamp(int(elapsed_ * 100. / 3.), 0, 100); }
private:
    bool armed_ = false;
    double elapsed_ = 0;
};
}
