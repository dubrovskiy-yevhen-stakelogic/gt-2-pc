#include "scene/scene_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gt2export/car_mesh.h"
#include "gt2formats/car_model.h"

namespace gt2 {
namespace {

uint64_t Key(int x, int y, int z) { return (uint64_t(uint16_t(x)) << 32) | (uint64_t(uint16_t(y)) << 16) | uint16_t(z); }

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// RT rows carry a per-object scale (the game keeps coordinates inside 16 bits): row = scale * axis.
struct Decomposed {
    double axis[3][3];  // unit rows
    double translation[3]; // in object units (the units of the transformed vertices)
};

Decomposed Decompose(const Gte::CapturedTransform& t) {
    Decomposed d{};
    for (int r = 0; r < 3; r++) {
        const double norm = std::sqrt(double(t.rt[r][0]) * t.rt[r][0] + double(t.rt[r][1]) * t.rt[r][1] + double(t.rt[r][2]) * t.rt[r][2]);
        const double safe = norm > 0 ? norm : 1;
        for (int c = 0; c < 3; c++) d.axis[r][c] = t.rt[r][c] / safe;
        d.translation[r] = t.tr[r] * 4096.0 / safe;
    }
    return d;
}

} // namespace

SceneExtractor::SceneExtractor(const GtfsVolume& vol) : vol_(vol) {
    for (const auto& f : vol.Files()) {
        if (f.path.rfind("carobj/", 0) != 0 || !EndsWith(f.path, ".cdo.gz")) continue;
        CarModel model = ParseCarModel(vol.Read(f));
        const std::string id = f.path.substr(7, 5);
        for (const CarLod& lod : model.lods) {
            const uint32_t index = uint32_t(carLods_.size());
            carLods_.push_back({id, CarBodyMetresPerUnit(lod)});
            for (const CarVertex& v : lod.vertices) {
                auto& list = vertexToCarLod_[Key(v.x, v.y, v.z)];
                if (list.empty() || list.back() != index) list.push_back(index);
            }
        }
    }
}

bool SceneExtractor::SelectTrack(std::span<const Gte::CapturedVertex> vertices) {
    if (trackCandidates_.empty()) {
        for (const auto& f : vol_.Files()) {
            if (f.path.rfind("crsobj/", 0) != 0 || !EndsWith(f.path, ".tro.gz")) continue;
            TrackCandidate c;
            c.name = f.path.substr(7, f.path.size() - 7 - 7);
            Track t = ParseTrack(vol_.Read(f));
            for (const auto& chunk : t.chunks)
                for (const TrackShape* s : {&chunk.road, &chunk.surround})
                    for (const auto& v : s->vertices) c.sortedKeys.push_back(Key(v.x, v.y, v.z));
            std::sort(c.sortedKeys.begin(), c.sortedKeys.end());
            c.sortedKeys.erase(std::unique(c.sortedKeys.begin(), c.sortedKeys.end()), c.sortedKeys.end());
            trackCandidates_.push_back(std::move(c));
        }
    }
    size_t bestHits = 0;
    const TrackCandidate* best = nullptr;
    for (const auto& c : trackCandidates_) {
        size_t hits = 0;
        for (const auto& v : vertices) hits += std::binary_search(c.sortedKeys.begin(), c.sortedKeys.end(), Key(v.x, v.y, v.z));
        if (hits > bestHits) { bestHits = hits; best = &c; }
    }
    if (!best || bestHits < 200) return false; // not a race scene
    if (best->name != trackName_) {
        trackName_ = best->name;
        track_ = std::make_unique<Track>(ParseTrack(vol_.Read("crsobj/" + trackName_ + ".tro")));
        IndexTrack();
    }
    return true;
}

void SceneExtractor::IndexTrack() {
    vertexToChunk_.clear();
    for (uint32_t c = 0; c < track_->chunks.size(); c++)
        for (const TrackShape* s : {&track_->chunks[c].road, &track_->chunks[c].surround})
            for (const auto& v : s->vertices) {
                auto& list = vertexToChunk_[Key(v.x, v.y, v.z)];
                if (list.empty() || list.back() != c) list.push_back(c);
            }
}

SceneExtractor::Scene SceneExtractor::Extract(std::span<const Gte::CapturedTransform> transforms,
                                              std::span<const Gte::CapturedVertex> vertices) {
    Scene scene;
    scene.transformsTotal = transforms.size();
    if (vertices.size() < 300) return scene;

    auto voteChunk = [&](const Gte::CapturedTransform& t, uint32_t& chunk) {
        std::unordered_map<uint32_t, size_t> votes;
        for (uint32_t k = 0; k < t.vertexCount; k++) {
            const auto& v = vertices[t.firstVertex + k];
            auto it = vertexToChunk_.find(Key(v.x, v.y, v.z));
            if (it == vertexToChunk_.end()) continue;
            for (uint32_t c : it->second) votes[c]++;
        }
        size_t best = 0;
        for (const auto& [c, n] : votes) if (n > best) { best = n; chunk = c; }
        return best;
    };

    // Camera: the transform with the strongest single-chunk match.
    const Gte::CapturedTransform* cameraSource = nullptr;
    uint32_t cameraChunk = 0;
    for (int attempt = 0; attempt < 2 && !cameraSource; attempt++) {
        if (track_) {
            size_t bestVotes = 0;
            for (const auto& t : transforms) {
                if (t.vertexCount < 12) continue;
                uint32_t chunk = 0;
                const size_t votes = voteChunk(t, chunk);
                if (votes * 10 >= t.vertexCount * 7 && votes > bestVotes) { bestVotes = votes; cameraSource = &t; cameraChunk = chunk; }
            }
        }
        if (!cameraSource && (attempt == 1 || !SelectTrack(vertices))) break;
    }
    if (!cameraSource) {
        if (++framesWithoutMatch_ > 120) { track_.reset(); trackName_.clear(); vertexToChunk_.clear(); }
        return scene;
    }
    framesWithoutMatch_ = 0;

    // p_cam = R * L + T with L = chunk-local metres (x, worldZ, height) relative to the cell origin.
    const Decomposed view = Decompose(*cameraSource);
    const TrackChunk& chunk = track_->chunks[cameraChunk];
    double T[3], local[3];
    for (int r = 0; r < 3; r++) T[r] = view.translation[r] / 64.0;
    for (int c = 0; c < 3; c++) local[c] = -(view.axis[0][c] * T[0] + view.axis[1][c] * T[1] + view.axis[2][c] * T[2]);
    scene.valid = true;
    scene.trackName = trackName_;
    scene.projectionDistance = float(cameraSource->h);
    scene.cameraPosition = {float(chunk.cellOrigin[0] / 65536.0 + local[0]), float(local[2]), float(chunk.cellOrigin[1] / 65536.0 + local[1])};
    double Rw[3][3]; // camera rows in world axes (x, height, z)
    for (int r = 0; r < 3; r++) {
        Rw[r][0] = view.axis[r][0];
        Rw[r][1] = view.axis[r][2];
        Rw[r][2] = view.axis[r][1];
        for (int c = 0; c < 3; c++) scene.cameraAxes[size_t(r)][size_t(c)] = float(Rw[r][c]);
    }

    // Cars: world = C + Rw^T * (Rc * v + Tc), v in car metres.
    for (const auto& t : transforms) {
        if (t.vertexCount < 8) continue;
        std::unordered_map<uint32_t, size_t> votes;
        for (uint32_t k = 0; k < t.vertexCount; k++) {
            const auto& v = vertices[t.firstVertex + k];
            auto it = vertexToCarLod_.find(Key(v.x, v.y, v.z));
            if (it == vertexToCarLod_.end()) continue;
            for (uint32_t idx : it->second) votes[idx]++;
        }
        uint32_t bestLod = 0;
        size_t bestVotes = 0;
        for (const auto& [idx, n] : votes) if (n > bestVotes) { bestVotes = n; bestLod = idx; }
        if (bestVotes * 10 < t.vertexCount * 8) {
            uint32_t ignored = 0;
            if (voteChunk(t, ignored) * 10 >= t.vertexCount * 7) scene.transformsExplained++;
            continue;
        }
        scene.transformsExplained++;

        const Decomposed car = Decompose(t);
        const double mu = carLods_[bestLod].metresPerUnit;
        Car out;
        out.id = carLods_[bestLod].id;
        double M[3][3], Tc[3], pos[3];
        for (int r = 0; r < 3; r++) Tc[r] = car.translation[r] * mu;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) M[r][c] = Rw[0][r] * car.axis[0][c] + Rw[1][r] * car.axis[1][c] + Rw[2][r] * car.axis[2][c];
        for (int r = 0; r < 3; r++) pos[r] = scene.cameraPosition[size_t(r)] + Rw[0][r] * Tc[0] + Rw[1][r] * Tc[1] + Rw[2][r] * Tc[2];

        bool duplicate = false;
        for (const Car& other : scene.cars)
            if (other.id == out.id && std::abs(other.world[12] - pos[0]) + std::abs(other.world[14] - pos[2]) < 2.0) duplicate = true;
        if (duplicate) continue;
        out.world = {float(M[0][0]), float(M[1][0]), float(M[2][0]), 0, float(M[0][1]), float(M[1][1]), float(M[2][1]), 0,
                     float(M[0][2]), float(M[1][2]), float(M[2][2]), 0, float(pos[0]), float(pos[1]), float(pos[2]), 1};
        scene.cars.push_back(std::move(out));
    }
    return scene;
}

std::array<float, 16> SceneExtractor::ViewProjection(const Scene& scene, float aspect, float zNear, float zFar) {
    const auto& R = scene.cameraAxes;
    const auto& C = scene.cameraPosition;
    const float fy = scene.projectionDistance / 120.0f, fx = fy / aspect;
    // Reversed Z with an infinite far plane (gt2view/vk_scene_renderer.h): z' = zNear, w' = z -> z_ndc = zNear / z.
    (void)zFar;
    const float a = 0.0f, b = zNear;
    float t[3];
    for (size_t r = 0; r < 3; r++) t[r] = -(R[r][0] * C[0] + R[r][1] * C[1] + R[r][2] * C[2]);
    // clip = P * [R | t]: x' = fx * x, y' = fy * y (PS1 camera y is down, like Vulkan), z' = a * z + b (= zNear), w' = z.
    std::array<float, 16> m{};
    for (size_t c = 0; c < 3; c++) {
        m[c * 4 + 0] = fx * R[0][c];
        m[c * 4 + 1] = fy * R[1][c];
        m[c * 4 + 2] = a * R[2][c];
        m[c * 4 + 3] = R[2][c];
    }
    m[12] = fx * t[0];
    m[13] = fy * t[1];
    m[14] = a * t[2] + b;
    m[15] = t[2];
    return m;
}

} // namespace gt2
