#include "gt2export/car_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "gt2export/png_writer.h"

namespace gt2 {
namespace {

struct Pt {
    double x, y, z, u, v;
};

} // namespace

void RenderCarPreview(const CarModel& model, const CarTexture& texture, const std::string& pngPath,
                      const CarPreviewOptions& opt) {
    const CarLod& lod = model.lods.at(opt.lod);
    const int W = opt.width, H = opt.height;
    std::vector<uint8_t> image(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < image.size(); i += 4) {
        image[i] = 58; image[i + 1] = 62; image[i + 2] = 70; image[i + 3] = 255;
    }
    std::vector<double> depth(static_cast<size_t>(W) * H, std::numeric_limits<double>::infinity());

    std::array<std::vector<uint8_t>, CarTexture::kClutCount> tiles;
    for (size_t c = 0; c < tiles.size(); c++) tiles[c] = texture.DecodeRgba(opt.paint, c);

    const double kPi = 3.14159265358979;
    const double yaw = opt.yawDegrees * kPi / 180, pitch = opt.pitchDegrees * kPi / 180;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);

    // File space: +Y up, -Z front, -X left. View: rotate about Y, then tilt about X.
    double extent = 1;
    for (const CarVertex& v : lod.vertices)
        extent = std::max({extent, std::abs(double(v.x)), std::abs(double(v.y)), std::abs(double(v.z))});
    const double scale = 0.46 * std::min(double(W), H * 1.6) / extent;
    const double centreY = (lod.bbox[1] + lod.bbox[5]) / 2.0;

    auto project = [&](const CarVertex& v, double u, double tv) {
        double x = v.x, y = v.y - centreY, z = v.z;
        double rx = x * cy + z * sy, rz = -x * sy + z * cy;
        double ry = y * cp - rz * sp, rz2 = y * sp + rz * cp;
        return Pt{W / 2.0 + rx * scale, H / 2.0 - ry * scale, -rz2, u, tv};
    };

    auto drawTriangle = [&](const Pt& a, const Pt& b, const Pt& c, const CarPolygon& p) {
        double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-9) return;
        int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        int x1 = std::min(W - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        int y1 = std::min(H - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                double px = x + 0.5, py = y + 0.5;
                double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                double z = w0 * a.z + w1 * b.z + w2 * c.z;
                size_t idx = static_cast<size_t>(y) * W + x;
                if (z >= depth[idx]) continue;
                uint8_t r = p.r, g = p.g, bl = p.b;
                if (p.IsTextured()) {
                    int tu = std::clamp(int(w0 * a.u + w1 * b.u + w2 * c.u), 0, CarTexture::kWidth - 1);
                    int tv = std::clamp(int(w0 * a.v + w1 * b.v + w2 * c.v), 0, CarTexture::kHeight - 1);
                    const uint8_t* t = &tiles[p.palette][(static_cast<size_t>(tv) * CarTexture::kWidth + tu) * 4];
                    if (t[3] == 0) continue;
                    if (p.primCode & CarPolygon::kCodeRawTexture) {
                        r = t[0]; g = t[1]; bl = t[2];
                    } else {
                        r = uint8_t(std::min(255, t[0] * p.r / 128));
                        g = uint8_t(std::min(255, t[1] * p.g / 128));
                        bl = uint8_t(std::min(255, t[2] * p.b / 128));
                    }
                }
                depth[idx] = z;
                image[idx * 4] = r; image[idx * 4 + 1] = g; image[idx * 4 + 2] = bl;
            }
        }
    };

    for (const CarPolygon& p : lod.polygons) {
        std::array<Pt, 4> pts{};
        for (size_t i = 0; i < (p.IsQuad() ? 4u : 3u); i++)
            pts[i] = project(lod.vertices[p.vertex[i]], p.u[i] + 0.5, p.v[i] + 0.5);
        drawTriangle(pts[0], pts[1], pts[2], p);
        if (p.IsQuad()) drawTriangle(pts[0], pts[2], pts[3], p); // ring order
    }

    WritePngRgba(pngPath, W, H, image);
}

} // namespace gt2
