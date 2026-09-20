#pragma once
// Minimal glTF 2.0 reader for mod car meshes: .gltf (JSON + external or data-URI buffers) and .glb, node
// hierarchy with TRS / matrix transforms baked into the vertices, triangle primitives (TRIANGLES, STRIP, FAN)
// with POSITION, NORMAL, TEXCOORD_0, COLOR_0 and 8/16/32-bit indices, and the materials' baseColorFactor /
// baseColorTexture (PNG images only, decoded with png_reader.h). Sparse accessors, skins, morph targets,
// animations and non-PNG images are not supported. Own code.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/png_reader.h"

namespace gt2 {

struct GltfVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<float, 4> color{1, 1, 1, 1};
};

struct GltfMaterial {
    std::string name;
    std::array<float, 4> baseColorFactor{1, 1, 1, 1};
    int baseColorImage = -1; // index into GltfMesh::images, -1 = untextured
    bool doubleSided = false;
    // "extras": {"gt2pc": {"reflection": true}} - the material's triangles take the game's reflection pass (the .cdo's
    // renderFlags bit 15; docs/formats/car_json.md "Reflections").
    bool reflection = false;
};

// One triangle list in world (scene) space: the node transforms are already applied.
struct GltfPrimitive {
    std::vector<GltfVertex> vertices;
    std::vector<uint32_t> indices; // triangles (3 per face)
    int material = -1;             // index into GltfMesh::materials, -1 = default material
    bool hasNormals = false, hasUvs = false, hasColors = false;
};

struct GltfMesh {
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfMaterial> materials;
    std::vector<PngImage> images;      // decoded; an undecodable image is left empty (width 0) with a warning
    std::vector<std::string> warnings;
    std::array<float, 3> boundsMin{}, boundsMax{};
    size_t TriangleCount() const;
};

// Throws std::runtime_error on malformed input.
GltfMesh ReadGltf(const std::string& path);

} // namespace gt2
