#include "game/net/state_codec.h"
#include "game/net/state_archive.h"
#include <algorithm>
#include <limits>

namespace gt2::net {
// Schema v1: explicit scalar order. Reserved native storage and setup-only car
// parameters remain local. Keep this list stable; changing it requires a version bump.
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::Wheel>
void Fields(A& a, T& v) {
    a.FieldsOf(v.surfaceHeightAvg, v.surfaceHeight, v.load, v.steerAngle, v.travel);
    a.FieldsOf(v.travelVelocity, v.surface, v.surfaceAttribute, v.accel, v.rimSpeed);
    a.FieldsOf(v.skidLevel, v.smokeLevel, v.skidDelay, v.dustLevel, v.rotation);
    a.FieldsOf(v.damage, v.offsetX, v.offsetY, v.offsetHeight, v.slipScale);
    a.FieldsOf(v.contactForwardSpeed, v.contactLateralSpeed, v.gripForce, v.wearGrip, v.slipRatioAbs);
    a.FieldsOf(v.lateralFactorAbs, v.wearRate, v.wearStage, v.speedFactor, v.roughness);
    a.FieldsOf(v.slipRatio, v.slipRatioGrip, v.springForce, v.damperForce, v.slipAngle);
    a.FieldsOf(v.slipBlend, v.lateralForce, v.driveForce, v.brakeForce, v.brakeInput);
    a.FieldsOf(v.contactFlags, v.wear);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::CarBody>
void Fields(A& a, T& v) {
    a.FieldsOf(v.contactType, v.wheels, v.chunkIndex, v.courseDistance, v.lap);
    a.FieldsOf(v.clutchRequest, v.steerAngle, v.steerAngleRate, v.throttle, v.brake);
    a.FieldsOf(v.handbrake, v.gear, v.clutchState, v.axleDiffLocked, v.shiftTimer);
    a.FieldsOf(v.revLimiterActive, v.engineLoad, v.turboSpool, v.engineSpeed, v.velocity);
    a.FieldsOf(v.axleSpeed, v.centreLocked, v.contactFlags, v.scrapeDirection, v.hitCorner);
    a.FieldsOf(v.wallScrape, v.transmissionMode, v.pitch, v.roll, v.heading);
    a.FieldsOf(v.yawAccel, v.yawRate, v.pitchRate, v.rollRate, v.hitNormal0);
    a.FieldsOf(v.hitNormal1, v.dirtiness, v.position, v.basis, v.visualPosition);
    a.FieldsOf(v.visualBasis, v.forwardSpeed, v.lateralSpeed, v.engineRpm, v.speedReadout);
    a.FieldsOf(v.sector, v.airborne, v.gridSlot, v.footprintSet, v.footprint);
    a.FieldsOf(v.visualPitch, v.visualRoll, v.maxSpeedReadout, v.resetState, v.resetCounter);
    a.FieldsOf(v.finishFlag, v.inputFlag6FD, v.stepTime, v.steerBlend, v.stepRate12);
    a.FieldsOf(v.effectiveThrottle, v.steerLimitNeg, v.steerLimitPos, v.engineTorque, v.engineBrakeTorque);
    a.FieldsOf(v.scriptedControl, v.contactMask, v.dragForce, v.downforceForce, v.forwardAccel);
    a.FieldsOf(v.lateralAccel, v.externalLongForce, v.externalLatForce, v.netLongitudinalForce, v.impactHold);
    a.FieldsOf(v.viewYawOffset, v.viewPitchOffset, v.impactTimer, v.wallImpact, v.intakeLoad);
    a.FieldsOf(v.blowOffState, v.turboSpoolMax, v.viewYaw, v.viewYawRate, v.viewPitch);
    a.FieldsOf(v.viewPitchRate, v.racePosition, v.licenseState, v.viewYawAccel, v.licenseCode);
    a.FieldsOf(v.engineVisual0, v.engineVisual1, v.exhaustFlame, v.licenseTime, v.word760);
    a.FieldsOf(v.loadSoundLevel, v.impactSoundFlag, v.messageCode, v.messageFrames, v.timeScale);
    a.FieldsOf(v.steerInput, v.aiScriptThrottleOn, v.neighbourClass, v.neighbourFlags, v.draftInput);
    a.FieldsOf(v.draftBlend, v.pushForce, v.lastLineTime, v.aiSlowZone, v.wallHitMask);
    a.FieldsOf(v.raceState, v.aiRecovery, v.aiLine, v.aiSection, v.aiPreviousType);
    a.FieldsOf(v.aiSectionType, v.flags78D, v.penaltyFrames, v.aiPreviousTargetSpeed, v.aiTargetSpeed);
    a.FieldsOf(v.hudInvalid, v.hudMessageKind, v.hudTimer, v.hudTimer2, v.hudTimer3);
    a.FieldsOf(v.hudValue, v.hudLabel, v.hudCompare, v.hudGap);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::ContactCorner>
void Fields(A& a, T& v) {
    a.FieldsOf(v.edgeFlags, v.x, v.y);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::CarContactState>
void Fields(A& a, T& v) {
    a.FieldsOf(v.buffer, v.fraction, v.corner, v.side, v.corners);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::DriveWheelWork>
void Fields(A& a, T& v) {
    a.FieldsOf(v.brakeTorque, v.roadForce, v.torque, v.steerCos, v.steerSin);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::DriveCarWork>
void Fields(A& a, T& v) {
    a.FieldsOf(v.wheels, v.axleTorque, v.clutchInputSpeed, v.clutchOutputSpeed, v.clutchEngagement);
    a.FieldsOf(v.activeDiffFactor);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::DriveStepWork>
void Fields(A& a, T& v) {
    a.FieldsOf(v.stepTime, v.cars);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::DrivetrainGlobals>
void Fields(A& a, T& v) {
    a.FieldsOf(v.raceModeByte, v.wheelClutch);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::GearRequest>
void Fields(A& a, T& v) {
    a.FieldsOf(v.reverse, v.shift, v.reserved);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::SectorBoard>
void Fields(A& a, T& v) {
    a.FieldsOf(v.times, v.bestLap, v.count, v.shown, v.car);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::LapEntry>
void Fields(A& a, T& v) {
    a.FieldsOf(v.time, v.split, v.maxSpeed, v.pad);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::PlayerResults>
void Fields(A& a, T& v) {
    a.FieldsOf(v.position, v.lapNumber, v.count, v.bestLapNumber, v.laps);
    a.FieldsOf(v.best, v.pending, v.finishTime);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, sim::RaceShellState>
void Fields(A& a, T& v) {
    a.FieldsOf(v.raceClock, v.twoPlayerLaps, v.timeLimited, v.holdInitial, v.hold);
    a.FieldsOf(v.sinceFinish, v.board, v.startTimer, v.endTimer, v.endTimerAux);
    a.FieldsOf(v.clockFrames, v.finishPosition, v.newRecord, v.licenseResult, v.licenseTime);
    a.FieldsOf(v.pointsTotal, v.pointsRace, v.results, v.courseRecord, v.musicRequestFlag);
    a.FieldsOf(v.musicRequest, v.musicRaceTrack, v.music2F0, v.music2F1);
}
template<class A, class T> requires std::is_same_v<std::remove_const_t<T>, Result>
void Fields(A& a, T& v) {
    a.FieldsOf(v.bestLap, v.lastLap, v.finishTime, v.laps, v.position, v.dnf, v.lastLapValid);
}
namespace {
uint64_t Checksum(std::span<const uint8_t> bytes) {
    uint64_t value = 14695981039346656037ull;
    for (auto b : bytes) { value ^= b; value *= 1099511628211ull; }
    return value; // accidental corruption only, not authentication
}
void Require(bool valid, const char* reason) { if (!valid) throw std::invalid_argument(reason); }
}
std::vector<uint8_t> StateCodec::Encode(const RaceSession& session, const ContentKey& content) {
    return EncodeState(session, content, session.Capture());
}
uint64_t StateCodec::Digest(const RaceSession& session, const ContentKey& content, const RaceSession::State& state) {
    const auto bytes = EncodeState(session, content, state);
    StateReader tail(std::span<const uint8_t>(bytes).last(8));
    uint64_t digest = 0; tail(digest); return digest;
}
std::vector<uint8_t> StateCodec::EncodeState(const RaceSession& session, const ContentKey& content, const RaceSession::State& s) {
    Require(content != ContentKey{} && session.race_.networkDrivers_, "state: network setup and content key required");
    Require(s.race.setup == session.race_.setupIdentity_ && s.race.cars.size() == session.CarCount(),
        "state: checkpoint belongs to a different race");
    StateWriter a;
    a.FieldsOf(uint32_t(0x53533247), kVersion, uint8_t(session.CarCount()), uint8_t(0), content, session.finishTimeout_);
    for (const auto& participant : session.roster_) a(participant.player);
    a.FieldsOf(s.nextTick, s.firstFinish, s.finishing, s.complete, s.results);
    for (const auto& car : s.race.cars) a.FieldsOf(car.finishTime, car.body);
    a.FieldsOf(s.race.contact, s.race.work, s.race.requests, s.race.deltas, s.race.shell,
        s.race.order, s.race.drivetrain, s.race.taskOver);
    // The ground scratchpad is rewritten by PrepareContactQueries/GroundPass
    // before its next use. Its overlapping native views are deliberately omitted.
    a(Checksum(a.bytes));
    Require(a.bytes.size() <= kMaxBytes, "state: snapshot size exceeded");
    return std::move(a.bytes);
}
void StateCodec::Restore(RaceSession& session, const ContentKey& expected, std::span<const uint8_t> bytes, std::optional<Tick> expectedTick) {
    Require(expected != ContentKey{} && session.race_.networkDrivers_, "state: network setup and content key required");
    Require(bytes.size() >= 8 && bytes.size() <= kMaxBytes, "state: invalid size");
    const auto payload = bytes.first(bytes.size() - 8);
    StateReader tail(bytes.last(8)); uint64_t checksum = 0; tail(checksum);
    Require(checksum == Checksum(payload), "state: checksum mismatch");
    StateReader a(payload);
    uint32_t magic = 0; uint16_t version = 0; uint8_t cars = 0, flags = 0;
    ContentKey content{}; Tick finishTimeout = 0;
    a.FieldsOf(magic, version, cars, flags, content, finishTimeout);
    Require(magic == 0x53533247 && version == kVersion && !flags, "state: unsupported schema");
    Require(content == expected && cars == session.CarCount() && finishTimeout == session.finishTimeout_, "state: setup mismatch");
    for (const auto& participant : session.roster_) {
        PlayerId player = 0; a(player);
        Require(player == participant.player, "state: participant mismatch");
    }
    auto s = session.Capture(); // staged in local storage, never mutate the live race while decoding
    a.FieldsOf(s.nextTick, s.firstFinish, s.finishing, s.complete, s.results);
    Require(!expectedTick || *expectedTick == s.nextTick, "state: transfer boundary mismatch");
    for (auto& car : s.race.cars) a.FieldsOf(car.finishTime, car.body);
    a.FieldsOf(s.race.contact, s.race.work, s.race.requests, s.race.deltas, s.race.shell,
        s.race.order, s.race.drivetrain, s.race.taskOver);
    Require(a.Done(), "state: trailing fields");
    Require(s.nextTick != std::numeric_limits<Tick>::max() && (!s.finishing || s.firstFinish < s.nextTick) &&
        (s.finishing || (!s.firstFinish && !s.complete)), "state: invalid session clock");
    Require(s.race.contact.buffer == 0 || s.race.contact.buffer == 1, "state: invalid contact buffer");
    uint8_t seen = 0;
    for (size_t i = 0; i < cars; ++i) {
        const int order = s.race.order[i];
        Require(order >= 0 && order < cars && !(seen & (1u << order)), "state: invalid race order");
        seen |= uint8_t(1u << order);
        const auto& body = s.race.cars[i].body;
        Require(body.gear <= body.forwardGears && body.gear < 8 && body.clutchState < 4 && body.footprintSet < 2 &&
            body.chunkIndex >= 0 && size_t(body.chunkIndex) < session.race_.track_->chunks.size() &&
            body.sector <= 3 && body.aiLine < 7 && body.raceState < 8 && body.contactType <= 2 &&
            body.racePosition >= 1 && body.racePosition <= cars && body.transmissionMode <= 1,
            "state: invalid car index or mode");
        Require(body.aiSection <= session.race_.courseData_.sections[body.aiLine].size(), "state: invalid AI section");
        for (const auto& wheel : body.wheels) Require(wheel.surface < 16, "state: invalid wheel surface");
        const auto& result = s.results[i];
        Require(result.position <= cars && result.bestLap >= -1 && result.lastLap >= -1 && result.finishTime >= -1 &&
            (!result.dnf || (s.complete && result.finishTime < 0)), "state: invalid participant result");
    }
    for (size_t i = cars; i < kSlots; ++i) Require(s.results[i] == Result{}, "state: inactive participant result");
    for (size_t i = 0; i < kSlots; ++i) for (size_t j = 0; j < kSlots - 1; ++j) {
        const auto corner = s.race.contact.corner[i][j];
        const auto side = s.race.contact.side[i][j];
        Require((corner == 255 || corner < 4) && side >= -1 && side < 4, "state: invalid contact index");
    }
    const auto& shell = s.race.shell;
    for (size_t i = 0; i < 4; ++i) {
        Require(shell.board.count[i] >= 0 && shell.board.count[i] <= cars && shell.board.shown[i] >= 0 &&
            shell.board.shown[i] <= shell.board.count[i], "state: invalid sector board count");
        for (auto car : shell.board.car[i]) Require(car >= -1 && car < cars, "state: invalid sector car");
    }
    for (const auto& result : shell.results) Require(result.count >= 0 && result.count <= 10, "state: invalid lap count");
    Require(shell.timeLimited <= 1 && shell.twoPlayerLaps <= 1 && shell.hold <= shell.holdInitial &&
        s.race.drivetrain.raceModeByte == session.race_.constants_.gameMode, "state: invalid shell mode");
    std::fill(s.race.scratch.begin(), s.race.scratch.end(), uint8_t(0));
    session.Restore(s);
}
std::vector<std::vector<uint8_t>> EncodeStateChunks(uint64_t session, Tick tick, std::span<const uint8_t> bytes) {
    Require(session && !bytes.empty() && bytes.size() <= StateCodec::kMaxBytes, "state transfer: invalid source");
    std::vector<std::vector<uint8_t>> result;
    for (size_t offset = 0; offset < bytes.size(); offset += 3072) {
        const auto count = std::min(size_t(3072), bytes.size() - offset);
        StateWriter a;
        a.FieldsOf(uint32_t(0x43533247), uint16_t(1), uint16_t(0), session, tick,
            uint32_t(bytes.size()), uint32_t(offset), uint32_t(count));
        const auto headerSize = a.bytes.size();
        a.bytes.resize(headerSize + count);
        std::copy_n(bytes.data() + offset, count, a.bytes.data() + headerSize);
        result.push_back(std::move(a.bytes));
    }
    return result;
}
void StateAssembler::Feed(std::span<const uint8_t> bytes) {
    Require(bytes.size() >= 32 && bytes.size() <= 3104 && !Complete(), "state transfer: invalid chunk size");
    StateReader a(bytes.first(32));
    uint32_t magic = 0, total = 0, offset = 0, count = 0; uint16_t version = 0, flags = 0;
    uint64_t session = 0; Tick tick = 0;
    a.FieldsOf(magic, version, flags, session, tick, total, offset, count);
    Require(magic == 0x43533247 && version == 1 && !flags && session == session_ && tick == tick_, "state transfer: wrong identity");
    Require(total > 0 && total <= StateCodec::kMaxBytes && (!total_ || total == total_) && offset == bytes_.size() &&
        offset < total && count == std::min(uint32_t(3072), total - offset) && bytes.size() == 32 + count,
        "state transfer: invalid chunk sequence");
    bytes_.insert(bytes_.end(), bytes.begin() + 32, bytes.end()); total_ = total;
}
RoomState StateAssembler::Take() {
    Require(Complete(), "state transfer: incomplete snapshot");
    return {tick_, std::move(bytes_)};
}
} // namespace gt2::net
