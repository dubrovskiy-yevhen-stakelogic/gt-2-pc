#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include "game/sim/race_sim.h"

namespace gt2::net {
constexpr size_t kSlots = sim::kMaxCars;
using PlayerId = uint32_t; // zero is a server-owned AI slot
using Tick = uint32_t;
using Inputs = std::array<sim::PadRecord, kSlots>;

struct Participant { PlayerId player = 0; };
using Roster = std::array<Participant, kSlots>;
// Uses the normal race's assets and physics while fixing presentation-dependent
// setup inputs. Human control must be chosen before tyre/gearbox setup, not
// switched from an AI car afterwards.
void SetupNetworkRace(sim::RaceSim& race, const Track& track, const sim::CourseExtras& extras,
    const sim::SimConstants& constants, const sim::RaceCourseData& course,
    std::vector<sim::CarParams> params, std::vector<sim::RaceSlot> slots,
    const sim::RaceShellOptions& options, const Roster& roster, uint8_t botDifficulty = 1);
struct Result {
    int32_t bestLap = -1;
    int32_t lastLap = -1;
    int32_t finishTime = -1;
    uint16_t laps = 0;
    uint8_t position = 0;
    bool dnf = false;
    bool lastLapValid = false;
    bool operator==(const Result&) const = default;
};
enum class EventKind : uint8_t { Sound, Lap, Split, Finish, RaceEnd };
struct Event {
    Tick tick = 0;
    uint16_t sequence = 0;
    EventKind kind = EventKind::Sound;
    uint8_t car = 0;
    int32_t a = 0, b = 0, c = 0;
    bool operator==(const Event&) const = default;
};

// No window, audio device, OS clock, socket or save writes. The caller advances
// exactly one simulation tick and decides when its events become authoritative.
class RaceSession {
    friend class StateCodec;
public:
    struct State {
        sim::RaceSim::Checkpoint race;
        Tick nextTick = 0, firstFinish = 0;
        std::array<Result, kSlots> results{};
        bool finishing = false, complete = false;
    };
    RaceSession(sim::RaceSim& race, const Roster& roster, Tick finishTimeout = 30 * 30);
    ~RaceSession();
    RaceSession(const RaceSession&) = delete;
    RaceSession& operator=(const RaceSession&) = delete;
    std::vector<Event> Step(const Inputs& inputs);
    State Capture() const;
    void Restore(const State& state);
    static unsigned FinishSeconds(Tick now, Tick first, bool finishing, bool complete, Tick timeout) {
        if(!finishing || complete || now-first>=timeout)return 0;
        return (timeout-(now-first)+29)/30;
    }
    unsigned FinishSeconds() const { return FinishSeconds(tick_,firstFinish_,finishing_,complete_,finishTimeout_); }
    Tick FinishTimeout() const { return finishTimeout_; }
    Tick NextTick() const { return tick_; }
    bool Complete() const { return complete_; }
    const std::array<Result, kSlots>& Results() const { return results_; }
    const Roster& Participants() const { return roster_; }
    size_t CarCount() const { return race_.CarCount(); }
private:
    void Add(EventKind kind, int car, int32_t a = 0, int32_t b = 0, int32_t c = 0);
    sim::RaceSim& race_;
    Roster roster_{};
    sim::ShellHooks previousHooks_{};
    Tick tick_ = 0, firstFinish_ = 0, finishTimeout_ = 0;
    bool finishing_ = false, complete_ = false;
    std::array<Result, kSlots> results_{};
    std::vector<Event> events_;
};

bool SameInput(const sim::PadRecord& a, const sim::PadRecord& b);
bool SameInputs(const Inputs& a, const Inputs& b);
bool ValidInput(const sim::PadRecord& pad, bool allowShiftSpeedOverride = false);
sim::PadRecord SafeIdleInput();

// One bounded input inbox for an authoritative server. Ownership is supplied by
// the authenticated connection; an untrusted packet cannot select another car.
class InputInbox {
public:
    explicit InputInbox(const Roster& roster, size_t cars);
    bool Submit(PlayerId sender, Tick tick, const sim::PadRecord& input);
    void Release(PlayerId sender); // disconnect: discard queued input and brake immediately
    Inputs Consume();
    Tick NextTick() const { return tick_; }
    static constexpr Tick kFutureWindow = 120;
    static constexpr Tick kHoldTicks = 6;
private:
    struct Pending { Tick tick = 0; sim::PadRecord input{}; bool occupied = false; };
    Roster roster_{};
    size_t cars_ = 0;
    Tick tick_ = 0;
    std::array<std::array<Pending, kFutureWindow>, kSlots> pending_{};
    Inputs last_{};
    std::array<Tick, kSlots> age_{};
};

// Local checkpoints only. Server input frames arrive in contiguous tick order;
// transport reordering/recovery belongs outside this object. Effects are returned
// only once their tick is confirmed, never while speculative ticks are replayed.
class PredictionTimeline {
public:
    explicit PredictionTimeline(RaceSession& session, size_t capacity = 120);
    void Predict(const Inputs& inputs);
    std::vector<Event> Confirm(Tick tick, const Inputs& authoritativeInputs);
    void StageAuthority(Tick tick, const Inputs& inputs); // coalesce a received batch before confirming
    void DiscardPrediction(); // restore the confirmed boundary, e.g. on the server's terminal tick
    void AcceptRestoredState(); // caller has atomically restored a validated authority snapshot
    unsigned ConfirmedFinishSeconds() const;
    RaceSession::State ConfirmedState() const; // excludes speculative ticks; never mutates the live race
    size_t PendingTicks() const { return pending_.size(); }
    Tick ConfirmedNextTick() const { return confirmedNext_; }
    uint64_t ReplayedTicks() const { return replayed_; }
private:
    struct Frame { RaceSession::State before; Inputs inputs{}; std::vector<Event> events; bool dirty=false; };
    RaceSession& session_;
    size_t capacity_;
    Tick confirmedNext_;
    uint64_t replayed_ = 0;
    std::deque<Frame> pending_;
};
} // namespace gt2::net
