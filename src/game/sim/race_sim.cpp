#include "game/sim/race_sim.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "game/sim/fixed.h"
#include "game/sim/trig.h"

namespace gt2::sim {

namespace {

// 32-bit wrapping arithmetic like the original's.
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t Neg(int32_t a) { return int32_t(0u - uint32_t(a)); }

// 0x80081A78: the low 32 bits of (dot product of the s32 vector and the s16 direction) >> 12.
int32_t Dot12(const int32_t v[3], const std::array<int16_t, 3>& direction) {
    int64_t dot = 0;
    for (uint32_t i = 0; i < 3; i++) dot += int64_t(direction[i]) * int64_t(v[i]);
    return int32_t(uint64_t(dot) >> 12);
}

// Rotation rows of the body's attitude (forward, lateral, up; x, y, height) into the PSX render matrix and the
// physics position into the 16.16 world position (0x8001336C).
CarPose ConvertPose(const int32_t position[3], const int16_t rows[3][4]) {
    CarPose pose;
    const int32_t px = position[0], py = position[1], pz = position[2];
    pose.worldPosition = {int32_t(uint32_t(px) << 4), int32_t(uint32_t(pz) << 4), int32_t(uint32_t(py) * 0xFFFFFFF0u)};
    auto row = [&](uint32_t r, uint32_t k) { return rows[r][k]; };
    // out[0] = row1.x, out[1] = row2.x, out[2] = -row0.x; out[3] = row1.z, out[4] = row2.z, out[5] = -row0.z;
    // out[6] = -row1.y, out[7] = -row2.y, out[8] = row0.y  (row 0 forward, 1 lateral, 2 up; x, y, height)
    pose.rotation[0] = {row(1, 0), row(2, 0), int16_t(-row(0, 0))};
    pose.rotation[1] = {row(1, 2), row(2, 2), int16_t(-row(0, 2))};
    pose.rotation[2] = {int16_t(-row(1, 1)), int16_t(-row(2, 1)), row(0, 1)};
    return pose;
}

// 0x800133F0 after the conversion: the reference point is moved along the car by the CG offset (GTE MVMVA with
// sf = 0 on (0, 0, cgOffset): MAC_i = m[i][2] * cgOffset, added >> 8 to the 16.16 position).
void ApplyReferenceOffset(CarPose& pose, int32_t cgOffset) {
    for (uint32_t i = 0; i < 3; i++) pose.worldPosition[i] = Add(pose.worldPosition[i], (int32_t(pose.rotation[i][2]) * cgOffset) >> 8);
}

} // namespace

// ================================================================ course extras from the .tro

CourseExtras BuildCourseExtras(const Track& track, std::span<const uint8_t> tro) {
    // gt2::ParseTrack parses the surface grids and the distance fields now; the .tro bytes are only checked
    // to be the file the track came from.
    if (!tro.empty() && (tro.size() < 0x1A0 || std::memcmp(tro.data(), "@(#)GT-PS", 9) != 0)) throw std::runtime_error("course extras: bad magic");
    if (track.chunks.empty()) throw std::runtime_error("course extras: the track has no chunks");
    CourseExtras extras;
    extras.courseLength = track.courseLength;
    for (const TrackChunk& chunk : track.chunks) {
        extras.chunks.push_back({chunk.distance, chunk.weightNext, chunk.weightThis});
        SurfaceGrid g;
        g.originX = chunk.surfaceGrid.originX;
        g.originZ = chunk.surfaceGrid.originZ;
        g.shiftX = chunk.surfaceGrid.shiftX;
        g.shiftZ = chunk.surfaceGrid.shiftZ;
        for (size_t cell = 0; cell < 16; cell++) g.cells[cell] = chunk.surfaceGrid.cells[cell];
        extras.grids.push_back(std::move(g));
    }
    return extras;
}

// ================================================================ native course queries

NativeCourse::NativeCourse(const Track& track, const CourseExtras& extras) : extras_(&extras) {
    surface_.track = &track;
    surface_.grids = extras.grids;
}

int32_t NativeCourse::GroundHeight(int32_t x, int32_t y, int32_t& chunk) { // 0x80028900
    ContactQuery query{};
    query.chunkIndex = uint16_t(uint32_t(chunk));
    query.x = int32_t(uint32_t(x) << 4);
    query.height = 0x64000 << 4; // the constant the shell passes (100 m in 1/4096 m)
    query.y = int32_t(uint32_t(y) << 4);
    QueryContact(surface_, query);
    chunk = int32_t(query.chunkIndex);
    return query.surfaceHeight == kNoSurfaceHeight ? kNoSurfaceHeight : query.surfaceHeight >> 4;
}

int32_t NativeCourse::CourseDistance(int32_t chunk, int32_t x16, int32_t height16, int32_t y16) { // 0x80028C6C
    const int32_t point[3] = {x16, height16, Neg(y16)};
    return CourseDistanceOfWorldPoint(chunk, point);
}

void NativeCourse::Contact(ContactQuery& query) { QueryContact(surface_, query); } // 0x80028830

int32_t NativeCourse::CourseDistanceOfWorldPoint(int32_t chunkHint, const int32_t point[3]) const { // 0x80028588
    const Track& track = *surface_.track;
    const uint32_t index = FindChunkAlongCourse(track, uint32_t(chunkHint), point);
    const TrackChunk& chunk = track.chunks[index];
    const ChunkExtra& extra = extras_->chunks[index];
    const uint32_t nextIndex = chunk.next;
    const TrackChunk& next = track.chunks[nextIndex];
    int32_t d[3];
    for (uint32_t i = 0; i < 3; i++) d[i] = Sub(point[i], chunk.origin[i]);
    const int32_t a = Dot12(d, chunk.direction);
    const int32_t weightThis = int16_t(extra.weightThis);
    const int32_t p1 = int32_t(uint64_t(int64_t(a) * int64_t(weightThis)) >> 12);
    for (uint32_t i = 0; i < 3; i++) d[i] = Sub(next.origin[i], point[i]);
    const int32_t b = Dot12(d, next.direction);
    const int32_t weightNext = int16_t(extra.weightNext);
    const int32_t p2 = int32_t(uint64_t(int64_t(b) * int64_t(weightNext)) >> 12);
    const int32_t sum = Add(p2, p1);
    const int32_t fraction = sum == 0 ? 0 : int32_t(uint64_t(Div64(int64_t(p1) << 12, int64_t(sum))));
    const int32_t length = extras_->courseLength;
    const int32_t span = Sub(extras_->chunks[nextIndex].distance, extra.distance);
    int64_t product;
    if (length < span) product = int64_t(Sub(span, length)) * int64_t(fraction);
    else if (span >= 0) product = int64_t(span) * int64_t(fraction);
    else product = int64_t(Add(span, length)) * int64_t(fraction);
    int32_t result = Add(extra.distance, int32_t(uint64_t(product >> 12)));
    if (length < result) result = Sub(result, length);
    else if (result < 0) result = Add(result, length);
    return result;
}

int32_t NativeCourse::NearestChunk(const int32_t point[3]) const { // 0x80028288
    const Track& track = *surface_.track;
    uint32_t best = 0xFFFFFFFFu;
    int32_t bestIndex = -1;
    for (size_t i = 0; i < track.chunks.size(); i++) {
        const TrackChunk& chunk = track.chunks[i];
        auto magnitude = [&](uint32_t k) {
            const int32_t delta = Sub(chunk.centre[k], point[k]);
            return delta < 0 ? uint32_t(Neg(delta)) : uint32_t(delta);
        };
        uint32_t dx = magnitude(0), dy = magnitude(1), dz = magnitude(2);
        uint32_t mid = dy;
        if (dx < dy) { mid = dx; dx = dy; }
        uint32_t low = dz;
        if (dx < dz) { low = dx; dx = dz; }
        uint32_t least = low;
        if (mid < low) { least = mid; mid = low; }
        const uint32_t metric = dx + (mid >> 1) + (least >> 2);
        if (metric < best) { best = metric; bestIndex = int32_t(i); }
    }
    if (bestIndex > 0) bestIndex = int32_t(FindChunkAlongCourse(track, uint32_t(bestIndex), point));
    return bestIndex;
}

// ================================================================ entries

int16_t EntryPadSlot(uint8_t entryKind) { // 0x80012CD4: sh 2 / 3 / 1 / 0 -> car + 0x18
    switch (entryKind) {
    case kEntryPlayer1: return 2;
    case kEntryPlayer2: return 3;
    case kEntryGhost: return 1;
    default: return 0;
    }
}

uint8_t EntryControlClass(uint8_t entryKind) { // 0x80012CD4: the 8th argument of 0x80033384 (sp + 0x40)
    return entryKind == kEntryGhost || entryKind == kEntryPlayer1 || entryKind == kEntryPlayer2 ? 0 : 2;
}

// ================================================================ grid placement

namespace {

using Rot = int16_t[3][3];

int16_t Sat16(int64_t v) { return int16_t(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }

// 0x8008220C(out, m, v, t): out = m v / 4096 + t with v split into 12-bit slices: low (MVMVA sf 1 -> IR), high (v >> 24,
// sf 0 -> IR, << 12), middle (sf 0 -> MAC). The same GTE sequence as camera/race_camera.cpp ApplyLong.
void ApplyLong(int32_t out[3], const Rot& m, const int32_t v[3], const int32_t t[3]) {
    int32_t r[3];
    for (int i = 0; i < 3; i++) {
        int64_t low = 0, mid = 0, high = 0;
        for (int k = 0; k < 3; k++) {
            low += int64_t(m[i][k]) * (v[k] & 0xFFF);
            mid += int64_t(m[i][k]) * ((v[k] >> 12) & 0xFFF);
            high += int64_t(m[i][k]) * int16_t(v[k] >> 24);
        }
        r[i] = int32_t(uint32_t(Sat16(low >> 12)) + (uint32_t(int32_t(Sat16(high))) << 12) + uint32_t(int32_t(mid)) + uint32_t(t[i]));
    }
    std::memcpy(out, r, sizeof r);
}

// 0x8007B994(out, a, b): out = a * b through MVMVA (sf = 1, lm = 0).
void MulMatrix(Rot& out, const Rot& a, const Rot& b) {
    int16_t r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int64_t s = 0;
            for (int k = 0; k < 3; k++) s += int64_t(a[i][k]) * b[k][j];
            r[i][j] = Sat16(s >> 12);
        }
    std::memcpy(out, r, sizeof r);
}

} // namespace

int32_t CarNoseOffset(int16_t lod0BboxMinZ, int16_t lod0Scale) { // 0x80017E74: local_20 = scaled(bbox + 0x40) << 4
    const int32_t shift = int32_t(uint32_t(uint16_t(lod0Scale)) - 16u); // (u16 + 0x4C) - 0x10
    const int32_t v = lod0BboxMinZ;
    const int32_t scaled = shift < 0 ? v >> ((-shift) & 31) : int32_t(uint32_t(v) << (shift & 31));
    return int32_t(uint32_t(scaled) << 4);
}

uint8_t GridSlotOfEntry(uint8_t entryGridSlot, uint8_t gameMode, uint16_t courseFlags) { // 0x80012CD4 (0x80012378 = mode 6)
    return gameMode == 6 && (courseFlags & 0x20) == 0 ? uint8_t(2) : entryGridSlot;
}

RaceSlot PlaceOnGrid(const Track& track, const NativeCourse& course, const GridPlacement& grid) { // 0x80012CD4 up to 0x80033384
    const int32_t hint = course.NearestChunk(grid.pole.data()); // 0x80028288(chunks, 0x800B4A8C)
    // car + 0x81C: 0x8007AF60, 0x8007B050(m, slot position), 0x8007B14C(m, angle), 0x80017E74 -> 0x8007B050(m, (0, 0, nose))
    Rot m = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};
    int32_t t[3] = {0, 0, 0};
    ApplyLong(t, m, grid.position.data(), t);
    const uint32_t angle = uint32_t(grid.startAngle) & 0xFFF;
    const int16_t s = int16_t(Sin(angle)), c = int16_t(Cos(angle)); // the table at angle and angle + 0x400
    const Rot ry = {{c, 0, s}, {0, 4096, 0}, {int16_t(-s), 0, c}};
    MulMatrix(m, m, ry);
    if (grid.noseZ != 0) {
        const int32_t nose[3] = {0, 0, grid.noseZ};
        ApplyLong(t, m, nose, t);
    }
    RaceSlot slot;
    // 0x80028394(chunks, hint, car + 0x830): the hint is used as the original has it (0x80028288 answers -1 only for an
    // empty chunk table, which ParseTrack rejects).
    slot.chunkHint = int32_t(FindChunkAlongCourse(track, uint32_t(hint < 0 ? 0 : hint), t));
    slot.x = t[0] >> 4;
    slot.y = Neg(t[2]) >> 4;
    slot.headingSin = s;
    slot.headingCos = c;
    return slot;
}

RaceSlot SlotFromTrackGrid(const Track& track, const NativeCourse& course, size_t slot, int32_t noseZ) {
    if (slot >= track.startGrid.size()) throw std::runtime_error("race: grid slot out of range");
    GridPlacement grid;
    grid.pole = track.startGrid[0];
    grid.position = track.startGrid[slot];
    grid.startAngle = track.startAngle;
    grid.noseZ = noseZ;
    return PlaceOnGrid(track, course, grid);
}

// ================================================================ the race

void RaceSim::Setup(const Track& track, const CourseExtras& extras, const SimConstants& constants, const RaceCourseData& courseData,
                    std::vector<CarParams> params, const std::vector<RaceSlot>& slots, const RaceShellOptions& shellOptions) {
    if (slots.empty() || slots.size() > kMaxCars) throw std::runtime_error("race: 1 to 6 cars");
    if (params.size() != slots.size()) throw std::runtime_error("race: one parameter record per slot");
    if (extras.chunks.size() != track.chunks.size()) throw std::runtime_error("race: course extras do not match the track");
    track_ = &track;
    course_ = NativeCourse(track, extras);
    constants_ = constants;
    courseData_ = courseData;
    params_ = std::move(params);
    cars_.assign(slots.size(), Car{});
    curves_.assign(slots.size(), CarCurves{});
    input_.assign(slots.size(), InputTuning{});
    std::memset(&contact_, 0, sizeof(contact_));
    std::memset(&work_, 0, sizeof(work_));
    requests_.fill(GearRequest{});
    std::memset(deltas_, 0, sizeof(deltas_));
    std::fill(scratch_.begin(), scratch_.end(), uint8_t(0));
    shellOptions_ = shellOptions;
    shell_ = RaceShell{};
    raceTaskOver_ = false;
    for (size_t i = 0; i < kMaxCars; i++) raceOrder_[i] = int8_t(i);
    BindContext();
    // Game mode 6: the ghost session (0x8003C12C -> 0x80012410 at the race load, before the cars).
    const bool mode6 = constants_.gameMode == 6;
    const bool demo = constants_.flag800A951C != 0;
    ghost_ = nullptr;
    ghostDisplayValid_ = false;
    playerFrameNoted_ = false;
    pedalTable_ = shellOptions.pedalTable;
    if (mode6) {
        if (shellOptions.ghost) ghost_ = shellOptions.ghost;
        else {
            ownGhost_ = std::make_shared<GhostSession>();
            ghost_ = ownGhost_.get();
        }
        shellOptions_.ghost = ghost_;
        GhostRaceLoad(*ghost_, demo);
    }

    // Per-car setup (0x80033384 with the native course queries).
    for (size_t car = 0; car < slots.size(); car++) {
        const RaceSlot& slot = slots[car];
        RaceStartInputs in;
        in.setup.params = &params_[car];
        in.setup.bodyToken = 0;
        in.setup.dirtCourse = courseData_.dirtCourse;
        in.setup.constants.dragConstant = constants_.dragConstant;
        in.setup.constants.rollingResistanceGain = constants_.surfaceRolling[0];
        in.setup.constants.springRateRange[0] = constants_.springRateRange[0];
        in.setup.constants.springRateRange[1] = constants_.springRateRange[1];
        in.setup.constants.diffTypeCodes = constants_.diffTypeCodes.data();
        in.setup.constants.gearAutoTable = constants_.gearAutoTable.data();
        in.setup.constants.rollingResistance = physics_.rollingResistance;
        in.setup.constants.aiGripPercent = constants_.aiGripPercent.data();
        in.setup.constants.classTuning = constants_.classTuning.data();
        in.step = constants_.step;
        in.course = &course_;
        in.chunkHint = slot.chunkHint;
        in.x = slot.x;
        in.y = slot.y;
        in.headingSin = slot.headingSin;
        in.headingCos = slot.headingCos;
        in.controlClass = slot.controlClass;
        in.transmission = slot.transmission;
        // 0x80012CD4: the contact type byte of the entry kind (the ghost of mode 6: 2) unless the slot gives one.
        in.contactType = slot.contactType != 0 ? slot.contactType : EntryContactType(slot.entryKind, constants_.gameMode, demo);
        in.byte1C = 0;
        in.carIndex = uint8_t(car);
        in.gridOffset = slot.gridOffset;
        in.courseLength = course_.Extras().courseLength;
        in.pointToPoint = shellOptions_.pointToPoint;
        in.raceMode = constants_.gameMode;
        in.word801C98A0 = constants_.word801C98A0;
        in.byte801D5869 = constants_.byte801D5869;
        in.startLineDistances = courseData_.startLineDistances.data();
        in.startLineCount = int32_t(courseData_.startLineDistances.size());
        for (size_t i = 0; i < 7; i++) {
            in.sectionLists[i].count = int32_t(courseData_.sections[i].size());
            in.sectionLists[i].sections = courseData_.sections[i].empty() ? nullptr : courseData_.sections[i].data();
        }
        in.hasGridList = courseData_.grid.count != 0;
        in.raceStateTable = constants_.raceStateTable.data();
        in.wear = constants_.wear;
        bool anyList = false;
        for (const auto& list : courseData_.sections) anyList |= !list.empty();
        if (!anyList) throw std::runtime_error("race: the course data has no AI section list (0x800367AC needs one)");
        if (mode6 && slot.entryKind == kEntryPlayer1) GhostSetupPlayer(*ghost_, demo);   // 0x80013244
        if (mode6 && slot.entryKind == kEntryGhost) GhostSetupGhostCar(*ghost_);         // 0x80012CD4, kind 2
        StartCar(cars_[car].body, in);
        // 0x8003EF40 inside the car start of the ghost (contact type 2): a ghost with a lap waits at the reference's start.
        // The original's car start ends there; the rest of StartCar writes only body + 0x45C .. + 0x798, which the restore
        // clears and rebuilds, so running it first leaves the same body.
        // In a replay (0x800A951C) player 1 has contact type 2: 0x8003EF40 places it at its ring lap's start, and 0x80012CD4 sets
        // 0x8002F4B8 instead of 0x8003F6B8 (the next frame's 0x8003F990 restarts the lap: GhostReplayLapStart).
        if (mode6 && in.contactType == 2) GhostStartCar(*ghost_, cars_[car].body, courseData_.grid.count != 0, demo);
        if (mode6 && slot.entryKind == kEntryPlayer1 && !demo) GhostSavePlayerStart(*ghost_, cars_[car].body); // 0x8003F6B8
        if (mode6 && slot.entryKind == kEntryPlayer1 && demo) ghost_->replayRestart = 1;                        // 0x8002F4B8
        // 0x80012CD4 around the setup: the car record's pad slot from the entry kind (the shell keeps the results of
        // slots 2 / 3) and, for player 1, the dirtiness from 0x801D58B4 (car + 0x684 = body + 0x658, written after
        // 0x80033384 cleared it).
        cars_[car].padSlot = EntryPadSlot(slot.entryKind);
        if (slot.entryKind == kEntryPlayer1) cars_[car].body.dirtiness = uint32_t(uint64_t(int64_t(slot.dirtLevel) * 600000) >> 12);
    }
    BindContext(); // curve pointers into the bodies are valid now
    // 0x8001523C after the cars: 0x8003C200 = the contact tables (0x8003FE8C), the positions / order table (0x80042680:
    // car i in position i + 1, order = identity - as the setup left them) and the race order (0x80042568).
    InitContactState(contact_);
    UpdateRaceOrder(cars_.data(), int(cars_.size()), raceOrder_.data());
    SetupShell(shellOptions_);
}

int32_t HandicapGridOffset(uint8_t entryKind, int8_t handicap, uint8_t gameMode) { // 0x80012CD4
    int32_t s1 = 0;
    if (entryKind == kEntryPlayer1 && handicap != 0) s1 = handicap > 0 ? handicap * 10 : 1;
    if (entryKind == kEntryPlayer2 && handicap != 0) s1 = handicap < 0 ? -handicap * 10 : 1;
    if (gameMode != 0) s1 = 0;
    return -s1;
}

void InitContactState(CarContactState& state) { // 0x8003FE8C
    for (int buffer = 0; buffer < 2; buffer++)
        for (int car = 0; car < 6; car++)
            for (int slot = 0; slot < 5; slot++) {
                for (int corner = 0; corner < 4; corner++) {
                    ContactCorner& c = state.corners[car][slot][corner];
                    c.edgeFlags[buffer] = 0xF;
                    c.x[buffer] = 0;
                    c.y[buffer] = 0;
                }
                state.fraction[car][slot] = 0x1000;
                state.corner[car][slot] = 0xFF;
                state.side[car][slot] = -1;
            }
    state.buffer = 1;
}

// ================================================================ AI catch-up (arcade modes)

CatchUpTuning CatchUpFromSettings(std::span<const uint8_t> settings, bool enabled) { // 0x80041E4C
    if (settings.size() < 0x1D) throw std::runtime_error("race: the settings block is too short for the catch-up tuning");
    CatchUpTuning t;
    t.chained = settings[0x16]; // 0x801C98B6, read by the frame driver itself
    if (!enabled) return t;
    const uint32_t percent = settings[0x14] == 0 ? 100u : settings[0x14];
    t.slowFrom = int32_t(uint32_t(settings[0x18]) << 16);
    t.slowTo = int32_t(uint32_t(settings[0x19]) * 0xA0000u);
    t.fastFrom = int32_t(uint32_t(settings[0x1B]) << 16);
    t.fastTo = int32_t(uint32_t(settings[0x1C]) * 0xA0000u);
    t.slowGain = int32_t((uint32_t(settings[0x17]) << 12) / 100u);
    t.fastGain = int32_t((uint32_t(settings[0x1A]) << 12) / 100u);
    t.scale = int32_t((percent << 12) / 100u);
    return t;
}

namespace {

int32_t LapsOf(const CarBody& body) { // 0x8004239C
    int32_t lap = body.lap;
    if (body.flags78D & 2) lap--;
    return lap;
}

// (a << 12) / b through the runtime's 64-bit division (0x80086084), low word.
int32_t RampOf(int32_t a, int32_t b) { return int32_t(uint32_t(uint64_t(Div64(int64_t(a) << 12, int64_t(b))))); }

void SpeedUp(CarBody& body, int32_t laps, int32_t distance, const CatchUpTuning& t) { // 0x800420AC
    int32_t v = 0x1000;
    if ((body.flags78D & 0x10) == 0) {
        if (laps > 0 || !(distance < t.fastTo)) v = t.fastGain + 0x1000;
        else if (t.fastFrom < distance) v = (int32_t(uint32_t(RampOf(Sub(distance, t.fastFrom), Sub(t.fastTo, t.fastFrom))) * uint32_t(t.fastGain)) >> 12) + 0x1000;
    }
    body.timeScale = int16_t(v);
}

void SlowDown(CarBody& body, int32_t laps, int32_t distance, const CatchUpTuning& t) { // 0x80042174
    int16_t v;
    if (laps < 1 && distance < t.slowTo) {
        if (t.slowFrom < distance) v = int16_t(0x1000 - int16_t(int32_t(uint32_t(RampOf(Sub(distance, t.slowFrom), Sub(t.slowTo, t.slowFrom))) * uint32_t(t.slowGain)) >> 12));
        else v = 0x1000;
    } else {
        v = int16_t(0x1000 - int16_t(t.slowGain));
    }
    body.timeScale = v;
}

} // namespace

void CatchUpCar(const CarBody& reference, CarBody& body, const CatchUpTuning& t, int32_t courseLength) { // 0x80042230
    if (body.finishFlag != 0) return;
    int32_t laps = LapsOf(reference) - LapsOf(body); // 0x800423BC
    int32_t distance = Sub(reference.courseDistance, body.courseDistance);
    if (laps >= 1 && distance < 0) {
        laps--;
        distance = Add(distance, courseLength);
    } else if (laps < 0 && distance > 0) {
        laps++;
        distance = Sub(distance, courseLength);
    }
    const bool referenceAhead = laps >= 1 || (laps >= 0 && distance > 0);
    if (referenceAhead) {
        if (t.fastGain == 0) {
            body.timeScale = int16_t(t.scale);
            return;
        }
        SpeedUp(body, laps, distance, t);
    } else {
        if (t.slowGain == 0) {
            body.timeScale = int16_t(t.scale);
            return;
        }
        SlowDown(body, Neg(laps), Neg(distance), t);
    }
    body.timeScale = int16_t(int32_t(uint32_t(t.scale) * uint32_t(int32_t(body.timeScale))) >> 12);
}

namespace {

// 0x800423BC(reference, body, &laps, &distance) as CatchUpCar computes it: the lap / course-distance gap, normalised to less than
// a lap of distance.
void CatchUpGap(const CarBody& reference, const CarBody& body, int32_t courseLength, int32_t& laps, int32_t& distance) {
    laps = LapsOf(reference) - LapsOf(body);
    distance = Sub(reference.courseDistance, body.courseDistance);
    if (laps >= 1 && distance < 0) {
        laps--;
        distance = Add(distance, courseLength);
    } else if (laps < 0 && distance > 0) {
        laps++;
        distance = Sub(distance, courseLength);
    }
}

void BattleSpeedUp(CarBody& body, int32_t laps, int32_t distance, const CatchUpTuning& t) { // 0x80041F68
    int32_t v = 0x1000;
    if ((body.flags78D & 0x10) == 0 && laps >= 0) {
        if (laps > 0 || !(distance < t.fastTo)) v = t.fastGain + 0x1000;
        else if (t.fastFrom < distance) v = (int32_t(uint32_t(RampOf(Sub(distance, t.fastFrom), Sub(t.fastTo, t.fastFrom))) * uint32_t(t.fastGain)) >> 12) + 0x1000;
    }
    body.timeScale = int16_t(v);
}

} // namespace

void CatchUpBattle(CarBody& car0, CarBody& car1, const CatchUpTuning& t, int32_t courseLength) { // 0x80042038
    if (t.fastGain == 0) return;
    int32_t laps = 0, distance = 0;
    CatchUpGap(car0, car1, courseLength, laps, distance);
    BattleSpeedUp(car1, laps, distance, t);
    BattleSpeedUp(car0, Neg(laps), Neg(distance), t);
}

bool SlowCarBoostSettings(uint8_t option, std::span<uint8_t> settings) { // 0x8003B73C
    if (settings.size() < 0x1D) throw std::runtime_error("race: the settings block is too short for the catch-up bytes");
    if (option == 1) settings[0x1A] = 15, settings[0x1B] = 20, settings[0x1C] = 100;
    else if (option == 2) settings[0x1A] = 20, settings[0x1B] = 10, settings[0x1C] = 20;
    else {
        settings[0x1A] = settings[0x1B] = settings[0x1C] = 0;
        return false;
    }
    return true;
}

void TyreWearSettings(uint8_t option, std::span<uint8_t> settings) { // 0x8003B69C
    if (settings.size() < 0x24) throw std::runtime_error("race: the settings block is too short for the tyre wear bytes");
    static constexpr uint8_t kRows[3][7] = {{0, 0, 100, 0, 0, 0, 0}, {20, 25, 13, 2, 10, 16, 15}, {10, 25, 13, 1, 10, 8, 15}}; // option 2: + 0x20 = 1 (v1 still holds the 1 of the first compare, 0x8003B6FC)
    const uint8_t* row = kRows[option == 1 ? 1 : option == 2 ? 2 : 0];
    for (size_t i = 0; i < 7; i++) settings[0x1D + i] = row[i];
}

bool RaceLoadSettings(uint8_t gameMode, uint8_t tyreWearOption, uint8_t slowCarBoost, std::span<uint8_t> settings) { // 0x8003C12C
    switch (gameMode) {
    case 0: {
        const bool enabled = SlowCarBoostSettings(slowCarBoost, settings);
        TyreWearSettings(tyreWearOption, settings);
        return enabled;
    }
    case 2: case 4: case 0xC: return true;
    case 1: return false;
    default: TyreWearSettings(0, settings); return false;
    }
}

void ApplyCatchUp(Car* cars, int count, const int8_t* order, const CatchUpTuning& t, int32_t courseLength) { // 0x8003EBF0 (modes 2 / 4 / 0xC)
    if (t.chained == 0) {
        for (int car = 1; car < count; car++) CatchUpCar(cars[0].body, cars[car].body, t, courseLength);
        return;
    }
    const int position = int(int8_t(cars[0].body.racePosition)); // 0x800A9E04
    for (int i = 1; i <= position - 1; i++) CatchUpCar(cars[order[i]].body, cars[order[i - 1]].body, t, courseLength);
    for (int i = position; i < count; i++) CatchUpCar(cars[order[i - 1]].body, cars[order[i]].body, t, courseLength);
}

void RaceSim::SetupShell(const RaceShellOptions& options) {
    ShellGlobals g;
    g.gameMode = constants_.gameMode;
    g.frameStep = uint8_t(constants_.step.rate == 60 ? 1 : 2);
    g.carCount = uint8_t(cars_.size());
    g.carCountShell = uint8_t(cars_.size());
    g.demoFlag = constants_.flag800A951C;
    g.countdownEnabled = constants_.byte801D5869;
    g.rate = constants_.step.rate;
    g.wear = constants_.wear;
    ShellCourse c;
    c.courseLength = course_.Extras().courseLength;
    c.startLines = courseData_.startLineDistances;
    c.startLines.push_back(0); // the word after the table (0 in the dump; see ShellCourse::startLines)
    c.startLineCount = int32_t(courseData_.startLineDistances.size());
    c.grid = courseData_.grid;
    c.ai = aiContext_;
    shell_.Setup(g, c, options, cars_.data(), &contact_);
}

void RaceSim::BindContext() {
    // Curves resolved by offset inside the bodies (the setup wrote the samples there; the tokens are unused).
    for (size_t car = 0; car < cars_.size(); car++) {
        CarBody& body = cars_[car].body;
        uint8_t* raw = reinterpret_cast<uint8_t*>(&body);
        for (size_t axle = 0; axle < 2; axle++) {
            const AxleTyreBlock& block = *reinterpret_cast<const AxleTyreBlock*>(raw + kAxleTyreBlockOffset + axle * sizeof(AxleTyreBlock));
            AxleTyreCurves& a = curves_[car].axles[axle];
            a.slipAngleForce = {block.slipAngleXs, block.slipAngleYs, block.slipAngleCount};
            a.slipRatioForce = {block.slipRatioXs, block.slipRatioYs, block.slipRatioCount};
            a.slipRatioGrip = block.slipRatioGripYs;
            a.loadGrip = {block.loadGripXs, block.loadGripYs, block.loadGripCount};
            a.camberGrip = {block.camberGripXs, block.camberGripYs, block.camberGripCount};
        }
        const EngineBlock& engine = *reinterpret_cast<const EngineBlock*>(raw + kEngineBlockOffset);
        curves_[car].engineTorque = {engine.xs, engine.ys, engine.count};
        curves_[car].steerLimit = {body.steerLimitXs, body.steerLimitYs, body.steerLimitCount};
        InputTuning& t = input_[car];
        t.steerSpringGain = constants_.steerSpringGain;
        t.steerDamping = constants_.steerDamping;
        t.steerCentring = constants_.steerCentring;
        t.steerCurve = {constants_.steerCurveXs.data(), constants_.steerCurveYs.data(), uint32_t(constants_.steerCurveXs.size())};
        // 0x80046DB0 + car * 2 and the five tables that follow it, read like the original (players are cars 0 / 1).
        t.throttleRise = constants_.pedalRates[car];
        t.throttleFall = constants_.pedalRates[2 + car];
        t.brakeRise = constants_.pedalRates[4 + car];
        t.brakeFall = constants_.pedalRates[6 + car];
        t.handbrakeRise = constants_.pedalRates[8 + car];
        t.handbrakeFall = constants_.pedalRates[10 + car];
    }
    const uint16_t holdFrames = shell_.State().hold;
    PhysicsContext& p = physics_;
    p.move.track = track_;
    p.move.globals = constants_.step;
    p.move.collisionDisabled = holdFrames != 0;
    p.move.dirtCourse = courseData_.dirtCourse;
    p.move.controlClass = constants_.shellControlClass;
    p.contact = &contact_;
    p.drivetrain.raceModeByte = constants_.gameMode;
    p.wear = constants_.wear;
    p.curves = curves_.data();
    p.input = input_.data();
    p.rollingResistance = {constants_.rollingXs.data(), constants_.rollingYs.data(), uint32_t(constants_.rollingXs.size())};
    p.surfaceRolling = constants_.surfaceRolling.data();
    for (size_t i = 0; i < kAiAidEntries; i++) {
        const DriveClassTuning& record = constants_.classTuning[i];
        p.aiAids[i].yawBrakeGain = record.yawBrakeGain;
        p.aiAids[i].yawBrakeThreshold = record.yawBrakeThreshold;
        p.aiAids[i].steerFalloffGain = record.steerFalloffGain;
        p.aiAids[i].steerFalloffThreshold = record.steerFalloffThreshold;
        p.aiAids[i].tractionGain = record.tractionGain;
    }
    p.slideSensitivity = constants_.slideSensitivity;
    p.gameMode = constants_.gameMode;
    p.holdFrames = holdFrames;
    p.scriptedHold = 0;
    // The AI driver's view of the course data and the constants (ai_driver.h): the seven line lists of the race
    // object, the course length, the dirt flag, the race-state table, the class tuning table and the wear constants.
    for (size_t i = 0; i < 7; i++) {
        aiContext_.course.lines[i].count = int32_t(courseData_.sections[i].size());
        aiContext_.course.lines[i].sections = courseData_.sections[i].empty() ? nullptr : courseData_.sections[i].data();
    }
    aiContext_.course.courseLength = course_.Extras().courseLength;
    aiContext_.course.dirtCourse = courseData_.dirtCourse;
    aiContext_.course.raceStateTable = constants_.raceStateTable.data();
    aiContext_.classTuning = constants_.classTuning.data();
    aiContext_.wear = constants_.wear;
    aiContext_.raceClock = shell_.State().raceClock;
    aiContext_.rate = constants_.step.rate;
    p.user = this;
    p.aiInput = &RaceSim::AiInputHook;
    p.soundEvent = nullptr; // no sound yet

    GroundGlobals& g = groundGlobals_;
    g.rate = constants_.step.rate;
    g.viewMode = constants_.viewMode;
    g.raceMode = constants_.gameMode;
    g.collisionDisabled = holdFrames != 0;
    g.dirtCourse = courseData_.dirtCourse;
    g.flag800A951C = constants_.flag800A951C;
    g.flag801C9995 = constants_.flag801C9995;
    g.flag800AF232 = constants_.flag800AF232;
    g.wheelEffectsTail = constants_.wheelEffectsTail;
    g.courseLength = course_.Extras().courseLength;
    g.grid = courseData_.grid;
}

void RaceSim::AiInputHook(void* user, CarBody& body, int car, const AiDispatch& dispatch) {
    RaceSim& self = *static_cast<RaceSim*>(user);
    // The original's steering rate limit reads the scratchpad's word 0, which the physics core set to this
    // body's step time (body + 0x6FE) right before the input routine.
    RunAiDriver(self.aiContext_, body, self.requests_[size_t(car)], dispatch, body.stepTime);
}

void RaceSim::LoadState(std::span<const Car> cars, const CarContactState& contact) {
    if (cars.size() != cars_.size()) throw std::runtime_error("race: state car count differs from the setup");
    for (size_t i = 0; i < cars.size(); i++) cars_[i] = cars[i];
    contact_ = contact;
    // The shell of the dumped race: the cars are released and its clock runs.
    SetupShell(shellOptions_);
    shell_.State().hold = 0;
    shell_.State().raceClock = constants_.word80046F64;
    BindContext();
}

void RaceSim::Step(const PadRecord* pads, const uint32_t* buttons) {
    shell_.BeginFrame();                                                                  // 0x80015B64: hold counter (fields)
    ReplayLapStartIfPending();                                                            // 0x80015B64: 0x8002F4B8 -> 0x8003F990
    const uint16_t holdFrames = shell_.State().hold;
    physics_.holdFrames = holdFrames;
    physics_.move.collisionDisabled = holdFrames != 0;
    groundGlobals_.collisionDisabled = holdFrames != 0;
    aiContext_.raceClock = shell_.State().raceClock;
    const int count = int(cars_.size());
    PadRecord padCopy[kMaxCars] = {};
    const bool ghost = HasGhost();
    if (padSource_ || ghost) { // 0x8003C250 per car inside the physics core (see SetPadSource)
        for (int car = 0; car < count && pads; car++) padCopy[car] = pads[car];
        playerFrameNoted_ = false;
        if (padSource_) padSource_(padSourceUser_, *this, padCopy);
        pads = padCopy;
    }
    if (ghost) {
        // Game mode 6: the ghost's hold (0x8003FAEC, taken in the physics core's first pass, before the pads are read), then
        // 0x8003C250 per car: player 1's frame into the current lap stream (0x80013EF0), the ghost's frame (pad slot 1).
        ShellContext& ctx = shell_.Context();
        physics_.scriptedHold = GhostHold(ctx);
        for (int car = 0; car < count; car++) {
            if (cars_[car].padSlot == 2 && constants_.flag800A951C == 0) {
                ReplayFrame frame = FrameOfPad(LogicalPad{});
                if (playerFrameNoted_) frame = {playerFrame_[0], playerFrame_[1], playerFrame_[2], playerFrame_[3], playerFrame_[4], playerWheelFine_};
                GhostPlayerInput(ctx, frame);
            } else if (cars_[car].padSlot == 2) { // a mode 6 replay: player 1 plays the ring's laps (0x80013EF0)
                ReplayFrame frame;
                padCopy[car] = GhostReplayInput(ctx, frame) ? PadOfFrame(frame, std::span<const uint16_t, 16>(pedalTable_)) : PadRecord{};
            } else if (cars_[car].padSlot == 1) {
                ReplayFrame frame;
                padCopy[car] = GhostCarInput(ctx, frame) ? PadOfFrame(frame, std::span<const uint16_t, 16>(pedalTable_)) : PadRecord{};
            }
        }
    }

    PhysicsCore(physics_, cars_.data(), count, pads, work_, requests_.data(), deltas_);      // 0x8003E0C4
    if (holdFrames == 0) SimulateCars(physics_, cars_.data(), count, deltas_);            // 0x80034480
    PrepareContactQueries(cars_.data(), count, scratch_.data());                                 // 0x80043388
    QueryContacts(course_.Surface(), count, scratch_.data());                                    // 0x800434DC
    ApplyContacts(cars_.data(), count, scratch_.data(), constants_.ground, groundGlobals_);      // 0x80043578
    GroundPass(cars_.data(), count, scratch_.data(), constants_.ground, groundGlobals_);         // 0x8003E8E4
    shell_.ProgressCars(raceOrder_.data(), count, course_);                              // 0x8003CF94 (course distance, laps, sections)
    shell_.AdvanceClock();                                                                // 0x8003D168
    shell_.LicenseChecks(course_);                                                        // 0x8003D5F8 per car (mode 3 only)
    // 0x800133F0 (render transforms: see Pose), 0x80042230 (AI time scale): see the header.
    const bool ghostPose = ghost && count == 2;
    if (ghostPose) { // 0x8003F2F0(1): car 1 is drawn (and ordered) at its blended pose
        GhostBlendPose(shell_.Context(), 1);
        ghostDisplay_ = cars_[1];
        ghostDisplayValid_ = true;
    }
    UpdateRaceOrder(cars_.data(), count, raceOrder_.data());                             // 0x80042568
    if (ghostPose) GhostRestorePose(shell_.Context(), 1);                                // 0x8003F548(1)
    const uint8_t mode = constants_.gameMode;
    if (mode == 2 || mode == 4 || mode == 0xC)                                            // the AI catch-up of the arcade modes
        ApplyCatchUp(cars_.data(), count, raceOrder_.data(), constants_.catchUp, course_.Extras().courseLength);
    else if (mode == 0 && count >= 2)                                                     // 0x80042038: the 2 player Battle's Slow Car Boost
        CatchUpBattle(cars_[0].body, cars_[1].body, constants_.catchUp, course_.Extras().courseLength);
    const uint32_t none[2] = {0, 0};
    if (!shell_.EndFrame(buttons ? buttons : none)) raceTaskOver_ = true;                // 0x8002E550 (after the tick in 0x80015B64)
}

void UpdateRaceOrder(Car* cars, int count, int8_t* raceOrder) { // 0x80042568 with the comparison 0x80042490 / 0x8004239C
    auto laps = [&](const CarBody& body) { // 0x8004239C
        int32_t lap = body.lap;
        if (body.flags78D & 2) lap--;
        return lap;
    };
    auto compare = [&](int a, int b) { // 0x80042490(cars, a, b): 1 if a is ahead of b, -1 if behind, 0 otherwise
        const CarBody& ba = cars[a].body;
        const CarBody& bb = cars[b].body;
        if (bb.finishFlag != 2) {
            if (ba.finishFlag == 2) return 0;
            const int32_t la = laps(ba), lb = laps(bb);
            if (lb < la) return 1;
            if (la < lb) return -1;
            if (bb.courseDistance < ba.courseDistance) return 1;
            if (ba.courseDistance < bb.courseDistance) return -1;
        }
        return 0;
    };
    for (int i = 1; i < count; i++) { // insertion sort of the order table
        const int8_t car = raceOrder[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            if (compare(raceOrder[j], car) >= 0) break;
            raceOrder[j + 1] = raceOrder[j];
        }
        raceOrder[j + 1] = car;
    }
    for (int i = 0; i < count; i++) {
        CarBody& body = cars[raceOrder[i]].body;
        body.racePosition = uint8_t(i + 1);
        if (body.finishFlag == 1) body.finishFlag = 2;
    }
}

CarPose PhysicsPoseOf(const CarBody& body) {
    CarPose pose = ConvertPose(body.position, body.basis);
    ApplyReferenceOffset(pose, body.cgOffset);
    return pose;
}

CarPose VisualPoseOf(const CarBody& body) {
    // 0x800133F0 adds the offset computed with the PHYSICS matrix (car + 0x81C) to both positions.
    const CarPose physics = ConvertPose(body.position, body.basis);
    CarPose pose = ConvertPose(body.visualPosition, body.visualBasis);
    const int32_t cgOffset = body.cgOffset;
    for (uint32_t i = 0; i < 3; i++) pose.worldPosition[i] = Add(pose.worldPosition[i], (int32_t(physics.rotation[i][2]) * cgOffset) >> 8);
    return pose;
}

CarPose RaceSim::Pose(size_t car) const {
    if (car == 1 && ghostDisplayValid_ && HasGhost()) return PhysicsPoseOf(ghostDisplay_.body); // the ghost's blended pose (0x8003F2F0)
    return PhysicsPoseOf(cars_[car].body);
}

void RaceSim::NotePlayerFrame(const ReplayFrame& frame) {
    playerFrame_ = {frame.flags, frame.buttons, frame.steer, frame.throttle, frame.brake};
    playerWheelFine_ = frame.wheelFine;
    playerFrameNoted_ = true;
}

void RaceSim::ReplayLapStartIfPending() {
    if (!HasGhost() || ghost_->replayRestart == 0) return;
    ghost_->replayRestart = 0;
    GhostReplayLapStart(shell_.Context(), cars_[0].body);
}

void RaceSim::EndGhostRace() {
    if (HasGhost()) GhostRaceEnd(shell_.Context());
}

CarPose RaceSim::VisualPose(size_t car) const {
    if (car == 1 && ghostDisplayValid_ && HasGhost()) return VisualPoseOf(ghostDisplay_.body);
    return VisualPoseOf(cars_[car].body);
}

std::array<WheelVisual, 4> RaceSim::Wheels(size_t car) const {
    const CarBody& body = cars_[car].body;
    std::array<WheelVisual, 4> out{};
    for (size_t w = 0; w < 4; w++) {
        const Wheel& wheel = body.wheels[w];
        out[w].steerAngle = wheel.steerAngle;
        out[w].rotation = wheel.rotation;
        out[w].verticalOffset = int16_t(AxleSuspensionOf(body, w >> 1).rideReference - wheel.travel);
    }
    return out;
}

CarTelemetry RaceSim::Telemetry(size_t car) const {
    const CarBody& body = cars_[car].body;
    CarTelemetry t;
    t.forwardSpeed = body.forwardSpeed;
    t.rpm = body.engineRpm;
    t.gear = body.gear;
    t.throttle = body.throttle;
    t.brake = body.brake;
    t.handbrake = body.handbrake;
    t.steerAngle = body.steerAngle;
    t.heading = body.heading;
    t.courseDistance = body.courseDistance;
    t.racePosition = body.racePosition;
    t.wallHitMask = body.wallHitMask;
    return t;
}

std::vector<uint8_t> RaceSim::Snapshot() const {
    const RaceShellState& shell = shell_.State();
    std::vector<uint8_t> bytes(cars_.size() * sizeof(Car) + sizeof(contact_) + sizeof(shell) + raceOrder_.size());
    uint8_t* out = bytes.data();
    std::memcpy(out, cars_.data(), cars_.size() * sizeof(Car));
    out += cars_.size() * sizeof(Car);
    std::memcpy(out, &contact_, sizeof(contact_));
    out += sizeof(contact_);
    std::memcpy(out, &shell, sizeof(shell));
    out += sizeof(shell);
    std::memcpy(out, raceOrder_.data(), raceOrder_.size());
    return bytes;
}

} // namespace gt2::sim
