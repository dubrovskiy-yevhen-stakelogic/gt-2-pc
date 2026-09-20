#include "game/sim/disc_data.h"

#include <cstring>
#include <stdexcept>

#include "game/sim/fixed.h"
#include "game/sim/trig.h"
#include "gt2formats/exe_profile.h"

namespace gt2::sim {

namespace {

// ---------------------------------------------------------------- overlay / executable addresses (Sim US v1.2)
// Every address below is the US Simulation v1.2 address; the images translate it to their own build through the address
// profile (GuestImage::Sim, gt2formats/exe_profile.h): the race overlay's tables sit 0xE0 lower in US Arcade v1.1
// (0x80046E20 -> 0x80046D40, the raw bytes identical), the executable's gear / arc-tangent tables 0x308 lower.

constexpr uint32_t kRawTuning = 0x80046E20u;       // raw byte tables of 0x8003B7B8 / 0x8003BA64
constexpr uint32_t kClassRows = 0x80046E5Cu;       // 16-byte rows per drive class, row 4 (+0x40) = dirt
constexpr uint32_t kPedalRaw = 0x80046ED4u;        // 6 bytes: the pedal rate divisors
constexpr uint32_t kSteerRaw = 0x80046EE5u;        // 3 bytes: steer spring / damping / centring
constexpr uint32_t kViewYaw = 0x80046C94u;
constexpr uint32_t kSteerCurve = 0x80046DA4u;      // { u16 count; s16* xs; s16* ys }
constexpr uint32_t kPedalRates = 0x80046DB0u;      // u16[16] (the last four overlap the two tables below)
constexpr uint32_t kSpringRange = 0x80046DC8u;
constexpr uint32_t kDiffCodes = 0x80046DCCu;
constexpr uint32_t kRaceStates = 0x80046DD4u;
constexpr uint32_t kListOrder = 0x80046DF8u;
constexpr uint32_t kWheelEffects = 0x80046EACu;
constexpr uint32_t kSlideSensitivity = 0x80046EE8u;
constexpr uint32_t kRoughnessBlock = 0x80046F88u;  // 0x40 bytes: amplitude s16[8], frequency s16[8], speed-scaled u8[8], ...
constexpr uint32_t kGearAutoTable = 0x800923E2u;   // executable
constexpr uint32_t kAtanTable = 0x800A4AC8u;       // executable

int32_t DivChecked(int32_t a, int32_t b) { // the MIPS `div` of the original; it traps on zero, we refuse
    if (b == 0) throw std::runtime_error("disc data: division by zero in a tuning table");
    return a / b;
}
inline int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
inline int32_t Neg(int32_t a) { return int32_t(0u - uint32_t(a)); }

} // namespace

int32_t ShellControlClass(const ShellState& shell, bool dirtCourse) { // 0x800418E8
    switch (shell.gameMode) {
    case 0:
        if (dirtCourse) return 0;
        return shell.modeFlag60 == 1 ? 2 : 0;
    case 2: case 4: case 0xC:
        if (dirtCourse) return 0;
        if (shell.modeFlag65 == 1) return 0;
        return shell.modeFlag5D == 1 ? 2 : 0;
    case 3: return 1;
    default: return 0;
    }
}

SimConstants LoadSimConstants(const GuestImage& ovl, const GuestImage& exe, const RaceSettings& s, const ShellState& shell, bool dirtCourse) {
    // The tables of this build: every base address translated once (the tables themselves are contiguous).
    const uint32_t rawTuning = ovl.Sim(kRawTuning), classRows = ovl.Sim(kClassRows), pedalRaw = ovl.Sim(kPedalRaw), steerRaw = ovl.Sim(kSteerRaw);
    auto b = [&](uint32_t address) { return int32_t(ovl.Get<uint8_t>(address)); };
    SimConstants c;
    if (ovl.profile) c.wheelEffectsTail = ovl.profile->wheelEffectsTail;

    // 0x8001523C: the frame constants from the shell's frame-rate mode.
    if (shell.frameRateMode == 1) { c.step.rate = 60; c.step.frameTime = 0x444; }
    else { c.step.rate = 30; c.step.frameTime = 0x888; }

    // ---- 0x8003B7B8: the aerodynamic, rolling, roughness and steering constants from the raw bytes.
    const int32_t dragBase = b(rawTuning);
    c.dragConstant = DivChecked(dragBase << 12, 10);                                       // 0x80046EF0
    c.step.draftDragFloor = dragBase == 0 ? 0 : DivChecked(b(rawTuning + 1) << 12, dragBase); // 0x80046EF4
    const int32_t rollingCount = b(rawTuning + 0xB);                                // 0x80046F38 / 0x801C8730
    if (rollingCount > 8) throw std::runtime_error("disc data: rolling-resistance curve longer than its 8-entry tables");
    for (int32_t i = 0; i < rollingCount; i++) {
        c.rollingXs.push_back(b(rawTuning + 0xC + uint32_t(i)) * 0x8E4);            // 0x80046EF8
        c.rollingYs.push_back(DivChecked(b(rawTuning + 0x14 + uint32_t(i)) << 12, 1000)); // 0x80046F18
    }
    std::array<uint8_t, 0x40> roughness{};
    std::memcpy(roughness.data(), ovl.At(ovl.Sim(kRoughnessBlock), roughness.size()), roughness.size());
    auto put16 = [&](size_t offset, int32_t v) { const int16_t s = int16_t(v); std::memcpy(&roughness[offset], &s, 2); };
    for (uint32_t i = 0; i < 8; i++) {
        put16(i * 2, DivChecked(b(rawTuning + 0x1C + i) << 12, 1000));                     // 0x80046F88 amplitude
        c.surfaceRolling[i] = DivChecked(b(rawTuning + 0x24 + i) << 12, 10);               // 0x80046E00
        put16(0x10 + i * 2, DivChecked(b(rawTuning + 0x2C + i) << 8, 10));                 // 0x80046F98 frequency
        roughness[0x20 + i] = uint8_t(b(rawTuning + 0x34 + i));                     // 0x80046FA8 speed-scaled
    }
    for (uint32_t i = 0; i < 16; i++) { // the 16-entry views of the contiguous block (ground.h)
        std::memcpy(&c.ground.roughnessAmplitude[i], &roughness[i * 2], 2);
        std::memcpy(&c.ground.roughnessFrequency[i], &roughness[0x10 + i * 2], 2);
        c.ground.roughnessSpeedScaled[i] = roughness[0x20 + i];
    }
    c.steerSpringGain = b(steerRaw) << 12;                                          // 0x80046F3C
    c.steerCentring = b(steerRaw + 2) << 12;                                        // 0x80046F44
    c.steerDamping = DivChecked(b(steerRaw + 1) << 12, 10);                                // 0x80046F40

    // ---- 0x8003BA64: the drive-class tuning records from the class rows and the race settings.
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t row = classRows + (dirtCourse ? 0x40u : k * 16u);
        DriveClassTuning& t = c.classTuning[k];
        t = DriveClassTuning{}; // BSS: fields the routine does not write stay 0
        t.steerGain = int16_t(DivChecked(b(row + 3) << 12, 100));
        t.lookaheadMin = b(row + 7) << 16;
        t.lookaheadMax = b(row + 8) << 16;
        t.lookaheadSpeedGain = int16_t(Div12Shift(0x1000, b(row + 6) * 0x8E4)); // 0x80075E90(0x1000, b * 0x8E4, 0)
        t.brakeMarginFactor = int16_t(DivChecked(0x64000, b(row)));
        t.cornerSpeedGain = int16_t(DivChecked(b(row + 1) << 12, 100));
        t.cornerSpeedBase = (b(row + 2) - 100) * 0x472;
        t.overshootThrottleCut = int16_t(DivChecked(b(row + 4) << 12, 100));
        t.overshootBrake = int16_t(DivChecked(b(row + 5) << 12, 100));
        t.frontGripWeight = int16_t(DivChecked(b(row + 0xE) << 12, 100));
        t.yawBrakeGain = int16_t(DivChecked(b(row + 9) << 12, 100));
        t.yawBrakeThreshold = int16_t(DivChecked(b(row + 0xA) << 12, 100));
        t.steerFalloffGain = int16_t(DivChecked(b(row + 0xB) << 12, 10));
        t.steerFalloffThreshold = int16_t(DivChecked(b(row + 0xC) << 12, 100));
        t.tractionGain = DivChecked(b(row + 0xD) << 12, 10);
    }
    for (uint32_t k = 0; k < 4; k++) {
        DriveClassTuning& t = c.classTuning[k];
        const int32_t corner = s.cornerGripPercent[k];
        int32_t scaled = (corner == 0 ? 100 : corner) << 12;
        if (corner > 100) scaled = 0x64000;
        t.throttleScale = int16_t(DivChecked(scaled, 100));
        const int32_t speed = s.speedScalePercent[k];
        const int32_t divisor = speed == 0 || speed > 100 ? 100 : speed;
        t.brakeMarginFactor = int16_t(DivChecked(int32_t(t.brakeMarginFactor) * 100, divisor));
    }
    c.wear = TyreWearConstants{};
    if (s.wearLimit != 0 && !dirtCourse && s.wearKnee < s.wearLimit && s.kneeGripLossPercent < s.wornGripLossPercent &&
        s.pitGripPercent < s.wornGripLossPercent) {
        c.wear.wearLimit = int32_t(s.wearLimit) * 10000;
        c.wear.coldLimit = int32_t(s.coldLimit) * -10000;
        c.wear.wearKnee = int32_t(s.wearKnee) * 10000;
        c.wear.wornGripLoss = DivChecked(int32_t(s.wornGripLossPercent) << 12, 100);
        c.wear.pitGripFactor = DivChecked((100 - int32_t(s.pitGripPercent)) * 0x1000, 100);
        c.wear.coldGripLoss = DivChecked(int32_t(s.coldGripLossPercent) << 12, 100);
        c.wear.kneeGripLoss = DivChecked(int32_t(s.kneeGripLossPercent) << 12, 100);
    }
    std::memcpy(c.pedalRates.data(), ovl.At(ovl.Sim(kPedalRates), 32), 32);
    for (uint32_t table = 0; table < 6; table++)
        for (uint32_t j = 0; j < 2; j++) {
            int16_t rate = int16_t(DivChecked(0x64000, b(pedalRaw + table) * c.step.rate));
            if (rate > 0x1000) rate = 0x1000;
            c.pedalRates[table * 2 + j] = uint16_t(rate);
        }

    // ---- plain tables of the overlay and the executable.
    c.slideSensitivity = ovl.Get<uint8_t>(ovl.Sim(kSlideSensitivity));
    {
        const uint32_t curve = ovl.Sim(kSteerCurve); // { count; xs; ys }: the pointers are this build's addresses
        const uint32_t count = ovl.Get<uint16_t>(curve), xs = ovl.Get<uint32_t>(curve + 4), ys = ovl.Get<uint32_t>(curve + 8);
        for (uint32_t i = 0; i < count; i++) {
            c.steerCurveXs.push_back(ovl.Get<int16_t>(xs + i * 2));
            c.steerCurveYs.push_back(ovl.Get<int16_t>(ys + i * 2));
        }
    }
    const uint32_t viewYaw = ovl.Sim(kViewYaw);
    for (uint32_t i = 0; i < 2; i++) {
        c.ground.viewYawGain[i] = ovl.Get<int32_t>(viewYaw + i * 4);
        c.ground.viewYawDamping[i] = ovl.Get<int32_t>(viewYaw + 8 + i * 4);
    }
    std::memcpy(&c.ground.effects, ovl.At(ovl.Sim(kWheelEffects), sizeof(c.ground.effects)), sizeof(c.ground.effects));
    const uint32_t springRange = ovl.Sim(kSpringRange);
    c.springRateRange[0] = ovl.Get<uint8_t>(springRange);
    c.springRateRange[1] = ovl.Get<uint8_t>(springRange + 1);
    std::memcpy(c.diffTypeCodes.data(), ovl.At(ovl.Sim(kDiffCodes), 8), 8);
    std::memcpy(c.raceStateTable.data(), ovl.At(ovl.Sim(kRaceStates), c.raceStateTable.size()), c.raceStateTable.size());
    std::memcpy(c.gearAutoTable.data(), exe.At(exe.Sim(kGearAutoTable), c.gearAutoTable.size()), c.gearAutoTable.size());
    c.aiGripPercent = s.aiGripPercent;

    // ---- shell state.
    c.gameMode = shell.gameMode;
    c.shellControlClass = ShellControlClass(shell, dirtCourse);
    c.viewMode = shell.viewMode;
    c.flag800A951C = shell.flag800A951C;
    c.flag801C9995 = shell.flag801C9995;
    c.flag800AF232 = shell.flag800AF232;
    c.word801C98A0 = s.controlWord;
    c.byte801D5869 = shell.byte801D5869;
    c.word80046F64 = shell.raceClock;
    return c;
}

std::span<const int16_t> AtanTableOf(const GuestImage& exe) {
    return {reinterpret_cast<const int16_t*>(exe.At(exe.Sim(kAtanTable), 4097 * 2)), 4097};
}

// ================================================================ 0x80038DA0: the race lists on the course

namespace {

struct RaceListBuilder {
    const Track& track;
    NativeCourse course;
    std::array<std::vector<TrackRaceRecord>, 7>& lists;
    const std::array<bool, 7>& present;
    const int8_t* raceStates;

    // 0x800386B4 -> 0x800287DC -> 0x80028394: the chunk of the simulation-plane point (x, y) from `chunk`.
    int32_t Walk(int32_t x, int32_t y, int32_t chunk) const {
        const int32_t point[3] = {int32_t(uint32_t(x) << 4), 0, Neg(int32_t(uint32_t(y) << 4))};
        return int32_t(FindChunkAlongCourse(track, uint32_t(chunk), point));
    }

    // 0x800386F4: walks from record i - 1 to record i (a straight or the arc of the previous corner) and
    // returns the chunk at the end; rewrites the previous record's type for corners.
    int32_t WalkSection(std::vector<TrackRaceRecord>& list, size_t i, int32_t chunk, uint32_t listIndex) const {
        TrackRaceRecord& prev = list[i - 1];
        const TrackRaceRecord& cur = list[i];
        const int32_t x0 = prev.x, y0 = prev.y, x1 = cur.x, y1 = cur.y;
        if (prev.type == 0) {
            int32_t steps = ApproxLength(Sub(x0, x1), Sub(y0, y1)) / 0x14000;
            if (steps == 0) steps = 1;
            for (int32_t k = 1; k <= steps; k++) {
                const int32_t t = (k << 12) / steps;
                chunk = Walk(Add(Mul12Wide(t, Sub(x1, x0)), x0), Add(Mul12Wide(t, Sub(y1, y0)), y0), chunk);
            }
            return chunk;
        }
        int32_t r = prev.radius;
        const int32_t s = Sin(uint32_t(prev.heading)), co = Cos(uint32_t(prev.heading));
        const int32_t cx = Sub(x0, Mul12Wide(co, r));
        const int32_t cy = Add(y0, Mul12Wide(Neg(s), r));
        int32_t a0 = Atan2(Sub(cx, x0), Sub(y0, cy));
        int32_t a1 = Atan2(Sub(cx, x1), Sub(y1, cy));
        int32_t span;
        if (r < 1) {
            prev.type = 2;
            r = Neg(r);
            if (a0 < a1) a0 += 0x1000;
            span = a0 - a1;
        } else {
            prev.type = 1;
            if (a1 < a0) a1 += 0x1000;
            span = a1 - a0;
        }
        int32_t steps = span / 0xAA;
        if (steps == 0) steps = 1;
        if (listIndex == 5) prev.type = 1;
        for (int32_t k = 1; k <= steps; k++) {
            const uint32_t angle = uint32_t(a0 + Mul12Wide((k << 12) / steps, a1 - a0)) & 0xFFF;
            chunk = Walk(Sub(cx, Mul12Wide(Sin(angle), r)), Add(cy, Mul12Wide(Cos(angle), r)), chunk);
        }
        return chunk;
    }

    // 0x800389E0: the road under a record and its slopes from four probes 2 m away. Returns false when the
    // record is off the road (its geometry fields stay zero).
    bool Probe(TrackRaceRecord& rec, int32_t chunk) {
        const int32_t sx = Mul12Wide(Sin(uint32_t(rec.heading)), 0x800), cx = Mul12Wide(Cos(uint32_t(rec.heading)), 0x800);
        ContactQuery query{};
        query.chunkIndex = uint16_t(uint32_t(chunk));
        query.x = int32_t(uint32_t(rec.x) << 4);
        query.height = 0x640000;
        query.y = int32_t(uint32_t(rec.y) << 4);
        course.Contact(query);
        int32_t centre, surface, attribute;
        if (query.surfaceHeight == kNoSurfaceHeight) { centre = kNoSurfaceHeight; surface = 0; attribute = 0; }
        else { centre = query.surfaceHeight >> 4; surface = query.surface; attribute = query.attribute1; }
        int32_t c = chunk;
        const int32_t hL = course.GroundHeight(Sub(rec.x, sx), Add(rec.y, cx), c);
        const int32_t hR = course.GroundHeight(Add(rec.x, sx), Sub(rec.y, cx), c);
        const int32_t hB = course.GroundHeight(Sub(rec.x, cx), Sub(rec.y, sx), c);
        const int32_t hF = course.GroundHeight(Add(rec.x, cx), Add(rec.y, sx), c);
        if (centre == kNoSurfaceHeight) return false;
        if (hL == kNoSurfaceHeight && hR == kNoSurfaceHeight) return false;
        if (hB == kNoSurfaceHeight && hF == kNoSurfaceHeight) return false;
        rec.height = centre;
        int32_t d, w;
        if (hL == kNoSurfaceHeight) { d = Sub(centre, hR); w = 0x800; }
        else if (hR == kNoSurfaceHeight) { d = Sub(hL, centre); w = 0x800; }
        else { d = Sub(hL, hR); w = 0x1000; }
        rec.slopeAcross = Atan2(d, w);
        if (hB == kNoSurfaceHeight) { d = Sub(centre, hF); w = 0x800; }
        else if (hF == kNoSurfaceHeight) { d = Sub(hB, centre); w = 0x800; }
        else { d = Sub(hB, hF); w = 0x1000; }
        rec.slopeAlong = Atan2(d, w);
        rec.surface = int8_t(surface);
        rec.attribute = int8_t(attribute);
        return true;
    }

    // 0x800358E0: the first present list among the candidates of race state `state`.
    int ListForState(uint32_t state) const {
        if (state > 6) state = 6;
        for (uint32_t k = 0; k < 5; k++) {
            const int8_t li = raceStates[state * 5 + k];
            if (li >= 0 && li < 7 && present[size_t(li)]) return li;
        }
        return -1;
    }

    // 0x80038C88: the chunk of a grid record from the section records of race state 6.
    bool GridChunk(int32_t& chunk, int32_t x, int32_t y) {
        const int li = ListForState(6);
        if (li < 0) return false;
        for (const TrackRaceRecord& q : lists[size_t(li)]) {
            int32_t c = int16_t(q.chunk);
            if (course.GroundHeight(x, y, c) != kNoSurfaceHeight) { chunk = c; return true; }
        }
        return false;
    }

    void Build(const std::array<uint8_t, 7>& order, int32_t courseLength) {
        for (uint32_t step = 0; step < 7; step++) {
            const uint32_t li = order[step];
            if (li >= 7 || !present[li]) continue;
            std::vector<TrackRaceRecord>& list = lists[li];
            int32_t chunk = course.NearestChunk(track.startGrid[0].data()); // 0x80028288 on the start position (tro + 0x58)
            for (size_t i = 0; i < list.size(); i++) {
                TrackRaceRecord& rec = list[i];
                if (li == 4) GridChunk(chunk, rec.x, rec.y);
                else if (i == 0) chunk = Walk(rec.x, rec.y, chunk);
                else chunk = WalkSection(list, i, chunk, li);
                rec.chunk = uint16_t(uint32_t(chunk));
                rec.distance = course.CourseDistance(chunk, int32_t(uint32_t(rec.x) << 4), 0, int32_t(uint32_t(rec.y) << 4));
                rec.height = 0;
                rec.slopeAcross = 0;
                rec.slopeAlong = 0;
                rec.surface = 0;
                rec.attribute = 0;
                if (li != 5) Probe(rec, chunk);
                if (li == 4 && rec.distance < courseLength / 2) rec.distance = Add(rec.distance, courseLength);
            }
        }
    }
};

} // namespace

RaceCourseData BuildRaceCourseData(const Track& track, const CourseExtras& extras, const TrackRaceData& file, const GuestImage& ovl, bool dirtCourse) {
    if (extras.chunks.size() != track.chunks.size()) throw std::runtime_error("race data: course extras do not match the track");
    std::array<uint8_t, 7> order{};
    std::memcpy(order.data(), ovl.At(ovl.Sim(kListOrder), 7), 7);
    const int8_t* raceStates = reinterpret_cast<const int8_t*>(ovl.At(ovl.Sim(kRaceStates), 35));
    std::array<std::vector<TrackRaceRecord>, 7> lists = file.lists;
    RaceListBuilder builder{track, NativeCourse(track, extras), lists, file.present, raceStates};
    builder.Build(order, extras.courseLength);

    RaceCourseData data;
    data.dirtCourse = dirtCourse;
    for (size_t i = 0; i < file.startLineDistances.size() && i < 16; i++) data.startLineDistances.push_back(file.startLineDistances[i]);
    for (size_t li = 0; li < 7; li++) {
        if (!file.present[li]) continue;
        data.sections[li].resize(lists[li].size());
        if (!lists[li].empty()) std::memcpy(data.sections[li].data(), lists[li].data(), lists[li].size() * sizeof(RaceSection));
    }
    data.grid.count = file.present[4] ? int32_t(lists[4].size()) : 0;
    for (size_t slot = 0; slot < lists[4].size() || slot < 12; slot++) { // 12 records like the dump's; the reset path indexes them without a count check
        const bool have = file.present[4] && slot < lists[4].size();
        data.grid.distance.push_back(have ? lists[4][slot].distance : 0);
        data.grid.heading.push_back(have ? lists[4][slot].heading : 0);
    }
    return data;
}

} // namespace gt2::sim
