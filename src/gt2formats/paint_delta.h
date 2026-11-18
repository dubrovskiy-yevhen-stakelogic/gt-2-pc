#pragma once
// GT4 paint variants stored as a delta against the base texture.
#include "gt2formats/png_reader.h"

#include <string>

namespace gt2 {

// Reads a paint variant. A legacy .png is returned as-is; a .delta.gz file is
// applied to `original`, which must be the base image the delta was built from.
PngImage ReadPaintVariant(const PngImage& original, const std::string& path);

} // namespace gt2
