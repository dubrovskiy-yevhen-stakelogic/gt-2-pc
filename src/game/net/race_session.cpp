#include "game/net/race_session.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gt2::net {
namespace {
void CheckRoster(const Roster& roster, size_t cars) {
    if (cars == 0 || cars > kSlots) throw std::invalid_argument("network roster: 1 to 6 cars required");
    for (size_t i = 0; i < kSlots; ++i) {
        if (i >= cars && roster[i].player) throw std::invalid_argument("network roster: player has no car");
        if (!roster[i].player) continue;
        for (size_t j = 0; j < i; ++j)
            if (roster[i].player == roster[j].player) throw std::invalid_argument("network roster: duplicate player");
    }
}
}
void SetupNetworkRace(sim::RaceSim& race, const Track& track, const sim::CourseExtras& extras,
    const sim::SimConstants& constants, const sim::RaceCourseData& course,
    std::vector<sim::CarParams> params, std::vector<sim::RaceSlot> slots,
    const sim::RaceShellOptions& options, const Roster& roster, uint8_t botDifficulty) {
    if(botDifficulty>2)throw std::invalid_argument("invalid bot difficulty");
    CheckRoster(roster, slots.size());
    if (constants.gameMode != 0 && constants.gameMode != 2 && constants.gameMode != 4 && constants.gameMode != 0xC)
        throw std::invalid_argument("network race: normal circuit mode required");
    if (options.ghost || options.courseRecordTarget || constants.step.rate != 30)
        throw std::invalid_argument("network race: circuit at 30 Hz required, without external save/ghost state");
    auto canonical = constants;
    canonical.gameMode = 2;
    // Match Arcade: inherited GT-mode wall damage must not bend player steering.
    canonical.shellControlClass = 0;
    canonical.viewMode = 0;
    canonical.flag800A951C = 0;
    canonical.word801C98A0 &= 0xff00ffffu;
    canonical.catchUp = {};
    const int targetPercent=botDifficulty==0?80:botDifficulty==2?105:100;
    for(auto& tuning:canonical.classTuning) {
        tuning.cornerSpeedGain=int16_t(int32_t(tuning.cornerSpeedGain)*targetPercent/100);
        tuning.cornerSpeedBase=int32_t(int64_t(tuning.cornerSpeedBase)*targetPercent/100);
        if(botDifficulty==0)tuning.throttleScale=int16_t(int32_t(tuning.throttleScale)*80/100);
    }
    auto shell = options;
    // Point-to-point finish emits lap 1; a circuit lap target would hide its finish event.
    if(shell.pointToPoint)shell.lapCount=1;
    shell.introFields = 0;
    shell.hasCourseRecord = false;
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].entryKind = sim::kEntryAi;
        slots[i].controlClass = roster[i].player ? 0 : 2;
        slots[i].contactType = 0;
        slots[i].gridOffset = 0;
        slots[i].dirtLevel = 0;
    }
    race.Setup(track, extras, canonical, course, std::move(params), slots, shell);
}
bool SameInput(const sim::PadRecord& a, const sim::PadRecord& b) {
    return a.flags == b.flags && a.steer == b.steer && a.brake == b.brake && a.throttle == b.throttle &&
        a.shift == b.shift && a.reverse == b.reverse && a.handbrake == b.handbrake && a.reserved == b.reserved;
}
bool SameInputs(const Inputs& a, const Inputs& b) {
    for (size_t i = 0; i < kSlots; ++i) if (!SameInput(a[i], b[i])) return false;
    return true;
}
bool ValidInput(const sim::PadRecord& p, bool allowShiftSpeedOverride) {
    if (p.shift < -1 || p.shift > 1 || p.reverse > 1 || p.handbrake > 1) return false;
    const bool wheel = (p.flags & sim::kWheelPad) != 0;
    if (wheel) {
        if ((p.flags & ~uint16_t(0x3ff)) || (p.flags & 7) != 7 || (p.flags & 8)) return false;
        if (!allowShiftSpeedOverride && (p.flags & sim::kWheelIgnoreShiftSpeed)) return false;
        const uint8_t gear = p.reserved & 15;
        if (gear > 9 && gear != 15) return false;
    } else if ((p.flags & ~uint16_t(15)) || p.reserved) return false;
    if (p.steer < ((p.flags & 1) ? -4096 : -2) || p.steer > ((p.flags & 1) ? 4096 : 2)) return false;
    if (p.throttle > ((p.flags & 2) ? 4096 : 1) || p.brake > ((p.flags & 4) ? 4096 : 1)) return false;
    return true;
}
sim::PadRecord SafeIdleInput() {
    sim::PadRecord p{};
    p.flags = 7;
    p.brake = 4096;
    return p;
}

RaceSession::RaceSession(sim::RaceSim& race, const Roster& roster, Tick finishTimeout)
    : race_(race), roster_(roster), finishTimeout_(finishTimeout) {
    CheckRoster(roster, race.CarCount());
    if (!finishTimeout || finishTimeout > 30 * 300) throw std::invalid_argument("network race: invalid finish timeout");
    uint8_t humans = 0;
    for (size_t i = 0; i < race.CarCount(); ++i) if (roster[i].player) humans |= uint8_t(1u << i);
    race_.ConfigureNetworkDrivers(humans);
    previousHooks_ = race_.Shell().Hooks();
    sim::ShellHooks hooks{};
    hooks.user = this;
    hooks.sound = [](void* u, uint32_t routine, int32_t argument) {
        static_cast<RaceSession*>(u)->Add(EventKind::Sound, 0, int32_t(routine), argument);
    };
    hooks.lapLine = [](void* u, int car, int lap, int32_t time, int32_t elapsed) {
        auto& s = *static_cast<RaceSession*>(u);
        auto& r = s.results_.at(size_t(car));
        if (r.finishTime >= 0) return;
        r.lastLap = time;
        r.lastLapValid = s.race_.CarAt(size_t(car)).body.hudInvalid == 0;
        r.laps = uint16_t(std::max(0, lap));
        if (r.lastLapValid && time >= 0 && (r.bestLap < 0 || time < r.bestLap)) r.bestLap = time;
        s.Add(EventKind::Lap, car, lap, time, elapsed);
    };
    hooks.split = [](void* u, int car, int sector, int32_t time) {
        static_cast<RaceSession*>(u)->Add(EventKind::Split, car, sector, time);
    };
    hooks.finished = [](void* u, int car, int position, int32_t elapsed) {
        auto& s = *static_cast<RaceSession*>(u);
        auto& r = s.results_.at(size_t(car));
        if (r.finishTime >= 0) return;
        r.finishTime = elapsed;
        r.position = uint8_t(position);
        if (!s.finishing_ && (s.roster_[size_t(car)].player || std::none_of(s.roster_.begin(),s.roster_.end(),[](const auto& slot){return bool(slot.player);}))) { s.firstFinish_ = s.tick_; s.finishing_ = true; }
        s.Add(EventKind::Finish, car, position, elapsed);
    };
    race_.Shell().Hooks() = hooks;
}
RaceSession::~RaceSession() { race_.Shell().Hooks() = previousHooks_; }
void RaceSession::Add(EventKind kind, int car, int32_t a, int32_t b, int32_t c) {
    events_.push_back({tick_, uint16_t(events_.size()), kind, uint8_t(car), a, b, c});
}
std::vector<Event> RaceSession::Step(const Inputs& inputs) {
    if (tick_ == std::numeric_limits<Tick>::max()) throw std::overflow_error("network tick exhausted");
    for (size_t i = 0; i < kSlots; ++i)
        if (!ValidInput(inputs[i])) throw std::invalid_argument("network race: invalid input");
    events_.clear();
    if (!complete_) {
        race_.Step(inputs.data());
        bool allFinished = true;
        const bool humans=std::any_of(roster_.begin(),roster_.end(),[](const auto& slot){return bool(slot.player);});
        for (size_t i = 0; i < race_.CarCount(); ++i) {
            results_[i].position = race_.CarAt(i).body.racePosition;
            if(!humans || roster_[i].player)allFinished = allFinished && results_[i].finishTime >= 0;
        }
        if (finishing_ && (allFinished || tick_ - firstFinish_ >= finishTimeout_)) {
            for (size_t i = 0; i < race_.CarCount(); ++i) results_[i].dnf = results_[i].finishTime < 0;
            complete_ = true;
            Add(EventKind::RaceEnd, 0);
        }
    }
    ++tick_;
    return events_;
}
RaceSession::State RaceSession::Capture() const {
    return {race_.CaptureCheckpoint(), tick_, firstFinish_, results_, finishing_, complete_};
}
void RaceSession::Restore(const State& s) {
    race_.RestoreCheckpoint(s.race); // validates ownership before any mutation
    tick_ = s.nextTick;
    firstFinish_ = s.firstFinish;
    results_ = s.results;
    finishing_ = s.finishing;
    complete_ = s.complete;
    events_.clear();
}

InputInbox::InputInbox(const Roster& roster, size_t cars) : roster_(roster), cars_(cars) {
    CheckRoster(roster, cars);
    last_.fill(SafeIdleInput());
    age_.fill(kHoldTicks);
}
bool InputInbox::Submit(PlayerId sender, Tick tick, const sim::PadRecord& input) {
    if (!sender || tick < tick_ || tick - tick_ >= kFutureWindow || !ValidInput(input)) return false;
    for (size_t i = 0; i < cars_; ++i) if (roster_[i].player == sender) {
        auto& p = pending_[i][tick % kFutureWindow];
        if (p.occupied) return p.tick == tick && SameInput(p.input, input);
        p = {tick, input, true};
        return true;
    }
    return false;
}
void InputInbox::Release(PlayerId sender) {
    if (!sender) return;
    for (size_t i = 0; i < cars_; ++i) if (roster_[i].player == sender) {
        pending_[i] = {};
        last_[i] = SafeIdleInput();
        age_[i] = kHoldTicks;
    }
}
Inputs InputInbox::Consume() {
    if (tick_ == std::numeric_limits<Tick>::max()) throw std::overflow_error("network tick exhausted");
    Inputs result{};
    for (size_t i = 0; i < cars_; ++i) if (roster_[i].player) {
        auto& p = pending_[i][tick_ % kFutureWindow];
        if (p.occupied && p.tick == tick_) {
            last_[i] = p.input;
            age_[i] = 0;
            p.occupied = false;
        } else {
            age_[i] = std::min(age_[i] + 1, kHoldTicks);
            // A missing packet must never repeat a sequential gear edge.
            last_[i].shift = 0;
            if (age_[i] >= kHoldTicks) last_[i] = SafeIdleInput();
        }
        result[i] = last_[i];
    }
    ++tick_;
    return result;
}

PredictionTimeline::PredictionTimeline(RaceSession& session, size_t capacity)
    : session_(session), capacity_(capacity), confirmedNext_(session.NextTick()) {
    if (capacity == 0 || capacity > 300) throw std::invalid_argument("prediction: invalid history size");
}
void PredictionTimeline::Predict(const Inputs& inputs) {
    if (pending_.size() == capacity_) throw std::runtime_error("prediction: waiting for authority (history full)");
    auto before = session_.Capture();
    auto events = session_.Step(inputs);
    pending_.push_back({std::move(before), inputs, std::move(events)});
}
void PredictionTimeline::DiscardPrediction() {
    if (!pending_.empty()) session_.Restore(pending_.front().before);
    pending_.clear();
}
void PredictionTimeline::AcceptRestoredState() {
    pending_.clear();
    confirmedNext_ = session_.NextTick();
}
unsigned PredictionTimeline::ConfirmedFinishSeconds() const {
    if(pending_.empty())return session_.FinishSeconds();
    const auto& s=pending_.front().before;
    return RaceSession::FinishSeconds(s.nextTick,s.firstFinish,s.finishing,s.complete,session_.FinishTimeout());
}
RaceSession::State PredictionTimeline::ConfirmedState() const {
    return pending_.empty() ? session_.Capture() : pending_.front().before;
}
void PredictionTimeline::StageAuthority(Tick tick,const Inputs& inputs) {
    for(const auto& p:inputs)if(!ValidInput(p))throw std::invalid_argument("prediction: invalid staged input");
    if(tick<confirmedNext_ || size_t(tick-confirmedNext_)>=pending_.size())return;
    auto& frame=pending_[size_t(tick-confirmedNext_)];
    if(!SameInputs(frame.inputs,inputs)){frame.inputs=inputs;frame.dirty=true;}
}
std::vector<Event> PredictionTimeline::Confirm(Tick tick, const Inputs& inputs) {
    if (tick < confirmedNext_) return {}; // retransmission; effects were already delivered
    if (tick != confirmedNext_ || pending_.empty()) throw std::invalid_argument("prediction: non-contiguous authority");
    for (const auto& p : inputs) if (!ValidInput(p)) throw std::invalid_argument("prediction: invalid authority input");
    if (!SameInputs(pending_.front().inputs, inputs) || std::any_of(pending_.begin(),pending_.end(),[](const auto& frame){return frame.dirty;})) {
        session_.Restore(pending_.front().before);
        pending_.front().inputs = inputs;
        for (auto& frame : pending_) {
            frame.before = session_.Capture();
            frame.events = session_.Step(frame.inputs);
            frame.dirty = false;
            ++replayed_;
        }
    }
    auto events = std::move(pending_.front().events);
    pending_.pop_front();
    ++confirmedNext_;
    return events;
}
} // namespace gt2::net
