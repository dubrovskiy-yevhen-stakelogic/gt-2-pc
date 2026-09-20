#include "game/menu/menu_car.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "game/sim/trig.h"
#include "gt2formats/car_info.h"
#include "gt2vfs/gtfs.h"

namespace gt2::menu {

namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("carlogo TIM: short");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(U16(b, o)) | (uint32_t(U16(b, o + 2)) << 16); }

int16_t Sat16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -0x8000, 0x7FFF)); }
int32_t Shr12(int64_t v) { return int32_t(v >> 12); } // arithmetic (floor), as the GTE's sf = 1

using Mat = std::array<std::array<int16_t, 3>, 3>;

// 0x8007B994(out, a, b): MVMVA with a as the rotation and the columns of b as vectors, sf = 1, lm = 0: out = a * b.
Mat Multiply(const Mat& a, const Mat& b) {
    Mat out{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int64_t s = 0;
            for (int k = 0; k < 3; k++) s += int64_t(a[size_t(i)][size_t(k)]) * b[size_t(k)][size_t(j)];
            out[size_t(i)][size_t(j)] = Sat16(Shr12(s));
        }
    return out;
}

// 0x8008220C(out, m, v, t): out = m * v / 4096 + t with v split into 12-bit parts (low: MVMVA sf 1 -> IR; high:
// sf 0 -> IR, x 4096; middle: sf 0 -> MAC).
std::array<int32_t, 3> ApplyLong(const Mat& m, const std::array<int32_t, 3>& v, const std::array<int32_t, 3>& t) {
    std::array<int32_t, 3> out{};
    for (int i = 0; i < 3; i++) {
        int64_t low = 0, mid = 0, high = 0;
        for (int k = 0; k < 3; k++) {
            const int32_t x = v[size_t(k)];
            low += int64_t(m[size_t(i)][size_t(k)]) * (x & 0xFFF);
            mid += int64_t(m[size_t(i)][size_t(k)]) * ((x >> 12) & 0xFFF);
            high += int64_t(m[size_t(i)][size_t(k)]) * int16_t(x >> 24);
        }
        out[size_t(i)] = int32_t(uint32_t(Sat16(Shr12(low))) + uint32_t(Sat16(high)) * 0x1000u + uint32_t(int32_t(mid)) + uint32_t(t[size_t(i)]));
    }
    return out;
}

// The GTE's unsigned Newton-Raphson division of RTPS: min(0x1FFFF, H * 0x10000 / SZ3) rounded like the hardware
// (reciprocal table generated from its defining formula).
uint32_t GteDivide(uint32_t h, uint32_t sz3) {
    if (!(h < sz3 * 2)) return 0x1FFFF;
    static const std::array<uint8_t, 0x101> table = [] {
        std::array<uint8_t, 0x101> t{};
        for (int i = 0; i < 0x101; i++) t[size_t(i)] = uint8_t(std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101));
        return t;
    }();
    int z = 0;
    while (z < 16 && !((sz3 << z) & 0x8000u)) z++;
    const uint64_t n = uint64_t(h) << z;
    uint32_t d = (sz3 << z) & 0xFFFFu;
    const uint32_t u = uint32_t(table[(d - 0x7FC0u) >> 7]) + 0x101u;
    d = uint32_t((0x2000080u - uint64_t(d) * u) >> 8);
    d = uint32_t((0x0000080u + uint64_t(d) * u) >> 8);
    const uint64_t q = (n * d + 0x8000u) >> 16;
    return q > 0x1FFFF ? 0x1FFFFu : uint32_t(q);
}

int LeadingZeros(uint32_t v) { // LZCS of a non-negative value
    int n = 0;
    while (n < 32 && !(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------- logo

MenuCarLogo ParseMenuCarLogo(std::span<const uint8_t> tim) {
    if (U32(tim, 0) != 0x10) throw std::runtime_error("carlogo: not a TIM");
    MenuCarLogo logo;
    size_t at = 8;
    if (U32(tim, 4) & 8) {
        const uint32_t length = U32(tim, at);
        logo.clutWords = U16(tim, at + 8);
        logo.clutRows = U16(tim, at + 10);
        const size_t bytes = size_t(logo.clutWords) * logo.clutRows * 2;
        if (at + 12 + bytes > tim.size()) throw std::runtime_error("carlogo: short CLUT");
        logo.clut.assign(tim.begin() + std::ptrdiff_t(at + 12), tim.begin() + std::ptrdiff_t(at + 12 + bytes));
        at += length;
    }
    logo.imageWords = U16(tim, at + 8);
    logo.imageRows = U16(tim, at + 10);
    const size_t bytes = size_t(logo.imageWords) * logo.imageRows * 2;
    if (at + 12 + bytes > tim.size()) throw std::runtime_error("carlogo: short image");
    logo.image.assign(tim.begin() + std::ptrdiff_t(at + 12), tim.begin() + std::ptrdiff_t(at + 12 + bytes));
    return logo;
}

std::optional<MenuCarLogo> LoadMenuCarLogo(const GtfsVolume& vol, uint32_t carId) {
    auto exists = [&](const std::string& p) { return vol.Find(p) || vol.Find(p + ".gz"); };
    const std::string id = UnpackCarId(carId);
    for (const char region : {'n', 'l'}) {
        const std::string path = "carlogo/" + id + region + "--.tim";
        if (exists(path)) return ParseMenuCarLogo(vol.Read(path));
    }
    for (const GtfsEntry& f : vol.Files()) // no logo of its own: the first file of the directory
        if (f.path.rfind("carlogo/", 0) == 0) return ParseMenuCarLogo(vol.Read(f));
    return std::nullopt;
}

void UploadMenuCarLogo(MenuVram& vram, const MenuCarLogo& logo) {
    if (!logo.clut.empty()) vram.Upload(MenuCarLogo::kClutX, MenuCarLogo::kClutY, logo.clutWords, logo.clutRows, logo.clut);
    vram.Upload(MenuCarLogo::kImageX, MenuCarLogo::kImageY, logo.imageWords, logo.imageRows, logo.image);
}

MenuPrim MenuCarLogoSprite(const MenuCarLogo& logo, int cx, int cy) {
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.w = int16_t(logo.Width());
    p.h = int16_t(logo.Height());
    p.x[0] = int16_t(cx - (p.w >> 1));
    p.y[0] = int16_t(cy - (p.h >> 1));
    p.u = 0;
    p.v = uint8_t(MenuCarLogo::kImageY & 0xFF);
    p.tpage = MenuCarLogo::kTpage;
    p.clut = MenuCarLogo::kClut;
    p.colour[0] = 0x808080;
    return p;
}

// ---------------------------------------------------------------- camera

void MenuCarCamera::Reset() {
    floor = true;
    pitch = kResetPitch;
    yaw = kResetYaw;
    floorSemi = false;
    floorColour = kFloorColour;
}

void MenuCarCamera::Update(bool washing) {
    pitch = kPitch;
    yaw = int16_t((yaw + kYawStep) & 0x3FFF);
    if (washing) yaw = int16_t((yaw + kWashYawStep) & 0x3FFF);
}

MenuCarProjection MenuCarProject(const MenuItem& viewport, const MenuCarCamera& camera, bool withYaw) {
    MenuCarProjection p;
    p.x0 = viewport.x0;
    p.y0 = viewport.y0;
    p.w = int16_t(viewport.x1 - viewport.x0);
    p.h = int16_t(viewport.y1 - viewport.y0);
    const int16_t a = int16_t(((int32_t(p.w) * 0x100) / int32_t(p.h)) >> 1); // 0x8001B9AC case 0x0A
    p.left = int16_t(-a);
    p.right = a;
    // 0x8007B320(cam, left, right, top, bottom, H, far)
    const int32_t centreX = ((int32_t(p.left) + p.right) >> 1 << 12) / p.distance;
    const int32_t centreY = ((int32_t(p.top) + p.bottom) >> 1 << 12) / p.distance;
    const int32_t spanX = int16_t(p.right - p.left), spanY = int16_t(p.top - p.bottom);
    // 0x8007B374(cam, V, ot, 16): the screen matrix P
    const int32_t sx = (int32_t(p.w) << 12) / spanX, sy = (int32_t(p.h) << 12) / spanY;
    const int32_t cx = int32_t(int16_t(centreX)) * sx, cy = int32_t(int16_t(centreY)) * sy;
    const Mat screen = {{{int16_t(sx), 0, int16_t(cx >> 12)}, {0, int16_t(-sy), int16_t(-(cy >> 12))}, {0, 0, -4096}}};
    // 0x8001A8A4: V = I, Ry(-yaw) (0x8007B14C), Rx(-pitch) (0x8007B0C4), translated by (0, 0, distance) (0x8007B050)
    Mat v = {{{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}}};
    if (withYaw) {
        const uint32_t ang = uint32_t(-int32_t(camera.yaw)) & 0xFFF;
        const int16_t c = int16_t(sim::Cos(ang)), s = int16_t(sim::Sin(ang));
        v = Multiply(v, Mat{{{c, 0, s}, {0, 4096, 0}, {int16_t(-s), 0, c}}});
    }
    {
        const uint32_t ang = uint32_t(-int32_t(camera.pitch)) & 0xFFF;
        const int16_t c = int16_t(sim::Cos(ang)), s = int16_t(sim::Sin(ang));
        v = Multiply(v, Mat{{{4096, 0, 0}, {0, c, int16_t(-s)}, {0, s, c}}});
    }
    p.view = v;
    p.eye = ApplyLong(v, {0, 0, MenuCarCamera::kDistance}, {0, 0, 0});
    for (int i = 0; i < 3; i++) p.negEye[size_t(i)] = int32_t(0u - uint32_t(p.eye[size_t(i)]));
    Mat vt{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) vt[size_t(i)][size_t(j)] = v[size_t(j)][size_t(i)]; // 0x80082324
    p.rotation = Multiply(screen, vt);
    p.ofx = int32_t(p.w) << 15;
    p.ofy = int32_t(p.h) << 15;
    p.H = p.distance;
    return p;
}

// ---------------------------------------------------------------- the race overlay's model views

namespace {
// 0x80083AE0(&seed): seed = seed * 17 + 17, returns seed ^ (seed rotated by 16).
uint32_t OverlayRandom(uint32_t& seed) {
    seed = seed * 17u + 17u;
    return seed ^ ((seed << 16) | (seed >> 16));
}
} // namespace

OverlayModelCamera ResultsModelCamera(uint32_t vsyncCounter) {
    OverlayModelCamera c;
    c.floor = true;
    c.pitch = 0x5E;
    c.yaw = 0x1500;
    c.position = {0, 0, 0x80000};
    c.w = 0x160, c.h = 300;
    c.left = -0xB0, c.right = 0xB0, c.top = 0x85, c.bottom = -0x34, c.distance = 400;
    c.floorSemi = false;
    c.floorColour = 0x3E3E3E;
    c.roll = 0;
    c.x = c.y = 0;
    c.farZ = 0x7FFF;
    uint32_t seed = vsyncCounter;
    c.yaw = int16_t(OverlayRandom(seed) & 0xFFF);
    OverlayRandom(seed);
    OverlayRandom(seed);
    c.pitch = int16_t((OverlayRandom(seed) & 0x7F) + 0x50);
    return c;
}

void TurnModelCamera(OverlayModelCamera& camera, int16_t step, int32_t frameLength) {
    camera.yaw = int16_t((camera.yaw + step) & 0x3FFF);
    if (frameLength > 0xFB90) camera.position[2] = int32_t(uint32_t(camera.position[2]) + 0x8000u);
}

OverlayModelCamera ModelViewCamera(int16_t w, int16_t h) {
    OverlayModelCamera c;
    c.floor = true;
    c.floorSemi = true;
    c.pitch = 0x5E;
    c.w = w;
    c.yaw = 0x1500;
    c.h = h;
    const int32_t a = (int32_t(h) * 0x3E) / 100;
    c.floorColour = 0xA2A2A2;
    c.roll = 0;
    c.x = c.y = 0;
    c.right = int16_t(w >> 1);
    c.position = {0, 0, 0xB0000};
    c.left = int16_t(-c.right);
    c.distance = 400;
    c.farZ = 0x7FFF;
    c.top = int16_t((a * 0x50) / 100);
    c.bottom = int16_t(-(a / 5));
    return c;
}

void ChampionModelMotion(OverlayModelCamera& camera, int32_t t) {
    camera.position[2] = 0xC0000;
    if (t < 0x5A) camera.position[2] = (t * 0x80000) / 0x5A + 0x40000;
    camera.roll = -0x40;
    if (t < 0x5A) camera.roll = int16_t(-((0x5A - int16_t(t)) * 6 + 0x40));
    camera.position[1] = 0x4CCC;
    camera.yaw = int16_t((camera.yaw + 8) & 0x3FFF);
}

MenuCarProjection OverlayModelProject(const OverlayModelCamera& camera, int16_t envX, int16_t envY, bool withYaw) {
    MenuCarProjection p;
    p.x0 = envX;
    p.y0 = envY;
    p.w = camera.w;
    p.h = camera.h;
    p.left = camera.left, p.right = camera.right, p.top = camera.top, p.bottom = camera.bottom, p.distance = camera.distance;
    // 0x8007B320(cam, left, right, top, bottom, H, far) / 0x8007B374(cam, V, ot, 16): as MenuCarProject.
    const int32_t centreX = ((int32_t(p.left) + p.right) >> 1 << 12) / p.distance;
    const int32_t centreY = ((int32_t(p.top) + p.bottom) >> 1 << 12) / p.distance;
    const int32_t spanX = int16_t(p.right - p.left), spanY = int16_t(p.top - p.bottom);
    const int32_t sx = (int32_t(p.w) << 12) / spanX, sy = (int32_t(p.h) << 12) / spanY;
    const int32_t cx = int32_t(int16_t(centreX)) * sx, cy = int32_t(int16_t(centreY)) * sy;
    const Mat screen = {{{int16_t(sx), 0, int16_t(cx >> 12)}, {0, int16_t(-sy), int16_t(-(cy >> 12))}, {0, 0, -4096}}};
    Mat v = {{{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}}};
    if (withYaw) { // 0x8007B14C
        const uint32_t ang = uint32_t(-int32_t(camera.yaw)) & 0xFFF;
        const int16_t c = int16_t(sim::Cos(ang)), s = int16_t(sim::Sin(ang));
        v = Multiply(v, Mat{{{c, 0, s}, {0, 4096, 0}, {int16_t(-s), 0, c}}});
    }
    { // 0x8007B0C4
        const uint32_t ang = uint32_t(-int32_t(camera.pitch)) & 0xFFF;
        const int16_t c = int16_t(sim::Cos(ang)), s = int16_t(sim::Sin(ang));
        v = Multiply(v, Mat{{{4096, 0, 0}, {0, c, int16_t(-s)}, {0, s, c}}});
    }
    if (withYaw) { // 0x8007B1D4 (the floor's camera has no roll)
        const uint32_t ang = uint32_t(-int32_t(camera.roll)) & 0xFFF;
        const int16_t c = int16_t(sim::Cos(ang)), s = int16_t(sim::Sin(ang));
        v = Multiply(v, Mat{{{c, int16_t(-s), 0}, {s, c, 0}, {0, 0, 4096}}});
    }
    p.view = v;
    p.eye = ApplyLong(v, camera.position, {0, 0, 0}); // 0x8007B050
    for (int i = 0; i < 3; i++) p.negEye[size_t(i)] = int32_t(0u - uint32_t(p.eye[size_t(i)]));
    Mat vt{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) vt[size_t(i)][size_t(j)] = v[size_t(j)][size_t(i)];
    p.rotation = Multiply(screen, vt);
    p.ofx = int32_t(p.w) << 15;
    p.ofy = int32_t(p.h) << 15;
    p.H = p.distance;
    return p;
}

std::vector<MenuPrim> OverlayModelFloor(const MenuCarProjection& floorProjection, const OverlayModelCamera& camera) {
    MenuCarCamera c;
    c.floor = camera.floor;
    c.floorSemi = camera.floorSemi;
    c.floorColour = camera.floorColour;
    return MenuCarFloor(floorProjection, c);
}

MenuCarScreenPoint MenuCarTransform(const MenuCarProjection& p, const std::array<int32_t, 3>& world) {
    std::array<int32_t, 3> t{};
    uint32_t magnitude = 0;
    for (int i = 0; i < 3; i++) {
        t[size_t(i)] = int32_t(uint32_t(world[size_t(i)]) + uint32_t(p.negEye[size_t(i)]));
        magnitude |= t[size_t(i)] < 0 ? 0u - uint32_t(t[size_t(i)]) : uint32_t(t[size_t(i)]);
    }
    int shift = 31 - LeadingZeros(magnitude) - 12;
    if (shift < 0) shift = 0;
    int16_t v[3];
    for (int i = 0; i < 3; i++) v[i] = int16_t(t[size_t(i)] >> shift);
    int64_t mac[3];
    int16_t ir[3];
    for (int i = 0; i < 3; i++) {
        int64_t s = 0;
        for (int k = 0; k < 3; k++) s += int64_t(p.rotation[size_t(i)][size_t(k)]) * v[k];
        mac[i] = s >> 12;
        ir[i] = Sat16(mac[i]);
    }
    const uint32_t sz3 = uint32_t(std::clamp<int64_t>(mac[2], 0, 0xFFFF));
    const int64_t n = GteDivide(uint32_t(p.H), sz3);
    MenuCarScreenPoint out;
    out.x = int16_t(std::clamp<int64_t>((n * ir[0] + p.ofx) >> 16, -0x400, 0x3FF));
    out.y = int16_t(std::clamp<int64_t>((n * ir[1] + p.ofy) >> 16, -0x400, 0x3FF));
    out.z = int16_t((int32_t(uint32_t(int32_t(ir[2])) << shift)) >> 13);
    return out;
}

std::vector<MenuPrim> MenuCarFloor(const MenuCarProjection& p, const MenuCarCamera& camera) {
    std::vector<MenuPrim> out;
    if (!camera.floor) return out;
    std::array<MenuCarScreenPoint, 26> s{};
    for (int i = 0; i < 25; i++) { // 0x8006C274
        const uint32_t ang = uint32_t((i << 12) / 24) & 0xFFF;
        const int32_t x = ((1024 * sim::Cos(ang)) >> 12) << 8, z = ((1024 * sim::Sin(ang)) >> 12) << 8;
        s[size_t(i)] = MenuCarTransform(p, {x, 0, z});
    }
    s[25] = MenuCarTransform(p, {0, 0, 0});
    struct Tri { int z; int order; MenuPrim prim; };
    std::vector<Tri> tris;
    for (int i = 0; i < 24; i++) { // 0x8006C31C
        MenuPrim t;
        t.kind = MenuPrim::kPolyG4;
        t.gouraud = true;
        const MenuCarScreenPoint* c[4] = {&s[25], &s[size_t(i)], &s[size_t(i + 1)], &s[size_t(i + 1)]};
        for (int k = 0; k < 4; k++) {
            t.x[k] = int16_t(c[k]->x + p.x0);
            t.y[k] = int16_t(c[k]->y + p.y0);
        }
        t.colour[0] = camera.floorColour & 0xFFFFFF;
        t.colour[1] = t.colour[2] = t.colour[3] = 0;
        t.semi = camera.floorSemi;
        t.tpage = 0x200; // 0x8007DA44(ot, 0x200): dither on, page 0, mode 0
        t.dither = true;
        t.clipX0 = p.x0; // the car environment's drawing area
        t.clipY0 = p.y0;
        t.clipX1 = int16_t(p.x0 + p.w - 1);
        t.clipY1 = int16_t(p.y0 + p.h - 1);
        const int z = std::max<int>({s[25].z, s[size_t(i)].z, s[size_t(i + 1)].z});
        tris.push_back({z, i, t});
    }
    // The OT is walked from the far end; primitives of one entry come out newest first (0x8007E708 prepends).
    std::stable_sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.z != b.z ? a.z > b.z : a.order > b.order; });
    for (const Tri& t : tris) out.push_back(t.prim);
    return out;
}

} // namespace gt2::menu

// ---------------------------------------------------------------- bought wheels

namespace gt2::menu {

uint32_t MenuWheelIdOfName(const GuestImage& exe, const std::string& name) { // 0x80011570
    if (name.size() < 8) throw std::invalid_argument("wheel file name shorter than 8 characters");
    auto c = [&](size_t i) { return int32_t(int8_t(name[i])); };
    const int32_t pairOfName = (c(0) | c(1) << 8) & 0xFFFF; // lb + lb << 8, compared with an lhu
    uint32_t maker = 0;
    for (uint32_t a = 0x80033DD0u;; a += 2) { // u16 pairs, 0-terminated (EXE data, read at boot)
        const uint32_t pair = exe.Get<uint16_t>(a);
        if (pair == 0 || int32_t(pair) == pairOfName) break;
        if (++maker > 64) throw std::runtime_error("wheel maker list (EXE 0x80033DD0) not terminated");
    }
    const int32_t number = (c(2) - 0x30) * 100 + (c(3) - 0x30) * 10 - 0x30 + c(4);
    uint32_t kind = 0;
    if (c(6) == '5') kind = 2;
    else if (c(6) == '4') kind = 1;
    else if (c(6) == '6') kind = 3;
    return ((uint32_t(int32_t(maker << 12) | number) << 3 | kind) << 13) | uint32_t(c(7));
}

MenuWheelFiles LoadMenuWheelFiles(const GtfsVolume& vol, const GuestImage& exe) { // 0x8001194C
    std::vector<const GtfsEntry*> entries;
    for (const GtfsEntry& f : vol.Files())
        if (f.path.rfind("carwheel/", 0) == 0) entries.push_back(&f);
    std::sort(entries.begin(), entries.end(), [](const GtfsEntry* a, const GtfsEntry* b) { return a->index < b->index; });
    MenuWheelFiles files;
    for (const GtfsEntry* e : entries) {
        files.ids.push_back(MenuWheelIdOfName(exe, e->path.substr(9)));
        files.paths.push_back(e->path);
    }
    return files;
}

int MenuWheelFile(const MenuWheelFiles& files, uint32_t word) { // 0x800615E8 + 0x80060D74
    word &= 0xFFFFE0FFu; // 0x800615E8 masks the dish / colour code (bits 8..12) before the search
    if (word == 0 || files.ids.empty()) return -1;
    int32_t lo = 0, hi = int32_t(files.ids.size());
    for (;;) {
        const int32_t mid = (lo + hi) >> 1;
        // mid == count only when lo == hi == count: the original compares the word after the table once, then stops.
        const bool inTable = mid < int32_t(files.ids.size());
        const uint32_t id = inTable ? files.ids[size_t(mid)] : 0;
        if (inTable && id == word) return mid;
        if (hi <= lo) break;
        if (id < word) lo = mid + 1;
        else hi = mid;
    }
    return 0; // not found: the base file number = the directory's first file
}

int16_t MenuWheelDish(const GuestImage& exe, int index) { return exe.Get<int16_t>(0x80091A70u + uint32_t(index) * 2u); }

MenuWheelTexture LoadMenuWheelTexture(const GtfsVolume& vol, const std::string& path) {
    const MenuCarLogo tim = ParseMenuCarLogo(vol.Read(path)); // the same 4-bit TIM layout (CLUT block, image block)
    MenuWheelTexture w;
    if (tim.clut.size() < 32) throw std::runtime_error(path + ": no 16-colour CLUT");
    for (size_t i = 0; i < 16; i++) w.clut[i] = uint16_t(tim.clut[2 * i] | tim.clut[2 * i + 1] << 8);
    w.words = tim.imageWords;
    w.rows = tim.imageRows;
    w.image.resize(size_t(w.words) * size_t(w.rows));
    for (size_t i = 0; i < w.image.size(); i++) w.image[i] = uint16_t(tim.image[2 * i] | tim.image[2 * i + 1] << 8);
    return w;
}

} // namespace gt2::menu
