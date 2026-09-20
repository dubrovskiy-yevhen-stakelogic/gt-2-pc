#include "gt2view/glow.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/sim/trig.h"
#include "gt2view/psx_gte.h"

namespace gt2view {

namespace {

int16_t S16(const uint8_t* p) { return int16_t(uint16_t(p[0] | (p[1] << 8))); }
int32_t S32(const uint8_t* p) { return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24); }
uint32_t U32(int32_t v) { return uint32_t(v); }

// The two sine-table entries the drawers load (0x80093150 + 0x200 / + 0xA00): sin 22.5 and sin 112.5 = cos 22.5.
int32_t StarSin() { return gt2::sim::Sin(256); }
int32_t StarCos() { return gt2::sim::Sin(1280); }

// 0x80020FD8: the sort key of a chunk, the camera distance of its centre as max + mid / 2 + min / 4 of |centre + offset|.
uint32_t ChunkKey(const GlowView& v, const std::array<int32_t, 3>& centre) {
    uint32_t a = U32(centre[0]) + U32(v.offset[0]), b = U32(centre[1]) + U32(v.offset[1]), c = U32(centre[2]) + U32(v.offset[2]);
    if (int32_t(a) < 0) a = 0u - a;
    if (int32_t(b) < 0) b = 0u - b;
    if (int32_t(c) < 0) c = 0u - c;
    if (a < b) std::swap(a, b);
    if (a < c) std::swap(a, c);
    if (b < c) std::swap(b, c);
    return a + (b >> 1) + (c >> 2);
}

// 0x80020EC4 -> 0x8007B640: 2 = outside (every corner beyond one frustum plane or one screen edge), else 1 = some corner
// clipped, 0 = inside. The GTE holds the view's rotation (TR = 0), light matrix / colour matrix = frustum planes.
int ViewTest(const GlowView& v, const gt2::TrackChunkBounds& box) {
    int16_t points[8][3];
    for (int set = 0; set < 2; set++) {
        const int32_t y = set == 0 ? box.yLow : box.yHigh;
        for (int k = 0; k < 4; k++) {
            int32_t x = int32_t(U32(box.x[size_t(k)]) + U32(v.offset[0]));
            int32_t yy = int32_t(U32(y) + U32(v.offset[1]));
            int32_t z = int32_t(U32(box.z[size_t(k)]) + U32(v.offset[2]));
            auto mag = [](int32_t q) { return q < 0 ? int32_t(0u - U32(q)) : q; }; // negu
            const int32_t m = int32_t(U32(mag(x)) | U32(mag(yy)) | U32(mag(z)));
            const int shift = 19 - psx::LeadingSignBits(m);
            if (shift >= 0) {
                x >>= shift;
                yy >>= shift;
                z >>= shift;
            }
            points[set * 4 + k][0] = int16_t(x); // VX
            points[set * 4 + k][1] = int16_t(z); // VY (the swapped columns: chunk space is (x, z, height))
            points[set * 4 + k][2] = int16_t(yy);
        }
    }
    static constexpr int32_t kZero[3] = {0, 0, 0};
    uint32_t orAll = 0, andPlanes = 0xFFFFFFFFu, andScreen = 0xFFFFFFFFu;
    for (int i = 0; i < 8; i++) {
        const psx::RtpsResult p = psx::Rtps(v.rotation, kZero, v.ofx, v.ofy, v.h, 4096, 0, points[i][0], points[i][1], points[i][2]);
        const bool error = (p.flag & 0x80000000u) != 0;
        const std::array<int16_t, 3> l = psx::Mvmva(v.llm, p.ir);
        const std::array<int16_t, 3> c = psx::Mvmva(v.lcm, p.ir);
        uint32_t planes = (U32(int32_t(p.ir[2]) - int32_t(v.nearZ)) >> 31);
        if (l[0] < 0) planes |= 2;
        if (l[1] < 0) planes |= 4;
        if (c[0] < 0) planes |= 8;
        if (c[1] < 0) planes |= 0x10;
        if (error) planes |= 0x20;
        uint32_t screen = 0;
        if (p.sx < 0) screen |= 2;
        if (!(int32_t(p.sx) < int32_t(v.clipX))) screen |= 4;
        if (p.sy < 0) screen |= 8;
        if (!(int32_t(p.sy) < int32_t(v.clipY))) screen |= 0x10;
        if (error) screen = 0;
        orAll |= planes;
        andPlanes &= planes;
        andScreen &= screen;
    }
    const uint32_t result = (orAll << 16) | (andPlanes | andScreen);
    if (result & 0x1F) return 2;
    return ((result >> 16) & 0x3F) != 0 ? 1 : 0;
}

// 0x80026BB4: the GTE state of one chunk: shift 0..2 from the squared camera distance, rotation << shift, translation =
// rotation << shift applied (MVMVA through the 16-bit IR registers) to the 64 m cell origin in 1/64 m.
struct ChunkGte {
    int16_t rotation[3][3];
    int32_t translation[3];
    int shift;
};
ChunkGte SetupChunk(const GlowView& v, const gt2::TrackChunk& chunk) {
    const int32_t d[3] = {int32_t(U32(chunk.centre[0]) + U32(v.offset[0])), int32_t(U32(chunk.centre[1]) + U32(v.offset[1])),
                          int32_t(U32(chunk.centre[2]) + U32(v.offset[2]))};
    uint64_t sum = 0;
    for (int32_t q : d) sum += uint64_t(int64_t(q) * q);
    sum += uint64_t(int64_t(chunk.extent) * chunk.extent);
    const int32_t hi = int32_t(uint32_t(sum >> 32));
    const uint32_t lo = uint32_t(sum);
    int32_t tHi = 0x40000;
    uint32_t tLo = 0;
    int k = 0;
    if (hi < tHi) {
        do {
            tLo = (tLo >> 2) | (uint32_t(tHi) << 30);
            tHi >>= 2;
            k++;
        } while (hi < tHi || (hi == tHi && lo < tLo));
    }
    ChunkGte g{};
    g.shift = k ? k - 1 : 0;
    if (g.shift >= 2) g.shift = 2;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) g.rotation[r][c] = int16_t(uint16_t(uint32_t(int32_t(v.rotation[r][c])) << g.shift)); // 0x80081A34
    auto cell = [&](int axis) { return int32_t((U32(chunk.centre[size_t(axis)]) & 0xFFC00000u) + U32(v.offset[axis])) >> 10; };
    const int16_t ir[3] = {int16_t(cell(0)), int16_t(cell(2)), int16_t(cell(1))}; // IR1 = x, IR2 = z, IR3 = y
    const std::array<int32_t, 3> mac = psx::MvmvaMac(g.rotation, ir);
    for (int i = 0; i < 3; i++) g.translation[i] = mac[size_t(i)];
    return g;
}

// The glow drawers (chunks 0x80020A00 / 0x800205EC, models 0x8001FBA8): four packets per record, appended in drawing-code
// order. `model`: the models' radius and ordering-table rules (see DrawModelGlows).
void DrawGlows(const int16_t rotation[3][3], const int32_t translation[3], int32_t ofx, int32_t ofy, uint16_t h, int shift, bool model,
               const std::vector<gt2::TrackGlow>& glows, uint16_t chunkIndex, std::vector<GlowPrimitive>& out) {
    const int32_t s = StarSin(), c = StarCos();
    for (const gt2::TrackGlow& rec : glows) {
        const psx::RtpsResult p = psx::Rtps(rotation, translation, ofx, ofy, h, rec.size, 0, rec.position[0], rec.position[1], rec.position[2]);
        if (p.flag & 0x80000000u) continue;
        int32_t r, ot;
        if (!model) {
            r = int32_t(U32(p.mac0) << shift) >> 15;
            ot = int32_t(uint32_t(p.sz3) >> (shift + 3));
        } else {
            const int down = shift - 10;
            r = (down >= 0 ? p.mac0 >> down : int32_t(U32(p.mac0) << (-down))) >> 15;
            ot = int32_t(uint32_t(p.sz3) << shift) >> 13;
        }
        if (!(ot < 4096)) ot = 4095;
        const int32_t X = p.sx, Y = p.sy;
        const int32_t sr = int32_t(U32(s) * U32(r)), cr = int32_t(U32(c) * U32(r)); // mult, mflo
        const int32_t hs = sr >> 13, s1 = sr >> 12, hc = cr >> 13, c1 = cr >> 12;
        // Screen corners exactly as the drawer computes them (register by register).
        const int32_t xA = X - hs, xB = X - hs + s1;                  // v0.x of quads 0/1, 2/3
        const int32_t xM = X - hc;                                    // a3 after the subtraction
        const int32_t xP = xM + c1;                                   // t1
        const int32_t yA = Y + hc, yB = Y + hc - c1;                  // v0.y of quads 0/1, 2/3
        const int32_t yM = Y - hs, yP = yM + s1;                      // a1, t0
        const int32_t x1[4] = {xM - hs, xP - hs, xM - hs + s1, xP - hs + s1};
        const int32_t y1[4] = {yM + hc, yP + hc, yM + hc - c1, yP + hc - c1};
        const int32_t x0[4] = {xA, xA, xB, xB}, y0[4] = {yA, yA, yB, yB};
        const int32_t x3[4] = {xM, xP, xM, xP}, y3[4] = {yM, yP, yM, yP};
        static constexpr uint8_t kU[4][4] = {{160, 128, 160, 128}, {160, 191, 160, 191}, {160, 128, 160, 128}, {160, 191, 160, 191}};
        static constexpr uint8_t kV[4][4] = {{64, 64, 96, 96}, {64, 64, 96, 96}, {127, 127, 96, 96}, {127, 127, 96, 96}};
        const uint32_t half = (rec.colour & 0xFEFEFEu) >> 1, eighth = (half & 0xFCFCFCu) >> 2, sixtyFourth = (eighth & 0xF8F8F8u) >> 3;
        for (int q = 0; q < 4; q++) {
            GlowPrimitive prim;
            prim.x[0] = int16_t(x0[q]); prim.y[0] = int16_t(y0[q]);
            prim.x[1] = int16_t(x1[q]); prim.y[1] = int16_t(y1[q]);
            prim.x[2] = int16_t(X);     prim.y[2] = int16_t(Y);
            prim.x[3] = int16_t(x3[q]); prim.y[3] = int16_t(y3[q]);
            for (int k = 0; k < 4; k++) { prim.u[k] = kU[q][k]; prim.v[k] = kV[q][k]; }
            prim.rgb[0] = eighth; prim.rgb[1] = sixtyFourth; prim.rgb[2] = half; prim.rgb[3] = eighth;
            prim.otEntry = ot - 8;
            prim.chunk = chunkIndex;
            out.push_back(prim);
        }
    }
}

} // namespace

GlowView GlowView::FromViewBytes(const uint8_t* p) {
    GlowView v;
    for (int r = 0; r < 3; r++) { // 0x80020E38: columns 1 and 2 swapped
        v.rotation[r][0] = S16(p + r * 6);
        v.rotation[r][1] = S16(p + r * 6 + 4);
        v.rotation[r][2] = S16(p + r * 6 + 2);
    }
    for (int i = 0; i < 3; i++) {
        v.translation[i] = S32(p + 0x14 + i * 4);
        v.offset[i] = S32(p + 0x20 + i * 4);
    }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            v.llm[r][c] = S16(p + 0x2C + (r * 3 + c) * 2);
            v.lcm[r][c] = S16(p + 0x40 + (r * 3 + c) * 2);
        }
    v.ofx = S32(p + 0x54);
    v.ofy = S32(p + 0x58);
    v.h = uint16_t(S16(p + 0x5C));
    v.clipX = uint16_t(S16(p + 0x5E));
    v.clipY = uint16_t(S16(p + 0x60));
    v.nearZ = uint16_t(S16(p + 0x62));
    return v;
}

std::string GlowPrimitive::ToString() const {
    char line[320];
    std::snprintf(line, sizeof(line),
                  "POLY 3E quad tex gouraud semi rgb=%06X v0=(%d,%d) uv0=(%u,%u) clut=%04X rgb1=%06X v1=(%d,%d) uv1=(%u,%u) tpage=%04X rgb2=%06X v2=(%d,%d) uv2=(%u,%u) "
                  "rgb3=%06X v3=(%d,%d) uv3=(%u,%u)",
                  rgb[0], x[0], y[0], u[0], v[0], clut, rgb[1], x[1], y[1], u[1], v[1], tpage, rgb[2], x[2], y[2], u[2], v[2], rgb[3], x[3], y[3], u[3], v[3]);
    return line;
}

GlowFrame BuildChunkGlows(const gt2::Track& track, const GlowView& view, int cameraChunk, bool mirror) {
    GlowFrame frame;
    if (cameraChunk < 0 || size_t(cameraChunk) >= track.chunks.size()) return frame;
    struct Node { uint16_t entry; uint32_t key; };
    std::vector<Node> sorted;
    for (uint16_t e : track.chunks[size_t(cameraChunk)].renderList) {
        const size_t index = e & 0x3FFF;
        if (index >= track.chunks.size()) continue;
        const gt2::TrackChunk& chunk = track.chunks[index];
        const uint32_t key = ChunkKey(view, chunk.centre);
        if (mirror && key > 0x63FFFFu) continue;
        if (ViewTest(view, chunk.bounds[mirror ? 1 : 0]) == 2) continue;
        size_t at = 0;
        while (at < sorted.size() && !(key < sorted[at].key)) at++;
        sorted.insert(sorted.begin() + std::ptrdiff_t(at), Node{e, key});
    }
    std::vector<GlowPrimitive> drawn; // in submission order
    for (const Node& n : sorted) {
        const uint16_t index = uint16_t(n.entry & 0x3FFF);
        const gt2::TrackChunk& chunk = track.chunks[index];
        frame.chunks.push_back(n.entry);
        const ChunkGte g = SetupChunk(view, chunk);
        DrawGlows(g.rotation, g.translation, view.ofx, view.ofy, view.h, g.shift, false, mirror ? chunk.surround.glows : chunk.road.glows, index, drawn);
    }
    frame.primitives = SortGlowPrimitives(drawn);
    frame.submitted = std::move(drawn);
    return frame;
}

std::vector<GlowPrimitive> SortGlowPrimitives(const std::vector<GlowPrimitive>& drawn) {
    std::vector<size_t> order(drawn.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (drawn[a].otEntry != drawn[b].otEntry) return drawn[a].otEntry > drawn[b].otEntry;
        return a > b;
    });
    std::vector<GlowPrimitive> out;
    for (size_t i : order) out.push_back(drawn[i]);
    return out;
}

std::vector<GlowPrimitive> DrawModelGlows(const GlowGte& gte, uint16_t shift, const std::vector<gt2::TrackGlow>& glows) {
    std::vector<GlowPrimitive> out;
    DrawGlows(gte.rotation, gte.translation, gte.ofx, gte.ofy, gte.h, shift, true, glows, 0xFFFF, out);
    return out;
}

namespace {
// One star (four quads in the camera plane) at `centre` (world metres): radius size / 32 m for chunks and models alike
// (chunks: r = 2 size H / SZ with SZ in 1/64 m >> shift; models: r = size H 2^(11 - s) / SZ with SZ in 2^(s - 16) m).
void AppendGlowStar(std::vector<SceneVertex>& out, const std::array<float, 3>& centre, const gt2::TrackGlow& g, const float right[3], const float down[3]) {
    const float s = float(StarSin()) / 4096.0f, c = float(StarCos()) / 4096.0f;
    static constexpr float kU[4][4] = {{160, 128, 160, 128}, {160, 191, 160, 191}, {160, 128, 160, 128}, {160, 191, 160, 191}};
    static constexpr float kV[4][4] = {{64, 64, 96, 96}, {64, 64, 96, 96}, {127, 127, 96, 96}, {127, 127, 96, 96}};
    // Corner offsets in units of (A, B): quad q = {v0, v1, v2, v3}.
    static constexpr float kA[4][4] = {{0, -1, 0, -1}, {0, 1, 0, 1}, {0, -1, 0, -1}, {0, 1, 0, 1}};
    static constexpr float kB[4][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {-1, -1, 0, 0}, {-1, -1, 0, 0}};
    static constexpr int kSplit[6] = {0, 1, 2, 1, 2, 3}; // the GPU's two triangles of a quad
    const float radius = float(g.size) / 32.0f;
    float A[3], B[3];
    for (int i = 0; i < 3; i++) {
        A[i] = radius * 0.5f * (c * right[i] + s * down[i]);
        B[i] = radius * 0.5f * (-s * right[i] + c * down[i]);
    }
    const uint32_t half = (g.colour & 0xFEFEFEu) >> 1, eighth = (half & 0xFCFCFCu) >> 2, sixtyFourth = (eighth & 0xF8F8F8u) >> 3;
    const uint32_t colours[4] = {eighth, sixtyFourth, half, eighth};
    for (int q = 0; q < 4; q++)
        for (int corner : kSplit) {
            SceneVertex o{};
            for (int i = 0; i < 3; i++) o.pos[i] = centre[size_t(i)] + kA[q][corner] * A[i] + kB[q][corner] * B[i];
            o.texel[0] = kU[q][corner] + 0.5f;
            o.texel[1] = kV[q][corner] + 0.5f;
            for (int k = 0; k < 3; k++) o.color[k] = float((colours[corner] >> (8 * k)) & 0xFF) / 255.0f;
            o.page = 576u;                          // tpage 0x29: page (576, 0), 4-bit
            o.clut = 368u | (509u << 16);           // CLUT 0x7F57
            o.flags = kTextured | kSemiTransparent; // blend mode 1 (the caller's list)
            out.push_back(o);
        }
}
} // namespace

void AppendChunkGlowSprites(std::vector<SceneVertex>& out, const gt2::Track&, const gt2::TrackChunk& chunk, const float right[3], const float down[3]) {
    for (const gt2::TrackGlow& g : chunk.road.glows)
        AppendGlowStar(out, gt2::TrackVertexToWorld(chunk, {g.position[0], g.position[1], g.position[2]}), g, right, down);
}

void AppendModelGlowSprites(std::vector<SceneVertex>& out, const gt2::TrackSceneryModel& model, const float* matrix, const float right[3], const float down[3]) {
    for (const gt2::TrackGlow& g : model.glows) {
        std::array<float, 3> p{};
        for (int r = 0; r < 3; r++)
            p[size_t(r)] = matrix[r] * g.position[0] + matrix[4 + r] * g.position[1] + matrix[8 + r] * g.position[2] + matrix[12 + r];
        AppendGlowStar(out, p, g, right, down);
    }
}

} // namespace gt2view
