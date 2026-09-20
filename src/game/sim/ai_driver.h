#pragma once
#include <cstdint>

#include "game/sim/car_body.h"
#include "game/sim/car_setup.h"
#include "game/sim/drivetrain.h"

// The AI drivers: the three routines 0x80038540 (drivetrain.h PrepareAiInput) hands over to and their callees.
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_ai.cpp).
//
//   0x80037834  AiDrive     the racing-line follower: section bookkeeping, lookahead point, steering, target speed
//   0x800377E8  AiFinished  the car has finished (body + 0x786 == 7): steer to centre, no pedals
//   0x800372EC  AiScripted  the start script (revs the engine, then feeds the throttle in over the last 2/5 s)
//
// Course data of the AI (AiCourseData): the game shell's race object (0x801C8568) holds seven "line" lists
// (car_setup.h RaceSection: one record per section with its start point, radius, heading and course distance),
// the course length (chunk table + 0) and the course table's dirt flag (0x80060E94(course) + 8 & 4). The AI
// follows a line by interpolating between consecutive records (a straight linearly, an arc around its centre)
// at its course distance plus a speed-dependent lookahead, and steers towards that point.
//
// AI state in the body (offsets relative to the body, car + 0x2C):
//   body + 0x1D   u8   drive class: indexes the tuning records at 0x801C8690 (car_setup.h DriveClassTuning)
//   body + 0x769  u8   AiScripted: throttle toggle (revs between (limit + upshift) / 2 and 2 * upshift - limit)
//   body + 0x76A  s16  neighbour class of the car ahead (0x400 / 0x800 / 0x1000, physics_core.h UpdateCarPairAwareness)
//   body + 0x76C  s8[8] neighbour sectors: [0] ahead, [1] ahead-left, [2] ahead-right, [3] left, [4] right, ...
//   body + 0x784  u8   slow zone: 1 on the grid / formation line, selects the 20 m lookahead in race state 4
//   body + 0x785  u8   wall-hit corner mask of the last move (3 = both front corners)
//   body + 0x786  u8   race state: 0 racing, 3 / 4 shell states with fixed lookaheads, 5 / 6 stop the car, 7 finished
//   body + 0x788  u8   recovery: 0 normal, 3 reversing away from a wall, 4 turning back towards the line
//   body + 0x789  u8   line (which list is followed): 6 main, 1 / 2 overtaking lines, 3 pit / formation, 4 grid slots
//   body + 0x78A  u8   section index in the line (may equal the count until the course distance wraps)
//   body + 0x78B  u8   previous section type, or 3 / 4 while braking for the current section (0x8003643C)
//   body + 0x78C  u8   current section type (0 straight, 1 / 2 corner)
//   body + 0x78D  u8   bit 4: tyre wear ignored in the grip estimate
//   body + 0x790  s32  target speed of the previous section, 1/4096 m/s     body + 0x794  s32  ... of the current one
// Outputs: steering (0x60C through the rate limit 0x80037664), throttle 0x610 = effective throttle 0x708, brake
// 0x612 = the four wheel brake inputs (wheel + 0x60), the shift timer 0x61C (delayed shifts under partial
// throttle), the gear request's reverse byte (0x1F800364 + car * 4), race state 0x786 = 7 when stopped, and the
// line bookkeeping above. No random numbers are used.
namespace gt2::sim {

// The race object's lists and the course facts the AI reads (game data, owned by the caller).
struct AiCourseData {
    RaceSectionList lines[7];              // 0x801C8568 -> +8 + line * 4 (count 0 / null = no list)
    int32_t courseLength = 0;              // course object chunk table + 0, 16.16 m
    bool dirtCourse = false;               // course table entry + 8 & 4: no steering clamp at the window
    const int8_t* raceStateTable = nullptr; // 0x80046DD4: 7 rows of 5 candidate lines (0x800358E0)
};

struct AiContext {
    AiCourseData course;
    const DriveClassTuning* classTuning = nullptr; // 0x801C8690, 4 records
    TyreWearConstants wear;                // 0x80046F48..: wearLimit != 0 enables the wear-scaled grip estimate
    uint32_t raceClock = 0;                // 0x80046F64: the shell's race clock in 1/3000 s (0x8003D168 adds 100 per 30 Hz frame
                                           // while the cars are not held); below 9000 (the first 3 s) a car on a straight does not steer
    int32_t rate = 30;                     // 0x801C8570 (AiScripted)
};

// ---- entry points ----
// 0x80037834(body, car). `stepTime` is the scratchpad's word 0 (= body + 0x6FE, 16.16 s), read by the steering rate limit.
void AiDrive(const AiContext& context, CarBody& body, GearRequest& request, int32_t stepTime);
// 0x800377E8(body).
void AiFinished(CarBody& body, int32_t stepTime);
// 0x800372EC(body, car, framesLeft): `framesLeft` is the dispatch argument of PrepareAiInput.
void AiScripted(CarBody& body, int32_t framesLeft, int32_t rate);
// Runs the routine PrepareAiInput selected (nothing for AiControl::None).
void RunAiDriver(const AiContext& context, CarBody& body, GearRequest& request, const AiDispatch& dispatch, int32_t stepTime);

// ---- callees (exposed for the verifier) ----
// 0x80035874: lookahead distance (16.16 m) for `speed`: min at rest, blending to max by speed * lookaheadSpeedGain.
int32_t LookaheadDistance(const DriveClassTuning& tuning, int32_t speed);
// 0x80035C48: the segment of `list` containing `distance`: 3 = between `previous` and `next` (searched outwards
// from `hint`), 1 = before the first record, 2 = past the last (both: previous = last, next = 0), 0 = not found.
int32_t FindLineSegment(const RaceSectionList& list, int32_t hint, int32_t distance, int32_t& next, int32_t& previous);
// 0x80035D68: the point of `line` at `distance` (wrapped by the course length), simulation plane 1/4096 m.
bool LinePoint(const AiCourseData& course, int32_t line, int32_t hint, int32_t distance, int32_t& x, int32_t& y);
// 0x80036160: the same for the grid list (line 4): linear between the slots, false outside them (`x` / `y` untouched).
bool GridLinePoint(const AiCourseData& course, int32_t line, int32_t distance, int32_t& x, int32_t& y);
// 0x80037538: the line point `lookahead` (16.16 m) ahead of the car, pushed out by 1 m steps (at most 9) until it
// is more than 5 m away; returns the lookahead used, `dx` / `dy` = point - position.
int32_t LookaheadPoint(const AiCourseData& course, const CarBody& body, int32_t lookahead, int32_t& dx, int32_t& dy);
// 0x80037420: throttle 0..0x1000 = base + gain * clamp(speed - reference, +-0x2C74) (12.12).
int32_t ThrottleForSpeed(int32_t speed, int32_t reference, int32_t base, int32_t gain);
// 0x80037494: effective throttle from ThrottleForSpeed(speed, target - 0x8E4, 0xC00, gain); full brakes when it is 0
// and the car is faster than the target; the shift timer advances under partial throttle.
void HoldSpeed(CarBody& body, int32_t target, int32_t gain);
// 0x80037664: steering angle (body + 0x60C) towards `target` at the car's steering rate (body + 0x62 per second).
void EaseSteering(CarBody& body, int32_t target, int32_t stepTime);
// 0x800376D8: steer to `steer`, no throttle, full brakes until the car is (nearly) at rest and not rotating, then
// no brakes and race state 7 (0x80036980(body, 7)).
void StopCar(CarBody& body, int32_t steer, int32_t stepTime);
// 0x8003643C: advances the section index when the car passes the section start, and returns the throttle scale
// (0x1000 = full) of the braking model: while the car is faster than the current section's target speed the
// braking distance is estimated from the grip and the section's gradient; when the section start is closer than
// that the "braking" types 3 / 4 replace body + 0x78B. `situation` (out) = 0 free, 1 braking zone approaching
// (within 50 m of the margin), 2 braking.
int32_t AdvanceLine(const AiContext& context, CarBody& body, int32_t& situation);
// 0x800360C8: type != 0 of the line-5 record at `distance` (false without line 5).
bool InLineSwapZone(const AiCourseData& course, int32_t section, int32_t distance);
// 0x80036844 / 0x80036890: the overtaking line on one side of the car (1 or 2, swapped inside a line-5 zone), or -1
// when the car's line does not allow it (line 6 -> 1 or 2, 1 -> 2, 2 -> 1) or the list is missing.
int32_t OvertakingLineA(const AiCourseData& course, int32_t line, int32_t section, int32_t distance);
int32_t OvertakingLineB(const AiCourseData& course, int32_t line, int32_t section, int32_t distance);
// 0x800358E0: the first candidate line of the race-state table row `requested` that has a list, or -1 (a -1 candidate
// ends the search: the original then tests the race object's word at +4, which is non-zero).
int32_t PickLine(const AiCourseData& course, uint32_t requested);
// 0x80039040 (Arcade 0x80038FEC): a start position `offset` metres along the main line (the 2 player Battle's Handicap Start:
// 0x80012CD4 passes it as the 13th argument of 0x80033384). The line = PickLine(6) (none: returns 0, nothing written); the
// distance offset << 16 is clamped - a circuit to +-(length - 20 m), a point-to-point course to [last record's distance - length,
// last sector line - 20 m] - and wrapped into [0, length); the section = the first record at or beyond it (else the last one),
// whose s16 + 0x10 is the chunk hint; the point = LinePoint(line, section, distance) and the heading vector = the point 10 m
// further minus it (LinePoint's outputs are left as they were when it finds no segment). Returns `offset`.
int32_t LineStartPlacement(const AiCourseData& course, bool pointToPoint, const int32_t* startLines, int32_t startLineCount, int32_t offset, int32_t& chunk,
                           int32_t& x, int32_t& y, int32_t& dx, int32_t& dy);
// 0x800368DC: the line to return to when no car is around: PickLine(6) (the main line when its list exists), or -1
// when the car is already on line 6.
int32_t HomeLine(const AiCourseData& course, uint32_t line);
// 0x800367AC: switches the car to `line` and re-derives its section bookkeeping (car_setup.h InitRaceProgress).
void SwitchLine(const AiContext& context, CarBody& body, uint8_t line);

} // namespace gt2::sim
