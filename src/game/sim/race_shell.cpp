#include "game/sim/race_shell.h"

#include <cstring>
#include <stdexcept>

#include "game/sim/car_setup.h"
#include "game/sim/drive_shafts.h"
#include "game/sim/drivetrain.h"
#include "game/sim/fixed.h"
#include "game/sim/physics_core.h"
#include "gt2formats/replay.h"

// The car record and body fields are the named members of sim::Car / CarBody (car_body.h); the HUD request
// fields live in the record at car + 0xA8C.. and are CarBody::hud* here.
namespace gt2::sim {

namespace {

int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t Mul(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }
[[maybe_unused]] uint32_t DivU(uint32_t n, uint32_t d) { return d == 0 ? 0xFFFFFFFFu : n / d; } // MIPS divu: LO = -1 for a zero divisor

Car& RecordOf(const ShellContext& ctx, int car) { return ctx.cars[car]; }
CarBody& BodyOf(const ShellContext& ctx, int car) { return ctx.cars[car].body; }
int32_t MsPerFrame(const ShellGlobals& g) { return g.frameStep == 1 ? 50 : 100; } // 1/1000 s per frame, as 0x8003C70C picks it
PlayerResults* ResultsOfRecord(ShellContext& ctx, const Car& record) {
    const int16_t slot = record.padSlot;
    if (slot == 2) return &ctx.state->results[0];
    if (slot == 3) return &ctx.state->results[1];
    return nullptr;
}
int32_t GridDistance(const ShellCourse& course, int32_t slot) {
    if (slot < 0 || slot >= int32_t(course.grid.distance.size())) throw std::runtime_error("race shell: grid slot outside the list");
    return course.grid.distance[size_t(slot)];
}

// 0x8005DD68: clears the pending lap entry.
void ClearPending(LapEntry& e) {
    e.time = -1;
    e.split[0] = e.split[1] = e.split[2] = -1;
    e.maxSpeed = 0;
    e.pad = -1;
}

} // namespace

// ================================================================ init / start / per-frame globals

void InitRaceState(ShellContext& ctx) { // 0x8003C3F4
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    s.raceClock = 0;
    for (uint32_t row = 0; row < 4; row++) {
        for (uint32_t i = 0; i < 6; i++) s.board.times[row][i] = kNoTime;
        s.board.bestLap[row] = -1;
        s.board.count[row] = 0;
        s.board.shown[row] = 0;
    }
    s.twoPlayerLaps = g.gameMode == 6 ? 1 : 0;
    if (g.gameMode == 2 || g.gameMode == 4 || g.gameMode == 0xC) s.timeLimited = g.lapCount == 'c' ? 1 : 0;
    else s.timeLimited = 0;
}

void StartRace(ShellContext& ctx, uint16_t introFields) { // 0x8001584C (hold part), 0x800299D8, 0x8002E390
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    s.holdInitial = g.gameMode == 3 ? 0x78 : 0xB4;
    if (g.countdownEnabled == 0) s.holdInitial = 0;
    s.sinceFinish = 0;
    s.hold = s.holdInitial;
    if (s.hold != 0 && s.hold < introFields) { s.holdInitial = introFields; s.hold = introFields; } // 0x800299D8 (a race with music)
    s.startTimer = int16_t(s.hold + 240);                                                            // 0x8002A0C8
    s.endTimer = -1;                                                                                  // 0x8002A6D0
    s.endTimerAux = -1;                                                                               // 0x8002A398
    if (ctx.ghost) ctx.ghost->replayEnded = false;                                                   // 0x80015AB0: 0x800A8D68 = 0
}

void BeginFrame(ShellContext& ctx) { // 0x80015B64
    RaceShellState& s = *ctx.state;
    const uint32_t step = ctx.globals->frameStep;
    s.hold = uint16_t(s.hold > step ? s.hold - step : 0);
    if (s.sinceFinish != 0) s.sinceFinish = uint16_t(s.sinceFinish + step);
}

void AdvanceClock(ShellContext& ctx) { // 0x8003D168
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    if (s.hold != 0) return;
    if (g.gameMode == 6 && BodyOf(ctx, 0).lap == 0) return; // 0x800A9CBC = car 0's lap counter
    s.raceClock += g.frameStep == 1 ? 0x32 : 100;
}

void RequestMessage(CarBody& body, uint8_t code, int32_t frames) { // 0x80030308
    if (frames != 0 || body.messageFrames == 0) {
        body.messageCode = code;
        body.messageFrames = uint8_t(frames);
    }
}

bool TimeLimitReached(const ShellContext& ctx) { // 0x8003D138
    return ctx.state->timeLimited != 0 && ctx.state->raceClock > 0x01499700u; // 2 hours
}

bool HasFinished(const ShellContext& ctx, const CarBody& body) { // 0x80035714
    const ShellGlobals& g = *ctx.globals;
    if (g.lapCount == 0 || (body.flags78D & 2) != 0) return false;
    if (TimeLimitReached(ctx)) return true;
    int32_t laps = g.lapCount;
    if (uint32_t(body.courseDistance) < (uint32_t(ctx.course->courseLength) >> 1)) laps++;
    return laps <= body.lap;
}

// ================================================================ race state / section machine

void MarkContactCorners(CarContactState& contact, uint32_t car, uint32_t carCount) { // 0x8003FFDC
    constexpr uint32_t kTableCars = 6; // the contact tables hold six cars
    if (car >= kTableCars) throw std::runtime_error("race shell: car index outside the contact tables");
    for (uint32_t parity = 0; parity < 2; parity++) {
        for (uint32_t slot = 0; slot + 1 < carCount; slot++)
            for (uint32_t corner = 0; corner < 4; corner++) contact.corners[car][slot][corner].edgeFlags[parity] = 0xF;
        for (uint32_t other = 0; other < carCount; other++) {
            if (other == car || other >= kTableCars) continue;
            const uint32_t slot = car - (other <= car ? 1 : 0);
            for (uint32_t corner = 0; corner < 4; corner++) contact.corners[other][slot][corner].edgeFlags[parity] = 0xF;
        }
    }
}

int32_t SetRaceState(ShellContext& ctx, CarBody& body, uint32_t code) { // 0x80036980
    const AiContext& ai = ctx.course->ai;
    // 0x80036340 (through 0x800367AC or directly) walks the section list of the line; a line without a list (a -1
    // from 0x800358E0, or a car on a line the course does not have, e.g. anything but line 6 on the licence
    // course) makes the original read through a null list pointer (0xFFFFFFDC: a bus error on the console).
    auto requireList = [&](int32_t line) {
        if (line < 0 || line > 6 || ai.course.lines[line].sections == nullptr || ai.course.lines[line].count <= 0)
            throw std::runtime_error("race shell: race state change onto a line without a section list");
    };
    auto switchTo = [&](int32_t line) { requireList(line); SwitchLine(ai, body, uint8_t(line)); };
    auto lineIfListed = [&](uint32_t line, uint32_t current) { // 0x80036910 (line 3) / 0x80036948 (line 4)
        if (ai.course.lines[line].count == 0 || current == line) return int32_t(-1);
        return int32_t(line);
    };
    if (code == 0) body.aiRecovery = 0;
    if (code == 0 || code == 1 || code >= 8) { // 0x80036A50
        const uint32_t state = body.raceState;
        if (state == 2 || state == 4 || state == 3) switchTo(PickLine(ai.course, 6));
        else if (int8_t(body.controlClass) == 0 && state == 0) {
            requireList(body.aiLine);
            InitRaceProgress(body, body.aiLine, ai.course.lines, ai.classTuning, ai.wear); // 0x80036340 on the current line
        }
        body.raceState = uint8_t(code);
        return 1;
    }
    if (code == 2 || code == 4) { // 0x800369CC / 0x800369DC
        if (code == 2) body.flags78D = uint8_t(body.flags78D | 8);
        const uint32_t line = body.aiLine;
        if (line != 3) {
            const int32_t target = lineIfListed(3, line);
            if (target == -1) return 0;
            switchTo(target);
        }
        body.raceState = uint8_t(code);
        return 1;
    }
    if (code == 3) { // 0x80036A14
        const int32_t target = lineIfListed(4, body.aiLine);
        if (target == -1) return 0;
        switchTo(target);
        MarkContactCorners(*ctx.contact, body.carIndex, ctx.globals->carCount);
        body.raceState = uint8_t(code);
        return 1;
    }
    if (code == 5) switchTo(PickLine(ai.course, 6)); // 0x80036A70
    body.raceState = uint8_t(code);                  // 5, 6, 7
    return 1;
}

void Finish(ShellContext& ctx, CarBody& body) { // 0x80036ACC
    if (body.finishFlag == 0) {
        const uint32_t state = body.raceState;
        body.finishFlag = 1;
        if (state - 2 < 3) return; // on the grid / pit list: the section machine finishes the car later
    }
    uint32_t code;
    const uint32_t mode = ctx.globals->gameMode;
    if (ctx.course->pointToPoint) code = 5;
    else if (mode < 9) code = mode < 7 ? (mode == 3 ? 5 : 1) : 5;
    else code = mode == 11 ? 5 : 1;
    SetRaceState(ctx, body, code);
}

void SectionState(ShellContext& ctx, CarBody& body, int32_t current, int32_t previous) { // 0x80036CA4
    const ShellCourse& course = *ctx.course;
    const ShellGlobals& g = *ctx.globals;
    if (course.grid.count == 0) return;
    const int32_t length = course.courseLength;
    const int32_t count = course.grid.count;
    const int32_t half = length / 2;
    if (current < half) current = Add(current, length);
    if (previous < half) previous = Add(previous, length);
    if (body.penaltyFrames != 0) RequestMessage(body, 11, 0);
    const uint32_t flags = body.flags78D;
    if (flags & 1) { // driving the wrong way
        if (int8_t(body.controlClass) == 2 || body.raceState != 0 || (flags & 0x10) == 0) return;
        if (current <= GridDistance(course, 3)) return;
        if (GridDistance(course, count - 2) <= current) return;
        const int32_t line = GridDistance(course, count - 3);
        if (line < previous && current <= line) {
            SetRaceState(ctx, body, 4);
            RequestMessage(body, 12, g.rate << 1);
            return;
        }
        const int32_t forward = body.forwardSpeed, lateral = body.lateralSpeed;
        if (uint32_t(Add(forward, 0x163A)) < 0x2C75u && lateral < 0x163B && lateral > -0x163B) return;
        if (body.contactFlags != 0) return;
        RequestMessage(body, 2, 0);
        return;
    }
    uint32_t tyres = 0; // 1: worn / cold tyres, 2: damaged
    for (uint32_t w = 0; w < 4; w++) {
        const Wheel& wheel = body.wheels[w];
        if (wheel.wear != g.wear.coldLimit) tyres |= 1;
        if (wheel.damage != 0) tyres |= 2;
    }
    const bool finished = HasFinished(ctx, body);
    const uint32_t state = body.raceState;
    if (state == 3) { // in the pit box
        if (body.penaltyFrames != 0) {
            if (body.penaltyFrames != uint32_t(g.rate)) return;
            ResetTyreWear(body, g.wear); // 0x80032A1C
            return;
        }
        const int32_t box = GridDistance(course, body.gridSlot);
        if (tyres != 0 && previous < box && box <= current) {
            ResetDynamicState(body, g.rate); // 0x80032E6C
            body.penaltyFrames = uint16_t(g.rate * 5);
            RequestMessage(body, 11, 0);
            return;
        }
        if (current < Add(box, 0x8000)) return;
        SetRaceState(ctx, body, 4);
        body.flags78D = uint8_t(body.flags78D & 0xFB);
        return;
    }
    if (state == 2) { // on the pit lane
        if (finished) { SetRaceState(ctx, body, 4); RequestMessage(body, 8, g.rate << 1); return; }
        if (tyres == 0) { SetRaceState(ctx, body, 4); RequestMessage(body, 9, g.rate << 1); return; }
        const int32_t box = Sub(GridDistance(course, body.gridSlot), 0x190000);
        if (box <= previous || current < box) {
            RequestMessage(body, 10, 0);
            if (int8_t(body.controlClass) != 0 || (body.flags78D & 0x10) != 0) return;
            SetRaceState(ctx, body, 0);
            RequestMessage(body, 0, 0);
            return;
        }
        if (0x1BC87 < body.forwardSpeed) { SetRaceState(ctx, body, 4); RequestMessage(body, 7, g.rate << 1); return; }
        SetRaceState(ctx, body, 3);
        return;
    }
    if (state == 4) { // leaving the pit / grid list
        int32_t exitLine, entryLine;
        bool nearEnd = true;
        if (int8_t(body.controlClass) == 0) {
            if ((body.flags78D & 0x10) == 0 && current < length) {
                SetRaceState(ctx, body, 0);
                RequestMessage(body, 0, 0);
            }
            if (int8_t(body.controlClass) == 0 && body.finishFlag == 0) nearEnd = false;
        }
        if (nearEnd) { exitLine = GridDistance(course, count - 1); entryLine = GridDistance(course, count - 2); }
        else { exitLine = GridDistance(course, count - 2); entryLine = GridDistance(course, count - 3); }
        if (previous < exitLine && exitLine <= current) {
            if (body.finishFlag == 0) {
                SetRaceState(ctx, body, 0);
                if (!finished) RequestMessage(body, 6, g.rate << 1);
            } else {
                Finish(ctx, body);
            }
            body.flags78D = uint8_t(body.flags78D & 0xF7);
            return;
        }
        uint32_t code;
        if (current < entryLine) {
            if (body.finishFlag != 0) return;
            code = 10;
        } else {
            if (finished) return;
            code = body.inputFlag6FD == 0 ? 4 : 5;
        }
        RequestMessage(body, uint8_t(code), 0);
        return;
    }
    // states 0, 1, 5, 6, 7: entering the pit lane / the formation line
    int32_t line;
    if (int8_t(body.controlClass) == 0) {
        if ((body.flags78D & 0x10) == 0) return;
        line = GridDistance(course, 1);
    } else {
        if ((body.flags78D & 0x10) != 0 && body.wallHitMask != 0 && current < length) {
            SetRaceState(ctx, body, 2);
            body.aiRecovery = 4;
            return;
        }
        if ((body.flags78D & 8) == 0 || finished) return;
        line = GridDistance(course, 0);
    }
    if (line <= previous || current < line) return;
    SetRaceState(ctx, body, 2);
}

// ================================================================ boards / results / HUD requests

void RecordSector(SectorBoard& board, int32_t lapIndex, int32_t sector, int32_t time, int32_t car) { // 0x8003C520
    if (sector < 0 || sector >= 4) throw std::runtime_error("race shell: sector line outside the board");
    const size_t row = size_t(sector);
    const int32_t best = board.bestLap[row];
    if (best < lapIndex) { // a newer lap: restart the row
        board.bestLap[row] = int16_t(lapIndex);
        board.count[row] = 1;
        board.shown[row] = 0;
        board.times[row][0] = time;
        board.car[row][0] = int8_t(car);
        return;
    }
    if (lapIndex != best) return; // 0x800156B0: an older lap (empty routine)
    // Every car records every line once per lap, so a row never gets a 7th entry; the original would write it
    // past the board into the contact tables (0x801C8608), which the port's layout cannot reproduce.
    if (board.count[row] >= 6) throw std::runtime_error("race shell: a sector row already holds six entries");
    const int32_t count = board.count[row], shown = board.shown[row];
    int32_t at = count;
    if (count != shown) {
        if (shown < count) { // insertion position among the entries not displayed yet
            int32_t k = count;
            while (true) {
                const int32_t below = k - 1;
                if (!(uint32_t(time) < uint32_t(board.times[row][below]))) break;
                at = below;
                if (!(shown < at)) break;
                k = at;
            }
        }
        for (int32_t i = board.count[row]; at < i; i--) board.times[row][i] = board.times[row][i - 1];
    }
    board.times[row][at] = time;
    board.car[row][at] = int8_t(car);
    board.count[row] = int8_t(board.count[row] + 1);
}

void InitPlayerResults(PlayerResults& r) { // 0x8005E2FC
    r.position = 0;
    r.lapNumber = 0;
    r.count = 0;
    r.bestLapNumber = -1;
    ClearPending(r.best);
    ClearPending(r.pending);
    for (LapEntry& e : r.laps) ClearPending(e);
    r.finishTime = -1;
}

void RecordLap(PlayerResults& r, int32_t lap, int32_t lapTime, int32_t maxSpeed, bool invalid) { // 0x8005E3C4
    if (invalid) lapTime = -1;
    if (r.lapNumber == lap) return;
    r.pending.time = lapTime;
    r.pending.maxSpeed = int16_t(maxSpeed);
    r.count = int16_t(r.count + 1);
    if (r.count >= 11) {
        std::memmove(&r.laps[0], &r.laps[1], sizeof(LapEntry) * 9);
        r.count = 10;
    }
    r.laps[r.count - 1] = r.pending;
    bool better = false;
    if (r.pending.time != -1) better = uint32_t(r.pending.time) < uint32_t(r.best.time);
    if (better) {
        r.bestLapNumber = r.lapNumber;
        r.best = r.pending;
    }
    r.lapNumber = int16_t(r.lapNumber + 1);
    if (r.lapNumber >= 1000) r.lapNumber = 999;
    ClearPending(r.pending);
}

void FinishJingle(ShellContext& ctx) { // 0x80029C84
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    if (s.sinceFinish != 0) return;
    s.sinceFinish = g.frameStep;
    uint8_t jingle = 0xFF;
    if (g.demoFlag == 0) {
        const uint32_t m = s.music2F1;
        jingle = m == 3 ? 13 : m == 1 ? 16 : 14;
        if (g.gameMode != 0 && g.gameMode != 3) s.music2F0 = 8;
    }
    if (jingle == 0xFF) return; // no jingle in the attract race
    s.musicRequest = jingle;    // 0x80029C54(object, jingle, 0)
    s.musicRequestFlag = 0;
}

void OnLapLine(ShellContext& ctx, int car, int32_t lap, int32_t lapTime, int32_t elapsed, uint16_t maxSpeed, uint8_t invalid) { // 0x8001555C -> 0x80013824
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    Car& record = RecordOf(ctx, car);
    CarBody& body = record.body;
    body.hudInvalid = int8_t(invalid != 0 ? 1 : 0);
    const uint32_t mode = g.gameMode;
    bool finish = false;
    if (mode == 6 || mode == 1 || mode == 10) {
        if (lap >= 999) return;
        finish = ctx.course->pointToPointById;
    } else {
        int32_t laps = g.lapCount;
        if (laps == 0) laps = 100;
        if (laps < lap) return;
        if (lap == laps || TimeLimitReached(ctx)) finish = true;
        if (finish) {
            Finish(ctx, body);
            record.finishTime = elapsed;
        }
    }
    const int16_t slot = record.padSlot;
    PlayerResults* results = ResultsOfRecord(ctx, record);
    if (results != nullptr) {
        if (g.demoFlag == 0) {
            RecordLap(*results, lap, lapTime, maxSpeed, body.hudInvalid != 0);
            if (mode == 6) {
                body.hudCompare = s.courseRecord.time;
                body.hudTimer2 = 120;
                body.hudGap = lapTime;
            }
            if (body.hudInvalid == 0) {
                const int32_t best = s.courseRecord.time;
                if ((best == -1 || uint32_t(lapTime) < uint32_t(best)) && mode == 6) {
                    // The original copies the 20 bytes at record + 8 + (count - 1) * 20. With an empty record (only
                    // when RecordLap returned early on lapNumber == lap before any lap was kept) that is the memory
                    // before the record (0x801D5E7C: the points tables), which the port does not lay out; not a
                    // state a race produces (the first lap line comes with lap 1 and an empty record).
                    if (results->count < 1) throw std::runtime_error("race shell: course record copy from an empty results record");
                    s.courseRecord = results->laps[results->count - 1];
                    s.newRecord = 1;
                    if (ctx.courseRecordTarget) *ctx.courseRecordTarget = s.courseRecord; // through *(0x800A9524): the career's record
                }
            }
            if (finish) {
                const uint8_t position = body.racePosition;
                s.finishPosition = position;
                results->finishTime = elapsed;
                results->position = position;
                s.endTimer = 0; // 0x8002A6F8: the race-end sequence starts
                if (mode == 2 || mode == 11) {
                    int16_t byPosition[6] = {-1, -1, -1, -1, -1, -1};
                    s.pointsRace.fill(0);
                    for (int i = 0; i < int(g.carCountShell); i++) {
                        const uint32_t p = uint32_t(int32_t(int8_t(BodyOf(ctx, i).racePosition)) - 1);
                        if (p < 6) byPosition[p] = int16_t(i);
                    }
                    for (int i = 0; i < 6; i++)
                        if (byPosition[i] >= 0) s.pointsRace[size_t(byPosition[i])] = g.pointsByPosition[size_t(i)];
                    for (size_t i = 0; i < 6; i++) s.pointsTotal[i] = uint8_t(s.pointsTotal[i] + s.pointsRace[i]); // 0x8005E67C
                }
            }
        } else {
            body.hudCompare = s.courseRecord.time; // -1 = no record
        }
    }
    body.hudTimer = 120;
    body.hudValue = lapTime;
    body.hudLabel = g.lapTimeLabel; // kLapTimeLabel of the disc's build
    if (finish && (slot == 2 || slot == 3)) FinishJingle(ctx); // 0x80029C84(*(0x8002F4F4))
    if (ctx.hooks.lapLine) ctx.hooks.lapLine(ctx.hooks.user, car, lap, lapTime, elapsed);
    if (finish && ctx.hooks.finished) ctx.hooks.finished(ctx.hooks.user, car, body.racePosition, elapsed);
}

void OnSplit(ShellContext& ctx, int car, int32_t sector, int32_t splitTime, uint8_t invalid) { // 0x80015510 -> 0x8001374C
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    Car& record = RecordOf(ctx, car);
    CarBody& hud = record.body;
    hud.hudInvalid = int8_t(invalid != 0 ? 1 : 0);
    // The split index can exceed the sector lines in licence tests (0x8003D314 / 0x8003D3C0 number the lines of all
    // laps): the original stores word `sector` after the pending entry's time (index 3 = the max-speed / pad halfwords)
    // and reads caption `sector` of the 4-entry table; larger indices reach memory the port does not lay out.
    if (sector < 0 || sector >= 4) throw std::runtime_error("race shell: split index outside the lap entry / caption table");
    PlayerResults* results = ResultsOfRecord(ctx, record);
    if (results != nullptr && g.demoFlag == 0) {
        if (sector < 3) {
            results->pending.split[sector] = splitTime;
        } else {
            results->pending.maxSpeed = int16_t(uint32_t(splitTime) & 0xFFFF);
            results->pending.pad = int16_t(uint32_t(splitTime) >> 16);
        }
    }
    hud.hudTimer = 120;
    hud.hudValue = splitTime;
    hud.hudLabel = g.splitLabels[size_t(sector)];
    if (g.gameMode == 6) {
        hud.hudCompare = -1;
        if (s.courseRecord.time != -1) {
            const int32_t word = sector < 3 ? s.courseRecord.split[sector] : int32_t(uint32_t(uint16_t(s.courseRecord.maxSpeed)) | (uint32_t(uint16_t(s.courseRecord.pad)) << 16));
            hud.hudCompare = word;
        }
        hud.hudTimer2 = 120;
        hud.hudGap = splitTime;
    }
    if (ctx.hooks.split) ctx.hooks.split(ctx.hooks.user, car, sector, splitTime);
}

void ShowGap(ShellContext& ctx, int car, int32_t gap) { // 0x800155C4
    const uint32_t mode = ctx.globals->gameMode;
    if (mode == 2 || mode == 4 || mode == 11) {
        if (car != 0) return;
        CarBody& hud = BodyOf(ctx, 0);
        hud.hudCompare = 0;
        hud.hudTimer2 = 120;
        hud.hudGap = gap;
        return;
    }
    if (mode != 0) return;
    const int other = 1 - car;
    if (!ctx.allowOutOfRangeGap && (other < 0 || other >= int(ctx.globals->carCount))) return;
    CarBody& otherHud = BodyOf(ctx, other);
    otherHud.hudCompare = gap;
    otherHud.hudTimer2 = 120;
    otherHud.hudGap = 0;
    CarBody& hud = BodyOf(ctx, car);
    hud.hudCompare = 0;
    hud.hudTimer2 = 120;
    hud.hudGap = gap;
}

// ================================================================ the per-car progress

uint32_t LapCheck(ShellContext& ctx, int car, int32_t previous) { // 0x8003C70C
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    const ShellCourse& course = *ctx.course;
    CarBody& body = BodyOf(ctx, car);
    const bool license = g.gameMode == 3;
    // Game mode 6 (0x80012378): the ghost's line times run on its own clock offset (0x800122C4 + 0xC0), player 1's lap lines
    // drive the ghost (0x8001286C), and every call ends in 0x8003FB70 outside a replay.
    const bool mode6 = g.gameMode == 6;
    if (mode6 && ctx.ghost == nullptr) throw std::runtime_error("race shell: game mode 6 without its ghost session");
    auto ghostTime = [&](uint32_t atLine, uint32_t elapsed) { // the line time of the car since its last line
        if (mode6 && body.contactType == 2) {
            int32_t offset;
            std::memcpy(&offset, ctx.ghost->block.data() + kGhostPlayback + 0xC0, 4);
            return Sub(int32_t((atLine + uint32_t(offset)) / 3), body.lastLineTime);
        }
        return Sub(int32_t(elapsed), body.lastLineTime);
    };
    uint32_t result = 0;
    const int32_t current = body.courseDistance;
    const int32_t length = course.courseLength;
    const int32_t perFrame = MsPerFrame(g);
    if (current < previous) {
        if (Add(current, length / 2) < previous) { // wrapped forward across the start line
            if ((body.flags78D & 2) == 0) {
                if (body.lap == 0 && s.twoPlayerLaps == 0) {
                    body.lap = int16_t(body.lap + 1); // the crossing right after the start
                } else {
                    const int32_t remaining = Sub(length, previous);
                    const int32_t fraction = Div(Mul(remaining, perFrame), Add(remaining, current));
                    const uint8_t sector = body.sector;
                    body.sector = 0;
                    if (body.finishFlag == 0) {
                        const uint32_t atLine = s.raceClock + uint32_t(fraction);
                        const uint32_t elapsed = atLine / 3;
                        const int32_t lapTime = ghostTime(atLine, elapsed);
                        const int32_t frameRest = Sub(perFrame, fraction);                                          // s5
                        const int32_t fraction12 = Div(int32_t(uint32_t(remaining) << 12), Add(remaining, current)); // s0
                        const int32_t lap = body.lap;
                        body.lap = int16_t(lap + 1);
                        if (lap == 0) {
                            if (mode6) GhostLapLine(ctx, body, kGhostFirstLine, fraction12, frameRest, 0, true); // 0x8001286C
                        } else {
                            RecordSector(s.board, lap - 1, sector, int32_t(elapsed), car);
                            if (license) {
                                LicenseLapLine(ctx, body, car, lap, elapsed); // 0x8003D3C0
                            } else {
                                OnLapLine(ctx, car, lap, lapTime, int32_t(elapsed), body.maxSpeedReadout, body.resetState);
                                if (mode6) GhostLapLine(ctx, body, atLine, fraction12, frameRest, body.resetState, true); // 0x8001286C
                                if (g.gameMode != 9) {
                                    body.resetState = 0;
                                    body.resetCounter = 0;
                                    body.maxSpeedReadout = body.speedReadout;
                                }
                            }
                        }
                        if (!license) body.lastLineTime = int32_t(elapsed); // licence times run from the start
                    }
                }
            } else {
                body.flags78D = uint8_t(body.flags78D & 0xFD); // forward again after a backwards crossing
            }
        } else {
            result = 1; // moved backwards
        }
    } else if (Add(previous, length / 2) < current) { // wrapped backwards across the start line
        result = 1;
        body.flags78D = uint8_t(body.flags78D | 2);
    } else {
        if (body.finishFlag == 0) { // the 400 m / 1000 m marks of modes 7 / 8
            int32_t mark = 0;
            if (g.gameMode == 7) mark = 0x1900000;
            else if (g.gameMode == 8) mark = 0x3E80000;
            if (mark != 0 && (body.flags78D & 2) == 0 && previous < mark && mark <= current) {
                const int32_t fraction = Div(Mul(Sub(mark, previous), perFrame), Sub(current, previous));
                const uint32_t elapsed = (s.raceClock + uint32_t(fraction)) / 3;
                OnLapLine(ctx, car, 1, Sub(int32_t(elapsed), body.lastLineTime), int32_t(elapsed), body.maxSpeedReadout, body.resetState);
            }
        }
        const uint32_t sector = body.sector;
        if (sector >= course.startLines.size()) throw std::runtime_error("race shell: sector index beyond the start-line table");
        const int32_t line = course.startLines[sector];
        if ((body.flags78D & 2) == 0 && body.lap != 0 && previous < line && line <= current) {
            if (body.finishFlag == 0) {
                const int32_t fraction = Div(Mul(Sub(line, previous), perFrame), Sub(current, previous));
                const uint32_t atLine = s.raceClock + uint32_t(fraction);
                const uint32_t elapsed = atLine / 3;
                const int32_t split = ghostTime(atLine, elapsed);
                RecordSector(s.board, body.lap - 1, int32_t(sector), int32_t(elapsed), car);
                const bool finishLine = course.pointToPoint && int32_t(sector) == course.startLineCount - 1;
                if (license) {
                    LicenseSectorLine(ctx, body, car, body.lap, int32_t(sector), split); // 0x8003D314
                } else if (finishLine) {
                    OnLapLine(ctx, car, 1, split, int32_t(elapsed), body.maxSpeedReadout, body.resetState);
                    body.lap = 2;
                    Finish(ctx, body);
                    if (mode6) GhostLapLine(ctx, body, atLine, 0, 0, body.resetState, false); // 0x8001286C (the goal: no restart)
                    body.lastLineTime = int32_t(elapsed);
                } else {
                    OnSplit(ctx, car, int32_t(sector), split, body.resetState);
                }
            }
            body.sector = uint8_t(sector + 1);
        }
    }
    if (mode6 && g.demoFlag == 0) GhostCarCheck(ctx, body); // 0x8003FB70
    return result;
}

void ProgressCar(ShellContext& ctx, int car, CoursePlacementQueries& course) { // 0x8003CE3C
    CarBody& body = BodyOf(ctx, car);
    const int32_t previous = body.courseDistance;
    const int32_t distance = course.CourseDistance(body.chunkIndex, int32_t(uint32_t(body.position[0]) << 4), int32_t(uint32_t(body.position[2]) << 4),
                                                   int32_t(uint32_t(body.position[1]) << 4));
    body.courseDistance = distance;
    if (ctx.state->hold != 0) return;
    if (LapCheck(ctx, car, previous) == 1) {
        body.flags78D = uint8_t(body.flags78D | 1);
        const int32_t forward = body.forwardSpeed, lateral = body.lateralSpeed;
        const int32_t squared = Add(Mul12Floor(forward, forward), Mul12Floor(lateral, lateral)); // 0x80075BF4
        uint8_t code;
        if (squared < 0x1EE1) {
            if (0xB1C < squared) { SectionState(ctx, body, distance, previous); return; }
            code = 0;
        } else {
            if (body.contactFlags != 0 || int8_t(body.controlClass) == 2 || body.raceState != 0) { SectionState(ctx, body, distance, previous); return; }
            code = 1; // wrong way at speed
        }
        RequestMessage(body, code, 0);
    } else {
        body.flags78D = uint8_t(body.flags78D & 0xFE);
        RequestMessage(body, 0, 0);
    }
    SectionState(ctx, body, distance, previous);
}

void ProgressCars(ShellContext& ctx, const int8_t* raceOrder, int count, CoursePlacementQueries& course) { // 0x8003CF94
    for (int i = 0; i < count; i++) ProgressCar(ctx, raceOrder[i], course);
    SectorBoard& board = ctx.state->board;
    const int32_t rows = ctx.course->startLineCount + 1;
    if (rows > 4) throw std::runtime_error("race shell: more sector lines than the board has rows");
    for (int32_t row = 0; row < rows; row++) {
        const int32_t count_ = board.count[row];
        int32_t i = board.shown[row];
        if (i == count_) continue;
        for (; i < count_; i++)
            if (i != 0) ShowGap(ctx, board.car[row][i], Sub(board.times[row][i], board.times[row][0]));
        board.shown[row] = int8_t(count_);
    }
}

// ================================================================ end of frame: display timers, start / end sequences

namespace {

void StartSignalTimer(ShellContext& ctx) { // 0x8002A0D4
    RaceShellState& s = *ctx.state;
    const int32_t before = s.startTimer;
    if (before <= 0) return;
    const int32_t after = before - int32_t(ctx.globals->frameStep);
    const int32_t clamped = after < 0 ? 0 : after;
    s.startTimer = int16_t(clamped);
    auto sound = [&](int32_t argument) { if (ctx.hooks.sound) ctx.hooks.sound(ctx.hooks.user, 0x800189C4u, argument); };
    if (after < 0xF0 && 0xEF < before) { // green light: the race music (0x80029C60)
        sound(1);
        s.musicRequest = s.musicRaceTrack;
        s.musicRequestFlag = 1;
    } else if ((clamped < 300 && 299 < before) || (clamped < 0x168 && 0x167 < before) || (clamped < 0x1A4 && 0x1A3 < before)) {
        sound(0);
    }
}

void CountAux(int16_t& aux) { // 0x8002A3AC
    if (aux < 0) return;
    const int32_t next = int32_t(uint16_t(aux)) + 1;
    aux = int16_t(next);
    if (int16_t(next) >= 257) aux = 256;
}

bool EndTimer(ShellContext& ctx, const uint32_t pads[2]) { // 0x8002A700: false when the results wait is over
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    if (s.endTimer < 0) return true;
    auto sound = [&](int32_t argument) { if (ctx.hooks.sound) ctx.hooks.sound(ctx.hooks.user, 0x80060840u, argument); };
    auto step = [&](int32_t& n) { // one count, up to three per frame while X / Start is held
        const uint32_t buttons = pads[0];
        s.endTimer = int16_t(s.endTimer + 1);
        n--;
        if ((buttons & 0xA00) == 0) n = 0;
    };
    auto waitForButton = [&](int32_t& n) { // the timer stops until the pad's X / Start
        n = 0;
        if ((pads[1] & 0xA00) != 0) { sound(1); s.endTimer = int16_t(s.endTimer + 1); }
    };
    auto cap = [&](int32_t limit) {
        if (limit < s.endTimer) { s.endTimer = int16_t(limit); return false; }
        return true;
    };
    int32_t n = 3;
    switch (g.gameMode) {
    case 1: case 6: case 7: case 8: case 9: case 10:
        do step(n); while (0 < n);
        return cap(0xC6);
    case 2:
        do {
            const int16_t value = s.endTimer;
            if (value == 0x13B || (value >= 0x13C && (value == 0x1B9 || value == 0x237))) waitForButton(n);
            else {
                if (value < 0x13C && value == 0x9E) s.endTimerAux = 0; // 0x8002A3A4
                step(n);
            }
            CountAux(s.endTimerAux);
        } while (0 < n);
        return cap(g.players801D5DF6 > 1 ? 0x238 : 0x13B);
    case 3:
        if (s.licenseResult == kLicenseResultPass) { // the word 0x801D5DEC (0x800156EC)
            do {
                if (s.endTimer == 0x78) { sound(3); step(n); }
                else if (s.endTimer != 0x92) step(n);
                else waitForButton(n);
                if (n < 1) return cap(0x93);
            } while (true);
        }
        if (s.endTimer == 0x1D) {
            if ((pads[1] & 0xA00) != 0) { sound(1); s.endTimer = int16_t(s.endTimer + 1); }
        } else {
            s.endTimer = int16_t(s.endTimer + 1);
        }
        return cap(0x1E);
    case 0xB:
        do {
            if (s.endTimer == 0xE5) waitForButton(n);
            else step(n);
        } while (0 < n);
        return cap(0xE6);
    default:
        do {
            if (s.endTimer == 0x9E) { s.endTimerAux = 0; step(n); }
            else if (s.endTimer != 0x13B) step(n);
            else waitForButton(n);
            CountAux(s.endTimerAux);
        } while (0 < n);
        return cap(0x13C);
    }
}

} // namespace

bool EndFrame(ShellContext& ctx, const uint32_t pads[2]) { // 0x8002E550
    RaceShellState& s = *ctx.state;
    const ShellGlobals& g = *ctx.globals;
    for (int car = 0; car < int(g.carCountShell); car++) {
        CarBody& hud = BodyOf(ctx, car);
        if (0 < hud.hudTimer) hud.hudTimer = int16_t(hud.hudTimer - 1);
        if (0 < hud.hudTimer2) hud.hudTimer2 = int16_t(hud.hudTimer2 - 1);
        if (0 < hud.hudTimer3) hud.hudTimer3 = int16_t(hud.hudTimer3 - 1);
    }
    if (s.raceClock != 0) s.clockFrames = uint8_t(s.clockFrames + 1);
    StartSignalTimer(ctx);
    return EndTimer(ctx, pads);
}

int32_t DisplayLap(const ShellContext& ctx, const CarBody& body) { // 0x8003D1E4
    const int32_t lap = body.lap;
    if (lap <= 0) return 1;
    if (ctx.globals->gameMode == 3 && ctx.globals->license.type == 5) return 1;
    return lap;
}

// ================================================================ licence tests (mode 3)

void WheelCourseExtent(const CarBody& body, int32_t courseLength, CoursePlacementQueries& course, int32_t& minimum, int32_t& maximum) { // 0x8003D498
    const int32_t wrap = (courseLength / 8) * 7; // the last eighth of the course lies before the start line
    int32_t low = 0x7FFFFFFF, high = -0x7FFFFFFF;
    for (uint32_t w = 0; w < 4; w++) {
        const Wheel& wheel = body.wheels[w];
        const int32_t x = int32_t(uint32_t(Add(body.position[0], wheel.offsetX)) << 4);
        const int32_t height = int32_t(uint32_t(Add(body.position[2], wheel.offsetHeight)) << 4);
        const int32_t y = int32_t(uint32_t(Add(body.position[1], wheel.offsetY)) << 4);
        int32_t distance = course.CourseDistance(body.chunkIndex, x, height, y); // 0x80028C6C
        if (wrap < distance) distance = Sub(distance, courseLength);
        if (distance < low) low = distance;
        if (high < distance) high = distance;
    }
    minimum = low;
    maximum = high;
}

int32_t LooseSurfaceWheels(const CarBody& body) { // 0x8003D458
    int32_t count = 0;
    for (uint32_t w = 0; w < 4; w++)
        if (body.wheels[w].surface >= 2) count++;
    return count;
}

void LicenseResult(ShellContext& ctx, uint32_t carIndex, int32_t code, uint32_t time, uint16_t maxSpeed) { // 0x800156EC
    RaceShellState& s = *ctx.state;
    s.licenseResult = code;
    s.licenseTime = time;
    if (carIndex >= ctx.globals->carCount) throw std::runtime_error("race shell: licence result of a car outside the race");
    RecordOf(ctx, int(carIndex)).finishTime = -1;
    if (code == kLicenseResultPass) RecordLap(s.results[0], 1, int32_t(time), maxSpeed, false); // the player's record, whatever the car
    if (ctx.globals->demoFlag == 0) {
        s.endTimer = 0;    // 0x8002A6F8: the race-end sequence starts
        FinishJingle(ctx); // 0x80029C84(*(0x8002F4F4))
    }
    if (ctx.hooks.license) ctx.hooks.license(ctx.hooks.user, int(carIndex), code, time);
}

namespace {
void LicenseEnd(ShellContext& ctx, CarBody& body, uint8_t state, uint32_t raceState) { // 0x8003D244 / 0x8003D2A0
    body.licenseState = state;
    SetRaceState(ctx, body, raceState);
    body.finishFlag = 1;
    LicenseResult(ctx, body.carIndex, body.licenseCode, uint32_t(body.licenseTime), body.maxSpeedReadout);
}
} // namespace

void LicenseFail(ShellContext& ctx, CarBody& body) { LicenseEnd(ctx, body, kLicenseFailed, 6); } // 0x8003D244

// 0x8003D2A0 tests for type 5 before the race state change, but both paths pass state 5 (the branch only skips a
// register copy that holds the same body).
void LicensePass(ShellContext& ctx, CarBody& body) { LicenseEnd(ctx, body, kLicensePassed, 5); }

void LicenseSectorLine(ShellContext& ctx, CarBody& body, int car, int32_t lap, int32_t sector, int32_t split) { // 0x8003D314
    const LicenseTest& test = ctx.globals->license;
    const int32_t lines = ctx.course->startLineCount;
    if (test.type == 3 && sector == lines - 1) { // the goal line
        body.licenseCode = uint8_t(kLicenseResultPass);
        body.licenseTime = split;
        LicensePass(ctx, body);
        return;
    }
    OnSplit(ctx, car, Add(Mul(lap - 1, lines + 1), sector), split, body.resetState); // 0x80015510
}

void LicenseLapLine(ShellContext& ctx, CarBody& body, int car, int32_t lap, uint32_t elapsed) { // 0x8003D3C0
    const LicenseTest& test = ctx.globals->license;
    if (test.type == 5 && int32_t(test.targetLap) == lap) {
        body.licenseCode = uint8_t(kLicenseResultPass);
        body.licenseTime = int32_t(elapsed);
        LicensePass(ctx, body);
        return;
    }
    OnSplit(ctx, car, Sub(Mul(lap, ctx.course->startLineCount + 1), 1), int32_t(elapsed), body.resetState); // 0x80015510
}

void LicenseCheck(ShellContext& ctx, CarBody& body, CoursePlacementQueries& course) { // 0x8003D5F8
    const uint32_t state = body.licenseState;
    if (state == kLicenseFailed || state == kLicensePassed) return;
    const LicenseTest& test = ctx.globals->license;
    const uint32_t reset = body.resetState;
    int32_t code;
    if (reset & 2) code = kLicenseResultOffCourse;   // the physics' stuck / off-course reset
    else if (reset & 1) code = kLicenseResultWall;   // a hard wall hit (control class 1)
    else {
        if (test.type != 2) return;
        int32_t low = 0, high = 0;
        WheelCourseExtent(body, ctx.course->courseLength, course, low, high);
        const int32_t tens = int32_t(test.boxStart) * 5;
        const int32_t boxEnd = int32_t(uint32_t(tens * 2 + int32_t(test.boxLength)) << 16);
        if (boxEnd < high) code = kLicenseResultOvershot;
        else {
            if (low < int32_t(uint32_t(tens) << 17)) return; // not in the box yet
            const int32_t speed = ApproxLength3(body.velocity[0], body.velocity[1], body.velocity[2]); // 0x8003C398
            if (speed >= 1138) return;                                                                  // still moving (1 km/h)
            if (LooseSurfaceWheels(body) != 0) code = kLicenseResultOffCourse;
            else {
                // The stop time: the clock plus the frame fraction the residual speed stands for.
                const int32_t fraction = Mul(speed, MsPerFrame(*ctx.globals)) / 1138;
                const uint32_t clock = ctx.state->raceClock + uint32_t(fraction);
                body.licenseCode = uint8_t(kLicenseResultPass);
                body.licenseTime = int32_t(clock / 3 - uint32_t(body.lastLineTime));
                LicensePass(ctx, body);
                return;
            }
        }
    }
    body.licenseCode = uint8_t(code);
    LicenseFail(ctx, body);
}

uint32_t LicenseTargetTime(const uint8_t* record, uint32_t medal) { // 0x8003D7B8
    constexpr size_t kMedalTimes = 0x26; // settings block: { u8 minutes * 100 + seconds; u8 hundredths }[medal]
    const uint8_t* pair = record + kMedalTimes + size_t(medal) * 2;
    const uint32_t secondsField = pair[0]; // minutes * 100 + seconds
    return (secondsField / 100) * 60000 + (secondsField % 100) * 1000 + uint32_t(pair[1]) * 10;
}

// ================================================================ game mode 6: the ghost (Time Trial / Rally)

namespace {

template <typename T> T GetAt(const uint8_t* p, size_t offset) { T v; std::memcpy(&v, p + offset, sizeof v); return v; }
template <typename T> void PutAt(uint8_t* p, size_t offset, T v) { std::memcpy(p + offset, &v, sizeof v); }
uint8_t* BytesOf(CarBody& body) { return reinterpret_cast<uint8_t*>(&body); }
const uint8_t* BytesOf(const CarBody& body) { return reinterpret_cast<const uint8_t*>(&body); }

constexpr size_t kPlaybackIndex = 0xB8, kPlaybackPending = 0xBA, kPlaybackLap = 0xBC, kPlaybackBlend = 0xBE, kPlaybackClockOffset = 0xC0;
constexpr size_t kBodyGhostFrom = 0x45C; // the body part the snapshots hold (+ 0x33C)

int16_t RingCount(const GhostSession& g) { return GetAt<int16_t>(g.ring.data(), 0); }
int16_t RingIndex(const GhostSession& g) { return GetAt<int16_t>(g.ring.data(), 2); }
uint8_t* RingLap(GhostSession& g, int32_t index) {
    if (index < 0 || index > 3) throw std::runtime_error("race shell: ghost lap buffer index outside the ring");
    return g.ring.data() + 4 + size_t(index) * kGhostLapSize;
}
uint8_t* CurrentLap(GhostSession& g) { return RingLap(g, RingIndex(g)); }                                      // 0x800122D0
uint8_t* ReferenceLap(GhostSession& g, bool demo) { return demo ? RingLap(g, g.playerStreamLap) : g.reference.data(); } // 0x80012304
uint8_t* PlayerSnapshot(GhostSession& g) { return g.block.data() + kGhostPlayerSnapshot; }                    // 0x8001236C
uint8_t* GhostSnapshot(GhostSession& g) { return g.block.data() + kGhostGhostSnapshot; }                      // 0x80012360
uint8_t* Playback(GhostSession& g) { return g.block.data() + kGhostPlayback; }                                // 0x800122C4
uint8_t* PlayerStream(GhostSession& g) { return RingLap(g, g.playerStreamLap) + kGhostLapStream; }           // *(car 0 + 0x1C)
uint8_t* GhostStream(GhostSession& g) { // *(car 1 + 0x1C): 0x801DA580, or a ring lap after 0x800125BC's car record copy
    return (g.ghostStreamLap < 0 ? g.reference.data() : RingLap(g, g.ghostStreamLap)) + kGhostLapStream;
}

// A stream operation on the object image in a lap buffer (gt2formats/replay.h works on its own copy).
template <typename F> void WithStream(uint8_t* object, F&& operation) {
    ReplayStream stream = ReplayStream::FromBytes(std::span<const uint8_t>(object, kGhostLapStreamBytes));
    operation(stream);
    const std::vector<uint8_t>& bytes = stream.Bytes();
    std::memcpy(object, bytes.data(), std::min(bytes.size(), kGhostLapStreamBytes));
}
bool StreamEnded(const uint8_t* object) { return GetAt<int16_t>(object, 0x0C) != 0; }

void ClearLapHead(uint8_t* lap) { // 0x8003FE14
    std::memset(lap, 0, 0xE0);
    PutAt<int32_t>(lap, 4, kNoTime);
}
void ClearSnapshot(uint8_t* snapshot) { std::memset(snapshot, 0, kGhostSnapshotSize); } // 0x8003FDE0
void ClearPlayback(uint8_t* playback) { // 0x8003FE4C
    std::memset(playback, 0, kPlaybackIndex);
    PutAt<int16_t>(playback, kPlaybackIndex, 0);
    PutAt<int16_t>(playback, kPlaybackBlend, 0x800);
    PutAt<int16_t>(playback, kPlaybackPending, 0);
    PutAt<int32_t>(playback, kPlaybackClockOffset, 0);
}
uint32_t CarStateSum(const uint8_t* state) { // 0x8003EF18
    uint32_t sum = 0;
    for (size_t i = 0; i < kGhostCarStateSize; i++) sum += state[i];
    return sum;
}
bool GhostStreamDone(GhostSession& g, bool demo) { // 0x8001252C
    if (demo) return false;
    return StreamEnded(GhostStream(g));
}
void ResetGhostStream(GhostSession& g) { // 0x800124B0
    WithStream(GhostStream(g), [](ReplayStream& s) { s.Init(true, kGhostStreamCapacity); });
    PutAt<uint16_t>(GhostSnapshot(g), 2, 0);
}
void ReadGhostFrame(GhostSession& g, ReplayFrame& frame) { // 0x80014074 -> 0x80013C90 (playback) on the ghost's stream
    frame = ReplayFrame{};
    WithStream(GhostStream(g), [&](ReplayStream& s) { s.Read(frame); });
}
void AdvanceRing(GhostSession& g) { // 0x80012570
    int16_t index = int16_t(RingIndex(g) + 1);
    if (index == 4) index = 0;
    int16_t count = int16_t(RingCount(g) + 1);
    if (count >= 4) count = 4;
    PutAt<int16_t>(g.ring.data(), 2, index);
    PutAt<int16_t>(g.ring.data(), 0, count);
}
void SavePose(uint8_t* pose, const CarBody& body) { // 0x8003F09C
    const uint8_t* b = BytesOf(body);
    std::memcpy(pose, b + 0x65C, 0x24);        // position, rows
    std::memcpy(pose + 0x24, b + 0x680, 0x24); // visual position, rows
    PutAt<int32_t>(pose, 0x48, body.courseDistance);
    PutAt<int16_t>(pose, 0x50, body.pitch);
    PutAt<int16_t>(pose, 0x52, body.roll);
    PutAt<int16_t>(pose, 0x54, body.heading);
    PutAt<int16_t>(pose, 0x58, body.visualPitch);
    PutAt<int16_t>(pose, 0x5A, body.visualRoll);
}
void LoadPose(CarBody& body, const uint8_t* pose) { // 0x8003F16C
    uint8_t* b = BytesOf(body);
    std::memcpy(b + 0x65C, pose, 0x24);
    std::memcpy(b + 0x680, pose + 0x24, 0x24);
    body.courseDistance = GetAt<int32_t>(pose, 0x48);
}
// 0x80031440: the body's pointers into itself (the curve tables of the steering limit / the engine and of both axles' tyre
// blocks), for the body at `token`.
void RelinkBodyPointers(CarBody& body, uint32_t token) {
    uint8_t* b = BytesOf(body);
    auto link = [&](uint32_t field, uint32_t target) { PutAt<uint32_t>(b, field, token + target); };
    link(0x80, 0x88);
    link(0x84, 0xC8);
    link(0x40, 0x48);
    link(0x44, 0x54);
    for (uint32_t axle = 0, o = 0x194; axle < 2; axle++, o += 0xD8) {
        link(o + 0x04, o + 0x0C);
        link(o + 0x08, o + 0x1C);
        link(o + 0x30, o + 0x38);
        link(o + 0x70, o + 0x38);
        link(o + 0x74, o + 0x78);
        link(o + 0x94, o + 0x9C);
        link(o + 0x98, o + 0xAC);
        link(o + 0xC0, o + 0xC8);
        link(o + 0x34, o + 0x50);
        link(o + 0xC4, o + 0xD0);
    }
}

int32_t AngleBlend(int32_t wa, int32_t a, int32_t wb, int32_t b) { // 0x8003EFF0: the two angles taken the short way round
    int32_t x = WrapAngle(a), y = WrapAngle(b);
    const int32_t d = Sub(x, y);
    if (d >= 2048) y = Add(y, 4096);
    else if (d < -2048) x = Add(x, 4096);
    int32_t sum = Add(Mul(wa, x), Mul(wb, y));
    if (sum < 0) sum = Add(sum, 4095);
    return WrapAngle(sum >> 12);
}
int32_t DistanceBlend(int32_t wa, int32_t a, int32_t wb, int32_t b, int32_t length) { // 0x8003F200: across the start line
    const int32_t half = length / 2;
    const int32_t sa = int16_t(wa), sb = int16_t(wb);
    if (Add(a, half) < b) {
        int32_t v = Add(Mul12Wide(sa, a), Mul12Wide(sb, Sub(b, length)));
        if (v < 0) v = Add(v, length);
        return v;
    }
    if (Add(b, half) < a) {
        int32_t v = Add(Mul12Wide(sa, Sub(a, length)), Mul12Wide(sb, b));
        if (v < 0) v = Add(v, length);
        return v;
    }
    return Add(Mul12Wide(sa, a), Mul12Wide(sb, b));
}

} // namespace

void SaveCarState(uint8_t* out, const CarBody& body) { // 0x800350FC
    const uint8_t* b = BytesOf(body);
    for (size_t w = 0; w < 4; w++) std::memcpy(out + 0x68 + w * 0x1C, b + 0x460 + w * 0x68, 0x1C);
    std::memcpy(out, b + 0x600, 0x68);
}

void RestoreCarState(CarBody& body, const uint8_t* state, bool hasGridList) { // 0x8003519C
    uint8_t* b = BytesOf(body);
    std::memset(b + kBodyGhostFrom, 0, kGhostSnapshotBodySize);
    for (size_t w = 0; w < 4; w++) std::memcpy(b + 0x460 + w * 0x68, state + 0x68 + w * 0x1C, 0x1C);
    std::memcpy(b + 0x600, state, 0x68);
    body.timeScale = 0x1000;
    body.airborne = 0;
    if (Add(Add(body.wheels[0].load, body.wheels[1].load), Add(body.wheels[2].load, body.wheels[3].load)) == 0) body.airborne = 1;
    body.gridSlot = int8_t(hasGridList ? 8 : 0); // 0x800392AC(0)
    body.flags78D = 0;
    BuildAttitudeMatrix(body.basis[0], body.basis[1], body.basis[2], body.pitch, body.roll, body.heading); // 0x80044EA4
    UpdateVisualPose(body);      // 0x8003E7EC
    UpdateWheelGeometry(body);   // 0x8004335C
    UpdateBodyFrameSpeeds(body); // 0x800304DC
    UpdateEngineRpm(body);       // 0x8003941C
    ResetViewState(body);        // 0x80032E44
    body.footprintSet = 0;
    UpdateFootprint(body);       // 0x80041AE8
    const int current = body.footprintSet; // 0x80041C78: the other corner set = the current one
    for (int corner = 0; corner < 4; corner++) {
        body.footprint[1 - current][corner][0] = body.footprint[current][corner][0];
        body.footprint[1 - current][corner][1] = body.footprint[current][corner][1];
    }
    for (int i = 7; i >= 0; i--) body.neighbourFlags[i] = 0;
    body.neighbourClass = 0;
}

uint8_t EntryContactType(uint8_t entryKind, uint8_t gameMode, bool demo) { // 0x80012CD4
    if (entryKind == 2) return gameMode == 6 ? 2 : 1;
    if (entryKind == 3) return gameMode == 6 && demo ? 2 : 0;
    return 0;
}

int32_t ApproxDistance(int32_t dx, int32_t dy, int32_t dz) { // 0x800140A4 (the sort key and LOD measure, car + 0x804)
    uint32_t a = dx < 0 ? 0u - uint32_t(dx) : uint32_t(dx);
    uint32_t b = dy < 0 ? 0u - uint32_t(dy) : uint32_t(dy);
    uint32_t c = dz < 0 ? 0u - uint32_t(dz) : uint32_t(dz);
    uint32_t hi = a, mid = b;
    if (hi < mid) std::swap(hi, mid);
    uint32_t lo = c;
    if (hi < lo) std::swap(hi, lo); // hi = the largest; lo = the old hi
    if (mid < lo) std::swap(mid, lo);
    return int32_t(hi + (mid >> 1) + (lo >> 2));
}

CarDrawMode CarDrawRule(const CarDrawInputs& in) { // 0x800140A4
    CarDrawMode m;
    m.distance = ApproxDistance(in.dx, in.dy, in.dz);
    if (in.mirror && m.distance > 0x63FFFF) return m;
    if (in.noLap != 0) return m;
    if (in.viewCar && in.viewCarHidden) return m;
    // The wheels' ground class: 0 unless class 1 (2) is on more wheels than class 0 (than the larger of the two).
    int32_t count[3] = {0, 0, 0};
    for (uint8_t g : in.wheelGround)
        if (g < 3) count[g]++;
    uint8_t group = count[0] < count[1] ? 1 : 0;
    if ((group ? count[1] : count[0]) < count[2]) group = 2;
    uint8_t lod = 3;
    if (!in.mirror) {
        lod = in.viewCar ? 1 : 0;
        if (in.padSlot == 1 && (in.gameMode == 6 || (in.hold == 0 && in.demo == 0))) {
            if (in.ghostToggle == 0) return m;
            bool ghostLook = false;
            switch (in.ghostOption) {
            case 0:
                if (in.gameMode == 6) return m;
                ghostLook = true;
                break;
            case 1: ghostLook = true; break;
            case 2: ghostLook = m.distance <= 0x2FFFF; break;
            case 3: ghostLook = m.distance <= 0x4FFFF; break;
            default: break;
            }
            if (ghostLook) lod = group = kCarDrawGhostLook;
        }
    }
    m.drawn = true;
    m.lod = lod;
    m.group = group;
    return m;
}

void GhostRaceLoad(GhostSession& g, bool demo) { // 0x80012410
    ClearPlayback(Playback(g));
    if (g.initialised != 0) return;
    g.initialised = 1;
    if (demo) return;
    ClearLapHead(ReferenceLap(g, demo));
    ClearSnapshot(GhostSnapshot(g));
    ClearLapHead(CurrentLap(g));
    ClearSnapshot(PlayerSnapshot(g));
}

void GhostSetupPlayer(GhostSession& g, bool demo) { // 0x80013244
    if (!demo) {
        PutAt<int16_t>(g.ring.data(), 0, 0);
        PutAt<int16_t>(g.ring.data(), 2, 0);
    }
    g.playerStreamLap = 0; // car 0 + 0x1C = the player's results record + 0x1E0 = lap buffer 0
    WithStream(PlayerStream(g), [&](ReplayStream& s) { s.Init(demo, kGhostStreamCapacity); });
}

void GhostSetupGhostCar(GhostSession& g) { // 0x80012CD4, entry kind 2 in mode 6
    g.newBest = 0;
    g.ghostStreamLap = -1; // car + 0x1C = 0x801DA580
    g.ghostPresent = 1; // car + 0x0E = 1 for every car at the top of 0x80012CD4
    if (!g.entryHasLap) {
        WithStream(GhostStream(g), [](ReplayStream& s) {
            s.Init(false, kGhostStreamCapacity);
            s.End(false);
        });
        g.ghostPresent = 0;
    }
    g.ghostStreamOwned = 1;
    WithStream(GhostStream(g), [](ReplayStream& s) { s.Init(true, kGhostStreamCapacity); });
}

void GhostSavePlayerStart(GhostSession& g, const CarBody& body) { // 0x8003F6B8
    uint8_t* lap = CurrentLap(g);
    uint8_t* snapshot = PlayerSnapshot(g);
    SaveCarState(lap + kGhostCarState, body);
    snapshot[1] = 2;
    snapshot[0] = 1;
    PutAt<uint16_t>(snapshot, 2, 0);
    PutAt<int16_t>(lap, 0, 0);
    lap[2] = 0;
    lap[3] = 0;
}

bool GhostStartCar(GhostSession& g, CarBody& body, bool hasGridList, bool demo) { // 0x8003EF40 (0x80012304: the reference / the replay's ring lap)
    uint8_t* snapshot = GhostSnapshot(g);
    const uint8_t* reference = ReferenceLap(g, demo);
    snapshot[0] = 3;
    ResetGhostStream(g);
    if (CarStateSum(reference + kGhostCarState) == 0) snapshot[1] = 0;
    if (snapshot[1] == 0) return false;
    std::memset(snapshot + kGhostSnapshotBody, 0, kGhostSnapshotBodySize);
    snapshot[1] = 1;
    const uint8_t contactType = body.contactType, carIndex = body.carIndex;
    RestoreCarState(body, reference + kGhostCarState, hasGridList);
    body.contactType = contactType;
    body.carIndex = carIndex;
    return true;
}

uint8_t GhostHold(const ShellContext& ctx) { // 0x8003FAEC
    GhostSession& g = *ctx.ghost;
    const uint8_t* snapshot = GhostSnapshot(g);
    if (snapshot[1] == 0) return 2;
    if (GetAt<int16_t>(snapshot, 0) == 0x203) return 1;
    if (ctx.course->pointToPoint) return 0;
    return GhostStreamDone(g, ctx.globals->demoFlag != 0) ? 2 : 0;
}

void GhostPlayerInput(ShellContext& ctx, const ReplayFrame& frame) { // 0x80013EF0 (live) -> 0x80013C90 (record)
    GhostSession& g = *ctx.ghost;
    const uint16_t limit = ctx.globals->gameMode == 3 ? 60 : 300;
    const bool recording = ctx.state->sinceFinish < limit;
    WithStream(PlayerStream(g), [&](ReplayStream& s) {
        if (recording) s.Record(frame);
        else s.End(false);
    });
    uint8_t* snapshot = PlayerSnapshot(g);
    PutAt<uint16_t>(snapshot, 2, uint16_t(GetAt<uint16_t>(snapshot, 2) + 1));
}

bool GhostCarInput(ShellContext& ctx, ReplayFrame& frame) { // 0x8003C250, pad slot 1
    GhostSession& g = *ctx.ghost;
    uint8_t* snapshot = GhostSnapshot(g);
    if (snapshot[1] == 1 || (snapshot[0] != 3 && !GhostStreamDone(g, ctx.globals->demoFlag != 0))) {
        PutAt<uint16_t>(snapshot, 2, uint16_t(GetAt<uint16_t>(snapshot, 2) + 1));
        ReadGhostFrame(g, frame);
        return true;
    }
    frame = ReplayFrame{};
    return false;
}

void GhostLapLine(ShellContext& ctx, CarBody& body, uint32_t clockAtLine, int32_t fraction12, int32_t frameRest, uint8_t invalid, bool arm) { // 0x8001286C
    GhostSession& g = *ctx.ghost;
    if (body.carIndex != 0 || ctx.globals->demoFlag != 0) return;
    if (ctx.globals->carCount < 2) throw std::runtime_error("race shell: game mode 6 without the ghost car");
    CarBody& ghost = BodyOf(ctx, 1);
    WithStream(PlayerStream(g), [](ReplayStream& s) { s.End(false); }); // 0x800167D0
    if (clockAtLine != kGhostFirstLine) {
        const int32_t lapTime = Sub(int32_t(clockAtLine / 3), body.lastLineTime);
        bool better = false;
        if (invalid == 0) better = g.ghostPresent == 0 || uint32_t(lapTime) < GetAt<uint32_t>(ReferenceLap(g, false), 4);
        if (better) {
            g.newBest = 1;
            uint8_t* lap = CurrentLap(g);
            PutAt<int32_t>(lap, 4, lapTime);
            // (the race block's entry 1 = entry 0 with kind 2, and car 1's parameter record = car 0's: the same car in these
            // races; the entry keeps its lap for the next race through GhostRaceEnd)
            std::memcpy(g.reference.data(), lap, 0xE0);
            std::memcpy(GhostSnapshot(g), PlayerSnapshot(g), kGhostSnapshotSize);
            std::memcpy(GhostStream(g), PlayerStream(g), kGhostLapStreamBytes);
            std::memcpy(BytesOf(ghost), BytesOf(body), kBodyGhostFrom); // 0x8001239C
            RelinkBodyPointers(ghost, ctx.bodyTokenBase + ctx.bodyTokenStride);  // 0x80031440 on car 1's body
            g.ghostPresent = 1;
            g.ghostStreamOwned = 0;
            if (ctx.hooks.ghostReference) ctx.hooks.ghostReference(ctx.hooks.user);
        }
    }
    if (!arm) return;
    if (g.ghostPresent != 0) GhostLapStart(ctx, ghost, body.lap, int16_t(fraction12)); // 0x8003F724
    AdvanceRing(g);
    g.playerStreamLap = int8_t(RingIndex(g));
    WithStream(PlayerStream(g), [](ReplayStream& s) { s.Init(false, kGhostStreamCapacity); });
    // 0x8003F5E0: the start state of the next lap.
    uint8_t* lap = CurrentLap(g);
    uint8_t* snapshot = PlayerSnapshot(g);
    SaveCarState(lap + kGhostCarState, body);
    snapshot[1] = 1;
    snapshot[0] = 1;
    PutAt<uint16_t>(snapshot, 2, 0);
    PutAt<int16_t>(lap, 0, int16_t(fraction12));
    lap[2] = uint8_t(frameRest);
    lap[3] = uint8_t((ctx.state->raceClock + uint32_t(MsPerFrame(*ctx.globals))) % 3);
}

void GhostLapStart(ShellContext& ctx, CarBody& ghost, int16_t lap, int16_t fraction12) { // 0x8003F724
    GhostSession& g = *ctx.ghost;
    const bool demo = ctx.globals->demoFlag != 0;
    uint8_t* playback = Playback(g);
    const uint8_t* reference = ReferenceLap(g, demo);
    if (GetAt<int16_t>(playback, kPlaybackPending) == 0) {
        const int16_t referenceFraction = GetAt<int16_t>(reference, 0);
        if (referenceFraction < fraction12) { // the reference crossed later in its frame: one frame later (0x8003FB70)
            PutAt<int16_t>(playback, kPlaybackBlend, int16_t(uint16_t(referenceFraction) - uint16_t(fraction12 - 0x1000)));
            PutAt<int16_t>(playback, kPlaybackLap, lap);
            PutAt<int16_t>(playback, kPlaybackPending, 2);
            return;
        }
        PutAt<int16_t>(playback, kPlaybackBlend, int16_t(uint16_t(referenceFraction) - uint16_t(fraction12)));
    } else {
        lap = GetAt<int16_t>(playback, kPlaybackLap);
        PutAt<int16_t>(playback, kPlaybackPending, 0);
    }
    PutAt<int16_t>(playback, kPlaybackIndex, 0);
    const uint8_t contactType = ghost.contactType, carIndex = ghost.carIndex, controlClass = ghost.controlClass;
    RestoreCarState(ghost, reference + kGhostCarState, ctx.course->grid.count != 0);
    uint8_t* pose = playback + size_t(GetAt<int16_t>(playback, kPlaybackIndex)) * kGhostPoseSize;
    SavePose(pose, ghost);
    std::memcpy(BytesOf(ghost) + kBodyGhostFrom, GhostSnapshot(g) + kGhostSnapshotBody, kGhostSnapshotBodySize);
    SavePose(pose, ghost);
    ghost.contactType = contactType;
    ghost.carIndex = carIndex;
    ghost.controlClass = controlClass;
    ghost.lap = lap;
    GhostSnapshot(g)[0] = 2;
    ResetGhostStream(g);
    ReplayFrame discarded; // 0x800124E8: the first frame is consumed
    ReadGhostFrame(g, discarded);
    uint8_t* snapshot = GhostSnapshot(g);
    PutAt<uint16_t>(snapshot, 2, uint16_t(GetAt<uint16_t>(snapshot, 2) + 1));
    const uint32_t clock = ctx.state->raceClock;
    const int32_t offset = Sub(int32_t(int8_t(reference[3])), int32_t(clock % 3));
    PutAt<int32_t>(playback, kPlaybackClockOffset, offset);
    ghost.lastLineTime = int32_t((clock + uint32_t(offset) - uint32_t(reference[2])) / 3);
}

void GhostCarCheck(ShellContext& ctx, CarBody& body) { // 0x8003FB70
    GhostSession& g = *ctx.ghost;
    uint8_t* player = PlayerSnapshot(g);
    uint8_t* ghostSnapshot = GhostSnapshot(g);
    uint8_t* playback = Playback(g);
    if (body.contactType == 2 && ghostSnapshot[1] == 1) {
        std::memcpy(ghostSnapshot + kGhostSnapshotBody, BytesOf(body) + kBodyGhostFrom, kGhostSnapshotBodySize);
        ghostSnapshot[1] = 2;
    }
    if (body.contactType != 0) return;
    const int16_t pending = GetAt<int16_t>(playback, kPlaybackPending);
    if (pending != 0) {
        if (pending == 1) {
            if (ctx.globals->carCount < 2) throw std::runtime_error("race shell: game mode 6 without the ghost car");
            GhostLapStart(ctx, BodyOf(ctx, 1), 0, 0);
        } else {
            PutAt<int16_t>(playback, kPlaybackPending, int16_t(pending - 1));
        }
    }
    if (player[0] == 1 && GetAt<uint16_t>(player, 2) == 1) {
        std::memcpy(player + kGhostSnapshotBody, BytesOf(body) + kBodyGhostFrom, kGhostSnapshotBodySize);
        player[1] = 2;
    }
}

void GhostBlendPose(ShellContext& ctx, int car) { // 0x8003F2F0
    GhostSession& g = *ctx.ghost;
    CarBody& body = BodyOf(ctx, car);
    uint8_t* playback = Playback(g);
    const int16_t index = GetAt<int16_t>(playback, kPlaybackIndex);
    uint8_t* next = playback + size_t(1 - index) * kGhostPoseSize;
    const uint8_t* last = playback + size_t(index) * kGhostPoseSize;
    SavePose(next, body);
    const uint16_t blend = GetAt<uint16_t>(playback, kPlaybackBlend);
    const int32_t wLast = int16_t(uint16_t(0x1000 - blend)), wNext = int16_t(blend);
    for (size_t i = 0; i < 3; i++)
        body.position[i] = Add(Mul12Wide(wLast, GetAt<int32_t>(last, i * 4)), Mul12Wide(wNext, GetAt<int32_t>(next, i * 4)));
    const int32_t pitch = AngleBlend(wLast, GetAt<int16_t>(last, 0x50), wNext, GetAt<int16_t>(next, 0x50));
    const int32_t roll = AngleBlend(wLast, GetAt<int16_t>(last, 0x52), wNext, GetAt<int16_t>(next, 0x52));
    const int32_t heading = AngleBlend(wLast, GetAt<int16_t>(last, 0x54), wNext, GetAt<int16_t>(next, 0x54));
    BuildAttitudeMatrix(body.basis[0], body.basis[1], body.basis[2], pitch, roll, heading);
    body.visualPosition[0] = body.position[0];
    body.visualPosition[1] = body.position[1];
    body.visualPosition[2] = Add(Mul12Wide(wLast, GetAt<int32_t>(last, 0x2C)), Mul12Wide(wNext, GetAt<int32_t>(next, 0x2C)));
    const int32_t visualPitch = AngleBlend(wLast, GetAt<int16_t>(last, 0x58), wNext, GetAt<int16_t>(next, 0x58));
    const int32_t visualRoll = AngleBlend(wLast, GetAt<int16_t>(last, 0x5A), wNext, GetAt<int16_t>(next, 0x5A));
    BuildAttitudeMatrix(body.visualBasis[0], body.visualBasis[1], body.visualBasis[2], visualPitch, visualRoll, heading);
    body.courseDistance = DistanceBlend(wLast, GetAt<int32_t>(last, 0x48), wNext, GetAt<int32_t>(next, 0x48), ctx.course->courseLength);
}

void GhostRestorePose(ShellContext& ctx, int car) { // 0x8003F548
    GhostSession& g = *ctx.ghost;
    uint8_t* playback = Playback(g);
    const int16_t index = GetAt<int16_t>(playback, kPlaybackIndex);
    LoadPose(BodyOf(ctx, car), playback + size_t(1 - index) * kGhostPoseSize);
    PutAt<int16_t>(playback, kPlaybackIndex, int16_t(1 - index));
}

void GhostReplayLapStart(ShellContext& ctx, CarBody& body) { // 0x8003F990
    GhostSession& g = *ctx.ghost;
    uint8_t* snapshot = GhostSnapshot(g);
    const uint8_t* lap = ReferenceLap(g, true); // 0x80012304 with 0x800A951C set: the ring lap of car 0 + 0x20
    uint8_t* playback = Playback(g);
    PutAt<int16_t>(playback, kPlaybackPending, 0);
    PutAt<int16_t>(playback, kPlaybackLap, 0);
    PutAt<int16_t>(playback, kPlaybackBlend, 0);
    PutAt<int16_t>(playback, kPlaybackIndex, 0);
    const uint8_t contactType = body.contactType, carIndex = body.carIndex, controlClass = body.controlClass;
    RestoreCarState(body, lap + kGhostCarState, ctx.course->grid.count != 0);
    SavePose(playback, body);                  // pose (index 0)
    SavePose(playback + kGhostPoseSize, body); // pose (1 - index)
    body.contactType = contactType;
    body.carIndex = carIndex;
    body.controlClass = controlClass;
    snapshot[0] = 2;
    ResetGhostStream(g); // 0x800124B0
    if (body.lap == 0) {
        ctx.state->raceClock = 0;
        PutAt<int32_t>(playback, kPlaybackClockOffset, 0);
        body.lastLineTime = 0;
    } else {
        ctx.state->raceClock = 180000;
        const int8_t offset = int8_t(lap[3]);
        PutAt<int32_t>(playback, kPlaybackClockOffset, offset);
        body.lastLineTime = int32_t((uint32_t(int32_t(offset) + 180000) - uint32_t(lap[2])) / 3);
    }
    snapshot[1] = 2;
}

bool GhostReplayInput(ShellContext& ctx, ReplayFrame& frame) { // 0x80013EF0 (0x800A951C set, game mode 6)
    GhostSession& g = *ctx.ghost;
    const int32_t step = g.replayLapStep; // car + 0x21
    g.replayLapStep = 0;
    const int32_t lap = g.playerStreamLap; // car + 0x20
    int32_t next = lap + step;
    if (next < 0) next = 0;
    else if (RingCount(g) <= next) next = RingCount(g) - 1;
    bool read = false;
    bool switchLap = next != lap;
    if (!switchLap) {
        bool ended = false;
        frame = ReplayFrame{};
        WithStream(PlayerStream(g), [&](ReplayStream& s) { // 0x80013C90 (playback): returns the stream's end flag
            s.Read(frame);
            ended = s.Ended();
        });
        read = true;
        if (ended) {
            switchLap = true;
            next = lap + 1;
        }
    }
    if (switchLap) {
        g.replayRestart = 1; // 0x8002F4B8
        if (next < RingCount(g)) { // 0x800132D0(car, lap): the lap's stream from its start
            g.playerStreamLap = int8_t(next);
            WithStream(PlayerStream(g), [](ReplayStream& s) { s.Init(true, kGhostStreamCapacity); });
        } else {
            g.replayEnded = true; // 0x800A8D68
        }
    }
    uint8_t* snapshot = PlayerSnapshot(g); // 0x8001236C + 2
    PutAt<uint16_t>(snapshot, 2, uint16_t(GetAt<uint16_t>(snapshot, 2) + 1));
    return read;
}

void GhostRaceEnd(ShellContext& ctx) { // 0x800153B8 (live mode 6): 0x800131AC, 0x80012570, 0x800125BC
    GhostSession& g = *ctx.ghost;
    if (ctx.globals->demoFlag != 0) return;
    WithStream(PlayerStream(g), [](ReplayStream& s) { s.End(false); }); // 0x800131AC for pad slot 2
    AdvanceRing(g);
    const int16_t count = RingCount(g);
    if (count > 3) { // the laps in recording order, the oldest first
        std::vector<uint8_t> ordered(size_t(count) * kGhostLapSize);
        int32_t index = RingIndex(g);
        for (int16_t i = 0; i < count; i++) {
            std::memcpy(ordered.data() + size_t(i) * kGhostLapSize, RingLap(g, index), kGhostLapSize);
            if (++index > 3) index = 0;
        }
        for (int32_t i = 0; i < 4; i++) std::memcpy(RingLap(g, i), ordered.data() + size_t(i) * kGhostLapSize, kGhostLapSize);
        PutAt<int16_t>(g.ring.data(), 2, 0);
    }
    if (g.newBest != 0) {
        g.savedBest = 1;
        g.entryHasLap = true; // entry 0 (+ 0x8C = 1) copied over the ghost's entry, kind 2
        g.ghostStreamLap = g.playerStreamLap; // car record 0 (0x800A9688..0x800AA1C8) copied over car 1: + 0x1C
    }
}

// ================================================================ driver

void RaceShell::Setup(const ShellGlobals& globals, const ShellCourse& course, const RaceShellOptions& options, Car* cars, CarContactState* contact) {
    globals_ = globals;
    globals_.lapCount = options.lapCount;
    globals_.countdownEnabled = options.countdown ? globals.countdownEnabled : 0;
    globals_.pointsByPosition = options.pointsByPosition;
    globals_.splitLabels = options.splitLabels;
    globals_.lapTimeLabel = options.lapTimeLabel;
    globals_.license = options.license;
    course_ = course;
    course_.pointToPoint = options.pointToPoint;
    course_.pointToPointById = options.pointToPoint;
    if (course_.startLineCount > 3) throw std::runtime_error("race shell: the sector board holds at most 3 sector lines");
    if (int32_t(course_.startLines.size()) != course_.startLineCount + 1) throw std::runtime_error("race shell: the start-line table needs its trailing word");
    state_ = RaceShellState{};
    state_.courseRecord.time = -1;
    if (options.hasCourseRecord) state_.courseRecord = options.courseRecord; // *(0x800A9524) (mode 6 compares with it)
    ctx_.state = &state_;
    ctx_.globals = &globals_;
    ctx_.course = &course_;
    ctx_.cars = cars;
    ctx_.contact = contact;
    ctx_.allowOutOfRangeGap = false;
    ctx_.ghost = globals_.gameMode == 6 ? options.ghost : nullptr;
    ctx_.courseRecordTarget = options.courseRecordTarget;
    if (globals_.gameMode == 6 && ctx_.ghost == nullptr) throw std::runtime_error("race shell: a game mode 6 race needs its ghost session");
    if (globals_.demoFlag == 0) {            // 0x8001523C (race load): the players' result records, not in the attract race
        InitPlayerResults(state_.results[0]);
        InitPlayerResults(state_.results[1]);
    }
    InitRaceState(ctx_);                     // 0x8003C3F4 (race load)
    StartRace(ctx_, options.introFields);    // 0x8001584C (race start)
    // RaceShellState's defaults are the race init 0x8005E624 (0x801D5DE8.. zero, licence result -1).
}

void RaceShell::LicenseChecks(CoursePlacementQueries& course) { // 0x8003EBF0: after 0x8003D168, mode 3 only
    if (globals_.gameMode != 3) return;
    for (int car = 0; car < int(globals_.carCount); car++) LicenseCheck(ctx_, ctx_.cars[car].body, course);
}

} // namespace gt2::sim
