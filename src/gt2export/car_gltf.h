#pragma once
#include <string>

#include "gt2export/car_mesh.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"

namespace gt2 {

struct CarGltfOptions {
    size_t lod = 0;
    size_t paint = 0;
    bool placeholderWheels = true; // plain cylinders; the game generates wheel meshes in code
    // Quad corners are stored in ring order (corpus vote: 532,797 ring vs 300 strip of 535,311).
    // true forces PS1 strip order (0,1,2)+(1,3,2) for experiments.
    bool quadStripOrder = false;
};

// Inspection export only (work\export): writes <base>.gltf, <base>.bin, <base>.png.
// The PNG is a 4x4 atlas of the 256x224 texture decoded with each of the paint's 16 CLUTs.
void ExportCarGltf(const CarModel& model, const CarTexture& texture, const std::string& outDir,
                   const std::string& baseName, const CarGltfOptions& options);

} // namespace gt2
