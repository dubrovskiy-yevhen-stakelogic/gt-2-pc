#pragma once
// Dev aid for the arcade menu screens (docs/research/arcade_disc.md section 18): a frame's primitives as a text listing in the
// format of gt2play --prims (RECT / LINE / POLY lines, E1 when the draw mode changes), so that a native frame can be compared line
// by line with a capture of the original (the capture's lines without their sequence numbers).
#include <string>
#include <vector>

#include "gt2formats/gt_menu_images.h"

namespace gt2game {

void WriteMenuPrimListing(const std::string& path, const std::vector<gt2::MenuPrim>& prims);

} // namespace gt2game
