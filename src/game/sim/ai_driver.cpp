#include "game/sim/ai_driver.h"

#include <cstring>
#include <stdexcept>

#include "game/sim/fixed.h"
#include "game/sim/physics_core.h"
#include "game/sim/drive_shafts.h"
#include "game/sim/trig.h"
#include "game/sim/tyres.h"

namespace gt2::sim {

namespace {

// 32-bit wrapping arithmetic like the original's.
int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t Neg(int32_t v) { return int32_t(0u - uint32_t(v)); }
int32_t Abs(int32_t v) { return v < 0 ? Neg(v) : v; }
int32_t MulLo(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); } // low word of `mult`
int32_t Div64Low(int64_t n, int64_t d) { return int32_t(uint64_t(Div64(n, d))); }              // 0x80086084, low word

void SetWheelBrakes(CarBody& body, int32_t value) {
    for (uint32_t w = 0; w < 4; w++) body.wheels[w].brakeInput = int16_t(value);
}
// The original reads the type byte and the three bytes after it as one word here.
int32_t SectionTypeWord(const RaceSection& s) {
    int32_t word;
    std::memcpy(&word, &s, sizeof word);
    return word;
}
static_assert(offsetof(RaceSection, type) == 0);
int32_t Neighbour(const CarBody& body, uint32_t sector) { return body.neighbourFlags[sector]; }

// 0x800367CC: line 2 for a car on line 1 or 6, when list 2 exists.
int32_t LineTwoFrom(const AiCourseData& course, int32_t line) {
    if ((line == 1 || line == 6) && course.lines[2].sections != nullptr) return 2;
    return -1;
}
// 0x80036808: line 1 for a car on line 2 or 6, when list 1 exists.
int32_t LineOneFrom(const AiCourseData& course, int32_t line) {
    if ((line == 2 || line == 6) && course.lines[1].sections != nullptr) return 1;
    return -1;
}

// 0x80036980(body, 7): the race-state change to "finished" (jump-table case 7 only writes the state).
void SetFinished(CarBody& body) { body.raceState = 7; }

} // namespace

// ================================================================ callees

int32_t LookaheadDistance(const DriveClassTuning& tuning, int32_t speed) { // 0x80035874
    if (speed < 1) return tuning.lookaheadMin;
    const int32_t blend = Mul12Floor(tuning.lookaheadSpeedGain, speed);
    if (blend < 0x1000) return Add(tuning.lookaheadMin, Mul12Floor(blend, Sub(tuning.lookaheadMax, tuning.lookaheadMin)));
    return tuning.lookaheadMax;
}

int32_t FindLineSegment(const RaceSectionList& list, int32_t hint, int32_t distance, int32_t& next, int32_t& previous) { // 0x80035C48
    const int32_t count = list.count;
    int32_t result = 1;
    if (list.sections[0].distance <= distance) {
        if (distance < list.sections[count - 1].distance) {
            for (int32_t i = 0;; i++) { // outwards from the hint: hint, hint + 1, hint - 1, hint + 2, ...
                if (count < i) return 0;
                hint = Add(hint, (i & 1) ? i : Neg(i));
                int32_t s = hint;
                if (hint < 0) s = Add(hint, count);
                else if (count <= hint) s = Sub(hint, count);
                if (s != 0) {
                    int32_t j = s - 1;
                    if (j < 0) j = Add(j, count);
                    if (list.sections[j].distance <= distance && distance < list.sections[s].distance) {
                        previous = j;
                        next = s;
                        return 3;
                    }
                }
            }
        }
        result = 2;
    }
    previous = count - 1;
    next = 0;
    return result;
}

bool LinePoint(const AiCourseData& course, int32_t line, int32_t hint, int32_t distance, int32_t& x, int32_t& y) { // 0x80035D68
    const int32_t length = course.courseLength;
    const RaceSectionList& list = course.lines[line];
    if (distance < 0) distance = Add(distance, length);
    else if (length <= distance) distance = Sub(distance, length);
    int32_t next = 0, previous = 0;
    const int32_t kind = FindLineSegment(list, hint, distance, next, previous);
    int32_t along, span;
    const int32_t previousDistance = list.sections[previous].distance, nextDistance = list.sections[next].distance;
    if (kind == 2) { // past the last record: the wrap-around segment to the first one
        along = Sub(distance, previousDistance);
        span = Sub(Add(nextDistance, length), previousDistance);
    } else if (kind == 3) {
        along = Sub(distance, previousDistance);
        span = Sub(nextDistance, previousDistance);
    } else if (kind == 1) { // before the first record: the same segment, seen from the other side of the start line
        along = Sub(Add(distance, length), previousDistance);
        span = Sub(Add(nextDistance, length), previousDistance);
    } else {
        return false;
    }
    const int32_t fraction = Div64Low(int64_t(along) << 12, int64_t(span));
    const RaceSection& p = list.sections[previous];
    const RaceSection& n = list.sections[next];
    if (SectionTypeWord(p) == 0) { // straight
        x = Add(Mul12Wide(fraction, Sub(n.x, p.x)), p.x);
        y = Add(Mul12Wide(fraction, Sub(n.y, p.y)), p.y);
        return true;
    }
    // Arc: the centre is `radius` along the normal of the start heading; the point turns from the start angle
    // towards the angle of the next record's start point.
    int32_t radius = p.curvature;
    const uint32_t heading = uint32_t(p.heading) & 0xFFF;
    const int32_t centreX = Sub(p.x, Mul12Wide(Cos(heading), radius));
    const int32_t centreY = Add(p.y, Mul12Wide(Neg(Sin(heading)), radius));
    int32_t startAngle = Atan2(Sub(centreX, p.x), Sub(p.y, centreY));
    int32_t endAngle = Atan2(Sub(centreX, n.x), Sub(n.y, centreY));
    if (radius < 1) {
        radius = Neg(radius);
        if (startAngle < endAngle) startAngle = Add(startAngle, 0x1000);
    } else if (endAngle < startAngle) {
        endAngle = Add(endAngle, 0x1000);
    }
    const uint32_t angle = uint32_t(Add(startAngle, Mul12(fraction, Sub(endAngle, startAngle)))) & 0xFFF;
    x = Sub(centreX, Mul12Wide(Sin(angle), radius));
    y = Add(centreY, Mul12Wide(Cos(angle), radius));
    return true;
}

bool GridLinePoint(const AiCourseData& course, int32_t line, int32_t distance, int32_t& x, int32_t& y) { // 0x80036160
    const RaceSectionList& list = course.lines[line];
    const int32_t length = course.courseLength;
    const int32_t count = list.count;
    if (distance < 0) distance = Add(distance, length);
    else if (length <= distance) distance = Sub(distance, length);
    if (distance < Div(length, 2)) distance = Add(distance, length); // the grid straddles the start line
    if (distance < list.sections[0].distance) return false;
    if (!(distance < list.sections[count - 1].distance)) return false;
    int32_t a = 0, b = 0, fraction = 0;
    if (count > 0) {
        for (a = 0;;) {
            b = a + 1;
            const int32_t start = list.sections[a].distance;
            if (!(distance < start)) {
                const int32_t end = list.sections[b].distance;
                if (distance < end) {
                    fraction = Div64Low(int64_t(Sub(distance, start)) << 12, int64_t(Sub(end, start)));
                    break;
                }
            }
            if (!(b < count)) break;
            a = b;
        }
    }
    const int32_t ax = list.sections[a].x, ay = list.sections[a].y;
    x = Add(Mul12Wide(fraction, Sub(list.sections[b].x, ax)), ax);
    y = Add(Mul12Wide(fraction, Sub(list.sections[b].y, ay)), ay);
    return true;
}

int32_t LookaheadPoint(const AiCourseData& course, const CarBody& body, int32_t lookahead, int32_t& dx, int32_t& dy) { // 0x80037538
    const int32_t line = int32_t(body.aiLine), section = int32_t(body.aiSection);
    // The original keeps x / y in uninitialised stack slots: only a failing grid-line lookup (a car in line 4
    // outside the grid's span) leaves them unset, in which case it steers at stack garbage. We keep 0.
    int32_t x = 0, y = 0;
    dx = 0;
    dy = 0;
    for (int32_t i = 0; i < 10; i++) {
        const int32_t distance = Add(body.courseDistance, lookahead);
        if (line == 4) GridLinePoint(course, 4, distance, x, y);
        else LinePoint(course, line, section, distance, x, y);
        dx = Sub(x, body.position[0]);
        dy = Sub(y, body.position[1]);
        if (ApproxLength(dx, dy) > 0x4FFF) break;
        if (i < 9) lookahead = Add(lookahead, 0x10000);
    }
    return lookahead;
}

int32_t ThrottleForSpeed(int32_t speed, int32_t reference, int32_t base, int32_t gain) { // 0x80037420
    int32_t error = Sub(speed, reference);
    if (error < -0x2C74) error = -0x2C74;
    else if (error > 0x2C74) error = 0x2C74;
    int32_t throttle = Add(base, Mul12(gain, error));
    if (throttle > 0x1000) throttle = 0x1000;
    else if (throttle < 0) throttle = 0;
    return throttle;
}

void HoldSpeed(CarBody& body, int32_t target, int32_t gain) { // 0x80037494
    const int32_t throttle = ThrottleForSpeed(body.forwardSpeed, Sub(target, 0x8E4), 0xC00, gain);
    body.effectiveThrottle = int16_t(throttle);
    if (throttle == 0 && target < body.forwardSpeed) SetWheelBrakes(body, 0x1000);
    if (throttle != 0x1000 && throttle != 0) body.shiftTimer = uint8_t(body.shiftTimer + 1);
}

void EaseSteering(CarBody& body, int32_t target, int32_t stepTime) { // 0x80037664
    const int32_t step = MulLo(body.steerMaxRate, stepTime) >> 16;
    const int32_t current = body.steerAngle;
    if (current < target) {
        const int32_t next = Add(int32_t(uint16_t(body.steerAngle)), step); // the original adds the unsigned halfword
        body.steerAngle = int16_t(next);
        if (int16_t(next) > target) body.steerAngle = int16_t(target);
    } else {
        const int32_t next = Sub(int32_t(uint16_t(body.steerAngle)), step);
        if (current <= target) return;
        body.steerAngle = int16_t(next);
        if (int16_t(next) < target) body.steerAngle = int16_t(target);
    }
}

void StopCar(CarBody& body, int32_t steer, int32_t stepTime) { // 0x800376D8
    EaseSteering(body, steer, stepTime);
    body.throttle = 0;
    body.effectiveThrottle = 0;
    int32_t yaw = body.yawRate;
    yaw = (yaw < 0 ? Add(yaw, 127) : yaw) >> 7;
    int32_t roll = body.rollRate;
    roll = (roll < 0 ? roll + 15 : roll) >> 4;
    int32_t pitch = body.pitchRate;
    pitch = (pitch < 0 ? pitch + 15 : pitch) >> 4;
    const int32_t speed = ApproxLength3(body.velocity[0], body.velocity[1], body.velocity[2]);
    const int32_t rotation = ApproxLength3(yaw, roll, pitch);
    if (speed < 284 && rotation < 34) {
        body.brake = 0;
        SetWheelBrakes(body, 0);
        SetFinished(body);
    } else {
        body.brake = 0x1000;
        SetWheelBrakes(body, 0x1000);
    }
}

int32_t AdvanceLine(const AiContext& context, CarBody& body, int32_t& situation) { // 0x8003643C
    situation = 0;
    int32_t scale = 0x1000;
    const uint32_t line = body.aiLine;
    if (line == 4) return 0x1000;
    const RaceSectionList& list = context.course.lines[line];
    const int32_t count = list.count;
    const int32_t distance = body.courseDistance;
    if (uint32_t(body.aiSection) == uint32_t(count) && distance < Div(context.course.courseLength, 2)) {
        body.aiSection = 0; // the course distance wrapped: back to the first section
        AdvanceSection(body, list, context.classTuning, context.wear);
    } else {
        const uint32_t section = body.aiSection;
        if (int32_t(section) < count && list.sections[section].distance <= distance) {
            body.aiSection = uint8_t(section + 1);
            AdvanceSection(body, list, context.classTuning, context.wear);
        }
    }
    const DriveClassTuning& tuning = context.classTuning[body.driveClass];
    const bool braking = (uint32_t(body.aiPreviousType) - 3u) < 2u; // 0x78B is 3 or 4
    bool tail = true;                                                // reach the common tail (the class throttle scale)
    if (!braking) {
        const int32_t speed = body.forwardSpeed, target = body.aiTargetSpeed;
        if (body.aiSectionType != 0 && speed > target) {
            const int32_t ratio = Mul12Floor(Add(speed, target), Sub(speed, target)); // ~ (v^2 - vt^2)
            int32_t grip;
            if (context.wear.wearLimit == 0 || (body.flags78D & 0x10)) {
                grip = int32_t(uint32_t(uint16_t(body.inverseGripWeight)) << 16) >> 17; // the original reads the halfword unsigned
            } else {
                int32_t wearSum = 0;
                for (uint32_t w = 0; w < 4; w++) wearSum = Add(wearSum, int32_t(uint16_t(body.wheels[w].wearGrip)));
                grip = MulLo(int32_t(int16_t(wearSum)), body.inverseGripWeight) >> 15;
            }
            // The section the index names; an index equal to the count reads past the list in the original, which
            // never happens with the game's lists (their last section is a straight, so 0x78C is 0 here).
            const uint32_t sectionIndex = body.aiSection;
            const RaceSection& section = list.sections[sectionIndex < uint32_t(count) ? sectionIndex : uint32_t(count - 1)];
            // The original indexes body + 0x358 by the signed section surface (s8 * 2): a negative surface would read
            // the halfwords below the table; the index arithmetic is kept as it is.
            const int32_t surfaceTerm = MulLo(grip, body.inverseSurfaceGrip[int32_t(section.surface)]) >> 8;
            int32_t factor = body.brakeBalanceSpeed;
            if (factor < surfaceTerm) factor = surfaceTerm;
            int32_t brakingDistance = Mul12Floor(ratio, MulLo(Sub(0x1000, Sin(uint32_t(section.gradient))), factor) >> 12);
            if (body.neighbourClass != 0) brakingDistance = Add(brakingDistance, MulLo(body.neighbourClass, 0x14000) >> 12);
            const int32_t remaining = ApproxLength(Sub(section.x, body.position[0]), Sub(section.y, body.position[1]));
            if (brakingDistance < remaining) {
                if (tuning.brakeMarginFactor > 0x1000) {
                    const int32_t margin = Mul12Floor(brakingDistance, tuning.brakeMarginFactor - 0x1000);
                    const int32_t gap = Sub(remaining, brakingDistance);
                    if (gap < Add(margin, 0x32000)) situation = 1;
                    if (gap < margin) scale = Div12Shift(gap, margin);
                }
            } else {
                body.aiPreviousType = uint8_t(body.aiPreviousType == 0 ? 3 : 4);
                tail = false;
            }
        }
    } else {
        tail = false;
    }
    if (!tail) situation = 2;
    if (body.aiLine != 3) scale = MulLo(scale, tuning.throttleScale) >> 12;
    return scale;
}

bool InLineSwapZone(const AiCourseData& course, int32_t section, int32_t distance) { // 0x800360C8
    const RaceSectionList& zones = course.lines[5];
    if (zones.sections == nullptr) return false;
    if (zones.count < section) section = 0;
    int32_t next = 0, previous = 0;
    if (FindLineSegment(zones, section, distance, next, previous) == 0) return false;
    return SectionTypeWord(zones.sections[previous]) != 0;
}

int32_t OvertakingLineA(const AiCourseData& course, int32_t line, int32_t section, int32_t distance) { // 0x80036844
    return InLineSwapZone(course, section, distance) ? LineOneFrom(course, line) : LineTwoFrom(course, line);
}

int32_t OvertakingLineB(const AiCourseData& course, int32_t line, int32_t section, int32_t distance) { // 0x80036890
    return InLineSwapZone(course, section, distance) ? LineTwoFrom(course, line) : LineOneFrom(course, line);
}

int32_t PickLine(const AiCourseData& course, uint32_t requested) { // 0x800358E0
    if (requested >= 7) requested = 6;
    for (uint32_t i = 0; i < 5; i++) {
        const int32_t candidate = course.raceStateTable[requested * 5 + i];
        if (candidate < 0) return -1; // the original reads the race object's word at +4 (the list count, 7) and returns -1
        if (course.lines[candidate].sections != nullptr) return candidate;
    }
    return -1;
}

int32_t LineStartPlacement(const AiCourseData& course, bool pointToPoint, const int32_t* startLines, int32_t startLineCount, int32_t offset, int32_t& chunk,
                           int32_t& x, int32_t& y, int32_t& dx, int32_t& dy) { // 0x80039040
    const int32_t length = course.courseLength;
    const int32_t line = PickLine(course, 6);
    if (line == -1) return 0;
    const RaceSectionList& list = course.lines[line];
    const int32_t count = list.count;
    if (count <= 0 || length <= 0) throw std::runtime_error("line start placement: no main line records / no course length");
    constexpr int32_t kTwentyMetres = int32_t(0xFFEC0000u); // -20 m
    int32_t d = int32_t(uint32_t(offset) << 16);
    if (pointToPoint) {
        const int32_t low = Sub(list.sections[count - 1].distance, length);
        if (d < low) d = low;
        if (startLineCount < 1) throw std::runtime_error("line start placement: no sector lines");
        const int32_t high = Add(startLines[startLineCount - 1], kTwentyMetres);
        if (high < d) d = high;
    } else {
        const int32_t high = Add(length, kTwentyMetres);
        if (d < Neg(high)) d = Neg(high);
        else if (high < d) d = high;
    }
    while (d < 0) d = Add(d, length);
    while (d >= length) d = Sub(d, length);
    auto chunkOf = [](const RaceSection& s) { return int32_t(int16_t(uint16_t(s.reserved10[0] | s.reserved10[1] << 8))); };
    int32_t section = count - 1;
    int32_t hint = chunkOf(list.sections[count - 1]);
    for (int32_t i = 0; i < count; i++)
        if (list.sections[i].distance >= d) {
            hint = chunkOf(list.sections[i]);
            section = i;
            break;
        }
    int32_t x0 = x, y0 = y, x1 = Add(x, dx), y1 = Add(y, dy); // the stack words LinePoint leaves unwritten (ours: the given values)
    LinePoint(course, line, section, d, x0, y0);
    LinePoint(course, line, section, Add(d, 0xA0000), x1, y1);
    chunk = hint;
    x = x0;
    y = y0;
    dx = Sub(x1, x0);
    dy = Sub(y1, y0);
    return offset;
}

int32_t HomeLine(const AiCourseData& course, uint32_t line) { // 0x800368DC
    // The delay slot of the call passes the constant 6, not the car's line: the home line is the first candidate
    // of row 6 of the race-state table (line 6 itself when its list exists).
    return line == 6 ? -1 : PickLine(course, 6);
}

void SwitchLine(const AiContext& context, CarBody& body, uint8_t line) { // 0x800367AC
    InitRaceProgress(body, line, context.course.lines, context.classTuning, context.wear);
}

// ================================================================ the line follower

namespace {

// The line-change decision of a car on a racing line (the tail of 0x80037834 for lines other than 3): a car ahead
// (neighbour class set) is passed on a free side; otherwise the throttle is eased by the neighbour class, a car
// with no neighbour at all returns to its home line, and a car beside another moves to the other side's line.
void ConsiderLineChange(const AiContext& context, CarBody& body, int32_t situation) {
    const AiCourseData& course = context.course;
    const int32_t line = int32_t(body.aiLine), section = int32_t(body.aiSection), distance = body.courseDistance;
    if (body.neighbourClass != 0) {
        int32_t sideA = -1, sideB = -1;
        if (Neighbour(body, 1) == 0) sideA = OvertakingLineA(course, line, section, distance); // ahead-left free
        if (Neighbour(body, 2) == 0) sideB = OvertakingLineB(course, line, section, distance); // ahead-right free
        const uint32_t type = body.aiSectionType;
        if (type == 1) {
            if (sideA >= 0) { SwitchLine(context, body, uint8_t(sideA)); return; }
        } else if (type == 2 && sideB >= 0) {
            SwitchLine(context, body, uint8_t(sideB));
            return;
        } else if (sideA >= 0 && situation != 3) {
            SwitchLine(context, body, uint8_t(sideA));
            return;
        }
        if (sideB >= 0 && situation != 4) { SwitchLine(context, body, uint8_t(sideB)); return; }
    }
    body.effectiveThrottle = int16_t(MulLo(Sub(0x1000, body.neighbourClass), body.effectiveThrottle) >> 12);
    const int32_t left = Neighbour(body, 3), right = Neighbour(body, 4);
    const int32_t occupied = Add(Add(Add(Add(Neighbour(body, 0), Neighbour(body, 1)), Neighbour(body, 2)), left), right);
    if (occupied == 0) {
        const int32_t home = HomeLine(course, body.aiLine);
        if (home != -1) SwitchLine(context, body, uint8_t(home));
        return;
    }
    if (!(left != 0 || right != 0)) return;
    if (left != 0 && right != 0) return;
    if (left != 0 && situation != 4) {
        const int32_t target = OvertakingLineB(course, line, section, distance);
        if (target != -1) { SwitchLine(context, body, uint8_t(target)); return; }
    }
    if (right == 0 || situation == 3) return;
    const int32_t target = OvertakingLineA(course, line, section, distance);
    if (target != -1) SwitchLine(context, body, uint8_t(target));
}

} // namespace

void AiDrive(const AiContext& context, CarBody& body, GearRequest& request, int32_t stepTime) { // 0x80037834
    if (body.raceState == 6) { StopCar(body, 0, stepTime); return; }
    int32_t motion = 0;    // sign of the forward speed beyond 2 km/h
    int32_t mode = 0;      // 1 drive forward, -1 reverse, 0 brake
    int32_t brakeFull = 0; // brake with the pedal at full while turning around
    int32_t steer = 0;
    int32_t overshoot = 0; // steering demand / window (12.12) when the demand exceeded the window
    int32_t situation = 0;
    const DriveClassTuning& tuning = context.classTuning[body.driveClass];
    const int32_t throttleScale = AdvanceLine(context, body, situation);
    const int32_t speed = body.forwardSpeed;
    if (body.aiPreviousType == 0 && context.raceClock < 9000u) { // the start straight in the first 3 s: no steering
        mode = 1;
    } else {
        const uint32_t headingUnits = uint32_t(uint16_t(body.heading)) & 0xFFFu;
        const int32_t headingNegSin = Neg(Sin(headingUnits)), headingCos = Cos(headingUnits);
        const uint32_t raceState = body.raceState;
        uint32_t recovery = body.aiRecovery;
        int32_t lookahead;
        if (raceState == 3) lookahead = 0x50000;
        else if (raceState == 4 && body.aiSlowZone != 0) lookahead = 0x140000;
        else if (recovery < 3) lookahead = LookaheadDistance(tuning, speed);
        else lookahead = recovery == 3 ? 0xA0000 : 0x280000;
        int32_t dx = 0, dy = 0;
        LookaheadPoint(context.course, body, lookahead, dx, dy);
        const int32_t error = AngleDifference(Atan2(Neg(dx), dy), body.heading);
        const int32_t absError = Abs(error);
        // Recovery state machine from the heading error and the wall hits.
        const uint32_t hits = body.wallHitMask;
        if (recovery == 3) {
            if (absError < 0x71) body.aiRecovery = uint8_t(speed > 0x855C ? 0u : 4u);
            else if (hits & 0xC) body.aiRecovery = 4;
        } else if (recovery == 4) {
            if (absError < 0x1C7 && speed > 0x855C) body.aiRecovery = 0;
            else if ((hits & 3) != 0 && (hits == 3 || absError > 0x2AA)) body.aiRecovery = 3;
        } else if (absError < 0x38F) {
            if (hits == 3) body.aiRecovery = 3;
        } else {
            body.aiRecovery = 4;
        }
        if (speed >= 0x8E5) motion = 1;
        else if (speed < -0x8E4) motion = -1;
        recovery = body.aiRecovery;
        int32_t threshold = 0x2AA; // heading error beyond which the car reverses instead of driving on
        if (recovery == 3) {
            if (speed < 0) {
                if (speed > -0x855C) threshold = Sub(0x71, Div(MulLo(speed, 0x238), 0x855C));
            } else {
                threshold = 0x71;
            }
        }
        if (absError < threshold) {
            if (speed < -0x8E3) brakeFull = 1;
            else mode = 1;
        } else if (speed > 0x8E4) {
            brakeFull = 1;
        } else {
            mode = -1;
        }
        if (motion != 0) {
            if (mode == 1) {
                // Yaw demand: the lateral offset of the lookahead point in the direction of travel (the heading
                // below 5 km/h) over its squared distance, times the speed.
                const int32_t vx = body.velocity[0], vy = body.velocity[1];
                const int32_t travel = SquareRoot(int32_t(uint64_t(int64_t(vx) * vx + int64_t(vy) * vy) >> 12), 6);
                const int32_t distanceSq = int32_t(uint64_t(int64_t(dx) * dx + int64_t(dy) * dy) >> 12);
                int32_t cosTravel = headingCos, negSinTravel = headingNegSin;
                if (travel > 0x1639) {
                    const uint32_t direction = uint32_t(Atan2(Neg(vx), vy)) & 0xFFF;
                    cosTravel = Cos(direction);
                    negSinTravel = Neg(Sin(direction));
                }
                int32_t across = Add(MulLo(cosTravel, dx), MulLo(Neg(negSinTravel), dy));
                if (across < 0) across = Add(across, 0xFFF);
                across >>= 12;
                const int32_t curvature = Mul12Wide(Div64Low(int64_t(travel) * int64_t(across), int64_t(distanceSq)), 0x518);
                const int32_t demand = speed < 0 ? curvature : Neg(curvature);
                const int32_t yawRate = body.yawRate;
                const int32_t yaw = yawRate < 0 ? Neg(Neg(yawRate) >> 7) : yawRate >> 7;
                const int32_t window = body.peakSlipAngle; // the AI's steering window
                steer = 0;
                if (demand > 0) {
                    if (yaw < demand) {
                        steer = Mul12(Sub(demand, yaw), tuning.steerGain);
                        if (window < steer) {
                            overshoot = Div(int32_t(uint32_t(steer) << 12), window);
                            if (!context.course.dirtCourse) steer = window;
                        }
                    }
                } else if (demand < 0 && demand < yaw) {
                    const int32_t magnitude = MulLo(Sub(yaw, demand), tuning.steerGain) >> 12;
                    steer = Neg(magnitude);
                    if (steer < Neg(window)) {
                        overshoot = Div(int32_t(uint32_t(magnitude) << 12), window);
                        if (!context.course.dirtCourse) steer = Neg(window);
                    }
                }
                const int32_t slip = Add(body.wheels[0].slipAngle, body.wheels[1].slipAngle);
                steer = Add(steer, Div(slip, 2));
                if (steer > 0x200) steer = 0x200;
                else if (steer < -0x200) steer = -0x200;
            } else {
                if (error < -0xE4) steer = -0x155;
                else if (error < 0xE4) steer = Mul12(error, 0x1809);
                else steer = 0x155;
            }
            if (motion == -1) steer = Neg(steer);
        }
    }
    if (body.raceState == 5) { StopCar(body, steer, stepTime); return; }
    if (int8_t(body.controlClass) == 2 || body.raceState != 0) EaseSteering(body, steer, stepTime);
    body.effectiveThrottle = 0;
    SetWheelBrakes(body, 0);
    body.throttle = 0;
    body.brake = 0;
    if (mode != 1) {
        if (mode == -1) {
            request.reverse = 1;
            body.effectiveThrottle = 0x1000;
        } else {
            request.reverse = 0;
            body.effectiveThrottle = 0;
        }
        SetWheelBrakes(body, brakeFull << 12);
        body.throttle = body.effectiveThrottle;
        body.brake = body.wheels[0].brakeInput;
    } else {
        // Target speed: the section speeds, or fixed limits on the grid / formation lines.
        bool cruising = false;
        body.aiSlowZone = 0;
        int32_t speedPrevious = 0, speedCurrent = 0;
        uint32_t control;
        const uint32_t line = body.aiLine;
        if (line == 4) {
            speedPrevious = speedCurrent = 0x58E8;
            control = 3;
            body.aiSlowZone = 1;
        } else if (line == 3) {
            const RaceSectionList& grid = context.course.lines[4];
            const int32_t length = context.course.courseLength;
            int32_t distance = body.courseDistance;
            if (distance < Div(length, 2)) distance = Add(distance, length);
            if (distance < grid.sections[1].distance) {
                speedPrevious = body.aiPreviousTargetSpeed < 0x1C56C ? body.aiPreviousTargetSpeed : 0x1C56C;
                speedCurrent = body.aiTargetSpeed < 0x1C56C ? body.aiTargetSpeed : 0x1C56C;
                control = body.aiPreviousType;
            } else if (distance < grid.sections[2].distance) {
                speedPrevious = speedCurrent = 0x15F2E;
                control = 3;
            } else if (distance < grid.sections[grid.count - 3].distance) {
                speedPrevious = speedCurrent = 0x15F2E;
                control = 1;
                body.aiSlowZone = 1;
            } else {
                speedPrevious = body.aiPreviousTargetSpeed;
                speedCurrent = body.aiTargetSpeed;
                control = body.aiPreviousType;
            }
        } else {
            cruising = true;
            speedPrevious = body.aiPreviousTargetSpeed;
            speedCurrent = body.aiTargetSpeed;
            control = body.aiPreviousType;
        }
        switch (control) {
        case 0: body.effectiveThrottle = 0x1000; break;
        case 1:
        case 2:
            HoldSpeed(body, speedPrevious, -0x800);
            if (situation == 0) situation = body.effectiveThrottle == 0x1000 ? (control == 1 ? 4 : 3) : 5;
            break;
        case 3: HoldSpeed(body, speedCurrent, -0x2000); break;
        case 4: HoldSpeed(body, speedCurrent, -0x1000); break;
        default: break;
        }
        body.effectiveThrottle = int16_t(MulLo(throttleScale, body.effectiveThrottle) >> 12);
        if (line == 3) {
            if ((Neighbour(body, 3) != 0 && Neighbour(body, 1) != 0) || (Neighbour(body, 4) != 0 && Neighbour(body, 2) != 0))
                body.effectiveThrottle = int16_t(Div(body.effectiveThrottle, 2));
        } else {
            if (situation > 0 && (situation < 3 || situation == 5)) cruising = false;
            if (cruising) ConsiderLineChange(context, body, situation);
        }
        body.brake = body.wheels[0].brakeInput;
        body.throttle = body.effectiveThrottle;
        if (int8_t(body.controlClass) == 2 || body.raceState != 0) request.reverse = 0;
    }
    if (overshoot != 0 && body.gear > 1) { // steering demand beyond the window: ease the throttle, add brake
        int32_t keep = Sub(0x1000, MulLo(tuning.overshootThrottleCut, Sub(overshoot, 0x1000)) >> 12);
        if (keep < 0) keep = 0;
        const int32_t throttle = MulLo(keep, body.effectiveThrottle) >> 12;
        body.effectiveThrottle = int16_t(throttle);
        body.throttle = int16_t(throttle);
        // The original reads the wheel's brake input as an unsigned halfword here.
        int32_t brake = Add(int32_t(uint16_t(body.wheels[0].brakeInput)), MulLo(tuning.overshootBrake, Sub(overshoot, 0x1000)) >> 12);
        if (int16_t(brake) > 0x1000) brake = 0x1000;
        SetWheelBrakes(body, brake);
        body.brake = int16_t(brake);
    }
}

void AiFinished(CarBody& body, int32_t stepTime) { // 0x800377E8
    EaseSteering(body, 0, stepTime);
    body.throttle = 0;
    body.brake = 0;
    body.effectiveThrottle = 0;
    SetWheelBrakes(body, 0);
}

void AiScripted(CarBody& body, int32_t framesLeft, int32_t rate) { // 0x800372EC
    body.clutchRequest = 0;
    const int32_t rpm = body.engineRpm;
    const int32_t limit = int32_t(body.revLimitRpm), upshift = int32_t(body.upshiftRpm);
    if (Div(rate * 2, 5) < framesLeft) { // waiting: blip the throttle between the two rpm marks
        if (body.aiScriptThrottleOn == 0) {
            if (rpm < Sub(upshift * 2, limit)) body.aiScriptThrottleOn = 1;
        } else if (!(rpm < ((limit + upshift) >> 1))) {
            body.aiScriptThrottleOn = 0;
        }
        body.throttle = int16_t(body.aiScriptThrottleOn != 0 ? 0x1000 : 0);
    } else { // the last 2/5 s: throttle by how far the revs are above the upshift mark
        const int32_t throttle = Sub(2560, Div(int32_t(uint32_t(Sub(rpm, upshift)) << 13), limit));
        body.throttle = int16_t(throttle);
        if (int16_t(throttle) > 0x1000) body.throttle = 0x1000;
        else if (int16_t(throttle) < 0) body.throttle = 0;
    }
    body.brake = 0;
    body.effectiveThrottle = body.throttle; // the original copies the halfword (read unsigned, stored as 16 bits)
    SetWheelBrakes(body, 0);
    body.steerAngle = 0;
}

void RunAiDriver(const AiContext& context, CarBody& body, GearRequest& request, const AiDispatch& dispatch, int32_t stepTime) {
    switch (dispatch.control) {
    case AiControl::Drive: AiDrive(context, body, request, stepTime); break;
    case AiControl::Finished: AiFinished(body, stepTime); break;
    case AiControl::Scripted: AiScripted(body, dispatch.argument, context.rate); break;
    case AiControl::None: break;
    }
}

} // namespace gt2::sim
