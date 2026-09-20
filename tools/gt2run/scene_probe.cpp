// scene-probe: runs the game to a given field, captures one game frame of GTE transforms and checks the
// hypothesis behind the native renderer: the vertices the game feeds to the GTE are the vertices stored
// in the course (.tro) and car (.cdo/.cno) files, so captured transforms can be attributed to assets.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gt2formats/car_model.h"
#include "gt2formats/track.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "machine/machine.h"

using namespace gt2;

namespace {

uint64_t Key(int x, int y, int z) { return (uint64_t(uint16_t(x)) << 32) | (uint64_t(uint16_t(y)) << 16) | uint16_t(z); }

std::vector<uint8_t> ReadRootFile(const DiscImage& disc, const std::string& name) {
    auto f = disc.FindRootFile(name);
    if (!f) throw std::runtime_error(name + " not found in disc root");
    std::vector<uint8_t> data(f->size);
    disc.ReadForm1(f->lba, 0, data.data(), data.size());
    return data;
}

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

int CmdSceneProbe(const DiscImage& disc, uint64_t field) {
    GtfsVolume vol(disc);
    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;

    Machine m;
    m.AttachDisc(&disc);
    m.gpu.skip3dRaster = true;
    m.LoadExe(ReadRootFile(disc, exeName), 0x801FFF00);
    std::printf("running to field %llu...\n", static_cast<unsigned long long>(field));
    m.Run(Machine::kInstructionsPerVBlank * field);
    m.gte.captureEnabled = true;
    m.gte.ClearCapture();
    m.Run(Machine::kInstructionsPerVBlank * 2); // one 30 fps game frame
    const auto& transforms = m.gte.capturedTransforms;
    const auto& vertices = m.gte.capturedVertices;
    std::printf("captured %zu transforms, %zu vertices in 2 fields\n", transforms.size(), vertices.size());
    if (vertices.empty()) return 1;

    // --- which course?
    struct TrackSets { std::string name; Track track; std::unordered_set<uint64_t> all; };
    std::vector<TrackSets> tracks;
    for (const auto& f : vol.Files()) {
        if (f.path.rfind("crsobj/", 0) != 0 || !EndsWith(f.path, ".tro.gz")) continue;
        TrackSets t;
        t.name = f.path.substr(7, f.path.size() - 7 - 7);
        t.track = ParseTrack(vol.Read(f));
        for (const auto& c : t.track.chunks)
            for (const TrackShape* s : {&c.road, &c.surround})
                for (const auto& v : s->vertices) t.all.insert(Key(v.x, v.y, v.z));
        tracks.push_back(std::move(t));
    }
    std::vector<std::pair<size_t, size_t>> score; // hits, track index
    for (size_t i = 0; i < tracks.size(); i++) {
        size_t hits = 0;
        for (const auto& v : vertices) hits += tracks[i].all.count(Key(v.x, v.y, v.z));
        score.push_back({hits, i});
    }
    std::sort(score.rbegin(), score.rend());
    std::printf("course match (captured vertices found in the course's vertex set):\n");
    for (size_t i = 0; i < 4; i++)
        std::printf("  %-20s %zu of %zu\n", tracks[score[i].second].name.c_str(), score[i].first, vertices.size());
    const TrackSets& best = tracks[score[0].second];

    // --- per transform: which chunk?
    std::vector<std::unordered_set<uint64_t>> chunkSets(best.track.chunks.size());
    for (size_t c = 0; c < best.track.chunks.size(); c++)
        for (const TrackShape* s : {&best.track.chunks[c].road, &best.track.chunks[c].surround})
            for (const auto& v : s->vertices) chunkSets[c].insert(Key(v.x, v.y, v.z));

    size_t trackGroups = 0, trackVertices = 0, unmatchedGroups = 0, unmatchedVertices = 0;
    std::vector<size_t> unmatched;
    int shown = 0;
    for (size_t ti = 0; ti < transforms.size(); ti++) {
        const auto& t = transforms[ti];
        size_t bestChunk = 0, bestHits = 0;
        for (size_t c = 0; c < chunkSets.size(); c++) {
            size_t hits = 0;
            for (uint32_t k = 0; k < t.vertexCount; k++) {
                const auto& v = vertices[t.firstVertex + k];
                hits += chunkSets[c].count(Key(v.x, v.y, v.z));
            }
            if (hits > bestHits) { bestHits = hits; bestChunk = c; }
        }
        if (t.vertexCount >= 3 && bestHits * 10 >= t.vertexCount * 9) {
            trackGroups++;
            trackVertices += t.vertexCount;
            if (shown < 6) {
                // Camera from this transform. Local chunk axes are (x, worldZ, height) in 1/64 m; RT = s * R.
                const auto& ch = best.track.chunks[bestChunk];
                double s = std::sqrt(double(t.rt[0][0]) * t.rt[0][0] + double(t.rt[0][1]) * t.rt[0][1] + double(t.rt[0][2]) * t.rt[0][2]) / 4096.0;
                double R[3][3], T[3], L[3];
                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 3; c++) R[r][c] = t.rt[r][c] / (4096.0 * s);
                    T[r] = t.tr[r] / (64.0 * s);
                }
                for (int c = 0; c < 3; c++) L[c] = -(R[0][c] * T[0] + R[1][c] * T[1] + R[2][c] * T[2]);
                std::printf("    scale %.2f  camera world (m): x %.2f  y %.2f  z %.2f   forward (x,z): %.3f %.3f\n", s,
                            ch.cellOrigin[0] / 65536.0 + L[0], L[2], ch.cellOrigin[1] / 65536.0 + L[1], R[2][0], R[2][1]);
            }
            if (shown++ < 6) {
                const auto& ch = best.track.chunks[bestChunk];
                std::printf("  transform %zu: %u verts -> chunk %zu (%zu hits) TR %d %d %d  RT row0 %d %d %d  cell %d %d (m)\n", ti,
                            t.vertexCount, bestChunk, bestHits, t.tr[0], t.tr[1], t.tr[2], t.rt[0][0], t.rt[0][1], t.rt[0][2],
                            ch.cellOrigin[0] >> 16, ch.cellOrigin[1] >> 16);
            }
        } else {
            unmatchedGroups++;
            unmatchedVertices += t.vertexCount;
            unmatched.push_back(ti);
        }
    }
    std::printf("course chunks explain %zu transforms / %zu vertices; other: %zu transforms / %zu vertices\n", trackGroups,
                trackVertices, unmatchedGroups, unmatchedVertices);

    // --- cars: match the remaining groups against every car LOD
    struct CarLodSet { std::string id; size_t lod; std::unordered_set<uint64_t> set; };
    std::unordered_map<uint64_t, std::vector<uint32_t>> vertexToCarLod;
    std::vector<CarLodSet> carLods;
    for (const auto& f : vol.Files()) {
        if (f.path.rfind("carobj/", 0) != 0 || !(EndsWith(f.path, ".cdo.gz") || EndsWith(f.path, ".cno.gz"))) continue;
        CarModel model = ParseCarModel(vol.Read(f));
        for (size_t l = 0; l < model.lods.size(); l++) {
            CarLodSet s{f.path.substr(7, f.path.size() - 7 - 3), l, {}};
            for (const auto& v : model.lods[l].vertices) s.set.insert(Key(v.x, v.y, v.z));
            for (uint64_t k : s.set) vertexToCarLod[k].push_back(uint32_t(carLods.size()));
            carLods.push_back(std::move(s));
        }
    }
    std::map<std::string, size_t> carHits;
    size_t carGroups = 0, stillUnknown = 0, stillUnknownVertices = 0;
    for (size_t ti : unmatched) {
        const auto& t = transforms[ti];
        std::unordered_map<uint32_t, size_t> votes;
        for (uint32_t k = 0; k < t.vertexCount; k++) {
            const auto& v = vertices[t.firstVertex + k];
            auto it = vertexToCarLod.find(Key(v.x, v.y, v.z));
            if (it == vertexToCarLod.end()) continue;
            for (uint32_t idx : it->second) votes[idx]++;
        }
        uint32_t bestIdx = 0;
        size_t bestVotes = 0;
        for (const auto& [idx, n] : votes) if (n > bestVotes) { bestVotes = n; bestIdx = idx; }
        if (t.vertexCount >= 8 && bestVotes * 10 >= t.vertexCount * 8) {
            carGroups++;
            carHits[carLods[bestIdx].id + " LOD" + std::to_string(carLods[bestIdx].lod)] += t.vertexCount;
        } else {
            stillUnknown++;
            stillUnknownVertices += t.vertexCount;
        }
    }
    std::printf("car models explain %zu transforms:\n", carGroups);
    for (const auto& [name, n] : carHits) std::printf("  %-16s %zu vertices\n", name.c_str(), n);
    std::printf("unexplained: %zu transforms / %zu vertices (wheels, sky, shadows, effects, clipped geometry...)\n", stillUnknown,
                stillUnknownVertices);
    return 0;
}
