#include "gt2export/track_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "gt2export/png_writer.h"

namespace gt2 {
namespace {

struct Corner {
    std::array<float, 3> world;
    std::array<float, 3> rgb; // polygon colour / 255
    float u, v;
};

struct Triangle {
    std::array<Corner, 3> c;
    const TrackUvSet* uv = nullptr; // null: untextured
};

struct Screen {
    double x, y, depth, invZ;
};

void CollectShape(const Track& track, const TrackChunk& chunk, const TrackShape& shape, bool textured,
                  std::vector<Triangle>& out) {
    for (const TrackPolygon& p : shape.polygons) {
        const TrackUvSet* uv = textured && p.IsTextured() ? &track.uvTable[p.uvIndex].nearSet : nullptr;
        auto corner = [&](size_t i) {
            const auto& c = p.color[i];
            return Corner{TrackVertexToWorld(chunk, shape.vertices[p.vertex[i]]),
                          {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f},
                          uv ? uv->u[i] + 0.5f : 0.0f,
                          uv ? uv->v[i] + 0.5f : 0.0f};
        };
        out.push_back({{corner(0), corner(1), corner(2)}, uv});
        if (p.IsQuad()) out.push_back({{corner(0), corner(2), corner(3)}, uv}); // ring order (no bow-ties in testline renders)
    }
}

} // namespace

void RenderTrackPreview(const Track& track, const PsxVram* vram, const std::string& pngPath,
                        const TrackPreviewOptions& opt) {
    const int W = opt.width, H = opt.height;
    std::vector<Triangle> tris;
    for (const TrackChunk& chunk : track.chunks) {
        if (opt.road) CollectShape(track, chunk, chunk.road, vram != nullptr, tris);
        if (opt.surround) CollectShape(track, chunk, chunk.surround, vram != nullptr, tris);
    }

    std::vector<uint8_t> image(size_t(W) * H * 4);
    for (size_t i = 0; i < image.size(); i += 4) {
        image[i] = 20; image[i + 1] = 22; image[i + 2] = 28; image[i + 3] = 255;
    }
    std::vector<double> depth(size_t(W) * H, std::numeric_limits<double>::infinity());

    const bool topDown = opt.chunk < 0;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& t : tris)
        for (const auto& c : t.c)
            for (int k = 0; k < 3; k++) {
                mn[k] = std::min(mn[k], c.world[k]);
                mx[k] = std::max(mx[k], c.world[k]);
            }
    const double mapScale = 0.96 * std::min(W / double(mx[0] - mn[0]), H / double(mx[2] - mn[2]));
    double eye[3] = {0, 0, 0}, fwd[3] = {0, 0, 1}, right[3] = {1, 0, 0};
    if (!topDown) {
        const TrackChunk& c = track.chunks.at(size_t(opt.chunk));
        eye[0] = c.origin[0] / 65536.0;
        eye[1] = c.origin[1] / 65536.0 + opt.eyeHeight;
        eye[2] = c.origin[2] / 65536.0;
        double fx = c.direction[0], fz = c.direction[2], fl = std::sqrt(fx * fx + fz * fz);
        fwd[0] = fx / fl; fwd[2] = fz / fl;
        right[0] = -fwd[2]; right[2] = fwd[0];
    }
    const double focal = 0.5 * W / std::tan(35.0 * 3.14159265358979 / 180.0);

    auto project = [&](const Corner& c, Screen& s) {
        if (topDown) {
            s.x = W / 2.0 + (c.world[0] - (mn[0] + mx[0]) / 2) * mapScale;
            s.y = H / 2.0 + (c.world[2] - (mn[2] + mx[2]) / 2) * mapScale;
            s.depth = -c.world[1];
            s.invZ = 1;
            return true;
        }
        double d[3] = {c.world[0] - eye[0], c.world[1] - eye[1], c.world[2] - eye[2]};
        double z = d[0] * fwd[0] + d[2] * fwd[2];
        if (z < 0.3) return false; // no near-plane clipping in this preview
        double x = d[0] * right[0] + d[2] * right[2];
        s.x = W / 2.0 + focal * x / z;
        s.y = H / 2.0 - focal * d[1] / z;
        s.depth = z;
        s.invZ = 1 / z;
        return true;
    };

    for (const Triangle& t : tris) {
        Screen s[3];
        if (!project(t.c[0], s[0]) || !project(t.c[1], s[1]) || !project(t.c[2], s[2])) continue;
        const Screen &a = s[0], &b = s[1], &c = s[2];
        double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-9) continue;
        int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        int x1 = std::min(W - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        int y1 = std::min(H - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                double px = x + 0.5, py = y + 0.5;
                double w[3];
                w[0] = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                w[1] = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                w[2] = 1 - w[0] - w[1];
                if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                // Perspective-correct weights.
                double pw[3] = {w[0] * a.invZ, w[1] * b.invZ, w[2] * c.invZ};
                double sum = pw[0] + pw[1] + pw[2];
                for (double& v : pw) v /= sum;
                double z = topDown ? w[0] * a.depth + w[1] * b.depth + w[2] * c.depth : 1 / sum;
                size_t idx = size_t(y) * W + x;
                if (z >= depth[idx]) continue;

                double rgb[3];
                for (int k = 0; k < 3; k++) rgb[k] = pw[0] * t.c[0].rgb[k] + pw[1] * t.c[1].rgb[k] + pw[2] * t.c[2].rgb[k];
                if (t.uv) {
                    double u = pw[0] * t.c[0].u + pw[1] * t.c[1].u + pw[2] * t.c[2].u;
                    double v = pw[0] * t.c[0].v + pw[1] * t.c[1].v + pw[2] * t.c[2].v;
                    uint16_t texel = vram->Sample(t.uv->tpage, t.uv->clut, uint8_t(std::clamp(u, 0.0, 255.0)),
                                                  uint8_t(std::clamp(v, 0.0, 255.0)));
                    if (texel == 0) continue; // PS1: colour 0x0000 is transparent
                    double tex[3] = {double(texel & 0x1F) / 31.0, double((texel >> 5) & 0x1F) / 31.0, double((texel >> 10) & 0x1F) / 31.0};
                    for (int k = 0; k < 3; k++) rgb[k] = std::min(1.0, tex[k] * rgb[k] * (255.0 / 128.0)); // 0x80 = neutral
                }
                depth[idx] = z;
                for (int k = 0; k < 3; k++) image[idx * 4 + k] = uint8_t(255.0 * rgb[k]);
            }
    }
    WritePngRgba(pngPath, W, H, image);
}

} // namespace gt2
