#include "game/sim/car_contact.h"

#include "game/sim/fixed.h"
#include "game/sim/trig.h"

namespace gt2::sim {

namespace {
// low <= value <= high (0x80040454).
bool InRange(int32_t low, int32_t value, int32_t high) { return low <= value && value <= high; }
// Half width as the original computes it from the s16: truncation towards zero.
int32_t HalfWidth(int16_t width) { const int32_t w = width; return (w - (w >> 31)) >> 1; }
} // namespace

void UpdateCarProximity(CarContactState& state, Car* cars, int count) {
    state.buffer = 1 - state.buffer;
    const int buffer = state.buffer;
    for (int i = 0; i < count; i++) {
        const CarBody& self = cars[i].body;
        if (!TakesPartInContact(self)) continue;
        const int32_t s = Sin(uint32_t(uint16_t(self.heading))), c = Cos(uint32_t(uint16_t(self.heading)));
        int slot = 0;
        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            const CarBody& other = cars[j].body;
            if (TakesPartInContact(other)) {
                const int32_t dz = int32_t(uint32_t(other.position[2]) - uint32_t(self.position[2]));
                // -0x2000 < dz <= 0x2000 (2 m of height difference), written as the original's sign test
                if (int32_t((0u - uint32_t(dz) - 0x2000u) ^ (0x2000u - uint32_t(dz))) < 1) {
                    for (int k = 0; k < 4; k++) {
                        ContactCorner& record = state.corners[i][slot][k];
                        const int32_t relX = int32_t(uint32_t(other.footprint[other.footprintSet][k][0]) - uint32_t(self.position[0]));
                        const int32_t relY = int32_t(uint32_t(other.footprint[other.footprintSet][k][1]) - uint32_t(self.position[1]));
                        const bool nearX = int32_t((0u - uint32_t(relX) - 0x10000u) ^ (0xFFFFu - uint32_t(relX))) < 1; // -0x10000 < v <= 0xFFFF
                        const bool nearY = int32_t((0u - uint32_t(relY) - 0x10000u) ^ (0xFFFFu - uint32_t(relY))) < 1;
                        if (!nearX || !nearY) {
                            record.edgeFlags[buffer] = 0xF;
                            continue;
                        }
                        const int32_t across = Dot2Shift12(c, relX, s, relY, 0);
                        const int32_t along = Dot2Shift12(c, relY, -s, relX, 0);
                        record.x[buffer] = across;
                        record.y[buffer] = along;
                        const int32_t half = HalfWidth(self.width);
                        uint8_t flags = uint8_t((self.frontExtent < along) << 3);
                        if (along < -self.rearExtent) flags |= 4;
                        if (across < -half) flags |= 2;
                        if (half < across) flags |= 1;
                        record.edgeFlags[buffer] = flags;
                    }
                }
            }
            slot++;
        }
    }
}

int32_t SweepCarPair(const CarContactState& state, int car, int slot, const CarBody& body, int32_t& corner, int32_t& side) {
    const int current = state.buffer, previous = 1 - state.buffer;
    int32_t best = 0x1000;
    corner = -1;
    side = -1;
    for (int k = 0; k < 4; k++) {
        const ContactCorner& record = state.corners[car][slot][k];
        const uint8_t before = record.edgeFlags[previous];
        if (before == 0 || before == 0xF) continue;
        const uint8_t after = record.edgeFlags[current];
        if (after == 0xF || (before & after) != 0) continue;
        const uint8_t crossed = uint8_t(before ^ after);
        const int32_t half = HalfWidth(body.width), front = body.frontExtent, rear = body.rearExtent;
        const int32_t x0 = record.x[previous], y0 = record.y[previous], x1 = record.x[current], y1 = record.y[current];
        auto crossingAcross = [&](int32_t t) { return int32_t(uint32_t(x0) + uint32_t(Mul12(t, int32_t(uint32_t(x1) - uint32_t(x0))))); };
        auto crossingAlong = [&](int32_t t) { return int32_t(uint32_t(y0) + uint32_t(Mul12(t, int32_t(uint32_t(y1) - uint32_t(y0))))); };
        if (crossed & 8) { // front edge
            const int32_t t = Div12Shift(int32_t(uint32_t(front) - uint32_t(y0)), int32_t(uint32_t(y1) - uint32_t(y0)));
            if (t < best && InRange(-half, crossingAcross(t), half)) { side = 0; best = t; corner = k; }
        }
        if (crossed & 4) { // rear edge
            const int32_t t = Div12Shift(int32_t(0u - uint32_t(y0) - uint32_t(rear)), int32_t(uint32_t(y1) - uint32_t(y0)));
            if (t < best && InRange(-half, crossingAcross(t), half)) { side = 1; best = t; corner = k; }
        }
        if (crossed & 2) { // left edge
            const int32_t t = Div12Shift(int32_t(0u - uint32_t(x0) - uint32_t(half)), int32_t(uint32_t(x1) - uint32_t(x0)));
            if (t < best && InRange(-rear, crossingAlong(t), front)) { side = 2; best = t; corner = k; }
        }
        if (crossed & 1) { // right edge
            const int32_t t = Div12Shift(int32_t(uint32_t(half) - uint32_t(x0)), int32_t(uint32_t(x1) - uint32_t(x0)));
            if (t < best && InRange(-rear, crossingAlong(t), front)) { side = 3; best = t; corner = k; }
        }
    }
    return best;
}

void SweepCarPairs(CarContactState& state, Car* cars, int count) {
    for (int i = 0; i < count; i++) {
        const CarBody& self = cars[i].body;
        if (!TakesPartInContact(self)) continue;
        int slot = 0;
        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            if (TakesPartInContact(cars[j].body)) {
                int32_t corner = 0, side = 0;
                state.fraction[i][slot] = int16_t(SweepCarPair(state, i, slot, self, corner, side));
                state.corner[i][slot] = uint8_t(corner);
                state.side[i][slot] = int8_t(side);
            }
            slot++;
        }
    }
}

void ResolveCarContacts(const MoveContext& context, const CarContactState& state, Car* cars, int count) {
    for (int i = 1; i < count; i++) {
        for (int j = 0; j < i; j++) {
            const int16_t fractionIJ = state.fraction[i][j], fractionJI = state.fraction[j][i - 1];
            if (fractionIJ == 0x1000 && fractionJI == 0x1000) continue;
            // The car whose edge was crossed earlier owns the contact ("A"); the other is "B".
            CarBody* a;
            CarBody* b;
            int8_t side;
            int32_t fraction;
            if (fractionJI < fractionIJ) {
                a = &cars[j].body; b = &cars[i].body; side = state.side[j][i - 1]; fraction = fractionJI;
            } else {
                a = &cars[i].body; b = &cars[j].body; side = state.side[i][j]; fraction = fractionIJ;
            }
            const int32_t remainingFraction = 0x1000 - fraction;
            const int32_t s = Sin(uint32_t(uint16_t(a->heading))), c = Cos(uint32_t(uint16_t(a->heading)));
            int32_t normal[3] = {0, 0, 0}; // edge normal of A in the simulation plane (a valid side always exists here)
            switch (side) {
            case 0: normal[0] = int16_t(-s); normal[1] = int16_t(c); break;  // front
            case 1: normal[0] = int16_t(s); normal[1] = int16_t(-c); break;  // rear
            case 2: normal[0] = int16_t(-c); normal[1] = int16_t(-s); break; // left
            case 3: normal[0] = int16_t(c); normal[1] = int16_t(s); break;   // right
            default: break;
            }
            int32_t closingA = 0, closingB = 0;
            for (int k = 0; k < 3; k++) {
                closingA = int32_t(uint32_t(closingA) + uint32_t(Mul12Wide(normal[k], a->velocity[k])));
                closingB = int32_t(uint32_t(closingB) - uint32_t(Mul12Wide(normal[k], b->velocity[k])));
            }
            // Momentum-weighted closing speed; the original weights with the s16 at body + 0x766.
            const int32_t weightA = a->timeScale, weightB = b->timeScale;
            const int32_t impulse = int32_t(uint64_t(Div64(int64_t(closingA) * weightA + int64_t(closingB) * weightB, int64_t(weightA) + weightB)));
            int32_t impulseVector[3];
            for (int k = 0; k < 3; k++) impulseVector[k] = Mul12Wide(normal[k], impulse);
            const int32_t magnitude = impulse < 0 ? int32_t(0u - uint32_t(impulse)) : impulse;
            const int16_t impact = int16_t(int32_t(uint32_t(magnitude) * 0xA4u) >> 12);
            a->wallImpact = int16_t(a->wallImpact + impact);
            b->wallImpact = int16_t(b->wallImpact + impact);
            a->contactFlags |= 2;
            b->contactFlags |= 2;
            for (int k = 0; k < 3; k++) {
                a->velocity[k] = int32_t(uint32_t(a->velocity[k]) - uint32_t(impulseVector[k]));
                b->velocity[k] = int32_t(uint32_t(b->velocity[k]) + uint32_t(impulseVector[k]));
            }
            // Separation move: the impulse over the remaining fraction of the step plus one 32nd of the normal.
            int16_t separation[3];
            for (int k = 0; k < 3; k++) {
                const int16_t travel = int16_t(Mul16(context.globals.frameTime, Mul12Wide(remainingFraction, impulseVector[k])));
                separation[k] = int16_t(travel + int16_t(Mul12ShiftWide(normal[k], 0x1000, 5)));
            }
            int32_t deltaA[4] = {-separation[0], -separation[1], -separation[2], 0};
            int32_t deltaB[4] = {separation[0], separation[1], separation[2], 0};
            int32_t remainingA = 0, remainingB = 0;
            const uint32_t hitA = MoveBody(*context.track, *a, deltaA, remainingA, 2, context.globals.rate, context.collisionDisabled);
            const uint32_t hitB = MoveBody(*context.track, *b, deltaB, remainingB, 2, context.globals.rate, context.collisionDisabled);
            if (hitA == 0) {
                if (hitB != 0)
                    for (int k = 0; k < 3; k++) a->position[k] = int32_t(uint32_t(a->position[k]) - uint32_t(Mul12Wide(remainingB, separation[k])));
            } else if (hitB == 0) {
                for (int k = 0; k < 3; k++) b->position[k] = int32_t(uint32_t(b->position[k]) + uint32_t(Mul12Wide(remainingA, separation[k])));
            }
        }
    }
}

} // namespace gt2::sim
