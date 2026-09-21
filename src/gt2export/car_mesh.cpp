#include "gt2export/car_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "gt2formats/car_texture.h"
#include "gt2view/psx_gte.h"

namespace gt2 {
namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

double CarBodyMetresPerUnit(const CarLod& lod) {
    return std::ldexp(1.0, lod.scale - 16) / 4096.0;
}

std::array<WheelAxleDims, 2> DefaultWheelDims(const CarModel& model) { // 0x80061504
    std::array<WheelAxleDims, 2> d;
    d[0].radius = model.wheelRadiusFront;
    d[0].width = model.wheelWidthFront;
    d[1].radius = model.wheelRadiusRear;
    d[1].width = model.wheelWidthRear;
    return d;
}

WheelAxleDims RaceWheelDims(int16_t wheelRadius, int16_t width, int16_t tyreHeight, int16_t dish) { // 0x80017FA0
    WheelAxleDims d;
    d.radius = wheelRadius;
    d.width = width;
    d.ratio = int16_t(4096 - (wheelRadius != 0 ? (int32_t(tyreHeight) << 12) / int32_t(wheelRadius) : 0)); // MIPS div truncates
    d.dish = dish;
    return d;
}

int16_t WheelDishDepth(uint32_t configWord00, const std::array<int16_t, 4>& table) { return table[(configWord00 >> 8) & 3]; }

namespace {
constexpr uint32_t kWheelTemplateAddress[3] = {0x80091670u, 0x80091878u, 0x80091990u};
constexpr uint32_t kWheelDishTable = 0x80091A70u;
} // namespace

WheelTemplates LoadWheelTemplates(const GuestImage& exe) {
    WheelTemplates t;
    for (size_t lod = 0; lod < 3; lod++) {
        uint32_t a = exe.Sim(kWheelTemplateAddress[lod]);
        std::vector<int16_t>& s = t.lods[lod];
        for (int group = 0;; group++) {
            const uint32_t count = exe.Get<uint32_t>(a);
            if (group > 8 || count > 64) throw std::runtime_error("wheel template: unexpected group count");
            s.push_back(int16_t(uint16_t(count & 0xFFFF)));
            s.push_back(int16_t(uint16_t(count >> 16)));
            a += 4;
            if (count == 0) break;
            for (uint32_t i = 0; i < count * 6; i++, a += 2) s.push_back(exe.Get<int16_t>(a));
        }
    }
    return t;
}

std::array<int16_t, 4> LoadWheelDishDepths(const GuestImage& exe) {
    std::array<int16_t, 4> d{};
    const uint32_t a = exe.Sim(kWheelDishTable);
    for (uint32_t i = 0; i < 4; i++) d[i] = exe.Get<int16_t>(a + i * 2);
    return d;
}

WheelTemplates GeneratedWheelTemplates() {
    // An n-gon of apothem 4096 with vertices at (k + 1/2) * 360 / n degrees: group 0 walks clockwise from +half a
    // side (outer, inner pairs at +W/2), group 1 counter-clockwise (outer at +W/2, -W/2), group 2 pairs the upper and
    // the lower vertex of each column (-W/2).
    WheelTemplates t;
    const int sides[3] = {16, 8, 6};
    for (size_t lod = 0; lod < 3; lod++) {
        const int n = sides[lod];
        const double scale = 4096.0 / std::cos(kPi / n);
        auto x = [&](int k) { return int16_t(std::lround(scale * std::cos((2.0 * k + 1.0) * kPi / n))); };
        auto y = [&](int k) { return int16_t(std::lround(scale * std::sin((2.0 * k + 1.0) * kPi / n))); };
        std::vector<int16_t>& s = t.lods[lod];
        auto count = [&](int c) {
            s.push_back(int16_t(c));
            s.push_back(0);
        };
        auto record = [&](int16_t a, int16_t b, int16_t c, int16_t d, int16_t e, int16_t f) {
            for (int16_t v : {a, b, c, d, e, f}) s.push_back(v);
        };
        count(n + 1);
        for (int i = 0; i <= n; i++) record(x(-i), y(-i), 2048, 2048, x(-i), y(-i));
        count(n + 1);
        for (int i = 0; i <= n; i++) record(x(i), y(i), 2048, -2048, x(i), y(i));
        count(n / 2);
        for (int i = 0; i < n / 2; i++) record(x(i), y(i), -2048, -2048, x(-1 - i), y(-1 - i));
        count(0);
    }
    return t;
}

WheelArea GenerateWheelArea(const std::array<WheelAxleDims, 2>& dims, const WheelTemplates& templates) { // 0x80061308
    WheelArea a;
    auto mul = [](int32_t t, int32_t k) { return int16_t(int32_t(uint32_t(t) * uint32_t(k)) >> 12); }; // mult lo, sra 12
    for (size_t axle = 0; axle < 2; axle++) {
        const WheelAxleDims& d = dims[axle];
        a.rimZ[axle] = int16_t((int32_t(d.width) >> 1) - d.dish);
        const int16_t r = mul(d.radius, d.ratio);
        const int16_t corners[8] = {int16_t(-r), r, r, r, r, int16_t(-r), int16_t(-r), int16_t(-r)};
        std::copy(corners, corners + 8, a.square[axle].begin());
        for (size_t lod = 0; lod < 3; lod++) { // 0x800611F8
            const std::vector<int16_t>& t = templates.lods[lod];
            std::vector<int16_t>& out = a.lods[axle][lod];
            int32_t k = r;
            size_t i = 0;
            while (i + 1 < t.size()) {
                const uint32_t count = uint32_t(uint16_t(t[i])) | uint32_t(uint16_t(t[i + 1])) << 16;
                out.push_back(t[i]);
                out.push_back(t[i + 1]);
                i += 2;
                if (count == 0) break;
                for (uint32_t rec = 0; rec < count && i + 6 <= t.size(); rec++, i += 6) {
                    out.push_back(mul(t[i], d.radius));
                    out.push_back(mul(t[i + 1], d.radius));
                    out.push_back(mul(t[i + 2], d.width));
                    out.push_back(mul(t[i + 3], d.width));
                    out.push_back(mul(t[i + 4], k));
                    out.push_back(mul(t[i + 5], k));
                }
                k = d.radius; // `move t0, t1` after the first group
            }
        }
    }
    return a;
}

bool WheelQuadGrey(int32_t n1, int32_t n2, int8_t mirror) { // 0x80066EF8 (the colour of the next packet)
    const uint32_t t0 = ~uint32_t(n1);
    uint32_t t1 = mirror >= 0 ? (t0 & uint32_t(n2)) : (t0 | uint32_t(n2));
    t1 ^= uint32_t(int32_t(mirror));
    return (t1 & 0x80000000u) != 0;
}

namespace {
uint32_t RamU32(const uint8_t* ram, uint32_t a) {
    const uint8_t* p = ram + (a & 0x1FFFFF);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void RamPut32(uint8_t* ram, uint32_t a, uint32_t v) {
    uint8_t* p = ram + (a & 0x1FFFFF);
    for (int k = 0; k < 4; k++) p[k] = uint8_t(v >> (8 * k));
}
} // namespace

uint32_t EmitWheelStrips(uint8_t* ram, uint32_t stream, const WheelStripGte& gte, uint32_t packet, uint32_t otBase, uint16_t otShift, int8_t mirror,
                         uint32_t colourMask) {
    // The GTE's screen / depth FIFOs (their contents before the first transform only reach packet slots that are rewritten).
    uint32_t sxy[3] = {};
    uint16_t sz[4] = {};
    auto rtps = [&](uint32_t xy, uint32_t z) { // VXY0 = xy, VZ0 = low half of z; returns FLAG
        const gt2view::psx::RtpsResult r = gt2view::psx::Rtps(gte.rotation, gte.translation, gte.ofx, gte.ofy, gte.h, 4096, 0, int16_t(xy & 0xFFFF),
                                                              int16_t(xy >> 16), int16_t(z & 0xFFFF));
        sxy[0] = sxy[1];
        sxy[1] = sxy[2];
        sxy[2] = uint32_t(uint16_t(r.sx)) | uint32_t(uint16_t(r.sy)) << 16;
        sz[0] = sz[1];
        sz[1] = sz[2];
        sz[2] = sz[3];
        sz[3] = r.sz3;
        return r.flag;
    };
    auto nclip = [&] { // MAC0 of NCLIP on SXY0..2
        const int64_t x0 = int16_t(sxy[0] & 0xFFFF), y0 = int16_t(sxy[0] >> 16), x1 = int16_t(sxy[1] & 0xFFFF), y1 = int16_t(sxy[1] >> 16),
                      x2 = int16_t(sxy[2] & 0xFFFF), y2 = int16_t(sxy[2] >> 16);
        return int32_t(uint32_t(uint64_t(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1)));
    };
    uint32_t t3 = packet, t2 = stream;
    uint32_t count = RamU32(ram, t2);
    t2 += 4;
    while (count != 0) {
        uint32_t t4 = RamU32(ram, t2 + 4);
        const uint32_t fa = rtps(RamU32(ram, t2), t4);
        const uint32_t fb = rtps(RamU32(ram, t2 + 8), t4 >> 16);
        uint32_t t9 = 0, t1 = 0, t5 = 0xFFFFFFFFu, t6 = fa | fb;
        uint32_t nextXy = RamU32(ram, t2 + 12);
        t4 = RamU32(ram, t2 + 16);
        t2 += 12;
        const uint32_t end = t2 + count * 12;
        do {
            RamPut32(ram, t3 + 12, sxy[0]);
            RamPut32(ram, t3 + 20, sxy[2]);
            const uint32_t v0 = sxy[1];
            const uint16_t z[4] = {sz[0], sz[1], sz[2], sz[3]};
            const uint32_t t7 = rtps(nextXy, t4); // A of this pair
            RamPut32(ram, t3 + 8, t9);
            RamPut32(ram, t3 + 16, v0);
            t9 = v0;
            const bool farthest = (t1 >> 31) != 0; // grey: the largest SZ, black: the smallest
            int32_t depth = z[0];
            for (int k = 1; k < 4; k++)
                if ((depth < int32_t(z[k])) == farthest) depth = z[k];
            t1 = uint32_t(int32_t(t1) >> 31) >> 8;
            const int32_t n1 = nclip();
            t1 = (t1 & colourMask) | 0x28000000u;
            const uint32_t bxy = RamU32(ram, t2 + 8);
            const uint32_t a1 = rtps(bxy, t4 >> 16); // B of this pair
            nextXy = RamU32(ram, t2 + 12);
            t4 = RamU32(ram, t2 + 16);
            int32_t index = int32_t(uint32_t(depth) << otShift) >> 13;
            if (!(index < 4096)) index = 4095;
            const uint32_t entry = uint32_t(index) * 4 + otBase + 4;
            const uint32_t link = RamU32(ram, entry) & 0xFFFFFFu; // lwl: the entry's low three bytes
            RamPut32(ram, t3 + 4, t1);
            ram[(t3 + 3) & 0x1FFFFF] = 5;
            const int32_t n2 = nclip();
            if (!(int32_t(t5) < 0)) { // addPrim: the packet takes the entry's link, the entry points at the packet
                for (int k = 0; k < 3; k++) ram[(t3 + uint32_t(k)) & 0x1FFFFF] = uint8_t(link >> (8 * k));
                for (int k = 0; k < 3; k++) ram[(entry + uint32_t(k)) & 0x1FFFFF] = uint8_t(t3 >> (8 * k));
                t3 += 24;
            }
            t2 += 12;
            t5 = t6;
            t6 = t7 | a1;
            t5 |= t6;
            t1 = WheelQuadGrey(n1, n2, mirror) ? 0x80000000u : 0u; // only bit 31 is used by the next packet
        } while (t2 != end);
        count = RamU32(ram, t2 - 12);
        t2 -= 8;
    }
    return t3;
}

void BuildWheelMesh(std::vector<CarMeshVertex>& out, const WheelArea& area, int axle, const WheelMeshParams& w) {
    const float u = static_cast<float>(kCarWheelMetresPerUnit);
    // Wheel frame: (x, y) in the wheel plane, z along the axle (+z = outboard).
    auto place = [&](int x, int y, int z) {
        return CarMeshVertex{{w.centre[0] + float(z) * u * w.side, w.centre[1] + float(y) * u, w.centre[2] + float(x) * u}, {0, 0}, {0, 0, 0}, 0, false, false, false};
    };
    // The strips: per quad of consecutive pairs two one-sided copies. 0x80066EF8 colours a quad grey when it is seen from the
    // wheel's inside (its ring (A_k, A_k+1, B_k+1, B_k) turned the way the rule tests; the mirrored wheels' test is inverted
    // together with their mirrored matrix) and black when seen from outside: the copy facing the wheel centre is grey.
    const float grey = 128.0f / 255.0f;
    const std::vector<int16_t>& s = area.lods[size_t(axle)][0];
    struct Pair {
        CarMeshVertex a, b;
    };
    std::vector<std::vector<Pair>> groups;
    for (size_t i = 0; i + 1 < s.size();) {
        const uint32_t count = uint32_t(uint16_t(s[i])) | uint32_t(uint16_t(s[i + 1])) << 16;
        i += 2;
        if (count == 0) break;
        std::vector<Pair> g;
        for (uint32_t rec = 0; rec < count && i + 6 <= s.size(); rec++, i += 6)
            g.push_back({place(s[i], s[i + 1], s[i + 2]), place(s[i + 4], s[i + 5], s[i + 3])});
        groups.push_back(std::move(g));
    }
    auto tinted = [](CarMeshVertex v, const float c[3]) {
        std::copy(c, c + 3, v.color);
        v.cullBack = true;
        return v;
    };
    static constexpr float kBlack[3] = {0, 0, 0};
    for (size_t gi = 0; gi < groups.size(); gi++)
        for (size_t k = 0; k + 1 < groups[gi].size(); k++) {
            const std::vector<Pair>& g = groups[gi];
            const CarMeshVertex* ring[4] = {&g[k].a, &g[k + 1].a, &g[k + 1].b, &g[k].b};
            if (!w.twoFaces) { // one double-sided copy: sidewall and tread black, inboard disc grey
                for (int c : {0, 3, 2, 0, 2, 1}) {
                    CarMeshVertex v = *ring[c];
                    std::copy(gi == 2 ? w.stripGrey : kBlack, (gi == 2 ? w.stripGrey : kBlack) + 3, v.color);
                    out.push_back(v);
                }
                continue;
            }
            // the ring's right-handed normal against the direction from its centre to the wheel centre
            float e1[3], e2[3], n[3], toCentre[3];
            for (int c = 0; c < 3; c++) {
                e1[c] = ring[1]->pos[c] - ring[0]->pos[c];
                e2[c] = ring[3]->pos[c] - ring[0]->pos[c];
                toCentre[c] = w.centre[c] - (ring[0]->pos[c] + ring[1]->pos[c] + ring[2]->pos[c] + ring[3]->pos[c]) * 0.25f;
            }
            n[0] = e1[1] * e2[2] - e1[2] * e2[1];
            n[1] = e1[2] * e2[0] - e1[0] * e2[2];
            n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const bool ringFacesIn = n[0] * toCentre[0] + n[1] * toCentre[1] + n[2] * toCentre[2] > 0;
            const float* ringColour = ringFacesIn ? w.stripGrey : kBlack;
            const float* reverseColour = ringFacesIn ? kBlack : w.stripGrey;
            for (int c : {0, 1, 2, 0, 2, 3}) out.push_back(tinted(*ring[c], ringColour));
            for (int c : {0, 3, 2, 0, 2, 1}) out.push_back(tinted(*ring[c], reverseColour));
        }
    // The rim: two POLY_FT4 at z = W / 2 - dish, split on the (r, r) - (-r, -r) diagonal through the centre.
    const std::array<int16_t, 8>& c = area.square[size_t(axle)];
    const int z = area.rimZ[size_t(axle)];
    auto rim = [&](int x, int y, int tu, int tv) {
        CarMeshVertex m = place(x, y, z);
        m.texel[0] = float(tu) + 0.5f;
        m.texel[1] = float(tv) + 0.5f;
        m.color[0] = m.color[1] = m.color[2] = grey; // 0x80 = neutral modulation
        m.textured = true;
        m.palette = 0; // the rim CLUT: the car texture's CLUT 0 unless wheels were fitted (rim = true selects the wheel CLUT)
        m.rim = true;
        return m;
    };
    const CarMeshVertex c0 = rim(c[0], c[1], 0, 0), c1 = rim(c[2], c[3], 47, 0), c2 = rim(c[4], c[5], 47, 47), c3 = rim(c[6], c[7], 0, 47);
    const CarMeshVertex centre = rim(0, 0, 23, 23);
    for (const CarMeshVertex* v : {&c1, &c0, &centre, &c0, &centre, &c3, &c1, &centre, &c2, &c2, &centre, &c3}) out.push_back(*v);
}

void BuildWheelMesh(std::vector<CarMeshVertex>& out, float centre[3], float side, float radius, float width) {
    std::array<WheelAxleDims, 2> dims;
    dims[0].radius = dims[1].radius = int16_t(std::lround(radius / kCarWheelMetresPerUnit));
    dims[0].width = dims[1].width = int16_t(std::lround(width / kCarWheelMetresPerUnit));
    static const WheelTemplates templates = GeneratedWheelTemplates();
    const WheelArea area = GenerateWheelArea(dims, templates);
    WheelMeshParams p;
    std::copy(centre, centre + 3, p.centre);
    p.side = side;
    BuildWheelMesh(out, area, 0, p);
}

void WheelModelMatrix(int wheel, const float centre[3], int16_t steerAngle, int16_t camber, uint16_t rotation, float out[16]) {
    constexpr double kTurn = 6.283185307179586 / 4096.0;
    auto rotY = [&](double a, double m[3][3]) {
        const double c = std::cos(a * kTurn), s = std::sin(a * kTurn);
        const double r[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
        std::memcpy(m, r, sizeof(r));
    };
    auto mul = [](const double a[3][3], const double b[3][3], double o[3][3]) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    };
    const int odd = wheel & 1;
    const double yaw = double(int32_t(steerAngle) - 1024 + (odd ? 4096 : 0)); // odd: + 1024 + 2048 = - 1024 + 4096
    const double pitch = odd ? double(camber) : -double(camber);
    const double roll = double(int16_t(rotation));
    double ry[3][3], rx[3][3], rz[3][3], back[3][3], t0[3][3], t1[3][3], m[3][3];
    rotY(yaw, ry);
    const double cx = std::cos(-pitch * kTurn), sx = std::sin(-pitch * kTurn);
    const double rxv[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    std::memcpy(rx, rxv, sizeof(rx));
    const double cz = std::cos(roll * kTurn), sz = std::sin(roll * kTurn);
    const double rzv[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    std::memcpy(rz, rzv, sizeof(rz));
    rotY(1024.0, back); // undoes the baked Ry(-1024) (the baked mirror commutes with Rz and stays on the right)
    mul(ry, rx, t0);
    mul(t0, rz, t1);
    mul(t1, back, m);
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) out[col * 4 + row] = float(m[row][col]);
        out[col * 4 + 3] = 0;
    }
    out[12] = centre[0];
    out[13] = centre[1];
    out[14] = centre[2];
    out[15] = 1;
}

float CarShadowHeight(const CarModel& model) {
    const float ws = static_cast<float>(kCarWheelMetresPerUnit);
    return (model.wheels[0].y - model.wheelRadiusFront) * ws + 0.04f;
}

void GroundShadowMatrix(float* ground, float meshHeight) {
    for (int axis=0; axis<3; ++axis) ground[12+axis] += ground[4+axis]*(0.04f-meshHeight);
}

std::vector<CarMeshVertex> BuildCarShadowMesh(const CarModel& model, float groundY) {
    std::vector<CarMeshVertex> out;
    if (model.lods.empty() || model.shadow.polygons.empty()) return out;
    const float s = std::ldexp(1.0f, model.shadow.scale - 28);
    auto corner = [&](const CarShadowPolygon& p, size_t c) {
        const auto& v = model.shadow.vertices[p.vertex[c]];
        const float shade = (p.fullyShaded || c >= 2) ? 1.0f : 0.0f;
        return CarMeshVertex{{v[0] * s, groundY, v[1] * s}, {0, 0}, {shade, shade, shade}, 0, false, false, false};
    };
    for (const CarShadowPolygon& p : model.shadow.polygons) {
        static constexpr size_t kTri[3] = {0, 1, 2}, kQuad[6] = {0, 1, 2, 0, 2, 3}; // ring order (corners 0-1 outer, 2-3 inner)
        const size_t* order = p.quad ? kQuad : kTri;
        for (size_t k = 0; k < (p.quad ? 6u : 3u); k++) out.push_back(corner(p, order[k]));
    }
    return out;
}

std::vector<CarMeshVertex> BuildCarMesh(const CarModel& model, const CarMeshOptions& opt) {
    const CarLod& lod = model.lods.at(opt.lod);
    const float s = static_cast<float>(CarBodyMetresPerUnit(lod));
    std::vector<CarMeshVertex> out;

    for (const CarPolygon& p : lod.polygons) {
        static constexpr int kTri[3] = {0, 1, 2};
        static constexpr int kQuadRing[6] = {0, 1, 3, 1, 2, 3}; // the PS1's split: GPU slots (v0 v1 v3 v2) -> (v0 v1 v3) + (v1 v2 v3)
        static constexpr int kQuadStrip[6] = {0, 1, 2, 1, 3, 2};
        const int* order = !p.IsQuad() ? kTri : (opt.quadStripOrder ? kQuadStrip : kQuadRing);
        for (int k = 0; k < (p.IsQuad() ? 6 : 3); k++) {
            int c = order[k];
            const CarVertex& v = lod.vertices[p.vertex[c]];
            out.push_back({{v.x * s, v.y * s, v.z * s},
                           {p.u[c] + 0.5f, p.v[c] + 0.5f},
                           {p.r / 255.0f, p.g / 255.0f, p.b / 255.0f},
                           p.palette,
                           p.IsTextured(),
                           (p.primCode & CarPolygon::kCodeRawTexture) != 0});
        }
    }

    if (opt.wheels) {
        static const WheelTemplates generated = GeneratedWheelTemplates();
        const WheelArea fallback = opt.wheelArea ? WheelArea{} : GenerateWheelArea(DefaultWheelDims(model), generated);
        const WheelArea& area = opt.wheelArea ? *opt.wheelArea : fallback;
        const float ws = static_cast<float>(kCarWheelMetresPerUnit);
        for (size_t i = 0; i < 4; i++) {
            // .cdo 0x20 + i * 8: { s16 outerFace, y, x, z }, 1/4096 m: z lateral (centre; the first field is the
            // outboard face = z +- width / 2), x longitudinal (negative = front), y height. Verified against the
            // wheel transforms captured from the original (offsets 1497 / 3033 / 2523 in LOD0 units of 2/4096 m
            // = 2965 / 6064 / 5045 here).
            const CarWheelOffset& w = model.wheels[i];
            WheelMeshParams params;
            params.centre[0] = w.z * ws;
            params.centre[1] = w.y * ws;
            params.centre[2] = w.x * ws;
            params.side = w.z < 0 ? -1.0f : 1.0f;
            for (int c = 0; c < 3; c++) params.stripGrey[c] = model.wheelDishColor[size_t(c)] / 255.0f; // .cdo + 0x08: the LOD 0 strip colour
            params.twoFaces = opt.wheelStripFaces;
            if (opt.wheelCentres) (*opt.wheelCentres)[i] = {params.centre[0], params.centre[1], params.centre[2]};
            if (opt.wheelsAtOrigin) params.centre[0] = params.centre[1] = params.centre[2] = 0;
            const uint32_t first = uint32_t(out.size());
            BuildWheelMesh(out, area, i < 2 ? 0 : 1, params);
            if (opt.wheelRanges) (*opt.wheelRanges)[i] = {first, uint32_t(out.size()) - first};
        }
    }
    return out;
}

} // namespace gt2
