#include "gt2export/track_gltf.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "gt2export/png_deflate.h"

namespace gt2 {
namespace {

constexpr int kAtlasWidth = 2048;

// A texture region: one (tpage, clut) pair's used texels.
struct Region {
    uint16_t tpage = 0, clut = 0;
    int u0 = 255, v0 = 255, u1 = 0, v1 = 0; // inclusive bounds of the used texels
    int x = 0, y = 0;                        // place in the atlas
};

uint32_t RegionKey(uint16_t tpage, uint16_t clut) {
    const uint16_t page = uint16_t(tpage & 0x19F); // page x / y and colour depth (the blend bits 5-6 do not change texels)
    const bool direct = ((tpage >> 7) & 3) >= 2;
    return (uint32_t(page) << 16) | (direct ? 0u : clut);
}

// One primitive's vertex streams.
struct Stream {
    std::vector<float> pos, uv, color;
    size_t Count() const { return pos.size() / 3; }
};
// Per mesh: [0] textured opaque, [1] textured semi-transparent, [2] untextured opaque, [3] untextured semi-transparent.
using MeshStreams = std::array<Stream, 4>;

struct Corner {
    std::array<float, 3> pos;
    std::array<uint8_t, 3> color;
    uint8_t u, v;
};

class Builder {
public:
    Builder(const Track& t, const PsxVram& vram) : track_(t), vram_(vram) {}

    void CollectRegions() {
        auto note = [&](uint16_t tpage, uint16_t clut, const std::array<uint8_t, 4>& u, const std::array<uint8_t, 4>& v, size_t corners) {
            Region& r = RegionOf(tpage, clut);
            for (size_t c = 0; c < corners; c++) {
                r.u0 = std::min(r.u0, int(u[c]));
                r.u1 = std::max(r.u1, int(u[c]));
                r.v0 = std::min(r.v0, int(v[c]));
                r.v1 = std::max(r.v1, int(v[c]));
            }
        };
        for (const TrackChunk& c : track_.chunks)
            for (const TrackPolygon& p : c.road.polygons)
                if (p.IsTextured()) {
                    const TrackUvSet& s = track_.uvTable[p.uvIndex].nearSet;
                    note(s.tpage, s.clut, s.u, s.v, p.IsQuad() ? 4 : 3);
                }
        for (const TrackSceneryModel& m : track_.sceneryModels)
            for (const TrackSceneryPolygon& p : m.polygons)
                if (p.IsTextured()) note(p.tpage, p.clut, p.u, p.v, p.IsQuad() ? 4 : 3);
    }

    // Shelf packing (tallest first) into a kAtlasWidth wide atlas, then the texels.
    void BuildAtlas() {
        std::vector<Region*> order;
        for (auto& [k, r] : regions_) order.push_back(&r);
        std::stable_sort(order.begin(), order.end(), [](const Region* a, const Region* b) { return (a->v1 - a->v0) > (b->v1 - b->v0); });
        int x = 0, y = 0, shelf = 0;
        for (Region* r : order) {
            const int w = r->u1 - r->u0 + 1, h = r->v1 - r->v0 + 1;
            if (x + w > kAtlasWidth) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            r->x = x;
            r->y = y;
            x += w;
            shelf = std::max(shelf, h);
        }
        width_ = kAtlasWidth;
        height_ = std::max(1, y + shelf);
        rgba_.assign(size_t(width_) * size_t(height_) * 4, 0);
        for (Region* r : order)
            for (int v = r->v0; v <= r->v1; v++)
                for (int u = r->u0; u <= r->u1; u++) {
                    const uint16_t c = vram_.Sample(r->tpage, r->clut, uint8_t(u), uint8_t(v));
                    uint8_t* p = &rgba_[(size_t(r->y + v - r->v0) * size_t(width_) + size_t(r->x + u - r->u0)) * 4];
                    p[0] = uint8_t(((c & 31) << 3) | ((c & 31) >> 2));
                    p[1] = uint8_t((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
                    p[2] = uint8_t((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
                    p[3] = c == 0 ? 0 : 255; // texel 0x0000 is transparent on the PS1
                }
    }

    // Quads as the PS1 draws them: triangles (v0, v1, v3) + (v1, v2, v3) (scene_assets.h BuildCourse).
    void Emit(MeshStreams& mesh, const Corner* corners, size_t count, bool textured, bool semi, bool raw, uint16_t tpage, uint16_t clut) {
        Stream& s = mesh[(textured ? 0 : 2) + (semi ? 1 : 0)];
        static constexpr size_t kQuad[6] = {0, 1, 3, 1, 2, 3}, kTri[3] = {0, 1, 2};
        const Region* r = textured ? &RegionOf(tpage, clut) : nullptr;
        for (size_t k = 0; k < (count == 4 ? 6u : 3u); k++) {
            const Corner& c = corners[count == 4 ? kQuad[k] : kTri[k]];
            s.pos.insert(s.pos.end(), c.pos.begin(), c.pos.end());
            if (textured) {
                s.uv.push_back((float(r->x + c.u - r->u0) + 0.5f) / float(width_));
                s.uv.push_back((float(r->y + c.v - r->v0) + 0.5f) / float(height_));
                const float k2 = 1.0f / 128.0f; // PS1 texture modulation: 128 = 1.0
                if (raw) s.color.insert(s.color.end(), {1.0f, 1.0f, 1.0f});
                else s.color.insert(s.color.end(), {std::min(1.0f, c.color[0] * k2), std::min(1.0f, c.color[1] * k2), std::min(1.0f, c.color[2] * k2)});
            } else {
                s.color.insert(s.color.end(), {c.color[0] / 255.0f, c.color[1] / 255.0f, c.color[2] / 255.0f});
            }
        }
        triangles_ += count == 4 ? 2 : 1;
    }

    MeshStreams ChunkMesh(const TrackChunk& chunk) {
        MeshStreams m;
        for (const TrackPolygon& p : chunk.road.polygons) {
            const size_t n = p.IsQuad() ? 4 : 3;
            const TrackUvSet* uv = p.IsTextured() ? &track_.uvTable[p.uvIndex].nearSet : nullptr;
            Corner corners[4];
            for (size_t c = 0; c < n; c++) {
                corners[c].pos = TrackVertexToWorld(chunk, chunk.road.vertices[p.vertex[c]]);
                corners[c].color = p.color[c];
                corners[c].u = uv ? uv->u[c] : 0;
                corners[c].v = uv ? uv->v[c] : 0;
            }
            Emit(m, corners, n, uv != nullptr, (p.primCode & 2) != 0, (p.primCode & 1) != 0, uv ? uv->tpage : 0, uv ? uv->clut : 0);
        }
        return m;
    }

    MeshStreams ModelMesh(const TrackSceneryModel& model) {
        MeshStreams m;
        for (const TrackSceneryPolygon& p : model.polygons) {
            const size_t n = p.IsQuad() ? 4 : 3;
            Corner corners[4];
            for (size_t c = 0; c < n; c++) {
                const TrackVertex& v = model.vertices[p.vertex[c]];
                corners[c].pos = {float(v.x), float(v.y), float(v.z)};
                corners[c].color = p.color[c];
                corners[c].u = p.u[c];
                corners[c].v = p.v[c];
            }
            Emit(m, corners, n, p.IsTextured(), (p.primCode & 2) != 0, (p.primCode & 1) != 0, p.tpage, p.clut);
        }
        return m;
    }

    int Width() const { return width_; }
    int Height() const { return height_; }
    const std::vector<uint8_t>& Rgba() const { return rgba_; }
    size_t Triangles() const { return triangles_; }
    size_t RegionCount() const { return regions_.size(); }

private:
    Region& RegionOf(uint16_t tpage, uint16_t clut) {
        Region& r = regions_[RegionKey(tpage, clut)];
        r.tpage = tpage;
        r.clut = clut;
        return r;
    }

    const Track& track_;
    const PsxVram& vram_;
    std::map<uint32_t, Region> regions_;
    int width_ = 1, height_ = 1;
    std::vector<uint8_t> rgba_;
    size_t triangles_ = 0;
};

} // namespace

TrackGltfStats ExportTrackGltf(const Track& track, const PsxVram& vram, const std::string& outDir, const std::string& baseName) {
    Builder b(track, vram);
    b.CollectRegions();
    b.BuildAtlas();
    TrackGltfStats stats;
    std::vector<uint8_t> bin;
    std::ostringstream views, meshes, nodes;
    nodes << std::setprecision(9);
    size_t viewCount = 0, meshCount = 0;
    auto addView = [&](const std::vector<float>& v) {
        const size_t offset = bin.size();
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        bin.insert(bin.end(), p, p + v.size() * sizeof(float));
        views << (viewCount ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << v.size() * sizeof(float) << ",\"target\":34962}";
        return viewCount++;
    };
    auto addAccessor = [&](const std::vector<float>& v, int comps, bool bounds) {
        const size_t view = addView(v);
        std::ostringstream a;
        a << std::setprecision(9);
        a << "{\"bufferView\":" << view << ",\"componentType\":5126,\"count\":" << v.size() / size_t(comps) << ",\"type\":\"" << (comps == 3 ? "VEC3" : "VEC2") << "\"";
        if (bounds) {
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < v.size(); i++) {
                mn[i % 3] = std::min(mn[i % 3], v[i]);
                mx[i % 3] = std::max(mx[i % 3], v[i]);
            }
            a << ",\"min\":[" << mn[0] << "," << mn[1] << "," << mn[2] << "],\"max\":[" << mx[0] << "," << mx[1] << "," << mx[2] << "]";
        }
        a << "}";
        return a.str();
    };
    size_t accessorIndex = 0;
    std::ostringstream accessorList;
    auto pushAccessor = [&](const std::vector<float>& v, int comps, bool bounds) {
        accessorList << (accessorIndex ? "," : "") << addAccessor(v, comps, bounds);
        return accessorIndex++;
    };
    // A mesh from its streams; returns -1 when it has no triangles.
    auto addMesh = [&](const MeshStreams& m, const std::string& name) -> long {
        std::ostringstream prims;
        bool first = true;
        for (size_t k = 0; k < 4; k++) {
            const Stream& s = m[k];
            if (!s.Count()) continue;
            const size_t pos = pushAccessor(s.pos, 3, true);
            const size_t col = pushAccessor(s.color, 3, false);
            prims << (first ? "" : ",") << "{\"attributes\":{\"POSITION\":" << pos;
            if (k < 2) prims << ",\"TEXCOORD_0\":" << pushAccessor(s.uv, 2, false);
            prims << ",\"COLOR_0\":" << col << "},\"material\":" << k << "}";
            first = false;
        }
        if (first) return -1;
        meshes << (meshCount ? "," : "") << "{\"name\":\"" << name << "\",\"primitives\":[" << prims.str() << "]}";
        return long(meshCount++);
    };
    size_t nodeCount = 0;
    std::ostringstream rootNodes;
    for (size_t i = 0; i < track.chunks.size(); i++) {
        char name[32];
        std::snprintf(name, sizeof(name), "chunk_%03zu", i);
        const long mesh = addMesh(b.ChunkMesh(track.chunks[i]), name);
        if (mesh < 0) continue;
        stats.chunkMeshes++;
        nodes << (nodeCount ? "," : "") << "{\"name\":\"" << name << "\",\"mesh\":" << mesh << ",\"extras\":{\"gt2pc\":{\"chunk\":" << i << "}}}";
        rootNodes << (nodeCount ? "," : "") << nodeCount;
        nodeCount++;
    }
    const size_t chunkTriangles = b.Triangles();
    std::vector<long> modelMesh(track.sceneryModels.size(), -1);
    std::vector<size_t> modelTriangles(track.sceneryModels.size(), 0);
    for (size_t i = 0; i < track.sceneryModels.size(); i++) {
        char name[32];
        std::snprintf(name, sizeof(name), "model_%03zu", i);
        const size_t before = b.Triangles();
        modelMesh[i] = addMesh(b.ModelMesh(track.sceneryModels[i]), name);
        modelTriangles[i] = b.Triangles() - before;
        if (modelMesh[i] >= 0) stats.modelMeshes++;
    }
    stats.sceneTriangles = chunkTriangles;
    for (size_t i = 0; i < track.sceneryInstances.size(); i++) {
        const TrackSceneryInstance& inst = track.sceneryInstances[i];
        const size_t model = track.sceneryLods[inst.lodList].front().model; // the most detailed level
        if (modelMesh[model] < 0) continue;
        const std::array<float, 16> m = SceneryInstanceMatrix(inst, track.sceneryModels[model]);
        nodes << (nodeCount ? "," : "") << "{\"name\":\"instance_" << i << "\",\"mesh\":" << modelMesh[model] << ",\"matrix\":[";
        for (size_t k = 0; k < 16; k++) nodes << (k ? "," : "") << m[k];
        nodes << "],\"extras\":{\"gt2pc\":{\"instance\":" << i << ",\"model\":" << model << ",\"list\":" << int(inst.list) << "}}}";
        rootNodes << (nodeCount ? "," : "") << nodeCount;
        nodeCount++;
        stats.instances++;
        stats.sceneTriangles += modelTriangles[model];
    }

    WritePngRgbaCompressed(outDir + "/" + baseName + ".png", b.Width(), b.Height(), b.Rgba());
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"gt2tool export-tracks\"},\"scene\":0,\"scenes\":[{\"nodes\":[" << rootNodes.str() << "]}],"
      << "\"nodes\":[" << nodes.str() << "],\"meshes\":[" << meshes.str() << "],"
      << "\"materials\":["
         "{\"name\":\"textured\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5,\"doubleSided\":true},"
         "{\"name\":\"textured_semi\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"baseColorFactor\":[1,1,1,0.5],\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"BLEND\",\"doubleSided\":true},"
         "{\"name\":\"flat\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true},"
         "{\"name\":\"flat_semi\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,0.5],\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"BLEND\",\"doubleSided\":true}],"
      << "\"textures\":[{\"sampler\":0,\"source\":0}],"
      << "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":33071,\"wrapT\":33071}],"
      << "\"images\":[{\"uri\":\"" << baseName << ".png\"}],"
      << "\"buffers\":[{\"uri\":\"" << baseName << ".bin\",\"byteLength\":" << bin.size() << "}],"
      << "\"bufferViews\":[" << views.str() << "],\"accessors\":[" << accessorList.str() << "]}";
    auto writeFile = [](const std::string& path, const void* data, size_t size) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot create " + path);
        const size_t n = std::fwrite(data, 1, size, f);
        std::fclose(f);
        if (n != size) throw std::runtime_error("cannot write " + path);
    };
    const std::string text = j.str();
    writeFile(outDir + "/" + baseName + ".gltf", text.data(), text.size());
    writeFile(outDir + "/" + baseName + ".bin", bin.data(), bin.size());
    stats.triangles = b.Triangles();
    stats.textureRegions = b.RegionCount();
    stats.atlasWidth = b.Width();
    stats.atlasHeight = b.Height();
    return stats;
}

} // namespace gt2
