#include "gt2export/car_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "gt2export/png_writer.h"

namespace gt2 {
namespace {

constexpr int kAtlasTiles = 4;
constexpr int kAtlasWidth = CarTexture::kWidth * kAtlasTiles;
constexpr int kAtlasHeight = CarTexture::kHeight * kAtlasTiles;

struct Mesh {
    std::vector<float> pos, uv, color, normal;
    size_t Count() const { return pos.size() / 3; }
};

struct BufferView {
    size_t offset, length;
};

void Append(std::vector<uint8_t>& bin, const std::vector<float>& v, BufferView& view) {
    view = {bin.size(), v.size() * sizeof(float)};
    const auto* p = reinterpret_cast<const uint8_t*>(v.data());
    bin.insert(bin.end(), p, p + view.length);
}

void MinMax(const std::vector<float>& pos, float mn[3], float mx[3]) {
    for (int k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }
    for (size_t i = 0; i < pos.size(); i++) {
        mn[i % 3] = std::min(mn[i % 3], pos[i]);
        mx[i % 3] = std::max(mx[i % 3], pos[i]);
    }
}

} // namespace

void ExportCarGltf(const CarModel& model, const CarTexture& texture, const std::string& outDir,
                   const std::string& baseName, const CarGltfOptions& opt) {
    // Four primitives: textured / flat x plain / reflective. Reflective = the polygons with renderFlags bit 15, which the
    // game draws a second time with the environment map (0x80061798); their material carries extras.gt2pc.reflection so
    // that the mesh keeps the pass when it comes back as a mod body (car_json.md "Reflections"). NORMAL = the LOD's
    // normal table (the original feeds bits 2-11, 12-21, 22-31 = CarNormal (z, y, x)), face normals for the wheels.
    std::array<Mesh, 4> meshes; // [reflective * 2 + (textured ? 0 : 1)]
    const CarLod& lod = model.lods.at(opt.lod);
    const std::vector<CarMeshVertex> verts = BuildCarMesh(model, {opt.lod, opt.placeholderWheels, opt.quadStripOrder, nullptr, false}); // double-sided: one copy per wheel quad
    size_t bodyCount = 0;
    for (const CarPolygon& p : lod.polygons) bodyCount += p.IsQuad() ? 6 : 3;
    if (bodyCount > verts.size()) throw std::runtime_error("car glTF: mesh shorter than its polygons");
    std::vector<uint8_t> reflective(verts.size(), 0);
    std::vector<std::array<float, 3>> normals(verts.size(), std::array<float, 3>{0, 1, 0});
    {
        static constexpr int kTri[3] = {0, 1, 2}, kQuadRing[6] = {0, 1, 3, 1, 2, 3}, kQuadStrip[6] = {0, 1, 2, 1, 3, 2}; // as BuildCarMesh
        size_t at = 0;
        for (const CarPolygon& p : lod.polygons) {
            const int* order = !p.IsQuad() ? kTri : (opt.quadStripOrder ? kQuadStrip : kQuadRing);
            for (int k = 0; k < (p.IsQuad() ? 6 : 3); k++, at++) {
                reflective[at] = (p.renderFlags & 0x8000) ? 1 : 0;
                if (!lod.normals.empty()) {
                    const CarNormal& n = lod.normals[p.normal[size_t(order[k])] % lod.normals.size()];
                    const float g[3] = {float(n.z), float(n.y), float(n.x)};
                    const float l = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    if (l > 0) normals[at] = {g[0] / l, g[1] / l, g[2] / l};
                }
            }
        }
    }
    for (size_t t = bodyCount; t + 2 < verts.size(); t += 3) { // wheels: face normals
        const float* a = verts[t].pos;
        const float* b = verts[t + 1].pos;
        const float* c = verts[t + 2].pos;
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (size_t k = 0; k < 3; k++) normals[t + k] = l > 0 ? std::array<float, 3>{n[0] / l, n[1] / l, n[2] / l} : std::array<float, 3>{0, 1, 0};
    }
    for (size_t idx = 0; idx < verts.size(); idx++) {
        const CarMeshVertex& v = verts[idx];
        Mesh& m = meshes[size_t(reflective[idx]) * 2 + (v.textured ? 0 : 1)];
        m.pos.insert(m.pos.end(), {v.pos[0], v.pos[1], v.pos[2]});
        m.normal.insert(m.normal.end(), normals[idx].begin(), normals[idx].end());
        if (!v.textured) {
            m.color.insert(m.color.end(), {v.color[0], v.color[1], v.color[2]});
            continue;
        }
        int tileX = v.palette % kAtlasTiles, tileY = v.palette / kAtlasTiles;
        m.uv.push_back((tileX * CarTexture::kWidth + v.texel[0]) / kAtlasWidth);
        m.uv.push_back((tileY * CarTexture::kHeight + v.texel[1]) / kAtlasHeight);
        // PS1 texture modulation: 0x80 is neutral.
        const float k = 255.0f / 128.0f;
        if (v.rawTexture) m.color.insert(m.color.end(), {1.0f, 1.0f, 1.0f});
        else m.color.insert(m.color.end(), {std::min(1.0f, v.color[0] * k), std::min(1.0f, v.color[1] * k), std::min(1.0f, v.color[2] * k)});
    }

    std::vector<uint8_t> atlas(static_cast<size_t>(kAtlasWidth) * kAtlasHeight * 4);
    for (int c = 0; c < CarTexture::kClutCount; c++) {
        auto tile = texture.DecodeRgba(opt.paint, static_cast<size_t>(c));
        int ox = (c % kAtlasTiles) * CarTexture::kWidth, oy = (c / kAtlasTiles) * CarTexture::kHeight;
        for (int y = 0; y < CarTexture::kHeight; y++)
            std::memcpy(&atlas[(static_cast<size_t>(oy + y) * kAtlasWidth + ox) * 4],
                        &tile[static_cast<size_t>(y) * CarTexture::kWidth * 4], CarTexture::kWidth * 4);
    }
    WritePngRgba(outDir + "/" + baseName + ".png", kAtlasWidth, kAtlasHeight, atlas);

    std::vector<uint8_t> bin;
    std::vector<BufferView> views;
    std::ostringstream accessors, primitives;
    auto addAccessor = [&](const std::vector<float>& data, const char* type, int comps, bool withBounds) {
        BufferView bv;
        Append(bin, data, bv);
        views.push_back(bv);
        if (views.size() > 1) accessors << ",";
        accessors << "{\"bufferView\":" << views.size() - 1 << ",\"componentType\":5126,\"count\":"
                  << data.size() / comps << ",\"type\":\"" << type << "\"";
        if (withBounds) {
            float mn[3], mx[3];
            MinMax(data, mn, mx);
            accessors << ",\"min\":[" << mn[0] << "," << mn[1] << "," << mn[2] << "],\"max\":[" << mx[0] << ","
                      << mx[1] << "," << mx[2] << "]";
        }
        accessors << "}";
        return views.size() - 1;
    };

    bool first = true;
    for (size_t k = 0; k < 4; k++) {
        const Mesh& m = meshes[k];
        if (!m.Count()) continue;
        const bool textured = (k & 1) == 0;
        const size_t a = addAccessor(m.pos, "VEC3", 3, true);
        const size_t n = addAccessor(m.normal, "VEC3", 3, false);
        primitives << (first ? "" : ",") << "{\"attributes\":{\"POSITION\":" << a << ",\"NORMAL\":" << n;
        if (textured) primitives << ",\"TEXCOORD_0\":" << addAccessor(m.uv, "VEC2", 2, false);
        primitives << ",\"COLOR_0\":" << addAccessor(m.color, "VEC3", 3, false) << "},\"material\":" << k << "}";
        first = false;
    }

    const char* const reflectionExtras = ",\"extras\":{\"gt2pc\":{\"reflection\":true}}";
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"gt2tool\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      << "\"nodes\":[{\"mesh\":0,\"name\":\"" << baseName << "\"}],"
      << "\"meshes\":[{\"primitives\":[" << primitives.str() << "]}],"
      << "\"materials\":[";
    for (int r = 0; r < 2; r++) {
        j << (r ? "," : "")
          << "{\"name\":\"textured" << (r ? "_reflective" : "") << "\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,"
             "\"roughnessFactor\":1},\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5,\"doubleSided\":true" << (r ? reflectionExtras : "") << "},"
          << "{\"name\":\"flat" << (r ? "_reflective" : "") << "\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true"
          << (r ? reflectionExtras : "") << "}";
    }
    j << "],"
      << "\"textures\":[{\"sampler\":0,\"source\":0}],"
      << "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":33071,\"wrapT\":33071}],"
      << "\"images\":[{\"uri\":\"" << baseName << ".png\"}],"
      << "\"buffers\":[{\"uri\":\"" << baseName << ".bin\",\"byteLength\":" << bin.size() << "}],"
      << "\"bufferViews\":[";
    for (size_t i = 0; i < views.size(); i++)
        j << (i ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << views[i].offset << ",\"byteLength\":" << views[i].length
          << ",\"target\":34962}";
    j << "],\"accessors\":[" << accessors.str() << "]}";

    auto writeFile = [](const std::string& path, const void* data, size_t size) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot create " + path);
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    };
    writeFile(outDir + "/" + baseName + ".bin", bin.data(), bin.size());
    std::string json = j.str();
    writeFile(outDir + "/" + baseName + ".gltf", json.data(), json.size());
}

} // namespace gt2
