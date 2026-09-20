#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "gt2formats/track.h"
#include "gt2vfs/gtfs.h"
#include "interp/gte.h"

namespace gt2 {

// Turns one frame of captured GTE transforms into a scene description for the native renderer -
// without any knowledge of the game's code. The vertices the game feeds to the GTE are the vertices
// stored in its course and car files, so each transform can be attributed to a course chunk or a car
// model by vertex matching; the camera and every car's world matrix then follow from the matrices.
// Verified on the attract-mode race: the camera position derived from chunks in different grid cells
// agrees to the centimetre (tools/gt2run scene-probe).
class SceneExtractor {
public:
    struct Car {
        std::string id;                // e.g. "us36n"
        std::array<float, 16> world{}; // column-major, car model metres -> world metres
    };

    struct Scene {
        bool valid = false;            // a course chunk was recognised, camera is known
        std::string trackName;
        std::array<float, 3> cameraPosition{};           // world metres, +Y up
        std::array<std::array<float, 3>, 3> cameraAxes{}; // rows: right, DOWN, forward (PS1 camera space) in world axes
        float projectionDistance = 0;                    // GTE H: pixels at 320x240
        std::vector<Car> cars;
        size_t transformsTotal = 0, transformsExplained = 0;
    };

    explicit SceneExtractor(const GtfsVolume& vol); // indexes every car model (about a second)

    Scene Extract(std::span<const Gte::CapturedTransform> transforms, std::span<const Gte::CapturedVertex> vertices);

    const Track* CurrentTrack() const { return track_.get(); }

    // View-projection for Vulkan clip space (y down, depth 0..1) reproducing the game's camera with its
    // own field of view, widened horizontally for the given aspect ratio. Column-major.
    static std::array<float, 16> ViewProjection(const Scene& scene, float aspect, float zNear = 0.25f, float zFar = 6000.0f);

private:
    bool SelectTrack(std::span<const Gte::CapturedVertex> vertices);
    void IndexTrack();

    const GtfsVolume& vol_;
    struct IndexedCarLod { std::string id; double metresPerUnit; };
    std::vector<IndexedCarLod> carLods_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> vertexToCarLod_;

    struct TrackCandidate { std::string name; std::vector<uint64_t> sortedKeys; };
    std::vector<TrackCandidate> trackCandidates_; // built on first use
    std::string trackName_;
    std::unique_ptr<Track> track_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> vertexToChunk_;
    int framesWithoutMatch_ = 0;
};

} // namespace gt2
