#include "gt2view/particles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/sim/trig.h"
#include "gt2view/psx_gte.h"

namespace gt2view {

namespace {

// Guest addresses of the dev mirror (US v1.2).
constexpr uint32_t kPoolAddress = 0x800ADA0C;
constexpr uint32_t kCarArray = 0x800A9688, kCarStride = 0xB40, kCarCountAddress = 0x800AF231, kFieldsPerFrameAddress = 0x801D5864;

// The sprite words of 0x8002EB60 per kind: uv0 | clut << 16, uv1 | tpage << 16, uv2, uv3.
struct SpriteWords { uint32_t uv0Clut, uv1Tpage, uv2, uv3; };
constexpr SpriteWords kSprite[2] = {{0x7F960080u, 0x0029009Fu, 0x1F80u, 0x1F9Fu}, {0x7F9700C0u, 0x002900EFu, 0x2FC0u, 0x2FEFu}};

int32_t FloorShift12(int64_t v) { return int32_t(v >> 12); } // arithmetic: floor

// The GTE's unsigned Newton-Raphson division (RTPS / RTPT): gt2view/psx_gte.h.
using psx::GteDivide;

int16_t S16(const uint8_t* p) { return int16_t(uint16_t(p[0] | (p[1] << 8))); }
int32_t S32(const uint8_t* p) { return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24); }
const uint8_t* At(const uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }

} // namespace

// ================================================================ pool

void SmokePool::LoadFadeScale(const gt2::GuestImage& raceOverlay) {
    const uint32_t fade = raceOverlay.Sim(kFadeScaleAddress); // this build's table (gt2formats/exe_profile.h)
    for (size_t i = 0; i < fadeScale.size(); i++) fadeScale[i] = raceOverlay.Get<int16_t>(fade + uint32_t(i) * 2);
}

void SmokePool::Reset() { // 0x8001681C (the header's + 0x10 allocation counter is left as it is)
    records.fill(SmokeParticle{});
    for (int i = 0; i < kCapacity; i++) records[size_t(i)].next = int16_t(i + 1);
    records[kCapacity - 1].next = -1;
    activeHead = -1;
    rise = 0x444;
    fadeRate[0] = 0x90;
    fadeRate[1] = 0x48;
    freeHead = 0;
    growth = 8;
}

SmokeParticle* SmokePool::Allocate(int kind, int fieldsPerFrame) { // 0x800168A4
    const int16_t index = freeHead;
    if (index < 0 || fieldsPerFrame < 2) return nullptr;
    SmokeParticle& r = records[size_t(index)];
    const int16_t previousHead = activeHead;
    activeHead = index;
    freeHead = r.next;
    r.next = previousHead;
    r.kind = int16_t(kind);
    const int32_t rate = kind == 0 ? fadeRate[0] : fadeRate[1];
    // mult + mflo + sra 12, stored as a halfword (the index is read with lh: 0..15 in practice)
    r.fade = int16_t(int32_t(uint32_t(rate) * uint32_t(int32_t(fadeScale[size_t(uint16_t(scaleIndex)) & 15]))) >> 12);
    scaleIndex = int16_t(scaleIndex + 1);
    if (scaleIndex == 16) scaleIndex = 0;
    return &r;
}

void SmokePool::Update() { // 0x80016978
    int16_t previous = -1;
    for (int16_t index = activeHead; index >= 0;) {
        SmokeParticle& r = records[size_t(index)];
        const int16_t next = r.next;
        r.intensity = int16_t(uint16_t(r.intensity) - uint16_t(r.fade));
        if (r.intensity >= 0) {
            r.position[1] = int32_t(uint32_t(r.position[1]) + uint32_t(rise));
            r.size = int16_t(uint16_t(r.size) + uint16_t(growth));
            previous = index;
        } else { // faded out: back to the free list
            if (previous < 0) activeHead = next;
            else records[size_t(previous)].next = next;
            r.next = freeHead;
            freeHead = index;
        }
        index = next;
    }
}

size_t SmokePool::ActiveCount() const {
    size_t n = 0;
    for (int16_t index = activeHead; index >= 0 && n <= size_t(kCapacity); index = records[size_t(index)].next) n++;
    return n;
}

void SmokePool::SpawnFromCar(const CarInput& car, int fieldsPerFrame) { // the wheel loop of 0x800133F0
    for (const CarInput::Wheel& w : car.wheels) {
        if (w.skidLevel < 0x80) continue; // `lb` of wheel + 0x1C: a negative byte spawns
        SmokeParticle* r = Allocate(0, fieldsPerFrame);
        if (!r) continue;
        // 0x8007AFC0 / 0x8007AF60 / 0x8007B008: the car matrix composed with a unit matrix translated by the wheel
        // record << 4; the translation is 0x8008220C (ApplyMatrixLV: three 12-bit slices through MVMVA), which is
        // exactly floor(R . v / 4096) + t.
        const int64_t v[3] = {int64_t(w.x) * 16, int64_t(w.y) * 16, int64_t(w.z) * 16};
        for (int i = 0; i < 3; i++) {
            const int64_t dot = int64_t(car.rotation[i][0]) * v[0] + int64_t(car.rotation[i][1]) * v[1] + int64_t(car.rotation[i][2]) * v[2];
            r->position[i] = int32_t(uint32_t(FloorShift12(dot)) + uint32_t(car.translation[i]));
        }
        r->angle = uint16_t(uint32_t(r->position[0]) ^ uint32_t(r->position[1]) ^ uint32_t(r->position[2]));
        r->size = 0;
        r->intensity = int16_t(uint16_t(w.skidLevel) << 4);
    }
}

// ================================================================ the original's projection

std::string SmokePool::Primitive::ToString() const {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "POLY 2E quad tex semi rgb=%02X%02X%02X v0=(%d,%d) uv0=(%u,%u) clut=%04X v1=(%d,%d) uv1=(%u,%u) tpage=%04X v2=(%d,%d) uv2=(%u,%u) v3=(%d,%d) uv3=(%u,%u)",
                  shade, shade, shade, x[0], y[0], u[0], v[0], clut, x[1], y[1], u[1], v[1], tpage, x[2], y[2], u[2], v[2], x[3], y[3], u[3], v[3]);
    return line;
}

std::vector<SmokePool::Primitive> SmokePool::BuildPrimitives(const PsxView& view) const { // 0x8002EB60
    std::vector<Primitive> out;
    for (int16_t index = activeHead, guard = 0; index >= 0 && guard <= kCapacity; index = records[size_t(index)].next, guard++) {
        const SmokeParticle& r = records[size_t(index)];
        if (r.size == 0) continue;
        int32_t vtx[3];
        uint32_t magnitude = 0;
        for (int i = 0; i < 3; i++) {
            vtx[i] = int32_t(uint32_t(r.position[i]) + uint32_t(view.offset[i])) >> 10;
            magnitude |= vtx[i] < 0 ? uint32_t(-int64_t(vtx[i])) : uint32_t(vtx[i]);
        }
        if (!(int32_t(magnitude) < 0x8000)) continue;
        // RTPS, sf = 1, TR = 0: IR = MAC >> 12 saturated to s16; SZ3 = MAC3 >> 12 saturated to 0..0xFFFF.
        const int16_t vx = int16_t(vtx[0]), vy = int16_t(vtx[1]), vz = int16_t(vtx[2]);
        bool error = false;
        int32_t ir[3];
        int64_t mac3 = 0;
        for (int i = 0; i < 3; i++) {
            const int64_t mac = (int64_t(view.rotation[i][0]) * vx + int64_t(view.rotation[i][1]) * vy + int64_t(view.rotation[i][2]) * vz) >> 12;
            if (i == 2) mac3 = mac;
            if (mac < -0x8000 || mac > 0x7FFF) error = true;
            ir[i] = int32_t(std::clamp<int64_t>(mac, -0x8000, 0x7FFF));
        }
        if (mac3 < 0 || mac3 > 0xFFFF) error = true;
        const uint32_t sz3 = uint32_t(std::clamp<int64_t>(mac3, 0, 0xFFFF));
        bool divideOverflow = false;
        const uint32_t q = GteDivide(view.h, sz3, divideOverflow);
        if (divideOverflow) error = true;
        const int64_t sxMac = int64_t(q) * ir[0] + view.ofx, syMac = int64_t(q) * ir[1] + view.ofy;
        if (sxMac > INT32_MAX || sxMac < INT32_MIN || syMac > INT32_MAX || syMac < INT32_MIN) error = true;
        const int32_t sx = int32_t(sxMac >> 16), sy = int32_t(syMac >> 16);
        if (sx < -0x400 || sx > 0x3FF || sy < -0x400 || sy > 0x3FF) error = true;
        const int64_t mac0 = int64_t(q) * r.size; // DQA = size, DQB = 0
        if (mac0 > INT32_MAX || mac0 < INT32_MIN) error = true;
        if (error) continue; // FLAG bit 31
        const int32_t radius = int32_t(mac0) >> 16;
        if (radius >= 0x100) continue;
        const int16_t cx = int16_t(std::clamp(sx, -0x400, 0x3FF)), cy = int16_t(std::clamp(sy, -0x400, 0x3FF));
        const int32_t a = (radius * gt2::sim::Sin(r.angle & 0xFFFu)) >> 12, b = (radius * gt2::sim::Cos(r.angle & 0xFFFu)) >> 12;
        Primitive p;
        p.shade = uint8_t(int32_t(r.intensity) >> (r.kind == 0 ? 5 : 3));
        p.x[0] = int16_t(cx - b + a); p.y[0] = int16_t(cy - a - b);
        p.x[1] = int16_t(cx + b + a); p.y[1] = int16_t(cy + a - b);
        p.x[2] = int16_t(cx - b - a); p.y[2] = int16_t(cy - a + b);
        p.x[3] = int16_t(cx + b - a); p.y[3] = int16_t(cy + a + b);
        const SpriteWords& s = kSprite[r.kind == 0 ? 0 : 1];
        const uint32_t uv[4] = {s.uv0Clut & 0xFFFF, s.uv1Tpage & 0xFFFF, s.uv2, s.uv3};
        for (int k = 0; k < 4; k++) { p.u[k] = uint8_t(uv[k] & 0xFF); p.v[k] = uint8_t(uv[k] >> 8); }
        p.clut = uint16_t(s.uv0Clut >> 16);
        p.tpage = uint16_t(s.uv1Tpage >> 16);
        p.otIndex = std::min(sz3 >> 3, 0xFFFu);
        out.push_back(p);
    }
    return out;
}

// ================================================================ native sprites

void SmokePool::AppendBillboards(std::vector<SceneVertex>& out, const float eye[3], const float right[3], const float up[3], const float forward[3],
                                 float projectionDistance) const {
    for (int16_t index = activeHead, guard = 0; index >= 0 && guard <= kCapacity; index = records[size_t(index)].next, guard++) {
        const SmokeParticle& r = records[size_t(index)];
        if (r.size == 0) continue;
        const float p[3] = {float(r.position[0] / 65536.0), float(r.position[1] / 65536.0), float(r.position[2] / 65536.0)};
        const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
        // The original's tests in its units (1/64 m): the camera offset must fit in 16 bits, the projection must
        // not overflow (H < 2 SZ) and the radius on the 240-line frame must stay below 256 pixels.
        if (std::fabs(d[0]) * 64.0f >= 32768.0f || std::fabs(d[1]) * 64.0f >= 32768.0f || std::fabs(d[2]) * 64.0f >= 32768.0f) continue;
        const float depth64 = (d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2]) * 64.0f;
        if (!(projectionDistance < depth64 * 2.0f)) continue;
        if (float(r.size) * projectionDistance / depth64 >= 256.0f) continue;
        const float half = float(r.size) / 64.0f;
        const float s = float(gt2::sim::Sin(r.angle & 0xFFFu)) / 4096.0f * half, c = float(gt2::sim::Cos(r.angle & 0xFFFu)) / 4096.0f * half;
        // PS1 corners (screen y down) -> camera plane: v0 (a - b, -a - b), v1 (a + b, a - b), v2 (-a - b, b - a), v3 (b - a, a + b).
        const float k[4][2] = {{s - c, s + c}, {s + c, c - s}, {-s - c, s - c}, {c - s, -s - c}};
        const SpriteWords& w = kSprite[r.kind == 0 ? 0 : 1];
        const uint32_t uv[4] = {w.uv0Clut & 0xFFFF, w.uv1Tpage & 0xFFFF, w.uv2, w.uv3};
        const uint16_t clut = uint16_t(w.uv0Clut >> 16), tpage = uint16_t(w.uv1Tpage >> 16);
        const float shade = float(uint8_t(int32_t(r.intensity) >> (r.kind == 0 ? 5 : 3))) / 255.0f;
        static constexpr int kSplit[6] = {0, 1, 2, 1, 2, 3}; // the GPU's two triangles of a quad
        for (int corner : kSplit) {
            SceneVertex o{};
            for (int a = 0; a < 3; a++) o.pos[a] = p[a] + k[corner][0] * right[a] + k[corner][1] * up[a];
            o.texel[0] = float(uv[corner] & 0xFF) + 0.5f;
            o.texel[1] = float(uv[corner] >> 8) + 0.5f;
            o.color[0] = o.color[1] = o.color[2] = shade;
            o.page = uint32_t((tpage & 0xF) * 64) | (uint32_t(((tpage >> 4) & 1) * 256) << 16);
            o.clut = uint32_t((clut & 0x3F) * 16) | (uint32_t(clut >> 6) << 16);
            o.flags = kTextured | kSemiTransparent | (uint32_t((tpage >> 7) & 3) << 8);
            out.push_back(o);
        }
    }
}

// ================================================================ dev mirror of the running original

SmokePool SmokeGuestMirror::ReadGuestPool(const uint8_t* ram) {
    SmokePool pool;
    const uint8_t* h = At(ram, kPoolAddress);
    pool.rise = S32(h + 0);
    pool.fadeRate[0] = S32(h + 4);
    pool.fadeRate[1] = S32(h + 8);
    pool.growth = S32(h + 0xC);
    pool.scaleIndex = S16(h + 0x10);
    pool.activeHead = S16(h + 0x12);
    pool.freeHead = S16(h + 0x14);
    std::memcpy(pool.records.data(), h + 0x18, sizeof(SmokeParticle) * SmokePool::kCapacity);
    for (size_t i = 0; i < pool.fadeScale.size(); i++) pool.fadeScale[i] = S16(At(ram, SmokePool::kFadeScaleAddress + uint32_t(i) * 2));
    return pool;
}

void SmokeGuestMirror::OnUpdate() {
    if (synced_) pool_.Update();
    updatePending_ = true;
}

std::string SmokeGuestMirror::OnDraw(const uint8_t* ram) {
    const SmokePool guest = ReadGuestPool(ram);
    char line[256];
    if (!synced_ || !updatePending_) { // first frame (or a draw without an update since the last): take the guest's pool
        pool_ = guest;
        synced_ = true;
        updatePending_ = false;
        std::snprintf(line, sizeof(line), "sync: %zu active", pool_.ActiveCount());
        return line;
    }
    updatePending_ = false;
    const int fieldsPerFrame = At(ram, kFieldsPerFrameAddress)[0];
    const size_t before = pool_.ActiveCount();
    const int cars = At(ram, kCarCountAddress)[0];
    for (int c = 0; c < cars && c < 6; c++) {
        const uint32_t car = kCarArray + uint32_t(c) * kCarStride;
        SmokePool::CarInput in;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) in.rotation[i][j] = S16(At(ram, car + 0x81C + uint32_t(i * 3 + j) * 2));
            in.translation[i] = S32(At(ram, car + 0x830 + uint32_t(i) * 4));
        }
        for (uint32_t w = 0; w < 4; w++) {
            in.wheels[w].x = S16(At(ram, car + 0x7C4 + w * 0x10));
            in.wheels[w].y = S16(At(ram, car + 0x7C6 + w * 0x10));
            in.wheels[w].z = S16(At(ram, car + 0x7C8 + w * 0x10));
            in.wheels[w].skidLevel = At(ram, car + 0x4A8 + w * 0x68)[0];
        }
        pool_.SpawnFromCar(in, fieldsPerFrame);
    }
    frames++;
    const size_t after = pool_.ActiveCount();
    spawnsSeen += after > before ? after - before : 0;
    // Compare the header and the active chain (free records keep stale bytes that nobody reads).
    bool same = pool_.rise == guest.rise && pool_.fadeRate[0] == guest.fadeRate[0] && pool_.fadeRate[1] == guest.fadeRate[1] && pool_.growth == guest.growth &&
                pool_.scaleIndex == guest.scaleIndex && pool_.activeHead == guest.activeHead && pool_.freeHead == guest.freeHead;
    int differing = -1;
    for (int16_t index = pool_.activeHead, guard = 0; same && index >= 0 && guard <= SmokePool::kCapacity; index = pool_.records[size_t(index)].next, guard++)
        if (std::memcmp(&pool_.records[size_t(index)], &guest.records[size_t(index)], sizeof(SmokeParticle)) != 0) {
            same = false;
            differing = index;
        }
    if (same) matched++;
    std::snprintf(line, sizeof(line), "frame %zu: %zu active (%zu new) %s", frames, after, after > before ? after - before : size_t(0),
                  same ? "== guest" : "DIFFERS from guest");
    std::string report = line;
    if (!same) {
        if (differing >= 0) {
            const SmokeParticle& a = pool_.records[size_t(differing)];
            const SmokeParticle& b = guest.records[size_t(differing)];
            std::snprintf(line, sizeof(line), " (record %d: ours size %d int %d ang %04X fade %d pos %d %d %d / guest size %d int %d ang %04X fade %d pos %d %d %d)", differing,
                          a.size, a.intensity, a.angle, a.fade, a.position[0], a.position[1], a.position[2], b.size, b.intensity, b.angle, b.fade, b.position[0],
                          b.position[1], b.position[2]);
        } else {
            std::snprintf(line, sizeof(line), " (header: ours idx %d active %d free %d / guest idx %d active %d free %d)", pool_.scaleIndex, pool_.activeHead, pool_.freeHead,
                          guest.scaleIndex, guest.activeHead, guest.freeHead);
        }
        report += line;
        pool_ = guest; // resynchronise
    }
    return report;
}

std::vector<std::string> SmokeGuestMirror::DrawLines(const uint8_t* ram, uint32_t viewAddress) {
    const SmokePool guest = ReadGuestPool(ram);
    SmokePool::PsxView view;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) view.rotation[i][j] = S16(At(ram, viewAddress + 8 + uint32_t(i * 3 + j) * 2));
        view.offset[i] = S32(At(ram, viewAddress + 0x28 + uint32_t(i) * 4));
    }
    view.ofx = S32(At(ram, viewAddress + 0x5C));
    view.ofy = S32(At(ram, viewAddress + 0x60));
    view.h = uint16_t(S16(At(ram, viewAddress + 0x64)));
    std::vector<std::string> lines;
    for (const SmokePool::Primitive& p : guest.BuildPrimitives(view)) lines.push_back(p.ToString() + " ot=" + std::to_string(p.otIndex));
    return lines;
}

} // namespace gt2view
