#pragma once
#include <string>

#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"

namespace gt2 {

struct CarPreviewOptions {
    size_t lod = 0;
    size_t paint = 0;
    int width = 1024, height = 640;
    double yawDegrees = 35, pitchDegrees = 18;
};

// Software Z-buffer render (orthographic, nearest texel, no lighting) of the parsed data -
// a dependency-free check that geometry, UVs and CLUT selection decode correctly.
void RenderCarPreview(const CarModel& model, const CarTexture& texture, const std::string& pngPath,
                      const CarPreviewOptions& options);

} // namespace gt2
