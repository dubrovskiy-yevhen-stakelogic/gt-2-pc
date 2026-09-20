#pragma once
// DEV CAPTURE AID (tools only): drives the ORIGINAL through the race overlay's screens between races, so that a whole
// event / championship (with ai_player.h driving the car) runs unattended in gt2run session `auto=<from>[:<to>]`.
// Every press it makes is logged and collected as a gt2play --script (the interpreter is deterministic, so the same run
// can be repeated for GP0 captures at chosen fields). It only reads guest RAM and presses pad buttons.
//
// Evidence for the RAM it reads (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a, GT2.OVL member 0):
//   0x800130DC == jal 0x80033384      the race overlay is loaded (ai_player.h)
//   s32 0x80046F64                    the race clock (the HUD's time source): changes while a race / replay runs
//   s16 0x800AF226                    race-end timer (-1 racing, counts from the finish, held while 0x8002A700 waits)
//   u8  0x800A951C                    1 = replay / attract race (camera and course detail follow it)
//   0x800585C0 called                 the event menu ("SINGLE RACE" / "SESSION n" / post-race) is drawn
//   s16 0x8005D284 + 0 / + 6          its list widget's row count / selected row (starts at 0; up wraps)
//   0x8004A55C called                 the post-race menu (Replay / Save Replay ... / Continue; update 0x8004A0BC) is drawn
//   s16 0x8005ADC0 + 0 / + 6          its list widget's row count / selected row
//   0x80059800 called                 the championship end (champion's BONUS view with the 'gtprz' trophy, set up
//                                     once by 0x80017A28): no presses for kChampionHold fields, so that whole view
//                                     can be captured before X leaves it
#include <cstdint>
#include <cstdio>
#include <string>

#include "machine/machine.h"

namespace gt2 {

class RaceAutopilot {
public:
    // Hook for Machine::cpu.onCall.
    void OnCall(uint32_t to) {
        if (to == 0x800585C0u) menuFrames_++;
        if (to == 0x8004A55Cu) postMenuFrames_++;
        if (to == 0x80059800u) championHold_ = kChampionHold;
    }

    // Called once per field after the field ran; returns the pad for the next field. `log` gets the presses.
    uint16_t Next(const Machine& m, uint64_t field, std::FILE* log) {
        const uint32_t menu = menuFrames_, postMenu = postMenuFrames_;
        menuFrames_ = postMenuFrames_ = 0;
        auto u8 = [&](uint32_t a) { return uint8_t(m.bus.Read(a, 1)); };
        auto s16 = [&](uint32_t a) { return int16_t(uint16_t(m.bus.Read(a, 2))); };
        const uint32_t clock = m.bus.Read(0x80046F64u, 4);
        const bool running = clock != lastClock_;
        lastClock_ = clock;
        sinceRunning_ = running ? 0 : sinceRunning_ + 1;
        const int16_t timer = s16(0x800AF226u);
        stable_ = (timer >= 0 && timer == lastTimer_) ? stable_ + 1 : 0;
        lastTimer_ = timer;
        if (hold_ > 0) { hold_--; return held_; }
        if (championHold_ > 0) { championHold_--; idle_ = 0; return 0; }
        if (cooldown_ > 0) { cooldown_--; return 0; }
        uint32_t jal = 0;
        if (!m.bus.FastRead32(0x800130DCu, jal) || jal != 0x0C00CCE1u) return 0; // not in the race overlay
        const char* why = nullptr;
        uint16_t press = 0;
        int wait = 24;
        if (replayStep_ > 0) { // the replay exit sequence: Start (pause), down (Exit), cross
            press = replayStep_ == 1 ? kDown : kCross, why = replayStep_ == 1 ? "replay pause: to Exit" : "replay pause: Exit";
            replayStep_ = replayStep_ == 1 ? 2 : 0;
            if (replayStep_ == 0) wait = 150;
        } else if (postMenu > 0) { // Continue = the last row
            // Rows (0x8004A0BC): a championship race with more to come has Replay / Next Session / Save Replay ... /
            // Exit (0x8005AD78): row 1; otherwise Replay / Save Replay ... / Continue (0x8005AD98): the last row.
            const int16_t rows = s16(0x8005ADC0u), row = s16(0x8005ADC0u + 6);
            const int16_t target = rows == 4 ? int16_t(1) : int16_t(rows - 1);
            if (row < target) press = kDown, why = "post-race menu: down to Continue / Next Session";
            else if (row > target) press = kUp, why = "post-race menu: up to Next Session";
            else press = kCross, why = "post-race menu: Continue", wait = 90;
            raced_ = false;
        } else if (menu > 0) {
            const int16_t rows = s16(0x8005D284u), row = s16(0x8005D284u + 6);
            // Pre-race: "Start Race" (row 4 of the 6-row menu, then AT on the transmission bar); post-race (Replay /
            // Save Replay ... / Continue): the last row.
            if (rows == 6) raced_ = false;
            const int16_t target = raced_ ? int16_t(rows - 1) : int16_t(4);
            if (row != target) press = kUp, why = raced_ ? "post-race menu: up to the last row" : "pre-race menu: up to Start Race";
            else {
                press = kCross, why = raced_ ? "post-race menu: Continue" : "pre-race menu: Start Race / AT";
                raced_ = false;
            }
        } else if (running && u8(0x800A951Cu) != 0) {
            raced_ = true;
            press = kStart, why = "replay: pause", replayStep_ = 1;
        } else if (running && timer < 0) {
            raced_ = true;
            idle_ = 0; // racing: no input (the AI drives)
        } else if (timer >= 0 && stable_ >= 20 && sinceRunning_ < 600) { // the race loop waits for X (0x8002A700)
            raced_ = true;
            press = kCross, why = "race end: the results wait";
        } else if (++idle_ >= 90) {
            idle_ = 0;
            press = kCross, why = "other view: X";
        }
        if (!press) return 0;
        held_ = press;
        hold_ = 5;       // held 6 fields
        cooldown_ = wait;
        const char* name = press == kCross ? "cross" : press == kDown ? "down" : press == kUp ? "up" : "start";
        std::fprintf(log, "f%llu auto %s (%s)\n", (unsigned long long)field + 1, name, why);
        std::fflush(log);
        script_ += (script_.empty() ? "" : ",") + std::to_string(field + 1) + ":" + name;
        return press;
    }
    const std::string& Script() const { return script_; }

private:
    static constexpr int kChampionHold = 900;
    static constexpr uint16_t kStart = 1u << 3, kUp = 1u << 4, kDown = 1u << 6, kCross = 1u << 14;
    uint32_t menuFrames_ = 0, postMenuFrames_ = 0, lastClock_ = 0;
    uint16_t held_ = 0;
    int championHold_ = 0;
    int hold_ = 0, cooldown_ = 0, stable_ = 0, idle_ = 0, replayStep_ = 0, sinceRunning_ = 1000;
    int16_t lastTimer_ = -1;
    bool raced_ = false;
    std::string script_;
};

} // namespace gt2
