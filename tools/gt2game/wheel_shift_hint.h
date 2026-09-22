#pragma once

namespace gt2game {
// Tick with physics (30 Hz), never with rendering: brief paddle presses must
// remain readable at every display refresh rate.
class WheelShiftHint {
public:
    void Reset() { ticks_ = 0; }
    void Tick(bool blocked, int gear, bool driving) {
        if (!driving || (ticks_ && gear != gear_)) ticks_ = 0;
        if (driving && blocked) { ticks_ = 120; gear_ = gear; }
        else if (ticks_) --ticks_;
    }
    bool Visible() const { return ticks_ > 0; }
private:
    int ticks_ = 0, gear_ = 0;
};
}
