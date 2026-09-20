#include "game/sim/car_body.h"

#include "game/sim/fixed.h"
#include "game/sim/track_collision.h"
#include "game/sim/trig.h"

namespace gt2::sim {

void UpdateFootprint(CarBody& body) {
    // The original indexes the table at 0x80093150 for `s` and 0x80093950 for `c`; gt2verify establishes
    // which of our generated tables each one equals.
    const int32_t s = Sin(uint32_t(uint16_t(body.heading))), c = Cos(uint32_t(uint16_t(body.heading)));
    const uint32_t frontS = uint32_t(Mul12(body.frontExtent, s)), frontC = uint32_t(Mul12(body.frontExtent, c));
    const uint32_t rearS = uint32_t(Mul12(body.rearExtent, s)), rearC = uint32_t(Mul12(body.rearExtent, c));
    const uint32_t halfWidthC = uint32_t(Mul12Shift(body.width, c, 1)), halfWidthS = uint32_t(Mul12Shift(body.width, s, 1));
    const uint32_t x = uint32_t(body.position[0]), y = uint32_t(body.position[1]);

    auto& corner = body.footprint[body.footprintSet]; // unsigned arithmetic: wraps exactly like the original
    corner[0][0] = int32_t((x - halfWidthC) - frontS);
    corner[0][1] = int32_t((y - halfWidthS) + frontC);
    corner[1][0] = int32_t((x + halfWidthC) - frontS);
    corner[1][1] = int32_t((y + halfWidthS) + frontC);
    corner[2][0] = int32_t((x - halfWidthC) + rearS);
    corner[2][1] = int32_t((y - halfWidthS) - rearC);
    corner[3][0] = int32_t((x + halfWidthC) + rearS);
    corner[3][1] = int32_t((y + halfWidthS) - rearC);
}

uint32_t CheckWalls(const Track& track, CarBody& body, int32_t& fraction, uint32_t& corner) {
    const uint32_t set = body.footprintSet, previous = 1u - set;
    SweepSegment segments[4]{};
    for (int i = 0; i < 4; i++) {
        segments[i].x0 = int32_t(uint32_t(body.footprint[previous][i][0]) << 4);
        segments[i].y0 = int32_t(uint32_t(body.footprint[previous][i][1]) << 4);
        segments[i].x1 = int32_t(uint32_t(body.footprint[set][i][0]) << 4);
        segments[i].y1 = int32_t(uint32_t(body.footprint[set][i][1]) << 4);
    }
    SweepAgainstCourse(track, uint32_t(body.chunkIndex), segments);

    uint32_t mask = 0, best = 0;
    int32_t earliest = 0x1000;
    for (uint32_t i = 0; i < 4; i++) {
        if (segments[i].fraction == 0x1000) continue;
        mask |= 1u << i;
        if (segments[i].fraction < earliest) { earliest = segments[i].fraction; best = i; }
    }
    corner = best;
    fraction = earliest;
    if (mask) {
        body.hitNormal0 = segments[best].hitNormal0;
        body.hitNormal1 = segments[best].hitNormal1;
    }
    return mask;
}

void UpdateScrapeDirection(CarBody& body, int32_t rate) {
    const int32_t n0 = body.hitNormal0, n1 = body.hitNormal1;
    const uint32_t set = body.footprintSet, corner = body.hitCorner;
    const int32_t dx = int32_t(uint32_t(body.footprint[set][corner][0]) - uint32_t(body.footprint[1u - set][corner][0]));
    const int32_t dy = int32_t(uint32_t(body.footprint[set][corner][1]) - uint32_t(body.footprint[1u - set][corner][1]));
    if (Dot2Shift12(-n1, dx, n0, dy, 0) > 0) return; // moving away from the wall
    const int32_t along = Dot2Shift12(n0, dx, n1, dy, 0);
    const int32_t scaled = int32_t(uint32_t(along) * uint32_t(rate));
    const int32_t threshold = Div(4096, rate);
    int32_t direction = scaled < 0 ? (scaled + 2047) >> 11 : scaled >> 11;
    if (direction > 64) direction = 64;
    else if (direction < -64) direction = -64;
    body.scrapeDirection = int8_t((threshold < along || along < -threshold) ? direction : 0);
}

uint32_t MoveBody(const Track& track, CarBody& body, const int32_t delta[4], int32_t& remaining, int mode, int32_t rate,
                  bool collisionDisabled) {
    auto wrapHeading = [](int32_t value) {
        int16_t h = int16_t(value);
        if (h > 0xFFF) h = int16_t(h - 0x1000);
        if (h < -0xFFF) h = int16_t(h + 0x1000);
        return h;
    };
    for (int i = 0; i < 3; i++) body.position[i] = int32_t(uint32_t(body.position[i]) + uint32_t(delta[i]));
    body.heading = wrapHeading(int16_t(body.heading + int16_t(delta[3])));
    if (mode != 1) body.footprintSet = uint8_t(1 - body.footprintSet);
    UpdateFootprint(body);
    remaining = 0;
    if (collisionDisabled) return 0;

    int32_t fraction = 0;
    uint32_t corner = 0;
    const uint32_t mask = CheckWalls(track, body, fraction, corner);
    if (mask == 0) return 0;
    body.hitCorner = uint8_t(corner);
    // Disassembly (0x80033F70): mode 0 clears the value and then still runs the update; modes 1-2 update only.
    if (mode == 0) body.scrapeDirection = 0;
    if (mode >= 0 && mode <= 2) UpdateScrapeDirection(body, rate);

    remaining = 0x1000 - fraction;
    for (int i = 0; i < 3; i++) body.position[i] = int32_t(uint32_t(body.position[i]) - uint32_t(Mul12(delta[i], remaining)));
    body.heading = wrapHeading(int32_t(uint16_t(body.heading)) - Mul12(delta[3], remaining));
    body.position[0] = int32_t(uint32_t(body.position[0]) - uint32_t(int32_t(body.hitNormal1) >> 4));
    body.position[1] = int32_t(uint32_t(body.position[1]) + uint32_t(int32_t(body.hitNormal0) >> 4));
    if (mode != 0) UpdateFootprint(body);
    return mask;
}

void SetStepTime(CarBody& body, const StepGlobals& globals) {
    if (body.timeScale == 0x1000) {
        body.stepTime = int16_t(globals.frameTime);
        body.stepRate12 = int32_t(uint32_t(globals.rate) << 12);
        return;
    }
    body.stepTime = int16_t(int32_t(uint32_t(globals.frameTime) * uint32_t(body.timeScale)) >> 12);
    body.stepRate12 = Div(int32_t(uint32_t(globals.rate) << 24), body.timeScale);
}

void UpdateAero(CarBody& body, const StepGlobals& globals) {
    const int32_t dt = body.stepTime;
    const int32_t speed = body.forwardSpeed < 0 ? int32_t(0u - uint32_t(body.forwardSpeed)) : body.forwardSpeed;
    if (body.draftInput == 0) {
        const int32_t decay = (dt < 0 ? dt + 0x3F : dt) >> 6; // dt / 64 towards zero
        const int32_t blend = int32_t(uint32_t(uint16_t(body.draftBlend)) - uint32_t(decay));
        body.draftBlend = int16_t(blend);
        if (int16_t(blend) < 0) body.draftBlend = 0;
    } else {
        const int32_t blend = int32_t(uint32_t(uint16_t(body.draftBlend)) + uint32_t(int32_t(uint32_t(body.draftInput) * uint32_t(dt)) >> 16));
        body.draftBlend = int16_t(blend);
        if (int16_t(blend) > 0x1000) body.draftBlend = 0x1000;
    }
    const int32_t floor = globals.draftDragFloor;
    const int32_t draftFactor = int32_t(uint32_t(floor) + uint32_t(int32_t(uint32_t(0x1000 - floor) * uint32_t(0x1000 - body.draftBlend)) >> 12));
    const int32_t speedTerm = int32_t(uint32_t(speed >> 5) * uint32_t(speed >> 5)) >> 12;
    const int32_t factor = int32_t(uint32_t(draftFactor) * uint32_t(speedTerm)) >> 12;
    int16_t drag = int16_t(int32_t(uint32_t(body.dragCoefficient) * uint32_t(factor)) >> 12);
    if (body.forwardSpeed >= 0) drag = int16_t(-drag);
    body.dragForce = drag;
    for (int i = 0; i < 2; i++) body.downforceForce[i] = int32_t(uint32_t(body.downforce[i]) * uint32_t(factor)) >> 12;
}

void ComputeDisplacement(const CarBody& body, int32_t delta[4]) {
    const int32_t dt = body.stepTime;
    for (int i = 0; i < 3; i++) delta[i] = Mul16(body.velocity[i], dt);
    delta[3] = Mul16ShiftWide(body.yawRate, dt, 7);
}

void MapPadInput(CarBody& body, const uint16_t pad[4]) {
    const int16_t steer = int16_t(pad[1]);
    if ((pad[0] & 1) == 0) {
        if (steer == 0) body.steerInput = 0;
        else body.steerInput = steer < 1 ? int8_t(-100) : int8_t(100);
    } else {
        body.steerInput = int8_t((int32_t(steer) * 0x19) >> 10);
    }
    if ((pad[0] & 2) == 0) {
        if (pad[3] == 0) body.inputFlag6FD = 0;
    } else if (pad[3] != 0x1000) {
        body.inputFlag6FD = 0;
    }
}

int32_t ApproxLength(int32_t a, int32_t b) {
    if (a < 0) a = int32_t(0u - uint32_t(a));
    if (b < 0) b = int32_t(0u - uint32_t(b));
    const int32_t smaller = a < b ? a : b;
    return int32_t(uint32_t(a) + uint32_t(b) - uint32_t(smaller >> 1));
}

void RespondToWall(const MoveContext& context, CarBody& body, int32_t delta[4], int32_t remaining, int mode) {
    const int32_t oldX = body.velocity[0], oldY = body.velocity[1];
    const int32_t n0 = body.hitNormal0, n1 = body.hitNormal1;
    const int32_t alongNormal = Dot2Shift12(oldX, n0, oldY, n1, 0);
    body.velocity[0] = Mul12Wide(n0, alongNormal);
    body.velocity[1] = Mul12Wide(n1, alongNormal);
    const int32_t oldSpeed = ApproxLength(oldX, oldY);
    if (body.velocity[2] > 0x1000 && oldSpeed > 0) body.velocity[2] = Mul12Wide(body.velocity[2], Div12Shift(alongNormal, oldSpeed, 0));
    if (mode == 1) return;

    const int32_t removed = ApproxLength(int32_t(uint32_t(oldX) - uint32_t(body.velocity[0])), int32_t(uint32_t(oldY) - uint32_t(body.velocity[1])));
    body.wallImpact = 0;
    // Constants of the original at 0x80046E20: impact gain 41 / 27, threshold units 120 / 60 and damping percent
    // 60 / 30 for tarmac / dirt courses.
    const int32_t impactGain = context.dirtCourse ? 27 : 41;
    const int32_t thresholdUnits = context.dirtCourse ? 60 : 120, dampingPercent = context.dirtCourse ? 30 : 60;
    if (oldSpeed >= 2277) body.wallImpact = int16_t(int32_t(uint32_t(removed) * uint32_t(impactGain)) >> 12);
    const int32_t threshold = thresholdUnits * 1138;
    const int32_t strength = threshold < removed ? 0x1000 : Div12Shift(removed, threshold, 0); // min(1.0, removed / threshold)
    if (body.wallScrape < strength) {
        body.wallScrape = int16_t(strength);
        const int32_t keep = 0x1000 - int32_t(uint32_t(100 - dampingPercent) * uint32_t(strength)) / 100;
        for (int i = 0; i < 3; i++) body.velocity[i] = Mul12Wide(keep, body.velocity[i]);
    }
    ScaleDisplacement(body, delta, remaining);
    int32_t unused = 0;
    MoveBody(*context.track, body, delta, unused, 1, context.globals.rate, context.collisionDisabled);
}

void ScaleDisplacement(const CarBody& body, int32_t delta[4], int32_t remaining) {
    const int32_t dt = body.stepTime;
    for (int i = 0; i < 3; i++) delta[i] = Mul16(Mul12(remaining, body.velocity[i]), dt);
    delta[3] = Mul16ShiftWide(Mul12Wide(remaining, body.yawRate), dt, 7);
}

void MovePass(const MoveContext& context, Car* cars, int count, int32_t (*deltas)[4]) {
    const int32_t scrapeDecay = Mul16(context.globals.frameTime, 0x555);
    for (int i = 0; i < count; i++) {
        CarBody& body = cars[i].body;
        int32_t remaining = 0;
        const uint32_t mask = MoveBody(*context.track, body, deltas[i], remaining, 0, context.globals.rate, context.collisionDisabled);
        body.wallHitMask = uint8_t(mask);
        if (mask == 0) {
            const int32_t scrape = int32_t(uint32_t(uint16_t(body.wallScrape)) - uint32_t(scrapeDecay));
            body.wallScrape = int16_t(scrape);
            if (int16_t(scrape) < 0) body.wallScrape = 0;
            body.wallImpact = 0;
            continue;
        }
        body.contactFlags |= 1;
        RespondToWall(context, body, deltas[i], remaining, 0);
        if (context.controlClass == 2 && body.wallImpact - 0x155 >= 0) {
            Wheel& wheel = body.wheels[body.hitCorner];
            int32_t damage = ((body.wallImpact - 0x155) >> 3) + wheel.damage;
            if (damage > 0xFF) damage = 0xFF;
            wheel.damage = uint8_t(damage);
        }
    }
}

int32_t Falloff(int32_t value, int32_t gain, int32_t threshold) {
    if (value < 0) value = int32_t(0u - uint32_t(value));
    const int32_t beyond = int32_t(uint32_t(value) - uint32_t(threshold));
    if (beyond < 0) return 0x1000;
    const int32_t drop = int32_t(uint32_t(beyond) * uint32_t(gain)) >> 12;
    if (drop < 0x1001) return 0x1000 - drop;
    return 0;
}

} // namespace gt2::sim
