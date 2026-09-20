#include "interp/gte.h"

#include <algorithm>

namespace gt2 {
namespace {

constexpr uint32_t kFlagMacPos[4] = {1u << 16, 1u << 30, 1u << 29, 1u << 28};
constexpr uint32_t kFlagMacNeg[4] = {1u << 15, 1u << 27, 1u << 26, 1u << 25};
constexpr uint32_t kFlagIr[4] = {1u << 12, 1u << 24, 1u << 23, 1u << 22};
constexpr uint32_t kFlagColor[3] = {1u << 21, 1u << 20, 1u << 19};
constexpr uint32_t kFlagSz = 1u << 18, kFlagDivide = 1u << 17, kFlagSx = 1u << 14, kFlagSy = 1u << 13;

// Reciprocal table of the UNR divider: max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101).
struct UnrTable {
    uint8_t v[257];
    constexpr UnrTable() : v{} {
        for (int i = 0; i < 257; i++) {
            int t = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            v[i] = uint8_t(t < 0 ? 0 : t);
        }
    }
};
constexpr UnrTable kUnr;

int CountLeadingZeros16(uint16_t v) {
    int n = 0;
    for (uint16_t bit = 0x8000; bit && !(v & bit); bit >>= 1) n++;
    return n;
}

} // namespace

// ---------------------------------------------------------------- helpers

int64_t Gte::CheckMac(int index, int64_t value) {
    if (value >= (int64_t(1) << 43)) flag_ |= kFlagMacPos[index];
    else if (value < -(int64_t(1) << 43)) flag_ |= kFlagMacNeg[index];
    return int64_t(uint64_t(value) << 20) >> 20;
}

void Gte::SetMac(int index, int64_t value) { mac_[index] = int32_t(value >> sf_); }

void Gte::SetMac0(int64_t value) {
    if (value > 0x7FFFFFFFll) flag_ |= kFlagMacPos[0];
    else if (value < -0x80000000ll) flag_ |= kFlagMacNeg[0];
    mac_[0] = int32_t(value);
}

void Gte::SetIr(int index, int32_t value, bool lm) {
    const int32_t low = lm ? 0 : -0x8000;
    if (value < low) { value = low; flag_ |= kFlagIr[index]; }
    else if (value > 0x7FFF) { value = 0x7FFF; flag_ |= kFlagIr[index]; }
    ir_[index] = int16_t(value);
}

void Gte::SetIr0(int32_t value) {
    if (value < 0) { value = 0; flag_ |= kFlagIr[0]; }
    else if (value > 0x1000) { value = 0x1000; flag_ |= kFlagIr[0]; }
    ir_[0] = int16_t(value);
}

void Gte::PushSxy(int32_t x, int32_t y) {
    if (x < -0x400) { x = -0x400; flag_ |= kFlagSx; } else if (x > 0x3FF) { x = 0x3FF; flag_ |= kFlagSx; }
    if (y < -0x400) { y = -0x400; flag_ |= kFlagSy; } else if (y > 0x3FF) { y = 0x3FF; flag_ |= kFlagSy; }
    for (int i = 0; i < 2; i++) { sxy_[i][0] = sxy_[i + 1][0]; sxy_[i][1] = sxy_[i + 1][1]; }
    sxy_[2][0] = int16_t(x);
    sxy_[2][1] = int16_t(y);
    if (captureEnabled) projectedXy.insert((uint32_t(x) & 0x7FF) | ((uint32_t(y) & 0x7FF) << 16));
}

void Gte::PushSz(int32_t value) {
    if (value < 0) { value = 0; flag_ |= kFlagSz; } else if (value > 0xFFFF) { value = 0xFFFF; flag_ |= kFlagSz; }
    for (int i = 0; i < 3; i++) sz_[i] = sz_[i + 1];
    sz_[3] = uint16_t(value);
}

void Gte::PushColor() {
    for (int i = 0; i < 2; i++) std::copy(rgbFifo_[i + 1], rgbFifo_[i + 1] + 4, rgbFifo_[i]);
    for (int i = 0; i < 3; i++) {
        int32_t c = mac_[i + 1] >> 4;
        if (c < 0) { c = 0; flag_ |= kFlagColor[i]; } else if (c > 0xFF) { c = 0xFF; flag_ |= kFlagColor[i]; }
        rgbFifo_[2][i] = uint8_t(c);
    }
    rgbFifo_[2][3] = rgbc_[3];
}

uint32_t Gte::Divide(uint16_t h, uint16_t sz3) {
    if (h >= uint32_t(sz3) * 2) { flag_ |= kFlagDivide; return 0x1FFFF; }
    const int shift = CountLeadingZeros16(sz3);
    const uint32_t n = uint32_t(h) << shift;
    uint32_t d = uint32_t(sz3) << shift;
    const uint32_t u = kUnr.v[(d - 0x7FC0) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    return std::min<uint32_t>(0x1FFFF, uint32_t((uint64_t(n) * d + 0x8000u) >> 16));
}

void Gte::MultiplyMatrix(const int16_t m[3][3], const Vec3& v, const int32_t* translation, bool lm) {
    for (int i = 0; i < 3; i++) {
        int64_t x = translation ? int64_t(translation[i]) << 12 : 0;
        x = CheckMac(i + 1, x + int64_t(m[i][0]) * v[0]);
        x = CheckMac(i + 1, x + int64_t(m[i][1]) * v[1]);
        x = CheckMac(i + 1, x + int64_t(m[i][2]) * v[2]);
        SetMacAndIr(i + 1, x, lm);
    }
}

void Gte::InterpolateToFarColor(int64_t m1, int64_t m2, int64_t m3) {
    const int64_t in[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++) {
        SetMac(i + 1, CheckMac(i + 1, (int64_t(fc_[i]) << 12) - in[i]));
        SetIr(i + 1, mac_[i + 1], false);
    }
    for (int i = 0; i < 3; i++) SetMacAndIr(i + 1, CheckMac(i + 1, int64_t(ir_[i + 1]) * ir_[0] + in[i]), lm_);
}

// ---------------------------------------------------------------- commands

void Gte::Rtp(int vi, bool last) {
    if (captureEnabled) {
        if (transformDirty_ || capturedTransforms.empty()) {
            CapturedTransform t{};
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) t.rt[r][c] = rt_[r][c];
            for (int i = 0; i < 3; i++) t.tr[i] = tr_[i];
            t.h = h_;
            t.ofx = ofx_;
            t.ofy = ofy_;
            t.firstVertex = uint32_t(capturedVertices.size());
            capturedTransforms.push_back(t);
            transformDirty_ = false;
        }
        capturedVertices.push_back({v_[vi][0], v_[vi][1], v_[vi][2]});
        capturedTransforms.back().vertexCount++;
    }
    int64_t full[3];
    for (int i = 0; i < 3; i++) {
        int64_t x = int64_t(tr_[i]) << 12;
        x = CheckMac(i + 1, x + int64_t(rt_[i][0]) * v_[vi][0]);
        x = CheckMac(i + 1, x + int64_t(rt_[i][1]) * v_[vi][1]);
        x = CheckMac(i + 1, x + int64_t(rt_[i][2]) * v_[vi][2]);
        full[i] = x;
        SetMac(i + 1, x);
    }
    SetIr(1, mac_[1], lm_);
    SetIr(2, mac_[2], lm_);
    // IR3: the saturation FLAG is decided on the value shifted by 12 regardless of sf; the stored value is
    // the clamped MAC3.
    const int32_t zShifted = int32_t(full[2] >> 12);
    if (zShifted < -0x8000 || zShifted > 0x7FFF) flag_ |= kFlagIr[3];
    ir_[3] = int16_t(std::clamp<int32_t>(mac_[3], lm_ ? 0 : -0x8000, 0x7FFF));
    PushSz(zShifted);

    const int64_t n = Divide(h_, sz_[3]);
    const int64_t sx = n * ir_[1] + ofx_;
    SetMac0(sx);
    const int64_t sy = n * ir_[2] + ofy_;
    SetMac0(sy);
    PushSxy(int32_t(sx >> 16), int32_t(sy >> 16));
    if (last) {
        const int64_t depthCue = n * dqa_ + dqb_;
        SetMac0(depthCue);
        SetIr0(int32_t(depthCue >> 12));
    }
}

void Gte::Mvmva(uint32_t command) {
    const uint32_t mx = (command >> 17) & 3, vsel = (command >> 15) & 3, cv = (command >> 13) & 3;
    int16_t garbage[3][3] = {{int16_t(-(rgbc_[0] << 4)), int16_t(rgbc_[0] << 4), ir_[0]},
                             {rt_[0][2], rt_[0][2], rt_[0][2]},
                             {rt_[1][1], rt_[1][1], rt_[1][1]}};
    const int16_t (*m)[3] = mx == 0 ? rt_ : mx == 1 ? llm_ : mx == 2 ? lcm_ : garbage;
    Vec3 v = vsel < 3 ? Vec3{v_[vsel][0], v_[vsel][1], v_[vsel][2]} : Vec3{ir_[1], ir_[2], ir_[3]};

    if (cv == 2) { // far colour as translation: hardware bug - only the last two products reach MAC
        for (int i = 0; i < 3; i++) {
            int64_t first = CheckMac(i + 1, (int64_t(fc_[i]) << 12) + int64_t(m[i][0]) * v[0]);
            int32_t shifted = int32_t(first >> sf_);
            if (shifted < -0x8000 || shifted > 0x7FFF) flag_ |= kFlagIr[i + 1];
            int64_t x = CheckMac(i + 1, int64_t(m[i][1]) * v[1]);
            x = CheckMac(i + 1, x + int64_t(m[i][2]) * v[2]);
            SetMacAndIr(i + 1, x, lm_);
        }
        return;
    }
    const int32_t* translation = cv == 0 ? tr_ : cv == 1 ? bk_ : nullptr;
    MultiplyMatrix(m, v, translation, lm_);
}

void Gte::ColorFromIr(int mode) {
    MultiplyMatrix(lcm_, Vec3{ir_[1], ir_[2], ir_[3]}, bk_, lm_);
    if (mode == 1) {
        for (int i = 0; i < 3; i++) SetMacAndIr(i + 1, (int64_t(rgbc_[i]) * ir_[i + 1]) << 4, lm_);
    } else if (mode == 2) {
        InterpolateToFarColor((int64_t(rgbc_[0]) * ir_[1]) << 4, (int64_t(rgbc_[1]) * ir_[2]) << 4, (int64_t(rgbc_[2]) * ir_[3]) << 4);
    }
    PushColor();
}

void Gte::NormalColor(int vi, int mode) {
    MultiplyMatrix(llm_, Vec3{v_[vi][0], v_[vi][1], v_[vi][2]}, nullptr, lm_);
    ColorFromIr(mode);
}

void Gte::Execute(uint32_t command) {
    flag_ = 0;
    sf_ = (command & (1u << 19)) ? 12 : 0;
    lm_ = (command & (1u << 10)) != 0;
    const uint32_t function = command & 0x3F;
    commandCounts[function]++;

    switch (function) {
    case 0x01: Rtp(0, true); break;
    case 0x30: Rtp(0, false); Rtp(1, false); Rtp(2, true); break;
    case 0x06: // NCLIP
        SetMac0(int64_t(sxy_[0][0]) * sxy_[1][1] + int64_t(sxy_[1][0]) * sxy_[2][1] + int64_t(sxy_[2][0]) * sxy_[0][1] -
                int64_t(sxy_[0][0]) * sxy_[2][1] - int64_t(sxy_[1][0]) * sxy_[0][1] - int64_t(sxy_[2][0]) * sxy_[1][1]);
        break;
    case 0x0C: { // OP: cross product of IR with the RT diagonal
        const int64_t d1 = rt_[0][0], d2 = rt_[1][1], d3 = rt_[2][2];
        const int64_t i1 = ir_[1], i2 = ir_[2], i3 = ir_[3];
        SetMac(1, CheckMac(1, i3 * d2 - i2 * d3));
        SetMac(2, CheckMac(2, i1 * d3 - i3 * d1));
        SetMac(3, CheckMac(3, i2 * d1 - i1 * d2));
        for (int i = 1; i <= 3; i++) SetIr(i, mac_[i], lm_);
        break;
    }
    case 0x10: InterpolateToFarColor(int64_t(rgbc_[0]) << 16, int64_t(rgbc_[1]) << 16, int64_t(rgbc_[2]) << 16); PushColor(); break; // DPCS
    case 0x2A: // DPCT
        for (int n = 0; n < 3; n++) {
            InterpolateToFarColor(int64_t(rgbFifo_[0][0]) << 16, int64_t(rgbFifo_[0][1]) << 16, int64_t(rgbFifo_[0][2]) << 16);
            PushColor();
        }
        break;
    case 0x11: InterpolateToFarColor(int64_t(ir_[1]) << 12, int64_t(ir_[2]) << 12, int64_t(ir_[3]) << 12); PushColor(); break; // INTPL
    case 0x29: // DCPL
        InterpolateToFarColor((int64_t(rgbc_[0]) * ir_[1]) << 4, (int64_t(rgbc_[1]) * ir_[2]) << 4, (int64_t(rgbc_[2]) * ir_[3]) << 4);
        PushColor();
        break;
    case 0x12: Mvmva(command); break;
    case 0x1E: NormalColor(0, 0); break;
    case 0x20: for (int i = 0; i < 3; i++) NormalColor(i, 0); break;
    case 0x1B: NormalColor(0, 1); break;
    case 0x3F: for (int i = 0; i < 3; i++) NormalColor(i, 1); break;
    case 0x13: NormalColor(0, 2); break;
    case 0x16: for (int i = 0; i < 3; i++) NormalColor(i, 2); break;
    case 0x1C: ColorFromIr(1); break; // CC
    case 0x14: ColorFromIr(2); break; // CDP
    case 0x28: for (int i = 1; i <= 3; i++) SetMacAndIr(i, int64_t(ir_[i]) * ir_[i], lm_); break; // SQR
    case 0x3D: for (int i = 1; i <= 3; i++) SetMacAndIr(i, int64_t(ir_[i]) * ir_[0], lm_); PushColor(); break; // GPF
    case 0x3E: // GPL
        for (int i = 1; i <= 3; i++) SetMacAndIr(i, CheckMac(i, (int64_t(mac_[i]) << sf_) + int64_t(ir_[i]) * ir_[0]), lm_);
        PushColor();
        break;
    case 0x2D: case 0x2E: { // AVSZ3 / AVSZ4
        int64_t sum = function == 0x2D ? int64_t(zsf3_) * (sz_[1] + sz_[2] + sz_[3]) : int64_t(zsf4_) * (sz_[0] + sz_[1] + sz_[2] + sz_[3]);
        SetMac0(sum);
        int64_t otz = sum >> 12;
        if (otz < 0) { otz = 0; flag_ |= kFlagSz; } else if (otz > 0xFFFF) { otz = 0xFFFF; flag_ |= kFlagSz; }
        otz_ = uint16_t(otz);
        break;
    }
    default: break; // unknown function numbers do nothing on hardware either
    }
}

// ---------------------------------------------------------------- registers

uint32_t Gte::ReadData(uint32_t reg) {
    auto s16 = [](int16_t v) { return uint32_t(int32_t(v)); };
    switch (reg) {
    case 0: case 2: case 4: return uint16_t(v_[reg / 2][0]) | (uint32_t(uint16_t(v_[reg / 2][1])) << 16);
    case 1: case 3: case 5: return s16(v_[reg / 2][2]);
    case 6: return rgbc_[0] | (rgbc_[1] << 8) | (rgbc_[2] << 16) | (uint32_t(rgbc_[3]) << 24);
    case 7: return otz_;
    case 8: case 9: case 10: case 11: return s16(ir_[reg - 8]);
    case 12: case 13: case 14: return uint16_t(sxy_[reg - 12][0]) | (uint32_t(uint16_t(sxy_[reg - 12][1])) << 16);
    case 15: return uint16_t(sxy_[2][0]) | (uint32_t(uint16_t(sxy_[2][1])) << 16);
    case 16: case 17: case 18: case 19: return sz_[reg - 16];
    case 20: case 21: case 22: { const uint8_t* c = rgbFifo_[reg - 20]; return c[0] | (c[1] << 8) | (c[2] << 16) | (uint32_t(c[3]) << 24); }
    case 23: return res1_;
    case 24: case 25: case 26: case 27: return uint32_t(mac_[reg - 24]);
    case 28: case 29: {
        auto c = [&](int i) { return uint32_t(std::clamp(ir_[i] / 0x80, 0, 0x1F)); };
        return c(1) | (c(2) << 5) | (c(3) << 10);
    }
    case 30: return uint32_t(lzcs_);
    default: { // LZCR: leading bits equal to the sign bit
        uint32_t v = uint32_t(lzcs_ < 0 ? ~lzcs_ : lzcs_);
        uint32_t n = 0;
        for (uint32_t bit = 0x80000000u; bit && !(v & bit); bit >>= 1) n++;
        return n;
    }
    }
}

void Gte::WriteData(uint32_t reg, uint32_t value) {
    switch (reg) {
    case 0: case 2: case 4: v_[reg / 2][0] = int16_t(value); v_[reg / 2][1] = int16_t(value >> 16); break;
    case 1: case 3: case 5: v_[reg / 2][2] = int16_t(value); break;
    case 6: for (int i = 0; i < 4; i++) rgbc_[i] = uint8_t(value >> (i * 8)); break;
    case 7: otz_ = uint16_t(value); break;
    case 8: case 9: case 10: case 11: ir_[reg - 8] = int16_t(value); break;
    case 12: case 13: case 14: sxy_[reg - 12][0] = int16_t(value); sxy_[reg - 12][1] = int16_t(value >> 16); break;
    case 15: // SXYP: writing pushes the FIFO
        for (int i = 0; i < 2; i++) { sxy_[i][0] = sxy_[i + 1][0]; sxy_[i][1] = sxy_[i + 1][1]; }
        sxy_[2][0] = int16_t(value);
        sxy_[2][1] = int16_t(value >> 16);
        break;
    case 16: case 17: case 18: case 19: sz_[reg - 16] = uint16_t(value); break;
    case 20: case 21: case 22: for (int i = 0; i < 4; i++) rgbFifo_[reg - 20][i] = uint8_t(value >> (i * 8)); break;
    case 23: res1_ = value; break;
    case 24: case 25: case 26: case 27: mac_[reg - 24] = int32_t(value); break;
    case 28: // IRGB
        ir_[1] = int16_t((value & 0x1F) * 0x80);
        ir_[2] = int16_t(((value >> 5) & 0x1F) * 0x80);
        ir_[3] = int16_t(((value >> 10) & 0x1F) * 0x80);
        break;
    case 30: lzcs_ = int32_t(value); break;
    default: break; // ORGB and LZCR are read-only
    }
}

uint32_t Gte::ReadControl(uint32_t reg) {
    auto s16 = [](int16_t v) { return uint32_t(int32_t(v)); };
    auto pair = [](int16_t a, int16_t b) { return uint32_t(uint16_t(a)) | (uint32_t(uint16_t(b)) << 16); };
    auto matrix = [&](const int16_t m[3][3], uint32_t i) -> uint32_t {
        switch (i) {
        case 0: return pair(m[0][0], m[0][1]);
        case 1: return pair(m[0][2], m[1][0]);
        case 2: return pair(m[1][1], m[1][2]);
        case 3: return pair(m[2][0], m[2][1]);
        default: return s16(m[2][2]);
        }
    };
    if (reg <= 4) return matrix(rt_, reg);
    if (reg <= 7) return uint32_t(tr_[reg - 5]);
    if (reg <= 12) return matrix(llm_, reg - 8);
    if (reg <= 15) return uint32_t(bk_[reg - 13]);
    if (reg <= 20) return matrix(lcm_, reg - 16);
    if (reg <= 23) return uint32_t(fc_[reg - 21]);
    switch (reg) {
    case 24: return uint32_t(ofx_);
    case 25: return uint32_t(ofy_);
    case 26: return s16(int16_t(h_)); // hardware bug: H reads back sign-extended
    case 27: return s16(dqa_);
    case 28: return uint32_t(dqb_);
    case 29: return s16(zsf3_);
    case 30: return s16(zsf4_);
    default: return flag_ | ((flag_ & 0x7F87E000u) ? 0x80000000u : 0);
    }
}

void Gte::WriteControl(uint32_t reg, uint32_t value) {
    auto matrix = [&](int16_t m[3][3], uint32_t i) {
        const int16_t lo = int16_t(value), hi = int16_t(value >> 16);
        switch (i) {
        case 0: m[0][0] = lo; m[0][1] = hi; break;
        case 1: m[0][2] = lo; m[1][0] = hi; break;
        case 2: m[1][1] = lo; m[1][2] = hi; break;
        case 3: m[2][0] = lo; m[2][1] = hi; break;
        default: m[2][2] = lo; break;
        }
    };
    if (reg <= 4) { matrix(rt_, reg); transformDirty_ = true; return; }
    if (reg <= 7) { tr_[reg - 5] = int32_t(value); transformDirty_ = true; return; }
    if (reg <= 12) { matrix(llm_, reg - 8); return; }
    if (reg <= 15) { bk_[reg - 13] = int32_t(value); return; }
    if (reg <= 20) { matrix(lcm_, reg - 16); return; }
    if (reg <= 23) { fc_[reg - 21] = int32_t(value); return; }
    switch (reg) {
    case 24: ofx_ = int32_t(value); break;
    case 25: ofy_ = int32_t(value); break;
    case 26: h_ = uint16_t(value); break;
    case 27: dqa_ = int16_t(value); break;
    case 28: dqb_ = int32_t(value); break;
    case 29: zsf3_ = int16_t(value); break;
    case 30: zsf4_ = int16_t(value); break;
    default: flag_ = value & 0x7FFFF000u; break;
    }
}

} // namespace gt2
