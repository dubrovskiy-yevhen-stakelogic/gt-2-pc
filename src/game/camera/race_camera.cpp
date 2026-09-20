#include "game/camera/race_camera.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "game/sim/fixed.h"
#include "game/sim/ground.h"
#include "game/sim/trig.h"
#include "game/sim/tyres.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/track.h"

namespace gt2::camera {

namespace {

using Rot = int16_t[3][3];

int16_t Sat16(int64_t v) { return int16_t(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }
int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t MulLo(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); } // mult + mflo
// Low word of the 64-bit product shifted right (the srl lo / sll hi pairs of the original).
int32_t MulShift(int32_t a, int32_t b, uint32_t shift) { return int32_t(uint32_t(uint64_t(int64_t(a) * int64_t(b)) >> shift)); }

// GTE LZCS / LZCR: the number of leading bits equal to the sign bit (32 for 0).
int32_t LeadingSignBits(int32_t v) {
    uint32_t u = v < 0 ? ~uint32_t(v) : uint32_t(v);
    int32_t n = 0;
    while (n < 32 && !(u & 0x80000000u)) {
        u <<= 1;
        n++;
    }
    return n;
}
int32_t Abs(int32_t v) { return v < 0 ? int32_t(0u - uint32_t(v)) : v; }
int32_t ShiftBy(int32_t v, int32_t shift) { // srav for shift >= 0, sllv by -shift otherwise (5-bit amounts)
    return shift >= 0 ? v >> (shift & 31) : int32_t(uint32_t(v) << ((-shift) & 31));
}

// 0x8007B994(out, a, b): out = a * b through MVMVA (sf = 1, lm = 0), column by column.
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

// 0x8008220C(out, m, v, t): out = m v / 4096 + t, v split into 12-bit slices: low (MVMVA sf 1 -> IR), high (v >> 24,
// sf 0 -> IR, << 12), middle (sf 0 -> MAC).
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

// 0x8007AF60: identity rotation, zero translation (the pad word too).
void Identity(Matrix& m) {
    std::memset(&m, 0, sizeof m);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
}
// 0x8007B050: move along the matrix's own axes (t += m v).
void Translate(Matrix& m, int32_t x, int32_t y, int32_t z) {
    const int32_t v[3] = {x, y, z};
    ApplyLong(m.t, m.m, v, m.t);
}
// 0x8007B088: m = m * R(yaw, pitch, roll).
void Rotate(Matrix& m, int32_t yaw, int32_t pitch, int32_t roll) {
    int16_t r[3][3];
    RotationFromAngles(r, yaw, pitch, roll);
    MulMatrix(m.m, m.m, r);
}
// 0x8007B14C: m = m * Ry(angle), Ry = [[c, 0, s], [0, 1, 0], [-s, 0, c]].
void RotateY(Matrix& m, int32_t angle) {
    const int16_t s = int16_t(sim::Sin(uint32_t(angle))), c = int16_t(sim::Cos(uint32_t(angle)));
    const int16_t r[3][3] = {{c, 0, s}, {0, 4096, 0}, {int16_t(-s), 0, c}};
    MulMatrix(m.m, m.m, r);
}

// 0x800105E0: a + (b - a) t / 4096 (64-bit product, low word).
int32_t Lerp12(int32_t a, int32_t b, int32_t t) { return Add(a, MulShift(Sub(b, a), t, 12)); }
// 0x80010598: a + (b - a) n / d (64-bit product, the runtime's 64-bit division 0x80086084).
int32_t LerpDiv(int32_t a, int32_t b, int32_t n, int32_t d) {
    return Add(a, int32_t(uint32_t(uint64_t(sim::Div64(int64_t(Sub(b, a)) * n, d)))));
}
// 0x80011088: a + (b - a) t / 4096 per component.
void LerpVector(int32_t out[3], const int32_t a[3], const int32_t b[3], int32_t t) {
    for (int i = 0; i < 3; i++) out[i] = Add(a[i], MulShift(Sub(b[i], a[i]), t, 12));
}
// 0x80011144: |v| of the vector scaled down by 1024.
int32_t Length10(const int32_t v[3]) {
    int32_t s = 0;
    for (int i = 0; i < 3; i++) s = Add(s, MulLo(v[i] >> 10, v[i] >> 10));
    return sim::SquareRoot(s, 0);
}

// The arc sine table of the executable (0x800A2AC4, s16[4096]: round(asin(i / 4096) * 4096 / 2 pi); generated -
// gt2verify compares it with the executable's).
const std::array<int16_t, 4096>& AsinTable() {
    static const std::array<int16_t, 4096> table = [] {
        std::array<int16_t, 4096> t{};
        for (int i = 0; i < 4096; i++) t[size_t(i)] = int16_t(std::floor(std::asin(double(i) / 4096.0) * 4096.0 / (2.0 * 3.14159265358979323846) + 0.5));
        return t;
    }();
    return table;
}
// 0x80082DA8: arc sine of a 4096 = 1 value, 4096 units per turn.
int32_t Asin(int16_t x) {
    if (x < -4095) return -1024;
    if (x >= 4096) return 1024;
    if (x >= 0) return AsinTable()[size_t(x)];
    return int16_t(-AsinTable()[size_t(-x)]);
}

// 0x80082C58: scales v so that its largest component has 13 significant bits; returns the right shift applied.
int32_t Normalize13(int16_t out[3], const int32_t v[3]) {
    const int32_t shift = 19 - LeadingSignBits(Abs(v[0]) | Abs(v[1]) | Abs(v[2]));
    for (int i = 0; i < 3; i++) out[i] = int16_t(ShiftBy(v[i], shift));
    return shift;
}
// 0x80082CE0: unit vector (4096 = 1) of a short vector (SQR, square root, 0x1000000 / length, GPF); returns the length.
int32_t NormalizeShort(int16_t out[3], const int16_t v[3]) {
    if ((v[0] | v[1] | v[2]) == 0) {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        return 0;
    }
    const int32_t sum = int32_t(int64_t(v[0]) * v[0] + int64_t(v[1]) * v[1] + int64_t(v[2]) * v[2]);
    const int32_t length = sim::SquareRoot(sum, 0);
    const int16_t ir0 = int16_t(sim::Div(0x1000000, length));
    for (int i = 0; i < 3; i++) out[i] = Sat16((int64_t(ir0) * v[i]) >> 12);
    return length;
}

// The race's car the camera follows (the original indexes the six-slot car array without a check).
const CameraCar& CarAt(const CameraWorld& world, uint32_t index) {
    if (index >= world.cars.size()) throw std::out_of_range("race camera: car " + std::to_string(index) + " is not in the race");
    return world.cars[index];
}

uint32_t FindChunk(const CameraWorld& world, uint16_t hint, const int32_t point[3]) { // 0x80028394
    if (!world.track) throw std::logic_error("race camera: the trackside cameras need the course");
    return sim::FindChunkAlongCourse(*world.track, hint, point);
}

// 0x8007B320(camera, left, right, top, bottom, H, far): the window (far is not stored).
void LibraryWindow(RaceCamera& camera, int32_t left, int32_t right, int32_t top, int32_t bottom, int32_t H) {
    camera.centreX = int16_t(sim::Div(int32_t(uint32_t(Add(left, right) >> 1) << 12), H));
    camera.centreY = int16_t(sim::Div(int32_t(uint32_t(Add(bottom, top) >> 1) << 12), H));
    camera.spanX = int16_t(Sub(right, left));
    camera.spanY = int16_t(Sub(top, bottom));
    camera.H = int16_t(H);
}

// 0x800118F8: polyline path, segments { s32 length (of the stretch ending at this point), s32 point[3] } at + 4.
void PathPolyline(const ReplayCameraData& d, uint32_t path, int32_t progress, int32_t out[3]) {
    const int32_t count = d.U16(path + 2);
    int32_t total = 0;
    for (int32_t i = 0; i < count; i++) total = Add(total, d.S32(path + 4 + 16 * uint32_t(i)));
    if (total == 0 || count < 2) {
        for (uint32_t k = 0; k < 3; k++) out[k] = d.S32(path + 8 + 4 * k);
        return;
    }
    int32_t v = MulShift(total, progress, 12);
    int32_t segment = 0, length = total;
    const int32_t last = count - 1;
    if (last > 0)
        for (;;) {
            const int32_t next = segment + 1;
            length = d.S32(path + 16 * uint32_t(next) + 4);
            if (v < length) break;
            segment = next;
            v = Sub(v, length);
            if (!(next < last)) break;
        }
    int32_t f = length != 0 ? int32_t(uint32_t(uint64_t(sim::Div64(int64_t(v) << 12, length)))) : 0;
    if (f < 0) f = 0;
    else if (f >= 4096) f = 4096;
    int32_t a[3], b[3];
    for (uint32_t k = 0; k < 3; k++) {
        a[k] = d.S32(path + 16 * uint32_t(segment) + 8 + 4 * k);
        b[k] = d.S32(path + 16 * uint32_t(segment) + 24 + 4 * k);
    }
    LerpVector(out, a, b, f);
}

// 0x80011A58: cubic path, segments of 56 bytes at + 4: { s32 length, s32 point[3], u32 scale, s32 coefficients
// (t^3, t^2, t) of x, y, z }; point + scale * (a t^3 + b t^2 + c t) with t in 24-bit fixed point.
void PathCubic(const ReplayCameraData& d, uint32_t path, int32_t progress, int32_t out[3]) {
    const int32_t count = d.U16(path + 2);
    int32_t total = 0;
    for (int32_t i = 0; i < count; i++) total = Add(total, d.S32(path + 4 + 56 * uint32_t(i)));
    if (total == 0 || count < 2) {
        for (uint32_t k = 0; k < 3; k++) out[k] = d.S32(path + 8 + 4 * k);
        return;
    }
    int32_t v = MulShift(total, progress, 12);
    int32_t segment = 0, length = total;
    if (count != 0)
        do {
            length = d.S32(path + 56 * uint32_t(segment) + 4);
            if (v < length) break;
            v = Sub(v, length);
            segment++;
        } while (segment < count);
    int32_t f = length != 0 ? int32_t(uint32_t(uint64_t(sim::Div64(int64_t(v) << 12, length)))) : 0;
    if (f < 0) f = 0;
    else if (f >= 4096) f = 4096;
    const int32_t t1 = int32_t(uint32_t(f) << 12);
    const int32_t t2 = MulShift(t1, t1, 24), t3 = MulShift(t2, t1, 24);
    const uint32_t seg = path + 4 + 56 * uint32_t(segment);
    const uint32_t scale = uint32_t(d.S32(seg + 16));
    for (uint32_t k = 0; k < 3; k++) {
        const int64_t a = (int64_t(d.S32(seg + 20 + 12 * k)) * t3) >> 24;
        const int64_t b = (int64_t(d.S32(seg + 24 + 12 * k)) * t2) >> 24;
        const int64_t c = (int64_t(d.S32(seg + 28 + 12 * k)) * t1) >> 24;
        const uint64_t sum = uint64_t(a) + uint64_t(b) + uint64_t(c);
        const uint64_t product = uint64_t(scale) * sum;
        out[k] = Add(d.S32(seg + 4 + 4 * k), int32_t(uint32_t(product >> 24)));
    }
}

// 0x800111A8: trackside camera on a path looking at the car, H from H0 to H1 with the progress.
void TracksideLookAt(RaceCamera& camera, uint32_t record, const CameraWorld& world) {
    const ReplayCameraData& d = world.replayCameras;
    const CameraCar& car = CarAt(world, camera.target);
    const int32_t progress = *world.replayProgress;
    PathPosition(d, record + 0x18, progress, camera.view.t);
    camera.chunk = int32_t(FindChunk(world, d.U16(record + 6), camera.view.t));
    int32_t dir[3];
    for (int i = 0; i < 3; i++) dir[i] = Sub(car.pose.t[i], camera.view.t[i]);
    dir[1] = Add(dir[1], 0xCCCC);
    LookAlong(camera.view, dir);
    const int32_t h0 = d.U16(record + 0x10), h1 = d.U16(record + 0x12);
    SetProjection(camera, Add(MulLo(Sub(h1, h0), progress) >> 12, h0), 0, 0);
}

// 0x800112C4: fixed trackside camera (position, angles, H from the record).
void TracksideFixed(RaceCamera& camera, uint32_t record, const CameraWorld& world) {
    const ReplayCameraData& d = world.replayCameras;
    SetProjection(camera, d.S32(record + 0x10), 0, 0);
    camera.view.t[0] = d.S32(record + 0x14);
    camera.view.t[1] = d.S32(record + 0x18);
    camera.view.t[2] = Sub(0, d.S32(record + 0x1C));
    RotationFromAngles(camera.view.m, -int32_t(d.S16(record + 0x22)), -int32_t(d.S16(record + 0x20)), d.S16(record + 0x24));
    camera.chunk = int32_t(FindChunk(world, d.U16(record + 6), camera.view.t));
}

// 0x80011378: an onboard view chosen by the record (H1 = H, H0 = the view).
void TracksideOnboard(RaceCamera& camera, uint32_t record, const CameraWorld& world) {
    const ReplayCameraData& d = world.replayCameras;
    SetProjection(camera, d.U16(record + 0x12), 0, 0);
    OnboardCamera(camera, d.U16(record + 0x10), world);
}

// 0x80011568: path camera looking at the car, zoomed by the distance and the car's size (LOD 0 scale / size).
void TracksideZoom(RaceCamera& camera, uint32_t record, const CameraWorld& world) {
    const ReplayCameraData& d = world.replayCameras;
    const CameraCar& car = CarAt(world, camera.target);
    const int32_t progress = *world.replayProgress;
    PathPosition(d, record + 0x1C, progress, camera.view.t);
    camera.chunk = int32_t(FindChunk(world, d.U16(record + 6), camera.view.t));
    int32_t dir[3];
    for (int i = 0; i < 3; i++) dir[i] = Sub(car.pose.t[i], camera.view.t[i]);
    dir[1] = Add(dir[1], 0xCCCC);
    LookAlong(camera.view, dir);
    int32_t h = MulLo(Length10(dir), 13824);
    h = ShiftBy(h, int32_t(car.lodScale) - 16);
    h = sim::Div(h, int32_t(car.lodSize));
    h = MulLo(h, d.U16(record + 0x14)) >> 12;
    const int32_t hMax = d.U16(record + 0x10), hMin = d.U16(record + 0x12);
    if (hMax < h) h = hMax;
    else if (h < hMin) h = hMin;
    SetProjection(camera, h, 0, 0);
}

// 0x8001097C: the car at race position `position` (0 when none; mode 6 always 0).
uint8_t CarAtPosition(int32_t position, const CameraWorld& world) {
    if (world.gameMode == 6) return 0;
    const uint32_t count = uint8_t(world.cars.size());
    for (uint32_t i = 0; i < count; i++)
        if (int32_t(world.cars[i].racePosition) == position) return uint8_t(i);
    return 0;
}

} // namespace

// ================================================================ data

uint16_t ReplayCameraData::U16(uint32_t pointer) const {
    const uint32_t at = pointer - base;
    if (pointer < base || at > bytes.size() || bytes.size() - at < 2) throw std::out_of_range("replay cameras: read outside the data");
    return uint16_t(bytes[at] | (bytes[at + 1] << 8));
}

int32_t ReplayCameraData::S32(uint32_t pointer) const { return int32_t(uint32_t(U16(pointer)) | (uint32_t(U16(pointer + 2)) << 16)); }

std::vector<uint32_t> ReplayCameraData::Records() const {
    std::vector<uint32_t> out;
    if (!Valid()) return out;
    const uint32_t count = U16(list);
    for (uint32_t i = 0; i < count; i++) out.push_back(uint32_t(S32(list + 4 + 4 * i)));
    return out;
}

ReplayCameraData ReplayCamerasOfTro(std::span<const uint8_t> tro) {
    ReplayCameraData d;
    if (tro.size() < 0x20) throw std::runtime_error("replay cameras: short .tro");
    d.bytes = tro;
    d.base = 0;
    d.list = uint32_t(tro[0x1C]) | (uint32_t(tro[0x1D]) << 8) | (uint32_t(tro[0x1E]) << 16) | (uint32_t(tro[0x1F]) << 24);
    if (d.list != 0) (void)d.U16(d.list + 2); // the list header is in the file
    return d;
}

CameraConstants LoadCameraConstants(const GuestImage& overlay) {
    CameraConstants c;
    // Simulation addresses translated to the overlay's build (US Arcade v1.1: 0x54 lower; its onboard table differs in 8 bytes).
    const uint32_t chase = overlay.Sim(kChaseTableAddress), viewAngle = overlay.Sim(kViewAngleTableAddress), onboard = overlay.Sim(kOnboardTableAddress);
    for (uint32_t i = 0; i < c.chase.size(); i++) c.chase[i] = overlay.Get<int32_t>(chase + 4 * i);
    for (uint32_t i = 0; i < c.viewAngle.size(); i++) c.viewAngle[i] = overlay.Get<int16_t>(viewAngle + 2 * i);
    for (uint32_t i = 0; i < c.onboard.size(); i++) std::memcpy(&c.onboard[i], overlay.At(onboard + 24 * i, 24), 24);
    return c;
}

int32_t CarHalfWidth(const std::array<int16_t, 8>& bbox, int16_t scale) { // 0x80017E74
    int32_t a = bbox[0] < 0 ? -int32_t(bbox[0]) : bbox[0];
    int32_t w = bbox[4];
    if (w < a) w = a;
    w = ShiftBy(w, 16 - int32_t(uint16_t(scale))); // sllv by scale - 16 when >= 0, else srav
    return int32_t(uint32_t(w) << 4);
}

// ================================================================ library routines

int32_t ArcSine(int16_t x) { return Asin(x); }

void RotationFromAngles(int16_t out[3][3], int32_t yaw, int32_t pitch, int32_t roll) { // 0x80081374
    const int32_t sx = sim::Sin(uint32_t(yaw)), cx = sim::Cos(uint32_t(yaw));
    const int32_t sy = sim::Sin(uint32_t(pitch)), cy = sim::Cos(uint32_t(pitch));
    const int32_t sz = sim::Sin(uint32_t(roll)), cz = sim::Cos(uint32_t(roll));
    // The GTE rotation [[cx, -, r13], [0, 0, cy], [-sx, -, r33]] times (cz, 0, sz) and (-sz, 0, cz) (the middle column
    // holds sign bits of the packed words; it only meets VY = 0).
    const int32_t r13 = int16_t(MulLo(sx, -sy) >> 12), r33 = int16_t(MulLo(cx, -sy) >> 12);
    out[0][0] = Sat16((int64_t(cx) * cz + int64_t(r13) * sz) >> 12);
    out[1][0] = Sat16((int64_t(cy) * sz) >> 12);
    out[2][0] = Sat16((int64_t(-sx) * cz + int64_t(r33) * sz) >> 12);
    out[0][1] = Sat16((int64_t(cx) * -sz + int64_t(r13) * cz) >> 12);
    out[1][1] = Sat16((int64_t(cy) * cz) >> 12);
    out[2][1] = Sat16((int64_t(-sx) * -sz + int64_t(r33) * cz) >> 12);
    out[0][2] = int16_t(MulLo(sx, cy) >> 12);
    out[1][2] = int16_t(sy);
    out[2][2] = int16_t(MulLo(cx, cy) >> 12);
}

void AnglesFromRotation(int16_t out[3], const int16_t m[3][3]) { // 0x800811B0
    out[1] = int16_t(Asin(m[1][2]));
    // The original looks |m21| up in its cosine table (0x800A0A88, sqrt(1 - x^2), never 0 below 4096) and treats
    // 0 as the degenerate case: only |m21| >= 4096 is.
    int32_t r = m[2][1];
    if (r < 0) r = -r;
    if (!(r < 4096)) {
        out[2] = 0;
        out[0] = int16_t(sim::Atan2Narrow(int16_t(-m[0][1]), int16_t(-m[2][1])));
    } else {
        out[2] = int16_t(sim::Atan2Narrow(m[1][0], m[1][1]));
        out[0] = int16_t(sim::Atan2Narrow(m[0][2], m[2][2]));
    }
}

int32_t VectorLength(int16_t direction[3], const int32_t v[3]) { // 0x80081164
    int16_t scaled[3];
    const int32_t shift = Normalize13(scaled, v);
    const int32_t length = NormalizeShort(direction, scaled);
    return shift >= 0 ? int32_t(uint32_t(length) << (shift & 31)) : length >> ((-shift) & 31);
}

void LookAlong(Matrix& out, const int32_t d[3]) { // 0x80010E20
    const int32_t x = d[0], y = d[1], z = d[2];
    const int32_t ax = Abs(x), ay = Abs(y), az = Abs(z);
    int32_t shift = 31 - LeadingSignBits(ax | ay | az) - 14;
    int32_t sx = ShiftBy(x, shift), sy = ShiftBy(y, shift), sz = ShiftBy(z, shift);
    const int32_t xx = MulLo(sx, sx), yy = MulLo(sy, sy), zz = MulLo(sz, sz);
    int32_t sinPitch = 0, cosPitch = 4096;
    const int32_t r = sim::SquareRoot(Add(Add(xx, yy), zz), 0);
    if (r != 0) {
        const int32_t h = sim::SquareRoot(Add(xx, zz), 0);
        sinPitch = sim::Div(int32_t(uint32_t(sy) << 12), r);
        cosPitch = sim::Div(int32_t(uint32_t(h) << 12), r);
    }
    shift = 31 - LeadingSignBits(ax | az) - 14;
    sx = ShiftBy(x, shift);
    sz = ShiftBy(z, shift);
    int32_t sinYaw = 0, cosYaw = 4096;
    const int32_t r2 = sim::SquareRoot(Add(MulLo(sx, sx), MulLo(sz, sz)), 0);
    if (r2 != 0) {
        sinYaw = sim::Div(int32_t(uint32_t(sx) << 12), r2);
        cosYaw = sim::Div(int32_t(uint32_t(sz) << 12), r2);
    }
    const int32_t s3 = -sinYaw, s5 = -sinPitch, s7 = cosPitch, s0 = -cosYaw;
    out.m[0][0] = int16_t(s0);
    out.m[1][0] = 0;
    out.m[1][1] = int16_t(s7);
    out.m[1][2] = int16_t(s5);
    out.m[2][0] = int16_t(-s3);
    out.m[0][1] = int16_t(-(MulLo(s3, s5) >> 12));
    out.m[0][2] = int16_t(MulLo(s3, s7) >> 12);
    out.m[2][1] = int16_t(-(MulLo(s0, s5) >> 12));
    out.m[2][2] = int16_t(MulLo(s0, s7) >> 12);
}

void PathPosition(const ReplayCameraData& data, uint32_t path, int32_t progress, int32_t inout[3]) { // 0x80011890
    const uint16_t kind = data.U16(path);
    if (kind == 0) PathPolyline(data, path, progress, inout);
    else if (kind == 1) PathCubic(data, path, progress, inout);
    inout[2] = Sub(0, inout[2]);
}

uint32_t FindReplayCamera(const ReplayCameraData& d, int32_t lap, int32_t distance, int32_t courseLength) { // 0x800117C4
    const int32_t count = d.U16(d.list), modulus = d.U16(d.list + 2);
    if (modulus != 0 && modulus < lap) lap = (lap - 1) % modulus + 1;
    for (int32_t i = 0; i < count; i++) {
        const uint32_t record = uint32_t(d.S32(d.list + 4 + 4 * uint32_t(i)));
        const int32_t first = d.U16(record + 2), last = d.U16(record + 4);
        if (lap < first || last < lap) continue;
        int32_t end = d.S32(record + 0xC);
        const int32_t start = d.S32(record + 8);
        if (end < start) end = Add(end, courseLength);
        int32_t at = distance;
        if (first < lap) at = Add(at, courseLength);
        if (at < start) continue;
        if (at < end) return record;
    }
    return 0;
}

// ================================================================ the camera

void InitCamera(RaceCamera& camera, uint8_t car, uint8_t cameraPositionOption, uint8_t replayInfoOption, const CameraWorld& world) { // 0x80010000
    std::memset(&camera, 0, sizeof camera);
    camera.target = car;
    camera.replayInfo = 1;
    camera.position = cameraPositionOption;
    if (world.replay) camera.replayInfo = world.gameMode == 3 ? 2 : replayInfoOption;
}

void SetProjection(RaceCamera& camera, int32_t H, int32_t centreX, int32_t centreY) { // 0x80010088
    const int32_t split = camera.split != 0 ? 1 : 0;
    camera.rectX = 0;
    camera.rectY = 0;
    camera.rectW = 320;
    camera.rectH = int16_t(240 >> split);
    const int32_t half = 132 >> split, cy = centreY >> split;
    LibraryWindow(camera, Sub(centreX, 160), Add(centreX, 160), Add(cy, half), Sub(cy, half), H);
}

void SetViewAngleProjection(RaceCamera& camera, const CameraWorld& world) { // 0x80010298
    if (world.viewAngle >= world.constants->viewAngle.size()) throw std::out_of_range("race camera: view angle option out of range");
    SetProjection(camera, uint16_t(world.constants->viewAngle[world.viewAngle]), 0, 0);
}

void LoadTarget(RaceCamera& camera, const CameraWorld& world) { // 0x800101FC
    const CameraCar& car = CarAt(world, camera.target);
    camera.chunk = car.chunk;
    camera.view = car.pose;
    AnglesFromRotation(camera.angles, camera.view.m);
}

void RaceView(RaceCamera& camera, uint8_t position, const CameraWorld& world) { // 0x800103C0
    const CameraCar& car = CarAt(world, camera.target);
    LoadTarget(camera, world);
    camera.external = 0;
    if (position == 0) { // Driver: the car's frame raised 0.8 m
        Translate(camera.view, 0, 0xCCCC, 0);
        if (camera.lookBack == 0) camera.mirror = 1;
        else RotateY(camera.view, 0x800);
        camera.hideTarget = 1;
        camera.inCarSound = 1;
        return;
    }
    if (position > 2) throw std::out_of_range("race camera: camera position out of range");
    const int32_t x = camera.view.t[0], y = camera.view.t[1], z = camera.view.t[2];
    int32_t yaw = int32_t(camera.angles[0]) - car.viewYawOffset;
    int32_t pitch = int32_t(camera.angles[1]) + car.viewPitchOffset;
    if (camera.lookBack) {
        yaw += 0x800;
        pitch = -pitch;
    }
    if (camera.split) pitch += 32;
    Identity(camera.view);
    Translate(camera.view, x, y, z);
    Rotate(camera.view, yaw, pitch, 0);
    const size_t row = camera.split ? 4 : 0;
    const std::array<int32_t, 8>& chase = world.constants->chase;
    Translate(camera.view, 0, chase[row + position - 1], chase[row + 2 + position - 1]);
}

void PlayerCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world) { // 0x800102D8
    if (!world.replay) {
        if (pad.held & kButtonLookBack) camera.lookBack = 1;
        if (pad.pressed & kButtonView) {
            camera.position = uint8_t(camera.position + 1);
            if (camera.position == 3) camera.position = 0;
        }
        if (world.hold != 0) {
            StartIntro(camera, world);
            return;
        }
    }
    SetViewAngleProjection(camera, world);
    RaceView(camera, camera.position, world);
}

void StartIntro(RaceCamera& camera, const CameraWorld& world) { // 0x80010608
    const int32_t hold = world.hold, initial = world.holdInitial;
    if (world.viewAngle >= world.constants->viewAngle.size()) throw std::out_of_range("race camera: view angle option out of range");
    const int32_t H = uint16_t(world.constants->viewAngle[world.viewAngle]);
    LoadTarget(camera, world);
    if (hold < 60) { // zoom from H 921 to the view angle's H on the chosen camera (0x80010388)
        SetProjection(camera, LerpDiv(H, 921, hold, 60), 0, 0);
        RaceView(camera, camera.position, world);
        return;
    }
    if (hold - 60 < 60) { // swing from beside the car to behind it
        int32_t s = hold - 66;
        if (s < 0) s = 0;
        const int32_t v = s << 12;
        const int32_t t = int32_t((int64_t(v) * 0x4BDA12F7) >> 32) >> 4; // / 54
        const int32_t x = camera.view.t[0], y = Add(camera.view.t[1], 19660), z = camera.view.t[2];
        const int32_t yaw = int32_t(camera.angles[0]) + Lerp12(0, 1472, t);
        const int32_t pitch = int32_t(camera.angles[1]) + Lerp12(170, 0, t);
        Identity(camera.view);
        Translate(camera.view, x, y, z);
        Rotate(camera.view, yaw, pitch, 0);
        Translate(camera.view, 0, 0, Lerp12(0x354BC, 0x28000, t));
        Rotate(camera.view, Lerp12(0, 140, t), -Lerp12(0, 256, t), 0);
        SetProjection(camera, 160, 0, 0);
        return;
    }
    // in front of the car, pulling back and zooming out
    int32_t s = hold - 138;
    if (s < 0) s = 0;
    const int32_t f = sim::Div(s << 12, initial - 138);
    const int32_t far = MulLo(initial - 120, 19660);
    const int32_t x = camera.view.t[0], y = Add(camera.view.t[1], 0xCCCC), z = camera.view.t[2];
    const int32_t yaw = int32_t(camera.angles[0]) + 2048;
    const int32_t pitch = int32_t(camera.angles[1]) + Lerp12(24, 170, f);
    Identity(camera.view);
    Translate(camera.view, x, y, z);
    Rotate(camera.view, yaw, pitch, 0);
    Translate(camera.view, 0, 0, Lerp12(0x5D4BC, far, f));
    int32_t tilt = Lerp12(80, 0, f);
    if (tilt < 0) tilt = 0;
    else if (80 < tilt) tilt = 80;
    Rotate(camera.view, 0, -tilt, 0);
    int32_t h = Lerp12(450, 921, f);
    if (921 < h) h = 921;
    else if (h < 450) h = 450;
    SetProjection(camera, h, 0, -80);
}

void OnboardCamera(RaceCamera& camera, int32_t view, const CameraWorld& world) { // 0x800113C0
    const CameraCar& car = CarAt(world, camera.target);
    if (view < 0 || view > 12) throw std::out_of_range("race camera: onboard view out of range");
    if (view == 7 || view == 0) {
        camera.hideTarget = 1;
        camera.inCarSound = 1;
        camera.external = 0;
    }
    const int32_t side = Add(car.halfWidth, 19660);
    world.onboard[8].offset[0] = Sub(0, side);
    world.onboard[9].offset[0] = side;
    world.onboard[10].offset[0] = Sub(0, side);
    world.onboard[11].offset[0] = side;
    LoadTarget(camera, world);
    int32_t yaw = camera.angles[0], pitch = camera.angles[1], roll = camera.angles[2];
    const int32_t x = camera.view.t[0], y = camera.view.t[1], z = camera.view.t[2];
    if (!(view < 12)) yaw = pitch = roll = 0;
    const OnboardView& e = world.onboard[view];
    Identity(camera.view);
    Translate(camera.view, x, y, z);
    Rotate(camera.view, yaw, pitch, roll);
    Rotate(camera.view, e.before[0], e.before[1], e.before[2]);
    Translate(camera.view, e.offset[0], e.offset[1], e.offset[2]);
    Rotate(camera.view, e.after[0], e.after[1], e.after[2]);
}

void OnboardSet(RaceCamera& camera, const CameraWorld& world) { // 0x80011704
    SetViewAngleProjection(camera, world);
    switch (camera.onboardView) {
    case 1: RaceView(camera, 1, world); return;
    case 2: RaceView(camera, 2, world); return;
    case 3: OnboardCamera(camera, 12, world); return;
    case 4: OnboardCamera(camera, 7, world); return;
    case 5: OnboardCamera(camera, 8, world); return;
    case 6: OnboardCamera(camera, 9, world); return;
    case 7: OnboardCamera(camera, 10, world); return;
    case 8: OnboardCamera(camera, 11, world); return;
    default: OnboardCamera(camera, 0, world); return; // 0 and anything past the table
    }
}

void ReplayCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world) { // 0x800109FC
    const int32_t count = world.gameMode == 6 ? 1 : int32_t(uint8_t(world.cars.size()));
    const uint32_t buttons = pad.replayPressed;
    bool modePressed = false;
    if (buttons & 0x800) {
        camera.replayMode = uint8_t(camera.replayMode + 1);
        modePressed = true;
        if (camera.replayMode == 3) camera.replayMode = 0;
    }
    if (world.gameMode == 0) {
        if (buttons & 0x100) camera.split = camera.split == 0 ? 1 : 0;
    } else if (world.gameMode == 6) {
        if (world.hold == 0) {
            uint8_t* ghost = CarAt(world, camera.target).ghostByte;
            if ((buttons & 0x1010) == 0x10) {
                if (ghost) *ghost = 0xFF;
            } else if ((buttons & 0x1010) == 0x1000) {
                if (ghost) *ghost = 1;
            }
        }
    } else if ((buttons & 3) == 1 || (buttons & 3) == 2) {
        int32_t position = CarAt(world, camera.target).racePosition;
        if ((buttons & 3) == 1) {
            position--;
            if (position == 0) position = count;
        } else {
            position++;
            if (count < position) position = 1;
        }
        camera.target = CarAtPosition(position, world);
    }
    if (buttons & 0x400) {
        camera.onboardView = uint8_t(camera.onboardView + 1);
        if (camera.onboardView == 9) camera.onboardView = 0;
    }
    if (buttons & 0x200) {
        camera.replayInfo = uint8_t(camera.replayInfo + 1);
        if (camera.replayInfo == 3) camera.replayInfo = 0;
    }
    switch (camera.replayMode) {
    case 0: break;
    case 1: OnboardSet(camera, world); return;
    case 2:
        if (world.gameMode != 0) {
            camera.replayMode = 0;
            break;
        }
        if (modePressed) {
            OnboardSet(camera, world);
            return;
        }
        if (camera.split == 0) camera.target = CarAt(world, 0).racePosition == 1 ? 0 : 1;
        break;
    default: return;
    }
    // Trackside: the camera record of the followed car's lap and course distance.
    const CameraCar& car = CarAt(world, camera.target);
    const ReplayCameraData& d = world.replayCameras;
    const uint32_t record = d.Valid() ? FindReplayCamera(d, car.lap, car.courseDistance, world.courseLength) : 0;
    if (record == 0) {
        PlayerCamera(camera, pad, world);
        return;
    }
    int32_t distance = car.courseDistance, end = d.S32(record + 0xC);
    const int32_t start = d.S32(record + 8);
    if (end < start) end = Add(end, world.courseLength);
    if (distance < start) distance = Add(distance, world.courseLength);
    end = Sub(end, start);
    distance = Sub(distance, start);
    *world.replayProgress = end != 0 ? int32_t(uint32_t(uint64_t(sim::Div64(int64_t(distance) << 12, end)))) : 0;
    camera.replayFlags = d.U16(record);
    switch (camera.replayFlags & 0xF) {
    case 0: TracksideLookAt(camera, record, world); break;
    case 1: TracksideFixed(camera, record, world); break;
    case 2: TracksideOnboard(camera, record, world); break;
    case 3: TracksideZoom(camera, record, world); break;
    default: break;
    }
}

void UpdateCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world) { // 0x800100F4
    camera.lookBack = 0;
    camera.hideTarget = 0;
    camera.mirror = 0;
    for (int i = 0; i < 3; i++) camera.previous[i] = camera.view.t[i];
    camera.external = 1;
    camera.inCarSound = 0;
    if (!world.replay) PlayerCamera(camera, pad, world);
    else ReplayCamera(camera, pad, world);
    for (int i = 0; i < 3; i++) camera.velocity[i] = Sub(camera.view.t[i], camera.previous[i]);
    camera.speed = VectorLength(camera.direction, camera.velocity);
    AnglesFromRotation(camera.angles, camera.view.m);
    for (int i = 0; i < 3; i++) {
        camera.backAxis[i] = camera.view.m[i][2];
        camera.rightAxis[i] = camera.view.m[i][0];
    }
}

// ================================================================ renderer side

bool MirrorShown(const RaceCamera& camera, uint8_t gameMode, uint8_t frameRateMode) { // 0x800294D4
    return camera.split == 0 && camera.mirror != 0 && gameMode != 0 && frameRateMode > 1;
}

RaceCamera MirrorCamera(const RaceCamera& camera) { // 0x800294D4, the mirror branch up to 0x800298FC
    RaceCamera m = camera;
    Translate(m.view, 0, 0x3333, 0x8000);                 // 0x8007B050
    Rotate(m.view, 0, -32, 0);                            // 0x8007B088
    const int16_t mirrorZ[3][3] = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, -4096}};
    MulMatrix(m.view.m, m.view.m, mirrorZ);               // 0x8007B25C(m, 0x1000, 0x1000, -0x1000) = m * diag -> 0x8007B994
    m.rectX = 0;
    m.rectY = 0;
    m.rectW = 120;
    m.rectH = 32;
    LibraryWindow(m, -60, 60, 16, -16, 120);              // 0x8007B320(m, -60, 60, 16, -16, 120, 0x7FFF)
    return m;
}

CameraProjection ProjectionOf(const RaceCamera& c) {
    // 0x8007B374: P = [[sx, 0, cx], [0, -sy, -cy], [0, 0, -4096]] with sx = (w << 12) / spanX, sy = (h << 12) / spanY,
    // cx / cy = centre * s >> 12; GTE rotation = P V^T, translation = P V^T (-eye); OFX / OFY = w / 2, h / 2.
    CameraProjection p;
    const int32_t w = c.rectW, h = c.rectH;
    const int32_t sx = c.spanX ? sim::Div(w << 12, c.spanX) : 4096, sy = c.spanY ? sim::Div(h << 12, c.spanY) : 4096;
    const int32_t cx = (int32_t(c.centreX) * sx) >> 12, cy = (int32_t(c.centreY) * sy) >> 12;
    const float P[3][3] = {{float(sx), 0, float(cx)}, {0, float(-sy), float(-cy)}, {0, 0, -4096.0f}};
    float eye[3];
    for (int i = 0; i < 3; i++) eye[i] = float(double(c.view.t[i]) / 65536.0);
    for (int r = 0; r < 3; r++) {
        float t = 0;
        for (int j = 0; j < 3; j++) { // (P V^T)[r][j] = sum_k P[r][k] V[j][k]
            float s = 0;
            for (int k = 0; k < 3; k++) s += P[r][k] * float(c.view.m[j][k]);
            p.rows[r][j] = s / (4096.0f * 4096.0f);
            t -= p.rows[r][j] * eye[j];
        }
        p.rows[r][3] = t;
    }
    p.H = float(c.H);
    p.halfWidth = float(w) / 2;
    p.halfHeight = float(h) / 2;
    for (int i = 0; i < 3; i++) {
        p.eye[i] = eye[i];
        p.right[i] = float(c.view.m[i][0]) / 4096.0f;
        p.up[i] = float(c.view.m[i][1]) / 4096.0f;
        p.forward[i] = -float(c.view.m[i][2]) / 4096.0f;
    }
    return p;
}

void ClipMatrix(const RaceCamera& camera, float aspect, float zNear, float out[16]) {
    const CameraProjection p = ProjectionOf(camera);
    // ndc x = (pixel x - w / 2) / (h / 2 * aspect), ndc y = (pixel y - h / 2) / (h / 2); clip w = a.z (depth).
    const float kx = p.H / (p.halfHeight * aspect), ky = p.H / p.halfHeight;
    for (int col = 0; col < 4; col++) {
        out[col * 4 + 0] = kx * p.rows[0][col];
        out[col * 4 + 1] = ky * p.rows[1][col];
        out[col * 4 + 2] = col == 3 ? zNear : 0.0f;
        out[col * 4 + 3] = p.rows[2][col];
    }
}

} // namespace gt2::camera
