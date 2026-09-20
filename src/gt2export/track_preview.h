#pragma once
#include <string>

#include "gt2formats/psx_vram.h"
#include "gt2formats/track.h"

namespace gt2 {

struct TrackPreviewOptions {
    int width = 1280, height = 720;
    int chunk = -1;            // -1: top-down map of the whole course; otherwise a driver-height view from that chunk
    double eyeHeight = 1.2;    // metres above the chunk origin
    bool road = true, surround = true;
};

// Software render of course geometry. With `vram` (built from the course .trp) polygons are
// textured through the course UV table; without it only polygon colours are shown.
void RenderTrackPreview(const Track& track, const PsxVram* vram, const std::string& pngPath,
                        const TrackPreviewOptions& options);

} // namespace gt2
