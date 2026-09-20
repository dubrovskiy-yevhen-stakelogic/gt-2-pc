#pragma once
#include <string>

#include "gt2formats/psx_vram.h"
#include "gt2formats/track.h"

namespace gt2 {

struct TrackGltfStats {
    size_t triangles = 0, sceneTriangles = 0, chunkMeshes = 0, modelMeshes = 0, instances = 0, textureRegions = 0;
    int atlasWidth = 0, atlasHeight = 0;
};

// The drawable geometry of a course as glTF 2.0 (docs/formats/track_json.md "glTF export"): <base>.gltf + <base>.bin +
// <base>.png. One mesh per chunk (the road shape in world metres: x, y up, z - the same axes as the game's world), one
// mesh per scenery model (model units) placed by one node per instance with the original's matrix
// (SceneryInstanceMatrix). Textures: every (tpage, clut) region the polygons use, decoded from the course's VRAM image
// (the .trp pack) and packed into one RGBA atlas (texel 0x0000 = transparent, like the PS1). Vertex colours carry the PS1
// modulation (128 = 1.0; raw-texture polygons white). Materials: textured / untextured x opaque / semi-transparent.
// Billboards (camera-facing), the mirror copies and the glow records are not exported (they stay in the JSON).
TrackGltfStats ExportTrackGltf(const Track& track, const PsxVram& vram, const std::string& outDir, const std::string& baseName);

} // namespace gt2
